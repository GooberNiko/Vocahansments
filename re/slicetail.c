
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
    g_hoverValid = FALSE;
    InvalidateRect(g_viewHwnd, NULL, FALSE);
}

static UINT mod_keys(void)
{
    return ((GetKeyState(VK_SHIFT) & 0x8000) ? MK_SHIFT : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0);
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
    case WM_MOUSEMOVE: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (g_active) update_hover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (UINT)wp);
        return r;
    }
    case WM_MOUSELEAVE:
        invalidate_hover(); g_hoverValid = FALSE;
        break;
    case WM_KEYDOWN:
        if (wp == 'K' && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) { set_active(!g_active); return 0; }
        if (wp == VK_ESCAPE && g_active) { set_active(FALSE); return 0; }
        /* fallthrough */
    case WM_KEYUP:
        if ((wp == VK_SHIFT || wp == VK_CONTROL) && g_active) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            update_hover(pt.x, pt.y, mod_keys());
        }
        break;
    case WM_PAINT: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (g_active && g_hoverValid) { HDC dc = GetDC(h); draw_hover(dc); ReleaseDC(h, dc); }
        return r;
    }
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
    if (!on) { invalidate_hover(); g_hoverValid = FALSE; }
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
    g_cmdTool = A->AddMenuItem(L"Slice Tool\tK", menu_tool, NULL);
    g_cmdSnap = A->AddMenuItem(L"Slice: snap cuts to QUANTIZE", menu_snap, NULL);
    g_cmdSustain = A->AddMenuItem(L"Slice: new piece holds the vowel (\"-\")", menu_sustain, NULL);
    A->CheckMenuItem(g_cmdSnap, g_snap);
    A->CheckMenuItem(g_cmdSustain, g_sustain);
    g_knifeBtn = v4_add_tool_button(mainWnd, SLICE_BTN_ID, L"tool_slice", tooldlg_subclass, &g_toolDlg);
    if (!g_knifeBtn) A->Log("slice: tool bar not found");
}

__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api)
{
    A = api;
    if (api->version < VH_API_VERSION) return 0;
    load_settings();
    if (!v4_resolve_imports()) { A->Log("slice: editor DLL exports missing"); return 0; }
    if (!api->HookCode((void *)V4_MusicalPartWnd_OnLButtonDown, V4_SIG_OnLButtonDown, 6,
                       h_OnLButtonDown, (void **)&o_OnLButtonDown)) return 0;
    api->HookCode((void *)V4_MusicalPartWnd_OnMouseMove, V4_SIG_OnMouseMove, 6, h_OnMouseMove, (void **)&o_OnMouseMove);
    g_knife = make_knife_cursor();
    api->OnUiReady(on_ui_ready, NULL);
    return 1;
}
