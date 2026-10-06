/*
 * Engine view: exact pitch and phoneme timing straight from VOCALOID's synth.
 *
 * Silent render
 *   The editor's own "render pitch" pops a progress window. We run the same
 *   chain without it: build the part's MIDI buffer, let the synth manager
 *   export the score for the track (only parts changed since the last render
 *   are redone), and have the piano-roll pane load the frames. The editor
 *   then draws its exact rendered curve itself.
 *
 * Phoneme capture
 *   DSE4 exports one score frame per 256 samples (FUN_10017280). For every
 *   frame it keeps a segment record (a stationary phoneme or a transition
 *   between two) and the position inside that segment's voicebank
 *   articulation. The articulation (ARTp) lists where each of its phonemes
 *   begins, so every frame maps to exactly one phoneme. Frame i of a render
 *   is frame i of the pane's rendered-pitch array, which carries the tick.
 *
 * Included by pitchtools.c. Every address is verified before use; on another
 * editor or DSE4 build this part switches itself off.
 */

/* --------------------------------------------------- editor (exe) side -- */

#define V4_VT_CMainFrame      0x0055CAB4
#define V4_MF_SYNTHMGR        0x1348     /* synth manager */
#define V4_MF_SAMPLERATE      0x5C4
#define V4_SM_BUSY            0x480      /* playing / realtime synthesis active */
#define V4_SYNTHSTATE         ((void *)0x005D9C58)
#define V4_RS_RENDERED        0x0C       /* part render state: scores valid */

#define V4_BuildMidiBuffers   0x00428380 /* thiscall(frame, vector<unsigned> *partIds) */
#define V4_TrackOfPart        0x004ECD80 /* (unsigned partId) -> track index, 0x10 = none */
#define V4_SetSampleRate      0x004EB650 /* thiscall(mgr, rate) */
#define V4_RenderTrackScores  0x004E88D0 /* thiscall(mgr, track, cb, ctx, int, int, char force) */
#define V4_ProgressCb         0x004EFA80 /* stdcall(cur, total, int ctx[3]) */
#define V4_PaneLoadScores     0x004A1CE0 /* thiscall(pane) */
#define V4_RenderStateOf      0x004E9800 /* thiscall(V4_SYNTHSTATE, partId) */

typedef struct { DWORD addr; BYTE sig[8]; } FnSig;
static const FnSig kEngineSigs[] = {
    { V4_BuildMidiBuffers,  { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x1C, 0x45 } },
    { V4_TrackOfPart,       { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0xE8, 0x26, 0xA8 } },
    { V4_SetSampleRate,     { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x57 } },
    { V4_RenderTrackScores, { 0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0xB2, 0xE4 } },
    { V4_ProgressCb,        { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x55 } },
    { V4_PaneLoadScores,    { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x6C, 0xA1, 0x7C } },
    { V4_RenderStateOf,     { 0x55, 0x8B, 0xEC, 0x57, 0xFF, 0x75, 0x08, 0x8B } },
};

typedef void (THISCALL *BuildMidi_t)(void *frame, void *ids);
typedef int (THISCALL *TrackOfPart_t)(void *unused, unsigned partId);
typedef int (THISCALL *SetRate_t)(void *mgr, DWORD rate);
typedef void (THISCALL *RenderTrack_t)(void *mgr, int track, void *cb, int *ctx, int a, int b, char force);
typedef void (THISCALL *PaneLoad_t)(void *pane);
typedef BYTE *(THISCALL *RenderState_t)(void *synth, unsigned partId);

/* ------------------------------------------------------------ DSE4 side -- */

