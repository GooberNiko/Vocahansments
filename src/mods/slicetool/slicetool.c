/*
 * Slice Tool - a native knife tool for the VOCALOID4 piano roll.
 *
 *   K            toggle the tool (or the knife button next to the eraser,
 *                or Mods > Slice Tool)
 *   click        cut the note under the cursor at that point, snapped to
 *                the QUANTIZE grid
 *   Shift+click  cut without snapping
 *   Ctrl+click   cut every note that crosses that point (all pitches)
 *   Esc          back to the arrow tool
 *
 * No hover preview is drawn: painting over the editor's view outside its own
 * paint cycle left marks behind that only cleared when the note was redrawn.
 *
 * The cut goes through Vsq4's own editor interface as one commit, so a
 * single Undo restores the original note.
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include "../../common/vh_api.h"
static const VHApi *A;
#include "../../common/v4_editor.h"

/* ----------------------------------------------------------- settings -- */

static wchar_t g_ini[MAX_PATH];
static BOOL g_active;
static BOOL g_snap = TRUE;
static BOOL g_sustain = TRUE;   /* new slice: "-" sustaining the vowel; else repeat lyric */
static UINT g_cmdTool, g_cmdSnap, g_cmdSustain;

static void load_settings(void)
{
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    lstrcatW(dir, L"\\Vocahansments");
    CreateDirectoryW(dir, NULL);
    wsprintfW(g_ini, L"%s\\settings.ini", dir);
    g_snap = vh_ini_int(g_ini, L"SliceTool", L"Snap", 1);
    g_sustain = vh_ini_int(g_ini, L"SliceTool", L"Sustain", 1);
}

/* ---------------------------------------------------------- the cut -- */

#define MIN_PIECE 5   /* ticks */

static void read_note(void *note, VSNoteData *d)
{
    memset(d, 0, sizeof *d);
    d->pos = VCALL0(int, note, VSNOTE_POS);
    d->dur = VCALL0(int, note, VSNOTE_DUR);
    d->noteNum = VCALL0(int, note, VSNOTE_NOTENUM);
    d->velocity = VCALL0(int, note, VSNOTE_VELOCITY);
    d->phLock = VCALL0(int, note, VSNOTE_PHLOCK);
    const wchar_t *ly = VCALL0(const wchar_t *, note, VSNOTE_LYRIC);
    mswstr_set(&d->lyric, ly ? ly : L"a");
    unsigned n = VCALL0(unsigned, note, VSNOTE_PHCOUNT);
    if (n > 32) n = 32;
    PhonemeAt_t ph = VMETHOD(PhonemeAt_t, note, VSNOTE_PHONEME);
    if (n) {
        d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(MsWString));
        for (unsigned i = 0; i < n; i++) {
            const wchar_t *p = ph(note, i);
            mswstr_set(&d->phonemes.first[i], p ? p : L"");
        }
        d->phonemes.last = d->phonemes.end = d->phonemes.first + n;
    }
}

static void free_note(VSNoteData *d)
{
    mswstr_free(&d->lyric);
    for (MsWString *s = d->phonemes.first; s && s < d->phonemes.last; s++) mswstr_free(s);
    if (d->phonemes.first) HeapFree(GetProcessHeap(), 0, d->phonemes.first);
    memset(d, 0, sizeof *d);
}

/* Second half of a cut: same note, either "-" holding the last phoneme
 * (one sustained syllable) or a straight repeat of the syllable. */
static void make_tail(const VSNoteData *src, VSNoteData *d, int pos, int dur)
{
    memset(d, 0, sizeof *d);
    d->pos = pos; d->dur = dur;
    d->noteNum = src->noteNum; d->velocity = src->velocity; d->phLock = src->phLock;
    unsigned n = (unsigned)(src->phonemes.last - src->phonemes.first);
    if (g_sustain && n) {
        mswstr_set(&d->lyric, L"-");
        d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MsWString));
        mswstr_set(&d->phonemes.first[0], mswstr_c(&src->phonemes.first[n - 1]));
        d->phonemes.last = d->phonemes.end = d->phonemes.first + 1;
    } else {
        mswstr_set(&d->lyric, mswstr_c(&src->lyric));
        if (n) {
            d->phonemes.first = (MsWString *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * sizeof(MsWString));
            for (unsigned i = 0; i < n; i++) mswstr_set(&d->phonemes.first[i], mswstr_c(&src->phonemes.first[i]));
            d->phonemes.last = d->phonemes.end = d->phonemes.first + n;
        }
    }
}

