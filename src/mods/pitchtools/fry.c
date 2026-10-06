/*
 * Vocal Fry & Growl (Mods > Vocal Fry & Growl...): on the selected notes.
 *
 * Pitch sensitivity (PBS) goes to its maximum (24 semitones) under each note
 * and PIT draws a zig-zag:
 *   Vocal Fry (down)  from the note's pitch all the way down and back, again and again
 *   Vocal Fry (up)    the same, upwards
 *   Growl             a rough zig-zag around the note
 * Fade in / out shape how deep it goes, Randomness varies each dip, and
 * breathiness, brightness, clearness and dynamics can follow the same fade.
 * The window shows a live preview. Everything is put back right after each
 * note, and one Apply is one Undo step.
 *
 * Included by pitchtools.c (after notetools.c: it uses its helpers).
 */

enum { FRY_DOWN, FRY_UP, FRY_GROWL, FRY_MODES };
enum { FC_BRE, FC_BRI, FC_CLE, FC_DYN, FC_COUNT };

typedef struct {
    int mode;
    int depth10;               /* tenths of a semitone */
    int hz;                    /* dips (or zig-zags) per second */
    int jitter;                /* % each dip's depth may vary */
    int fadeIn, fadeOut;       /* % of the note */
    int val[FC_COUNT];         /* 0..127 */
    BOOL use[FC_COUNT];
} FryParams;

#define FRY_DEFAULTS { FRY_DOWN, 180, 12, 35, 10, 20, { 70, 40, 20, 64 }, { TRUE, TRUE, FALSE, FALSE } }
#define GROWL_DEFAULTS { FRY_GROWL, 25, 40, 30, 15, 20, { 40, 90, 20, 90 }, { TRUE, TRUE, FALSE, TRUE } }
static FryParams g_fry = FRY_DEFAULTS;
static const int kFcType[FC_COUNT] = { V4_CTRL_BRE, V4_CTRL_BRI, V4_CTRL_CLE, V4_CTRL_DYN };
static const int kFcDefault[FC_COUNT] = { 0, 64, 0, 64 };
static const COLORREF kFcColor[FC_COUNT] = { RGB(0x3C, 0x9A, 0xE0), RGB(0xE8, 0xA3, 0x17), RGB(0x2E, 0xB8, 0x6B), RGB(0x9B, 0x59, 0xD0) };
static const wchar_t *const kFcName[FC_COUNT] = { L"Breathiness (BRE)", L"Brightness (BRI)", L"Clearness (CLE)", L"Dynamics (DYN)" };
static const wchar_t *const kFcKey[FC_COUNT] = { L"BRE", L"BRI", L"CLE", L"DYN" };
static const wchar_t *const kFryModeName[FRY_MODES] = { L"Vocal Fry (down)", L"Vocal Fry (up)", L"Growl" };

/* --------------------------------------------------------- the shape -- */

/* how much of the effect applies at u (0..1 through the note) */
static double fry_env(const FryParams *p, double u)
{
    double fi = p->fadeIn / 100.0, fo = p->fadeOut / 100.0, env = 1;
    if (fi > 0 && u < fi) env = u / fi;
    if (fo > 0 && u > 1 - fo) env = fmin(env, (1 - u) / fo);
    return env < 0 ? 0 : env;
}

/* pitch offset in semitones at u, ph cycles into the note; seed makes each note's dips its own */
static double fry_pitch(const FryParams *p, double u, double ph, unsigned seed)
{
    double depth = p->depth10 / 10.0 * fry_env(p, u);
    long cycle = (long)floor(ph);
    double f = ph - cycle;
    /* each dip a little deeper or shallower than the last */
    unsigned h = (unsigned)cycle * 2654435761u ^ seed * 40503u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    depth *= 1 - p->jitter / 100.0 * (h % 1000) / 1000.0;
    if (p->mode == FRY_GROWL) return depth * (4 * fabs(f - 0.5) - 1) * -1;   /* around the pitch: up, down, up */
    double v = f < 0.5 ? f * 2 : (1 - f) * 2;                             /* 0 at the pitch, 1 at the far end */
    return (p->mode == FRY_UP ? 1 : -1) * depth * v;
}