#define DSE4_STAMP            0x54850D43
#define DSE4_EXPORT_FRAME     0x17280    /* thiscall(engine, events, float score[5]) */
#define DSE4_EXPORT_IMM       0x82178    /* SEH handler pushed by its prologue */
#define DSE4_VT_ARTP          0x347BDC   /* CDBVArticulationTargetPhUPart */
#define ENG_FRAME             0x1D9C0
#define ENG_RING              0x19E98    /* {seg ring, n, ...}: 12-byte entries {seg*, float artFrame, ?} */
#define SEG_PH_L              0x10       /* phoneme ids, -1 = none */
#define SEG_PH_R              0x14
#define SEG_ARTP              0x18
#define SEG_NAME_L            0x24       /* std::string */
#define SEG_NAME_R            0x3C
#define ARTP_UNITS            0x1AC      /* vector of {begin, end, useBegin, useEnd} per phoneme */

typedef int (THISCALL *ExportFrame_t)(BYTE *eng, void *ev, float *score);
static ExportFrame_t o_ExportFrame;
static BYTE *g_dse;

typedef int (THISCALL *DoSynth_t)(void *rts, void *midi, void *scores, DWORD rate, DWORD endMs, char b, const char *s1, char c, float f1, float f2);
static DoSynth_t o_DoSynth;
typedef unsigned (THISCALL *ObjId_t)(void *midi);
static ObjId_t pMidiObjId;

static BOOL g_engineOk;          /* editor side verified */
static BOOL g_showPhonemes = TRUE;


/* ------------------------------------------------------ phoneme capture -- */

#define PH_SIL     0x7FFF
#define PH_START   0x8000         /* a new phoneme starts on this frame */
#define PH_NAMES   512
static char g_phName[PH_NAMES][12];
static int g_nPhNames;

typedef struct { unsigned partId; int n, cap; WORD *ph; } PhTrack;
#define PH_PARTS 32
static PhTrack g_parts[PH_PARTS];
static PhTrack g_cap;             /* the render in progress */
static BOOL g_capturing;
static WORD g_capPrev;
static BYTE *g_capPrevSeg;
static int g_capPrevSide;

static WORD ph_symbol(const char *s)
{
    for (int i = 0; i < g_nPhNames; i++) if (!strcmp(g_phName[i], s)) return (WORD)i;
    if (g_nPhNames >= PH_NAMES) return PH_SIL;
    lstrcpynA(g_phName[g_nPhNames], s, sizeof g_phName[0]);
    return (WORD)g_nPhNames++;
}

