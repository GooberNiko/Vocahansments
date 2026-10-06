/*
 * Helpers drawn over the piano roll (in the buffered frame, with the pitch
 * curve):
 *
 * Scale Guide   pick a key and scale: rows outside the scale are dimmed, the
 *               key's own rows are lit, and notes outside it get a red frame.
 * Note Info     hover a note to see its name, length, duration, velocity
 *               and lyric just above it.
 *
 * Included by pitchtools.c.
 */

/* ------------------------------------------------------------ scales -- */

typedef struct { const wchar_t *name; WORD mask; } Scale;      /* bit n: n semitones above the key */
static const Scale kScales[] = {
    { L"Major",            0xAB5 },
    { L"Minor",            0x5AD },
    { L"Harmonic minor",   0x9AD },
    { L"Dorian",           0x6AD },
    { L"Major pentatonic", 0x295 },
    { L"Minor pentatonic", 0x4A9 },
    { L"Blues",            0x4E9 },
};
#define NSCALES (int)(sizeof kScales / sizeof kScales[0])
static const wchar_t *const kKeyNames[12] = { L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B" };

static int g_scale = -1;             /* index into kScales, -1 = off */
static int g_scaleKey;               /* 0 = C */
static BOOL g_scaleMark = TRUE;      /* frame notes outside the scale */
static UINT g_cmdScaleOff, g_cmdScale[NSCALES], g_cmdKey[12], g_cmdMark, g_cmdHover;

static BOOL in_scale(int num) { return (kScales[g_scale].mask >> (((num - g_scaleKey) % 12 + 12) % 12)) & 1; }

/* translucent fill: a 1x1 bitmap of the colour, stretched with AlphaBlend */
static void tint_rect(HDC dc, const RECT *r, COLORREF c, BYTE alpha)
{
    static HDC mem[2];
    static COLORREF memc[2];
    int k = c == memc[0] && mem[0] ? 0 : (c == memc[1] && mem[1] ? 1 : -1);
    if (k < 0) {
        k = mem[0] ? 1 : 0;
        if (mem[k]) DeleteDC(mem[k]);
        mem[k] = CreateCompatibleDC(NULL);
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 1, 1, 1, 32, BI_RGB } };
        DWORD *px;
        HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        if (!b) { DeleteDC(mem[k]); mem[k] = NULL; return; }
        *px = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
        SelectObject(mem[k], b);              /* kept for good: one per colour */
        memc[k] = c;
    }
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    AlphaBlend(dc, r->left, r->top, r->right - r->left, r->bottom - r->top, mem[k], 0, 0, 1, 1, bf);
}

static void scale_draw(HDC dc)
{
    if (g_scale < 0 || !g_view) return;
    RECT na = note_area();
    int rowH = PANE_I(V4_PRP_ROWH);
    if (rowH <= 0) return;
    int top = note_at_y(na.top), bottom = note_at_y(na.bottom - 1);
    for (int n = top; n >= bottom && n >= 0; n--) {
        if (n > 127) continue;
        int y = y_of_note(n);
        RECT row = { na.left, y, na.right, y + rowH };
        if (!IntersectRect(&row, &row, &na)) continue;
        if (!in_scale(n)) tint_rect(dc, &row, RGB(0, 0, 0), 46);                           /* out: dimmed */
        else if ((n - g_scaleKey) % 12 == 0) tint_rect(dc, &row, RGB(0xFF, 0xC4, 0x30), 52);   /* the key: lit */
    }
    if (!g_scaleMark || !pm_refresh()) return;
    HRGN clip = CreateRectRgnIndirect(&na);
    SelectClipRgn(dc, clip);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0xE5, 0x2B, 0x2B));
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *m = &g_pmNotes[i];
        if (in_scale(m->num)) continue;
        int x0 = x_of_tick(m->tOn), x1 = x_of_tick(m->tOff), y = y_of_note(m->num);
        if (x1 < na.left || x0 > na.right || y + rowH < na.top || y > na.bottom) continue;
        Rectangle(dc, x0, y, x1 + 1, y + rowH + 1);
    }
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

/* -------------------------------------------------------- note info -- */

static BOOL g_hoverOn = TRUE;
static int g_hover = -1;             /* index into g_pmNotes */
static unsigned g_hoverId;
static RECT g_hoverRect;             /* where the label is, client coords */
static wchar_t g_hoverText[160];
static BOOL g_hoverTracking;

static HFONT hover_font(void)
{
    static HFONT f;
    if (!f) f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return f;
}

static long gcd_l(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a; }

/* "1/4", "3/8", "2 bars" (4/4), or ticks when it is no simple fraction */
static void note_length(long dur, wchar_t *out)
{
    long g = gcd_l(dur, 1920), n = dur / g, d = 1920 / g;
    if (dur <= 0) lstrcpyW(out, L"0");
    else if (d == 1) wsprintfW(out, n == 1 ? L"1 bar" : L"%ld bars", n);
    else if (d <= 64) wsprintfW(out, L"%ld/%ld", n, d);
    else wsprintfW(out, L"%ld ticks", dur);
}

