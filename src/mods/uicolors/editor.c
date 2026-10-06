/*
 * UI Colors editor window: every colour the editor has drawn (per area) with
 * its replacement, a colour picker, an eyedropper that finds the original
 * colour of anything you click in VOCALOID, and quick-tint tools that restyle
 * a whole area at once. Edits apply live and autosave to a user theme.
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <math.h>
#include "engine.h"

extern wchar_t g_userThemes[], g_presetThemes[], g_themePath[], g_themeName[];
void settings_save_theme(void);
int engine_area_of_window(HWND h);

/* ------------------------------------------------------------- labels -- */

typedef struct { int area; COLORREF c; const wchar_t *label; } Label;
#define C(rgb) RGB(((rgb) >> 16) & 0xFF, ((rgb) >> 8) & 0xFF, (rgb) & 0xFF)
static const Label kLabels[] = {
    { AREA_ALL, C(0xD8D8D8), L"Piano roll: white-key rows" },
    { AREA_ALL, C(0xBEBEBE), L"Piano roll: black-key rows" },
    { AREA_ALL, C(0xC4C4C4), L"Piano roll: row shading" },
    { AREA_ALL, C(0xBDB89A), L"Piano roll: beat grid lines" },
    { AREA_ALL, C(0xF0F0F0), L"Keyboard: white keys / light panels" },
    { AREA_ALL, C(0x313131), L"Keyboard: black keys / dark panels" },
    { AREA_ALL, C(0xB5DC56), L"Notes: fill" },
    { AREA_ALL, C(0xC9F06A), L"Notes: overlapping another note" },
    { AREA_ALL, C(0x0BE9F4), L"Accent: playhead, readouts, selection" },
    { AREA_ALL, C(0x333333), L"Panels, bars and editor background" },
    { AREA_ALL, C(0x3D3D3D), L"Panel frames" },
    { AREA_ALL, C(0x464646), L"Track editor: lanes" },
    { AREA_ALL, C(0x23696B), L"Track editor: part fill" },
    { AREA_ALL, C(0x4C8487), L"Track editor: part (selected)" },
    { AREA_ALL, C(0xF5F5F5), L"Track grid / borders" },
    { AREA_ALL, C(0x838383), L"Grid lines" },
    { AREA_ALL, C(0xDEDEDE), L"Labels text" },
    { AREA_ALL, C(0x33FF7A), L"Track colour: green" },
    { AREA_ALL, C(0x0096FF), L"Track colour: blue" },
    { AREA_ALL, C(0x00C8FF), L"Track colour: cyan" },
    { AREA_ALL, C(0x80FF00), L"Control curve" },
    { AREA_ALL, C(0x0099FF), L"Pitch render: rendered pitch curve" },
    { AREA_ALL, C(0xFF6600), L"Pitch render: note pitch line" },
    { AREA_SPRITES, C(0x000000), L"Note symbols: vibrato squiggle, attack marks (the greys follow it)" },
    { AREA_SPRITES, C(0x292929), L"Note symbols: line from the attack mark to the vibrato" },
    { AREA_SPRITES, C(0x4C4C4C), L"Note symbols: soft edges" },
    /* Note text: rows are keyed by the note's fill colour (see engine.c) */
    { AREA_NOTETEXT, C(0xB5DC56), L"Lyrics on notes" },
    { AREA_NOTETEXT, C(0x0BE9F4), L"Lyrics on selected notes" },
    { AREA_NOTETEXT, C(0xC9F06A), L"Lyrics on overlapping notes" },
};

static int g_area;
static int g_kind;          /* KIND_ANY = every use */
static const wchar_t *label_for(COLORREF c)
{
    for (unsigned i = 0; i < sizeof kLabels / sizeof kLabels[0]; i++)
        if (kLabels[i].c == c && ((kLabels[i].area == AREA_ALL && g_area != AREA_NOTETEXT) || kLabels[i].area == g_area)) return kLabels[i].label;
    return L"";
}