static void read_expression(void *note, int expr[8], int vib[4])
{
    static const wchar_t *names[7] = { L"bendDep", L"bendLen", L"risePort", L"fallPort", L"decay", L"accent", L"opening" };
    static const int defaults[7] = { 8, 0, 50, 50, 50, 50, 127 };
    void *attr = VCALL0(void *, note, VSNOTE_STYLEATTR);
    AttrGet_t get = attr ? VMETHOD(AttrGet_t, attr, VSATTR_GET) : NULL;
    for (int i = 0; i < 7; i++) {
        expr[i] = defaults[i];
        if (get) get(attr, names[i], &expr[i]);
    }
    expr[7] = 0;
    int vtype = 0, vlen = 0;
    if (get) { get(attr, L"vibType", &vtype); get(attr, L"vibLen", &vlen); }
    if (vlen < 0 || vlen > 100) vlen = 0;
    if (vtype < 0 || vtype > 16) vtype = 0;
    vib[0] = (vlen > 0 && vtype > 0); vib[1] = vlen; vib[2] = vtype; vib[3] = 0;
}

/* Cut every note in refs at absolute tick t, as one undoable commit. */
static int cut_notes(PartRef *pr, NoteRef *refs, int nrefs, long t)
{
    void *ed = pGetVsqEditor(pr->mgr, 2);
    if (!ed) { A->Log("slice: no editor (another edit in progress?)"); return 0; }
    UpdateNote_t update = VMETHOD(UpdateNote_t, ed, VSED_UPDATE_NOTE);
    InsertNote_t insert = VMETHOD(InsertNote_t, ed, VSED_INSERT_NOTE);
    int done = 0;
    BOOL ok = TRUE;
    for (int i = 0; i < nrefs && ok; i++) {
        NoteRef *r = &refs[i];
        int cut = (int)(t - pr->partPos);
        if (cut - r->pos < MIN_PIECE || r->pos + r->dur - cut < MIN_PIECE) continue;
        VSNoteData head, tail;
        int expr[8], vib[4];
        read_note(r->note, &head);
        read_expression(r->note, expr, vib);
        make_tail(&head, &tail, cut, r->pos + r->dur - cut);
        head.dur = cut - r->pos;
        ok = update(ed, r->id, &head) && insert(ed, pr->listId, &tail, expr, vib);
        if (ok) done++;
        else A->Log("slice: edit failed on note %u, error %08x", r->id, VCALL0(unsigned, ed, VSED_LASTERROR));
        free_note(&head);
        free_note(&tail);
    }
    if (ok && done) {
        if (!VCALL0(BOOL, ed, VSED_COMMIT)) { A->Log("slice: commit failed %08x", VCALL0(unsigned, ed, VSED_LASTERROR)); done = 0; }
    } else {
        VCALL0(void, ed, VSED_ROLLBACK);
        done = 0;
    }
    pReleaseVsqEditor(pr->mgr, &ed);
    return done;
}

/* ------------------------------------------------------------- cursor -- */

static HCURSOR g_knife;

