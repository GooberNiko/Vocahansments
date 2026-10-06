/*
 * Real-time pitch model: predicts the pitch VOCALOID4 will sing from the score
 * alone, fast enough to recompute on every repaint. Fitted to the editor's own
 * renders (see re/model): note steps, portamento transitions (risePort,
 * direction, interval), attack bends (bendDep/bendLen), phrase onset/release,
 * the 16 vibrato types, and PIT x PBS (exact).
 *
 * Included by pitchtools.c.
 */
#include <math.h>
#include "pitch_tables.h"

typedef struct {
    long tOn, tOff;           /* absolute ticks */
    double on, off;           /* ms */
    int num, dep, blen, rise, vibType, vibLen;
    unsigned id;              /* VSQ object id */
    int vib;                  /* index into g_vib (real rendered vibrato), -1 = none */
    unsigned hash;            /* everything else about the note that changes how it is sung */
    WORD ph[8];               /* phoneme symbols (engine.c's table) */
    int nph;
    int vel;                  /* velocity */
    wchar_t lyric[16];
} PMNote;

static WORD ph_symbol(const char *s);
static void ph_drag_preview(HDC dc);

typedef struct { long tick; double ms; double msPerTick; } TempoSeg;

static PMNote *g_pmNotes;
static int g_pmCount, g_pmCap;
static TempoSeg g_tempo[512];
static int g_ntempo;
static ControlRef *g_pmPit, *g_pmPbs;
static int g_pmNpit, g_pmNpbs;
static long g_pmPartPos;
static unsigned g_pmPart = 0xFFFFFFFF;
static volatile LONG g_pmGen = 1;       /* bumped on every commit/undo/redo */
static LONG g_pmBuiltGen;
static unsigned g_pmSig;                /* the score minus PIT/PBS: equal = sung the same before PIT */

static unsigned fnv(unsigned h, const void *p, int n)
{
    for (int i = 0; i < n; i++) h = (h ^ ((const BYTE *)p)[i]) * 16777619u;
    return h;
}
static unsigned fnv_int(unsigned h, int v) { return fnv(h, &v, sizeof v); }

/* ------------------------------------------------------------- tempo -- */

#define V4_TD_IVSTempo ((void *)0x00598328)
typedef void *(THISCALL *GetSeq_t)(void *mgr);

static void pm_load_tempo(void *mgr)
{
    g_ntempo = 0;
    GetSeq_t getSeq = (GetSeq_t)GetProcAddress(GetModuleHandleA("vsq4.dll"), "?vsqGetCurrentSequence@CVSVsqManagerIF@@QAEPAVIVSVsqRoot@@XZ");
    void *root = getSeq ? getSeq(mgr) : NULL;
    void *seq = root ? VCALL0(void *, root, 0x14) : NULL;
    void *master = seq ? VCALL0(void *, seq, 0x1C) : NULL;
    void *list = master ? VCALL0(void *, master, 0x30) : NULL;
    int n = list ? VCALL0(int, list, VSLIST_COUNT) : 0;
    ListItem_t item = list ? VMETHOD(ListItem_t, list, VSLIST_ITEM) : NULL;
    for (int i = 0; i < n && g_ntempo < 512; i++) {
        void *obj = item(list, (unsigned)i);
        BYTE *tp = obj ? (BYTE *)pDynCast(obj, 0, V4_TD_IVSVsqObject, V4_TD_IVSTempo, 0) : NULL;
        if (!tp) continue;
        long pos = VCALL0(int, tp, 0x14);
        int bpm100 = VCALL0(int, tp, 0x18);
        if (bpm100 < 2000 || bpm100 > 30000) continue;
        g_tempo[g_ntempo].tick = pos;
        g_tempo[g_ntempo].msPerTick = 60000.0 / (bpm100 / 100.0) / 480.0;
        g_ntempo++;
    }
    if (!g_ntempo) { g_tempo[0].tick = 0; g_tempo[0].msPerTick = 60000.0 / 120.0 / 480.0; g_ntempo = 1; }
    g_tempo[0].ms = g_tempo[0].tick * g_tempo[0].msPerTick;
    for (int i = 1; i < g_ntempo; i++)
        g_tempo[i].ms = g_tempo[i - 1].ms + (g_tempo[i].tick - g_tempo[i - 1].tick) * g_tempo[i - 1].msPerTick;
}

