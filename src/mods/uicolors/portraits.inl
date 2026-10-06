/*
 * Voicebank Portraits: a full-body picture of the part's voicebank in the
 * piano roll, like Synthesizer V. It is drawn behind the notes - just before
 * the editor draws the first note of a repaint (or at the end of a repaint
 * with no notes) - faded to the chosen opacity, standing on the bottom edge.
 *
 * Which picture, for the voicebank named in the MUSICAL EDITOR title:
 *   1. the one chosen in Mods > Voicebank Portraits...   ([Portraits] in settings.ini)
 *   2. portrait.png / portrait.jpg / <name>.png next to the voicebank's .vvd
 *   3. %APPDATA%\Vocahansments\portraits\<name>.png (or .jpg)
 * PNG transparency is kept (loaded with GDI+).
 *
 * Included by engine.c (an .inl so the build does not compile it on its own).
 */

#include <commdlg.h>
#include <windowsx.h>
#include "../../common/vh_util.h"

extern wchar_t g_ini[];                 /* uicolors.c */

static BOOL g_ptOn = TRUE;
static int g_ptOpacity = 35;            /* % */
static BOOL g_ptLeft;                   /* stand on the left instead of the right */
static int g_ptScale = 100, g_ptOffX, g_ptOffY;   /* the current voicebank's layout: % of the roll's height, % offsets */

/* per voicebank: [PortraitLayout] <name>=scale,x,y */
static void layout_get(const wchar_t *name, int *scale, int *x, int *y)
{
    wchar_t v[64];
    *scale = 100; *x = 0; *y = 0;
    if (!name[0] || !GetPrivateProfileStringW(L"PortraitLayout", name, L"", v, 64, g_ini)) return;
    int n[3] = { 100, 0, 0 }, k = 0, sign = 1, have = 0;
    n[0] = 0;
    for (const wchar_t *p = v;; p++) {
        if (*p == L'-') sign = -1;
        else if (*p >= L'0' && *p <= L'9') { n[k] = n[k] * 10 + (*p - L'0'); have = 1; }
        else {
            n[k] *= sign;
            sign = 1;
            if (!*p || ++k >= 3) break;
        }
    }
    if (!have) return;
    *scale = n[0] < 20 ? 20 : (n[0] > 300 ? 300 : n[0]);
    *x = n[1] < -100 ? -100 : (n[1] > 100 ? 100 : n[1]);
    *y = n[2] < -100 ? -100 : (n[2] > 100 ? 100 : n[2]);
}

static void layout_set(const wchar_t *name, int scale, int x, int y)
{
    wchar_t v[64];
    if (!name[0]) return;
    wsprintfW(v, L"%d,%d,%d", scale, x, y);
    WritePrivateProfileStringW(L"PortraitLayout", name, scale == 100 && !x && !y ? NULL : v, g_ini);
}

/* ---------------------------------------------------- GDI+ (flat API) -- */

typedef struct { UINT32 version; void *debug; BOOL noThread, noCodecs; } GpStartup;
typedef struct { UINT w, h; INT stride; INT fmt; void *scan0; UINT_PTR reserved; } GpBitmapData;
typedef struct { INT x, y, w, h; } GpRectI;
static int (WINAPI *pGdiplusStartup)(ULONG_PTR *, const GpStartup *, void *);
static int (WINAPI *pGdipCreateBitmapFromFile)(const WCHAR *, void **);
static int (WINAPI *pGdipGetImageWidth)(void *, UINT *);
static int (WINAPI *pGdipGetImageHeight)(void *, UINT *);
static int (WINAPI *pGdipBitmapLockBits)(void *, const GpRectI *, UINT, INT, GpBitmapData *);
static int (WINAPI *pGdipBitmapUnlockBits)(void *, GpBitmapData *);
static int (WINAPI *pGdipDisposeImage)(void *);

