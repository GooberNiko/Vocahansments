/*
 * UI Colors engine: every colour the editor hands to GDI passes through
 * remap(area, colour). Brushes and pens are tracked by handle so a colour can
 * be overridden per editor area at the moment it is used, and the editor's
 * built-in bitmaps are re-tinted in place (original pixels are kept, so theme
 * changes apply live).
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <commctrl.h>
#include <psapi.h>
#include "physics.h"
#include "engine.h"

const wchar_t *const kAreaNames[AREA_COUNT] = {
    L"All", L"Musical", L"Track", L"Mixer", L"Bars", L"Other", L"Bitmaps", L"Sprites", L"NoteText",
};
const wchar_t *const kKindNames[KIND_COUNT] = { L"", L"Fill", L"Lines", L"Text" };

static BOOL g_enabled = TRUE;
static CRITICAL_SECTION g_lock;

/* ------------------------------------------------------------- theme -- */

#define THEME_SLOTS 2048   /* per area, open addressing */
/* key: orig | 0x01000000, 0 = empty; grad: second colour | (direction + 1) << 24, 0 = solid */
typedef struct { COLORREF key, val, grad; } ThemeSlot;
static ThemeSlot g_theme[TBL_COUNT][THEME_SLOTS];
static int g_themeCount[TBL_COUNT];

static unsigned hash32(DWORD x)
{
    x ^= x >> 16; x *= 0x7FEB352D; x ^= x >> 15; x *= 0x846CA68B; x ^= x >> 16;
    return x;
}

static ThemeSlot *theme_slot(int area, COLORREF orig)
{
    DWORD key = (orig & 0xFFFFFF) | 0x01000000;
    ThemeSlot *t = g_theme[area];
    for (unsigned i = hash32(key), n = 0; n < THEME_SLOTS; i++, n++) {
        ThemeSlot *s = &t[i & (THEME_SLOTS - 1)];
        if (s->key == key) return s;
        if (!s->key) return NULL;
    }
    return NULL;
}

COLORREF theme_lookup(int area, COLORREF orig)
{
    ThemeSlot *s = theme_slot(area, orig);
    return s ? s->val : NO_COLOR;
}

BOOL theme_lookup_grad(int area, COLORREF orig, COLORREF *c2, int *dir)
{
    ThemeSlot *s = theme_slot(area, orig);
    if (!s || !s->grad) return FALSE;
    *c2 = s->grad & 0xFFFFFF;
    *dir = (int)(s->grad >> 24) - 1;
    return TRUE;
}

void theme_set_grad(int area, COLORREF orig, COLORREF repl, COLORREF c2, int dir)
{
    DWORD key = (orig & 0xFFFFFF) | 0x01000000;
    ThemeSlot *t = g_theme[area];
    for (unsigned i = hash32(key), n = 0; n < THEME_SLOTS; i++, n++) {
        ThemeSlot *s = &t[i & (THEME_SLOTS - 1)];
        if (s->key == key || !s->key) {
            if (!s->key) { if (g_themeCount[area] >= THEME_SLOTS * 3 / 4) return; g_themeCount[area]++; }
            s->key = key; s->val = repl & 0xFFFFFF;
            s->grad = c2 == NO_COLOR || dir < 0 ? 0 : (c2 & 0xFFFFFF) | (DWORD)(dir + 1) << 24;
            return;
        }
    }
}

void theme_set(int area, COLORREF orig, COLORREF repl) { theme_set_grad(area, orig, repl, NO_COLOR, -1); }

/* open addressing without tombstones: rebuild the area table */
void theme_clear_entry(int area, COLORREF orig)
{
    static ThemeSlot tmp[THEME_SLOTS];
    memcpy(tmp, g_theme[area], sizeof tmp);
    memset(g_theme[area], 0, sizeof tmp);
    g_themeCount[area] = 0;
    DWORD key = (orig & 0xFFFFFF) | 0x01000000;
    for (int i = 0; i < THEME_SLOTS; i++)
        if (tmp[i].key && tmp[i].key != key) {
            theme_set(area, tmp[i].key & 0xFFFFFF, tmp[i].val);
            ThemeSlot *s = theme_slot(area, tmp[i].key & 0xFFFFFF);
            if (s) s->grad = tmp[i].grad;
        }
}

void theme_clear(void)
{
    memset(g_theme, 0, sizeof g_theme);
    memset(g_themeCount, 0, sizeof g_themeCount);
}

int theme_count(int area) { return g_themeCount[area]; }

BOOL theme_entry(int area, int index, COLORREF *orig, COLORREF *repl)
{
    for (int i = 0, k = 0; i < THEME_SLOTS; i++) {
        if (!g_theme[area][i].key) continue;
        if (k++ == index) { *orig = g_theme[area][i].key & 0xFFFFFF; *repl = g_theme[area][i].val; return TRUE; }
    }
    return FALSE;
}

static COLORREF parse_hex(const wchar_t *s)
{
    while (*s == L' ' || *s == L'#') s++;
    unsigned v = 0; int n = 0;
    for (; n < 6; n++, s++) {
        unsigned d;
        if (*s >= L'0' && *s <= L'9') d = *s - L'0';
        else if (*s >= L'a' && *s <= L'f') d = *s - L'a' + 10;
        else if (*s >= L'A' && *s <= L'F') d = *s - L'A' + 10;
        else return NO_COLOR;
        v = v << 4 | d;
    }
    return RGB(v >> 16, (v >> 8) & 0xFF, v & 0xFF);
}

/* Theme files are UTF-16/ANSI ini:  [Theme] Name=..., then one section per area
 * with lines RRGGBB=RRGGBB  (original=replacement, web order). */
static void table_section(int tbl, wchar_t *out)
{
    int a = tbl / KIND_COUNT, k = tbl % KIND_COUNT;
    if (k) wsprintfW(out, L"%s.%s", kAreaNames[a], kKindNames[k]);
    else lstrcpyW(out, kAreaNames[a]);
}

BOOL theme_load(const wchar_t *path, wchar_t *nameOut, int nameLen)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    theme_clear();
    if (nameOut) GetPrivateProfileStringW(L"Theme", L"Name", L"", nameOut, nameLen, path);
    static wchar_t buf[65536];
    for (int a = 0; a < TBL_COUNT; a++) {
        wchar_t sec[40];
        table_section(a, sec);
        DWORD n = GetPrivateProfileSectionW(sec, buf, 65536, path);
        for (wchar_t *p = buf; n && *p; p += lstrlenW(p) + 1) {
            if (*p == L';') continue;
            wchar_t *eq = wcschr(p, L'=');
            if (!eq) continue;
            COLORREF o = parse_hex(p), r = parse_hex(eq + 1);
            if (o == NO_COLOR || r == NO_COLOR) continue;
            /* gradient: RRGGBB>RRGGBB (left to right) or RRGGBBvRRGGBB (top to bottom) */
            const wchar_t *q = eq + 1;
            while (*q == L' ' || *q == L'#') q++;
            q += 6;
            int dir = *q == L'>' ? GRAD_H : (*q == L'v' || *q == L'V') ? GRAD_V : -1;
            COLORREF c2 = dir >= 0 ? parse_hex(q + 1) : NO_COLOR;
            theme_set_grad(a, o, r, c2, c2 == NO_COLOR ? -1 : dir);
        }
    }
    return TRUE;
}

BOOL theme_save(const wchar_t *path, const wchar_t *name)
{
    FILE *f = _wfopen(path, L"w, ccs=UTF-16LE");
    if (!f) return FALSE;
    fwprintf(f, L"; Vocahansments UI Colors theme\n; original=replacement, colours as RRGGBB\n; a gradient is RRGGBB>RRGGBB (left to right) or RRGGBBvRRGGBB (top to bottom)\n[Theme]\nName=%ls\n", name);
    for (int a = 0; a < TBL_COUNT; a++) {
        if (!g_themeCount[a]) continue;
        wchar_t sec[40];
        table_section(a, sec);
        fwprintf(f, L"\n[%ls]\n", sec);
        for (int i = 0; i < THEME_SLOTS; i++) {
            ThemeSlot *s = &g_theme[a][i];
            if (!s->key) continue;
            COLORREF o = s->key & 0xFFFFFF, r = s->val;
            fwprintf(f, L"%02X%02X%02X=%02X%02X%02X", GetRValue(o), GetGValue(o), GetBValue(o), GetRValue(r), GetGValue(r), GetBValue(r));
            if (s->grad) {
                COLORREF g = s->grad & 0xFFFFFF;
                fwprintf(f, L"%lc%02X%02X%02X", (int)(s->grad >> 24) - 1 == GRAD_V ? L'v' : L'>', GetRValue(g), GetGValue(g), GetBValue(g));
            }
            fwprintf(f, L"\n");
        }
    }
    fclose(f);
    return TRUE;
}

COLORREF remap(int area, int kind, COLORREF c)
{
    if (!g_enabled || (c & 0xFF000000)) return c;   /* palette-relative / CLR_INVALID untouched */
    COLORREF r = NO_COLOR;
    if (kind) r = theme_lookup(TBL(area, kind), c);
    if (r == NO_COLOR && area != AREA_ALL) r = theme_lookup(TBL(area, 0), c);
    if (r == NO_COLOR && kind) r = theme_lookup(TBL(AREA_ALL, kind), c);
    if (r == NO_COLOR) r = theme_lookup(TBL(AREA_ALL, 0), c);
    return r == NO_COLOR ? c : r;
}

