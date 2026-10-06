/*
 * Vocahansments loader - version.dll proxy for VOCALOID4.exe.
 *
 * VOCALOID4.exe is ASPack-packed: the unpacker stub resolves the real import
 * table with GetProcAddress at startup. We are loaded while that happens
 * (the exe imports version.dll), so we patch kernelbase!GetProcAddress and
 * hand the stub our hooks directly - the editor never sees an unhooked import.
 * mfc110u.dll's own IAT is patched the classic way.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "../common/vh_api.h"

/* ---------------------------------------------------------------- proxy -- */

static const char *g_exportNames[] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW", "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW", "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW",
};
#define NEXPORTS (sizeof(g_exportNames) / sizeof(g_exportNames[0]))
void *g_real[NEXPORTS];

#define STUB(n, i) ".globl _vh_" #n "\n_vh_" #n ": jmp *_g_real+" #i "*4\n"
__asm__(
    ".text\n"
    STUB(GetFileVersionInfoA, 0) STUB(GetFileVersionInfoByHandle, 1)
    STUB(GetFileVersionInfoExA, 2) STUB(GetFileVersionInfoExW, 3)
    STUB(GetFileVersionInfoSizeA, 4) STUB(GetFileVersionInfoSizeExA, 5)
    STUB(GetFileVersionInfoSizeExW, 6) STUB(GetFileVersionInfoSizeW, 7)
    STUB(GetFileVersionInfoW, 8) STUB(VerFindFileA, 9) STUB(VerFindFileW, 10)
    STUB(VerInstallFileA, 11) STUB(VerInstallFileW, 12) STUB(VerLanguageNameA, 13)
    STUB(VerLanguageNameW, 14) STUB(VerQueryValueA, 15) STUB(VerQueryValueW, 16));

/* ------------------------------------------------------------------ log -- */

static wchar_t g_modDir[MAX_PATH];
static CRITICAL_SECTION g_logLock;
static FILE *g_log;

static void __cdecl vh_log(const char *fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

/* ---------------------------------------------------------- import hooks -- */

#define EXE_BASE 0x400000u
#define MAX_HOOKS 256

typedef struct { char dll[32]; char func[64]; void *real; void *top; } HookEntry;
static HookEntry g_hooks[MAX_HOOKS];
static int g_nhooks;
static DWORD g_exeSize;

typedef struct { USHORT Length, MaximumLength; PCHAR Buffer; } ANSI_STR;
typedef LONG (NTAPI *LdrGetProcedureAddress_t)(HMODULE, ANSI_STR *, ULONG, void **);
static LdrGetProcedureAddress_t pLdrGetProcedureAddress;

static int in_exe(void *p)
{
    return (DWORD)p >= EXE_BASE && (DWORD)p < EXE_BASE + g_exeSize;
}

static void write_ptr(void **slot, void *val)
{
    DWORD old;
    if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        *slot = val;
        VirtualProtect(slot, 4, old, &old);
    }
}

/* Replace slot values == from with to in a module's import address tables. */
static int patch_module_iat(HMODULE mod, void *from, void *to)
{
    if (!mod) return 0;
    BYTE *b = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); d->Name; d++) {
        for (void **slot = (void **)(b + d->FirstThunk); *slot; slot++)
            if (*slot == from) { write_ptr(slot, to); n++; }
    }
    return n;
}

/* The unpacked exe has no usable import directory; its IAT lives in .rdata. */
static int patch_exe_iat(void *from, void *to)
{
    BYTE *b = (BYTE *)EXE_BASE;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (memcmp(s->Name, ".rdata", 6)) continue;
        /* ASPack rewrites VirtualSize in memory; the next section's VA is reliable */
        DWORD end = (i + 1 < nt->FileHeader.NumberOfSections) ? s[1].VirtualAddress
                                                              : s->VirtualAddress + s->Misc.VirtualSize;
        void **p = (void **)(b + s->VirtualAddress);
        void **e = (void **)(b + (end & ~3u));
        for (; p < e; p++) if (*p == from) { write_ptr(p, to); n++; }
    }
    return n;
}

