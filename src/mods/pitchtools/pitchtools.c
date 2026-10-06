/*
 * Pitch Tools for the VOCALOID4 piano roll.
 *
 * Live pitch render
 *   VOCALOID only draws the rendered pitch curve when you press the render
 *   button, and throws it away on the next edit. This re-renders it shortly
 *   after every committed change (edits, undo, redo, switching parts), but
 *   never while a mouse button is held.
 *
 * Pitch pen  (button next to the tools, P, or Mods > Pitch Pen)
 *   drag          draw the pitch you want, in the piano roll's own semitone
 *                 space; it is written as PIT (and PBS when the bend needs
 *                 more range) over the notes under the stroke
 *   Shift+drag    straight line from where you started
 *   right-drag    erase: pitch bend back to 0 under the stroke
 *   Esc           back to the arrow tool
 *   One stroke = one Undo step.
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <math.h>
#include "../../common/vh_api.h"
static const VHApi *A;
#include "../../common/v4_editor.h"
#include "pitchmodel.c"

/* ----------------------------------------------------------- settings -- */

static wchar_t g_ini[MAX_PATH];

static int g_liveDelay = 350;          /* ms after the last change */
static int g_exactPasses = 2;          /* pitch pen correction passes (0 = raw PIT) */
static BOOL g_keepVibrato;             /* pen ignores the note's vibrato instead of cancelling it */
static UINT g_cmdLive, g_cmdPen, g_cmdExact, g_cmdVib, g_cmdPhon, g_cmdEngine;

static void load_settings(void)
{
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    lstrcatW(dir, L"\\Vocahansments");
    CreateDirectoryW(dir, NULL);
    wsprintfW(g_ini, L"%s\\settings.ini", dir);
    g_pmShow = vh_ini_int(g_ini, L"PitchTools", L"RealtimePitch", 1);
    g_liveDelay = vh_ini_int(g_ini, L"PitchTools", L"LiveRenderDelayMs", 350);
    if (g_liveDelay < 50) g_liveDelay = 50;
    g_exactPasses = vh_ini_int(g_ini, L"PitchTools", L"PenExact", 1);
    g_keepVibrato = vh_ini_int(g_ini, L"PitchTools", L"PenKeepVibrato", 0);
}

static void load_engine_settings(void);

/* ===================================================== live pitch render == */

#define LIVE_TIMER 0x50544C52   /* 'PTLR' */
static BOOL g_rendering, g_suppress;
static DWORD g_lastRenderMs;
static unsigned g_lastPart = 0xFFFFFFFF;
static void render_now(void);
static int engine_render(void);

/* Rendering blocks the editor for as long as it takes, so the longer the
 * part renders, the longer we wait for the user to pause before doing it. */
static DWORD idle_needed(void)
{
    DWORD d = (DWORD)g_liveDelay;
    if (g_lastRenderMs * 3 > d) d = g_lastRenderMs * 3;
    return d > 4000 ? 4000 : d;
}

static BOOL g_engineLive;

