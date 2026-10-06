root = 'C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/'
p = root + 'src/mods/pitchtools/pitchtools.c'
s = open(p).read()


def sub(old, new, cnt=1):
    global s
    if old not in s:
        raise SystemExit('missing: ' + old[:160])
    s = s.replace(old, new, cnt)


# 1. include the model right after the shared editor header
sub('#include "../../common/v4_editor.h"\n', '#include "../../common/v4_editor.h"\n#include "pitchmodel.c"\n')

# 2. every commit/undo/redo now just redraws the real-time curve
sub("""static void schedule_render(void)
{
    (void)live_timer;
}""", """static void schedule_render(void)
{
    (void)live_timer;
    pm_invalidate();
}""")

# 3. draw on every repaint of the piano roll
sub("""    case WM_PAINT: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (g_drawing) { HDC dc = GetDC(h); draw_stroke(dc); ReleaseDC(h, dc); }
        return r;
    }""", """    case WM_PAINT: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        HDC dc = GetDC(h);
        pm_draw(dc);
        if (g_drawing) draw_stroke(dc);
        ReleaseDC(h, dc);
        return r;
    }""")

# 4. menu: the live item now toggles the real-time curve
sub("""static void __cdecl menu_live(void *c)
{
    (void)c;
    g_live = !g_live;
    A->CheckMenuItem(g_cmdLive, g_live);
    vh_ini_set_int(g_ini, L"PitchTools", L"LiveRender", g_live);
    if (g_live) schedule_render();
}""", """static void __cdecl menu_live(void *c)
{
    (void)c;
    g_pmShow = !g_pmShow;
    A->CheckMenuItem(g_cmdLive, g_pmShow);
    vh_ini_set_int(g_ini, L"PitchTools", L"RealtimePitch", g_pmShow);
    if (g_viewHwnd) InvalidateRect(g_viewHwnd, NULL, FALSE);
}""")
sub('g_cmdLive = A->AddMenuItem(L"Pitch: live pitch render", menu_live, NULL);',
    'g_cmdLive = A->AddMenuItem(L"Pitch: real-time pitch curve", menu_live, NULL);')
sub('A->CheckMenuItem(g_cmdLive, g_live);\n    A->CheckMenuItem(g_cmdExact', 'A->CheckMenuItem(g_cmdLive, g_pmShow);\n    A->CheckMenuItem(g_cmdExact')
sub('g_live = vh_ini_int(g_ini, L"PitchTools", L"RealtimePitch", 1);',
    'g_pmShow = vh_ini_int(g_ini, L"PitchTools", L"RealtimePitch", 1);')

# 5. exact pen: solve against the model in one shot (no renders, no windows)
sub('g_exactPasses = 0;   /* model-based correction replaces render passes */',
    'g_exactPasses = vh_ini_int(g_ini, L"PitchTools", L"PenExact", 1);')
a = s.index('    /* Correction passes: write, render, measure what VOCALOID really sings')
b = s.index('    int written = 0;\n    ok = write_pitch(&c, dev, &written);')
s = s[:a] + """    /* Exact mode: PIT is additive, so subtract what the model says VOCALOID
     * sings without PIT (note + portamento + bend [+ vibrato]) from the target. */
    double worst = 0;
    if (!g_erasing && g_exactPasses && pm_refresh()) {
        for (int s2 = 0; s2 < nslots; s2++) {
            double sung;
            if (!set[s2] || !pm_pitch_at(t0 + (long)s2 * SAMPLE_STEP, !g_keepVibrato, FALSE, &sung)) continue;
            dev[s2] = target[s2] - sung;
            if (dev[s2] > 24) dev[s2] = 24;
            if (dev[s2] < -24) dev[s2] = -24;
        }
    }
""" + s[b:]
sub('A->Log("pitch: %s %ld..%ld, %d PIT events, last error %.1f cents", g_erasing ? "erased" : "drew", t0, t1, written, worst * 100);',
    'A->Log("pitch: %s %ld..%ld, %d PIT events", g_erasing ? "erased" : "drew", t0, t1, written);\n    (void)worst;')
sub("""    g_exactPasses = g_exactPasses ? 0 : 2;
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenExactPasses", g_exactPasses);""", """    g_exactPasses = !g_exactPasses;
    A->CheckMenuItem(g_cmdExact, g_exactPasses != 0);
    vh_ini_set_int(g_ini, L"PitchTools", L"PenExact", g_exactPasses);""")
open(p, 'w').write(s)

m = root + 'src/mods/pitchtools/pitchmodel.c'
t = open(m).read()
t = t.replace("""        int y = y_of_pitch(p - 0.5);   /* y_of_pitch maps row centres; p is a note number */
        if (!down) { MoveToEx(dc, x, y, NULL); down = TRUE; }
        else if (abs(y - lastY) > 0 || x == na.right - 1) LineTo(dc, x, y);
        else LineTo(dc, x, y);
        lastY = y;""", """        int y = y_of_pitch(p);         /* note number -> row centre */
        if (!down) { MoveToEx(dc, x, y, NULL); down = TRUE; }
        else LineTo(dc, x, y);""")
t = t.replace("    BOOL down = FALSE;\n    int lastY = 0;\n", "    BOOL down = FALSE;\n")
open(m, 'w').write(t)
print('ok')