static void *real_proc(HMODULE m, const char *name)
{
    void *addr = NULL;
    ANSI_STR s;
    if ((ULONG_PTR)name >> 16) {
        s.Length = (USHORT)strlen(name); s.MaximumLength = s.Length + 1; s.Buffer = (PCHAR)name;
        pLdrGetProcedureAddress(m, &s, 0, &addr);
    } else {
        pLdrGetProcedureAddress(m, NULL, (ULONG)(ULONG_PTR)name, &addr);
    }
    return addr;
}

static volatile LONG g_mfcPatched;
static void patch_mfc_all(void);
static volatile LONG g_exeHooked;
static void apply_pending_code_hooks(void);

/* Replacement for kernelbase!GetProcAddress. Never calls the original. */
static FARPROC WINAPI hook_GetProcAddress(HMODULE m, LPCSTR name)
{
    void *caller = __builtin_return_address(0);
    void *addr = real_proc(m, name);
    if (!addr) { SetLastError(ERROR_PROC_NOT_FOUND); return NULL; }
    if (in_exe(caller)) {
        for (int i = 0; i < g_nhooks; i++)
            if (g_hooks[i].real == addr) return (FARPROC)g_hooks[i].top;
        if (!g_mfcPatched && GetModuleHandleA("mfc110u.dll")) patch_mfc_all();
    }
    return (FARPROC)addr;
}

static void patch_mfc_all(void)
{
    HMODULE mfc = GetModuleHandleA("mfc110u.dll");
    if (!mfc || InterlockedExchange(&g_mfcPatched, 1)) return;
    int n = 0;
    for (int i = 0; i < g_nhooks; i++) n += patch_module_iat(mfc, g_hooks[i].real, g_hooks[i].top);
    vh_log("mfc110u IAT patched (%d slots)", n);
}

static BOOL __cdecl vh_HookImport(const char *dll, const char *func, void *hook, void **orig)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    void *real = m ? real_proc(m, func) : NULL;
    if (!real) { vh_log("HookImport: %s!%s not found", dll, func); return FALSE; }

    HookEntry *e = NULL;
    for (int i = 0; i < g_nhooks; i++) if (g_hooks[i].real == real) { e = &g_hooks[i]; break; }
    if (!e) {
        if (g_nhooks >= MAX_HOOKS) return FALSE;
        e = &g_hooks[g_nhooks];
        lstrcpynA(e->dll, dll, sizeof e->dll); lstrcpynA(e->func, func, sizeof e->func);
        e->real = e->top = real;
        g_nhooks++;
    }
    void *prev = e->top;
    *orig = prev;
    e->top = hook;
    /* Late registration (or a second mod chaining): fix already-resolved slots. */
    int n = 0;
    if (g_mfcPatched || prev != real) n += patch_module_iat(GetModuleHandleA("mfc110u.dll"), prev, hook);
    n += patch_exe_iat(prev, hook);
    vh_log("hook %s!%s -> %p (late slots %d)", dll, func, hook, n);
    return TRUE;
}

/* --------------------------------------------------- original entry point --
 * ASPack resolves imports and decompresses .text around the time it loads us,
 * so we hook the very end of its stub instead:
 *     popa ; jne L ; mov eax,1 ; ret 0Ch ; L: push OEP ; ret
 * Our jmp replaces "popa; jne; mov.." and fires after every import is
 * resolved and the code is unpacked, right before the first editor instruction.
 * The OEP itself is read from the push at that moment (the stub fills it late). */

static const BYTE kStubTail[] = { 0x61, 0x75, 0x08, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00, 0x68 };
BYTE *g_stubTail;
DWORD g_oep;

static void patch_exe_all(const char *why)
{
    int n = 0;
    for (int i = 0; i < g_nhooks; i++) n += patch_exe_iat(g_hooks[i].real, g_hooks[i].top);
    patch_mfc_all();
    if (!InterlockedExchange(&g_exeHooked, 1) || n) vh_log("exe IAT patched at %s (%d slots)", why, n);
}

void __stdcall vh_oep_handler(void)
{
    g_oep = *(DWORD *)(g_stubTail + 12);
    patch_exe_all("OEP");
    vh_log("OEP %08lx", g_oep);
    apply_pending_code_hooks();
}