/* the gradient a fill of colour c gets, same lookup order as remap() */
BOOL remap_grad(int area, int kind, COLORREF c, COLORREF *c1, COLORREF *c2, int *dir)
{
    if (!g_enabled || (c & 0xFF000000)) return FALSE;
    int tbls[4] = { kind ? TBL(area, kind) : -1, area != AREA_ALL ? TBL(area, 0) : -1, kind ? TBL(AREA_ALL, kind) : -1, TBL(AREA_ALL, 0) };
    for (int i = 0; i < 4; i++) {
        if (tbls[i] < 0) continue;
        ThemeSlot *s = theme_slot(tbls[i], c);
        if (!s) continue;
        if (!s->grad) return FALSE;      /* the first table that maps it decides */
        *c1 = s->val;
        *c2 = s->grad & 0xFFFFFF;
        *dir = (int)(s->grad >> 24) - 1;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------ census -- */

#define CENSUS_SLOTS 4096
static CensusEntry g_census[CENSUS_SLOTS];
static int g_censusN;

static int kind_of(const char *what)
{
    if (!strcmp(what, "fill") || !strcmp(what, "bk")) return KIND_FILL;
    if (!strcmp(what, "line") || !strcmp(what, "frame")) return KIND_LINE;
    if (!strcmp(what, "text")) return KIND_TEXT;
    return KIND_ANY;
}

static void census_hit(int area, COLORREF c, const char *what)
{
    if (c & 0xFF000000) return;
    DWORD key = (c & 0xFFFFFF) | (area << 24) | (what[0] << 28);
    for (unsigned i = hash32(key), n = 0; n < 64; i++, n++) {
        CensusEntry *e = &g_census[i & (CENSUS_SLOTS - 1)];
        if (e->count && e->color == (c & 0xFFFFFF) && e->area == area && !strcmp(e->what, what)) { e->count++; return; }
        if (!e->count) {
            if (g_censusN >= CENSUS_SLOTS * 3 / 4) return;
            e->color = c & 0xFFFFFF; e->area = area; e->count = 1; e->kind = kind_of(what);
            lstrcpynA(e->what, what, sizeof e->what);
            g_censusN++;
            return;
        }
    }
}

int engine_bitmap_count(void);
int census_snapshot(CensusEntry *out, int max)
{
    int k = 0;
    for (int i = 0; i < CENSUS_SLOTS && k < max; i++) if (g_census[i].count) out[k++] = g_census[i];
    return k;
}

void census_reset(void) { memset(g_census, 0, sizeof g_census); g_censusN = 0; }

static int cmp_census(const void *a, const void *b)
{
    const CensusEntry *x = a, *y = b;
    if (x->area != y->area) return x->area - y->area;
    return (int)y->count - (int)x->count;
}

BOOL census_export(const wchar_t *path)
{
    static CensusEntry all[CENSUS_SLOTS];
    int n = census_snapshot(all, CENSUS_SLOTS);
    qsort(all, n, sizeof all[0], cmp_census);
    FILE *f = _wfopen(path, L"w");
    if (!f) return FALSE;
    fprintf(f, "; colours VOCALOID4 drew since the census was reset\n; area  kind  RRGGBB  uses\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%-8ls %-6s %02X%02X%02X %u\n", kAreaNames[all[i].area], all[i].what,
                GetRValue(all[i].color), GetGValue(all[i].color), GetBValue(all[i].color), all[i].count);
    fclose(f);
    return TRUE;
}

/* --------------------------------------------------------------- areas -- */

static DWORD g_tlsPaint = TLS_OUT_OF_INDEXES;
typedef struct { HWND hwnd; int area; } AreaCache;
static AreaCache g_areaCache[64];
static int g_areaNext;

static int classify_hwnd(HWND h)
{
    if (!h) return AREA_OTHER;
    for (int i = 0; i < 64; i++) if (g_areaCache[i].hwnd == h) return g_areaCache[i].area;
    int area = AREA_OTHER;
    for (HWND w = h; w; w = GetParent(w)) {
        wchar_t cls[64], title[64];
        GetClassNameW(w, cls, 64);
        if (!lstrcmpW(cls, L"AfxControlBar110u")) { area = AREA_BARS; break; }
        GetWindowTextW(w, title, 64);
        if (!wcsncmp(title, L"MUSICAL EDITOR", 14)) { area = AREA_MUSICAL; break; }
        if (!wcsncmp(title, L"TRACK EDITOR", 12)) { area = AREA_TRACK; break; }
        if (!wcsncmp(title, L"MIXER", 5)) { area = AREA_MIXER; break; }
    }
    g_areaCache[g_areaNext].hwnd = h; g_areaCache[g_areaNext].area = area;
    g_areaNext = (g_areaNext + 1) & 63;
    return area;
}

/* memory DCs remember the window of the DC they were made compatible with */
typedef struct { HDC dc; HWND hwnd; } MemDC;
static MemDC g_memdc[256];

static HWND memdc_owner(HDC dc)
{
    for (int i = 0; i < 256; i++) if (g_memdc[i].dc == dc) return g_memdc[i].hwnd;
    return NULL;
}

static HDC (WINAPI *o_CreateCompatibleDC)(HDC);
static HDC WINAPI h_CreateCompatibleDC(HDC ref)
{
    HDC dc = o_CreateCompatibleDC(ref);
    if (!dc) return dc;
    HWND h = ref ? WindowFromDC(ref) : NULL;
    if (!h && ref) h = memdc_owner(ref);
    if (!h && g_tlsPaint != TLS_OUT_OF_INDEXES) h = (HWND)TlsGetValue(g_tlsPaint);
    if (h) {
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < 256; i++) if (!g_memdc[i].dc) { g_memdc[i].dc = dc; g_memdc[i].hwnd = h; break; }
        LeaveCriticalSection(&g_lock);
    }
    return dc;
}

static BOOL (WINAPI *o_DeleteDC)(HDC);
static BOOL WINAPI h_DeleteDC(HDC dc)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < 256; i++) if (g_memdc[i].dc == dc) g_memdc[i].dc = NULL;
    LeaveCriticalSection(&g_lock);
    return o_DeleteDC(dc);
}

int current_area(HDC dc)
{
    HWND h = dc ? WindowFromDC(dc) : NULL;
    if (!h && dc) h = memdc_owner(dc);
    if (!h && g_tlsPaint != TLS_OUT_OF_INDEXES) h = (HWND)TlsGetValue(g_tlsPaint);
    return classify_hwnd(h);
}

/* --------------------------------------------------- tracked GDI objects -- */

enum { OBJ_NONE = 0, OBJ_TBRUSH, OBJ_TPEN };
typedef struct { HGDIOBJ h; BYTE type; BYTE style; WORD width; COLORREF orig, made; BOOL keep; } Tracked;
#define TRACK_SLOTS 16384
static Tracked g_track[TRACK_SLOTS];

static Tracked *track_find(HGDIOBJ h, BOOL create)
{
    for (unsigned i = hash32((DWORD)h), n = 0; n < 128; i++, n++) {
        Tracked *t = &g_track[i & (TRACK_SLOTS - 1)];
        if (t->h == h) return t;
        if (!t->h) { if (!create) return NULL; t->h = h; return t; }
    }
    return NULL;
}

static void track_forget(HGDIOBJ h)
{
    Tracked *t = track_find(h, FALSE);
    if (t && !t->keep) t->type = OBJ_NONE;   /* keep the slot as a tombstone (handle may be reused) */
}

/* cache of brushes/pens in final colours, never freed (bounded by #colours) */
typedef struct { DWORD key; COLORREF c; HGDIOBJ h; } Cached;
#define CACHE_SLOTS 4096
static Cached g_cache[CACHE_SLOTS];

static HBRUSH (WINAPI *o_CreateSolidBrush)(COLORREF);
static HPEN (WINAPI *o_CreatePen)(int, int, COLORREF);

static HGDIOBJ cached_object(BYTE type, int style, int width, COLORREF c)
{
    DWORD key = (type << 28) | ((style & 0xFF) << 20) | (width & 0xFFFF);
    for (unsigned i = hash32(key ^ c), n = 0; n < 256; i++, n++) {
        Cached *e = &g_cache[i & (CACHE_SLOTS - 1)];
        if (e->h && e->key == key && e->c == c) return e->h;
        if (!e->h) {
            e->key = key; e->c = c;
            e->h = type == OBJ_TBRUSH ? (HGDIOBJ)o_CreateSolidBrush(c) : (HGDIOBJ)o_CreatePen(style, width, c);
            if (e->h) {
                /* The editor gets this handle back from SelectObject and may
                 * DeleteObject it; mark it so h_DeleteObject refuses. Type stays
                 * OBJ_NONE so it is never remapped a second time. */
                EnterCriticalSection(&g_lock);
                Tracked *t = track_find(e->h, TRUE);
                if (t) { t->type = OBJ_NONE; t->keep = TRUE; }
                LeaveCriticalSection(&g_lock);
            }
            return e->h;
        }
    }
    return NULL;
}

/* stock pens/brushes (GetStockObject) behave like tracked objects */
static Tracked *stock_info(HGDIOBJ h)
{
    static Tracked st[5];
    static BOOL init;
    if (!init) {
        struct { int id; BYTE type; COLORREF c; } k[5] = {
            { BLACK_PEN, OBJ_TPEN, RGB(0, 0, 0) }, { WHITE_PEN, OBJ_TPEN, RGB(255, 255, 255) },
            { BLACK_BRUSH, OBJ_TBRUSH, RGB(0, 0, 0) }, { WHITE_BRUSH, OBJ_TBRUSH, RGB(255, 255, 255) },
            { GRAY_BRUSH, OBJ_TBRUSH, RGB(128, 128, 128) } };
        for (int i = 0; i < 5; i++) {
            st[i].h = GetStockObject(k[i].id); st[i].type = k[i].type;
            st[i].orig = st[i].made = k[i].c; st[i].style = PS_SOLID; st[i].width = 0;
        }
        init = TRUE;
    }
    for (int i = 0; i < 5; i++) if (st[i].h == h) return &st[i];
    return NULL;
}

/* the object to actually use for a tracked brush/pen in this DC's area */
static HGDIOBJ substitute(HDC dc, HGDIOBJ h, const char *what)
{
    Tracked *t = track_find(h, FALSE);
    if (!t || t->type == OBJ_NONE) t = stock_info(h);
    if (!t) return h;
    int area = current_area(dc);
    if (!strcmp(what, "select")) what = t->type == OBJ_TPEN ? "line" : "fill";
    census_hit(area, t->orig, what);
    COLORREF want = remap(area, kind_of(what), t->orig);
    if (want == t->made) return h;   /* the handle already has that colour */
    HGDIOBJ s = cached_object(t->type, t->style, t->width, want);
    return s ? s : h;
}

/* ---------------------------------------------------------------- hooks -- */

static HBRUSH WINAPI h_CreateSolidBrush(COLORREF c)
{
    COLORREF made = remap(AREA_ALL, KIND_FILL, c);
    HBRUSH b = o_CreateSolidBrush(made);
    if (b) {
        EnterCriticalSection(&g_lock);
        Tracked *t = track_find(b, TRUE);
        if (t) { t->type = OBJ_TBRUSH; t->orig = c; t->made = made; t->style = 0; t->width = 0; t->keep = FALSE; }
        LeaveCriticalSection(&g_lock);
    }
    return b;
}

static HPEN WINAPI h_CreatePen(int style, int width, COLORREF c)
{
    COLORREF made = remap(AREA_ALL, KIND_LINE, c);
    HPEN p = o_CreatePen(style, width, made);
    if (p) {
        EnterCriticalSection(&g_lock);
        Tracked *t = track_find(p, TRUE);
        if (t) { t->type = OBJ_TPEN; t->orig = c; t->made = made; t->style = (BYTE)style; t->width = (WORD)width; t->keep = FALSE; }
        LeaveCriticalSection(&g_lock);
    }
    return p;
}

/* Geometric/indirect pens are recoloured at creation only (AREA_ALL); their
 * extra attributes can't be rebuilt per area. */
static HPEN (WINAPI *o_ExtCreatePen)(DWORD, DWORD, const LOGBRUSH *, DWORD, const DWORD *);
static HPEN WINAPI h_ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *lb, DWORD n, const DWORD *dashes)
{
    if (!lb || lb->lbStyle != BS_SOLID) return o_ExtCreatePen(style, width, lb, n, dashes);
    LOGBRUSH b = *lb;
    census_hit(AREA_OTHER, lb->lbColor, "line");
    b.lbColor = remap(AREA_ALL, KIND_LINE, lb->lbColor);
    return o_ExtCreatePen(style, width, &b, n, dashes);
}

static HPEN (WINAPI *o_CreatePenIndirect)(const LOGPEN *);
static HPEN WINAPI h_CreatePenIndirect(const LOGPEN *lp)
{
    return lp ? h_CreatePen(lp->lopnStyle, lp->lopnWidth.x, lp->lopnColor) : o_CreatePenIndirect(lp);
}

static HBRUSH (WINAPI *o_CreateBrushIndirect)(const LOGBRUSH *);
static HBRUSH WINAPI h_CreateBrushIndirect(const LOGBRUSH *lb)
{
    if (lb && lb->lbStyle == BS_SOLID) return h_CreateSolidBrush(lb->lbColor);
    return o_CreateBrushIndirect(lb);
}

static HBRUSH (WINAPI *o_CreateHatchBrush)(int, COLORREF);
static HBRUSH WINAPI h_CreateHatchBrush(int s, COLORREF c) { return o_CreateHatchBrush(s, remap(AREA_ALL, KIND_FILL, c)); }

static HGDIOBJ (WINAPI *o_SelectObject)(HDC, HGDIOBJ);
static HGDIOBJ WINAPI h_SelectObject(HDC dc, HGDIOBJ h)
{
    return o_SelectObject(dc, g_enabled ? substitute(dc, h, "select") : h);
}

static BOOL (WINAPI *o_DeleteObject)(HGDIOBJ);
static BOOL WINAPI h_DeleteObject(HGDIOBJ h)
{
    EnterCriticalSection(&g_lock);
    Tracked *t = track_find(h, FALSE);
    BOOL keep = t && t->keep;
    if (!keep) track_forget(h);
    LeaveCriticalSection(&g_lock);
    return keep ? TRUE : o_DeleteObject(h);   /* one of our shared cached objects */
}

/* the colour each DC's background was last set to, before remapping:
 * FillSolidRect is SetBkColor + an opaque ExtTextOut, so a gradient or a
 * rainbow note is decided at the ExtTextOut by this colour */
typedef struct { HDC dc; COLORREF bk; } DcOrig;
static DcOrig g_bk[64];
static int g_bkNext;

static DcOrig *dc_orig(HDC dc, BOOL create)
{
    for (int i = 0; i < 64; i++) if (g_bk[i].dc == dc) return &g_bk[i];
    if (!create) return NULL;
    DcOrig *o = &g_bk[g_bkNext];
    g_bkNext = (g_bkNext + 1) & 63;
    o->dc = dc; o->bk = NO_COLOR;
    return o;
}

static void bk_remember(HDC dc, COLORREF c) { dc_orig(dc, TRUE)->bk = c; }

static COLORREF bk_orig(HDC dc)
{
    DcOrig *o = dc_orig(dc, FALSE);
    return o ? o->bk : NO_COLOR;
}

