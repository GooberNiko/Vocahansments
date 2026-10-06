/* Small helpers shared by Vocahansments mods. Header-only. */
#pragma once
#include <windows.h>

/* Find a live MFC object by its vtable and m_hWnd (CWnd::m_hWnd is at +0x20).
 * Scans committed private read/write memory; call once and cache. */
static inline BYTE *vh_find_cwnd(DWORD vtable, HWND hwnd)
{
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = (BYTE *)0x10000;
    while (p < (BYTE *)0x7FFF0000 && VirtualQuery(p, &mbi, sizeof mbi)) {
        BYTE *next = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD)) {
            DWORD *d = (DWORD *)mbi.BaseAddress, *e = (DWORD *)(next - 0x24);
            for (; d < e; d++)
                if (*d == vtable && *(HWND *)((BYTE *)d + 0x20) == hwnd) return (BYTE *)d;
        }
        p = next;
    }
    return NULL;
}

typedef struct { HWND found; int id; } VHFindChild;
static BOOL CALLBACK vh__find_child_cb(HWND h, LPARAM lp)
{
    VHFindChild *f = (VHFindChild *)lp;
    if (GetDlgCtrlID(h) == f->id) { f->found = h; return FALSE; }
    return TRUE;
}

/* Recursive search for a descendant control with the given id. */
static inline HWND vh_find_ctrl(HWND root, int id)
{
    VHFindChild f = { NULL, id };
    EnumChildWindows(root, vh__find_child_cb, (LPARAM)&f);
    return f.found;
}

static inline int vh_ini_int(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, ini);
}

static inline void vh_ini_set_int(const wchar_t *ini, const wchar_t *sec, const wchar_t *key, int v)
{
    wchar_t b[16];
    wsprintfW(b, L"%d", v);
    WritePrivateProfileStringW(sec, key, b, ini);
}