static double pm_ms(long tick)
{
    int i = g_ntempo - 1;
    while (i > 0 && g_tempo[i].tick > tick) i--;
    return g_tempo[i].ms + (tick - g_tempo[i].tick) * g_tempo[i].msPerTick;
}

static long pm_tick(double ms)
{
    int i = g_ntempo - 1;
    while (i > 0 && g_tempo[i].ms > ms) i--;
    return g_tempo[i].tick + lround((ms - g_tempo[i].ms) / g_tempo[i].msPerTick);
}

/* --------------------------------------------------- rendered vibrato -- */
/* The templates only know each vibrato type at its default depth and rate.
 * Once the engine has rendered a note, its real vibrato (final sung pitch
 * minus the pitch without vibrato, from the rendered frames) is kept per note
 * and used instead, as long as the note keeps its length and vibrato settings
 * (moving it keeps it). Filled by vib_capture() in pitchtools.c. */

typedef struct { unsigned id; double len; int vibType, vibLen, first, n; } PMVib;
typedef struct { float ms, cents; } PMVibPt;    /* ms since note on */
static PMVib *g_vib;
static int g_nvib, g_vibCap;
static PMVibPt *g_vibPts;
static int g_nvibPts, g_vibPtsCap;

static int cmp_pmvib(const void *a, const void *b)
{
    unsigned x = ((const PMVib *)a)->id, y = ((const PMVib *)b)->id;
    return x < y ? -1 : x > y;
}

static void pm_resolve_vib(PMNote *notes, int n)
{
    for (int i = 0; i < n; i++) {
        notes[i].vib = -1;
        int lo = 0, hi = g_nvib - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (g_vib[m].id == notes[i].id) { notes[i].vib = m; break; }
            if (g_vib[m].id < notes[i].id) lo = m + 1; else hi = m - 1;
        }
    }
}

/* real vibrato in cents at t ms since note on; FALSE = not rendered as it is now */
static BOOL pm_vib_real(const PMNote *n, double t, double *cents)
{
    if (n->vib < 0 || n->vib >= g_nvib) return FALSE;
    const PMVib *v = &g_vib[n->vib];
    if (v->id != n->id || v->vibType != n->vibType || v->vibLen != n->vibLen || fabs(v->len - (n->off - n->on)) > 1.0) return FALSE;
    const PMVibPt *p = g_vibPts + v->first;
    int k = v->n;
    *cents = 0;
    if (t <= p[0].ms || t >= p[k - 1].ms) return TRUE;
    int lo = 0, hi = k - 1;
    while (hi - lo > 1) { int m = (lo + hi) / 2; if (p[m].ms <= t) lo = m; else hi = m; }
    double span = p[hi].ms - p[lo].ms;
    *cents = span > 0 ? p[lo].cents + (p[hi].cents - p[lo].cents) * (t - p[lo].ms) / span : p[lo].cents;
    return TRUE;
}

/* ------------------------------------------------- rendered baseline -- */
/* What the engine sang without PIT (note + portamento, and the vibrato
 * separately), from the last exact render, with the score it was rendered
 * from. PIT/PBS only add on top, so while the notes are unchanged this stays
 * exact through any number of pitch edits. Filled by render_capture(). */

typedef struct { unsigned tick; float base, vib; } PMBaseFrame;   /* cents from MIDI 69 */
static PMBaseFrame *g_base;
static int g_nbase, g_baseCap;
static unsigned g_baseSig;
static BOOL g_baseOk;

static BOOL pm_base_valid(void) { return g_baseOk && g_nbase > 0 && g_baseSig == g_pmSig; }