/* ---------------------------------------------------------- writing -- */

typedef struct { ControlList cl; ControlRef *ev; int n; } FryCurve;

static int cmp_ctrl_ref(const void *a, const void *b) { return ((const ControlRef *)a)->pos - ((const ControlRef *)b)->pos; }

static BOOL fry_open(PartRef *pr, int type, FryCurve *c, ControlRef *buf, int max)
{
    if (!open_controls(pr, type, &c->cl)) return FALSE;
    c->ev = buf;
    c->n = control_events(&c->cl, buf, max);
    qsort(buf, c->n, sizeof buf[0], cmp_ctrl_ref);
    return TRUE;
}

/* drop the curve's own events in [a, b] (part ticks) */
static BOOL fry_clear(void *ed, FryCurve *c, int a, int b)
{
    RemoveControl_t removeCtl = VMETHOD(RemoveControl_t, ed, VSED_REMOVE_CONTROL);
    for (int i = 0; i < c->n; i++) if (c->ev[i].pos >= a && c->ev[i].pos <= b && !removeCtl(ed, c->ev[i].id)) return FALSE;
    return TRUE;
}

static BOOL fry_put(void *ed, FryCurve *c, int type, int pos, int value)
{
    UpsertControl_t upsert = VMETHOD(UpsertControl_t, ed, VSED_UPSERT_CONTROL);
    VSControlData d = { pos, type, value };
    return upsert(ed, c->cl.listId, &d);
}

/* one note: PBS 24 and the PIT zig-zag, the voice controls, all restored at its end */
static BOOL fry_note(void *ed, PartRef *pr, const NoteRef *r, FryCurve *pit, FryCurve *pbs, FryCurve *fc)
{
    int a = r->pos, b = r->pos + r->dur;
    long ta = pr->partPos + a, tb = pr->partPos + b;
    double msA = pm_ms(ta), msB = pm_ms(tb), len = msB - msA;
    if (len <= 1) return TRUE;
    double msPerTick = len / r->dur, period = 1000.0 / (g_fry.hz > 0 ? g_fry.hz : 12);
    int step = (int)lround(period / 12 / msPerTick);          /* 12 points per dip */
    if (step < 2) step = 2;

    int pitAfter = control_value_at(pit->ev, pit->n, b, 0), pbsAfter = control_value_at(pbs->ev, pbs->n, b, 2);
    int fcAfter[FC_COUNT], fcBase[FC_COUNT];
    for (int k = 0; k < FC_COUNT; k++) {
        fcAfter[k] = control_value_at(fc[k].ev, fc[k].n, b, kFcDefault[k]);
        fcBase[k] = control_value_at(fc[k].ev, fc[k].n, a, kFcDefault[k]);
    }

    BOOL ok = fry_clear(ed, pit, a, b - 1) && fry_clear(ed, pbs, a + 1, b - 1) && fry_put(ed, pbs, V4_CTRL_PBS, a, 24);
    int last = 0x7FFFFFFF;
    for (int t = a; t < b && ok; t += step) {
        double ms = pm_ms(pr->partPos + t) - msA;
        double off = fry_pitch(&g_fry, ms / len, ms / period, r->id);
        if (off > 24) off = 24;
        if (off < -24) off = -24;
        int v = (int)lround(off / 24 * 8192);
        if (v > 8191) v = 8191;
        if (v < -8192) v = -8192;
        if (v == last) continue;
        ok = fry_put(ed, pit, V4_CTRL_PIT, t, v);
        last = v;
    }
    if (ok) ok = fry_put(ed, pit, V4_CTRL_PIT, b, pitAfter) && fry_put(ed, pbs, V4_CTRL_PBS, b, pbsAfter);

    for (int k = 0; k < FC_COUNT && ok; k++) {
        if (!g_fry.use[k]) continue;
        ok = fry_clear(ed, &fc[k], a, b - 1);
        int lastv = -1;
        for (int t = a; t < b && ok; t += 15) {
            double u = (pm_ms(pr->partPos + t) - msA) / len;
            int v = (int)lround(fcBase[k] + (g_fry.val[k] - fcBase[k]) * fry_env(&g_fry, u));
            if (v == lastv) continue;
            ok = fry_put(ed, &fc[k], kFcType[k], t, v);
            lastv = v;
        }
        if (ok) ok = fry_put(ed, &fc[k], kFcType[k], b, fcAfter[k]);
    }
    return ok;
}