/* entered with the stub's flags (from "or ecx,ecx") and pushad frame intact */
void vh_oep_stub(void);
__asm__(
    ".globl _vh_oep_stub\n_vh_oep_stub:\n"
    " popal\n"                         /* the popa we replaced */
    " jne 1f\n"
    " movl $1, %eax\n ret $0xc\n"      /* DLL path of the stub, never taken for the exe */
    "1: pushal\n call _vh_oep_handler@0\n popal\n"
    " jmp *_g_oep\n");

static BOOL install_oep_hook(void)
{
    BYTE *b = (BYTE *)EXE_BASE;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !g_stubTail; i++) {
        if (memcmp(s[i].Name, ".aspack", 7)) continue;
        BYTE *p = b + s[i].VirtualAddress, *e = p + 0x2000;
        for (; p < e; p++) if (!memcmp(p, kStubTail, sizeof kStubTail)) { g_stubTail = p; break; }
    }
    if (!g_stubTail) { vh_log("ASPack stub tail not found"); return FALSE; }
    DWORD old;
    if (!VirtualProtect(g_stubTail, 5, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    g_stubTail[0] = 0xE9;
    *(DWORD *)(g_stubTail + 1) = (DWORD)((BYTE *)vh_oep_stub - g_stubTail - 5);
    VirtualProtect(g_stubTail, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_stubTail, 5);
    vh_log("stub tail hook at %p", g_stubTail);
    return TRUE;
}

static BOOL install_gpa_hook(void)
{
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    pLdrGetProcedureAddress = (LdrGetProcedureAddress_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrGetProcedureAddress");
    BYTE *target = kb ? (BYTE *)GetProcAddress(kb, "GetProcAddress") : NULL;
    if (!target || !pLdrGetProcedureAddress) return FALSE;
    DWORD old;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    BYTE jmp[5] = { 0xE9 };
    *(DWORD *)(jmp + 1) = (DWORD)((BYTE *)hook_GetProcAddress - target - 5);
    memcpy(target, jmp, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return TRUE;
}

/* ------------------------------------------------------------ UI plumbing -- */

#define VH_WM_UIREADY (WM_APP + 0x3A1)
#define VH_CMD_FIRST  0xC100      /* clear of the mods' tool buttons (0xBF8x) */
#define VH_CMD_MAX    256
#define VH_TIMER_ID   0x56484D44 /* 'VHMD' */

typedef struct { VHUiReadyCallback cb; void *ctx; } UiReady;
static UiReady g_uiReady[32];
static int g_nUiReady;

typedef struct { wchar_t group[32]; wchar_t text[96]; VHMenuCallback cb; void *ctx; BOOL checked; } MenuItem;
static MenuItem g_items[VH_CMD_MAX];
static int g_nitems;

static HWND g_main;
static WNDPROC g_mainProc;
static HMENU g_modsMenu;
static HMENU g_subMenus[VH_CMD_MAX];
static int g_nsub;

static void __cdecl vh_OnUiReady(VHUiReadyCallback cb, void *ctx)
{
    if (g_nUiReady < 32) { g_uiReady[g_nUiReady].cb = cb; g_uiReady[g_nUiReady].ctx = ctx; g_nUiReady++; }
}

static void append_item(HMENU m, int i)
{
    AppendMenuW(m, MF_STRING | (g_items[i].checked ? MF_CHECKED : 0), VH_CMD_FIRST + i, g_items[i].text);
}

/* Tools (no group) on top, then one submenu per group, in order of first use. */
static void rebuild_mods_menu(void)
{
    if (!g_modsMenu) g_modsMenu = CreatePopupMenu();
    while (GetMenuItemCount(g_modsMenu) > 0) DeleteMenu(g_modsMenu, 0, MF_BYPOSITION);   /* destroys the submenus */
    g_nsub = 0;
    BOOL top = FALSE;
    for (int i = 0; i < g_nitems; i++) if (!g_items[i].group[0]) { append_item(g_modsMenu, i); top = TRUE; }
    for (int i = 0; i < g_nitems; i++) {
        if (!g_items[i].group[0]) continue;
        BOOL seen = FALSE;
        for (int j = 0; j < i && !seen; j++) seen = !lstrcmpW(g_items[j].group, g_items[i].group);
        if (seen) continue;
        if (top && !g_nsub) AppendMenuW(g_modsMenu, MF_SEPARATOR, 0, NULL);
        HMENU sub = CreatePopupMenu();
        for (int j = i; j < g_nitems; j++) if (!lstrcmpW(g_items[j].group, g_items[i].group)) append_item(sub, j);
        AppendMenuW(g_modsMenu, MF_POPUP | MF_STRING, (UINT_PTR)sub, g_items[i].group);
        g_subMenus[g_nsub++] = sub;
    }
}

static BOOL is_mods_menu(HMENU m)
{
    if (m == g_modsMenu) return TRUE;
    for (int i = 0; i < g_nsub; i++) if (g_subMenus[i] == m) return TRUE;
    return FALSE;
}

/* MDI frames swap the menu bar; make sure ours is on whichever bar is current. */
static void ensure_mods_on_menubar(void)
{
    if (!g_main || !g_nitems) return;
    HMENU bar = GetMenu(g_main);
    if (!bar) return;
    int n = GetMenuItemCount(bar);
    for (int i = 0; i < n; i++) if (GetSubMenu(bar, i) == g_modsMenu) return;
    /* insert before Help (last item) */
    InsertMenuW(bar, n > 0 ? n - 1 : 0, MF_BYPOSITION | MF_POPUP | MF_STRING, (UINT_PTR)g_modsMenu, L"Mods(&O)");
    DrawMenuBar(g_main);
}

static UINT __cdecl vh_AddMenuItemIn(const wchar_t *group, const wchar_t *text, VHMenuCallback cb, void *ctx)
{
    if (g_nitems >= VH_CMD_MAX || !text) return 0;
    MenuItem *it = &g_items[g_nitems];
    lstrcpynW(it->group, group ? group : L"", 32);
    lstrcpynW(it->text, text, 96); it->cb = cb; it->ctx = ctx; it->checked = FALSE;
    g_nitems++;
    rebuild_mods_menu();
    ensure_mods_on_menubar();
    return VH_CMD_FIRST + g_nitems - 1;
}

static UINT __cdecl vh_AddMenuItem(const wchar_t *text, VHMenuCallback cb, void *ctx)
{
    return vh_AddMenuItemIn(NULL, text, cb, ctx);
}

static void __cdecl vh_CheckMenuItem(UINT id, BOOL checked)
{
    if (id < VH_CMD_FIRST || id >= VH_CMD_FIRST + (UINT)g_nitems) return;
    g_items[id - VH_CMD_FIRST].checked = checked;
    /* MF_BYCOMMAND also searches the submenus */
    if (g_modsMenu) CheckMenuItem(g_modsMenu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
}

static HWND __cdecl vh_GetMainWindow(void) { return g_main; }

/* Inline hooks on editor code. The exe is still packed while mods init, so
 * requests are queued and applied once ASPack hands over (see OEP below). */
typedef struct { BYTE *addr; BYTE expect[16]; int len; void *hook; BYTE *tramp; int prev; BOOL ok; } CodeHook;
static int g_ncode;

static void point_jmp(BYTE *at, void *to)
{
    DWORD old;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *(DWORD *)(at + 1) = (DWORD)((BYTE *)to - at - 5);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
}

static CodeHook g_code[64];
static BOOL apply_code_hook(CodeHook *c)
{
    if (c->prev >= 0) {
        /* second mod on the same function: its "original" is the previous hook */
        if (!g_code[c->prev].ok) return FALSE;
        point_jmp(c->tramp, g_code[c->prev].hook);
        point_jmp(c->addr, c->hook);
        c->ok = TRUE;
        vh_log("HookCode %p -> %p (chained)", c->addr, c->hook);
        return TRUE;
    }
    if (IsBadReadPtr(c->addr, c->len) || memcmp(c->addr, c->expect, c->len)) {
        vh_log("HookCode %p: signature mismatch - unsupported editor build?", c->addr);
        return FALSE;
    }
    BYTE *t = c->tramp;
    memcpy(t, c->addr, c->len);
    t[c->len] = 0xE9;
    *(DWORD *)(t + c->len + 1) = (DWORD)((c->addr + c->len) - (t + c->len + 5));
    DWORD old;
    VirtualProtect(c->addr, c->len, PAGE_EXECUTE_READWRITE, &old);
    c->addr[0] = 0xE9;
    *(DWORD *)(c->addr + 1) = (DWORD)((BYTE *)c->hook - c->addr - 5);
    for (int i = 5; i < c->len; i++) c->addr[i] = 0x90;
    VirtualProtect(c->addr, c->len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), c->addr, c->len);
    vh_log("HookCode %p -> %p", c->addr, c->hook);
    c->ok = TRUE;
    return TRUE;
}

static BOOL __cdecl vh_HookCode(void *addr, const void *expect, int len, void *hook, void **tramp)
{
    if (len < 5 || len > 16 || g_ncode >= 64) return FALSE;
    CodeHook *c = &g_code[g_ncode];
    c->addr = (BYTE *)addr; memcpy(c->expect, expect, len); c->len = len; c->hook = hook;
    c->prev = -1; c->ok = FALSE;
    for (int i = g_ncode - 1; i >= 0; i--) if (g_code[i].addr == c->addr) { c->prev = i; break; }
    /* trampoline exists up front so the mod can store its pointer now */
    c->tramp = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!c->tramp) return FALSE;
    *tramp = c->tramp;
    g_ncode++;
    if (g_exeHooked) return apply_code_hook(c);
    return TRUE;
}

static void apply_pending_code_hooks(void)
{
    for (int i = 0; i < g_ncode; i++) apply_code_hook(&g_code[i]);
}

/* theApp (CDaisyEditorApp) is a static object in the exe; +0xDC8 holds the
 * CVSVsqManagerIF*. Checked by vtable so a different build returns NULL. */
#define V4_THEAPP        0x005D93C8
#define V4_VT_DAISYAPP   0x00559824
static void *__cdecl vh_GetVsqManager(void)
{
    if (IsBadReadPtr((void *)V4_THEAPP, 0xDCC) || *(DWORD *)V4_THEAPP != V4_VT_DAISYAPP) return NULL;
    return *(void **)((BYTE *)V4_THEAPP + 0xDC8);
}

static LRESULT CALLBACK main_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case VH_WM_UIREADY:
        for (int i = 0; i < g_nUiReady; i++) g_uiReady[i].cb(h, g_uiReady[i].ctx);
        ensure_mods_on_menubar();
        SetTimer(h, VH_TIMER_ID, 500, NULL);
        vh_log("UI ready: %d callbacks, %d menu items", g_nUiReady, g_nitems);
        return 0;
    case WM_TIMER:
        if (wp == VH_TIMER_ID) { ensure_mods_on_menubar(); return 0; }
        break;
    case WM_COMMAND:
        if (HIWORD(wp) == 0 && LOWORD(wp) >= VH_CMD_FIRST && LOWORD(wp) < VH_CMD_FIRST + g_nitems) {
            MenuItem *it = &g_items[LOWORD(wp) - VH_CMD_FIRST];
            if (it->cb) it->cb(it->ctx);
            return 0;
        }
        break;
    case WM_INITMENUPOPUP: {
        /* MFC greys out items it has no handler for - undo that for ours. */
        LRESULT r = CallWindowProcW(g_mainProc, h, msg, wp, lp);
        if (is_mods_menu((HMENU)wp))
            for (int i = 0; i < g_nitems; i++) EnableMenuItem(g_modsMenu, VH_CMD_FIRST + i, MF_BYCOMMAND | MF_ENABLED);
        return r;
    }
    }
    return CallWindowProcW(g_mainProc, h, msg, wp, lp);
}

