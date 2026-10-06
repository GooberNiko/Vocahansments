import re
root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/src/mods/uicolors/'


def sub(s, old, new):
    if old not in s:
        raise SystemExit('pattern not found:\n' + old[:200])
    return s.replace(old, new)


p = root + 'engine.h'
s = open(p).read()
s = sub(s, """extern const wchar_t *const kAreaNames[AREA_COUNT];

#define NO_COLOR 0xFFFFFFFFu

/* theme: original RGB -> replacement RGB, per area (COLORREF, 0x00BBGGRR) */
COLORREF theme_lookup(int area, COLORREF orig);   /* NO_COLOR if unmapped */
void theme_set(int area, COLORREF orig, COLORREF repl);
void theme_clear_entry(int area, COLORREF orig);
void theme_clear(void);
BOOL theme_load(const wchar_t *path, wchar_t *nameOut, int nameLen);
BOOL theme_save(const wchar_t *path, const wchar_t *name);
int theme_count(int area);
BOOL theme_entry(int area, int index, COLORREF *orig, COLORREF *repl);

COLORREF remap(int area, COLORREF c);

/* census of colours the editor really uses, so themes can be built from it */
typedef struct { COLORREF color; int area; unsigned count; char what[12]; } CensusEntry;""",
"""extern const wchar_t *const kAreaNames[AREA_COUNT];

/* What a colour is used for. A theme table is (area, kind); lookups fall back
 * area+kind -> area -> All+kind -> All. Sections: [Musical], [Musical.Lines]... */
enum { KIND_ANY = 0, KIND_FILL, KIND_LINE, KIND_TEXT, KIND_COUNT };
extern const wchar_t *const kKindNames[KIND_COUNT];
#define TBL(area, kind) ((area) * KIND_COUNT + (kind))
#define TBL_COUNT (AREA_COUNT * KIND_COUNT)

#define NO_COLOR 0xFFFFFFFFu

/* theme: original RGB -> replacement RGB per table (COLORREF, 0x00BBGGRR) */
COLORREF theme_lookup(int tbl, COLORREF orig);   /* NO_COLOR if unmapped */
void theme_set(int tbl, COLORREF orig, COLORREF repl);
void theme_clear_entry(int tbl, COLORREF orig);
void theme_clear(void);
BOOL theme_load(const wchar_t *path, wchar_t *nameOut, int nameLen);
BOOL theme_save(const wchar_t *path, const wchar_t *name);
int theme_count(int tbl);
BOOL theme_entry(int tbl, int index, COLORREF *orig, COLORREF *repl);

COLORREF remap(int area, int kind, COLORREF c);

/* census of colours the editor really uses, so themes can be built from it */
typedef struct { COLORREF color; int area; int kind; unsigned count; char what[12]; } CensusEntry;""")
open(p, 'w').write(s)

p = root + 'engine.c'
s = open(p).read()
s = sub(s, """const wchar_t *const kAreaNames[AREA_COUNT] = {
    L"All", L"Musical", L"Track", L"Mixer", L"Bars", L"Other", L"Bitmaps",
};""", """const wchar_t *const kAreaNames[AREA_COUNT] = {
    L"All", L"Musical", L"Track", L"Mixer", L"Bars", L"Other", L"Bitmaps",
};
const wchar_t *const kKindNames[KIND_COUNT] = { L"", L"Fill", L"Lines", L"Text" };""")
s = sub(s, "static ThemeSlot g_theme[AREA_COUNT][THEME_SLOTS];\nstatic int g_themeCount[AREA_COUNT];",
        "static ThemeSlot g_theme[TBL_COUNT][THEME_SLOTS];\nstatic int g_themeCount[TBL_COUNT];")
s = sub(s, """BOOL theme_load(const wchar_t *path, wchar_t *nameOut, int nameLen)
{
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    theme_clear();
    if (nameOut) GetPrivateProfileStringW(L"Theme", L"Name", L"", nameOut, nameLen, path);
    static wchar_t buf[65536];
    for (int a = 0; a < AREA_COUNT; a++) {
        DWORD n = GetPrivateProfileSectionW(kAreaNames[a], buf, 65536, path);""",
"""static void table_section(int tbl, wchar_t *out)
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
        DWORD n = GetPrivateProfileSectionW(sec, buf, 65536, path);""")
s = sub(s, """    for (int a = 0; a < AREA_COUNT; a++) {
        if (!g_themeCount[a]) continue;
        fwprintf(f, L"\\n[%ls]\\n", kAreaNames[a]);""",
"""    for (int a = 0; a < TBL_COUNT; a++) {
        if (!g_themeCount[a]) continue;
        wchar_t sec[40];
        table_section(a, sec);
        fwprintf(f, L"\\n[%ls]\\n", sec);""")