static ControlRef g_frPit[65536], g_frPbs[4096], g_frFc[FC_COUNT][16384];

static BOOL fry_curves(PartRef *pr, FryCurve *pit, FryCurve *pbs, FryCurve *fc)
{
    if (!fry_open(pr, V4_CTRL_PIT, pit, g_frPit, 65536) || !fry_open(pr, V4_CTRL_PBS, pbs, g_frPbs, 4096)) return FALSE;
    for (int k = 0; k < FC_COUNT; k++) if (!fry_open(pr, kFcType[k], &fc[k], g_frFc[k], 16384)) return FALSE;
    return TRUE;
}

static void fry_apply(void)
{
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(L"Select the notes to growl or fry"); return; }
    if (!pm_refresh()) { toast_show(L"No notes"); return; }
    FryCurve pit, pbs, fc[FC_COUNT];
    if (!fry_curves(&pr, &pit, &pbs, fc)) { toast_show(L"The control tracks weren't found"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target || g_nt[i].r.dur < 30) continue;
        ok = fry_note(ed, &pr, &g_nt[i].r, &pit, &pbs, fc);
        if (ok) changed++;
        else A->Log("fry: write failed on note %u, error %08x", g_nt[i].r.id, VCALL0(unsigned, ed, VSED_LASTERROR));
    }
    ok = nt_end(&pr, ed, ok, changed);
    wchar_t what[96];
    wsprintfW(what, L"%s on %%d note%%s", kFryModeName[g_fry.mode]);
    nt_report(ok, changed, what, L"The selected notes are too short");
}

/* clears PIT, PBS and the four voice controls under the selected notes */
static void fry_remove(void)
{
    PartRef pr;
    int nt, n = nt_collect(&pr, FALSE, &nt), changed = 0;
    if (!nt) { toast_show(L"Select the notes to clear"); return; }
    FryCurve pit, pbs, fc[FC_COUNT];
    if (!fry_curves(&pr, &pit, &pbs, fc)) { toast_show(L"The control tracks weren't found"); return; }
    void *ed = nt_begin(&pr);
    if (!ed) return;
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!g_nt[i].target) continue;
        int a = g_nt[i].r.pos, b = a + g_nt[i].r.dur;
        ok = fry_clear(ed, &pit, a, b) && fry_clear(ed, &pbs, a, b);
        for (int k = 0; k < FC_COUNT && ok; k++) ok = fry_clear(ed, &fc[k], a, b);
        if (ok) changed++;
    }
    ok = nt_end(&pr, ed, ok, changed);
    nt_report(ok, changed, L"Cleared the effect from %d note%s", L"Nothing to clear");
}

/* ---------------------------------------------------------- preview -- */
/* A sample note of PREVIEW_MS: the note bar, the pitch it will sing (red) on
 * a +-24 semitone scale, and underneath the ticked voice controls. */

#define PREVIEW_MS 800.0

static HWND g_fryPreview;
static HFONT g_fryFont;

