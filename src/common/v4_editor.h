/*
 * Shared helpers for mods that work in the MUSICAL EDITOR: Vsq4/dscl4 entry
 * points, piano-roll geometry, part/note/control access, tool-bar buttons and
 * theme-aware colours. Header-only; each mod gets its own copy of the state.
 * Include after defining `static const VHApi *A;`.
 */
#pragma once
#include <windows.h>
#include <commctrl.h>
#include "vh_api.h"
#include "v4_internals.h"
#include "vh_util.h"

/* ------------------------------------------------- editor entry points -- */

typedef void *(THISCALL *GetVsqObject_t)(void *mgr, unsigned id);
typedef void *(THISCALL *GetVsqEditor_t)(void *mgr, unsigned kind);
typedef void (THISCALL *ReleaseVsqEditor_t)(void *mgr, void **ed);
typedef long (THISCALL *ScreenToClock_t)(void *grid, long x, int *out);
typedef long (THISCALL *ClockToScreen_t)(void *grid, long t);
typedef unsigned short (THISCALL *GetQuantize_t)(void *grid);
typedef void *(__cdecl *RTDynamicCast_t)(void *p, long vfdelta, void *src, void *dst, int isRef);

static GetVsqObject_t pGetVsqObject;
static GetVsqEditor_t pGetVsqEditor;
static ReleaseVsqEditor_t pReleaseVsqEditor;
static ScreenToClock_t pScreenToClock;
static ClockToScreen_t pClockToScreen;
static GetQuantize_t pGetQuantize;
static RTDynamicCast_t pDynCast;

typedef BOOL (THISCALL *AttrGet_t)(void *attr, const wchar_t *name, int *out);
typedef const wchar_t *(THISCALL *PhonemeAt_t)(void *note, unsigned i);
typedef void *(THISCALL *ListItem_t)(void *list, unsigned i);
typedef void *(THISCALL *PartControls_t)(void *part, int type);
typedef BOOL (THISCALL *UpdateNote_t)(void *ed, unsigned id, const VSNoteData *d);
typedef BOOL (THISCALL *InsertNote_t)(void *ed, unsigned listId, const VSNoteData *d, const int *expr, const int *vib);
typedef BOOL (THISCALL *UpsertControl_t)(void *ed, unsigned listId, const VSControlData *d);
typedef BOOL (THISCALL *RemoveControl_t)(void *ed, unsigned id);

static BOOL v4_resolve_imports(void)
{
    HMODULE vsq = GetModuleHandleA("vsq4.dll"), dscl = GetModuleHandleA("dscl4.dll"), crt = GetModuleHandleA("msvcr110.dll");
    if (!vsq || !dscl || !crt) return FALSE;
    pGetVsqObject = (GetVsqObject_t)GetProcAddress(vsq, "?vsqGetVsqObject@CVSVsqManagerIF@@QBEPAVIVSVsqObject@@I@Z");
    pGetVsqEditor = (GetVsqEditor_t)GetProcAddress(vsq, "?editGetVsqEditor@CVSVsqManagerIF@@QAEPAVIVSVsqEditorIF@@I@Z");
    pReleaseVsqEditor = (ReleaseVsqEditor_t)GetProcAddress(vsq, "?editReleaseVsqEditor@CVSVsqManagerIF@@QAEXAAPAVIVSVsqEditorIF@@@Z");
    pScreenToClock = (ScreenToClock_t)GetProcAddress(dscl, "?ScreenToClock@CGridManager@@QAEJJPAH@Z");
    pClockToScreen = (ClockToScreen_t)GetProcAddress(dscl, "?ClockToScreen@CGridManager@@QAEJJ@Z");
    pGetQuantize = (GetQuantize_t)GetProcAddress(dscl, "?GetQuantizeResolution@CGridManager@@QAEGXZ");
    pDynCast = (RTDynamicCast_t)GetProcAddress(crt, "__RTDynamicCast");
    return pGetVsqObject && pGetVsqEditor && pReleaseVsqEditor && pScreenToClock && pClockToScreen && pGetQuantize && pDynCast;
}

/* ------------------------------------------------------- view geometry -- */

static BYTE *g_view;        /* CMusicalPartWnd* */
static HWND g_viewHwnd;