/* the rendered pitch without PIT at an absolute tick; FALSE where nothing was voiced */
static BOOL pm_base_at(long t, BOOL withVib, double *semis)
{
    if (!pm_base_valid()) return FALSE;
    const PMBaseFrame *b = g_base;
    int n = g_nbase, lo = 0, hi = n - 1;
    if ((long)b[0].tick > t || (long)b[hi].tick < t) return FALSE;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if ((long)b[m].tick <= t) lo = m; else hi = m - 1; }
    if (b[lo].base < -1e30f) return FALSE;
    double c = b[lo].base + (withVib ? b[lo].vib : 0);
    if (lo + 1 < n && b[lo + 1].tick > b[lo].tick && b[lo + 1].base > -1e30f) {
        double f = (double)(t - (long)b[lo].tick) / (double)(b[lo + 1].tick - b[lo].tick);
        c += (b[lo + 1].base + (withVib ? b[lo + 1].vib : 0) - c) * f;
    }
    *semis = 69.0 + c / 100.0;
    return TRUE;
}

/* --------------------------------------------------------- the score -- */

static int cmp_pmnote(const void *a, const void *b)
{
    long x = ((const PMNote *)a)->tOn, y = ((const PMNote *)b)->tOn;
    return x < y ? -1 : x > y;
}

static int cmp_ctrl_pos(const void *a, const void *b) { return ((const ControlRef *)a)->pos - ((const ControlRef *)b)->pos; }

/* Rebuild the cached score if anything was committed since the last build. */
static void *g_pmPartObj, *g_pmList;
static int g_pmListN;