static HCURSOR make_knife_cursor(void)
{
    /* 32x32 monochrome: AND mask + XOR mask. Blade from the hotspot (bottom-left
     * tip) up to the right, with a handle. */
    BYTE andm[128], xorm[128];
    memset(andm, 0xFF, sizeof andm);
    memset(xorm, 0x00, sizeof xorm);
    #define PIX(x, y, black) do { int _i = (y) * 4 + (x) / 8; BYTE _b = 0x80 >> ((x) % 8); \
        andm[_i] &= ~_b; if (!(black)) xorm[_i] |= _b; else xorm[_i] &= ~_b; } while (0)
    /* outline (black) then fill (white) */
    for (int pass = 0; pass < 2; pass++) {
        int black = (pass == 0);
        for (int i = 0; i < 14; i++) {           /* blade: diagonal band */
            int x = 2 + i, y = 29 - i;
            for (int w = -1 - black; w <= 2 + black; w++) if (x + w >= 0 && x + w < 32) PIX(x + w, y, black);
        }
        for (int i = 14; i < 22; i++) {          /* handle */
            int x = 2 + i, y = 29 - i;
            for (int w = -2 - black; w <= 3 + black; w++) if (x + w >= 0 && x + w < 32) PIX(x + w, y, black);
        }
    }
    for (int i = 1; i < 13; i++) PIX(3 + i, 29 - i, 1);  /* edge line */
    #undef PIX
    ICONINFO ii = { FALSE, 2, 30, CreateBitmap(32, 32, 1, 1, andm), CreateBitmap(32, 32, 1, 1, xorm) };
    /* monochrome cursors take a double-height mask: AND on top, XOR below */
    BYTE both[256];
    memcpy(both, andm, 128); memcpy(both + 128, xorm, 128);
    DeleteObject(ii.hbmMask); DeleteObject(ii.hbmColor);
    ii.hbmMask = CreateBitmap(32, 64, 1, 1, both);
    ii.hbmColor = NULL;
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    return c ? c : LoadCursor(NULL, IDC_CROSS);
}

/* ------------------------------------------------------------ toolbar -- */

#define SLICE_BTN_ID   0xBF80

static HWND g_toolDlg, g_knifeBtn;
static void set_active(BOOL on);

static void draw_knife_glyph(HDC dc, RECT rc, COLORREF col)
{
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    POINT blade[4] = { { cx - 7, cy + 7 }, { cx + 1, cy - 1 }, { cx + 3, cy + 1 }, { cx - 6, cy + 8 } };
    Polygon(dc, blade, 4);
    POINT handle[4] = { { cx + 2, cy - 3 }, { cx + 6, cy - 7 }, { cx + 8, cy - 5 }, { cx + 4, cy - 1 } };
    Polygon(dc, handle, 4);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

static LRESULT CALLBACK tooldlg_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_DRAWITEM && wp == SLICE_BTN_ID) {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        v4_tool_button_bg(di);
        draw_knife_glyph(di->hDC, di->rcItem, themed(g_active ? TOOL_ACTIVE : TOOL_IDLE));
        return TRUE;
    }
    if (msg == WM_DRAWITEM && wp == V4_TOOL_ARROW && g_active) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        grey_out(di->hDC, di->rcItem);
        return r;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == SLICE_BTN_ID) { set_active(!g_active); return 0; }
        if (g_active) set_active(FALSE);    /* any other tool button */
    }
    if (msg == V4_WM_MODTOOL && wp != SLICE_BTN_ID && g_active) set_active(FALSE);
    return DefSubclassProc(h, msg, wp, lp);
}

/* --------------------------------------------------------- view hooks -- */

static void do_slice(int x, int y, UINT keys)
{
    PartRef pr;
    if (!open_part(&pr)) { A->Log("slice: no musical part open"); return; }
    BOOL all = (keys & MK_CONTROL) != 0;
    long t = snap_tick(tick_at_x(x), g_snap && !(keys & MK_SHIFT));
    NoteRef refs[64];
    int n = notes_at(&pr, t, all ? -1 : note_at_y(y), refs, 64);
    if (!n) { MessageBeep(MB_OK); return; }
    int done = cut_notes(&pr, refs, n, t);
    A->Log("slice: tick %ld, %d/%d note(s) cut", t, done, n);
    InvalidateRect(g_viewHwnd, NULL, FALSE);
}

