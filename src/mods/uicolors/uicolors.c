/*
 * UI Colors - recolour the VOCALOID4 editor: piano roll, notes, tracks,
 * rulers, bars, mixer and the built-in bitmaps. Themes are ini files of
 * original=replacement colours, optionally per editor area; Mods > UI Colors
 * opens a live editor with presets, an eyedropper and quick-tint tools.
 */
#include <windows.h>
#include <shellapi.h>
#include "engine.h"
#include "../../common/vh_util.h"

const VHApi *A;
wchar_t g_ini[MAX_PATH];               /* %APPDATA%\Vocahansments\settings.ini (portraits.c too) */
wchar_t g_userThemes[MAX_PATH];        /* %APPDATA%\Vocahansments\themes\ */
wchar_t g_presetThemes[MAX_PATH];      /* <V4>\vhmods\themes\ */
wchar_t g_themePath[MAX_PATH];         /* current theme file ("" = none) */
wchar_t g_themeName[128];
static UINT g_cmdEnable;

void editor_open(HWND owner);          /* editor.c */

void settings_save_theme(void)
{
    WritePrivateProfileStringW(L"UIColors", L"Theme", g_themePath, g_ini);
}

static void __cdecl menu_editor(void *c) { (void)c; editor_open(A->GetMainWindow()); }

static void __cdecl menu_enable(void *c)
{
    (void)c;
    engine_set_enabled(!engine_enabled());
    A->CheckMenuItem(g_cmdEnable, engine_enabled());
    vh_ini_set_int(g_ini, L"UIColors", L"Enabled", engine_enabled());
}

static void __cdecl menu_census(void *c)
{
    (void)c;
    wchar_t p[MAX_PATH];
    wsprintfW(p, L"%scolour census.txt", g_userThemes);
    if (census_export(p)) ShellExecuteW(A->GetMainWindow(), L"open", p, NULL, NULL, SW_SHOWNORMAL);
}

/* Mods > Rainbow Notes: Off / Gradient / Colour changing */
static UINT g_cmdRainbow[3];

static void rainbow_checks(void)
{
    for (int i = 0; i < 3; i++) A->CheckMenuItem(g_cmdRainbow[i], rainbow_mode() == i);
}

static void __cdecl menu_rainbow(void *c)
{
    int mode = (int)(INT_PTR)c;
    rainbow_set_mode(mode);
    rainbow_checks();
    vh_ini_set_int(g_ini, L"UIColors", L"RainbowNotes", mode);
}

/* Mods > Spinning Notes: when, how fast, and whether every note has its own speed */
static UINT g_cmdSpin[3], g_cmdSpeed[SPIN_SPEEDS], g_cmdVary;
static int g_spinMode, g_spinSpeed = 1;
static BOOL g_spinVary;

/* playing: the synth manager's busy flag (as Pitch Tools uses it) */
#define V4_VT_CMainFrame 0x0055CAB4
#define V4_MF_SYNTHMGR   0x1348
#define V4_SM_BUSY       0x480

static BOOL song_playing(void)
{
    static BYTE *frame;
    static HWND frameHwnd;
    static DWORD lastLook;
    HWND main = A->GetMainWindow();
    if (!main) return FALSE;
    if (!frame || frameHwnd != main || IsBadReadPtr(frame, V4_MF_SYNTHMGR + 4) || *(DWORD *)frame != V4_VT_CMainFrame) {
        if (GetTickCount() - lastLook < 2000) return FALSE;    /* the search walks memory: not every frame */
        lastLook = GetTickCount();
        frame = vh_find_cwnd(V4_VT_CMainFrame, main);
        frameHwnd = main;
        if (!frame) return FALSE;
    }
    BYTE *mgr = *(BYTE **)(frame + V4_MF_SYNTHMGR);
    return mgr && !IsBadReadPtr(mgr + V4_SM_BUSY, 4) && *(int *)(mgr + V4_SM_BUSY) != 0;
}