/* In the Note text area a row's key is the note's fill colour; what the
 * editor really draws there is this lyric colour. */
static COLORREF lyric_default(COLORREF fill)
{
    return fill == C(0xC9F06A) ? C(0x939393) : C(0x000000);
}

/* the colour a row stands for: itself, or the lyric colour in Note text */
static COLORREF row_color(COLORREF orig) { return g_area == AREA_NOTETEXT ? lyric_default(orig) : orig; }

/* what the row is drawn in now */
static COLORREF row_now(COLORREF orig)
{
    if (g_area != AREA_NOTETEXT) return remap(g_area, g_kind, orig);
    COLORREF r = theme_lookup(TBL(AREA_NOTETEXT, KIND_TEXT), orig);
    if (r == NO_COLOR) r = theme_lookup(TBL(AREA_NOTETEXT, KIND_ANY), orig);
    return r == NO_COLOR ? lyric_default(orig) : r;
}

/* --------------------------------------------------------------- rows -- */

typedef struct { COLORREF orig; unsigned uses; char kinds[40]; } Row;
static Row g_rows[4096];
static int g_nrows;
/* g_area: area being edited (declared above) */
/* g_kind: declared above */
#define CUR_TBL TBL(g_area, g_kind)
static HWND g_kindCombo;
static HWND g_wnd, g_list, g_themeCombo, g_areaCombo, g_enable, g_hue, g_sat, g_lit, g_invert, g_status;
static BOOL g_picking;

static int cmp_rows(const void *a, const void *b)
{
    const Row *x = a, *y = b;
    const wchar_t *lx = label_for(x->orig), *ly = label_for(y->orig);
    if (!!*lx != !!*ly) return *lx ? -1 : 1;            /* labelled first */
    return (int)y->uses - (int)x->uses;
}

static Row *row_for(COLORREF c)
{
    for (int i = 0; i < g_nrows; i++) if (g_rows[i].orig == c) return &g_rows[i];
    if (g_nrows >= 4096) return NULL;
    Row *r = &g_rows[g_nrows++];
    memset(r, 0, sizeof *r); r->orig = c;
    return r;
}

static void build_rows(void)
{
    static CensusEntry ce[4096];
    int n = census_snapshot(ce, 4096);
    g_nrows = 0;
    for (int i = 0; i < n; i++) {
        /* "All" lists everything; an area lists what that area drew */
        if (g_area != AREA_ALL && ce[i].area != g_area) continue;
        if (ce[i].area == AREA_NOTETEXT && g_area != AREA_NOTETEXT) continue;   /* keyed by note fill, not a drawn colour */
        if (g_kind != KIND_ANY && ce[i].kind != g_kind) continue;
        if (ce[i].area == AREA_BITMAPS && g_area == AREA_ALL && ce[i].count < 40) continue; /* antialias noise */
        Row *r = row_for(ce[i].color);
        if (!r) break;
        r->uses += ce[i].count;
        if (!strstr(r->kinds, ce[i].what) && strlen(r->kinds) + strlen(ce[i].what) + 2 < sizeof r->kinds) {
            if (r->kinds[0]) strcat(r->kinds, ",");
            strcat(r->kinds, ce[i].what);
        }
    }
    COLORREF o, rep;
    for (int i = 0; theme_entry(CUR_TBL, i, &o, &rep); i++) row_for(o);
    /* named colours are always listed, even before the editor has drawn them
     * (e.g. overlapping notes) */
    for (unsigned i = 0; i < sizeof kLabels / sizeof kLabels[0]; i++)
        if (kLabels[i].area == g_area || (kLabels[i].area == AREA_ALL && g_area != AREA_BITMAPS && g_area != AREA_SPRITES && g_area != AREA_NOTETEXT)) row_for(kLabels[i].c);
    qsort(g_rows, g_nrows, sizeof g_rows[0], cmp_rows);
}