static VOID CALLBACK live_timer(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    (void)msg; (void)t;
    /* never while dragging, and for slow renders only once the user is idle */
    if (GetCapture() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
    LASTINPUTINFO li = { sizeof li, 0 };
    if (g_lastRenderMs > 150 && GetLastInputInfo(&li) && GetTickCount() - li.dwTime < idle_needed()) return;
    KillTimer(h, id);
    if (!g_engineLive) return;
    int ms = engine_render();          /* silent: no progress window */
    if (ms >= 0) g_lastRenderMs = (DWORD)ms;
}

/* The real-time model draws at once; the exact engine render follows once
 * the user pauses. */
static void schedule_render(void)
{
    pm_invalidate();
    HWND main = A->GetMainWindow();
    if (g_engineLive && main && !g_rendering) SetTimer(main, LIVE_TIMER, g_liveDelay, live_timer);
}

/* every edit anywhere in the editor ends in IVSVsqEditorIF::commit */
typedef BOOL (THISCALL *Commit_t)(void *ed);
static Commit_t o_Commit;
static BOOL THISCALL h_Commit(void *ed)
{
    BOOL r = o_Commit(ed);
    if (r) schedule_render();
    return r;
}

typedef int (THISCALL *UndoRedo_t)(void *mgr);
static UndoRedo_t o_editUndo, o_editRedo;
static int THISCALL h_editUndo(void *mgr) { int r = o_editUndo(mgr); schedule_render(); return r; }
static int THISCALL h_editRedo(void *mgr) { int r = o_editRedo(mgr); schedule_render(); return r; }

#define VSQ4_VT_EDITORIF_RVA 0x7DB74   /* CVSVsqEditorIF::vftable */
#define VSQ4_COMMIT_RVA      0x1EB80

static BOOL hook_commit(void)
{
    BYTE *vsq = (BYTE *)GetModuleHandleA("vsq4.dll");
    if (!vsq) return FALSE;
    void **slot = (void **)(vsq + VSQ4_VT_EDITORIF_RVA);
    if (IsBadReadPtr(slot, 4) || *slot != vsq + VSQ4_COMMIT_RVA) { A->Log("pitch: Vsq4 build mismatch, live render off"); return FALSE; }
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return FALSE;
    o_Commit = (Commit_t)*slot;
    *slot = (void *)h_Commit;
    VirtualProtect(slot, 4, old, &old);
    return TRUE;
}

/* ============================================================ pitch pen == */

#define PEN_BTN_ID  0xBF81
#define SAMPLE_STEP 5        /* ticks between PIT events */
#define MAX_POINTS  8192

static BOOL g_active;
static HWND g_toolDlg, g_penBtn;
static HCURSOR g_penCursor;

typedef struct { long tick; double pitch; } PenPoint;
static PenPoint g_pts[MAX_POINTS];
static POINT g_screen[MAX_POINTS];   /* client coords, for drawing the stroke */
static int g_npts;
static BOOL g_drawing, g_erasing;

static void set_active(BOOL on);

/* ------------------------------------------------------- the writing -- */

static int cmp_ctrl(const void *a, const void *b) { return ((const ControlRef *)a)->pos - ((const ControlRef *)b)->pos; }

/* The rendered pitch the editor draws lives on the piano-roll pane as
 * {abs tick, 5 floats}; f1 note, f2 +portamento, f3 +PIT, f4 final sung pitch
 * (with vibrato), in cents relative to MIDI 69 (A4; the piano roll labels it A3). -FLT_MAX = no voice. */
typedef struct { unsigned tick; float note, porta, pit, final, dyn; } RenderFrame;
#define PRP_RENDER_BEGIN 0x224
#define PRP_RENDER_END   0x228

static void render_now(void)
{
    HWND main = A->GetMainWindow();
    if (!main) return;
    DWORD t = GetTickCount();
    g_rendering = TRUE;
    SendMessageW(main, WM_COMMAND, V4_CMD_PITCH_RENDER, 0);
    g_rendering = FALSE;
    g_lastRenderMs = GetTickCount() - t;
}

#include "engine.c"

/* Once per score state, when its exact render is in: keep the rendered pitch
 * without PIT (see pm_base_at) and each vibrato note's real vibrato (see
 * pm_vib_real) for the real-time curve and the pen. */
static LONG g_capGen;

static void render_capture(void)
{
    if (!g_engineOk || g_capGen == g_pmGen || !engine_has_frames() || !pm_refresh()) return;
    g_capGen = g_pmGen;
    RenderFrame *b = *(RenderFrame **)(PANE() + PRP_RENDER_BEGIN), *e = *(RenderFrame **)(PANE() + PRP_RENDER_END);
    int nf = (int)(e - b);

    g_baseOk = FALSE;
    if (nf > g_baseCap) {
        PMBaseFrame *p = (PMBaseFrame *)(g_base ? HeapReAlloc(GetProcessHeap(), 0, g_base, nf * sizeof(PMBaseFrame)) : HeapAlloc(GetProcessHeap(), 0, nf * sizeof(PMBaseFrame)));
        if (p) { g_base = p; g_baseCap = nf; }
    }
    if (nf <= g_baseCap) {
        for (int f = 0; f < nf; f++) {
            g_base[f].tick = b[f].tick;
            g_base[f].base = b[f].porta;
            g_base[f].vib = b[f].final > -1e30f && b[f].pit > -1e30f ? b[f].final - b[f].pit : 0;
        }
        g_nbase = nf;
        g_baseSig = g_pmSig;
        g_baseOk = TRUE;
    }

    g_nvib = 0; g_nvibPts = 0;
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *n = &g_pmNotes[i];
        if (n->vibType <= 0 || n->vibLen <= 0) continue;
        if (g_nvib >= g_vibCap) {
            int nc = g_vibCap ? g_vibCap * 2 : 256;
            PMVib *p = (PMVib *)(g_vib ? HeapReAlloc(GetProcessHeap(), 0, g_vib, nc * sizeof(PMVib)) : HeapAlloc(GetProcessHeap(), 0, nc * sizeof(PMVib)));
            if (!p) break;
            g_vib = p; g_vibCap = nc;
        }
        PMVib *v = &g_vib[g_nvib];
        v->id = n->id; v->len = n->off - n->on; v->vibType = n->vibType; v->vibLen = n->vibLen;
        v->first = g_nvibPts; v->n = 0;
        int lo = 0, hi = nf;                     /* first frame at or after note on */
        while (lo < hi) { int m = (lo + hi) / 2; if ((long)b[m].tick < n->tOn) lo = m + 1; else hi = m; }
        for (int f = lo; f < nf && (long)b[f].tick < n->tOff; f++) {
            if (b[f].final < -1e30f || b[f].pit < -1e30f) continue;
            if (g_nvibPts >= g_vibPtsCap) {
                int nc = g_vibPtsCap ? g_vibPtsCap * 2 : 8192;
                PMVibPt *p = (PMVibPt *)(g_vibPts ? HeapReAlloc(GetProcessHeap(), 0, g_vibPts, nc * sizeof(PMVibPt)) : HeapAlloc(GetProcessHeap(), 0, nc * sizeof(PMVibPt)));
                if (!p) break;
                g_vibPts = p; g_vibPtsCap = nc;
            }
            g_vibPts[g_nvibPts].ms = (float)(pm_ms((long)b[f].tick) - n->on);
            g_vibPts[g_nvibPts].cents = b[f].final - b[f].pit;
            g_nvibPts++; v->n++;
        }
        if (v->n >= 2) g_nvib++;
        else g_nvibPts = v->first;
    }
    qsort(g_vib, g_nvib, sizeof(PMVib), cmp_pmvib);
    pm_resolve_vib(g_pmNotes, g_pmCount);
}