static COLORREF (WINAPI *o_SetBkColor)(HDC, COLORREF);
static COLORREF WINAPI h_SetBkColor(HDC dc, COLORREF c)
{
    int a = current_area(dc);
    census_hit(a, c, "bk");
    bk_remember(dc, c);
    return o_SetBkColor(dc, remap(a, KIND_FILL, c));
}

static COLORREF (WINAPI *o_SetTextColor)(HDC, COLORREF);
static COLORREF WINAPI h_SetTextColor(HDC dc, COLORREF c)
{
    int a = current_area(dc);
    census_hit(a, c, "text");
    return o_SetTextColor(dc, remap(a, KIND_TEXT, c));
}

static COLORREF (WINAPI *o_SetPixel)(HDC, int, int, COLORREF);
static COLORREF WINAPI h_SetPixel(HDC dc, int x, int y, COLORREF c) { return o_SetPixel(dc, x, y, remap(current_area(dc), KIND_LINE, c)); }
static BOOL (WINAPI *o_SetPixelV)(HDC, int, int, COLORREF);
static BOOL WINAPI h_SetPixelV(HDC dc, int x, int y, COLORREF c) { return o_SetPixelV(dc, x, y, remap(current_area(dc), KIND_LINE, c)); }

static HBRUSH sys_brush(int index);

/* FillRect/FrameRect also accept (HBRUSH)(COLOR_xxx + 1) */
static HBRUSH use_brush(HDC dc, HBRUSH b, const char *what)
{
    if (!g_enabled) return b;
    if ((UINT_PTR)b > 0 && (UINT_PTR)b <= 31) return sys_brush((int)(UINT_PTR)b - 1);
    return (HBRUSH)substitute(dc, b, what);
}

static int (WINAPI *o_FillRect)(HDC, const RECT *, HBRUSH);

/* ------------------------------------------------ gradients and rainbow -- */

static int g_rainbow;            /* RAINBOW_* */

static void grad_rect(HDC dc, const RECT *r, COLORREF a, COLORREF b, int dir)
{
    TRIVERTEX v[2] = {
        { r->left, r->top, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0 },
        { r->right, r->bottom, (COLOR16)(GetRValue(b) << 8), (COLOR16)(GetGValue(b) << 8), (COLOR16)(GetBValue(b) << 8), 0 },
    };
    GRADIENT_RECT g = { 0, 1 };
    GradientFill(dc, v, 2, &g, 1, dir == GRAD_V ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
}

static COLORREF hsv(double h, double s, double v)
{
    h = fmod(h, 360); if (h < 0) h += 360;
    double c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; } else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    return RGB((int)((r + m) * 255 + 0.5), (int)((g + m) * 255 + 0.5), (int)((b + m) * 255 + 0.5));
}

#define RB_SAT 0.62
#define RB_VAL 1.0
#define RB_HUE_SPAN 300.0        /* gradient: red to violet (not back round to red) */

/* windows that drew rainbow notes: the colour-changing mode repaints them */
static HWND g_rbWnd[8];
static RECT g_rbBox[8];          /* where in each window rainbow notes have been drawn */

/* Remember the window and where the note is: the animation repaints only
 * that area. Repainting the whole window also repainted the control-track
 * pane below the piano roll, which the editor draws straight onto the screen
 * and so flickered. */
static RECT g_rbPane[8];          /* the piano roll's note area in each window (gravity's floor and walls) */
static SIZE g_rbSize[8];
static BOOL g_rbRepainting;

/* The window a note is drawn in, as an index into g_rbWnd (-1 = unknown). The
 * editor clips notes to the piano roll, so the clip box of its own full
 * repaints (not our animation frames, which clip to their box) gives the
 * roll's note area: the biggest one seen since the window last changed size. */
static int rb_index(HWND h)
{
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++) if (g_rbWnd[i] == h) k = i;
    for (int i = 0; i < 8 && k < 0; i++)
        if (!g_rbWnd[i] || !IsWindow(g_rbWnd[i])) { k = i; g_rbWnd[i] = h; SetRectEmpty(&g_rbBox[i]); SetRectEmpty(&g_rbPane[i]); }
    return k;
}

static int note_window(HDC dc)
{
    HWND h = WindowFromDC(dc);
    if (!h) h = memdc_owner(dc);
    if (!h && g_tlsPaint != TLS_OUT_OF_INDEXES) h = (HWND)TlsGetValue(g_tlsPaint);
    if (!h) return -1;
    int k = rb_index(h);
    if (k < 0) return -1;
    RECT cl, clip;
    GetClientRect(h, &cl);
    if (cl.right != g_rbSize[k].cx || cl.bottom != g_rbSize[k].cy) { g_rbSize[k].cx = cl.right; g_rbSize[k].cy = cl.bottom; SetRectEmpty(&g_rbPane[k]); }
    if (!g_rbRepainting && GetClipBox(dc, &clip) > NULLREGION && (clip.right - clip.left) * (clip.bottom - clip.top) > (g_rbPane[k].right - g_rbPane[k].left) * (g_rbPane[k].bottom - g_rbPane[k].top))
        g_rbPane[k] = clip;
    return k;
}

/* Remember the window and where the note is: the animation repaints only
 * that area. Repainting the whole window also repainted the control-track
 * pane below the piano roll, which the editor draws straight onto the screen
 * and so flickered. */
static void rainbow_window(HDC dc, const RECT *r)
{
    int k = note_window(dc);
    if (k < 0) return;
    RECT d = *r, clip;
    /* only the part that can show: the editor clips notes to the piano roll */
    if (GetClipBox(dc, &clip) != NULLREGION && GetClipBox(dc, &clip) != ERROR && !IntersectRect(&d, &d, &clip)) return;
    LPtoDP(dc, (POINT *)&d, 2);          /* device coords = the window's client coords (back buffers match it) */
    UnionRect(&g_rbBox[k], &g_rbBox[k], &d);
}

static void rainbow_rect(HDC dc, const RECT *r)
{
    rainbow_window(dc, r);
    if (g_rainbow == RAINBOW_CYCLE) {
        /* every note one colour, all of them drifting through the rainbow;
         * neighbours are a little apart so it flows along the roll */
        double phase = fmod(GetTickCount() * 0.09, 360);
        COLORREF c = hsv(phase + r->left * 0.25, RB_SAT, RB_VAL);
        HBRUSH b = o_CreateSolidBrush(c);
        o_FillRect(dc, r, b);
        o_DeleteObject(b);
        return;
    }
    /* the whole rainbow, red to violet, stretched over the note's length */
    enum { SEGS = 12 };                        /* 30 degrees of hue each */
    TRIVERTEX v[2 * SEGS];
    GRADIENT_RECT g[SEGS];
    int w = r->right - r->left, n = 0;
    for (int i = 0; i < SEGS; i++) {
        int x0 = r->left + w * i / SEGS, x1 = r->left + w * (i + 1) / SEGS;
        if (x1 <= x0) continue;
        COLORREF a = hsv(i * RB_HUE_SPAN / SEGS, RB_SAT, RB_VAL), b = hsv((i + 1) * RB_HUE_SPAN / SEGS, RB_SAT, RB_VAL);
        TRIVERTEX va = { x0, r->top, (COLOR16)(GetRValue(a) << 8), (COLOR16)(GetGValue(a) << 8), (COLOR16)(GetBValue(a) << 8), 0 };
        TRIVERTEX vb = { x1, r->bottom, (COLOR16)(GetRValue(b) << 8), (COLOR16)(GetGValue(b) << 8), (COLOR16)(GetBValue(b) << 8), 0 };
        v[2 * n] = va; v[2 * n + 1] = vb;
        g[n].UpperLeft = 2 * n; g[n].LowerRight = 2 * n + 1;
        n++;
    }
    if (n) GradientFill(dc, v, 2 * n, g, n, GRADIENT_FILL_RECT_H);
}

static BOOL is_note_fill(COLORREF c)
{
    c &= 0xFFFFFF;
    return c == RGB(0xB5, 0xDC, 0x56) || c == RGB(0xC9, 0xF0, 0x6A) || c == RGB(0x0B, 0xE9, 0xF4);
}

static BOOL (WINAPI *o_Polyline)(HDC, const POINT *, int);

/* ------------------------------------------------------- spinning notes -- */
/* Notes in the piano roll drawn rotated about their centre: the fill (in
 * whatever colour, gradient or rainbow they would have), an outline, and the
 * lyric through a world transform. The editor's own straight outline is
 * skipped (h_LineTo). Repainted by the animation timer below. */

static int g_spinMode, g_spinSpeed = 1;
static BOOL g_spinVary;
static BOOL (*g_isPlaying)(void);
static const double kSpinDegPerSec[SPIN_SPEEDS] = { 45, 120, 300, 900 };
#define NOTE_EDGE RGB(0x7D, 0x7B, 0x7C)          /* the editor's note outline */

static BOOL spin_now(void)
{
    return g_spinMode == SPIN_ALWAYS || (g_spinMode == SPIN_PLAYING && g_isPlaying && g_isPlaying());
}

/* Notes drawn spinning, newest last. The editor draws every note's box first
 * and the lyrics and outlines after, so those find their note by position. */
typedef struct { HDC dc; RECT r; XFORM x; } SpinNote;     /* x: how it is drawn */
#define SPIN_NOTES 1024
static SpinNote g_spin[SPIN_NOTES];
static int g_spinNext;

static void spin_record(HDC dc, const RECT *r, const XFORM *x)
{
    SpinNote *s = &g_spin[g_spinNext];
    g_spinNext = (g_spinNext + 1) & (SPIN_NOTES - 1);
    s->dc = dc; s->r = *r; s->x = *x;
}

static SpinNote *spin_note_at(HDC dc, int x, int y)
{
    for (int k = 1; k <= SPIN_NOTES; k++) {
        SpinNote *s = &g_spin[(g_spinNext - k) & (SPIN_NOTES - 1)];
        if (!s->dc) break;
        if (s->dc == dc && x >= s->r.left - 1 && x <= s->r.right && y >= s->r.top - 1 && y <= s->r.bottom) return s;
    }
    return NULL;
}

static double spin_angle(const RECT *r)
{
    double speed = kSpinDegPerSec[g_spinSpeed], phase = 0;
    if (g_spinVary) {
        /* each note its own speed (0.3x to 2x) and direction, from its size and row */
        unsigned h = hash32((DWORD)(r->right - r->left) * 2654435761u ^ (DWORD)r->top);
        speed *= 0.3 + (h % 1000) / 588.0;
        if (h & 0x40000) speed = -speed;
        phase = (h >> 20) % 360;
    }
    return fmod(phase + speed * GetTickCount() / 1000.0, 360);
}

/* the rectangle a note can sweep while it turns */
static RECT spin_reach(const RECT *r)
{
    double cx = (r->left + r->right) / 2.0, cy = (r->top + r->bottom) / 2.0;
    int rad = (int)ceil(hypot(r->right - r->left, r->bottom - r->top) / 2) + 2;
    RECT o = { (int)cx - rad, (int)cy - rad, (int)cx + rad + 1, (int)cy + rad + 1 };
    return o;
}

/* turned by deg about its centre, then moved down by dy */
static XFORM spin_xform(const RECT *r, double deg, double dy)
{
    double a = deg * 3.14159265358979 / 180, c = cos(a), s = sin(a);
    double cx = (r->left + r->right) / 2.0, cy = (r->top + r->bottom) / 2.0;
    XFORM x = { (FLOAT)c, (FLOAT)s, (FLOAT)-s, (FLOAT)c, (FLOAT)(cx - cx * c + cy * s), (FLOAT)(cy - cx * s - cy * c + dy) };
    return x;
}

/* --------------------------------------------------------- jelly notes -- */
/* Notes wobble like jelly: squash and stretch (keeping roughly their area)
 * and sway, pivoting on their bottom edge, each note in its own rhythm. */

static int g_jellyMode;

static BOOL jelly_now(void)
{
    return g_jellyMode == SPIN_ALWAYS || (g_jellyMode == SPIN_PLAYING && g_isPlaying && g_isPlaying());
}