static void fill_list(void)
{
    build_rows();
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_nrows; i++) {
        LVITEMW it = { LVIF_TEXT | LVIF_PARAM, i };
        it.pszText = L""; it.lParam = i;
        ListView_InsertItem(g_list, &it);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    wchar_t s[160];
    wsprintfW(s, L"%d colours in %s%s%s  -  theme: %s", g_nrows, kAreaNames[g_area], g_kind ? L" / " : L"",
              kKindNames[g_kind], g_themeName[0] ? g_themeName : L"(none)");
    SetWindowTextW(g_status, s);
}

/* ------------------------------------------------------- theme files -- */

typedef struct { wchar_t path[MAX_PATH]; wchar_t name[96]; } ThemeFile;
static ThemeFile g_files[128];
static int g_nfiles;

static void scan_dir(const wchar_t *dir, const wchar_t *tag)
{
    wchar_t pat[MAX_PATH];
    wsprintfW(pat, L"%s*.ini", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (g_nfiles >= 128) break;
        ThemeFile *t = &g_files[g_nfiles];
        wsprintfW(t->path, L"%s%s", dir, fd.cFileName);
        wchar_t nm[64];
        GetPrivateProfileStringW(L"Theme", L"Name", L"", nm, 64, t->path);
        if (!nm[0]) continue;      /* not a theme (e.g. census.txt never matches *.ini anyway) */
        wsprintfW(t->name, L"%s%s", nm, tag);
        g_nfiles++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void fill_themes(void)
{
    g_nfiles = 0;
    lstrcpyW(g_files[0].path, L""); lstrcpyW(g_files[0].name, L"VOCALOID4 default (no theme)");
    g_nfiles = 1;
    scan_dir(g_presetThemes, L"");
    scan_dir(g_userThemes, L"  (yours)");
    ComboBox_ResetContent(g_themeCombo);
    int sel = 0;
    for (int i = 0; i < g_nfiles; i++) {
        ComboBox_AddString(g_themeCombo, g_files[i].name);
        if (!lstrcmpiW(g_files[i].path, g_themePath)) sel = i;
    }
    ComboBox_SetCurSel(g_themeCombo, sel);
}

static void select_theme(int i)
{
    if (i < 0 || i >= g_nfiles) return;
    if (!g_files[i].path[0]) { theme_clear(); g_themePath[0] = 0; g_themeName[0] = 0; }
    else if (theme_load(g_files[i].path, g_themeName, 128)) lstrcpyW(g_themePath, g_files[i].path);
    settings_save_theme();
    engine_refresh();
    fill_list();
}

static BOOL is_user_theme(void)
{
    return g_themePath[0] && !wcsncmp(g_themePath, g_userThemes, lstrlenW(g_userThemes));
}

/* Edits always land in a user theme: editing a preset forks it first. */
static void autosave(void)
{
    if (!is_user_theme()) {
        wchar_t base[96];
        wsprintfW(base, L"%s", g_themeName[0] ? g_themeName : L"My Theme");
        if (!is_user_theme() && g_themeName[0] && !wcsstr(base, L"(edited)")) lstrcatW(base, L" (edited)");
        for (int n = 1; n < 100; n++) {
            wchar_t p[MAX_PATH];
            if (n == 1) wsprintfW(p, L"%s%s.ini", g_userThemes, base);
            else wsprintfW(p, L"%s%s %d.ini", g_userThemes, base, n);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
                lstrcpyW(g_themePath, p);
                lstrcpynW(g_themeName, n == 1 ? base : base, 128);
                break;
            }
        }
        settings_save_theme();
        theme_save(g_themePath, g_themeName);
        fill_themes();
        return;
    }
    theme_save(g_themePath, g_themeName);
}

static void save_as(void)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"UI Colors theme (*.ini)\0*.ini\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrInitialDir = g_userThemes;
    of.lpstrDefExt = L"ini";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&of)) return;
    wchar_t name[96];
    const wchar_t *b = wcsrchr(file, L'\\'); b = b ? b + 1 : file;
    lstrcpynW(name, b, 96);
    wchar_t *dot = wcsrchr(name, L'.'); if (dot) *dot = 0;
    lstrcpyW(g_themeName, name);
    lstrcpyW(g_themePath, file);
    theme_save(file, name);
    settings_save_theme();
    fill_themes();
    fill_list();
}