static LRESULT CALLBACK view_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    switch (msg) {
    case WM_SETCURSOR:
        if (g_active && LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (in_note_area(pt.x, pt.y)) { SetCursor(g_knife); return TRUE; }
        }
        break;
    case WM_KEYDOWN:
        if (wp == 'K' && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) { set_active(!g_active); return 0; }
        if (wp == VK_ESCAPE && g_active) { set_active(FALSE); return 0; }
        break;
    case WM_NCDESTROY:
        g_view = NULL; g_viewHwnd = NULL;
        RemoveWindowSubclass(h, view_subclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

typedef void (THISCALL *MouseHandler_t)(void *self, UINT flags, int x, int y);
static MouseHandler_t o_OnLButtonDown, o_OnMouseMove;

static void THISCALL h_OnLButtonDown(void *self, UINT flags, int x, int y)
{
    v4_attach_view((BYTE *)self, view_subclass, 1);
    if (g_active && in_note_area(x, y)) { SetFocus(g_viewHwnd); do_slice(x, y, flags); return; }
    o_OnLButtonDown(self, flags, x, y);
}

static void THISCALL h_OnMouseMove(void *self, UINT flags, int x, int y)
{
    v4_attach_view((BYTE *)self, view_subclass, 1);
    o_OnMouseMove(self, flags, x, y);
}

/* ---------------------------------------------------------------- UI -- */

static void set_active(BOOL on)
{
    if (on) {
        /* the editor's own arrow tool stays active underneath (outside the note area) */
        HWND main = A->GetMainWindow();
        if (main) SendMessageW(main, WM_COMMAND, V4_TOOL_ARROW, 0);
        if (g_toolDlg) SendMessageW(g_toolDlg, V4_WM_MODTOOL, SLICE_BTN_ID, 0);   /* other mod tools step aside */
    }
    g_active = on;
    A->CheckMenuItem(g_cmdTool, on);
    if (g_knifeBtn) InvalidateRect(g_knifeBtn, NULL, FALSE);
    if (g_toolDlg) { HWND arrow = GetDlgItem(g_toolDlg, V4_TOOL_ARROW); if (arrow) InvalidateRect(arrow, NULL, FALSE); }
    if (g_viewHwnd) { POINT pt; GetCursorPos(&pt); SetCursorPos(pt.x, pt.y); }
}

static void __cdecl menu_tool(void *c) { (void)c; set_active(!g_active); }
static void __cdecl menu_snap(void *c)
{
    (void)c; g_snap = !g_snap; A->CheckMenuItem(g_cmdSnap, g_snap);
    vh_ini_set_int(g_ini, L"SliceTool", L"Snap", g_snap);
}
static void __cdecl menu_sustain(void *c)
{
    (void)c; g_sustain = !g_sustain; A->CheckMenuItem(g_cmdSustain, g_sustain);
    vh_ini_set_int(g_ini, L"SliceTool", L"Sustain", g_sustain);
}

static void __cdecl on_ui_ready(HWND mainWnd, void *ctx)
{
    (void)ctx;
    g_cmdTool = vh_menu_add(A, NULL, L"Slice Tool\tK", menu_tool, NULL);
    g_cmdSnap = vh_menu_add(A, L"Slice Tool", L"Snap cuts to QUANTIZE", menu_snap, NULL);
    g_cmdSustain = vh_menu_add(A, L"Slice Tool", L"New piece holds the vowel (\"-\")", menu_sustain, NULL);
    A->CheckMenuItem(g_cmdSnap, g_snap);
    A->CheckMenuItem(g_cmdSustain, g_sustain);
    g_knifeBtn = v4_add_tool_button(mainWnd, SLICE_BTN_ID, L"tool_slice", tooldlg_subclass, &g_toolDlg);
    if (!g_knifeBtn) A->Log("slice: tool bar not found");
}

__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api)
{
    A = api;
    if (api->version < VH_API_MIN_VERSION) return 0;
    load_settings();
    if (!v4_resolve_imports()) { A->Log("slice: editor DLL exports missing"); return 0; }
    if (!api->HookCode((void *)V4_MusicalPartWnd_OnLButtonDown, V4_SIG_OnLButtonDown, 6,
                       h_OnLButtonDown, (void **)&o_OnLButtonDown)) return 0;
    api->HookCode((void *)V4_MusicalPartWnd_OnMouseMove, V4_SIG_OnMouseMove, 6, h_OnMouseMove, (void **)&o_OnMouseMove);
    g_knife = make_knife_cursor();
    api->OnUiReady(on_ui_ready, NULL);
    return 1;
}