static void load_engine_settings(void)
{
    g_showPhonemes = vh_ini_int(g_ini, L"PitchTools", L"PhonemeTiming", 1);
    g_engineLive = vh_ini_int(g_ini, L"PitchTools", L"EngineRender", 1);
}

typedef struct {
    PartRef pr;
    ControlList pit, pbs;
    ControlRef *pitEv, *pbsEv;   /* original events, sorted */
    int npit, npbs;
    long t0;
    int nslots;
    const BYTE *set;             /* sample has a note under it */
} StrokeCtx;

/* Write dev[] (semitones from the note) for every run of covered samples, as
 * one commit. PIT/PBS are restored right after each run. */
static BOOL write_pitch(StrokeCtx *c, const double *dev, int *eventsOut)
{
    void *ed = pGetVsqEditor(c->pr.mgr, 2);
    if (!ed) return FALSE;
    UpsertControl_t upsert = VMETHOD(UpsertControl_t, ed, VSED_UPSERT_CONTROL);
    RemoveControl_t removeCtl = VMETHOD(RemoveControl_t, ed, VSED_REMOVE_CONTROL);
    BOOL ok = TRUE;
    int written = 0;
    for (int s = 0; s < c->nslots && ok;) {
        if (!c->set[s]) { s++; continue; }
        int e = s;
        while (e + 1 < c->nslots && c->set[e + 1]) e++;
        int rA = (int)(c->t0 + (long)s * SAMPLE_STEP - c->pr.partPos);
        int rB = (int)(c->t0 + (long)e * SAMPLE_STEP - c->pr.partPos);
        double maxDev = 0;
        for (int i = s; i <= e; i++) if (fabs(dev[i]) > maxDev) maxDev = fabs(dev[i]);
        int pbsBefore = control_value_at(c->pbsEv, c->npbs, rA - 1, 2);
        int pbsAfter = control_value_at(c->pbsEv, c->npbs, rB, 2);
        int pitAfter = control_value_at(c->pitEv, c->npit, rB, 0);
        int need = (int)ceil(maxDev - 1e-6);
        int range = pbsBefore > need ? pbsBefore : need;
        if (range < 1) range = 1;
        if (range > 24) range = 24;

        for (int i = 0; i < c->npit && ok; i++) if (c->pitEv[i].pos >= rA && c->pitEv[i].pos <= rB) ok = removeCtl(ed, c->pitEv[i].id);
        for (int i = 0; i < c->npbs && ok; i++) if (c->pbsEv[i].pos > rA && c->pbsEv[i].pos <= rB) ok = removeCtl(ed, c->pbsEv[i].id);
        if (ok && range != pbsBefore) { VSControlData d = { rA, V4_CTRL_PBS, range }; ok = upsert(ed, c->pbs.listId, &d); }
        int last = 0x7FFFFFFF;
        for (int i = s; i <= e && ok; i++) {
            int v = (int)lround(dev[i] / range * 8192.0);
            if (v > 8191) v = 8191;
            if (v < -8192) v = -8192;
            if (v == last) continue;
            VSControlData d = { (int)(c->t0 + (long)i * SAMPLE_STEP - c->pr.partPos), V4_CTRL_PIT, v };
            ok = upsert(ed, c->pit.listId, &d);
            last = v;
            written++;
        }
        int after = rB + SAMPLE_STEP;
        if (ok && range != pbsAfter) { VSControlData d = { after, V4_CTRL_PBS, pbsAfter }; ok = upsert(ed, c->pbs.listId, &d); }
        if (ok && last != pitAfter) { VSControlData d = { after, V4_CTRL_PIT, pitAfter }; ok = upsert(ed, c->pit.listId, &d); }
        s = e + 1;
    }
    if (ok) ok = VCALL0(BOOL, ed, VSED_COMMIT);
    if (!ok) {
        A->Log("pitch: write failed, error %08x", VCALL0(unsigned, ed, VSED_LASTERROR));
        VCALL0(void, ed, VSED_ROLLBACK);
    }
    pReleaseVsqEditor(c->pr.mgr, &ed);
    if (eventsOut) *eventsOut = written;
    return ok;
}