static void fry_preview_paint(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(0xFA, 0xFA, 0xFA));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    int ctrlH = 46, pitchBottom = rc.bottom - ctrlH - 6;
    int left = 44, right = rc.right - 10, top = 8, w = right - left;
    /* +-24 semitones; the note sits in the middle */
    double range = 24;
    int midY = (top + pitchBottom) / 2;
    #define FRY_Y(st) (midY - (int)((st) / range * (pitchBottom - top) / 2))
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(0xE2, 0xE2, 0xE2)), axis = CreatePen(PS_DOT, 1, RGB(0xB0, 0xB0, 0xB0));
    HGDIOBJ op = SelectObject(mem, grid);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ of = SelectObject(mem, g_fryFont);
    SetTextColor(mem, RGB(0x80, 0x80, 0x80));
    for (int st = -24; st <= 24; st += 12) {
        int y = FRY_Y(st);
        SelectObject(mem, st ? grid : axis);
        MoveToEx(mem, left, y, NULL); LineTo(mem, right, y);
        wchar_t t[8];
        wsprintfW(t, st > 0 ? L"+%d" : L"%d", st);
        RECT tr = { 0, y - 8, left - 6, y + 8 };
        DrawTextW(mem, t, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    /* the note */
    RECT note = { left, midY - 7, right, midY + 7 };
    HBRUSH nb = CreateSolidBrush(RGB(0xB5, 0xDC, 0x56));
    FillRect(mem, &note, nb);
    DeleteObject(nb);
    FrameRect(mem, &note, (HBRUSH)GetStockObject(BLACK_BRUSH));

    /* the pitch */
    POINT pts[1024];
    int n = 0;
    double period = 1000.0 / (g_fry.hz > 0 ? g_fry.hz : 12);
    for (int x = 0; x <= w && n < 1024; x++) {
        double ms = PREVIEW_MS * x / w;
        pts[n].x = left + x;
        pts[n].y = FRY_Y(fry_pitch(&g_fry, ms / PREVIEW_MS, ms / period, 7));
        n++;
    }
    HPEN red = CreatePen(PS_SOLID, 2, RGB(0xE5, 0x22, 0x22));
    SelectObject(mem, red);
    Polyline(mem, pts, n);

    /* the voice controls, 0..127 in the strip underneath */
    int cTop = rc.bottom - ctrlH, cBot = rc.bottom - 4;
    RECT strip = { left, cTop, right, cBot };
    HBRUSH sb = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0));
    FillRect(mem, &strip, sb);
    DeleteObject(sb);
    RECT lt = { 0, cTop, left - 6, cBot };
    DrawTextW(mem, L"voice", -1, &lt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    for (int k = 0; k < FC_COUNT; k++) {
        if (!g_fry.use[k]) continue;
        HPEN cp = CreatePen(PS_SOLID, 2, kFcColor[k]);
        SelectObject(mem, cp);
        n = 0;
        for (int x = 0; x <= w && n < 1024; x += 2) {
            double v = kFcDefault[k] + (g_fry.val[k] - kFcDefault[k]) * fry_env(&g_fry, (double)x / w);
            pts[n].x = left + x;
            pts[n].y = cBot - (int)(v / 127 * (cBot - cTop));
            n++;
        }
        Polyline(mem, pts, n);
        SelectObject(mem, red);
        DeleteObject(cp);
    }
    wchar_t cap[64];
    wsprintfW(cap, L"preview: an %d ms note", (int)PREVIEW_MS);
    RECT ct = { left + 4, top, right, top + 16 };
    DrawTextW(mem, cap, -1, &ct, DT_LEFT | DT_SINGLELINE);
    #undef FRY_Y

    SelectObject(mem, op);
    SelectObject(mem, of);
    DeleteObject(grid); DeleteObject(axis); DeleteObject(red);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK fry_preview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        fry_preview_paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;          /* the paint covers it: no flicker */
    return DefWindowProcW(h, msg, wp, lp);
}

/* ----------------------------------------------------------- window -- */