static BOOL pm_refresh(void)
{
    if (!g_view) return FALSE;
    unsigned part = *(unsigned *)(PANE() + V4_PRP_PARTID);
    PartRef pr;
    if (!open_part(&pr)) { g_pmCount = 0; g_pmPart = 0xFFFFFFFF; return FALSE; }
    int n = VCALL0(int, pr.list, VSLIST_COUNT);
    if (n < 0) n = 0;
    /* also rebuild when the score changed without a commit we saw (file
     * opened, part replaced): the cache holds only copies, but a stale one
     * draws the wrong curve */
    if (g_pmBuiltGen == g_pmGen && part == g_pmPart && pr.part == g_pmPartObj && pr.list == g_pmList && n == g_pmListN)
        return g_pmCount > 0;
    g_pmCount = 0;
    if (n > g_pmCap) {
        PMNote *p = (PMNote *)(g_pmNotes ? HeapReAlloc(GetProcessHeap(), 0, g_pmNotes, (n + 256) * sizeof(PMNote))
                                         : HeapAlloc(GetProcessHeap(), 0, (n + 256) * sizeof(PMNote)));
        if (!p) return FALSE;
        g_pmNotes = p;
        g_pmCap = n + 256;
    }
    g_pmPart = part;
    g_pmBuiltGen = g_pmGen;
    g_pmPartObj = pr.part; g_pmList = pr.list; g_pmListN = n;
    g_pmPartPos = pr.partPos;
    pm_load_tempo(pr.mgr);

    ListItem_t item = VMETHOD(ListItem_t, pr.list, VSLIST_ITEM);
    for (int i = 0; i < n; i++) {
        NoteRef r;
        if (!note_ref(item(pr.list, (unsigned)i), &r)) continue;
        PMNote *m = &g_pmNotes[g_pmCount++];
        m->tOn = pr.partPos + r.pos;
        m->tOff = m->tOn + r.dur;
        m->num = r.num;
        m->id = r.id;
        m->dep = 8; m->blen = 0; m->rise = 50; m->vibType = 0; m->vibLen = 0; m->vib = -1;
        m->vel = VCALL0(int, r.note, VSNOTE_VELOCITY);
        const wchar_t *ly = VCALL0(const wchar_t *, r.note, VSNOTE_LYRIC);
        lstrcpynW(m->lyric, ly ? ly : L"", 16);
        unsigned h = fnv_int(2166136261u, m->vel);
        void *attr = VCALL0(void *, r.note, VSNOTE_STYLEATTR);
        if (attr) {
            AttrGet_t get = VMETHOD(AttrGet_t, attr, VSATTR_GET);
            get(attr, L"bendDep", &m->dep);
            get(attr, L"bendLen", &m->blen);
            get(attr, L"risePort", &m->rise);
            get(attr, L"vibType", &m->vibType);
            get(attr, L"vibLen", &m->vibLen);
            static const wchar_t *const more[] = { L"fallPort", L"decay", L"accent", L"opening" };
            for (int k = 0; k < 4; k++) { int v = 0; get(attr, more[k], &v); h = fnv_int(h, v); }
        }
        PhonemeAt_t ph = VMETHOD(PhonemeAt_t, r.note, VSNOTE_PHONEME);
        unsigned np = VCALL0(unsigned, r.note, VSNOTE_PHCOUNT);
        m->nph = 0;
        for (unsigned k = 0; k < np && k < 16; k++) {
            const wchar_t *w = ph(r.note, k);
            if (w) h = fnv(h, w, lstrlenW(w) * 2);
            h = fnv_int(h, -1);
            char a[12];
            if (w && m->nph < 8 && WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof a, NULL, NULL)) m->ph[m->nph++] = ph_symbol(a);
        }
        m->hash = h;
    }
    qsort(g_pmNotes, g_pmCount, sizeof(PMNote), cmp_pmnote);
    for (int i = 0; i < g_pmCount; i++) { g_pmNotes[i].on = pm_ms(g_pmNotes[i].tOn); g_pmNotes[i].off = pm_ms(g_pmNotes[i].tOff); }
    pm_resolve_vib(g_pmNotes, g_pmCount);
    unsigned sig = fnv_int(2166136261u, (int)part);
    for (int i = 0; i < g_ntempo; i++) { sig = fnv_int(sig, (int)g_tempo[i].tick); sig = fnv(sig, &g_tempo[i].msPerTick, sizeof(double)); }
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *m = &g_pmNotes[i];
        int k[8] = { (int)m->tOn, (int)m->tOff, m->num, m->dep, m->blen, m->rise, m->vibType, m->vibLen };
        sig = fnv_int(fnv(sig, k, sizeof k), (int)m->hash);
    }
    g_pmSig = sig;

    static ControlRef pitBuf[65536], pbsBuf[4096];
    ControlList cl;
    g_pmPit = pitBuf; g_pmPbs = pbsBuf;
    g_pmNpit = open_controls(&pr, V4_CTRL_PIT, &cl) ? control_events(&cl, pitBuf, 65536) : 0;
    g_pmNpbs = open_controls(&pr, V4_CTRL_PBS, &cl) ? control_events(&cl, pbsBuf, 4096) : 0;
    qsort(pitBuf, g_pmNpit, sizeof pitBuf[0], cmp_ctrl_pos);
    qsort(pbsBuf, g_pmNpbs, sizeof pbsBuf[0], cmp_ctrl_pos);
    return g_pmCount > 0;
}

/* PIT x PBS in semitones at an absolute tick (both are held steps) */
static double pm_bend(long tick)
{
    int rel = (int)(tick - g_pmPartPos);
    int lo = 0, hi = g_pmNpit - 1, pit = 0;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pmPit[m].pos <= rel) { pit = g_pmPit[m].value; lo = m + 1; } else hi = m - 1; }
    if (!pit) return 0;
    int pbs = control_value_at(g_pmPbs, g_pmNpbs, rel, 2);
    return pit * (double)pbs / 8192.0;
}

/* ------------------------------------------------------------ shapes -- */