static BOOL gdiplus_ready(void)
{
    static int state;                    /* 0 untried, 1 ok, -1 failed */
    if (state) return state > 0;
    state = -1;
    HMODULE g = LoadLibraryW(L"gdiplus.dll");
    if (!g) return FALSE;
    pGdiplusStartup = (void *)GetProcAddress(g, "GdiplusStartup");
    pGdipCreateBitmapFromFile = (void *)GetProcAddress(g, "GdipCreateBitmapFromFile");
    pGdipGetImageWidth = (void *)GetProcAddress(g, "GdipGetImageWidth");
    pGdipGetImageHeight = (void *)GetProcAddress(g, "GdipGetImageHeight");
    pGdipBitmapLockBits = (void *)GetProcAddress(g, "GdipBitmapLockBits");
    pGdipBitmapUnlockBits = (void *)GetProcAddress(g, "GdipBitmapUnlockBits");
    pGdipDisposeImage = (void *)GetProcAddress(g, "GdipDisposeImage");
    if (!pGdiplusStartup || !pGdipCreateBitmapFromFile || !pGdipGetImageWidth || !pGdipGetImageHeight ||
        !pGdipBitmapLockBits || !pGdipBitmapUnlockBits || !pGdipDisposeImage) return FALSE;
    ULONG_PTR token;
    GpStartup in = { 1, NULL, FALSE, FALSE };
    if (pGdiplusStartup(&token, &in, NULL) != 0) return FALSE;
    state = 1;
    return TRUE;
}

/* the picture's pixels, premultiplied BGRA, top-down; caller frees */
static DWORD *load_image(const wchar_t *path, int *w, int *h)
{
    if (!gdiplus_ready()) return NULL;
    void *bmp = NULL;
    if (pGdipCreateBitmapFromFile(path, &bmp) != 0 || !bmp) return NULL;
    UINT iw = 0, ih = 0;
    pGdipGetImageWidth(bmp, &iw);
    pGdipGetImageHeight(bmp, &ih);
    DWORD *px = NULL;
    if (iw && ih && iw <= 8192 && ih <= 8192) {
        GpRectI r = { 0, 0, (INT)iw, (INT)ih };
        GpBitmapData d;
        if (pGdipBitmapLockBits(bmp, &r, 1 /* read */, 0x000E200B /* 32bpp PARGB */, &d) == 0) {
            px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)iw * ih * 4);
            if (px) for (UINT y = 0; y < ih; y++) memcpy(px + (SIZE_T)y * iw, (BYTE *)d.scan0 + (SSIZE_T)y * d.stride, iw * 4);
            pGdipBitmapUnlockBits(bmp, &d);
        }
    }
    pGdipDisposeImage(bmp);
    if (px) { *w = (int)iw; *h = (int)ih; }
    return px;
}

/* ------------------------------------------------- finding a picture -- */

static void voice_name(wchar_t *out, int max)
{
    /* "MUSICAL EDITOR - Lead [MIKU_V4_English]" */
    static HWND frame;
    out[0] = 0;
    wchar_t t[256];
    if (!frame || !IsWindow(frame) || !GetWindowTextW(frame, t, 256) || wcsncmp(t, L"MUSICAL EDITOR", 14)) {
        frame = NULL;
        HWND main = A->GetMainWindow();
        for (HWND c = main ? FindWindowExW(main, NULL, NULL, NULL) : NULL; c && !frame; c = FindWindowExW(main, c, NULL, NULL))
            for (HWND d = FindWindowExW(c, NULL, NULL, NULL); d && !frame; d = FindWindowExW(c, d, NULL, NULL))
                if (GetWindowTextW(d, t, 256) && !wcsncmp(t, L"MUSICAL EDITOR", 14)) frame = d;
        if (!frame || !GetWindowTextW(frame, t, 256)) return;
    }
    wchar_t *open = wcsrchr(t, L'['), *close = open ? wcschr(open, L']') : NULL;
    if (!open || !close || close - open - 1 >= max) return;
    lstrcpynW(out, open + 1, (int)(close - open));
}

static BOOL file_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