static BOOL apply_stroke(void)
{
    if (g_npts < 1) return FALSE;
    DWORD tStart = GetTickCount();
    StrokeCtx c;
    memset(&c, 0, sizeof c);
    if (!open_part(&c.pr)) return FALSE;

    /* rasterise the stroke onto a tick grid; later strokes over the same tick win */
    long t0 = g_pts[0].tick, t1 = g_pts[0].tick;
    for (int i = 1; i < g_npts; i++) { if (g_pts[i].tick < t0) t0 = g_pts[i].tick; if (g_pts[i].tick > t1) t1 = g_pts[i].tick; }
    t0 = (t0 / SAMPLE_STEP) * SAMPLE_STEP;
    if (t0 < c.pr.partPos) t0 = c.pr.partPos;      /* strokes may start left of the part */
    if (t1 < t0) return FALSE;
    if (t1 - t0 > (long)SAMPLE_STEP * 4000000) return FALSE;
    int nslots = (int)((t1 - t0) / SAMPLE_STEP) + 1;
    BOOL ok = FALSE;
    double *target = (double *)HeapAlloc(GetProcessHeap(), 0, nslots * sizeof(double));
    double *dev = (double *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nslots * sizeof(double));
    int *base = (int *)HeapAlloc(GetProcessHeap(), 0, nslots * sizeof(int));
    BYTE *set = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nslots);
    if (!target || !dev || !base || !set) goto done;
    for (int i = 0; i < g_npts; i++) {
        PenPoint a = g_pts[i], b = g_pts[i + 1 < g_npts ? i + 1 : i];
        long lo = a.tick < b.tick ? a.tick : b.tick, hi = a.tick < b.tick ? b.tick : a.tick;
        /* a segment left of t0 would give a negative slot: start at 0 */
        int s0 = lo > t0 ? (int)((lo - t0 + SAMPLE_STEP - 1) / SAMPLE_STEP) : 0;
        for (int s = s0; s < nslots; s++) {
            long t = t0 + (long)s * SAMPLE_STEP;
            if (t > hi) break;
            if (t < lo) continue;
            double f = hi == lo ? 0 : (double)(t - a.tick) / (double)(b.tick - a.tick);
            target[s] = a.pitch + (b.pitch - a.pitch) * f;
            set[s] = 1;
        }
    }
    if (g_npts == 1 && g_pts[0].tick >= t0) { target[0] = g_pts[0].pitch; set[0] = 1; }

    /* Only where something is sung: on a note, or where the voice is already
     * sounding before it (the consonant and the transition into the note).
     * Silent gaps between phrases are left alone.
     * Exact mode: PIT adds straight onto what VOCALOID sings without it, so
     * PIT = stroke - that. It comes from the last exact render, which stays
     * valid through pitch edits; if the notes changed since, the part is
     * rendered once first (only with the engine render after edits on, else
     * the model is used). No trial passes: one write, no feedback loop. */
    static NoteRef notes[8192];
    int nn = part_notes(&c.pr, notes, 8192), covered = 0;
    BOOL withVib = !g_keepVibrato, model = pm_refresh();
    if (model && g_exactPasses && !g_erasing) {
        render_capture();
        if (!pm_base_valid() && g_engineLive && engine_render() >= 0) render_capture();
    }
    for (int s = 0; s < nslots; s++) {
        if (!set[s]) continue;
        long t = t0 + (long)s * SAMPLE_STEP;
        int rel = (int)(t - c.pr.partPos);
        base[s] = -1;
        for (int i = 0; i < nn; i++) if (rel >= notes[i].pos && rel < notes[i].pos + notes[i].dur) { base[s] = notes[i].num; break; }
        double sung;
        BOOL have = model && (pm_base_at(t, withVib, &sung) || pm_model_pitch_at(t, withVib, FALSE, &sung));
        if (base[s] < 0) {
            if (!have) { set[s] = 0; continue; }
            base[s] = (int)lround(sung);
        }
        if (g_erasing) dev[s] = 0.0;
        else if (g_exactPasses && have) dev[s] = target[s] - sung;
        else dev[s] = target[s] - base[s];
        if (dev[s] > 24) dev[s] = 24;
        if (dev[s] < -24) dev[s] = -24;
        covered++;
    }
    if (!covered) { A->Log("pitch: stroke is not over any sung pitch"); goto done; }

    if (!open_controls(&c.pr, V4_CTRL_PIT, &c.pit) || !open_controls(&c.pr, V4_CTRL_PBS, &c.pbs)) { A->Log("pitch: PIT/PBS lists not found"); goto done; }
    static ControlRef pitEv[65536], pbsEv[4096];
    c.pitEv = pitEv; c.pbsEv = pbsEv;
    c.npit = control_events(&c.pit, pitEv, 65536);
    c.npbs = control_events(&c.pbs, pbsEv, 4096);
    qsort(pitEv, c.npit, sizeof pitEv[0], cmp_ctrl);
    qsort(pbsEv, c.npbs, sizeof pbsEv[0], cmp_ctrl);
    c.t0 = t0; c.nslots = nslots; c.set = set;

    int written = 0;
    BOOL estimated = g_exactPasses && !g_erasing && !pm_base_valid();
    ok = write_pitch(&c, dev, &written);
    A->Log("pitch: %s %ld..%ld, %d PIT events%s, %lu ms", g_erasing ? "erased" : "drew", t0, t1, written,
           estimated ? " (estimated)" : "", GetTickCount() - tStart);