/* ---------------------------------------------------------- editing -- */

static COLORREF g_custom[16];

static void pick_color(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= g_nrows) return;
    Row *r = &g_rows[rowIndex];
    CHOOSECOLORW cc = { sizeof cc };
    cc.hwndOwner = g_wnd;
    cc.rgbResult = row_now(r->orig);
    cc.lpCustColors = g_custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return;
    if (cc.rgbResult == row_color(r->orig)) theme_clear_entry(CUR_TBL, r->orig);
    else theme_set(CUR_TBL, r->orig, cc.rgbResult);
    autosave();
    engine_refresh();
    InvalidateRect(g_list, NULL, FALSE);
}

/* Gradient: keeps the colour the row has now as the start and asks for the
 * end colour. Applies to fills; lines and text keep the start colour. */
static void make_gradient(int dir)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0 || i >= g_nrows) { SetWindowTextW(g_status, L"Select a colour in the list first, then pick the colour its gradient ends with."); return; }
    if (g_area == AREA_NOTETEXT) { SetWindowTextW(g_status, L"Lyric text can't be a gradient - pick a solid colour by double-clicking it."); return; }
    Row *r = &g_rows[i];
    COLORREF start, c2;
    int d;
    if (!remap_grad(g_area, g_kind, r->orig, &start, &c2, &d)) { start = remap(g_area, g_kind, r->orig); c2 = start; }
    SetWindowTextW(g_status, dir == GRAD_V ? L"Pick the colour the gradient ends with at the bottom" : L"Pick the colour the gradient ends with on the right");
    CHOOSECOLORW cc = { sizeof cc };
    cc.hwndOwner = g_wnd;
    cc.rgbResult = c2;
    cc.lpCustColors = g_custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) { fill_list(); return; }
    theme_set_grad(CUR_TBL, r->orig, start, cc.rgbResult, dir);
    autosave();
    engine_refresh();
    fill_list();
}

static void reset_selected(void)
{
    for (int i = -1; (i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) >= 0;)
        theme_clear_entry(CUR_TBL, g_rows[i].orig);
    autosave();
    engine_refresh();
    InvalidateRect(g_list, NULL, FALSE);
}