#define PANE()     (g_view + V4_MPW_PIANOROLL)
#define PANE_I(o)  (*(int *)(PANE() + (o)))
#define RULER()    (*(BYTE **)(PANE() + V4_PRP_RULER))
#define RULER_I(o) (*(int *)(RULER() + (o)))

static RECT note_area(void) { return *(RECT *)(PANE() + V4_PRP_RECT); }

static BOOL in_note_area(int x, int y)
{
    RECT na = note_area();
    POINT pt = { x, y };
    return PtInRect(&na, pt);
}

static long tick_at_x(int x)
{
    return pScreenToClock(RULER() + V4_RUL_GRID, x + RULER_I(V4_RUL_SCROLL_B) - RULER_I(V4_RUL_SCROLL_A) - RULER_I(V4_RUL_LEFT), NULL);
}

static int x_of_tick(long t)
{
    return pClockToScreen(RULER() + V4_RUL_GRID, t) + RULER_I(V4_RUL_SCROLL_A) - RULER_I(V4_RUL_SCROLL_B) + RULER_I(V4_RUL_LEFT);
}

static int note_at_y(int y)
{
    int rowH = PANE_I(V4_PRP_ROWH);
    if (rowH <= 0) return -1;
    return 127 - (PANE_I(V4_PRP_SCROLLY) - PANE_I(V4_PRP_RECT + 4) + y) / rowH;
}

/* continuous pitch in semitones; row centres are whole numbers */
static double pitch_at_y(int y)
{
    int rowH = PANE_I(V4_PRP_ROWH);
    if (rowH <= 0) return -1;
    return 127.5 - (double)(PANE_I(V4_PRP_SCROLLY) - PANE_I(V4_PRP_RECT + 4) + y) / rowH;
}

static int y_of_note(int n)   /* top of the row */
{
    return (127 - n) * PANE_I(V4_PRP_ROWH) - PANE_I(V4_PRP_SCROLLY) + PANE_I(V4_PRP_RECT + 4);
}

static int y_of_pitch(double p)
{
    return (int)((127.5 - p) * PANE_I(V4_PRP_ROWH)) - PANE_I(V4_PRP_SCROLLY) + PANE_I(V4_PRP_RECT + 4);
}

static long quantize_ticks(void) { return pGetQuantize(RULER() + V4_RUL_GRID); }

static long snap_tick(long t, BOOL snap)
{
    if (!snap) return t;
    long q = quantize_ticks();
    if (q <= 1) return t;
    return ((t + q / 2) / q) * q;
}

/* ------------------------------------------------------------ the part -- */

typedef struct {
    void *note;          /* IVSMusicalNote* */
    unsigned id;
    int pos, dur, num;   /* pos relative to the part */
} NoteRef;

typedef struct {
    void *mgr;
    void *part;          /* IVSMusicalPart* */
    int partPos;
    unsigned listId;     /* note list */
    void *list;
} PartRef;

static BOOL open_part(PartRef *pr)
{
    pr->mgr = A->GetVsqManager();
    if (!pr->mgr || !g_view) return FALSE;
    unsigned partId = *(unsigned *)(PANE() + V4_PRP_PARTID);
    void *obj = pGetVsqObject(pr->mgr, partId);
    pr->part = obj ? pDynCast(obj, 0, V4_TD_IVSVsqObject, V4_TD_IVSMusicalPart, 0) : NULL;
    if (!pr->part) return FALSE;
    pr->partPos = VCALL0(int, pr->part, VSPART_POS);
    pr->list = VCALL0(void *, pr->part, VSPART_NOTES);
    if (!pr->list) return FALSE;
    pr->listId = VCALL0(unsigned, pr->list, VSOBJ_ID);
    return TRUE;
}

static BOOL note_ref(void *obj, NoteRef *r)
{
    void *note = obj ? pDynCast(obj, 0, V4_TD_IVSVsqObject, V4_TD_IVSMusicalNote, 0) : NULL;
    if (!note) return FALSE;
    r->note = note;
    r->id = VCALL0(unsigned, note, VSOBJ_ID);
    r->pos = VCALL0(int, note, VSNOTE_POS);
    r->dur = VCALL0(int, note, VSNOTE_DUR);
    r->num = VCALL0(int, note, VSNOTE_NOTENUM);
    return TRUE;
}