static double lerp_table(const float *t, int n, double pos)   /* pos in samples */
{
    if (pos <= 0) return t[0];
    if (pos >= n - 1) return t[n - 1];
    int i = (int)pos;
    double f = pos - i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

static double pm_shape(int group, int interval, double tau)
{
    static const int b[4] = { 1, 2, 5, 12 };
    if (interval < 1) interval = 1;
    if (interval > 12) interval = 12;
    int hi = 0;
    while (b[hi] < interval) hi++;
    int lo = b[hi] == interval ? hi : hi - 1;
    double pos = tau * (PT_SHAPE_N - 1);
    double vlo = lerp_table(kShape[group][lo], PT_SHAPE_N, pos);
    if (lo == hi) return vlo;
    double f = (log((double)interval) - log((double)b[lo])) / (log((double)b[hi]) - log((double)b[lo]));
    return vlo + (lerp_table(kShape[group][hi], PT_SHAPE_N, pos) - vlo) * f;
}

/* recovery 1 (at the dip) -> 0 (on the note); kind 0 up, 1 down, 2 phrase start */
static double pm_rec(int kind, int blen, double x)
{
    if (x < 0) return 1;
    double pos = x / 5.0;
    if (blen < 0) blen = 0;
    if (blen > 100) blen = 100;
    if (kind == 2) {
        int lo = blen / 10, hi = lo < 10 ? lo + 1 : 10;
        double f = (blen - lo * 10) / 10.0;
        return lerp_table(kRecStart[lo], PT_REC_N, pos) * (1 - f) + lerp_table(kRecStart[hi], PT_REC_N, pos) * f;
    }
    const float (*t)[PT_REC_N] = kind == 0 ? kRecUp : kRecDown;
    int lo = blen / 25, hi = lo < 4 ? lo + 1 : 4;
    double f = (blen - lo * 25) / 25.0;
    return lerp_table(t[lo], PT_REC_N, pos) * (1 - f) + lerp_table(t[hi], PT_REC_N, pos) * f;
}

static double vib_sample(const short *v, double t)
{
    double pos = t / PT_VIB_STEP_MS;
    if (pos < 0) return 0;
    if (pos >= PT_VIB_N - 1) return 0;
    int i = (int)pos;
    return (v[i] + (v[i + 1] - v[i]) * (pos - i)) / 10.0;
}

/* vibrato in cents, t = ms since the vibrato started, span = its length in ms */
static double pm_vib(int type, double t, double span)
{
    if (type < 1 || type > 16 || t < 0 || t > span) return 0;
    const short (*v)[PT_VIB_N] = kVib[type - 1];
    if (!kVibFades[type - 1]) return vib_sample(v[2], t);
    const float *L = kVibLenMs[type - 1];        /* templates rendered at these spans */
    if (span <= L[0]) return vib_sample(v[0], t);
    if (span >= L[2]) return vib_sample(v[2], t);
    int i = span < L[1] ? 0 : 1;
    double f = (span - L[i]) / (L[i + 1] - L[i]);
    return vib_sample(v[i], t) * (1 - f) + vib_sample(v[i + 1], t) * f;
}

/* ------------------------------------------------------------- pitch -- */

#define PM_TS (-100.0)

/* note j's own pitch (semitones, no PIT) at ms; valid inside [on + (j?TS:0), off) */
static double pm_note_pitch(int j, double ms, BOOL withVib)
{
    PMNote *n = &g_pmNotes[j], *prev = j > 0 ? &g_pmNotes[j - 1] : NULL;
    double t = ms - n->on, p;
    double dip = n->num - 0.03 * n->dep;
    if (!prev) {
        double te = 98.0;
        if (t < te) p = n->num - 0.5 + (dip - (n->num - 0.5)) * (t < 0 ? 0 : t / te);
        else p = n->num + (dip - n->num) * pm_rec(2, n->blen, t - te);
    } else {
        BOOL rising = n->num >= prev->num;
        int group = rising && n->rise > 0 ? 0 : (rising ? 2 : 1);
        double te = group == 0 ? 96 + 1.51 * n->blen : 39 + 1.57 * n->blen;
        if (t < te) {
            double tau = (t - PM_TS) / (te - PM_TS);
            if (tau < 0) tau = 0;
            double u = pm_shape(group, abs(n->num - prev->num), tau);
            p = prev->num + (dip - prev->num) * u;
        } else {
            p = n->num + (dip - n->num) * pm_rec(rising ? 0 : 1, n->blen, t - te);
        }
    }
    if (j == g_pmCount - 1) {                       /* phrase-end release */
        double L = n->off - n->on;
        if (t > L - 142) p -= (t - (L - 142)) / 142.0 * 0.45;
    }
    if (withVib && n->vibType > 0 && n->vibLen > 0) {
        double c;
        if (pm_vib_real(n, t, &c)) p += c / 100.0;
        else {
            double span = (n->off - n->on) * n->vibLen / 100.0;
            p += pm_vib(n->vibType, ms - (n->off - span), span) / 100.0;
        }
    }
    return p;
}

/* Index of the note sounding at ms (incl. the transition lead-in), or -1. */
static int pm_note_at(double ms)
{
    int lo = 0, hi = g_pmCount - 1, j = -1;
    while (lo <= hi) {              /* last note whose window starts at or before ms */
        int m = (lo + hi) / 2;
        double start = g_pmNotes[m].on + (m > 0 ? PM_TS : 0);
        if (start <= ms) { j = m; lo = m + 1; } else hi = m - 1;
    }
    if (j < 0 || ms >= g_pmNotes[j].off) return -1;
    return j;
}

/* the model's predicted pitch at an absolute tick; FALSE in rests */
static BOOL pm_model_pitch_at(long tick, BOOL withVib, BOOL withBend, double *out)
{
    double ms = pm_ms(tick);
    int j = pm_note_at(ms);
    if (j < 0) return FALSE;
    *out = pm_note_pitch(j, ms, withVib) + (withBend ? pm_bend(tick) : 0);
    return TRUE;
}

/* full pitch at an absolute tick: the last render plus the current PIT when
 * only PIT/PBS changed since, else the model; FALSE in rests */
static BOOL g_pmNoBase;          /* drawing a drag shadow: the render no longer applies */

static BOOL pm_pitch_at(long tick, BOOL withVib, BOOL withBend, double *out)
{
    if (!g_pmNoBase && pm_base_valid()) {
        if (!pm_base_at(tick, withVib, out)) return FALSE;
        if (withBend) *out += pm_bend(tick);
        return TRUE;
    }
    return pm_model_pitch_at(tick, withVib, withBend, out);
}

/* ------------------------------------------------------- drag shadow -- */
/* While the user drags notes with the editor's own tools nothing is committed
 * until mouse-up, so the curve follows a shadow copy of the score with the
 * drag applied (same quantize snapping as the editor). */

enum { DRAG_NONE, DRAG_MOVE, DRAG_START, DRAG_END };
static int g_dragMode;
static int g_dragX0, g_dragY0;
static long g_dragDT;
static int g_dragDN;
static unsigned g_dragIds[1024];
static int g_dragNids;
static PMNote *g_shadow;
static int g_shadowCap;

#define V4_PRP_MODEL   0x100         /* CMusicalPart model: note items list */
#define V4_VT_CUIAnote 0x00564148

/* ids of the notes the editor shows as selected */
static int selected_ids(unsigned *out, int max)
{
    BYTE *model = *(BYTE **)(PANE() + V4_PRP_MODEL);
    int k = 0, guard = 0;
    if (!model) return 0;
    for (BYTE **node = *(BYTE ***)(model + 0x3C); node && k < max && guard < 100000; node = (BYTE **)node[0], guard++) {
        BYTE *item = node[2];
        if (item && *(DWORD *)item == V4_VT_CUIAnote && *(int *)(item + 0x10)) out[k++] = *(unsigned *)(item + 4);
    }
    return k;
}

static void pm_drag_begin(int x, int y)
{
    g_dragMode = DRAG_NONE;
    if (!pm_refresh()) return;
    long tick = tick_at_x(x);
    int num = note_at_y(y);
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *n = &g_pmNotes[i];
        if (n->num != num || tick < n->tOn || tick >= n->tOff) continue;
        int xs = x_of_tick(n->tOn), xe = x_of_tick(n->tOff);
        g_dragMode = x - xs <= 4 ? DRAG_START : (xe - x <= 4 ? DRAG_END : DRAG_MOVE);
        g_dragX0 = x; g_dragY0 = y; g_dragDT = 0; g_dragDN = 0;
        g_dragNids = selected_ids(g_dragIds, 1024);
        BOOL in = FALSE;
        for (int k = 0; k < g_dragNids; k++) if (g_dragIds[k] == n->id) in = TRUE;
        /* selection as it was before the click: an unselected note gets selected alone */
        if (!in) { g_dragIds[0] = n->id; g_dragNids = 1; }
        return;
    }
}