static void spin_apply(void)
{
    spin_set(g_spinMode, g_spinSpeed, g_spinVary);
    for (int i = 0; i < 3; i++) A->CheckMenuItem(g_cmdSpin[i], g_spinMode == i);
    for (int i = 0; i < SPIN_SPEEDS; i++) A->CheckMenuItem(g_cmdSpeed[i], g_spinSpeed == i);
    A->CheckMenuItem(g_cmdVary, g_spinVary);
    vh_ini_set_int(g_ini, L"UIColors", L"SpinNotes", g_spinMode);
    vh_ini_set_int(g_ini, L"UIColors", L"SpinSpeed", g_spinSpeed);
    vh_ini_set_int(g_ini, L"UIColors", L"SpinVary", g_spinVary);
}

static void __cdecl menu_spin(void *c) { g_spinMode = (int)(INT_PTR)c; spin_apply(); }
static void __cdecl menu_speed(void *c) { g_spinSpeed = (int)(INT_PTR)c; spin_apply(); }
static void __cdecl menu_vary(void *c) { (void)c; g_spinVary = !g_spinVary; spin_apply(); }

static void spin_menus(void)
{
    static const wchar_t *const speeds[SPIN_SPEEDS] = { L"Speed: Slow", L"Speed: Medium", L"Speed: Fast", L"Speed: Silly" };
    g_cmdSpin[SPIN_OFF] = vh_menu_add(A, L"Spinning Notes", L"Off", menu_spin, (void *)(INT_PTR)SPIN_OFF);
    g_cmdSpin[SPIN_ALWAYS] = vh_menu_add(A, L"Spinning Notes", L"Always spin", menu_spin, (void *)(INT_PTR)SPIN_ALWAYS);
    g_cmdSpin[SPIN_PLAYING] = vh_menu_add(A, L"Spinning Notes", L"Spin only while playing", menu_spin, (void *)(INT_PTR)SPIN_PLAYING);
    for (int i = 0; i < SPIN_SPEEDS; i++) g_cmdSpeed[i] = vh_menu_add(A, L"Spinning Notes", speeds[i], menu_speed, (void *)(INT_PTR)i);
    g_cmdVary = vh_menu_add(A, L"Spinning Notes", L"Every note its own speed and direction", menu_vary, NULL);
    g_spinMode = vh_ini_int(g_ini, L"UIColors", L"SpinNotes", SPIN_OFF);
    if (g_spinMode < SPIN_OFF || g_spinMode > SPIN_PLAYING) g_spinMode = SPIN_OFF;
    g_spinSpeed = vh_ini_int(g_ini, L"UIColors", L"SpinSpeed", 1);
    if (g_spinSpeed < 0 || g_spinSpeed >= SPIN_SPEEDS) g_spinSpeed = 1;
    g_spinVary = vh_ini_int(g_ini, L"UIColors", L"SpinVary", 0) != 0;
    spin_set_playing_fn(song_playing);
    spin_apply();
}

/* Mods > Gravity: notes fall to the bottom of the piano roll and bounce */
static UINT g_cmdGrav[3], g_cmdStrength[GRAV_STRENGTHS], g_cmdBounce[GRAV_BOUNCES], g_cmdDrag;
static int g_gravMode, g_gravStrength = 1, g_gravBounce = 1;
static BOOL g_gravDrag = TRUE;

static void grav_apply(void)
{
    grav_set(g_gravMode, g_gravStrength, g_gravBounce, g_gravDrag);
    A->CheckMenuItem(g_cmdDrag, g_gravDrag);
    vh_ini_set_int(g_ini, L"UIColors", L"GravityDrag", g_gravDrag);
    for (int i = 0; i < 3; i++) A->CheckMenuItem(g_cmdGrav[i], g_gravMode == i);
    for (int i = 0; i < GRAV_STRENGTHS; i++) A->CheckMenuItem(g_cmdStrength[i], g_gravStrength == i);
    for (int i = 0; i < GRAV_BOUNCES; i++) A->CheckMenuItem(g_cmdBounce[i], g_gravBounce == i);
    vh_ini_set_int(g_ini, L"UIColors", L"Gravity", g_gravMode);
    vh_ini_set_int(g_ini, L"UIColors", L"GravityStrength", g_gravStrength);
    vh_ini_set_int(g_ini, L"UIColors", L"GravityBounce", g_gravBounce);
}