static BOOL mem_ok(const void *p, DWORD n)
{
    MEMORY_BASIC_INFORMATION m;
    if ((DWORD)p < 0x10000 || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return FALSE;
    return (DWORD)p + n <= (DWORD)m.BaseAddress + m.RegionSize;
}

/* MSVC std::string: 16-byte buffer or pointer, then size, capacity */
static const char *msstr(const BYTE *s)
{
    DWORD size = *(DWORD *)(s + 16), cap = *(DWORD *)(s + 20);
    if (size >= 64 || cap < size) return NULL;
    const char *p = cap >= 16 ? *(const char **)s : (const char *)s;
    return cap >= 16 && !mem_ok(p, size + 1) ? NULL : p;
}

static void cap_push(WORD v)
{
    if (g_cap.n >= g_cap.cap) {
        int nc = g_cap.cap ? g_cap.cap * 2 : 8192;
        WORD *p = (WORD *)(g_cap.ph ? HeapReAlloc(GetProcessHeap(), 0, g_cap.ph, nc * sizeof(WORD)) : HeapAlloc(GetProcessHeap(), 0, nc * sizeof(WORD)));
        if (!p) { g_capturing = FALSE; return; }
        g_cap.ph = p; g_cap.cap = nc;
    }
    g_cap.ph[g_cap.n++] = v;
}

/* the phoneme sounding on the frame just exported */
static void capture_frame(BYTE *eng)
{
    int f = *(int *)(eng + ENG_FRAME) - 1;
    DWORD *ring = *(DWORD **)(eng + ENG_RING);
    WORD sym = PH_SIL;
    BYTE *seg = NULL;
    int side = 0;
    if (f >= 0 && mem_ok(ring, 8) && (int)ring[1] > 0) {
        DWORD *e = (DWORD *)(ring[0] + (DWORD)(f % (int)ring[1]) * 12);
        seg = mem_ok(e, 12) ? (BYTE *)e[0] : NULL;
        if (seg && mem_ok(seg, 0x60)) {
            int l = *(int *)(seg + SEG_PH_L), r = *(int *)(seg + SEG_PH_R);
            const char *nl = msstr(seg + SEG_NAME_L), *nr = msstr(seg + SEG_NAME_R);
            if (l >= 0 && nl) {
                side = 0;
                if (r >= 0 && nr) {
                    /* transition: the right phoneme starts at its unit's first articulation frame */
                    float pos = *(float *)&e[1];
                    BYTE *art = *(BYTE **)(seg + SEG_ARTP);
                    int boundary = -1;
                    if (mem_ok(art, ARTP_UNITS + 8) && *(DWORD *)art == (DWORD)g_dse + DSE4_VT_ARTP) {
                        int *ub = *(int **)(art + ARTP_UNITS), *ue = *(int **)(art + ARTP_UNITS + 4);
                        if (ub && ue > ub && (ue - ub) % 4 == 0 && (ue - ub) / 4 >= 2 && mem_ok(ub, 32)) boundary = ub[4];
                    }
                    if (boundary < 0) boundary = 0x7FFFFFFF;   /* unknown: treat as the left phoneme throughout */
                    side = pos >= (float)boundary;
                }
                const char *name = side ? nr : nl;
                if (strcmp(name, "Sil") && strcmp(name, "sil")) sym = ph_symbol(name);
            }
        }
    }
    WORD v = sym;
    if (sym != PH_SIL) {
        BOOL cont = (g_capPrev & 0x7FFF) == sym && (seg == g_capPrevSeg ? side == g_capPrevSide : TRUE);
        if (!cont) v |= PH_START;
    }
    g_capPrev = sym; g_capPrevSeg = seg; g_capPrevSide = side;
    cap_push(v);
}

static int THISCALL h_ExportFrame(BYTE *eng, void *ev, float *score)
{
    int r = o_ExportFrame(eng, ev, score);
    if (g_capturing && score) capture_frame(eng);
    return r;
}

static PhTrack *ph_track(unsigned partId, BOOL create)
{
    for (int i = 0; i < PH_PARTS; i++) if (g_parts[i].ph && g_parts[i].partId == partId) return &g_parts[i];
    if (!create) return NULL;
    for (int i = 0; i < PH_PARTS; i++) if (!g_parts[i].ph) return &g_parts[i];
    static int next;
    PhTrack *t = &g_parts[next++ % PH_PARTS];
    HeapFree(GetProcessHeap(), 0, t->ph);
    memset(t, 0, sizeof *t);
    return t;
}

/* every score render of a part (ours or the editor's button) goes through here */
static int THISCALL h_DoSynth(void *rts, void *midi, void *scores, DWORD rate, DWORD endMs, char b, const char *s1, char c, float f1, float f2)
{
    if (!o_ExportFrame) { int r = o_DoSynth(rts, midi, scores, rate, endMs, b, s1, c, f1, f2); return r; }
    g_cap.n = 0; g_capPrev = PH_SIL; g_capPrevSeg = NULL;
    g_capturing = TRUE;
    int r = o_DoSynth(rts, midi, scores, rate, endMs, b, s1, c, f1, f2);
    g_capturing = FALSE;
    unsigned part = pMidiObjId ? pMidiObjId(midi) : 0;
    if (r && part && g_cap.n) {
        PhTrack *t = ph_track(part, TRUE);
        WORD *p = (WORD *)(t->ph ? HeapReAlloc(GetProcessHeap(), 0, t->ph, g_cap.n * sizeof(WORD)) : HeapAlloc(GetProcessHeap(), 0, g_cap.n * sizeof(WORD)));
        if (p) {
            memcpy(p, g_cap.ph, g_cap.n * sizeof(WORD));
            t->ph = p; t->partId = part; t->n = g_cap.n; t->cap = g_cap.n;
        }
    }
    g_cap.n = 0;
    return r;
}

/* ---------------------------------------------------------------- setup -- */

static BOOL engine_hook_dse(void)
{
    if (o_ExportFrame) return TRUE;
    g_dse = (BYTE *)GetModuleHandleA("DSE4.dll");
    if (!g_dse) return FALSE;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_dse + ((IMAGE_DOS_HEADER *)g_dse)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != DSE4_STAMP) { A->Log("engine: DSE4 build %08lx not supported, phonemes off", nt->FileHeader.TimeDateStamp); g_dse = NULL; return FALSE; }
    BYTE expect[7] = { 0x6A, 0xFF, 0x68 };
    *(DWORD *)(expect + 3) = (DWORD)g_dse + DSE4_EXPORT_IMM;
    if (!A->HookCode(g_dse + DSE4_EXPORT_FRAME, expect, 7, h_ExportFrame, (void **)&o_ExportFrame)) { o_ExportFrame = NULL; return FALSE; }
    return TRUE;
}