static XFORM jelly_xform(const RECT *r)
{
    unsigned h = hash32((DWORD)r->left * 2654435761u ^ (DWORD)r->top);
    double ph = (h % 6283) / 1000.0, t = GetTickCount() / 1000.0, tau = 6.28318530718;
    double a = sin(tau * 1.6 * t + ph), b = sin(tau * 1.1 * t + ph * 1.7);
    double sx = 1 + 0.10 * a, sy = 1 - 0.35 * a, sh = 0.3 * b;     /* squash, stretch, sway */
    double cx = (r->left + r->right) / 2.0, by = r->bottom;
    /* x' = cx + sx (x - cx) - sh (y - by),  y' = by + sy (y - by) */
    XFORM x = { (FLOAT)sx, 0, (FLOAT)-sh, (FLOAT)sy, (FLOAT)(cx - sx * cx + sh * by), (FLOAT)(by - sy * by) };
    return x;
}

/* ------------------------------------------------------- gravity notes -- */
/* Google-Gravity style. When the drop starts, every note the piano roll shows
 * is captured as it looks (one normal frame) and becomes a rigid box in a
 * physics world (physics.c): the floor is the grey strip under the roll, the
 * walls are its sides, the top is open. From then on the editor's own drawing
 * of those notes is hidden and the boxes are drawn wherever they are, turned
 * however they landed, into the buffered frame (h_EndPaint). With the drag
 * tool they can be grabbed and thrown (anim_subclass). */

static int g_gravMode, g_gravStrength = 1, g_gravBounce = 1;
static BOOL g_dragTool = TRUE;
static const double kGravity[GRAV_STRENGTHS] = { 500, 1800, 4500, 0 }; /* px/s^2: moon, earth, jupiter, space */
static const double kBounce[GRAV_BOUNCES] = { 0.0, 0.3, 0.65 };        /* restitution */

static BOOL g_physOn, g_physCapture;     /* the world runs / the next frame captures the notes */
static int g_physK = -1;                 /* the window (g_rbWnd index) it lives in */
static RECT g_physPane;
static HBITMAP g_skin[PHYS_MAX_BODIES];  /* each note as it looked, by body tag */
static RECT g_cap[PHYS_MAX_BODIES];      /* notes seen this frame that have no box yet */
static int g_ncap, g_capK = -1;
static RECT g_known[PHYS_MAX_BODIES];    /* where the notes with a box were (their editor rect) */
static int g_nknown, g_nskin;
static int g_physQuiet;                  /* steps in a row nothing moved */
static DWORD g_quakeUntil;               /* earthquake running until then */

static BOOL grav_now(void)
{
    return g_gravMode == SPIN_ALWAYS || (g_gravMode == SPIN_PLAYING && g_isPlaying && g_isPlaying());
}

/* notes are being drawn somewhere other than where they are */
static BOOL motion_now(void) { return (g_spinMode && spin_now()) || (g_jellyMode && jelly_now()) || g_physOn; }

static void phys_end(void)
{
    g_physOn = FALSE;
    g_physCapture = FALSE;
    phys_reset();
    for (int i = 0; i < PHYS_MAX_BODIES; i++) if (g_skin[i]) { o_DeleteObject(g_skin[i]); g_skin[i] = NULL; }
    g_ncap = 0;
    g_capK = -1;
    g_nknown = g_nskin = 0;
}

/* this note already has a box in the world */
static BOOL phys_known(const RECT *r)
{
    RECT c = *r;
    c.right++; c.bottom++;
    /* the same note can come back a pixel different (selected, overlapping) */
    for (int i = 0; i < g_nknown; i++) {
        RECT o;
        if (!IntersectRect(&o, &g_known[i], &c)) continue;
        long ov = (long)(o.right - o.left) * (o.bottom - o.top), area = (long)(c.right - c.left) * (c.bottom - c.top);
        if (ov * 2 >= area) return TRUE;
    }
    return FALSE;
}

/* the next full repaint shows the notes as they are and captures them */
static void phys_start(void)
{
    phys_end();
    g_physCapture = TRUE;
}

/* during the capture frame: where each note is (drawn as normal) */
static void phys_capture_note(HDC dc, const RECT *r)
{
    int k = note_window(dc);
    if (k < 0 || g_ncap >= PHYS_MAX_BODIES || (g_capK >= 0 && k != g_capK)) return;   /* one piano roll */
    g_capK = k;
    RECT c = *r;
    c.right++; c.bottom++;                            /* its outline is drawn just outside */
    for (int i = 0; i < g_ncap; i++) {
        RECT o;
        if (IntersectRect(&o, &g_cap[i], &c) && (long)(o.right - o.left) * (o.bottom - o.top) * 2 >= (long)(c.right - c.left) * (c.bottom - c.top)) return;
    }
    g_cap[g_ncap++] = c;
}

/* The piano roll's note area in window k: from Pitch Tools, which knows it
 * exactly, else the biggest clip box the editor drew notes with. */
/* Pitch Tools' answer for a window: its note area, if it is the piano roll */
static BOOL phys_pane_of(HWND h, RECT *out)
{
    static BOOL (__cdecl *area)(HWND, RECT *);
    static BOOL looked;
    if (!looked) {
        HMODULE pt = GetModuleHandleA("PitchTools.dll");
        area = pt ? (BOOL (__cdecl *)(HWND, RECT *))GetProcAddress(pt, "PitchTools_NoteArea") : NULL;
        looked = TRUE;
    }
    return h && area && area(h, out) && !IsRectEmpty(out);
}

static BOOL phys_pane(int k, RECT *out)
{
    if (k < 0) return FALSE;
    if (phys_pane_of(g_rbWnd[k], out)) return TRUE;
    *out = g_rbPane[k];
    return !IsRectEmpty(out);
}

/* floor (the grey strip under the roll), walls (its sides) and a ceiling
 * (its top, so nothing thrown gets lost). Bodies 0..3, moved along when the
 * roll changes size. */
static void phys_bounds(const RECT *pane)
{
    double w = pane->right - pane->left, h = pane->bottom - pane->top;
    double pos[4][2] = { { pane->left + w / 2, pane->bottom + 200 }, { pane->left - 200, pane->top + h / 2 }, { pane->right + 200, pane->top + h / 2 },
                         { pane->left + w / 2, pane->top - 200 } };
    double size[4][2] = { { w + 4000, 400 }, { 400, h + 4000 }, { 400, h + 4000 }, { w + 4000, 400 } };
    for (int i = 0; i < 4; i++) {
        PBody *b = phys_body(i);
        if (!b) { phys_add(pos[i][0], pos[i][1], size[i][0], size[i][1], 0, -1); continue; }
        b->pos.x = pos[i][0]; b->pos.y = pos[i][1]; b->size.x = size[i][0]; b->size.y = size[i][1];
    }
    g_physPane = *pane;
}

/* the notes captured this frame, as they look in dc, become boxes */
static void phys_add_captured(HDC dc)
{
    for (int i = 0; i < g_ncap && g_nskin < PHYS_MAX_BODIES && g_nknown < PHYS_MAX_BODIES; i++) {
        g_known[g_nknown++] = g_cap[i];
        RECT r;
        if (!IntersectRect(&r, &g_cap[i], &g_physPane)) continue;
        int bw = r.right - r.left, bh = r.bottom - r.top;
        if (bw < 2 || bh < 2) continue;
        HBITMAP bmp = CreateCompatibleBitmap(dc, bw, bh);
        HDC mem = bmp ? CreateCompatibleDC(dc) : NULL;
        if (!mem) { if (bmp) o_DeleteObject(bmp); continue; }
        HGDIOBJ old = o_SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, bw, bh, dc, r.left, r.top, SRCCOPY);
        o_SelectObject(mem, old);
        DeleteDC(mem);
        if (phys_add(r.left + bw / 2.0, r.top + bh / 2.0, bw, bh, bw * bh * 0.01, g_nskin) < 0) { o_DeleteObject(bmp); continue; }
        g_skin[g_nskin++] = bmp;
    }
    g_ncap = 0;
}

/* end of the capture frame: the world, with every note the roll shows */
static void phys_build(HDC dc)
{
    RECT pane;
    if (!phys_pane(g_capK, &pane)) return;           /* the roll isn't known yet: try the next frame */
    phys_reset();
    g_physK = g_capK;
    phys_bounds(&pane);
    phys_add_captured(dc);
    if (kGravity[g_gravStrength] == 0)                 /* space: nothing pulls, so give them a nudge */
        for (int i = 0; i < phys_count(); i++) {
            PBody *b = phys_body(i);
            if (b->invMass == 0) continue;
            b->vel.x = rand() % 160 - 80; b->vel.y = rand() % 160 - 80; b->angVel = (rand() % 200 - 100) / 100.0;
        }
    g_physCapture = FALSE;
    g_physOn = TRUE;
    g_physQuiet = 0;
}

/* the boxes, turned and where they are, into the frame about to be shown */
static void phys_draw(HDC dc)
{
    int save = SaveDC(dc);
    IntersectClipRect(dc, g_physPane.left, g_physPane.top, g_physPane.right, g_physPane.bottom);
    HDC mem = CreateCompatibleDC(dc);
    for (int i = 0; mem && i < phys_count(); i++) {
        PBody *b = phys_body(i);
        if (b->invMass == 0 || b->user < 0 || !g_skin[b->user]) continue;
        double c = cos(b->rot), s = sin(b->rot), hw = b->size.x / 2, hh = b->size.y / 2;
        POINT p[3] = {                                                    /* top left, top right, bottom left */
            { (LONG)floor(b->pos.x - hw * c + hh * s + 0.5), (LONG)floor(b->pos.y - hw * s - hh * c + 0.5) },
            { (LONG)floor(b->pos.x + hw * c + hh * s + 0.5), (LONG)floor(b->pos.y + hw * s - hh * c + 0.5) },
            { (LONG)floor(b->pos.x - hw * c - hh * s + 0.5), (LONG)floor(b->pos.y - hw * s + hh * c + 0.5) },
        };
        HGDIOBJ old = o_SelectObject(mem, g_skin[b->user]);
        PlgBlt(dc, p, mem, 0, 0, (int)b->size.x, (int)b->size.y, NULL, 0, 0);
        o_SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    RestoreDC(dc, save);
}

/* one frame of physics: 4 steps of 1/120 s */
static void phys_tick(void)
{
    double maxv = 0;
    if (GetTickCount() < g_quakeUntil) {               /* the floor shakes: everything near it hops */
        for (int i = 0; i < phys_count(); i++) {
            PBody *b = phys_body(i);
            if (b->invMass == 0) continue;
            b->vel.x += rand() % 240 - 120;
            if (b->pos.y + b->size.y > g_physPane.bottom - 40) b->vel.y -= rand() % 220;
            b->angVel += (rand() % 600 - 300) / 100.0;
        }
        g_physQuiet = 0;
    }
    for (int n = 0; n < 4; n++) phys_step(1 / 120.0, kGravity[g_gravStrength], kBounce[g_gravBounce], 10);
    for (int i = 0; i < phys_count(); i++) {
        PBody *b = phys_body(i);
        if (b->invMass == 0) continue;
        double sp = hypot(b->vel.x, b->vel.y) + fabs(b->angVel) * 40;
        if (sp > maxv) maxv = sp;
    }
    g_physQuiet = maxv < 4 && phys_grabbed() < 0 ? g_physQuiet + 1 : 0;
}

static POINT spin_pt(const XFORM *x, double px, double py)
{
    POINT p = { (LONG)floor(px * x->eM11 + py * x->eM21 + x->eDx + 0.5), (LONG)floor(px * x->eM12 + py * x->eM22 + x->eDy + 0.5) };
    return p;
}

/* fill colour at (u along the note, v across it), both 0..1 */
typedef struct { int kind; COLORREF c1, c2; int dir; } SpinFill;   /* kind: 0 solid, 1 gradient, 2 rainbow */

static COLORREF lerp_rgb(COLORREF a, COLORREF b, double f)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * f + 0.5), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * f + 0.5),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * f + 0.5));
}

static COLORREF spin_color(const SpinFill *f, double u, double v)
{
    if (f->kind == 2) return hsv(u * RB_HUE_SPAN, RB_SAT, RB_VAL);
    if (f->kind == 1) return lerp_rgb(f->c1, f->c2, f->dir == GRAD_V ? v : u);
    return f->c1;
}