static void __cdecl menu_grav(void *c) { g_gravMode = (int)(INT_PTR)c; grav_apply(); }
static void __cdecl menu_strength(void *c) { g_gravStrength = (int)(INT_PTR)c; grav_apply(); }
static void __cdecl menu_bounce(void *c) { g_gravBounce = (int)(INT_PTR)c; grav_apply(); }
static void __cdecl menu_drop(void *c) { (void)c; grav_drop(); }
static void __cdecl menu_explode(void *c) { (void)c; grav_explode(); }
static void __cdecl menu_quake(void *c) { (void)c; grav_quake(); }
static void __cdecl menu_drag(void *c) { (void)c; g_gravDrag = !g_gravDrag; grav_apply(); }

/* Mods > Jelly Notes */
static UINT g_cmdJelly[3];
static int g_jellyMode;

static void __cdecl menu_jelly(void *c)
{
    g_jellyMode = (int)(INT_PTR)c;
    jelly_set(g_jellyMode);
    for (int i = 0; i < 3; i++) A->CheckMenuItem(g_cmdJelly[i], g_jellyMode == i);
    vh_ini_set_int(g_ini, L"UIColors", L"JellyNotes", g_jellyMode);
}

static void jelly_menus(void)
{
    g_cmdJelly[SPIN_OFF] = vh_menu_add(A, L"Jelly Notes", L"Off", menu_jelly, (void *)(INT_PTR)SPIN_OFF);
    g_cmdJelly[SPIN_ALWAYS] = vh_menu_add(A, L"Jelly Notes", L"Always wobble", menu_jelly, (void *)(INT_PTR)SPIN_ALWAYS);
    g_cmdJelly[SPIN_PLAYING] = vh_menu_add(A, L"Jelly Notes", L"Wobble only while playing", menu_jelly, (void *)(INT_PTR)SPIN_PLAYING);
    int mode = vh_ini_int(g_ini, L"UIColors", L"JellyNotes", SPIN_OFF);
    menu_jelly((void *)(INT_PTR)(mode >= SPIN_OFF && mode <= SPIN_PLAYING ? mode : SPIN_OFF));
}

static void grav_menus(void)
{
    static const wchar_t *const strengths[GRAV_STRENGTHS] = { L"Gravity: Moon", L"Gravity: Earth", L"Gravity: Jupiter", L"Gravity: Space (none)" };
    static const wchar_t *const bounces[GRAV_BOUNCES] = { L"Bounce: None", L"Bounce: Some", L"Bounce: Super bouncy" };
    g_cmdGrav[SPIN_OFF] = vh_menu_add(A, L"Gravity", L"Off", menu_grav, (void *)(INT_PTR)SPIN_OFF);
    g_cmdGrav[SPIN_ALWAYS] = vh_menu_add(A, L"Gravity", L"Always", menu_grav, (void *)(INT_PTR)SPIN_ALWAYS);
    g_cmdGrav[SPIN_PLAYING] = vh_menu_add(A, L"Gravity", L"Only while playing", menu_grav, (void *)(INT_PTR)SPIN_PLAYING);
    for (int i = 0; i < GRAV_STRENGTHS; i++) g_cmdStrength[i] = vh_menu_add(A, L"Gravity", strengths[i], menu_strength, (void *)(INT_PTR)i);
    for (int i = 0; i < GRAV_BOUNCES; i++) g_cmdBounce[i] = vh_menu_add(A, L"Gravity", bounces[i], menu_bounce, (void *)(INT_PTR)i);
    g_cmdDrag = vh_menu_add(A, L"Gravity", L"Drag tool: grab and throw notes", menu_drag, NULL);
    vh_menu_add(A, L"Gravity", L"Drop them again", menu_drop, NULL);
    vh_menu_add(A, L"Gravity", L"Explode!", menu_explode, NULL);
    vh_menu_add(A, L"Gravity", L"Earthquake", menu_quake, NULL);
    g_gravMode = vh_ini_int(g_ini, L"UIColors", L"Gravity", SPIN_OFF);
    if (g_gravMode < SPIN_OFF || g_gravMode > SPIN_PLAYING) g_gravMode = SPIN_OFF;
    g_gravStrength = vh_ini_int(g_ini, L"UIColors", L"GravityStrength", 1);
    if (g_gravStrength < 0 || g_gravStrength >= GRAV_STRENGTHS) g_gravStrength = 1;
    g_gravBounce = vh_ini_int(g_ini, L"UIColors", L"GravityBounce", 1);
    if (g_gravBounce < 0 || g_gravBounce >= GRAV_BOUNCES) g_gravBounce = 1;
    g_gravDrag = vh_ini_int(g_ini, L"UIColors", L"GravityDrag", 1) != 0;
    grav_apply();
}