static void engine_init(void)
{
    g_engineOk = TRUE;
    for (unsigned i = 0; i < sizeof kEngineSigs / sizeof kEngineSigs[0]; i++)
        if (memcmp((void *)kEngineSigs[i].addr, kEngineSigs[i].sig, 8)) { g_engineOk = FALSE; A->Log("engine: editor build mismatch at %08lx, silent render off", kEngineSigs[i].addr); break; }
    HMODULE dscl = GetModuleHandleA("dscl4.dll");
    pMidiObjId = dscl ? (ObjId_t)GetProcAddress(dscl, "?objID@CDaisyMidiBuffer@@QAEIXZ") : NULL;
    A->HookImport("dscl4.dll", "?DoSynthesis@CRtSynthesis@@QAEHPAVCDaisyMidiBuffer@@AAV?$vector@UtagDSE_score@@V?$allocator@UtagDSE_score@@@std@@@std@@KK_NPBD2MM@Z",
                  (void *)h_DoSynth, (void **)&o_DoSynth);
}

/* ---------------------------------------------------------- silent render -- */

static BYTE *main_frame(void)
{
    static BYTE *frame;
    static HWND frameHwnd;
    HWND main = A->GetMainWindow();
    if (!main) return NULL;
    if (!frame || frameHwnd != main || *(DWORD *)frame != V4_VT_CMainFrame) {
        frame = vh_find_cwnd(V4_VT_CMainFrame, main);
        frameHwnd = main;
    }
    return frame;
}

static BYTE *render_state(unsigned partId)
{
    return ((RenderState_t)V4_RenderStateOf)(V4_SYNTHSTATE, partId);
}

/* TRUE when the open part has exact frames for its current state */
static BOOL engine_has_frames(void)
{
    if (!g_view) return FALSE;
    RenderFrame *b = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *e = *(RenderFrame **)(PANE() + PRP_RENDER_END);
    if (!b || e <= b) return FALSE;
    if (!g_engineOk) return TRUE;
    BYTE *rs = render_state(*(unsigned *)(PANE() + V4_PRP_PARTID));
    return rs && *(int *)(rs + V4_RS_RENDERED);
}