done:
    if (target) HeapFree(GetProcessHeap(), 0, target);
    if (dev) HeapFree(GetProcessHeap(), 0, dev);
    if (base) HeapFree(GetProcessHeap(), 0, base);
    if (set) HeapFree(GetProcessHeap(), 0, set);
    return ok;
}

/* ---------------------------------------------------------- the stroke -- */

static void add_point(int x, int y)
{
    RECT na = note_area();
    if (x < na.left) x = na.left;
    if (x > na.right - 1) x = na.right - 1;
    if (y < na.top) y = na.top;
    if (y > na.bottom - 1) y = na.bottom - 1;
    if (g_npts && g_screen[g_npts - 1].x == x && g_screen[g_npts - 1].y == y) return;
    BOOL straight = (GetKeyState(VK_SHIFT) & 0x8000) && g_npts >= 1;
    if (straight) g_npts = 1;                       /* ruler: start point + current */
    if (g_npts >= MAX_POINTS) return;
    g_screen[g_npts].x = x; g_screen[g_npts].y = y;
    g_pts[g_npts].tick = tick_at_x(x);
    g_pts[g_npts].pitch = pitch_at_y(y);
    g_npts++;
}

/* a full repaint: the editor itself only repaints what it thinks changed,
 * which would leave our overlays (a stroke that wrote nothing) behind */
static void repaint_view(HWND h) { RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN); }

static void draw_stroke(HDC dc)
{
    if (g_npts < 1) return;
    RECT na = note_area();
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    HPEN pen = CreatePen(PS_SOLID, 2, themed(g_erasing ? RGB(0xFF, 0x60, 0x60) : TOOL_ACTIVE));
    HGDIOBJ old = SelectObject(dc, pen);
    if (g_npts == 1) { MoveToEx(dc, g_screen[0].x - 1, g_screen[0].y, NULL); LineTo(dc, g_screen[0].x + 1, g_screen[0].y); }
    else Polyline(dc, g_screen, g_npts);
    SelectObject(dc, old);
    DeleteObject(pen);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static void begin_stroke(HWND h, int x, int y, BOOL erase)
{
    g_npts = 0;
    g_drawing = TRUE;
    g_erasing = erase;
    SetCapture(h);
    SetFocus(h);
    add_point(x, y);
    HDC dc = GetDC(h); draw_stroke(dc); ReleaseDC(h, dc);
}

static void end_stroke(HWND h)
{
    if (!g_drawing) return;
    g_drawing = FALSE;
    ReleaseCapture();
    apply_stroke();
    g_npts = 0;
    repaint_view(h);
}

/* ------------------------------------------------ flicker-free overlays -- */
/* The curve, phoneme lane and stroke used to be drawn onto the screen after
 * the editor's paint, so every repaint showed the frame without them for a
 * moment (with rainbow notes animating, 16 times a second). Now the view's
 * paint goes into a buffer: the editor paints it, the overlays are drawn on
 * top, and only the finished frame is copied to the screen. */

static HDC g_paintMem, g_paintReal;
static HBITMAP g_paintBmp;
static HGDIOBJ g_paintOld;
static int g_paintW, g_paintH;
static BOOL g_overlayDone;
static BOOL g_paintHooked;          /* both BeginPaint and EndPaint */

#include "guides.c"
#include "notetools.c"
#include "fry.c"

static void draw_overlays(HDC dc)
{
    scale_draw(dc);                  /* under the curve */
    render_capture();
    /* the editor draws the exact engine curve itself once it is rendered */
    if (g_dragMode || !engine_has_frames()) pm_draw(dc);
    else g_xorValid = FALSE;
    engine_draw_phonemes(dc);
    if (g_drawing) draw_stroke(dc);
    hover_draw(dc);
    toast_draw(dc);
}

typedef HDC (WINAPI *BeginPaint_t)(HWND, PAINTSTRUCT *);
typedef BOOL (WINAPI *EndPaint_t)(HWND, const PAINTSTRUCT *);
static BeginPaint_t o_BeginPaint;
static EndPaint_t o_EndPaint;

static HDC WINAPI h_BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    if (!g_paintHooked || !g_viewHwnd || h != g_viewHwnd || g_paintMem) return o_BeginPaint(h, ps);
    RECT cl;
    GetClientRect(h, &cl);
    HDC wdc = GetDC(h);
    if (!wdc || cl.right <= 0 || cl.bottom <= 0) { if (wdc) ReleaseDC(h, wdc); return o_BeginPaint(h, ps); }
    if (!g_paintBmp || cl.right != g_paintW || cl.bottom != g_paintH) {
        if (g_paintBmp) DeleteObject(g_paintBmp);
        g_paintBmp = CreateCompatibleBitmap(wdc, cl.right, cl.bottom);
        g_paintW = cl.right; g_paintH = cl.bottom;
    }
    HDC mem = g_paintBmp ? CreateCompatibleDC(wdc) : NULL;
    if (!mem) { ReleaseDC(h, wdc); return o_BeginPaint(h, ps); }
    g_paintOld = SelectObject(mem, g_paintBmp);
    BitBlt(mem, 0, 0, cl.right, cl.bottom, wdc, 0, 0, SRCCOPY);   /* start from what is on screen */
    ReleaseDC(h, wdc);
    g_paintMem = mem;
    HDC real = o_BeginPaint(h, ps);                  /* its erase lands in the buffer (WM_ERASEBKGND) */
    if (!real) { SelectObject(mem, g_paintOld); DeleteDC(mem); g_paintMem = NULL; return real; }
    g_paintReal = real;
    ps->hdc = mem;
    return mem;
}