static void reset_area(void)
{
    if (MessageBoxW(g_wnd, L"Remove every replacement in this area?", L"UI Colors", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    COLORREF o, r;
    while (theme_entry(CUR_TBL, 0, &o, &r)) theme_clear_entry(CUR_TBL, o);
    autosave();
    engine_refresh();
    fill_list();
}

/* ---- quick tint: HSL transform of every colour listed in this area ---- */

static void rgb_to_hsl(COLORREF c, double *h, double *s, double *l)
{
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    *l = (mx + mn) / 2;
    if (d < 1e-9) { *h = 0; *s = 0; return; }
    *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h *= 60;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static COLORREF hsl_to_rgb(double h, double s, double l)
{
    double r, g, b;
    if (s < 1e-9) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q, hh = h / 360;
        r = hue2rgb(p, q, hh + 1.0 / 3); g = hue2rgb(p, q, hh); b = hue2rgb(p, q, hh - 1.0 / 3);
    }
    return RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}

/* Greys get the tint colour mixed in (otherwise hue shift does nothing on a
 * mostly-grey UI); saturated colours are hue-rotated. */
static COLORREF tint(COLORREF c, int hueShift, int satPct, int litPct, BOOL invert)
{
    double h, s, l;
    rgb_to_hsl(c, &h, &s, &l);
    if (invert) l = 1 - l;
    if (s < 0.08) { h = fmod(220 + hueShift + 360, 360); s = 0.10 * satPct / 100.0; }
    else { h = fmod(h + hueShift + 720, 360); s = fmin(1, s * satPct / 100.0); }
    l = fmin(1, fmax(0, l * litPct / 100.0));
    return hsl_to_rgb(h, s, l);
}

static void apply_tint(void)
{
    int hue = (int)SendMessageW(g_hue, TBM_GETPOS, 0, 0) - 180;
    int sat = (int)SendMessageW(g_sat, TBM_GETPOS, 0, 0);
    int lit = (int)SendMessageW(g_lit, TBM_GETPOS, 0, 0);
    BOOL inv = Button_GetCheck(g_invert) == BST_CHECKED;
    for (int i = 0; i < g_nrows; i++) {
        COLORREF n = tint(row_color(g_rows[i].orig), hue, sat, lit, inv);
        if (n == row_color(g_rows[i].orig)) theme_clear_entry(CUR_TBL, g_rows[i].orig);
        else theme_set(CUR_TBL, g_rows[i].orig, n);
    }
    autosave();
    engine_refresh();
    InvalidateRect(g_list, NULL, FALSE);
}

/* ----------------------------------------------------------- eyedropper -- */

static void finish_pick(POINT pt)
{
    HDC sdc = GetDC(NULL);
    COLORREF px = GetPixel(sdc, pt.x, pt.y);
    ReleaseDC(NULL, sdc);
    HWND under = WindowFromPoint(pt);
    int area = engine_area_of_window(under);
    /* candidates: colours whose current replacement (or themselves) is that pixel */
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        if (pass == 1 && g_area != AREA_ALL) {   /* retry in All */
            g_area = AREA_ALL; g_kind = KIND_ANY; ComboBox_SetCurSel(g_areaCombo, AREA_ALL);
            ComboBox_SetCurSel(g_kindCombo, 0); build_rows();
        }
        for (int i = 0; i < g_nrows; i++) {
            int a = g_area == AREA_ALL ? area : g_area;
            BOOL hit = g_rows[i].orig == px;
            for (int k = 0; k < KIND_COUNT && !hit; k++) hit = remap(a, k, g_rows[i].orig) == px;
            if (hit)
                if (best < 0 || g_rows[i].uses > g_rows[best].uses) best = i;
        }
    }
    fill_list();
    if (best < 0) {
        /* rebuild may reorder; find again */
        wchar_t s[128];
        wsprintfW(s, L"Picked #%02X%02X%02X - not a colour UI Colors has seen drawn yet.", GetRValue(px), GetGValue(px), GetBValue(px));
        SetWindowTextW(g_status, s);
        return;
    }
    COLORREF want = g_rows[best].orig;
    for (int i = 0; i < g_nrows; i++) if (g_rows[i].orig == want) best = i;
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(g_list, best, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, best, FALSE);
    pick_color(best);
}

/* -------------------------------------------------------------- window -- */

enum { ID_THEME = 100, ID_KIND, ID_SAVEAS, ID_ENABLE, ID_AREA, ID_PICK, ID_RESET, ID_RESETAREA, ID_TINT, ID_LIST, ID_HUE, ID_SAT, ID_LIT, ID_INVERT, ID_STATUS,
       ID_GRADH, ID_GRADV };

static HFONT g_font;

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(UINT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void draw_swatch_cell(HDC dc, RECT rc, COLORREF c, BOOL changed, COLORREF c2, int dir)
{
    RECT sw = { rc.left + 4, rc.top + 2, rc.left + 34, rc.bottom - 2 };
    if (c2 != NO_COLOR) {
        TRIVERTEX v[2] = {
            { sw.left, sw.top, (COLOR16)(GetRValue(c) << 8), (COLOR16)(GetGValue(c) << 8), (COLOR16)(GetBValue(c) << 8), 0 },
            { sw.right, sw.bottom, (COLOR16)(GetRValue(c2) << 8), (COLOR16)(GetGValue(c2) << 8), (COLOR16)(GetBValue(c2) << 8), 0 },
        };
        GRADIENT_RECT g = { 0, 1 };
        GradientFill(dc, v, 2, &g, 1, dir == GRAD_V ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
    } else {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &sw, b);
        DeleteObject(b);
    }
    FrameRect(dc, &sw, (HBRUSH)GetStockObject(GRAY_BRUSH));
    wchar_t t[40];
    if (c2 != NO_COLOR)
        wsprintfW(t, L"#%02X%02X%02X %s #%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c), dir == GRAD_V ? L"v" : L">",
                  GetRValue(c2), GetGValue(c2), GetBValue(c2));
    else wsprintfW(t, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    RECT tr = rc; tr.left = sw.right + 6;
    SetTextColor(dc, changed ? RGB(0, 90, 200) : GetSysColor(COLOR_WINDOWTEXT));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER);
}

static LRESULT on_custom_draw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int i = (int)cd->nmcd.dwItemSpec;
        if (i >= g_nrows) return CDRF_DODEFAULT;
        Row *r = &g_rows[i];
        RECT rc;
        ListView_GetSubItemRect(g_list, i, cd->iSubItem, LVIR_BOUNDS, &rc);
        if (cd->iSubItem == 0) rc.right = rc.left + ListView_GetColumnWidth(g_list, 0);
        BOOL sel = ListView_GetItemState(g_list, i, LVIS_SELECTED) != 0;
        FillRect(cd->nmcd.hdc, &rc, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        if (cd->iSubItem == 0) draw_swatch_cell(cd->nmcd.hdc, rc, row_color(r->orig), FALSE, NO_COLOR, 0);
        else if (cd->iSubItem == 1) {
            COLORREF own = theme_lookup(CUR_TBL, r->orig), g1, g2;
            if (g_area == AREA_NOTETEXT) { draw_swatch_cell(cd->nmcd.hdc, rc, row_now(r->orig), row_now(r->orig) != lyric_default(r->orig), NO_COLOR, 0); return CDRF_SKIPDEFAULT; }
            int dir;
            if (remap_grad(g_area, g_kind, r->orig, &g1, &g2, &dir)) draw_swatch_cell(cd->nmcd.hdc, rc, g1, own != NO_COLOR, g2, dir);
            else draw_swatch_cell(cd->nmcd.hdc, rc, own == NO_COLOR ? remap(g_area, g_kind, r->orig) : own, own != NO_COLOR, NO_COLOR, 0);
        } else {
            wchar_t t[128] = L"";
            if (cd->iSubItem == 2) lstrcpynW(t, label_for(r->orig), 128);
            else if (cd->iSubItem == 3) MultiByteToWideChar(CP_ACP, 0, r->kinds, -1, t, 128);
            else if (cd->iSubItem == 4) wsprintfW(t, L"%u", r->uses);
            RECT tr = rc; tr.left += 6;
            SetTextColor(cd->nmcd.hdc, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            SetBkMode(cd->nmcd.hdc, TRANSPARENT);
            DrawTextW(cd->nmcd.hdc, t, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        return CDRF_SKIPDEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static void layout(int w, int h)
{
    SetWindowPos(g_list, NULL, 10, 72, w - 20, h - 72 - 128, SWP_NOZORDER);
    int y = h - 120;
    HWND c;
    for (c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(c);
        int row = (int)(INT_PTR)GetPropW(c, L"vh_bottom");
        if (!row) continue;
        RECT r; GetWindowRect(c, &r); MapWindowPoints(NULL, g_wnd, (POINT *)&r, 2);
        SetWindowPos(c, NULL, r.left, y + (row - 1) * 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        (void)id;
    }
    SetWindowPos(g_status, NULL, 10, h - 26, w - 20, 20, SWP_NOZORDER);
}

static HWND bottom(HWND c, int row) { SetPropW(c, L"vh_bottom", (HANDLE)(INT_PTR)row); return c; }

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_wnd = h;
        NONCLIENTMETRICSW ncm = { sizeof ncm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        mk(L"STATIC", L"Theme:", SS_CENTERIMAGE, 10, 10, 50, 24, -1);
        g_themeCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 64, 10, 300, 300, ID_THEME);
        mk(L"BUTTON", L"Save as...", BS_PUSHBUTTON | WS_TABSTOP, 372, 9, 90, 26, ID_SAVEAS);
        g_enable = mk(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, 474, 10, 90, 24, ID_ENABLE);
        mk(L"STATIC", L"Area:", SS_CENTERIMAGE, 10, 40, 50, 24, -1);
        g_areaCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 64, 40, 160, 300, ID_AREA);
        g_kindCombo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 232, 40, 120, 300, ID_KIND);
        mk(L"BUTTON", L"Eyedropper", BS_PUSHBUTTON | WS_TABSTOP, 360, 39, 100, 26, ID_PICK);
        mk(L"STATIC", L"Double-click a colour to change it. Eyedropper: click anything in VOCALOID.", SS_CENTERIMAGE, 470, 40, 420, 24, -1);
        for (int a = 0; a < AREA_COUNT; a++) ComboBox_AddString(g_areaCombo, a == AREA_NOTETEXT ? L"Note text (lyrics)" : kAreaNames[a]);
        ComboBox_SetCurSel(g_areaCombo, g_area);
        ComboBox_AddString(g_kindCombo, L"Every use");
        ComboBox_AddString(g_kindCombo, L"Fills only");
        ComboBox_AddString(g_kindCombo, L"Lines only");
        ComboBox_AddString(g_kindCombo, L"Text only");
        ComboBox_SetCurSel(g_kindCombo, g_kind);
        Button_SetCheck(g_enable, engine_enabled() ? BST_CHECKED : BST_UNCHECKED);

        g_list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 10, 72, 600, 300, ID_LIST);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t *cols[] = { L"Original", L"Now", L"What it is", L"Drawn as", L"Uses" };
        int widths[] = { 120, 190, 250, 110, 70 };
        for (int i = 0; i < 5; i++) {
            LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH, 0, widths[i], (wchar_t *)cols[i] };
            ListView_InsertColumn(g_list, i, &c);
        }

        bottom(mk(L"BUTTON", L"Reset selected", BS_PUSHBUTTON, 10, 0, 120, 26, ID_RESET), 1);
        bottom(mk(L"BUTTON", L"Reset area", BS_PUSHBUTTON, 138, 0, 100, 26, ID_RESETAREA), 1);
        bottom(mk(L"BUTTON", L"Gradient \x2192 ...", BS_PUSHBUTTON, 256, 0, 110, 26, ID_GRADH), 1);
        bottom(mk(L"BUTTON", L"Gradient \x2193 ...", BS_PUSHBUTTON, 372, 0, 110, 26, ID_GRADV), 1);
        bottom(mk(L"STATIC", L"Gradients colour fills: select a colour, then pick where it fades to.", SS_CENTERIMAGE, 492, 0, 400, 24, -1), 1);
        bottom(mk(L"STATIC", L"Quick tint (applies to every colour listed):", SS_CENTERIMAGE, 10, 0, 300, 24, -1), 2);
        bottom(mk(L"STATIC", L"Hue", SS_CENTERIMAGE, 10, 0, 34, 24, -1), 3);
        g_hue = bottom(mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS, 44, 0, 150, 26, ID_HUE), 3);
        bottom(mk(L"STATIC", L"Saturation", SS_CENTERIMAGE, 200, 0, 66, 24, -1), 3);
        g_sat = bottom(mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS, 266, 0, 130, 26, ID_SAT), 3);
        bottom(mk(L"STATIC", L"Brightness", SS_CENTERIMAGE, 402, 0, 70, 24, -1), 3);
        g_lit = bottom(mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS, 472, 0, 130, 26, ID_LIT), 3);
        g_invert = bottom(mk(L"BUTTON", L"Invert light/dark", BS_AUTOCHECKBOX, 610, 0, 130, 24, ID_INVERT), 3);
        bottom(mk(L"BUTTON", L"Apply tint", BS_PUSHBUTTON, 744, 0, 90, 26, ID_TINT), 3);
        SendMessageW(g_hue, TBM_SETRANGE, TRUE, MAKELPARAM(0, 360)); SendMessageW(g_hue, TBM_SETPOS, TRUE, 180);
        SendMessageW(g_sat, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200)); SendMessageW(g_sat, TBM_SETPOS, TRUE, 100);
        SendMessageW(g_lit, TBM_SETRANGE, TRUE, MAKELPARAM(20, 180)); SendMessageW(g_lit, TBM_SETPOS, TRUE, 100);
        g_status = mk(L"STATIC", L"", SS_LEFTNOWORDWRAP, 10, 0, 600, 20, ID_STATUS);
        fill_themes();
        fill_list();
        return 0;
    }
    case WM_SIZE:
        layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 860;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 420;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_THEME: if (HIWORD(wp) == CBN_SELCHANGE) select_theme(ComboBox_GetCurSel(g_themeCombo)); break;
        case ID_AREA: if (HIWORD(wp) == CBN_SELCHANGE) { g_area = ComboBox_GetCurSel(g_areaCombo); fill_list(); } break;
        case ID_KIND: if (HIWORD(wp) == CBN_SELCHANGE) { g_kind = ComboBox_GetCurSel(g_kindCombo); fill_list(); } break;
        case ID_SAVEAS: save_as(); break;
        case ID_ENABLE: engine_set_enabled(Button_GetCheck(g_enable) == BST_CHECKED); break;
        case ID_RESET: reset_selected(); break;
        case ID_RESETAREA: reset_area(); break;
        case ID_TINT: apply_tint(); break;
        case ID_GRADH: make_gradient(GRAD_H); break;
        case ID_GRADV: make_gradient(GRAD_V); break;
        case ID_PICK:
            g_picking = TRUE;
            SetCapture(h);
            SetCursor(LoadCursor(NULL, IDC_CROSS));
            SetWindowTextW(g_status, L"Eyedropper: click any part of the VOCALOID window (Esc cancels)");
            break;
        }
        return 0;
    case WM_SETCURSOR:
        if (g_picking) { SetCursor(LoadCursor(NULL, IDC_CROSS)); return TRUE; }
        break;
    case WM_LBUTTONDOWN:
        if (g_picking) {
            g_picking = FALSE;
            ReleaseCapture();
            POINT pt; GetCursorPos(&pt);
            finish_pick(pt);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && g_picking) { g_picking = FALSE; ReleaseCapture(); fill_list(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        g_picking = FALSE;
        break;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_LIST) {
            if (n->code == NM_CUSTOMDRAW) return on_custom_draw((NMLVCUSTOMDRAW *)lp);
            if (n->code == NM_DBLCLK) pick_color(((NMITEMACTIVATE *)lp)->iItem);
            if (n->code == LVN_KEYDOWN) {
                WORD k = ((NMLVKEYDOWN *)lp)->wVKey;
                if (k == VK_DELETE) reset_selected();
                if (k == VK_RETURN) pick_color(ListView_GetNextItem(g_list, -1, LVNI_FOCUSED));
            }
        }
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_list) InvalidateRect(g_list, NULL, FALSE);
        break;
    case WM_DESTROY:
        g_wnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void editor_open(HWND owner)
{
    if (g_wnd) { ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); fill_list(); return; }
    static BOOL registered;
    if (!registered) {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"VocahansmentsUIColors";
        RegisterClassW(&wc);
        registered = TRUE;
    }
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"VocahansmentsUIColors", L"UI Colors - Vocahansments",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                             owner, NULL, GetModuleHandleW(NULL), NULL);
    ShowWindow(w, SW_SHOW);
}