/* Render the open part's score without the progress window. Returns ms, -1 if skipped. */
static int engine_render(void)
{
    if (!g_engineOk || !g_view) return -1;
    BYTE *frame = main_frame();
    BYTE *mgr = frame ? *(BYTE **)(frame + V4_MF_SYNTHMGR) : NULL;
    if (!mgr || !mem_ok(mgr, V4_SM_BUSY + 4) || *(int *)(mgr + V4_SM_BUSY)) return -1;   /* playing */
    unsigned part = *(unsigned *)(PANE() + V4_PRP_PARTID);
    BYTE *rs = render_state(part);
    if (!rs) return -1;
    if (*(int *)(rs + V4_RS_RENDERED)) {
        RenderFrame *b = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *e = *(RenderFrame **)(PANE() + PRP_RENDER_END);
        if (b && e > b) return -1;              /* already shown */
    }
    int track = ((TrackOfPart_t)V4_TrackOfPart)(NULL, part);
    if (track < 0 || track >= 0x10) return -1;
    engine_hook_dse();
    DWORD t0 = GetTickCount();
    unsigned ids[1] = { part };
    unsigned *vec[3] = { ids, ids + 1, ids + 1 };   /* std::vector<unsigned> */
    ((BuildMidi_t)V4_BuildMidiBuffers)(frame, vec);
    ((SetRate_t)V4_SetSampleRate)(mgr, *(DWORD *)(frame + V4_MF_SAMPLERATE));
    int ctx[3] = { 0, 0, 0 };                        /* {cancelled, progress dialog, pump} */
    g_rendering = TRUE;
    ((RenderTrack_t)V4_RenderTrackScores)(mgr, track, (void *)V4_ProgressCb, ctx, 0, 0, 0);
    g_rendering = FALSE;
    if (g_view) {
        ((PaneLoad_t)V4_PaneLoadScores)(PANE());
        /* a plain invalidate only repaints what the editor thinks changed,
         * which would leave the estimated curve overlay behind */
        RedrawWindow(g_viewHwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    int ms = (int)(GetTickCount() - t0);
    A->Log("engine: part %u rendered in %d ms", part, ms);
    return ms;
}

/* ------------------------------------------------------- phoneme drawing -- */
/* Exact: runs of one phoneme straight from the captured render, while that
 * render is current. Real-time otherwise (right after an edit, while dragging
 * notes): each note's phonemes are laid out where the last render put them for
 * that note (relative to its start, or to its end for trailing consonants), or
 * estimated from the average phoneme lengths seen so far. */

typedef struct { long t0, t1; WORD sym; int note; } PhRun;   /* note: index into the notes drawn from */

static BOOL ph_vowel(WORD sym)
{
    return sym < g_nPhNames && strchr("aiueoAEIOUQV@3M", g_phName[sym][0]) != NULL;
}

static int ph_exact_runs(PhRun *runs, int max)
{
    unsigned part = *(unsigned *)(PANE() + V4_PRP_PARTID);
    PhTrack *t = ph_track(part, FALSE);
    RenderFrame *fb = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *fe = *(RenderFrame **)(PANE() + PRP_RENDER_END);
    if (!t || !fb || fe - fb != t->n) return 0;
    const PMNote *notes = g_pmNotes;
    int nn = g_pmCount, nr = 0;

    for (int i = 0; i < t->n; i++) {
        WORD v = t->ph[i], sym = v & 0x7FFF;
        if (sym == PH_SIL) continue;
        if ((v & PH_START) || !nr || runs[nr - 1].sym != sym || runs[nr - 1].t1 != (long)fb[i].tick) {
            if (nr >= max) break;
            runs[nr].t0 = fb[i].tick; runs[nr].sym = sym; runs[nr].note = -1; nr++;
        }
        runs[nr - 1].t1 = i + 1 < t->n ? (long)fb[i + 1].tick : (long)fb[i].tick + 6;
    }

    /* which note each phoneme belongs to: walk the notes' own phoneme lists in order */
    int ni = 0, pi = 0;
    for (int r = 0; r < nr; r++) {
        WORD sym = runs[r].sym;
        while (ni < nn && notes[ni].tOff <= runs[r].t0 && pi >= notes[ni].nph) { ni++; pi = 0; }
        if (ni < nn && pi < notes[ni].nph && notes[ni].ph[pi] == sym) { runs[r].note = ni; pi++; continue; }
        if (ni + 1 < nn && notes[ni + 1].nph && notes[ni + 1].ph[0] == sym) { ni++; pi = 1; runs[r].note = ni; continue; }
        /* no match (e.g. a "-" sustain or an engine substitution): by time */
        long mid = (runs[r].t0 + runs[r].t1) / 2;
        int best = -1;
        for (int k = 0; k < nn; k++) if (mid >= notes[k].tOn && mid < notes[k].tOff) { best = k; break; }
        if (best < 0) for (int k = 0; k < nn; k++) if (notes[k].tOn >= mid) { best = k; break; }
        runs[r].note = best;
        if (best >= 0 && best > ni) { ni = best; pi = notes[best].nph; }
    }
    return nr;
}

/* --- per-note layout from the last exact render --- */

typedef struct { unsigned id, hash; int first, n; } PhCache;
typedef struct { float ms; WORD sym; BYTE atEnd; } PhPos;     /* ms from note on (or off if atEnd) */
#define PH_CACHE_NOTES 8192
#define PH_CACHE_POS   32768
static PhCache g_phc[PH_CACHE_NOTES];
static int g_nphc;
static PhPos g_php[PH_CACHE_POS];
static int g_nphp;
static float g_phLenSum[PH_NAMES];
static int g_phLenN[PH_NAMES];
static LONG g_phCacheGen;

static int cmp_phc(const void *a, const void *b)
{
    unsigned x = ((const PhCache *)a)->id, y = ((const PhCache *)b)->id;
    return x < y ? -1 : x > y;
}

static void ph_cache_update(const PhRun *runs, int nr)
{
    if (g_phCacheGen == g_pmGen) return;
    g_phCacheGen = g_pmGen;
    g_nphc = 0; g_nphp = 0;
    for (int k = 0; k < g_pmCount && g_nphc < PH_CACHE_NOTES; k++) {
        const PMNote *n = &g_pmNotes[k];
        PhCache *c = &g_phc[g_nphc];
        c->id = n->id; c->hash = n->hash; c->first = g_nphp; c->n = 0;
        BOOL nucleus = FALSE;
        for (int r = 0; r < nr && g_nphp < PH_CACHE_POS; r++) {
            if (runs[r].note != k) continue;
            double a = pm_ms(runs[r].t0), len = pm_ms(runs[r].t1) - a;
            PhPos *p = &g_php[g_nphp++];
            p->sym = runs[r].sym;
            c->n++;
            /* the first phoneme that starts on the note (within a frame) carries it */
            if (!nucleus && runs[r].t0 >= n->tOn - 6) { nucleus = TRUE; p->atEnd = 0; p->ms = (float)(a - n->on); continue; }
            p->atEnd = (BYTE)nucleus;
            p->ms = (float)(a - (nucleus ? n->off : n->on));
            if (p->sym < PH_NAMES && len > 0 && len < 1000) { g_phLenSum[p->sym] += (float)len; g_phLenN[p->sym]++; }
        }
        if (c->n) g_nphc++;
        else g_nphp = c->first;
    }
    qsort(g_phc, g_nphc, sizeof g_phc[0], cmp_phc);
}

static const PhCache *ph_cached(const PMNote *n)
{
    int lo = 0, hi = g_nphc - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_phc[m].id == n->id) return g_phc[m].hash == n->hash ? &g_phc[m] : NULL;
        if (g_phc[m].id < n->id) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

/* typical length of a consonant, ms */
static double ph_len(WORD sym)
{
    if (sym >= PH_NAMES || !g_phLenN[sym]) return 60.0;
    double l = g_phLenSum[sym] / g_phLenN[sym];
    return l < 20 ? 20 : (l > 300 ? 300 : l);
}

typedef struct { double t; int note; WORD sym; } PhItem;

static int cmp_phitem(const void *a, const void *b)
{
    const PhItem *x = (const PhItem *)a, *y = (const PhItem *)b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->note - y->note;
}

static int ph_realtime_runs(const PMNote *notes, int nn, PhRun *runs, int max)
{
    static PhItem it[16384];
    int ni = 0;
    for (int k = 0; k < nn; k++) {
        const PMNote *n = &notes[k];
        const PhCache *c = n->nph ? ph_cached(n) : NULL;
        if (c && ni + c->n <= 16384) {
            for (int j = 0; j < c->n; j++) {
                const PhPos *p = &g_php[c->first + j];
                double t = (p->atEnd ? n->off : n->on) + p->ms;
                if (p->atEnd && t < n->on) t = n->on;
                it[ni].t = t; it[ni].note = k; it[ni].sym = p->sym; ni++;
            }
            continue;
        }
        if (!n->nph || ni + n->nph > 16384) continue;
        /* estimate: consonants before the first vowel lead into the note,
         * the vowel starts on it, consonants after it close the note */
        int nuc = 0;
        for (int j = 0; j < n->nph; j++) if (ph_vowel(n->ph[j])) { nuc = j; break; }
        double t = n->on;
        for (int j = nuc - 1; j >= 0; j--) { t -= ph_len(n->ph[j]); it[ni + j].t = t; }
        it[ni + nuc].t = n->on;
        t = n->off;
        for (int j = n->nph - 1; j > nuc; j--) { t -= ph_len(n->ph[j]); if (t < n->on) t = n->on; it[ni + j].t = t; }
        for (int j = 0; j < n->nph; j++) { it[ni + j].note = k; it[ni + j].sym = n->ph[j]; }
        ni += n->nph;
    }
    qsort(it, ni, sizeof it[0], cmp_phitem);
    int nr = 0;
    for (int i = 0; i < ni && nr < max; i++) {
        /* a phoneme lasts until the next one starts, but not past its note */
        double end = notes[it[i].note].off;
        if (i + 1 < ni && it[i + 1].t < end) end = it[i + 1].t;
        if (end <= it[i].t) continue;
        runs[nr].t0 = pm_tick(it[i].t); runs[nr].t1 = pm_tick(end);
        runs[nr].sym = it[i].sym; runs[nr].note = it[i].note;
        nr++;
    }
    return nr;
}

/* --- drawing --- */

typedef struct { RECT rc; WORD sym; } PhBox;

static int ph_lane_h(void)
{
    int rowH = PANE_I(V4_PRP_ROWH);
    return rowH < 10 ? 10 : (rowH > 15 ? 15 : rowH);
}

static int ph_boxes(const PhRun *runs, int nr, const PMNote *notes, PhBox *out, int max)
{
    RECT na = note_area();
    int h = ph_lane_h(), k = 0;
    for (int r = 0; r < nr && k < max; r++) {
        if (runs[r].note < 0) continue;
        int x0 = x_of_tick(runs[r].t0), x1 = x_of_tick(runs[r].t1);
        if (x1 < na.left || x0 > na.right) continue;
        if (x1 - x0 < 2) x1 = x0 + 2;
        int y1 = y_of_note(notes[runs[r].note].num), y0 = y1 - h;
        if (y1 < na.top || y0 > na.bottom) continue;
        SetRect(&out[k].rc, x0, y0, x1, y1);
        out[k].sym = runs[r].sym;
        k++;
    }
    return k;
}

#define PH_VOWEL_COL RGB(0x2B, 0x5D, 0x8C)
#define PH_CONS_COL  RGB(0x7A, 0x45, 0x9E)

static void ph_paint(HDC dc, const PhBox *b, int n)
{
    if (!n) return;
    RECT na = note_area();
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    int h = ph_lane_h();
    static HFONT font;
    static int fontH;
    if (!font || fontH != h) {
        if (font) DeleteObject(font);
        fontH = h;
        font = CreateFontW(-(h - 3), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    }
    HGDIOBJ oldFont = SelectObject(dc, font);
    int oldBk = SetBkMode(dc, TRANSPARENT);
    COLORREF oldText = SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    HBRUSH vowelBr = CreateSolidBrush(PH_VOWEL_COL), consBr = CreateSolidBrush(PH_CONS_COL);
    HBRUSH edgeBr = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
    for (int i = 0; i < n; i++) {
        RECT box = b[i].rc;
        FillRect(dc, &box, ph_vowel(b[i].sym) ? vowelBr : consBr);
        RECT e0 = { box.left, box.top, box.left + 1, box.bottom }, e1 = { box.right - 1, box.top, box.right, box.bottom };
        FillRect(dc, &e0, edgeBr);
        FillRect(dc, &e1, edgeBr);
        if (b[i].sym >= g_nPhNames) continue;
        const char *name = g_phName[b[i].sym];
        SIZE sz;
        int len = lstrlenA(name);
        GetTextExtentPoint32A(dc, name, len, &sz);
        if (sz.cx + 4 <= box.right - box.left) {
            RECT tr = { box.left + 1, box.top, box.right - 1, box.bottom };
            DrawTextA(dc, name, len, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    DeleteObject(vowelBr); DeleteObject(consBr); DeleteObject(edgeBr);
    SetTextColor(dc, oldText);
    SetBkMode(dc, oldBk);
    SelectObject(dc, oldFont);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

/* XOR boxes for the drag preview: drawing them twice erases them */
static void ph_xor(HDC dc, const PhBox *b, int n)
{
    if (!n) return;
    RECT na = note_area();
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    HBRUSH vowelBr = CreateSolidBrush(PH_VOWEL_COL ^ 0x00FFFFFF), consBr = CreateSolidBrush(PH_CONS_COL ^ 0x00FFFFFF);
    HGDIOBJ old = SelectObject(dc, vowelBr);
    for (int i = 0; i < n; i++) {
        SelectObject(dc, ph_vowel(b[i].sym) ? vowelBr : consBr);
        PatBlt(dc, b[i].rc.left, b[i].rc.top, b[i].rc.right - b[i].rc.left, b[i].rc.bottom - b[i].rc.top, PATINVERT);
    }
    SelectObject(dc, old);
    DeleteObject(vowelBr); DeleteObject(consBr);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static PhRun g_phRuns[16384];
static PhBox g_phBoxes[8192];
static PhBox g_phXorBoxes[8192];
static int g_nphXor;
static BOOL g_phXorValid;

static void engine_draw_phonemes(HDC dc)
{
    g_phXorValid = FALSE;          /* any repaint wipes the drag preview */
    /* while a drag is shown the lane follows it as an XOR preview instead */
    if (!g_showPhonemes || !g_view || g_pmHide || !pm_refresh()) return;
    int nr = 0;
    if (engine_has_frames() && (nr = ph_exact_runs(g_phRuns, 16384)) > 0) ph_cache_update(g_phRuns, nr);
    else nr = ph_realtime_runs(g_pmNotes, g_pmCount, g_phRuns, 16384);
    ph_paint(dc, g_phBoxes, ph_boxes(g_phRuns, nr, g_pmNotes, g_phBoxes, 8192));
}

static void ph_drag_preview(HDC dc)
{
    if (!g_showPhonemes || !g_view || !pm_refresh()) return;
    if (g_phXorValid) ph_xor(dc, g_phXorBoxes, g_nphXor);
    const PMNote *notes = pm_build_shadow();
    int nr = ph_realtime_runs(notes, g_pmCount, g_phRuns, 16384);
    g_nphXor = ph_boxes(g_phRuns, nr, notes, g_phXorBoxes, 8192);
    ph_xor(dc, g_phXorBoxes, g_nphXor);
    g_phXorValid = TRUE;
}