static void spin_draw(HDC dc, const RECT *r, COLORREF orig, int area, const XFORM *xf)
{
    SpinFill f = { 0, 0, 0, 0 };
    if (g_rainbow && (orig & 0xFFFFFF) == RGB(0xB5, 0xDC, 0x56)) {
        if (g_rainbow == RAINBOW_CYCLE) f.c1 = hsv(fmod(GetTickCount() * 0.09, 360) + r->left * 0.25, RB_SAT, RB_VAL);
        else f.kind = 2;
    } else if (remap_grad(area, KIND_FILL, orig, &f.c1, &f.c2, &f.dir)) f.kind = 1;
    else f.c1 = remap(area, KIND_FILL, orig);

    XFORM x = *xf;
    enum { SEGS = 12 };
    int segs = f.kind == 0 ? 1 : SEGS;
    TRIVERTEX v[4 * SEGS];
    GRADIENT_TRIANGLE t[2 * SEGS];
    double w = r->right - r->left;
    for (int i = 0; i < segs; i++) {
        double u0 = (double)i / segs, u1 = (double)(i + 1) / segs, x0 = r->left + w * u0, x1 = r->left + w * u1;
        double pts[4][4] = { { x0, r->top, u0, 0 }, { x1, r->top, u1, 0 }, { x1, r->bottom, u1, 1 }, { x0, r->bottom, u0, 1 } };
        for (int k = 0; k < 4; k++) {
            POINT p = spin_pt(&x, pts[k][0], pts[k][1]);
            COLORREF c = spin_color(&f, pts[k][2], pts[k][3]);
            TRIVERTEX tv = { p.x, p.y, (COLOR16)(GetRValue(c) << 8), (COLOR16)(GetGValue(c) << 8), (COLOR16)(GetBValue(c) << 8), 0 };
            v[4 * i + k] = tv;
        }
        t[2 * i].Vertex1 = 4 * i; t[2 * i].Vertex2 = 4 * i + 1; t[2 * i].Vertex3 = 4 * i + 2;
        t[2 * i + 1].Vertex1 = 4 * i; t[2 * i + 1].Vertex2 = 4 * i + 2; t[2 * i + 1].Vertex3 = 4 * i + 3;
    }
    GradientFill(dc, v, 4 * segs, t, 2 * segs, GRADIENT_FILL_TRIANGLE);

    POINT edge[5] = { spin_pt(&x, r->left, r->top), spin_pt(&x, r->right - 1, r->top), spin_pt(&x, r->right - 1, r->bottom - 1),
                      spin_pt(&x, r->left, r->bottom - 1), spin_pt(&x, r->left, r->top) };
    HPEN pen = o_CreatePen(PS_SOLID, 1, remap(area, KIND_LINE, NOTE_EDGE));
    HGDIOBJ old = o_SelectObject(dc, pen);
    o_Polyline(dc, edge, 5);
    o_SelectObject(dc, old);
    o_DeleteObject(pen);

    spin_record(dc, r, &x);
}

#include "portraits.inl"

/* A note box, not something else in the note colours (the keyboard's
 * highlighted key is the selected-note colour): mostly inside the note area. */
static BOOL in_roll(HDC dc, const RECT *r)
{
    RECT pane, o;
    if (!phys_pane(note_window(dc), &pane)) return TRUE;           /* not known yet: trust the colour */
    if (!IntersectRect(&o, r, &pane)) return FALSE;
    return (long)(o.right - o.left) * (o.bottom - o.top) * 2 >= (long)(r->right - r->left) * (r->bottom - r->top);
}

/* fills that are not a plain colour: spinning notes, rainbow notes, gradient
 * replacements. TRUE = drawn here. */
static BOOL special_fill(HDC dc, const RECT *r, COLORREF orig)
{
    if (orig == NO_COLOR || !r || r->right <= r->left || r->bottom <= r->top) return FALSE;
    int area = current_area(dc);
    BOOL note = area == AREA_MUSICAL && is_note_fill(orig) && in_roll(dc, r);
    if (note) { portrait_draw(dc, note_window(dc)); portrait_outside_paint(dc); }   /* behind the first note */
    if (note) {
        if (g_physOn && phys_known(r)) {
            /* the physics box stands in for this note: hide it (and, through
             * this record, its lyric, outline and squiggle) */
            static const XFORM ident = { 1, 0, 0, 1, 0, 0 };
            spin_record(dc, r, &ident);
            return TRUE;
        }
        /* a note to capture: everything at the drop, later any new one (drawn
         * as normal this once, then it drops in too) */
        if (g_physCapture || g_physOn) phys_capture_note(dc, r);
    }
    if ((g_spinMode || g_jellyMode) && note) {
        /* the animation repaints everywhere the note can reach */
        RECT reach = g_spinMode ? spin_reach(r) : *r;
        if (g_jellyMode) InflateRect(&reach, (r->right - r->left) / 6 + 6, (r->bottom - r->top) + 6);
        rainbow_window(dc, &reach);
        BOOL spin = g_spinMode && spin_now(), jelly = g_jellyMode && jelly_now();
        if (spin || jelly) {
            XFORM x = { 1, 0, 0, 1, 0, 0 };
            if (jelly) x = jelly_xform(r);
            if (spin) { XFORM sp = spin_xform(r, spin_angle(r), 0), c; CombineTransform(&c, &x, &sp); x = c; }
            spin_draw(dc, r, orig, area, &x);
            return TRUE;
        }
    }
    if (g_rainbow && (orig & 0xFFFFFF) == RGB(0xB5, 0xDC, 0x56) && (area == AREA_MUSICAL || area == AREA_TRACK)) {
        rainbow_rect(dc, r);
        return TRUE;
    }
    COLORREF c1, c2;
    int dir;
    if (!remap_grad(area, KIND_FILL, orig, &c1, &c2, &dir)) return FALSE;
    grad_rect(dc, r, c1, c2, dir);
    return TRUE;
}

static int WINAPI h_FillRect(HDC dc, const RECT *r, HBRUSH b)
{
    if (r && (UINT_PTR)b > 31) {
        Tracked *t = track_find(b, FALSE);
        if (t && t->type == OBJ_TBRUSH && special_fill(dc, r, t->orig)) return 1;
    }
    return o_FillRect(dc, r, use_brush(dc, b, "fill"));
}

static BOOL (WINAPI *o_ExtTextOutW)(HDC, int, int, UINT, const RECT *, LPCWSTR, UINT, const INT *);
/* ------------------------------------------------------------ note text -- */
/* The lyric on a note ("a [a]") is a DrawText in plain black, the same black
 * as most other text, so it can't be themed by colour. What sets it apart:
 * the DC's background is still the note's fill colour when it is drawn. The
 * "NoteText" area is keyed by that fill colour (normal, selected or
 * overlapping note) and gives the lyric colour for it. */

/* the colour to draw this text in if it is a themed note lyric, else NO_COLOR */
static COLORREF note_text_color(HDC dc)
{
    COLORREF bk = bk_orig(dc);
    if (bk == NO_COLOR || !is_note_fill(bk) || current_area(dc) != AREA_MUSICAL) return NO_COLOR;
    census_hit(AREA_NOTETEXT, bk, "text");
    if (!g_enabled) return NO_COLOR;
    COLORREF r = theme_lookup(TBL(AREA_NOTETEXT, KIND_TEXT), bk & 0xFFFFFF);
    if (r == NO_COLOR) r = theme_lookup(TBL(AREA_NOTETEXT, KIND_ANY), bk & 0xFFFFFF);
    return r;
}

/* a spinning note's lyric turns with it */
typedef struct { int mode; XFORM x; BOOL on; } SpinText;

static void spin_text_begin(HDC dc, int px, int py, SpinText *t)
{
    t->on = FALSE;
    SpinNote *s;
    COLORREF bk = bk_orig(dc);
    if (bk == NO_COLOR || !is_note_fill(bk) || !motion_now() || !(s = spin_note_at(dc, px, py))) return;
    t->mode = SetGraphicsMode(dc, GM_ADVANCED);
    GetWorldTransform(dc, &t->x);
    t->on = SetWorldTransform(dc, &s->x);
    if (!t->on) SetGraphicsMode(dc, t->mode);
}

static void spin_text_end(HDC dc, SpinText *t)
{
    if (!t->on) return;
    SetWorldTransform(dc, &t->x);
    SetGraphicsMode(dc, t->mode);
}

/* a note the physics world stands in for: its lyric isn't drawn */
static BOOL phys_hides_text(HDC dc, int px, int py)
{
    COLORREF bk;
    return g_physOn && (bk = bk_orig(dc)) != NO_COLOR && is_note_fill(bk) && spin_note_at(dc, px, py);
}

#define WITH_NOTE_TEXT(dc, px, py, call) do { \
    if (phys_hides_text(dc, px, py)) { r = 1; break; } \
    COLORREF _nt = note_text_color(dc), _old = 0; \
    SpinText _st; \
    spin_text_begin(dc, px, py, &_st); \
    if (_nt != NO_COLOR) _old = o_SetTextColor(dc, _nt); \
    r = call; \
    if (_nt != NO_COLOR) o_SetTextColor(dc, _old); \
    spin_text_end(dc, &_st); } while (0)

static BOOL (WINAPI *o_TextOutW)(HDC, int, int, LPCWSTR, int);
static BOOL WINAPI h_TextOutW(HDC dc, int x, int y, LPCWSTR s, int n) { BOOL r; WITH_NOTE_TEXT(dc, x, y, o_TextOutW(dc, x, y, s, n)); return r; }
static int (WINAPI *o_DrawTextW)(HDC, LPCWSTR, int, LPRECT, UINT);
static int WINAPI h_DrawTextW(HDC dc, LPCWSTR s, int n, LPRECT rc, UINT f) { int r; WITH_NOTE_TEXT(dc, rc ? rc->left + 1 : 0, rc ? rc->top + 1 : 0, o_DrawTextW(dc, s, n, rc, f)); return r; }
static int (WINAPI *o_DrawTextExW)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
static int WINAPI h_DrawTextExW(HDC dc, LPWSTR s, int n, LPRECT rc, UINT f, LPDRAWTEXTPARAMS p) { int r; WITH_NOTE_TEXT(dc, rc ? rc->left + 1 : 0, rc ? rc->top + 1 : 0, o_DrawTextExW(dc, s, n, rc, f, p)); return r; }

static BOOL WINAPI h_ExtTextOutW(HDC dc, int x, int y, UINT opt, const RECT *rc, LPCWSTR s, UINT n, const INT *dx)
{
    if (n && !(opt & ETO_OPAQUE)) { BOOL r; WITH_NOTE_TEXT(dc, x, y, o_ExtTextOutW(dc, x, y, opt, rc, s, n, dx)); return r; }
    if ((opt & ETO_OPAQUE) && rc && special_fill(dc, rc, bk_orig(dc))) {
        if (!n) return TRUE;
        int mode = SetBkMode(dc, TRANSPARENT);
        BOOL r = o_ExtTextOutW(dc, x, y, opt & ~ETO_OPAQUE, rc, s, n, dx);
        SetBkMode(dc, mode);
        return r;
    }
    return o_ExtTextOutW(dc, x, y, opt, rc, s, n, dx);
}

static BOOL (WINAPI *o_ExtTextOutA)(HDC, int, int, UINT, const RECT *, LPCSTR, UINT, const INT *);
static BOOL WINAPI h_ExtTextOutA(HDC dc, int x, int y, UINT opt, const RECT *rc, LPCSTR s, UINT n, const INT *dx)
{
    if ((opt & ETO_OPAQUE) && rc && special_fill(dc, rc, bk_orig(dc))) {
        if (!n) return TRUE;
        int mode = SetBkMode(dc, TRANSPARENT);
        BOOL r = o_ExtTextOutA(dc, x, y, opt & ~ETO_OPAQUE, rc, s, n, dx);
        SetBkMode(dc, mode);
        return r;
    }
    return o_ExtTextOutA(dc, x, y, opt, rc, s, n, dx);
}

/* colour-changing mode: repaint the windows with rainbow notes ~16 times a
 * second, but never while a mouse button is held (drags draw their own
 * previews on top of the window).
 * The editor only redraws its notes after an erase, and its erase paints the
 * background straight onto the screen - that was the flicker. During our
 * repaint the erase goes to an off-screen bitmap instead: the editor still
 * sees it and redraws everything, but only the finished frame reaches the
 * screen. */
#define RB_TIMER 0x52424E57   /* 'RBNW' */

/* The animated windows' subclass: the off-screen erase above, and the
 * gravity drag tool - grab a note box, carry it, let go to throw it. */