enum { FRY_ID_MODE = 300, FRY_ID_APPLY, FRY_ID_REMOVE, FRY_ID_DEFAULTS, FRY_ID_SLIDER = 400, FRY_ID_CHECK = 500 };
enum { FS_DEPTH, FS_HZ, FS_JITTER, FS_FADEIN, FS_FADEOUT, FS_SHAPE_COUNT };   /* then the four voice controls */
#define FRY_SLIDERS (FS_SHAPE_COUNT + FC_COUNT)

static HWND g_fryWnd, g_fryMode, g_frySl[FRY_SLIDERS], g_fryVal[FRY_SLIDERS], g_fryCheck[FC_COUNT], g_fryHint;

static int *fry_field(int i)
{
    switch (i) {
    case FS_DEPTH: return &g_fry.depth10;
    case FS_HZ: return &g_fry.hz;
    case FS_JITTER: return &g_fry.jitter;
    case FS_FADEIN: return &g_fry.fadeIn;
    case FS_FADEOUT: return &g_fry.fadeOut;
    default: return &g_fry.val[i - FS_SHAPE_COUNT];
    }
}

static const wchar_t *fry_label(int i)
{
    static const wchar_t *const shape[FS_SHAPE_COUNT] = { L"Depth", L"Speed", L"Randomness", L"Fade in", L"Fade out" };
    return i < FS_SHAPE_COUNT ? shape[i] : kFcName[i - FS_SHAPE_COUNT];
}

static void fry_range(int i, int *lo, int *hi)
{
    static const int r[FS_SHAPE_COUNT][2] = { { 0, 240 }, { 3, 60 }, { 0, 100 }, { 0, 90 }, { 0, 90 } };
    if (i < FS_SHAPE_COUNT) { *lo = r[i][0]; *hi = r[i][1]; }
    else { *lo = 0; *hi = 127; }
}

static void fry_show_value(int i)
{
    wchar_t t[32];
    int v = *fry_field(i);
    switch (i) {
    case FS_DEPTH: wsprintfW(t, L"%d.%d st", v / 10, v % 10); break;
    case FS_HZ: wsprintfW(t, L"%d /s", v); break;
    case FS_JITTER: case FS_FADEIN: case FS_FADEOUT: wsprintfW(t, L"%d %%", v); break;
    default: wsprintfW(t, L"%d", v); break;
    }
    SetWindowTextW(g_fryVal[i], t);
}

static void fry_save(void)
{
    vh_ini_set_int(g_ini, L"VocalFry", L"Mode", g_fry.mode);
    vh_ini_set_int(g_ini, L"VocalFry", L"Depth10", g_fry.depth10);
    vh_ini_set_int(g_ini, L"VocalFry", L"Speed", g_fry.hz);
    vh_ini_set_int(g_ini, L"VocalFry", L"Randomness", g_fry.jitter);
    vh_ini_set_int(g_ini, L"VocalFry", L"FadeIn", g_fry.fadeIn);
    vh_ini_set_int(g_ini, L"VocalFry", L"FadeOut", g_fry.fadeOut);
    for (int k = 0; k < FC_COUNT; k++) {
        wchar_t key[16];
        vh_ini_set_int(g_ini, L"VocalFry", kFcKey[k], g_fry.val[k]);
        wsprintfW(key, L"Use%s", kFcKey[k]);
        vh_ini_set_int(g_ini, L"VocalFry", key, g_fry.use[k]);
    }
}