static BOOL WINAPI h_EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    if (!g_paintMem || !ps || ps->hdc != g_paintMem) return o_EndPaint(h, ps);
    draw_overlays(g_paintMem);
    g_overlayDone = TRUE;
    RECT r = ps->rcPaint;
    BitBlt(g_paintReal, r.left, r.top, r.right - r.left, r.bottom - r.top, g_paintMem, r.left, r.top, SRCCOPY);
    SelectObject(g_paintMem, g_paintOld);
    DeleteDC(g_paintMem);
    g_paintMem = NULL;
    PAINTSTRUCT p = *ps;
    p.hdc = g_paintReal;
    return o_EndPaint(h, &p);
}

static LRESULT CALLBACK view_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (g_active && g_view && in_note_area(x, y)) { begin_stroke(h, x, y, msg == WM_RBUTTONDOWN); return 0; }
        if (msg == WM_LBUTTONDOWN && g_view && (g_pmShow || g_showPhonemes) && in_note_area(x, y)) {
            /* The editor's drag is a modal loop inside this call: track it from
             * here. The mouse-up inside that loop already ends the drag, so the
             * curve must be un-hidden whether or not g_dragMode is still set. */
            pm_drag_begin(x, y);
            BOOL hid = g_dragMode != DRAG_NONE;
            if (hid) { g_pmHide = TRUE; RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW); }
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            pm_drag_end();
            if (hid) { g_pmHide = FALSE; RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN); }
            return r;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (g_active && g_view && in_note_area(x, y)) return 0;
        break;
    case WM_MOUSELEAVE:
        hover_leave(h);
        break;
    case WM_MOUSEMOVE:
        hover_move(h, x, y, wp);
        if (!g_drawing && g_dragMode && (wp & MK_LBUTTON)) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (pm_drag_update(x, y)) { RECT na = note_area(); InvalidateRect(h, &na, FALSE); }
            return r;
        }
        if (g_drawing) {
            BOOL straight = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            add_point(x, y);
            if (straight) repaint_view(h);   /* the ruler line moves */
            else if (g_npts >= 2) { HDC dc = GetDC(h); HPEN pen = CreatePen(PS_SOLID, 2, themed(g_erasing ? RGB(0xFF, 0x60, 0x60) : TOOL_ACTIVE));
                HGDIOBJ o = SelectObject(dc, pen); Polyline(dc, &g_screen[g_npts - 2], 2); SelectObject(dc, o); DeleteObject(pen); ReleaseDC(h, dc); }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_drawing) { end_stroke(h); return 0; }
        if (g_dragMode) pm_drag_end();
        g_pmHide = FALSE;
        break;
    case WM_CONTEXTMENU:
        if (g_active) return 0;
        break;
    case WM_CAPTURECHANGED:
        /* the editor takes the capture for its own drag loop: only a lost
         * capture with the button already up ends the drag */
        if (g_dragMode && (HWND)lp != h && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { pm_drag_end(); g_pmHide = FALSE; repaint_view(h); }
        if (g_drawing && (HWND)lp != h) { g_drawing = FALSE; g_npts = 0; repaint_view(h); }
        break;
    case WM_SETCURSOR:
        if (g_active && LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (g_view && in_note_area(pt.x, pt.y)) { SetCursor(g_penCursor); return TRUE; }
        }
        break;
    case WM_KEYDOWN:
        if (wp == 'P' && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) { set_active(!g_active); return 0; }
        if (wp == VK_ESCAPE && g_drawing) { g_drawing = FALSE; ReleaseCapture(); g_npts = 0; repaint_view(h); return 0; }
        if (wp == VK_ESCAPE && g_active) { set_active(FALSE); return 0; }
        break;
    case WM_ERASEBKGND:
        /* inside our buffered paint the erase goes to the buffer too */
        if (g_paintMem) return DefSubclassProc(h, msg, (WPARAM)g_paintMem, lp);
        break;
    case WM_PAINT: {
        g_overlayDone = FALSE;
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (!g_overlayDone) {            /* the paint didn't go through BeginPaint/EndPaint */
            HDC dc = GetDC(h);
            draw_overlays(dc);
            ReleaseDC(h, dc);
        }
        g_overlayDone = FALSE;
        return r;
    }
    case WM_NCDESTROY:
        g_view = NULL; g_viewHwnd = NULL;
        RemoveWindowSubclass(h, view_subclass, 2);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

typedef void (THISCALL *MouseHandler_t)(void *self, UINT flags, int x, int y);
static MouseHandler_t o_OnMouseMove;
static void THISCALL h_OnMouseMove(void *self, UINT flags, int x, int y)
{
    v4_attach_view((BYTE *)self, view_subclass, 2);
    /* opening another part shows no curve until rendered */
    unsigned part = *(unsigned *)(PANE() + V4_PRP_PARTID);
    if (part != g_lastPart) { g_lastPart = part; schedule_render(); }
    o_OnMouseMove(self, flags, x, y);
}

/* ------------------------------------------------------------- cursor -- */

static HCURSOR make_pen_cursor(void)
{
    BYTE andm[128], xorm[128];
    memset(andm, 0xFF, sizeof andm);
    memset(xorm, 0x00, sizeof xorm);
    #define PIX(x, y, black) do { if ((x) >= 0 && (x) < 32 && (y) >= 0 && (y) < 32) { int _i = (y) * 4 + (x) / 8; BYTE _b = 0x80 >> ((x) % 8); \
        andm[_i] &= ~_b; if (!(black)) xorm[_i] |= _b; else xorm[_i] &= ~_b; } } while (0)
    for (int pass = 0; pass < 2; pass++) {
        int black = pass == 0;
        for (int i = 2; i < 18; i++)                 /* pen body, tip at (1,30) */
            for (int w = -1 - black; w <= 1 + black; w++) PIX(1 + i + w, 30 - i, black);
    }
    PIX(1, 30, 1); PIX(2, 29, 1);                    /* nib */
    #undef PIX
    BYTE both[256];
    memcpy(both, andm, 128); memcpy(both + 128, xorm, 128);
    ICONINFO ii = { FALSE, 1, 30, CreateBitmap(32, 64, 1, 1, both), NULL };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    return c ? c : LoadCursor(NULL, IDC_CROSS);
}