static LRESULT CALLBACK rainbow_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_ERASEBKGND && g_rbRepainting) {
        RECT rc;
        GetClientRect(h, &rc);
        HDC wdc = (HDC)wp, mem = CreateCompatibleDC(wdc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(wdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1) : NULL;
        if (!bmp) { if (mem) DeleteDC(mem); return DefSubclassProc(h, msg, wp, lp); }
        HGDIOBJ old = o_SelectObject(mem, bmp);
        LRESULT r = DefSubclassProc(h, msg, (WPARAM)mem, lp);
        o_SelectObject(mem, old);
        o_DeleteObject(bmp);
        DeleteDC(mem);
        return r;
    }
    BOOL world = g_physOn && g_dragTool && g_physK >= 0 && g_rbWnd[g_physK] == h;
    PV p = { (short)LOWORD(lp), (short)HIWORD(lp) };
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (world) {
            PV local;
            int b = phys_hit(p, &local);
            if (b >= 0) {
                phys_grab(b, local, p);
                g_physQuiet = 0;
                SetCapture(h);
                return 0;               /* the editor never sees the click */
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (phys_grabbed() >= 0) { phys_drag_to(p); return 0; }
        break;
    case WM_LBUTTONUP:
        if (phys_grabbed() >= 0) { phys_release(); ReleaseCapture(); return 0; }
        break;
    case WM_CAPTURECHANGED:
        if (phys_grabbed() >= 0 && (HWND)lp != h) phys_release();
        break;
    case WM_SETCURSOR:
        if (world && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            PV q = { pt.x, pt.y };
            if (phys_grabbed() >= 0 || phys_hit(q, NULL) >= 0) {
                SetCursor(LoadCursor(NULL, phys_grabbed() >= 0 ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_NCDESTROY:
        if (g_physK >= 0 && g_rbWnd[g_physK] == h) phys_end();
        RemoveWindowSubclass(h, rainbow_subclass, id);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static BOOL g_spinWas;            /* notes were moving at the last frame */

static VOID CALLBACK rainbow_tick(HWND w, UINT m, UINT_PTR id, DWORD t)
{
    (void)w; (void)m; (void)id; (void)t;
    /* a held button belongs to the editor (its own drags) - unless it is ours */
    if (((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) && phys_grabbed() < 0) return;
    /* gravity switching on (e.g. playback starting) drops the notes; off brings them back */
    BOOL grav = g_gravMode && grav_now();
    if (grav && !g_physOn && !g_physCapture) { phys_start(); engine_refresh(); return; }
    if (!grav && (g_physOn || g_physCapture)) { phys_end(); engine_refresh(); return; }
    if (g_physOn) {
        RECT pane;
        if (phys_pane(g_physK, &pane) && !EqualRect(&pane, &g_physPane)) { phys_bounds(&pane); g_physQuiet = 0; }
        phys_tick();
        /* the world's frame covers the whole piano roll */
        if (g_physK >= 0) UnionRect(&g_rbBox[g_physK], &g_rbBox[g_physK], &g_physPane);
    }
    /* nothing moves: no frame (but one more when the motion just stopped) */
    BOOL moving = (g_spinMode && spin_now()) || (g_jellyMode && jelly_now()) || (g_physOn && g_physQuiet < 15);
    if (g_rainbow != RAINBOW_CYCLE && !moving && !g_spinWas) return;
    g_spinWas = moving;
    for (int i = 0; i < 8; i++) {
        HWND h = g_rbWnd[i];
        if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsRectEmpty(&g_rbBox[i])) continue;
        RECT box = g_rbBox[i], cl;
        GetClientRect(h, &cl);
        if (!IntersectRect(&box, &box, &cl)) continue;
        SetWindowSubclass(h, rainbow_subclass, RB_TIMER, 0);   /* no-op if already there */
        g_rbRepainting = TRUE;
        RedrawWindow(h, &box, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        g_rbRepainting = FALSE;
    }
}

/* the frame timer runs while anything can move */
static void anim_update(void)
{
    HWND main = A->GetMainWindow();
    if (!main) return;
    BOOL fast = g_spinMode || g_gravMode || g_jellyMode;
    if (g_rainbow == RAINBOW_CYCLE || fast) SetTimer(main, RB_TIMER, fast ? 33 : 60, rainbow_tick);
    else KillTimer(main, RB_TIMER);
}

void rainbow_set_mode(int mode)
{
    g_rainbow = mode;
    anim_update();
    engine_refresh();
}

int rainbow_mode(void) { return g_rainbow; }

void spin_set(int mode, int speed, BOOL vary)
{
    g_spinMode = mode;
    g_spinSpeed = speed >= 0 && speed < SPIN_SPEEDS ? speed : 1;
    g_spinVary = vary;
    g_spinWas = TRUE;                 /* one frame to settle whatever was showing */
    anim_update();
    engine_refresh();
}

void spin_set_playing_fn(BOOL (*fn)(void)) { g_isPlaying = fn; }

void jelly_set(int mode)
{
    g_jellyMode = mode;
    g_spinWas = TRUE;
    anim_update();
    engine_refresh();
}

void grav_set(int mode, int strength, int bounce, BOOL dragTool)
{
    BOOL newMode = mode != g_gravMode;
    g_gravMode = mode;
    g_gravStrength = strength >= 0 && strength < GRAV_STRENGTHS ? strength : 1;
    g_gravBounce = bounce >= 0 && bounce < GRAV_BOUNCES ? bounce : 1;
    g_dragTool = dragTool;
    if (!dragTool) phys_release();
    g_physQuiet = 0;                  /* new settings: let the world move again */
    if (newMode) grav_drop();
    else anim_update();
}

/* blow every box up and away, from just under the middle of the floor */
void grav_explode(void)
{
    if (!g_physOn) return;
    double cx = (g_physPane.left + g_physPane.right) / 2.0, cy = g_physPane.bottom + 60;
    for (int i = 0; i < phys_count(); i++) {
        PBody *b = phys_body(i);
        if (b->invMass == 0) continue;
        double dx = b->pos.x - cx, dy = b->pos.y - cy, d = hypot(dx, dy);
        if (d < 1) { dx = rand() % 200 - 100; dy = -100; d = hypot(dx, dy); }
        double sp = 1400 + rand() % 1200;
        b->vel.x += dx / d * sp;
        b->vel.y += dy / d * sp - 300;
        b->angVel += (rand() % 2000 - 1000) / 100.0;
    }
    g_physQuiet = 0;
}

void grav_quake(void)
{
    if (!g_physOn) return;
    g_quakeUntil = GetTickCount() + 1500;
    g_physQuiet = 0;
}

void grav_drop(void)
{
    phys_end();
    if (g_gravMode && grav_now()) phys_start();
    g_spinWas = TRUE;
    anim_update();
    engine_refresh();                 /* a full frame: the notes as they are, captured */
}

static int (WINAPI *o_FrameRect)(HDC, const RECT *, HBRUSH);
static int WINAPI h_FrameRect(HDC dc, const RECT *r, HBRUSH b) { return o_FrameRect(dc, r, use_brush(dc, b, "frame")); }
static BOOL (WINAPI *o_FillRgn)(HDC, HRGN, HBRUSH);
static BOOL WINAPI h_FillRgn(HDC dc, HRGN r, HBRUSH b) { return o_FillRgn(dc, r, use_brush(dc, b, "fill")); }
static BOOL (WINAPI *o_FrameRgn)(HDC, HRGN, HBRUSH, int, int);
static BOOL WINAPI h_FrameRgn(HDC dc, HRGN r, HBRUSH b, int w, int h) { return o_FrameRgn(dc, r, use_brush(dc, b, "frame"), w, h); }

/* Line drawing with whatever pen is current - catches the DC's default
 * black pen, which never goes through SelectObject. */
#define WITH_PEN(dc, call) do {     HGDIOBJ _cur = g_enabled ? GetCurrentObject(dc, OBJ_PEN) : NULL;     HGDIOBJ _sub = _cur ? substitute(dc, _cur, "line") : NULL;     if (_sub && _sub != _cur) o_SelectObject(dc, _sub);     r = call;     if (_sub && _sub != _cur) o_SelectObject(dc, _cur); } while (0)

static BOOL (WINAPI *o_Polyline)(HDC, const POINT *, int);
static BOOL WINAPI h_Polyline(HDC dc, const POINT *p, int n) { BOOL r; WITH_PEN(dc, o_Polyline(dc, p, n)); return r; }
static BOOL (WINAPI *o_LineTo)(HDC, int, int);
/* a spinning note's straight outline is skipped: spin_draw drew a turned one */
static BOOL spin_skip_edge(HDC dc, int x, int y)
{
    SpinNote *s;
    if (!motion_now()) return FALSE;
    Tracked *t = track_find(GetCurrentObject(dc, OBJ_PEN), FALSE);
    if (!t || (t->orig & 0xFFFFFF) != NOTE_EDGE) return FALSE;
    POINT cp;
    GetCurrentPositionEx(dc, &cp);
    if (!(s = spin_note_at(dc, cp.x, cp.y))) return FALSE;
    const RECT *r = &s->r;
    BOOL in0 = cp.x >= r->left - 1 && cp.x <= r->right && cp.y >= r->top - 1 && cp.y <= r->bottom;
    BOOL in1 = x >= r->left - 1 && x <= r->right && y >= r->top - 1 && y <= r->bottom;
    if (!in0 || !in1) return FALSE;
    MoveToEx(dc, x, y, NULL);
    return TRUE;
}

static BOOL WINAPI h_LineTo(HDC dc, int x, int y)
{
    if (spin_skip_edge(dc, x, y)) return TRUE;
    BOOL r;
    WITH_PEN(dc, o_LineTo(dc, x, y));
    return r;
}
static BOOL (WINAPI *o_PolylineTo)(HDC, const POINT *, DWORD);
static BOOL WINAPI h_PolylineTo(HDC dc, const POINT *p, DWORD n) { BOOL r; WITH_PEN(dc, o_PolylineTo(dc, p, n)); return r; }
static BOOL (WINAPI *o_PolyBezier)(HDC, const POINT *, DWORD);
static BOOL WINAPI h_PolyBezier(HDC dc, const POINT *p, DWORD n) { BOOL r; WITH_PEN(dc, o_PolyBezier(dc, p, n)); return r; }
static BOOL (WINAPI *o_PolyPolyline)(HDC, const POINT *, const DWORD *, DWORD);
static BOOL WINAPI h_PolyPolyline(HDC dc, const POINT *p, const DWORD *c, DWORD n) { BOOL r; WITH_PEN(dc, o_PolyPolyline(dc, p, c, n)); return r; }
static BOOL (WINAPI *o_PolyBezierTo)(HDC, const POINT *, DWORD);
static BOOL WINAPI h_PolyBezierTo(HDC dc, const POINT *p, DWORD n) { BOOL r; WITH_PEN(dc, o_PolyBezierTo(dc, p, n)); return r; }

/* Translucent overlays (selections) are AlphaBlends of a scratch DC;
 * recolour the source pixels through the theme first. */
static BOOL (WINAPI *o_AlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static BOOL WINAPI h_AlphaBlend(HDC dst, int dx, int dy, int dw, int dh, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION bf)
{
    if (!g_enabled || bf.AlphaFormat || sw <= 0 || sh <= 0 || sw * sh > 1 << 20)
        return o_AlphaBlend(dst, dx, dy, dw, dh, src, sx, sy, sw, sh, bf);
    int area = current_area(dst);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), sw, -sh, 1, 32, BI_RGB } };
    DWORD *px;
    HDC mem = CreateCompatibleDC(dst);
    HBITMAP dib = CreateDIBSection(dst, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!mem || !dib) { if (mem) DeleteDC(mem); return o_AlphaBlend(dst, dx, dy, dw, dh, src, sx, sy, sw, sh, bf); }
    HGDIOBJ old = o_SelectObject(mem, dib);
    BitBlt(mem, 0, 0, sw, sh, src, sx, sy, SRCCOPY);
    DWORD lastIn = 0xFFFFFFFF, lastOut = 0;
    BOOL changed = FALSE;
    for (int i = 0, n = sw * sh; i < n; i++) {
        DWORD p = px[i] & 0xFFFFFF;
        if (p != lastIn) {
            COLORREF c = RGB(p >> 16, (p >> 8) & 0xFF, p & 0xFF);
            census_hit(area, c, "blend");
            COLORREF m = remap(area, KIND_LINE, c);
            lastIn = p; lastOut = (GetRValue(m) << 16) | (GetGValue(m) << 8) | GetBValue(m);
        }
        if (lastOut != p) { px[i] = lastOut; changed = TRUE; }
    }
    BOOL r = changed ? o_AlphaBlend(dst, dx, dy, dw, dh, mem, 0, 0, sw, sh, bf)
                     : o_AlphaBlend(dst, dx, dy, dw, dh, src, sx, sy, sw, sh, bf);
    o_SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    return r;
}

/* ------------------------------------------------------- note symbols -- */
/* The vibrato squiggle and the attack marks under the notes are image-list
 * icons. comctl32 draws each tile with its own BitBlt - the mask (SRCAND),
 * then the image (SRCPAINT) - so the editor's imports never see it. Its
 * import of BitBlt is patched (patch_comctl32); the image pass is recoloured
 * through the [Sprites] table, only where the mask makes the icon opaque. */

#define SYM_MAX 4096                 /* largest tile handled, in pixels */
static BOOL (WINAPI *o_ccBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BYTE g_symMask[SYM_MAX];      /* 1 = opaque, from the mask pass just before */
static HDC g_symDst;
static int g_symX, g_symY, g_symW, g_symH;

/* a 32-bit copy of part of a DC; release with sym_free */
typedef struct { HDC mem; HBITMAP dib; HGDIOBJ old; DWORD *px; } SymBuf;

static BOOL sym_copy(SymBuf *b, HDC src, int sx, int sy, int w, int h)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    b->mem = CreateCompatibleDC(src);
    b->dib = b->mem ? CreateDIBSection(src, &bi, DIB_RGB_COLORS, (void **)&b->px, NULL, 0) : NULL;
    if (!b->dib) { if (b->mem) DeleteDC(b->mem); return FALSE; }
    b->old = o_SelectObject(b->mem, b->dib);
    BitBlt(b->mem, 0, 0, w, h, src, sx, sy, SRCCOPY);   /* a mask turns black (opaque) / white */
    GdiFlush();
    return TRUE;
}

static void sym_free(SymBuf *b)
{
    o_SelectObject(b->mem, b->old);
    o_DeleteObject(b->dib);
    DeleteDC(b->mem);
}

static COLORREF sym_lookup(COLORREF c)
{
    for (int k = 0; k < KIND_COUNT; k++) {
        COLORREF m = theme_lookup(TBL(AREA_SPRITES, k), c);
        if (m != NO_COLOR) return m;
    }
    return NO_COLOR;
}

/* The symbols are black (#000000) with a few dark greys (the line from the
 * attack mark to the vibrato, soft edges). A grey with no colour of its own
 * follows the black's colour, as much lighter as it was, so recolouring the
 * black recolours the whole symbol. */
static COLORREF sym_color(COLORREF c)
{
    COLORREF m = sym_lookup(c);
    if (m != NO_COLOR) return m;
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    if (r != g || g != b || r > 0x80 || (m = sym_lookup(RGB(0, 0, 0))) == NO_COLOR) return c;
    double f = r / 255.0;                        /* how far from black towards white */
    return RGB((int)(GetRValue(m) + (255 - GetRValue(m)) * f + 0.5), (int)(GetGValue(m) + (255 - GetGValue(m)) * f + 0.5),
               (int)(GetBValue(m) + (255 - GetBValue(m)) * f + 0.5));
}

static BOOL WINAPI h_ccBitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    if (!s || w <= 0 || h <= 0 || w * h > SYM_MAX || (rop != SRCAND && rop != SRCPAINT))
        return o_ccBitBlt(d, x, y, w, h, s, sx, sy, rop);
    int area = current_area(d);
    if (area != AREA_MUSICAL && area != AREA_TRACK) return o_ccBitBlt(d, x, y, w, h, s, sx, sy, rop);
    /* a note in the physics world leaves its squiggle behind: hide it (a
     * spinning one's stays put) */
    int ty = y;
    if (g_physOn && spin_note_at(d, x + w / 2, y - 2)) return TRUE;
    SymBuf b;
    if (rop == SRCAND) {
        g_symDst = NULL;
        if (sym_copy(&b, s, sx, sy, w, h)) {
            for (int i = 0; i < w * h; i++) g_symMask[i] = (b.px[i] & 0xFFFFFF) == 0;
            sym_free(&b);
            g_symDst = d; g_symX = x; g_symY = y; g_symW = w; g_symH = h;
        }
        return o_ccBitBlt(d, x, ty, w, h, s, sx, sy, rop);
    }
    /* the image pass of the tile whose mask we just saw */
    if (g_symDst != d || g_symX != x || g_symY != y || g_symW != w || g_symH != h || !sym_copy(&b, s, sx, sy, w, h))
        return o_ccBitBlt(d, x, ty, w, h, s, sx, sy, rop);
    g_symDst = NULL;
    BOOL changed = FALSE;
    DWORD lastIn = 0xFFFFFFFF, lastOut = 0;
    for (int i = 0; i < w * h; i++) {
        if (!g_symMask[i]) continue;
        DWORD p = b.px[i] & 0xFFFFFF;
        if (p != lastIn) {
            COLORREF c = RGB(p >> 16, (p >> 8) & 0xFF, p & 0xFF);
            census_hit(AREA_SPRITES, c, "bmp");
            COLORREF m = g_enabled ? sym_color(c) : c;
            lastIn = p; lastOut = (GetRValue(m) << 16) | (GetGValue(m) << 8) | GetBValue(m);
        }
        if (lastOut != p) { b.px[i] = lastOut; changed = TRUE; }
    }
    BOOL r = changed ? o_ccBitBlt(d, x, ty, w, h, b.mem, 0, 0, rop) : o_ccBitBlt(d, x, ty, w, h, s, sx, sy, rop);
    sym_free(&b);
    return r;
}

/* Point every comctl32 import of BitBlt at h_ccBitBlt. Its imports go
 * through API sets on newer Windows, so entries are matched by address
 * (gdi32's export or the gdi32full one it forwards to), not by DLL name. */
void engine_patch_symbols(void)
{
    if (o_ccBitBlt) return;
    void *real[2] = { (void *)GetProcAddress(GetModuleHandleA("gdi32.dll"), "BitBlt"), NULL };
    HMODULE full = GetModuleHandleA("gdi32full.dll");
    if (full) real[1] = (void *)GetProcAddress(full, "BitBlt");
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = 0;
    for (unsigned i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
        char name[MAX_PATH];
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH)) continue;
        const char *b = strrchr(name, '\\');
        if (lstrcmpiA(b ? b + 1 : name, "comctl32.dll")) continue;
        BYTE *base = (BYTE *)mods[i];
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
        if (!dir.VirtualAddress) continue;
        void **iat = (void **)(base + dir.VirtualAddress);
        for (unsigned k = 0; k < dir.Size / sizeof(void *); k++) {
            if (!iat[k] || (iat[k] != real[0] && iat[k] != real[1])) continue;
            DWORD old;
            if (!VirtualProtect(&iat[k], sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (!o_ccBitBlt) o_ccBitBlt = (BOOL (WINAPI *)(HDC, int, int, int, int, HDC, int, int, DWORD))iat[k];
            iat[k] = (void *)h_ccBitBlt;
            VirtualProtect(&iat[k], sizeof(void *), old, &old);
            n++;
        }
    }
    A->Log("uicolors: note symbols %s (%d comctl32 import%s)", n ? "themable" : "not found", n, n == 1 ? "" : "s");
}

static DWORD (WINAPI *o_GetSysColor)(int);
static DWORD WINAPI h_GetSysColor(int i)
{
    COLORREF c = o_GetSysColor(i);
    census_hit(AREA_OTHER, c, "sys");
    return remap(AREA_ALL, KIND_ANY, c);
}

static HBRUSH (WINAPI *o_GetSysColorBrush)(int);
static HBRUSH sys_brush(int index)
{
    COLORREF c = o_GetSysColor(index), m = remap(AREA_ALL, KIND_FILL, c);
    if (m == c) return o_GetSysColorBrush(index);
    return (HBRUSH)cached_object(OBJ_TBRUSH, 0, 0, m);
}
static HBRUSH WINAPI h_GetSysColorBrush(int i) { return g_enabled ? sys_brush(i) : o_GetSysColorBrush(i); }

static BOOL (WINAPI *o_EndPaint)(HWND, const PAINTSTRUCT *);
/* The frame is complete here and not yet on screen (Pitch Tools buffers the
 * piano roll's paint, and copies it out in its own EndPaint after this one):
 * the place to capture the notes for gravity, and to draw the gravity boxes. */
static BOOL WINAPI h_EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    if (ps && ps->hdc) {
        /* a repaint with no notes in it still shows the portrait */
        RECT na;
        int k = -1;
        for (int i = 0; i < 8 && k < 0; i++) if (g_rbWnd[i] == h) k = i;
        if (k < 0 && phys_pane_of(h, &na)) k = rb_index(h);
        portrait_draw(ps->hdc, k);
        if (g_physCapture && g_capK >= 0 && g_rbWnd[g_capK] == h && g_ncap) {
            phys_build(ps->hdc);
            if (g_physOn) SetWindowSubclass(h, rainbow_subclass, RB_TIMER, 0);   /* the drag tool */
        } else if (g_physOn && g_physK >= 0 && g_rbWnd[g_physK] == h) {
            /* not mid-way through one of the editor's own drags: those frames
             * show notes at in-between places */
            if (g_ncap && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { phys_add_captured(ps->hdc); g_physQuiet = 0; }
            g_ncap = 0;
            phys_draw(ps->hdc);
        }
    }
    if (g_tlsPaint != TLS_OUT_OF_INDEXES) TlsSetValue(g_tlsPaint, NULL);
    return o_EndPaint(h, ps);
}

static HDC (WINAPI *o_BeginPaint)(HWND, PAINTSTRUCT *);
static HDC WINAPI h_BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    if (g_tlsPaint != TLS_OUT_OF_INDEXES) TlsSetValue(g_tlsPaint, h);
    HDC dc = o_BeginPaint(h, ps);
    if (dc && ps) portrait_begin(h, dc, &ps->rcPaint);
    return dc;
}

/* --------------------------------------------------------------- bitmaps -- */

/* sprite: top half image, bottom half black/white mask (drawn SRCAND then
 * SRCPAINT). Only pixels the mask makes opaque may be recoloured. */
typedef struct { HBITMAP h; int w, hgt; DWORD *orig; BOOL sprite; } TintedBmp;
static TintedBmp g_bmps[1024];
static LONG g_themeGen = 1;           /* bumped by engine_refresh; derived bitmaps re-tint on change */
static int g_nbmps;

static void tint_bitmap(TintedBmp *t)
{
    int n = t->w * t->hgt;
    DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, n * 4);
    if (!px) return;
    int half = n / 2;
    for (int i = 0; i < n; i++) {
        DWORD p = t->orig[i];                       /* 0x00RRGGBB (DIB order) */
        COLORREF c = RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
        COLORREF m = NO_COLOR;
        if (t->sprite) {
            if (i >= half || (t->orig[i + half] & 0xFFFFFF) == 0xFFFFFF) { px[i] = p; continue; }
            m = theme_lookup(TBL(AREA_SPRITES, 0), c);
        }
        if (m == NO_COLOR) m = theme_lookup(TBL(AREA_BITMAPS, 0), c);
        if (m == NO_COLOR) m = theme_lookup(TBL(AREA_ALL, KIND_FILL), c);
        if (m == NO_COLOR) m = theme_lookup(TBL(AREA_ALL, 0), c);
        if (!g_enabled || m == NO_COLOR) px[i] = p;
        else px[i] = (p & 0xFF000000) | (GetRValue(m) << 16) | (GetGValue(m) << 8) | GetBValue(m);
    }
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), t->w, -t->hgt, 1, 32, BI_RGB } };
    HDC dc = GetDC(NULL);
    SetDIBits(dc, t->h, 0, t->hgt, px, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    HeapFree(GetProcessHeap(), 0, px);
}

static void register_bitmap(HBITMAP h)
{
    BITMAP bm;
    if (!h || g_nbmps >= 1024 || !GetObjectW(h, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return;
    if (bm.bmWidth * bm.bmHeight > 2048 * 2048) return;
    TintedBmp *t = &g_bmps[g_nbmps];
    t->h = h; t->w = bm.bmWidth; t->hgt = bm.bmHeight;
    t->orig = (DWORD *)HeapAlloc(GetProcessHeap(), 0, t->w * t->hgt * 4);
    if (!t->orig) return;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), t->w, -t->hgt, 1, 32, BI_RGB } };
    HDC dc = GetDC(NULL);
    int ok = GetDIBits(dc, h, 0, t->hgt, t->orig, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (!ok) { HeapFree(GetProcessHeap(), 0, t->orig); return; }
    g_nbmps++;
    int n = t->w * t->hgt, half = n / 2, black = 0, white = 0, other = 0;
    if (t->hgt >= 4 && !(t->hgt & 1))
        for (int i = half; i < n; i++) {
            DWORD p = t->orig[i] & 0xFFFFFF;
            if (p == 0) black++; else if (p == 0xFFFFFF) white++; else other++;
        }
    t->sprite = black && white && !other;
    /* census: every distinct pixel colour, weighted by pixel count */
    for (int i = 0; i < (t->sprite ? half : n); i++) {
        DWORD p = t->orig[i];
        if (t->sprite && (t->orig[i + half] & 0xFFFFFF) == 0xFFFFFF) continue;
        census_hit(t->sprite ? AREA_SPRITES : AREA_BITMAPS, RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF), "bmp");
    }
    tint_bitmap(t);
}