static BOOL pm_drag_update(int x, int y)
{
    if (!g_dragMode) return FALSE;
    long q = quantize_ticks();
    long dt = tick_at_x(x) - tick_at_x(g_dragX0);
    if (q > 1) dt = (dt >= 0 ? (dt + q / 2) / q : -((-dt + q / 2) / q)) * q;
    int dn = g_dragMode == DRAG_MOVE ? note_at_y(y) - note_at_y(g_dragY0) : 0;
    if (dt == g_dragDT && dn == g_dragDN) return FALSE;
    g_dragDT = dt; g_dragDN = dn;
    return TRUE;
}

static void pm_drag_end(void) { g_dragMode = DRAG_NONE; }

static int cmp_pm_on(const void *a, const void *b)
{
    long x = ((const PMNote *)a)->tOn, y = ((const PMNote *)b)->tOn;
    return x < y ? -1 : x > y;
}

/* returns the array to draw from (shadow while dragging) */
static PMNote *pm_build_shadow(void)
{
    if (!g_dragMode || (!g_dragDT && !g_dragDN)) return g_pmNotes;
    if (g_shadowCap < g_pmCount) {
        PMNote *p = (PMNote *)(g_shadow ? HeapReAlloc(GetProcessHeap(), 0, g_shadow, (g_pmCount + 64) * sizeof(PMNote))
                                        : HeapAlloc(GetProcessHeap(), 0, (g_pmCount + 64) * sizeof(PMNote)));
        if (!p) return g_pmNotes;
        g_shadow = p;
        g_shadowCap = g_pmCount + 64;
    }
    memcpy(g_shadow, g_pmNotes, g_pmCount * sizeof(PMNote));
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *n = &g_shadow[i];
        BOOL sel = FALSE;
        for (int k = 0; k < g_dragNids && !sel; k++) sel = g_dragIds[k] == n->id;
        if (!sel) continue;
        if (g_dragMode == DRAG_MOVE) { n->tOn += g_dragDT; n->tOff += g_dragDT; n->num += g_dragDN; }
        else if (g_dragMode == DRAG_START) { if (n->tOn + g_dragDT < n->tOff - 5) n->tOn += g_dragDT; }
        else { if (n->tOff + g_dragDT > n->tOn + 5) n->tOff += g_dragDT; }
        if (n->num < 0) n->num = 0;
        if (n->num > 127) n->num = 127;
    }
    qsort(g_shadow, g_pmCount, sizeof(PMNote), cmp_pm_on);
    for (int i = 0; i < g_pmCount; i++) { g_shadow[i].on = pm_ms(g_shadow[i].tOn); g_shadow[i].off = pm_ms(g_shadow[i].tOff); }
    return g_shadow;
}

