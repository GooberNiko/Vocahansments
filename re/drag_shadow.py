root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/'
m = root + 'src/mods/pitchtools/pitchmodel.c'
t = open(m).read()


def sub(s, old, new):
    if old not in s:
        raise SystemExit('missing: ' + old[:160])
    return s.replace(old, new, 1)


t = sub(t, """typedef struct {
    long tOn, tOff;           /* absolute ticks */
    double on, off;           /* ms */
    int num, dep, blen, rise, vibType, vibLen;
} PMNote;""", """typedef struct {
    long tOn, tOff;           /* absolute ticks */
    double on, off;           /* ms */
    int num, dep, blen, rise, vibType, vibLen;
    unsigned id;              /* VSQ object id */
} PMNote;""")
t = sub(t, """        m->num = r.num;
        m->dep = 8;""", """        m->num = r.num;
        m->id = r.id;
        m->dep = 8;""")

# drag shadow
t = sub(t, """/* ------------------------------------------------------------ drawing -- */""", """/* ------------------------------------------------------- drag shadow -- */
/* While the user drags notes with the editor's own tools nothing is committed
 * until mouse-up, so the curve follows a shadow copy of the score with the
 * drag applied (same quantize snapping as the editor). */

enum { DRAG_NONE, DRAG_MOVE, DRAG_START, DRAG_END };
static int g_dragMode;
static int g_dragX0, g_dragY0;
static long g_dragDT;
static int g_dragDN;
static unsigned g_dragIds[1024];
static int g_dragNids;
static PMNote *g_shadow;
static int g_shadowCap;

#define V4_PRP_MODEL   0x100         /* CMusicalPart model: note items list */
#define V4_VT_CUIAnote 0x00564148

/* ids of the notes the editor shows as selected */
static int selected_ids(unsigned *out, int max)
{
    BYTE *model = *(BYTE **)(PANE() + V4_PRP_MODEL);
    int k = 0;
    if (!model) return 0;
    for (BYTE **node = *(BYTE ***)(model + 0x3C); node && k < max; node = (BYTE **)node[0]) {
        BYTE *item = node[2];
        if (item && *(DWORD *)item == V4_VT_CUIAnote && *(int *)(item + 0x10)) out[k++] = *(unsigned *)(item + 4);
    }
    return k;
}

static void pm_drag_begin(int x, int y)
{
    g_dragMode = DRAG_NONE;
    if (!pm_refresh()) return;
    long tick = tick_at_x(x);
    int num = note_at_y(y);
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *n = &g_pmNotes[i];
        if (n->num != num || tick < n->tOn || tick >= n->tOff) continue;
        int xs = x_of_tick(n->tOn), xe = x_of_tick(n->tOff);
        g_dragMode = x - xs <= 4 ? DRAG_START : (xe - x <= 4 ? DRAG_END : DRAG_MOVE);
        g_dragX0 = x; g_dragY0 = y; g_dragDT = 0; g_dragDN = 0;
        g_dragNids = selected_ids(g_dragIds, 1024);
        BOOL in = FALSE;
        for (int k = 0; k < g_dragNids; k++) if (g_dragIds[k] == n->id) in = TRUE;
        if (!in) { g_dragIds[0] = n->id; g_dragNids = 1; }   /* clicking an unselected note selects just it */
        return;
    }
}

static BOOL pm_drag_update(int x, int y)
{
    if (!g_dragMode) return FALSE;
    long q = quantize_ticks();
    long dt = tick_at_x(x) - tick_at_x(g_dragX0);
    if (q > 1) dt = (dt >= 0 ? (dt + q / 2) / q : -((-dt + q / 2) / q)) * q;
    int dn = g_dragMode == DRAG_MOVE ? note_at_y(y) - note_at_y(g_dragY0) : 0;
    if (dt == g_dragDT && dn == g_dragDN) return FALSE;
    g_dragDT = dt; g_dragDN = dn;
    return TRUE;
}

static void pm_drag_end(void) { g_dragMode = DRAG_NONE; }

static int cmp_pm_on(const void *a, const void *b)
{
    long x = ((const PMNote *)a)->tOn, y = ((const PMNote *)b)->tOn;
    return x < y ? -1 : x > y;
}

/* returns the array to draw from (shadow while dragging) */
static PMNote *pm_build_shadow(void)
{
    if (!g_dragMode || (!g_dragDT && !g_dragDN)) return g_pmNotes;
    if (g_shadowCap < g_pmCount) {
        g_shadowCap = g_pmCount + 64;
        g_shadow = (PMNote *)HeapReAlloc(GetProcessHeap(), 0, g_shadow ? g_shadow : HeapAlloc(GetProcessHeap(), 0, 16), g_shadowCap * sizeof(PMNote));
    }
    memcpy(g_shadow, g_pmNotes, g_pmCount * sizeof(PMNote));
    for (int i = 0; i < g_pmCount; i++) {
        PMNote *n = &g_shadow[i];
        BOOL sel = FALSE;
        for (int k = 0; k < g_dragNids && !sel; k++) sel = g_dragIds[k] == n->id;
        if (!sel) continue;
        if (g_dragMode == DRAG_MOVE) { n->tOn += g_dragDT; n->tOff += g_dragDT; n->num += g_dragDN; }
        else if (g_dragMode == DRAG_START) { if (n->tOn + g_dragDT < n->tOff - 5) n->tOn += g_dragDT; }
        else { if (n->tOff + g_dragDT > n->tOn + 5) n->tOff += g_dragDT; }
        if (n->num < 0) n->num = 0;
        if (n->num > 127) n->num = 127;
    }
    qsort(g_shadow, g_pmCount, sizeof(PMNote), cmp_pm_on);
    for (int i = 0; i < g_pmCount; i++) { g_shadow[i].on = pm_ms(g_shadow[i].tOn); g_shadow[i].off = pm_ms(g_shadow[i].tOff); }
    return g_shadow;
}

/* ------------------------------------------------------------ drawing -- */""")
t = sub(t, """static void pm_draw(HDC dc)
{
    if (!g_pmShow || !g_view || !pm_refresh()) return;""", """static void pm_draw(HDC dc)
{
    if (!g_pmShow || !g_view || !pm_refresh()) return;
    PMNote *real = g_pmNotes;
    g_pmNotes = pm_build_shadow();""")