static void fry_load(void)
{
    FryParams d = FRY_DEFAULTS;
    g_fry.mode = vh_ini_int(g_ini, L"VocalFry", L"Mode", d.mode);
    if (g_fry.mode < 0 || g_fry.mode >= FRY_MODES) g_fry.mode = d.mode;
    g_fry.depth10 = vh_ini_int(g_ini, L"VocalFry", L"Depth10", d.depth10);
    g_fry.hz = vh_ini_int(g_ini, L"VocalFry", L"Speed", d.hz);
    g_fry.jitter = vh_ini_int(g_ini, L"VocalFry", L"Randomness", d.jitter);
    g_fry.fadeIn = vh_ini_int(g_ini, L"VocalFry", L"FadeIn", d.fadeIn);
    g_fry.fadeOut = vh_ini_int(g_ini, L"VocalFry", L"FadeOut", d.fadeOut);
    for (int k = 0; k < FC_COUNT; k++) {
        wchar_t key[16];
        g_fry.val[k] = vh_ini_int(g_ini, L"VocalFry", kFcKey[k], d.val[k]);
        wsprintfW(key, L"Use%s", kFcKey[k]);
        g_fry.use[k] = vh_ini_int(g_ini, L"VocalFry", key, d.use[k]) != 0;
    }
    for (int i = 0; i < FRY_SLIDERS; i++) {          /* out-of-range values from a hand-edited ini */
        int lo, hi, *f = fry_field(i);
        fry_range(i, &lo, &hi);
        if (*f < lo) *f = lo;
        if (*f > hi) *f = hi;
    }
}

static void fry_sync_controls(void)
{
    ComboBox_SetCurSel(g_fryMode, g_fry.mode);
    for (int i = 0; i < FRY_SLIDERS; i++) {
        SendMessageW(g_frySl[i], TBM_SETPOS, TRUE, *fry_field(i));
        fry_show_value(i);
    }
    for (int k = 0; k < FC_COUNT; k++) {
        Button_SetCheck(g_fryCheck[k], g_fry.use[k] ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(g_frySl[FS_SHAPE_COUNT + k], g_fry.use[k]);
    }
    SetWindowTextW(g_fryHint, g_fry.mode == FRY_GROWL ? L"Growl: a rough zig-zag around the note."
                   : g_fry.mode == FRY_DOWN ? L"Vocal Fry (down): from the note's pitch all the way down and back, again and again."
                                            : L"Vocal Fry (up): from the note's pitch all the way up and back, again and again.");
    if (g_fryPreview) InvalidateRect(g_fryPreview, NULL, FALSE);
}

static HWND fry_mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_fryWnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fryFont, TRUE);
    return c;
}