static void hover_text(const PMNote *m, wchar_t *out)
{
    wchar_t len[32];
    note_length(m->tOff - m->tOn, len);
    int ms = (int)(m->off - m->on + 0.5);
    /* VOCALOID names middle C (MIDI 60) C3 */
    wsprintfW(out, L"%s%d   \x00B7   %s   \x00B7   %d.%02d s   \x00B7   vel %d%s%s%s", kKeyNames[m->num % 12], m->num / 12 - 2, len,
              ms / 1000, (ms % 1000) / 10, m->vel, m->lyric[0] ? L"   \x00B7   \x201C" : L"", m->lyric, m->lyric[0] ? L"\x201D" : L"");
}

static void hover_set(HWND h, int idx)
{
    RECT old = g_hoverRect;
    g_hover = idx;
    SetRectEmpty(&g_hoverRect);
    if (idx >= 0) {
        PMNote *m = &g_pmNotes[idx];
        g_hoverId = m->id;
        hover_text(m, g_hoverText);
        HDC dc = GetDC(h);
        HGDIOBJ of = SelectObject(dc, hover_font());
        SIZE sz;
        GetTextExtentPoint32W(dc, g_hoverText, lstrlenW(g_hoverText), &sz);
        SelectObject(dc, of);
        ReleaseDC(h, dc);
        RECT na = note_area();
        int rowH = PANE_I(V4_PRP_ROWH), w = sz.cx + 16, ht = sz.cy + 8;
        int x = x_of_tick(m->tOn), y = y_of_note(m->num) - ht - 4;
        if (y < na.top) y = y_of_note(m->num) + rowH + 16;           /* no room above: below its squiggle */
        if (x + w > na.right) x = na.right - w;
        if (x < na.left) x = na.left;
        SetRect(&g_hoverRect, x, y, x + w, y + ht);
    }
    /* repaint where the label was and where it is (buffered: no flicker) */
    if (!IsRectEmpty(&old)) RedrawWindow(h, &old, NULL, RDW_INVALIDATE | RDW_ERASE);
    if (!IsRectEmpty(&g_hoverRect)) RedrawWindow(h, &g_hoverRect, NULL, RDW_INVALIDATE | RDW_ERASE);
}

static void hover_move(HWND h, int x, int y, WPARAM keys)
{
    int idx = -1;
    if (g_hoverOn && g_view && !g_active && !(keys & (MK_LBUTTON | MK_RBUTTON)) && in_note_area(x, y) && pm_refresh()) {
        long t = tick_at_x(x);
        int num = note_at_y(y);
        for (int i = 0; i < g_pmCount; i++)
            if (g_pmNotes[i].num == num && t >= g_pmNotes[i].tOn && t < g_pmNotes[i].tOff) { idx = i; break; }
    }
    if (idx >= 0 && !g_hoverTracking) {
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        g_hoverTracking = TrackMouseEvent(&tme);
    }
    if (idx != g_hover || (idx >= 0 && g_pmNotes[idx].id != g_hoverId)) hover_set(h, idx);
}

static void hover_leave(HWND h)
{
    g_hoverTracking = FALSE;
    if (g_hover >= 0) hover_set(h, -1);
}

static void hover_draw(HDC dc)
{
    if (g_hover < 0 || IsRectEmpty(&g_hoverRect)) return;
    RECT r = g_hoverRect;
    HBRUSH bg = CreateSolidBrush(RGB(0x26, 0x28, 0x30));
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(0x5A, 0x60, 0x70));
    HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, edge), of = SelectObject(dc, hover_font());
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
    int bk = SetBkMode(dc, TRANSPARENT);
    COLORREF tc = SetTextColor(dc, RGB(0xF2, 0xF2, 0xF2));
    DrawTextW(dc, g_hoverText, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, tc);
    SetBkMode(dc, bk);
    SelectObject(dc, ob); SelectObject(dc, op); SelectObject(dc, of);
    DeleteObject(bg); DeleteObject(edge);
}

/* ------------------------------------------------------------ toast -- */
/* a short message at the top of the piano roll (what a Note Tool did) */

static wchar_t g_toast[128];
static RECT g_toastRect;
#define TOAST_TIMER 0x544F4153   /* 'TOAS' */

static VOID CALLBACK toast_tick(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    (void)m; (void)t;
    KillTimer(h, id);
    RECT r = g_toastRect;
    g_toast[0] = 0;
    SetRectEmpty(&g_toastRect);
    if (!IsRectEmpty(&r)) RedrawWindow(h, &r, NULL, RDW_INVALIDATE | RDW_ERASE);
}