/* the folder holding <name>.vvd under the usual VoiceDB roots */
static BOOL voicebank_dir(const wchar_t *name, wchar_t *out)
{
    static const wchar_t *const roots[] = { L"C:\\Program Files (x86)\\VoiceDB", L"C:\\Program Files\\VoiceDB" };
    for (int r = 0; r < 2; r++) {
        wchar_t pat[MAX_PATH];
        WIN32_FIND_DATAW g, i;
        wsprintfW(pat, L"%s\\*", roots[r]);
        HANDLE hg = FindFirstFileW(pat, &g);
        if (hg == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(g.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || g.cFileName[0] == L'.') continue;
            wsprintfW(pat, L"%s\\%s\\*", roots[r], g.cFileName);
            HANDLE hi = FindFirstFileW(pat, &i);
            if (hi == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || i.cFileName[0] == L'.') continue;
                wchar_t vvd[MAX_PATH];
                wsprintfW(vvd, L"%s\\%s\\%s\\%s.vvd", roots[r], g.cFileName, i.cFileName, name);
                if (file_exists(vvd)) {
                    wsprintfW(out, L"%s\\%s\\%s", roots[r], g.cFileName, i.cFileName);
                    FindClose(hi); FindClose(hg);
                    return TRUE;
                }
            } while (FindNextFileW(hi, &i));
            FindClose(hi);
        } while (FindNextFileW(hg, &g));
        FindClose(hg);
    }
    return FALSE;
}

/* the picture for a voicebank; FALSE if there is none */
static BOOL portrait_path(const wchar_t *name, wchar_t *out)
{
    out[0] = 0;
    if (!name[0]) return FALSE;
    GetPrivateProfileStringW(L"Portraits", name, L"", out, MAX_PATH, g_ini);
    if (out[0] && file_exists(out)) return TRUE;
    wchar_t dir[MAX_PATH];
    if (voicebank_dir(name, dir)) {
        const wchar_t *const tries[] = { L"portrait.png", L"portrait.jpg", L"portrait.jpeg", NULL };
        for (int i = 0; tries[i]; i++) { wsprintfW(out, L"%s\\%s", dir, tries[i]); if (file_exists(out)) return TRUE; }
        wsprintfW(out, L"%s\\%s.png", dir, name); if (file_exists(out)) return TRUE;
        wsprintfW(out, L"%s\\%s.jpg", dir, name); if (file_exists(out)) return TRUE;
    }
    wchar_t app[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
    wsprintfW(out, L"%s\\Vocahansments\\portraits\\%s.png", app, name); if (file_exists(out)) return TRUE;
    wsprintfW(out, L"%s\\Vocahansments\\portraits\\%s.jpg", app, name); if (file_exists(out)) return TRUE;
    out[0] = 0;
    return FALSE;
}

/* ------------------------------------------------------------ cache -- */
/* the current voicebank's picture, scaled to the roll's height (area average) */

static wchar_t g_ptName[128], g_ptFile[MAX_PATH];
static DWORD *g_ptSrc;
static int g_ptSrcW, g_ptSrcH;
static HBITMAP g_ptBmp;
static HDC g_ptDC;
static int g_ptW, g_ptH;
static DWORD g_ptChecked;               /* last look for a picture (they can be added any time) */

static void portrait_forget(void)
{
    if (g_ptSrc) HeapFree(GetProcessHeap(), 0, g_ptSrc);
    g_ptSrc = NULL;
    if (g_ptDC) { DeleteDC(g_ptDC); g_ptDC = NULL; }
    if (g_ptBmp) { o_DeleteObject(g_ptBmp); g_ptBmp = NULL; }
    g_ptW = g_ptH = 0;
    g_ptFile[0] = 0;
}

/* the part's voicebank, its picture and its layout, looked up again now and then */
static void portrait_current(void)
{
    wchar_t name[128], file[MAX_PATH];
    voice_name(name, 128);
    if (!name[0] && g_ptName[0]) lstrcpyW(name, g_ptName);    /* the title can be blank for a moment: keep the picture */
    if (lstrcmpW(name, g_ptName) || GetTickCount() - g_ptChecked > 3000) {
        g_ptChecked = GetTickCount();
        portrait_path(name, file);
        if (lstrcmpW(name, g_ptName) || lstrcmpiW(file, g_ptFile)) {
            portrait_forget();
            lstrcpynW(g_ptName, name, 128);
            if (file[0] && (g_ptSrc = load_image(file, &g_ptSrcW, &g_ptSrcH)) != NULL) lstrcpyW(g_ptFile, file);
        }
        layout_get(g_ptName, &g_ptScale, &g_ptOffX, &g_ptOffY);
    }
}

static BOOL portrait_scaled(int targetH)
{
    if (!g_ptSrc || targetH < 8) return FALSE;
    if (g_ptH == targetH && g_ptDC) return TRUE;
    int th = targetH, tw = (int)((double)g_ptSrcW * th / g_ptSrcH + 0.5);
    if (tw < 1) return FALSE;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), tw, -th, 1, 32, BI_RGB } };
    DWORD *dst;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
    if (!b) return FALSE;
    for (int y = 0; y < th; y++) {
        int sy0 = y * g_ptSrcH / th, sy1 = (y + 1) * g_ptSrcH / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < tw; x++) {
            int sx0 = x * g_ptSrcW / tw, sx1 = (x + 1) * g_ptSrcW / tw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned s[4] = { 0, 0, 0, 0 }, n = 0;
            for (int sy = sy0; sy < sy1 && sy < g_ptSrcH; sy++)
                for (int sx = sx0; sx < sx1 && sx < g_ptSrcW; sx++, n++) {
                    DWORD p = g_ptSrc[(SIZE_T)sy * g_ptSrcW + sx];
                    s[0] += p & 0xFF; s[1] += (p >> 8) & 0xFF; s[2] += (p >> 16) & 0xFF; s[3] += p >> 24;
                }
            if (!n) n = 1;
            dst[(SIZE_T)y * tw + x] = (s[0] / n) | (s[1] / n) << 8 | (s[2] / n) << 16 | (s[3] / n) << 24;
        }
    }
    if (g_ptDC) DeleteDC(g_ptDC);
    if (g_ptBmp) o_DeleteObject(g_ptBmp);
    g_ptBmp = b;
    g_ptDC = CreateCompatibleDC(NULL);
    o_SelectObject(g_ptDC, g_ptBmp);
    g_ptW = tw; g_ptH = th;
    return TRUE;
}