/* ------------------------------------------------------------ drawing -- */

static BOOL g_pmShow = TRUE, g_pmHide;
/* two layers, like the editor's own rendered curve: the pitch without
 * vibrato, and the vibrato layer on top of it */
static COLORREF g_pmColor = RGB(0x00, 0x99, 0xFF);       /* with vibrato */
static COLORREF g_pmBaseColor = RGB(0xFF, 0x66, 0x00);   /* without */

/* the curve as polyline runs: pts[] with run lengths in runs[] */
#define PM_MAXPTS 8192
typedef struct { POINT pts[PM_MAXPTS]; DWORD runs[PM_MAXPTS / 2]; int nruns; } PMLine;

static void pm_build_line(PMLine *L, BOOL withVib)
{
    L->nruns = 0;
    PMNote *real = g_pmNotes;
    g_pmNotes = pm_build_shadow();
    g_pmNoBase = g_pmNotes != real;
    RECT na = note_area();
    int n = 0, runStart = 0;
    for (int x = na.left; x < na.right && n < PM_MAXPTS; x++) {
        double p;
        if (!pm_pitch_at(tick_at_x(x), withVib, TRUE, &p)) {
            if (n > runStart) { if (n - runStart >= 2 && L->nruns < PM_MAXPTS / 2) L->runs[L->nruns++] = n - runStart; else n = runStart; }
            runStart = n;
            continue;
        }
        L->pts[n].x = x; L->pts[n].y = y_of_pitch(p); n++;
    }
    if (n - runStart >= 2 && L->nruns < PM_MAXPTS / 2) L->runs[L->nruns++] = n - runStart;
    g_pmNotes = real;
    g_pmNoBase = FALSE;
}