static HBITMAP (WINAPI *o_LoadBitmapW)(HINSTANCE, LPCWSTR);
static HBITMAP WINAPI h_LoadBitmapW(HINSTANCE inst, LPCWSTR name)
{
    HBITMAP b = o_LoadBitmapW(inst, name);
    if (b && inst) { EnterCriticalSection(&g_lock); register_bitmap(b); LeaveCriticalSection(&g_lock); }
    return b;
}

static HANDLE (WINAPI *o_LoadImageW)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
static HANDLE WINAPI h_LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT fl)
{
    HANDLE h = o_LoadImageW(inst, name, type, cx, cy, fl);
    if (h && type == IMAGE_BITMAP && inst && !(fl & LR_LOADFROMFILE)) {
        EnterCriticalSection(&g_lock); register_bitmap((HBITMAP)h); LeaveCriticalSection(&g_lock);
    }
    return h;
}

/* ------------------------------------------------------------- control -- */

static BOOL CALLBACK redraw_top(HWND h, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId())
        RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    (void)lp;
    return TRUE;
}

void engine_refresh(void)
{
    InterlockedIncrement(&g_themeGen);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_nbmps; i++) tint_bitmap(&g_bmps[i]);
    memset(g_areaCache, 0, sizeof g_areaCache);
    LeaveCriticalSection(&g_lock);
    EnumWindows(redraw_top, 0);
}