static BOOL CALLBACK find_main(HWND h, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    wchar_t t[128]; GetWindowTextW(h, t, 128);
    if (wcsstr(t, L"VOCALOID4") && GetMenu(h)) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static DWORD WINAPI ui_watch_thread(void *unused)
{
    (void)unused;
    HWND h = NULL;
    for (int i = 0; i < 1200 && !h; i++) { /* up to 10 minutes (licence dialogs etc.) */
        EnumWindows(find_main, (LPARAM)&h);
        if (!h) Sleep(500);
    }
    if (!h) { vh_log("main window not found"); return 0; }
    if (!g_exeHooked) { patch_exe_all("ui (late)"); apply_pending_code_hooks(); }
    g_main = h;
    g_mainProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)main_subclass);
    PostMessageW(h, VH_WM_UIREADY, 0, 0);
    return 0;
}

/* ---------------------------------------------------------- crash log -- */

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, 0x5C);
    return s ? s + 1 : path;
}

static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    vh_log("EXCEPTION %08lx at %p (%s+%lx) addr=%08lx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           code, ep->ExceptionRecord->ExceptionAddress, base_name(name),
           (DWORD)ep->ExceptionRecord->ExceptionAddress - (DWORD)m,
           ep->ExceptionRecord->NumberParameters > 1 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0,
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    DWORD *sp = (DWORD *)c->Esp;
    if (!IsBadReadPtr(sp, 64 * 4)) {
        char line[600]; int k = 0;
        for (int i = 0; i < 64; i++) {
            DWORD v = sp[i];
            HMODULE mm = NULL;
            if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &mm)) {
                char mn[MAX_PATH]; GetModuleFileNameA(mm, mn, MAX_PATH);
                k += wsprintfA(line + k, "%s+%lx ", base_name(mn), v - (DWORD)mm);
                if (k > 500) { vh_log("  stack: %s", line); k = 0; }
            }
        }
        if (k) vh_log("  stack: %s", line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ init -- */

static const VHApi g_api = {
    VH_API_VERSION, vh_HookImport, vh_OnUiReady, vh_AddMenuItem, vh_CheckMenuItem,
    vh_log, g_modDir, vh_GetMainWindow, vh_HookCode, vh_GetVsqManager,
    vh_AddMenuItemIn,
};

static void load_mods(void)
{
    wchar_t pat[MAX_PATH];
    lstrcpyW(pat, g_modDir); lstrcatW(pat, L"*.dll");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) { vh_log("no mods in vhmods\\"); return; }
    do {
        wchar_t path[MAX_PATH];
        lstrcpyW(path, g_modDir); lstrcatW(path, fd.cFileName);
        HMODULE m = LoadLibraryW(path);
        VHModInitFn init = m ? (VHModInitFn)GetProcAddress(m, "VH_ModInit") : NULL;
        if (!init) { vh_log("skip %ls (load %p)", fd.cFileName, m); continue; }
        int r = init(&g_api);
        vh_log("mod %ls init -> %d", fd.cFileName, r);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH); /* SysWOW64 for a 32-bit process */
    lstrcatW(sys, L"\\version.dll");
    HMODULE real = LoadLibraryW(sys);
    for (unsigned i = 0; i < NEXPORTS; i++) g_real[i] = real ? (void *)GetProcAddress(real, g_exportNames[i]) : NULL;

    /* only mod the editor itself, never other hosts that might load us */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!wcsstr(_wcslwr(exe), L"vocaloid4.exe")) return TRUE;

    GetModuleFileNameW(inst, g_modDir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_modDir, L'\\');
    if (slash) lstrcpyW(slash + 1, L"vhmods\\");
    CreateDirectoryW(g_modDir, NULL);

    InitializeCriticalSection(&g_logLock);
    wchar_t logPath[MAX_PATH];
    lstrcpyW(logPath, g_modDir); lstrcatW(logPath, L"vocahansments.log");
    g_log = _wfopen(logPath, L"w");
    if (!g_log) { /* Program Files may be read-only: fall back to %TEMP% */
        GetTempPathW(MAX_PATH, logPath); lstrcatW(logPath, L"vocahansments.log");
        g_log = _wfopen(logPath, L"w");
    }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(EXE_BASE + ((IMAGE_DOS_HEADER *)EXE_BASE)->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;

    vh_log("Vocahansments loader 1.0 - %ls", exe);
    AddVectoredExceptionHandler(0, crash_logger);
    if (!install_gpa_hook()) vh_log("WARNING: GetProcAddress hook failed; hooks applied late");
    load_mods();
    if (GetModuleHandleA("mfc110u.dll")) patch_mfc_all();
    install_oep_hook();
    CloseHandle(CreateThread(NULL, 0, ui_watch_thread, NULL, 0, NULL));
    return TRUE;
}