/* ------------------------------------------------------------ toolbar -- */

static void draw_pen_glyph(HDC dc, RECT rc, COLORREF col)
{
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    /* a little pitch wave... */
    POINT wave[7] = { { cx - 9, cy + 4 }, { cx - 6, cy + 1 }, { cx - 3, cy + 4 }, { cx, cy + 1 }, { cx + 3, cy + 4 }, { cx + 6, cy + 1 }, { cx + 9, cy + 4 } };
    Polyline(dc, wave, 7);
    /* ...drawn by a pen */
    POINT nib[4] = { { cx + 1, cy - 1 }, { cx + 7, cy - 7 }, { cx + 9, cy - 5 }, { cx + 3, cy + 1 } };
    Polygon(dc, nib, 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

static LRESULT CALLBACK tooldlg_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_DRAWITEM && wp == PEN_BTN_ID) {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        v4_tool_button_bg(di);
        draw_pen_glyph(di->hDC, di->rcItem, themed(g_active ? TOOL_ACTIVE : TOOL_IDLE));
        return TRUE;
    }
    if (msg == WM_DRAWITEM && wp == V4_TOOL_ARROW && g_active) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        grey_out(di->hDC, di->rcItem);
        return r;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == PEN_BTN_ID) { set_active(!g_active); return 0; }
        if (g_active) set_active(FALSE);
    }
    if (msg == V4_WM_MODTOOL && wp != PEN_BTN_ID && g_active) set_active(FALSE);
    return DefSubclassProc(h, msg, wp, lp);
}

/* ---------------------------------------------------------------- UI -- */

static void set_active(BOOL on)
{
    if (on) {
        HWND main = A->GetMainWindow();
        if (main) SendMessageW(main, WM_COMMAND, V4_TOOL_ARROW, 0);
        if (g_toolDlg) SendMessageW(g_toolDlg, V4_WM_MODTOOL, PEN_BTN_ID, 0);
    }
    g_active = on;
    A->CheckMenuItem(g_cmdPen, on);
    if (g_penBtn) InvalidateRect(g_penBtn, NULL, FALSE);
    if (g_toolDlg) { HWND arrow = GetDlgItem(g_toolDlg, V4_TOOL_ARROW); if (arrow) InvalidateRect(arrow, NULL, FALSE); }
    if (g_viewHwnd) { POINT pt; GetCursorPos(&pt); SetCursorPos(pt.x, pt.y); }
}