/* all notes of the part, in list order (which is time order) */
static int part_notes(PartRef *pr, NoteRef *out, int max)
{
    int count = VCALL0(int, pr->list, VSLIST_COUNT), k = 0;
    ListItem_t item = VMETHOD(ListItem_t, pr->list, VSLIST_ITEM);
    for (int i = 0; i < count && k < max; i++)
        if (note_ref(item(pr->list, (unsigned)i), &out[k])) k++;
    return k;
}

/* Notes crossing absolute tick t (strictly inside), optionally only pitch n. */
static int notes_at(PartRef *pr, long t, int n, NoteRef *out, int max)
{
    int count = VCALL0(int, pr->list, VSLIST_COUNT), k = 0;
    ListItem_t item = VMETHOD(ListItem_t, pr->list, VSLIST_ITEM);
    for (int i = 0; i < count && k < max; i++) {
        NoteRef r;
        if (!note_ref(item(pr->list, (unsigned)i), &r)) continue;
        long a = pr->partPos + r.pos, b = a + r.dur;
        if (t > a && t < b && (n < 0 || r.num == n)) out[k++] = r;
    }
    return k;
}

/* --------------------------------------------------------- controls -- */

typedef struct { void *list; unsigned listId; } ControlList;
typedef struct { unsigned id; int pos, value; } ControlRef;

static BOOL open_controls(PartRef *pr, int type, ControlList *cl)
{
    cl->list = VMETHOD(PartControls_t, pr->part, VSPART_CONTROLS)(pr->part, type);
    if (!cl->list) return FALSE;
    cl->listId = VCALL0(unsigned, cl->list, VSOBJ_ID);
    return TRUE;
}

static int control_events(ControlList *cl, ControlRef *out, int max)
{
    int count = VCALL0(int, cl->list, VSLIST_COUNT), k = 0;
    ListItem_t item = VMETHOD(ListItem_t, cl->list, VSLIST_ITEM);
    for (int i = 0; i < count && k < max; i++) {
        void *obj = item(cl->list, (unsigned)i);
        void *c = obj ? pDynCast(obj, 0, V4_TD_IVSVsqObject, V4_TD_IVSMusicalControl, 0) : NULL;
        if (!c) continue;
        out[k].id = VCALL0(unsigned, c, VSOBJ_ID);
        out[k].pos = VCALL0(int, c, VSCTRL_POS);
        out[k].value = (short)VCALL0(int, c, VSCTRL_VALUE);
        k++;
    }
    return k;
}

/* value in effect at part-relative tick t (events sorted by pos) */
static int control_value_at(const ControlRef *ev, int n, int t, int def)
{
    int v = def;
    for (int i = 0; i < n && ev[i].pos <= t; i++) v = ev[i].value;
    return v;
}

/* -------------------------------------------------------- appearance -- */

/* follow the UI Colors theme when that mod is loaded */
static COLORREF themed(COLORREF c)
{
    typedef COLORREF (__cdecl *Remap_t)(int area, int kind, COLORREF c);
    static Remap_t remapFn;
    static BOOL looked;
    if (!looked) {
        HMODULE m = GetModuleHandleW(L"UIColors.dll");
        remapFn = m ? (Remap_t)GetProcAddress(m, "UIColors_Remap") : NULL;
        looked = TRUE;
    }
    return remapFn ? remapFn(6 /* bitmaps: same as the built-in tool icons */, 0, c) : c;
}

#define TOOL_BG      RGB(0x33, 0x33, 0x33)
#define TOOL_IDLE    RGB(0xFF, 0xFF, 0xFF)
#define TOOL_ACTIVE  RGB(0xD8, 0xFF, 0x30)

/* While a mod tool is active the editor runs its arrow tool underneath;
 * draw that button greyed so only the mod's button looks lit. */
static void grey_out(HDC dc, RECT rc)
{
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    DWORD *px;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!dib) { DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, dc, rc.left, rc.top, SRCCOPY);
    for (int i = 0; i < w * h; i++) {
        DWORD p = px[i];
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        int m = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (m - lo > 24) px[i] = (m << 16) | (m << 8) | m;
    }
    BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