static void toast_show(const wchar_t *text)
{
    A->Log("notetools: %ls", text);
    if (!g_viewHwnd || !g_view) { MessageBeep(MB_ICONASTERISK); return; }
    RECT old = g_toastRect;
    lstrcpynW(g_toast, text, 128);
    HDC dc = GetDC(g_viewHwnd);
    HGDIOBJ of = SelectObject(dc, hover_font());
    SIZE sz;
    GetTextExtentPoint32W(dc, g_toast, lstrlenW(g_toast), &sz);
    SelectObject(dc, of);
    ReleaseDC(g_viewHwnd, dc);
    RECT na = note_area();
    int w = sz.cx + 28, h = sz.cy + 12, x = (na.left + na.right - w) / 2;
    SetRect(&g_toastRect, x, na.top + 10, x + w, na.top + 10 + h);
    if (!IsRectEmpty(&old)) RedrawWindow(g_viewHwnd, &old, NULL, RDW_INVALIDATE | RDW_ERASE);
    RedrawWindow(g_viewHwnd, &g_toastRect, NULL, RDW_INVALIDATE | RDW_ERASE);
    SetTimer(g_viewHwnd, TOAST_TIMER, 2500, toast_tick);
}

static void toast_draw(HDC dc)
{
    if (!g_toast[0] || IsRectEmpty(&g_toastRect)) return;
    RECT r = g_toastRect;
    HBRUSH bg = CreateSolidBrush(RGB(0x1E, 0x5A, 0x8C));
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(0x6F, 0xA8, 0xD8));
    HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, edge), of = SelectObject(dc, hover_font());
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 10, 10);
    int bk = SetBkMode(dc, TRANSPARENT);
    COLORREF tc = SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    DrawTextW(dc, g_toast, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, tc);
    SetBkMode(dc, bk);
    SelectObject(dc, ob); SelectObject(dc, op); SelectObject(dc, of);
    DeleteObject(bg); DeleteObject(edge);
}

/* ------------------------------------------------------------- menus -- */

static void guides_repaint(void)
{
    if (g_viewHwnd) RedrawWindow(g_viewHwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void guides_checks(void)
{
    A->CheckMenuItem(g_cmdScaleOff, g_scale < 0);
    for (int i = 0; i < NSCALES; i++) A->CheckMenuItem(g_cmdScale[i], g_scale == i);
    for (int i = 0; i < 12; i++) A->CheckMenuItem(g_cmdKey[i], g_scaleKey == i);
    A->CheckMenuItem(g_cmdMark, g_scaleMark);
    A->CheckMenuItem(g_cmdHover, g_hoverOn);
}

static void guides_save(void)
{
    vh_ini_set_int(g_ini, L"PitchTools", L"ScaleGuide", g_scale);
    vh_ini_set_int(g_ini, L"PitchTools", L"ScaleKey", g_scaleKey);
    vh_ini_set_int(g_ini, L"PitchTools", L"ScaleMarkNotes", g_scaleMark);
    vh_ini_set_int(g_ini, L"PitchTools", L"NoteInfo", g_hoverOn);
    guides_checks();
    guides_repaint();
}

static void __cdecl menu_scale(void *c) { g_scale = (int)(INT_PTR)c; guides_save(); }
static void __cdecl menu_key(void *c) { g_scaleKey = (int)(INT_PTR)c; guides_save(); }
static void __cdecl menu_mark(void *c) { (void)c; g_scaleMark = !g_scaleMark; guides_save(); }
static void __cdecl menu_hover(void *c)
{
    (void)c;
    g_hoverOn = !g_hoverOn;
    if (!g_hoverOn && g_viewHwnd) hover_set(g_viewHwnd, -1);
    guides_save();
}

static void guides_menus(void)
{
    g_cmdHover = vh_menu_add(A, NULL, L"Note Info on Hover", menu_hover, NULL);
    g_cmdScaleOff = vh_menu_add(A, L"Scale Guide", L"Off", menu_scale, (void *)(INT_PTR)-1);
    for (int i = 0; i < NSCALES; i++) {
        wchar_t t[48];
        wsprintfW(t, L"Scale: %s", kScales[i].name);
        g_cmdScale[i] = vh_menu_add(A, L"Scale Guide", t, menu_scale, (void *)(INT_PTR)i);
    }
    for (int i = 0; i < 12; i++) {
        wchar_t t[16];
        wsprintfW(t, L"Key: %s", kKeyNames[i]);
        g_cmdKey[i] = vh_menu_add(A, L"Scale Guide", t, menu_key, (void *)(INT_PTR)i);
    }
    g_cmdMark = vh_menu_add(A, L"Scale Guide", L"Mark notes outside the scale", menu_mark, NULL);
    g_scale = vh_ini_int(g_ini, L"PitchTools", L"ScaleGuide", -1);
    if (g_scale < -1 || g_scale >= NSCALES) g_scale = -1;
    g_scaleKey = vh_ini_int(g_ini, L"PitchTools", L"ScaleKey", 0) % 12;
    if (g_scaleKey < 0) g_scaleKey = 0;
    g_scaleMark = vh_ini_int(g_ini, L"PitchTools", L"ScaleMarkNotes", 1) != 0;
    g_hoverOn = vh_ini_int(g_ini, L"PitchTools", L"NoteInfo", 1) != 0;
    guides_checks();
}