static void __cdecl menu_pen(void *c) { (void)c; set_active(!g_active); }
static void __cdecl menu_live(void *c)
{
    (void)c;
    g_pmShow = !g_pmShow;
    A->CheckMenuItem(g_cmdLive, g_pmShow);
    vh_ini_set_int(g_ini, L"PitchTools", L"RealtimePitch", g_pmShow);
    if (g_viewHwnd) InvalidateRect(g_viewHwnd, NULL, FALSE);
}

static void __cdecl menu_phon(void *c)
{
    (void)c;
    g_showPhonemes = !g_showPhonemes;
    A->CheckMenuItem(g_cmdPhon, g_showPhonemes);
    vh_ini_set_int(g_ini, L"PitchTools", L"PhonemeTiming", g_showPhonemes);
    if (g_showPhonemes) schedule_render();
    if (g_viewHwnd) InvalidateRect(g_viewHwnd, NULL, FALSE);
}

static void __cdecl menu_engine(void *c)
{
    (void)c;
    g_engineLive = !g_engineLive;
    A->CheckMenuItem(g_cmdEngine, g_engineLive);
    vh_ini_set_int(g_ini, L"PitchTools", L"EngineRender", g_engineLive);
    if (g_engineLive) schedule_render();
}

static void __cdecl menu_exact(void *c)
{
    (void)c;
    g_exactPasses = !g_exactPasses;
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenExact", g_exactPasses);
}

static void __cdecl menu_vib(void *c)
{
    (void)c;
    g_keepVibrato = !g_keepVibrato;
    A->CheckMenuItem(g_cmdVib, g_keepVibrato);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenKeepVibrato", g_keepVibrato);
}

#include "lab.c"

static void __cdecl on_ui_ready(HWND mainWnd, void *ctx)
{
    (void)ctx;
    g_cmdPen = vh_menu_add(A, NULL, L"Pitch Pen\tP", menu_pen, NULL);
    g_cmdLive = vh_menu_add(A, NULL, L"Real-time Pitch Curve", menu_live, NULL);
    g_cmdPhon = vh_menu_add(A, NULL, L"Phoneme Timing", menu_phon, NULL);
    g_cmdEngine = vh_menu_add(A, L"Pitch Curve", L"Exact engine render after edits (no progress window)", menu_engine, NULL);
    g_cmdExact = vh_menu_add(A, L"Pitch Pen", L"Exact (compensate portamento/vibrato)", menu_exact, NULL);
    g_cmdVib = vh_menu_add(A, L"Pitch Pen", L"Keep note vibrato on top", menu_vib, NULL);
    guides_menus();
    notetools_menus();
    fry_menus();
    if (vh_ini_int(g_ini, L"PitchTools", L"DevTools", 0)) vh_menu_add(A, L"Pitch Pen", L"[dev] Build pitch-model dataset", menu_lab, NULL);
    A->CheckMenuItem(g_cmdLive, g_pmShow);
    A->CheckMenuItem(g_cmdPhon, g_showPhonemes);
    A->CheckMenuItem(g_cmdEngine, g_engineLive);
    engine_init();
    engine_hook_dse();
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    A->CheckMenuItem(g_cmdVib, g_keepVibrato);
    g_penBtn = v4_add_tool_button(mainWnd, PEN_BTN_ID, L"tool_pitchpen", tooldlg_subclass, &g_toolDlg);
}

/* lets other mods find the piano roll's note area (UI Colors' gravity floor and walls) */
__declspec(dllexport) BOOL __cdecl PitchTools_NoteArea(HWND view, RECT *out)
{
    if (!g_view || !g_viewHwnd || view != g_viewHwnd || !out) return FALSE;
    *out = note_area();
    return TRUE;
}

__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api)
{
    A = api;
    if (api->version < VH_API_MIN_VERSION) return 0;
    load_settings();
    load_engine_settings();
    if (!v4_resolve_imports()) { A->Log("pitch: editor DLL exports missing"); return 0; }
    if (!api->HookCode((void *)V4_MusicalPartWnd_OnMouseMove, V4_SIG_OnMouseMove, 6, h_OnMouseMove, (void **)&o_OnMouseMove))
        return 0;
    hook_commit();
    api->HookCode((void *)V4_TrackPoint, V4_SIG_TrackPoint, 6, h_TrackPoint, (void **)&o_TrackPoint);
    api->HookImport("vsq4.dll", "?editUndo@CVSVsqManagerIF@@QAEHXZ", (void *)h_editUndo, (void **)&o_editUndo);
    api->HookImport("vsq4.dll", "?editRedo@CVSVsqManagerIF@@QAEHXZ", (void *)h_editRedo, (void **)&o_editRedo);
    if (!api->HookImport("user32.dll", "BeginPaint", (void *)h_BeginPaint, (void **)&o_BeginPaint) ||
        !api->HookImport("user32.dll", "EndPaint", (void *)h_EndPaint, (void **)&o_EndPaint))
        A->Log("pitch: paint hooks failed, overlays drawn unbuffered");
    else g_paintHooked = TRUE;
    g_penCursor = make_pen_cursor();
    api->OnUiReady(on_ui_ready, NULL);
    return 1;
}
