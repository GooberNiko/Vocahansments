root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/'
p = root + 'src/mods/pitchtools/pitchtools.c'
s = open(p).read()


def sub(old, new):
    global s
    if old not in s:
        raise SystemExit('missing: ' + old[:120])
    s = s.replace(old, new, 1)


# 1. replace base_note_at .. end of old apply_stroke with v2
a = s.index('/* the note a tick belongs to')
b = s.index('/* ---------------------------------------------------------- the stroke -- */')
new = open(root + 're/apply_stroke_v2.c').read()
s = s[:a] + 'static int cmp_ctrl(const void *a, const void *b) { return ((const ControlRef *)a)->pos - ((const ControlRef *)b)->pos; }\n\n' + new + s[b:]

# 2. settings
sub("""static BOOL g_live = TRUE;
static int g_liveDelay = 350;          /* ms after the last change */
static UINT g_cmdLive, g_cmdPen;""", """static BOOL g_live = TRUE;
static int g_liveDelay = 350;          /* ms after the last change */
static int g_exactPasses = 2;          /* pitch pen correction passes (0 = raw PIT) */
static BOOL g_keepVibrato;             /* pen ignores the note's vibrato instead of cancelling it */
static UINT g_cmdLive, g_cmdPen, g_cmdExact, g_cmdVib;""")
sub("""    if (g_liveDelay < 50) g_liveDelay = 50;""", """    if (g_liveDelay < 50) g_liveDelay = 50;
    g_exactPasses = vh_ini_int(g_ini, L"PitchTools", L"PenExactPasses", 2);
    g_keepVibrato = vh_ini_int(g_ini, L"PitchTools", L"PenKeepVibrato", 0);""")

# 3. live render: suppression + adaptive idle
sub("""static BOOL g_rendering;
static unsigned g_lastPart = 0xFFFFFFFF;

static VOID CALLBACK live_timer(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    (void)msg; (void)t;
    /* wait while the user is still dragging something */
    if (GetCapture() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
    KillTimer(h, id);
    if (!g_live) return;
    g_rendering = TRUE;
    SendMessageW(h, WM_COMMAND, V4_CMD_PITCH_RENDER, 0);
    g_rendering = FALSE;
}

static void schedule_render(void)
{
    HWND main = A->GetMainWindow();
    if (!g_live || !main || g_rendering) return;
    SetTimer(main, LIVE_TIMER, (UINT)g_liveDelay, live_timer);   /* re-arming restarts the delay */
}""", """static BOOL g_rendering, g_suppress;
static DWORD g_lastRenderMs;
static unsigned g_lastPart = 0xFFFFFFFF;
static void render_now(void);

/* Rendering blocks the editor for as long as it takes, so the longer the
 * part renders, the longer we wait for the user to pause before doing it. */
static DWORD idle_needed(void)
{
    DWORD d = (DWORD)g_liveDelay;
    if (g_lastRenderMs * 3 > d) d = g_lastRenderMs * 3;
    return d > 4000 ? 4000 : d;
}

static VOID CALLBACK live_timer(HWND h, UINT msg, UINT_PTR id, DWORD t)
{
    (void)msg; (void)t;
    /* never while dragging, and for slow renders only once the user is idle */
    if (GetCapture() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) return;
    LASTINPUTINFO li = { sizeof li };
    if (g_lastRenderMs > 150 && GetLastInputInfo(&li) && GetTickCount() - li.dwTime < idle_needed()) return;
    KillTimer(h, id);
    if (g_live) render_now();
}

static void schedule_render(void)
{
    HWND main = A->GetMainWindow();
    if (!g_live || !main || g_rendering || g_suppress) return;
    SetTimer(main, LIVE_TIMER, idle_needed(), live_timer);   /* re-arming restarts the delay */
}""")

# 4. menus
sub("""static void __cdecl on_ui_ready(HWND mainWnd, void *ctx)
{
    (void)ctx;
    g_cmdPen = A->AddMenuItem(L"Pitch Pen\\tP", menu_pen, NULL);
    g_cmdLive = A->AddMenuItem(L"Pitch: live pitch render", menu_live, NULL);
    A->CheckMenuItem(g_cmdLive, g_live);""", """static void __cdecl menu_exact(void *c)
{
    (void)c;
    g_exactPasses = g_exactPasses ? 0 : 2;
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenExactPasses", g_exactPasses);
}

static void __cdecl menu_vib(void *c)
{
    (void)c;
    g_keepVibrato = !g_keepVibrato;
    A->CheckMenuItem(g_cmdVib, g_keepVibrato);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenKeepVibrato", g_keepVibrato);
}

static void __cdecl on_ui_ready(HWND mainWnd, void *ctx)
{
    (void)ctx;
    g_cmdPen = A->AddMenuItem(L"Pitch Pen\\tP", menu_pen, NULL);
    g_cmdExact = A->AddMenuItem(L"Pitch Pen: exact (compensate portamento/vibrato)", menu_exact, NULL);
    g_cmdVib = A->AddMenuItem(L"Pitch Pen: keep note vibrato on top", menu_vib, NULL);
    g_cmdLive = A->AddMenuItem(L"Pitch: live pitch render", menu_live, NULL);
    A->CheckMenuItem(g_cmdLive, g_live);
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    A->CheckMenuItem(g_cmdVib, g_keepVibrato);""")
open(p, 'w').write(s)
print('ok')