static LRESULT CALLBACK fry_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_fryWnd = h;
        NONCLIENTMETRICSW ncm = { sizeof ncm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        g_fryFont = CreateFontIndirectW(&ncm.lfMessageFont);
        fry_mk(L"STATIC", L"Mode:", SS_CENTERIMAGE, 14, 12, 50, 24, -1);
        g_fryMode = fry_mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 70, 12, 200, 200, FRY_ID_MODE);
        for (int m = 0; m < FRY_MODES; m++) ComboBox_AddString(g_fryMode, kFryModeName[m]);
        g_fryHint = fry_mk(L"STATIC", L"", SS_LEFT, 14, 42, 470, 20, -1);
        g_fryPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"VocahansmentsFryPreview", L"", WS_CHILD | WS_VISIBLE, 14, 66, 470, 210, h, NULL, GetModuleHandleW(NULL), NULL);
        int y = 288;
        for (int i = 0; i < FRY_SLIDERS; i++) {
            if (i == FS_SHAPE_COUNT) { fry_mk(L"STATIC", L"Voice (each one only if ticked):", SS_LEFT, 14, y + 4, 300, 20, -1); y += 26; }
            int lo, hi;
            fry_range(i, &lo, &hi);
            if (i >= FS_SHAPE_COUNT) g_fryCheck[i - FS_SHAPE_COUNT] = fry_mk(L"BUTTON", fry_label(i), BS_AUTOCHECKBOX | WS_TABSTOP, 14, y, 160, 24, FRY_ID_CHECK + i - FS_SHAPE_COUNT);
            else fry_mk(L"STATIC", fry_label(i), SS_CENTERIMAGE, 14, y, 160, 24, -1);
            g_frySl[i] = fry_mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 178, y, 230, 26, FRY_ID_SLIDER + i);
            SendMessageW(g_frySl[i], TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
            g_fryVal[i] = fry_mk(L"STATIC", L"", SS_CENTERIMAGE, 414, y, 70, 24, -1);
            y += 30;
        }
        y += 8;
        fry_mk(L"BUTTON", L"Apply to selected notes", BS_DEFPUSHBUTTON | WS_TABSTOP, 14, y, 170, 30, FRY_ID_APPLY);
        fry_mk(L"BUTTON", L"Remove from selected", BS_PUSHBUTTON | WS_TABSTOP, 192, y, 150, 30, FRY_ID_REMOVE);
        fry_mk(L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, 350, y, 80, 30, FRY_ID_DEFAULTS);
        fry_sync_controls();
        RECT rc = { 0, 0, 500, y + 44 };
        AdjustWindowRectEx(&rc, GetWindowLongW(h, GWL_STYLE), FALSE, GetWindowLongW(h, GWL_EXSTYLE));
        SetWindowPos(h, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    }
    case WM_HSCROLL:
        for (int i = 0; i < FRY_SLIDERS; i++)
            if ((HWND)lp == g_frySl[i]) {
                *fry_field(i) = (int)SendMessageW(g_frySl[i], TBM_GETPOS, 0, 0);
                fry_show_value(i);
                InvalidateRect(g_fryPreview, NULL, FALSE);
                fry_save();
            }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == FRY_ID_MODE && HIWORD(wp) == CBN_SELCHANGE) {
            int m = ComboBox_GetCurSel(g_fryMode);
            /* growl and fry want very different depth and speed: switching starts from that mode's defaults */
            if ((m == FRY_GROWL) != (g_fry.mode == FRY_GROWL)) { FryParams d = GROWL_DEFAULTS, f = FRY_DEFAULTS; g_fry = m == FRY_GROWL ? d : f; }
            g_fry.mode = m;
            fry_sync_controls();
            fry_save();
        }
        else if (id >= FRY_ID_CHECK && id < FRY_ID_CHECK + FC_COUNT) {
            g_fry.use[id - FRY_ID_CHECK] = Button_GetCheck(g_fryCheck[id - FRY_ID_CHECK]) == BST_CHECKED;
            fry_sync_controls();
            fry_save();
        }
        else if (id == FRY_ID_APPLY) fry_apply();
        else if (id == FRY_ID_REMOVE) fry_remove();
        else if (id == FRY_ID_DEFAULTS) {
            int m = g_fry.mode;
            FryParams d = GROWL_DEFAULTS, f = FRY_DEFAULTS;
            g_fry = m == FRY_GROWL ? d : f;
            g_fry.mode = m;
            fry_sync_controls();
            fry_save();
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_fryWnd = NULL;
        g_fryPreview = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void __cdecl menu_fry(void *c)
{
    (void)c;
    if (g_fryWnd) { ShowWindow(g_fryWnd, SW_RESTORE); SetForegroundWindow(g_fryWnd); return; }
    static BOOL reg;
    if (!reg) {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = fry_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"VocahansmentsVocalFry";
        RegisterClassW(&wc);
        WNDCLASSW pc = { 0 };
        pc.lpfnWndProc = fry_preview_proc;
        pc.hInstance = GetModuleHandleW(NULL);
        pc.hCursor = LoadCursor(NULL, IDC_ARROW);
        pc.lpszClassName = L"VocahansmentsFryPreview";
        RegisterClassW(&pc);
        reg = TRUE;
    }
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"VocahansmentsVocalFry", L"Vocal Fry & Growl - Vocahansments", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 520, 700, A->GetMainWindow(), NULL, GetModuleHandleW(NULL), NULL);
    ShowWindow(w, SW_SHOW);
}

static void fry_menus(void)
{
    fry_load();
    vh_menu_add(A, NULL, L"Vocal Fry && Growl...", menu_fry, NULL);
}
