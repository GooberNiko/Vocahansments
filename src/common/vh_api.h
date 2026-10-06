/*
 * Vocahansments - native mod API for VOCALOID4 Editor (VOCALOID4.exe 4.x, x86).
 *
 * The loader (version.dll proxy) loads every DLL in <V4 dir>\vhmods\ and calls
 * its exported   int __cdecl VH_ModInit(const VHApi *api)
 * while the editor is still unpacking - before any of its own code has run.
 * That is the place to register import hooks. Anything that needs the UI
 * (menus, windows) goes into an OnUiReady callback.
 */
#pragma once
#include <windows.h>

#define VH_API_VERSION 2
#define VH_API_MIN_VERSION 1   /* mods still run on a v1 loader (flat Mods menu) */

typedef void (__cdecl *VHMenuCallback)(void *ctx);
typedef void (__cdecl *VHUiReadyCallback)(HWND mainWnd, void *ctx);

typedef struct VHApi {
    int version;

    /* Redirect every import of dll!func made by VOCALOID4.exe and mfc110u.dll
     * to `hook`. *orig receives the previous target (call through it).
     * Several mods may hook the same function; they chain. */
    BOOL (__cdecl *HookImport)(const char *dll, const char *func, void *hook, void **orig);

    /* Called on the UI thread once the main frame exists. */
    void (__cdecl *OnUiReady)(VHUiReadyCallback cb, void *ctx);

    /* Adds an item to the "Mods" menu of the main frame. Returns the command
     * id (0 on failure). accel is display text only, e.g. "K". */
    UINT (__cdecl *AddMenuItem)(const wchar_t *text, VHMenuCallback cb, void *ctx);
    void (__cdecl *CheckMenuItem)(UINT id, BOOL checked);

    /* printf-style log to vhmods\vocahansments.log */
    void (__cdecl *Log)(const char *fmt, ...);

    /* <V4 dir>\vhmods\ (with trailing backslash) */
    const wchar_t *modDir;

    HWND (__cdecl *GetMainWindow)(void);

    /* Inline-hook editor code at `addr`. The first `len` bytes must equal
     * `expect` (guards against other editor builds), be whole instructions
     * and position independent; they are relocated into *tramp, which then
     * calls the original. Returns FALSE (and patches nothing) on mismatch. */
    BOOL (__cdecl *HookCode)(void *addr, const void *expect, int len, void *hook, void **tramp);

    /* CVSVsqManagerIF* of the running editor (*(theApp + 0xDC8)), or NULL. */
    void *(__cdecl *GetVsqManager)(void);

    /* --- version 2 --- */

    /* Like AddMenuItem, but into the Mods > `group` submenu (created on first
     * use). group NULL or "" = the top section of the Mods menu, which is
     * meant for tools; settings go into a group named after the mod. */
    UINT (__cdecl *AddMenuItemIn)(const wchar_t *group, const wchar_t *text, VHMenuCallback cb, void *ctx);
} VHApi;

/* AddMenuItemIn when the loader has it, else a flat "group: text" item */
static inline UINT vh_menu_add(const VHApi *api, const wchar_t *group, const wchar_t *text, VHMenuCallback cb, void *ctx)
{
    if (api->version >= 2) return api->AddMenuItemIn(group, text, cb, ctx);
    if (!group || !group[0]) return api->AddMenuItem(text, cb, ctx);
    wchar_t t[128];
    wsprintfW(t, L"%s: %s", group, text);
    return api->AddMenuItem(t, cb, ctx);
}

typedef int (__cdecl *VHModInitFn)(const VHApi *api);