/* --------------------------------------------------------- drawing -- */

/* a portrait is (or was just) on screen: scrolling must repaint, not shift */
static BOOL portrait_showing(void) { return g_ptOn && g_ptSrc != NULL; }

/* The editor sometimes redraws part of the roll straight away, outside a
 * repaint (clicking a note, for one), over the portrait. When that happens a
 * moment later the roll is repainted, which brings the portrait back. */
#define PT_FIX_TIMER 0x50544658   /* 'PTFX' */
static VOID CALLBACK portrait_fix(HWND h, UINT m, UINT_PTR id, DWORD t)
{
    (void)m; (void)t;
    KillTimer(h, id);
    RECT pane;
    if (phys_pane_of(h, &pane)) RedrawWindow(h, &pane, NULL, RDW_INVALIDATE | RDW_ERASE);
}

static void portrait_outside_paint(HDC dc)
{
    if (!portrait_showing() || (g_tlsPaint != TLS_OUT_OF_INDEXES && TlsGetValue(g_tlsPaint))) return;
    HWND h = WindowFromDC(dc);
    if (!h) h = memdc_owner(dc);
    RECT pane;
    if (h && phys_pane_of(h, &pane)) SetTimer(h, PT_FIX_TIMER, 40, portrait_fix);
}

/* repaint the piano roll only (live slider changes) */
static void portrait_repaint(void)
{
    RECT pane;
    for (int i = 0; i < 8; i++)
        if (g_rbWnd[i] && IsWindow(g_rbWnd[i]) && phys_pane_of(g_rbWnd[i], &pane))
            RedrawWindow(g_rbWnd[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static HDC g_ptPaintDC;                 /* what the editor draws that repaint into */
static HWND g_ptPaintWnd;               /* the window being painted, its update area */
static RECT g_ptPaintRect;
static BOOL g_ptDone;                   /* drawn in this repaint */

static void portrait_begin(HWND h, HDC dc, const RECT *rcPaint) { g_ptPaintWnd = h; g_ptPaintDC = dc; g_ptPaintRect = *rcPaint; g_ptDone = FALSE; }

/* draw into dc (which shows window k), behind whatever comes next */
static void portrait_draw(HDC dc, int k)
{
    /* not into a picture of its own the editor might be drawing a note into */
    if (!g_ptOn || g_ptDone || k < 0 || g_rbWnd[k] != g_ptPaintWnd || dc != g_ptPaintDC) return;
    g_ptDone = TRUE;
    RECT pane, clip;
    /* only ever the piano roll (Pitch Tools knows its note area), never the track editor */
    if (!phys_pane_of(g_rbWnd[k], &pane) || !IntersectRect(&clip, &pane, &g_ptPaintRect)) return;
    portrait_current();
    int paneW = pane.right - pane.left, paneH = pane.bottom - pane.top;
    if (!portrait_scaled(paneH * g_ptScale / 100)) return;
    int x = (g_ptLeft ? pane.left + 10 : pane.right - g_ptW - 10) + paneW * g_ptOffX / 100;
    int y = pane.bottom - g_ptH + paneH * g_ptOffY / 100;
    /* whatever clipping and origin the editor set up for its first note is not ours */
    int save = SaveDC(dc);
    SetViewportOrgEx(dc, 0, 0, NULL);
    SetWindowOrgEx(dc, 0, 0, NULL);
    SelectClipRgn(dc, NULL);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(g_ptOpacity * 255 / 100), AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, g_ptW, g_ptH, g_ptDC, 0, 0, g_ptW, g_ptH, bf);
    RestoreDC(dc, save);
}

/* ----------------------------------------------------------- window -- */
/* Mods > Voicebank Portraits...: pick a picture for each voicebank */

static HWND g_ptWnd, g_ptList, g_ptStatus, g_ptShow, g_ptSlider, g_ptOpLabel, g_ptSide, g_ptLay[3], g_ptLayLabel[3];
static HFONT g_ptFont;
enum { PT_LIST = 600, PT_CHOOSE, PT_CLEAR, PT_SHOW, PT_SLIDER, PT_SIDE, PT_RESET, PT_LAYOUT = 620 };

static void portrait_refresh(void)
{
    g_ptChecked = 0;
    g_ptName[0] = 0;                     /* look again */
    engine_refresh();
}

static void pt_save(void)
{
    vh_ini_set_int(g_ini, L"PortraitOptions", L"Enabled", g_ptOn);
    vh_ini_set_int(g_ini, L"PortraitOptions", L"Opacity", g_ptOpacity);
    vh_ini_set_int(g_ini, L"PortraitOptions", L"Left", g_ptLeft);
}

static void pt_layout_labels(void)
{
    for (int i = 0; i < 3; i++) {
        wchar_t t[16];
        wsprintfW(t, L"%d %%", (int)SendMessageW(g_ptLay[i], TBM_GETPOS, 0, 0));
        SetWindowTextW(g_ptLayLabel[i], t);
    }
}

/* the position sliders show the selected voicebank's layout */
static void pt_layout_load(void)
{
    int i = ListBox_GetCurSel(g_ptList), sc = 100, x = 0, y = 0;
    wchar_t name[128] = L"";
    if (i >= 0) ListBox_GetText(g_ptList, i, name);
    layout_get(name, &sc, &x, &y);
    SendMessageW(g_ptLay[0], TBM_SETPOS, TRUE, sc);
    SendMessageW(g_ptLay[1], TBM_SETPOS, TRUE, x);
    SendMessageW(g_ptLay[2], TBM_SETPOS, TRUE, -y);          /* right on the slider = up */
    pt_layout_labels();
}

static void pt_layout_store(void)
{
    int i = ListBox_GetCurSel(g_ptList);
    if (i < 0) return;
    wchar_t name[128];
    ListBox_GetText(g_ptList, i, name);
    int sc = (int)SendMessageW(g_ptLay[0], TBM_GETPOS, 0, 0), x = (int)SendMessageW(g_ptLay[1], TBM_GETPOS, 0, 0);
    int y = -(int)SendMessageW(g_ptLay[2], TBM_GETPOS, 0, 0);
    layout_set(name, sc, x, y);
    if (!lstrcmpW(name, g_ptName)) { g_ptScale = sc; g_ptOffX = x; g_ptOffY = y; }
    pt_layout_labels();
    portrait_repaint();
}

static void pt_show_selection(void)
{
    pt_layout_load();
    int i = ListBox_GetCurSel(g_ptList);
    wchar_t name[128] = L"", file[MAX_PATH], t[MAX_PATH + 64];
    if (i < 0) { SetWindowTextW(g_ptStatus, L"Pick a voicebank."); return; }
    ListBox_GetText(g_ptList, i, name);
    if (portrait_path(name, file)) wsprintfW(t, L"Picture: %s", file);
    else wsprintfW(t, L"No picture yet. Choose one, or put portrait.png next to its .vvd.");
    SetWindowTextW(g_ptStatus, t);
}

static void pt_fill_list(void)
{
    ListBox_ResetContent(g_ptList);
    /* the editor's own list of installed voicebanks */
    for (int n = 1; n < 100; n++) {
        wchar_t key[96], name[128];
        wsprintfW(key, L"SOFTWARE\\VOCALOID4\\VVOICE_UCONF\\vVoice_%d", n);
        DWORD sz = sizeof name;
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"name", RRF_RT_REG_SZ, NULL, name, &sz) != ERROR_SUCCESS) break;
        ListBox_AddString(g_ptList, name);
    }
    wchar_t cur[128];
    voice_name(cur, 128);
    int sel = cur[0] ? ListBox_FindStringExact(g_ptList, -1, cur) : -1;
    ListBox_SetCurSel(g_ptList, sel >= 0 ? sel : 0);
    pt_show_selection();
}