s = sub(s, """COLORREF remap(int area, COLORREF c)
{
    if (!g_enabled || (c & 0xFF000000)) return c;   /* palette-relative / CLR_INVALID untouched */
    COLORREF r = NO_COLOR;
    if (area != AREA_ALL) r = theme_lookup(area, c);
    if (r == NO_COLOR) r = theme_lookup(AREA_ALL, c);
    return r == NO_COLOR ? c : r;
}""", """COLORREF remap(int area, int kind, COLORREF c)
{
    if (!g_enabled || (c & 0xFF000000)) return c;   /* palette-relative / CLR_INVALID untouched */
    COLORREF r = NO_COLOR;
    if (kind) r = theme_lookup(TBL(area, kind), c);
    if (r == NO_COLOR && area != AREA_ALL) r = theme_lookup(TBL(area, 0), c);
    if (r == NO_COLOR && kind) r = theme_lookup(TBL(AREA_ALL, kind), c);
    if (r == NO_COLOR) r = theme_lookup(TBL(AREA_ALL, 0), c);
    return r == NO_COLOR ? c : r;
}""")
s = sub(s, """static void census_hit(int area, COLORREF c, const char *what)
{
    if (c & 0xFF000000) return;
    DWORD key = (c & 0xFFFFFF) | (area << 24) | (what[0] << 28);
    for (unsigned i = hash32(key), n = 0; n < 64; i++, n++) {
        CensusEntry *e = &g_census[i & (CENSUS_SLOTS - 1)];
        if (e->count && e->color == (c & 0xFFFFFF) && e->area == area && e->what[0] == what[0]) { e->count++; return; }
        if (!e->count) {
            if (g_censusN >= CENSUS_SLOTS * 3 / 4) return;
            e->color = c & 0xFFFFFF; e->area = area; e->count = 1;""",
"""static int kind_of(const char *what)
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
            e->color = c & 0xFFFFFF; e->area = area; e->count = 1; e->kind = kind_of(what);""")
s = sub(s, """    int area = current_area(dc);
    census_hit(area, t->orig, what);
    COLORREF want = remap(area, t->orig);""", """    int area = current_area(dc);
    if (!strcmp(what, "select")) what = t->type == OBJ_TPEN ? "line" : "fill";
    census_hit(area, t->orig, what);
    COLORREF want = remap(area, kind_of(what), t->orig);""")
s = sub(s, """    COLORREF made = remap(AREA_ALL, c);
    HBRUSH b = o_CreateSolidBrush(made);""", """    COLORREF made = remap(AREA_ALL, KIND_FILL, c);
    HBRUSH b = o_CreateSolidBrush(made);""")
s = sub(s, """    COLORREF made = remap(AREA_ALL, c);
    HPEN p = o_CreatePen(style, width, made);""", """    COLORREF made = remap(AREA_ALL, KIND_LINE, c);
    HPEN p = o_CreatePen(style, width, made);""")
s = sub(s, "return o_CreateHatchBrush(s, remap(AREA_ALL, c));", "return o_CreateHatchBrush(s, remap(AREA_ALL, KIND_FILL, c));")
s = sub(s, "return o_SetBkColor(dc, remap(a, c));", "return o_SetBkColor(dc, remap(a, KIND_FILL, c));")
s = sub(s, "return o_SetTextColor(dc, remap(a, c));", "return o_SetTextColor(dc, remap(a, KIND_TEXT, c));")
s = sub(s, "return o_SetPixel(dc, x, y, remap(current_area(dc), c));", "return o_SetPixel(dc, x, y, remap(current_area(dc), KIND_LINE, c));")
s = sub(s, "return o_SetPixelV(dc, x, y, remap(current_area(dc), c));", "return o_SetPixelV(dc, x, y, remap(current_area(dc), KIND_LINE, c));")
s = sub(s, """    census_hit(AREA_OTHER, c, "sys");
    return remap(AREA_ALL, c);""", """    census_hit(AREA_OTHER, c, "sys");
    return remap(AREA_ALL, KIND_ANY, c);""")
s = sub(s, "COLORREF c = o_GetSysColor(index), m = remap(AREA_ALL, c);", "COLORREF c = o_GetSysColor(index), m = remap(AREA_ALL, KIND_FILL, c);")
s = sub(s, """        COLORREF m = theme_lookup(AREA_BITMAPS, c);
        if (m == NO_COLOR) m = theme_lookup(AREA_ALL, c);""", """        COLORREF m = theme_lookup(TBL(AREA_BITMAPS, 0), c);
        if (m == NO_COLOR) m = theme_lookup(TBL(AREA_ALL, KIND_FILL), c);
        if (m == NO_COLOR) m = theme_lookup(TBL(AREA_ALL, 0), c);""")
open(p, 'w').write(s)
print('engine ok')