t = sub(t, """    SelectObject(dc, old);
    DeleteObject(pen);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static void pm_invalidate(void)""", """    SelectObject(dc, old);
    DeleteObject(pen);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
    g_pmNotes = real;
}

static void pm_invalidate(void)""")
open(m, 'w').write(t)

p = root + 'src/mods/pitchtools/pitchtools.c'
s = open(p).read()
s = sub(s, """    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (g_active && g_view && in_note_area(x, y)) { begin_stroke(h, x, y, msg == WM_RBUTTONDOWN); return 0; }
        break;""", """    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (g_active && g_view && in_note_area(x, y)) { begin_stroke(h, x, y, msg == WM_RBUTTONDOWN); return 0; }
        if (msg == WM_LBUTTONDOWN && g_view && g_pmShow && in_note_area(x, y)) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);      /* let the editor select first */
            pm_drag_begin(x, y);
            return r;
        }
        break;""")
s = sub(s, """    case WM_MOUSEMOVE:
        if (g_drawing) {""", """    case WM_MOUSEMOVE:
        if (!g_drawing && g_dragMode && (wp & MK_LBUTTON)) {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            if (pm_drag_update(x, y)) { RECT na = note_area(); InvalidateRect(h, &na, FALSE); }
            return r;
        }
        if (g_drawing) {""")
s = sub(s, """    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_drawing) { end_stroke(h); return 0; }
        break;""", """    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_drawing) { end_stroke(h); return 0; }
        if (g_dragMode) pm_drag_end();
        break;""")
s = sub(s, """    case WM_CAPTURECHANGED:""", """    case WM_CAPTURECHANGED:
        if (g_dragMode && (HWND)lp != h) { pm_drag_end(); InvalidateRect(h, NULL, FALSE); }""")
open(p, 'w').write(s)
print('ok')
