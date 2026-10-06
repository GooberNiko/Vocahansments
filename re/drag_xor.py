root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/'
m = root + 'src/mods/pitchtools/pitchmodel.c'
t = open(m).read()


def sub(s, old, new):
    if old not in s:
        raise SystemExit('missing: ' + old[:160])
    return s.replace(old, new, 1)


# polyline builder shared by normal drawing and the XOR drag preview
t = sub(t, """static BOOL g_pmShow = TRUE;
static COLORREF g_pmColor = RGB(0x00, 0x99, 0xFF);

static void pm_draw(HDC dc)
{
    if (!g_pmShow || !g_view || !pm_refresh()) return;
    PMNote *real = g_pmNotes;
    g_pmNotes = pm_build_shadow();
    RECT na = note_area();
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    HPEN pen = CreatePen(PS_SOLID, 2, themed(g_pmColor));
    HGDIOBJ old = SelectObject(dc, pen);
    BOOL down = FALSE;
    for (int x = na.left; x < na.right; x++) {
        double p;
        if (!pm_pitch_at(tick_at_x(x), TRUE, TRUE, &p)) { down = FALSE; continue; }
        int y = y_of_pitch(p);         /* note number -> row centre */
        if (!down) { MoveToEx(dc, x, y, NULL); down = TRUE; }
        else LineTo(dc, x, y);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
    g_pmNotes = real;
}""", """static BOOL g_pmShow = TRUE, g_pmHide;
static COLORREF g_pmColor = RGB(0x00, 0x99, 0xFF);

/* the curve as polyline runs: pts[] with run lengths in runs[] */
#define PM_MAXPTS 8192
typedef struct { POINT pts[PM_MAXPTS]; DWORD runs[PM_MAXPTS / 2]; int nruns; } PMLine;

static void pm_build_line(PMLine *L)
{
    L->nruns = 0;
    PMNote *real = g_pmNotes;
    g_pmNotes = pm_build_shadow();
    RECT na = note_area();
    int n = 0, runStart = 0;
    for (int x = na.left; x < na.right && n < PM_MAXPTS; x++) {
        double p;
        if (!pm_pitch_at(tick_at_x(x), TRUE, TRUE, &p)) {
            if (n > runStart) { if (n - runStart >= 2 && L->nruns < PM_MAXPTS / 2) L->runs[L->nruns++] = n - runStart; else n = runStart; }
            runStart = n;
            continue;
        }
        L->pts[n].x = x; L->pts[n].y = y_of_pitch(p); n++;
    }
    if (n - runStart >= 2 && L->nruns < PM_MAXPTS / 2) L->runs[L->nruns++] = n - runStart;
    g_pmNotes = real;
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

static PMLine g_line, g_xorLine;
static BOOL g_xorValid;

static void pm_draw(HDC dc)
{
    g_xorValid = FALSE;          /* any repaint wipes the drag preview */
    if (!g_pmShow || g_pmHide || !g_view || !pm_refresh()) return;
    pm_build_line(&g_line);
    pm_stroke_line(dc, &g_line, themed(g_pmColor), R2_COPYPEN);
}

/* drag preview: rubber-band the curve with XOR so it never fights the
 * editor's own drag drawing */
static void pm_drag_preview(void)
{
    if (!g_viewHwnd || !g_pmShow) return;
    HDC dc = GetDC(g_viewHwnd);
    COLORREF xc = themed(g_pmColor) ^ 0x00FFFFFF;     /* NOTXOR with the inverse = the colour on white */
    if (g_xorValid) pm_stroke_line(dc, &g_xorLine, xc, R2_NOTXORPEN);
    pm_build_line(&g_xorLine);
    pm_stroke_line(dc, &g_xorLine, xc, R2_NOTXORPEN);
    g_xorValid = TRUE;
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
    if (r && g_dragMode && wnd && *(HWND *)(wnd + 0x20) == g_viewHwnd && pm_drag_update(pt[0], pt[1]))
        pm_drag_preview();
    return r;
}""")
open(m, 'w').write(t)

p = root + 'src/mods/pitchtools/pitchtools.c'
s = open(p).read()
s = sub(s, """            /* the editor's drag is a modal loop inside this call: track it from here */
            pm_drag_begin(x, y);
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (g_dragMode) { pm_drag_end(); InvalidateRect(h, NULL, FALSE); }
            return r;""", """            /* the editor's drag is a modal loop inside this call: track it from here */
            pm_drag_begin(x, y);
            if (g_dragMode) { g_pmHide = TRUE; RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW); }
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (g_dragMode) { pm_drag_end(); g_pmHide = FALSE; InvalidateRect(h, NULL, FALSE); }
            return r;""")
s = sub(s, """    hook_commit();""", """    hook_commit();
    api->HookCode((void *)V4_TrackPoint, V4_SIG_TrackPoint, 6, h_TrackPoint, (void **)&o_TrackPoint);""")
open(p, 'w').write(s)
print('ok')