static void pm_stroke_line(HDC dc, const PMLine *L, COLORREF col, int rop)
{
    if (!L->nruns) return;
    RECT na = note_area();
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    int oldRop = SetROP2(dc, rop);
    HPEN pen = CreatePen(PS_SOLID, 2, col);
    HGDIOBJ old = SelectObject(dc, pen);
    PolyPolyline(dc, L->pts, L->runs, L->nruns);
    SelectObject(dc, old);
    DeleteObject(pen);
    SetROP2(dc, oldRop);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static PMLine g_line, g_baseLine, g_xorLine;
static BOOL g_xorValid;

static void pm_draw(HDC dc)
{
    g_xorValid = FALSE;          /* any repaint wipes the drag preview */
    if (!g_pmShow || g_pmHide || !g_view || !pm_refresh()) return;
    /* vibrato layer first: the base line covers it wherever there is no vibrato */
    pm_build_line(&g_line, TRUE);
    pm_build_line(&g_baseLine, FALSE);
    pm_stroke_line(dc, &g_line, themed(g_pmColor), R2_COPYPEN);
    pm_stroke_line(dc, &g_baseLine, themed(g_pmBaseColor), R2_COPYPEN);
}

/* drag preview: rubber-band the curve with XOR so it never fights the
 * editor's own drag drawing */
static void pm_drag_preview(void)
{
    if (!g_viewHwnd) return;
    HDC dc = GetDC(g_viewHwnd);
    ph_drag_preview(dc);
    if (g_pmShow) {
        /* one XOR line only: two overlapping XOR lines would cancel out */
        COLORREF xc = themed(g_pmColor) ^ 0x00FFFFFF;     /* NOTXOR with the inverse = the colour on white */
        if (g_xorValid) pm_stroke_line(dc, &g_xorLine, xc, R2_NOTXORPEN);
        pm_build_line(&g_xorLine, TRUE);
        pm_stroke_line(dc, &g_xorLine, xc, R2_NOTXORPEN);
        g_xorValid = TRUE;
    }
    ReleaseDC(g_viewHwnd, dc);
}

/* FUN_00406100: the editor's drag tracker (GetMessage loop, swallows the
 * mouse moves); returns TRUE whenever the tracked point moved. */
#define V4_TrackPoint 0x00406100
static const BYTE V4_SIG_TrackPoint[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
typedef BOOL (WINAPI *TrackPoint_t)(BYTE *wnd, int *pt, int *status, UINT *msg);
static TrackPoint_t o_TrackPoint;

static BOOL WINAPI h_TrackPoint(BYTE *wnd, int *pt, int *status, UINT *msg)
{
    BOOL r = o_TrackPoint(wnd, pt, status, msg);
    if (r && g_dragMode && wnd && pt && !IsBadReadPtr(wnd + 0x20, 4) && *(HWND *)(wnd + 0x20) == g_viewHwnd && pm_drag_update(pt[0], pt[1]))
        pm_drag_preview();
    return r;
}

static void pm_invalidate(void)
{
    InterlockedIncrement(&g_pmGen);
    if (g_viewHwnd && g_pmShow) InvalidateRect(g_viewHwnd, NULL, FALSE);
}