static void __cdecl menu_portraits(void *c) { (void)c; portraits_open(); }

static void __cdecl on_ui_ready(HWND mainWnd, void *ctx)
{
    (void)ctx; (void)mainWnd;
    engine_patch_symbols();
    portraits_load();
    vh_menu_add(A, NULL, L"Voicebank Portraits...", menu_portraits, NULL);
    vh_menu_add(A, L"UI Colors", L"Theme editor...", menu_editor, NULL);
    g_cmdEnable = vh_menu_add(A, L"UI Colors", L"Enabled", menu_enable, NULL);
    A->CheckMenuItem(g_cmdEnable, engine_enabled());
    vh_menu_add(A, L"UI Colors", L"Export colour census", menu_census, NULL);
    g_cmdRainbow[RAINBOW_OFF] = vh_menu_add(A, L"Rainbow Notes", L"Off", menu_rainbow, (void *)(INT_PTR)RAINBOW_OFF);
    g_cmdRainbow[RAINBOW_GRADIENT] = vh_menu_add(A, L"Rainbow Notes", L"Gradient", menu_rainbow, (void *)(INT_PTR)RAINBOW_GRADIENT);
    g_cmdRainbow[RAINBOW_CYCLE] = vh_menu_add(A, L"Rainbow Notes", L"Colour changing", menu_rainbow, (void *)(INT_PTR)RAINBOW_CYCLE);
    int mode = vh_ini_int(g_ini, L"UIColors", L"RainbowNotes", RAINBOW_OFF);
    rainbow_set_mode(mode >= RAINBOW_OFF && mode <= RAINBOW_CYCLE ? mode : RAINBOW_OFF);   /* refreshes */
    rainbow_checks();
    spin_menus();
    jelly_menus();
    grav_menus();
}

__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api)
{
    A = api;
    if (api->version < VH_API_MIN_VERSION) return 0;
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    lstrcatW(dir, L"\\Vocahansments");
    CreateDirectoryW(dir, NULL);
    wsprintfW(g_ini, L"%s\\settings.ini", dir);
    wsprintfW(g_userThemes, L"%s\\themes\\", dir);
    CreateDirectoryW(g_userThemes, NULL);
    wsprintfW(g_presetThemes, L"%sthemes\\", api->modDir);

    if (!engine_install()) { A->Log("uicolors: GDI hooks failed"); return 0; }
    GetPrivateProfileStringW(L"UIColors", L"Theme", L"", g_themePath, MAX_PATH, g_ini);
    if (g_themePath[0] && !theme_load(g_themePath, g_themeName, 128)) g_themePath[0] = 0;
    if (!vh_ini_int(g_ini, L"UIColors", L"Enabled", 1)) engine_set_enabled(FALSE);
    A->Log("uicolors: theme '%ls' (%ls)", g_themeName, g_themePath[0] ? g_themePath : L"none");
    api->OnUiReady(on_ui_ready, NULL);
    return 1;
}

/* lets other mods draw in the active theme (e.g. the Slice Tool's button) */
__declspec(dllexport) COLORREF __cdecl UIColors_Remap(int area, int kind, COLORREF c)
{
    return remap(area, kind, c);
}