void engine_set_enabled(BOOL on) { g_enabled = on; engine_refresh(); }
int engine_bitmap_count(void) { return g_nbmps; }
BOOL engine_enabled(void) { return g_enabled; }

/* The editor scrolls the piano roll by shifting what is on screen and
 * repainting only the strip that comes into view. A portrait or the gravity
 * boxes stay put on screen, so the shifted copy smears them: while either is
 * showing, the whole scrolled area is repainted instead (buffered, no flicker). */
static int (WINAPI *o_ScrollWindowEx)(HWND, int, int, const RECT *, const RECT *, HRGN, LPRECT, UINT);
static int WINAPI h_ScrollWindowEx(HWND h, int dx, int dy, const RECT *scroll, const RECT *clip, HRGN rgnUpdate, LPRECT rcUpdate, UINT flags)
{
    RECT pane;
    if ((dx || dy) && (portrait_showing() || g_physOn) && phys_pane_of(h, &pane)) {
        RECT r;
        if (scroll) r = *scroll; else GetClientRect(h, &r);
        if (clip) IntersectRect(&r, &r, clip);
        RedrawWindow(h, &r, NULL, RDW_INVALIDATE | RDW_ERASE);
        if (rgnUpdate) SetRectRgn(rgnUpdate, r.left, r.top, r.right, r.bottom);
        if (rcUpdate) *rcUpdate = r;
        return SIMPLEREGION;
    }
    return o_ScrollWindowEx(h, dx, dy, scroll, clip, rgnUpdate, rcUpdate, flags);
}

/* ----------------------------------------------------- derived bitmaps -- */
/* The piano roll's rows come from a small octave picture the editor builds
 * itself (84 x 12 rows) when the view is set up, in whatever colours the
 * theme had then, and stretches across the roll from then on. Its colours are
 * baked in, so a later theme change never reached it. Such pictures are
 * tracked here: their original colours are worked out by undoing the theme
 * they were drawn with, and they are re-tinted in place when the theme
 * changes. A picture the editor redraws (zoom) is simply picked up again. */

typedef struct { HBITMAP h; int w, hh; DWORD *orig, *last; BYTE *line; LONG gen; } Derived;
#define MAX_DERIVED 16
static Derived g_derived[MAX_DERIVED];

/* the colour this pixel had before the theme replaced it; *line set if it came from a Lines table */
static DWORD un_theme(DWORD px, BYTE *line)
{
    COLORREF c = RGB((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
    static const int tbls[6] = { TBL(AREA_MUSICAL, KIND_FILL), TBL(AREA_MUSICAL, KIND_ANY), TBL(AREA_ALL, KIND_FILL), TBL(AREA_ALL, KIND_ANY),
                                 TBL(AREA_MUSICAL, KIND_LINE), TBL(AREA_ALL, KIND_LINE) };
    *line = 0;
    for (int t = 0; t < 6; t++) {
        COLORREF o, r;
        for (int i = 0; theme_entry(tbls[t], i, &o, &r); i++)
            if ((r & 0xFFFFFF) == c) { *line = t >= 4; return (GetRValue(o) << 16) | (GetGValue(o) << 8) | GetBValue(o); }
    }
    return px & 0xFFFFFF;
}

static BOOL derived_read(HDC src, int w, int h, DWORD *out)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    DWORD *bits;
    HDC mem = CreateCompatibleDC(src);
    HBITMAP dib = mem ? CreateDIBSection(src, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0) : NULL;
    if (!dib) { if (mem) DeleteDC(mem); return FALSE; }
    HGDIOBJ old = o_SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    GdiFlush();
    for (int i = 0; i < w * h; i++) out[i] = bits[i] & 0xFFFFFF;
    o_SelectObject(mem, old);
    o_DeleteObject(dib);
    DeleteDC(mem);
    return TRUE;
}

/* called before the editor stretches a picture of its own across the roll */
static void derived_check(HDC src, int area)
{
    HBITMAP b = (HBITMAP)GetCurrentObject(src, OBJ_BITMAP);
    BITMAP bm;
    if (!b || !GetObjectW(b, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0 || bm.bmWidth * bm.bmHeight > 256 * 2048) return;
    for (int i = 0; i < g_nbmps; i++) if (g_bmps[i].h == b) return;     /* a loaded bitmap: tinted already */
    int n = bm.bmWidth * bm.bmHeight, k = -1;
    for (int i = 0; i < MAX_DERIVED && k < 0; i++) if (g_derived[i].h == b && g_derived[i].w == bm.bmWidth && g_derived[i].hh == bm.bmHeight) k = i;
    DWORD *cur = (DWORD *)HeapAlloc(GetProcessHeap(), 0, n * 4);
    if (!cur || !derived_read(src, bm.bmWidth, bm.bmHeight, cur)) { if (cur) HeapFree(GetProcessHeap(), 0, cur); return; }
    Derived *d;
    if (k < 0 || memcmp(cur, g_derived[k].last, n * 4)) {
        /* new, or the editor redrew it with the theme as it is now: work out its originals */
        static int next;
        if (k < 0) { k = next; next = (next + 1) % MAX_DERIVED; }
        d = &g_derived[k];
        if (d->orig) HeapFree(GetProcessHeap(), 0, d->orig);
        if (d->last) HeapFree(GetProcessHeap(), 0, d->last);
        if (d->line) HeapFree(GetProcessHeap(), 0, d->line);
        d->h = b; d->w = bm.bmWidth; d->hh = bm.bmHeight; d->gen = g_themeGen;
        d->orig = (DWORD *)HeapAlloc(GetProcessHeap(), 0, n * 4);
        d->line = (BYTE *)HeapAlloc(GetProcessHeap(), 0, n);
        d->last = cur;
        if (!d->orig || !d->line) { d->h = NULL; return; }
        DWORD lastIn = 0xFFFFFFFF, lastOut = 0;
        BYTE lastLine = 0;
        for (int i = 0; i < n; i++) {
            if (cur[i] != lastIn) { lastIn = cur[i]; lastOut = un_theme(cur[i], &lastLine); }
            d->orig[i] = lastOut;
            d->line[i] = lastLine;
        }
        return;
    }
    HeapFree(GetProcessHeap(), 0, cur);
    d = &g_derived[k];
    if (d->gen == g_themeGen) return;
    /* the theme changed since: tint it again from its originals, in place */
    d->gen = g_themeGen;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), d->w, -d->hh, 1, 32, BI_RGB } };
    for (int i = 0; i < n; i++) {
        DWORD o = d->orig[i];
        COLORREF m = remap(area, d->line[i] ? KIND_LINE : KIND_FILL, RGB((o >> 16) & 0xFF, (o >> 8) & 0xFF, o & 0xFF));
        d->last[i] = (GetRValue(m) << 16) | (GetGValue(m) << 8) | GetBValue(m);
    }
    SetDIBitsToDevice(src, 0, 0, d->w, d->hh, 0, 0, 0, d->hh, d->last, &bi, DIB_RGB_COLORS);
}

static BOOL (WINAPI *o_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
static BOOL WINAPI h_StretchBlt(HDC d, int x, int y, int w, int hh, HDC s2, int sx, int sy, int sw, int sh, DWORD rop)
{
    int area = current_area(d);
    if (g_enabled && s2 && (area == AREA_MUSICAL || area == AREA_TRACK)) {
        derived_check(s2, area);
        portrait_outside_paint(d);
    }
    return o_StretchBlt(d, x, y, w, hh, s2, sx, sy, sw, sh, rop);
}

#define HOOK(dll, fn) A->HookImport(dll, #fn, (void *)h_##fn, (void **)&o_##fn)

BOOL engine_install(void)
{
    InitializeCriticalSection(&g_lock);
    g_tlsPaint = TlsAlloc();
    BOOL ok = HOOK("gdi32.dll", CreateSolidBrush) & HOOK("gdi32.dll", CreatePen) &
              HOOK("gdi32.dll", SelectObject) & HOOK("gdi32.dll", DeleteObject) &
              HOOK("gdi32.dll", SetBkColor) & HOOK("gdi32.dll", SetTextColor) &
              HOOK("user32.dll", FillRect) & HOOK("user32.dll", GetSysColor);
    HOOK("gdi32.dll", CreateHatchBrush);
    HOOK("gdi32.dll", SetPixel);
    HOOK("gdi32.dll", SetPixelV);
    HOOK("user32.dll", ScrollWindowEx);
    HOOK("gdi32.dll", StretchBlt);
    HOOK("gdi32.dll", ExtTextOutW);
    HOOK("gdi32.dll", ExtTextOutA);
    HOOK("gdi32.dll", TextOutW);
    HOOK("user32.dll", DrawTextW);
    HOOK("user32.dll", DrawTextExW);
    HOOK("gdi32.dll", FillRgn);
    HOOK("gdi32.dll", FrameRgn);
    HOOK("user32.dll", FrameRect);
    HOOK("user32.dll", GetSysColorBrush);
    HOOK("user32.dll", BeginPaint);
    HOOK("user32.dll", EndPaint);
    HOOK("gdi32.dll", Polyline);
    HOOK("gdi32.dll", LineTo);
    HOOK("gdi32.dll", PolylineTo);
    HOOK("gdi32.dll", PolyBezierTo);
    HOOK("gdi32.dll", PolyBezier);
    HOOK("msimg32.dll", AlphaBlend);
    HOOK("gdi32.dll", PolyPolyline);
    HOOK("gdi32.dll", ExtCreatePen);
    HOOK("gdi32.dll", CreatePenIndirect);
    HOOK("gdi32.dll", CreateBrushIndirect);
    HOOK("gdi32.dll", CreateCompatibleDC);
    HOOK("gdi32.dll", DeleteDC);
    HOOK("user32.dll", LoadBitmapW);
    HOOK("user32.dll", LoadImageW);
    return ok;
}

int engine_area_of_window(HWND h) { return classify_hwnd(h); }