static void pt_choose(void)
{
    int i = ListBox_GetCurSel(g_ptList);
    if (i < 0) return;
    wchar_t name[128], file[MAX_PATH] = L"";
    ListBox_GetText(g_ptList, i, name);
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = g_ptWnd;
    of.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    WritePrivateProfileStringW(L"Portraits", name, file, g_ini);
    pt_show_selection();
    portrait_refresh();
}

static void pt_clear(void)
{
    int i = ListBox_GetCurSel(g_ptList);
    if (i < 0) return;
    wchar_t name[128];
    ListBox_GetText(g_ptList, i, name);
    WritePrivateProfileStringW(L"Portraits", name, NULL, g_ini);
    pt_show_selection();
    portrait_refresh();
}

static HWND pt_mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_ptWnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_ptFont, TRUE);
    return c;
}

static void pt_opacity_label(void)
{
    wchar_t t[16];
    wsprintfW(t, L"%d %%", g_ptOpacity);
    SetWindowTextW(g_ptOpLabel, t);
}

static LRESULT CALLBACK pt_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_ptWnd = h;
        NONCLIENTMETRICSW ncm = { sizeof ncm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        g_ptFont = CreateFontIndirectW(&ncm.lfMessageFont);
        pt_mk(L"STATIC", L"Voicebank:", 0, 12, 12, 200, 18, -1);
        g_ptList = pt_mk(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 12, 32, 220, 180, PT_LIST);
        pt_mk(L"BUTTON", L"Choose image...", BS_PUSHBUTTON | WS_TABSTOP, 244, 32, 130, 28, PT_CHOOSE);
        pt_mk(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, 244, 66, 130, 28, PT_CLEAR);
        g_ptShow = pt_mk(L"BUTTON", L"Show portraits", BS_AUTOCHECKBOX | WS_TABSTOP, 244, 110, 140, 22, PT_SHOW);
        pt_mk(L"STATIC", L"Opacity", 0, 244, 140, 60, 18, -1);
        g_ptOpLabel = pt_mk(L"STATIC", L"", 0, 330, 140, 50, 18, -1);
        g_ptSlider = pt_mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 240, 158, 140, 26, PT_SLIDER);
        g_ptSide = pt_mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 244, 190, 130, 100, PT_SIDE);
        g_ptStatus = pt_mk(L"STATIC", L"", SS_PATHELLIPSIS, 12, 220, 370, 36, -1);
        static const wchar_t *const lay[3] = { L"Size", L"Left / right", L"Down / up" };
        static const int lo[3] = { 20, -100, -100 }, hi[3] = { 300, 100, 100 };
        pt_mk(L"STATIC", L"Where the selected voicebank's picture stands:", 0, 12, 262, 370, 18, -1);
        for (int i = 0; i < 3; i++) {
            pt_mk(L"STATIC", lay[i], SS_CENTERIMAGE, 12, 284 + i * 30, 90, 26, -1);
            g_ptLay[i] = pt_mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, 100, 284 + i * 30, 220, 26, PT_LAYOUT + i);
            SendMessageW(g_ptLay[i], TBM_SETRANGE, TRUE, MAKELPARAM(lo[i], hi[i]));
            g_ptLayLabel[i] = pt_mk(L"STATIC", L"", SS_CENTERIMAGE, 326, 284 + i * 30, 60, 26, -1);
        }
        pt_mk(L"BUTTON", L"Reset position", BS_PUSHBUTTON | WS_TABSTOP, 12, 378, 120, 28, PT_RESET);
        ComboBox_AddString(g_ptSide, L"Stand on the right");
        ComboBox_AddString(g_ptSide, L"Stand on the left");
        ComboBox_SetCurSel(g_ptSide, g_ptLeft ? 1 : 0);
        SendMessageW(g_ptSlider, TBM_SETRANGE, TRUE, MAKELPARAM(5, 100));
        SendMessageW(g_ptSlider, TBM_SETPOS, TRUE, g_ptOpacity);
        Button_SetCheck(g_ptShow, g_ptOn ? BST_CHECKED : BST_UNCHECKED);
        pt_opacity_label();
        pt_fill_list();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case PT_LIST: if (HIWORD(wp) == LBN_SELCHANGE) pt_show_selection(); else if (HIWORD(wp) == LBN_DBLCLK) pt_choose(); break;
        case PT_CHOOSE: pt_choose(); break;
        case PT_CLEAR: pt_clear(); break;
        case PT_SHOW: g_ptOn = Button_GetCheck(g_ptShow) == BST_CHECKED; pt_save(); engine_refresh(); break;
        case PT_SIDE: if (HIWORD(wp) == CBN_SELCHANGE) { g_ptLeft = ComboBox_GetCurSel(g_ptSide) == 1; pt_save(); portrait_repaint(); } break;
        case PT_RESET:
            SendMessageW(g_ptLay[0], TBM_SETPOS, TRUE, 100);
            SendMessageW(g_ptLay[1], TBM_SETPOS, TRUE, 0);
            SendMessageW(g_ptLay[2], TBM_SETPOS, TRUE, 0);
            pt_layout_store();
            break;
        }
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_ptSlider) {
            g_ptOpacity = (int)SendMessageW(g_ptSlider, TBM_GETPOS, 0, 0);
            pt_opacity_label();
            pt_save();
            portrait_repaint();
        }
        for (int i = 0; i < 3; i++) if ((HWND)lp == g_ptLay[i]) pt_layout_store();
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_ptWnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void portraits_open(void)
{
    if (g_ptWnd) { ShowWindow(g_ptWnd, SW_RESTORE); SetForegroundWindow(g_ptWnd); pt_fill_list(); return; }
    static BOOL reg;
    if (!reg) {
        INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = pt_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"VocahansmentsPortraits";
        RegisterClassW(&wc);
        reg = TRUE;
    }
    RECT rc = { 0, 0, 396, 418 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOOLWINDOW);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"VocahansmentsPortraits", L"Voicebank Portraits - Vocahansments", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, A->GetMainWindow(), NULL, GetModuleHandleW(NULL), NULL);
    ShowWindow(w, SW_SHOW);
}

void portraits_load(void)
{
    g_ptOn = vh_ini_int(g_ini, L"PortraitOptions", L"Enabled", 1) != 0;
    g_ptOpacity = vh_ini_int(g_ini, L"PortraitOptions", L"Opacity", 35);
    if (g_ptOpacity < 5 || g_ptOpacity > 100) g_ptOpacity = 35;
    g_ptLeft = vh_ini_int(g_ini, L"PortraitOptions", L"Left", 0) != 0;
}