/* ------------------------------------------------------ tool buttons -- */

#define V4_TOOL_ARROW  33329
#define V4_TOOL_ERASER 33332
#define V4_VT_CMyToolDialogBar 0x0055E9B4

/* MFC's idle update (CDialogBar -> UpdateDialogControls) disables every button
 * in the bar whose id has no command handler in the frame - which is ours, so
 * it never sends BN_CLICKED. Not answering DLGC_BUTTON makes MFC skip it, and
 * a disable that still gets through is undone. */
static LRESULT CALLBACK v4__tool_btn_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_GETDLGCODE:
        return 0;
    case WM_ENABLE:
        if (!wp) { EnableWindow(h, TRUE); return 0; }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, v4__tool_btn_proc, id);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* Adds an owner-drawn button after the right-most button of the tool bar
 * (arrow/pencil/line/eraser + other mods' buttons), growing the bar. The tool
 * dialog is subclassed with `proc` (SetWindowSubclass, so mods chain). */
static HWND v4_add_tool_button(HWND mainWnd, int id, const wchar_t *name, SUBCLASSPROC proc, HWND *toolDlgOut)
{
    HWND eraser = vh_find_ctrl(mainWnd, V4_TOOL_ERASER);
    if (!eraser) return NULL;
    HWND dlg = GetParent(eraser);
    RECT er;
    GetWindowRect(eraser, &er);
    MapWindowPoints(NULL, dlg, (POINT *)&er, 2);
    int right = er.right;
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        int cid = GetDlgCtrlID(c);
        if (!IsWindowVisible(c) || !((cid >= V4_TOOL_ARROW && cid <= V4_TOOL_ERASER) || cid >= 0xBF80)) continue;
        RECT r; GetWindowRect(c, &r); MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        if (r.right > right) right = r.right;
    }
    int bw = er.right - er.left, bx = right + 2, need = bx + bw + 6;
    RECT dr;
    GetClientRect(dlg, &dr);
    if (dr.right < need) {
        BYTE *bar = vh_find_cwnd(V4_VT_CMyToolDialogBar, dlg);
        RECT wr; GetWindowRect(dlg, &wr);
        int w = wr.right - wr.left, hgt = wr.bottom - wr.top, patched = 0;
        for (int off = 0x24; bar && off < 0x200; off += 4) {
            SIZE *sz = (SIZE *)(bar + off);
            if (sz->cx == w && sz->cy == hgt) { sz->cx += need - dr.right; patched = off; break; }
        }
        if (patched) {
            RECT mr; GetClientRect(mainWnd, &mr);
            SendMessageW(mainWnd, WM_SIZE, IsZoomed(mainWnd) ? SIZE_MAXIMIZED : SIZE_RESTORED, MAKELPARAM(mr.right, mr.bottom));
        } else {
            SetWindowPos(dlg, NULL, 0, 0, w + need - dr.right, hgt, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    HWND btn = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                               bx, er.top, bw, er.bottom - er.top, dlg, (HMENU)(UINT_PTR)id,
                               GetModuleHandleW(NULL), NULL);
    if (btn) { SetWindowSubclass(btn, v4__tool_btn_proc, (UINT_PTR)id, 0); EnableWindow(btn, TRUE); }
    SetWindowSubclass(dlg, proc, (UINT_PTR)id, 0);
    if (toolDlgOut) *toolDlgOut = dlg;
    return btn;
}

static void v4_tool_button_bg(DRAWITEMSTRUCT *di)
{
    HBRUSH bg = CreateSolidBrush(themed(TOOL_BG));
    FillRect(di->hDC, &di->rcItem, bg);
    DeleteObject(bg);
    if (di->itemState & ODS_SELECTED) OffsetRect(&di->rcItem, 1, 1);
}

/* view object is only reachable from inside its own handlers */
static void v4_attach_view(BYTE *self, SUBCLASSPROC viewProc, UINT_PTR id)
{
    HWND hwnd = *(HWND *)(self + V4_MPW_HWND);
    if (self == g_view && hwnd == g_viewHwnd) return;
    g_view = self;
    g_viewHwnd = hwnd;
    SetWindowSubclass(hwnd, viewProc, id, 0);
}
