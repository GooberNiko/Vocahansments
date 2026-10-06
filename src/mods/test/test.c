#include "../../common/vh_api.h"
static const VHApi *A;
static HBRUSH (WINAPI *o_CreateSolidBrush)(COLORREF);
static int nb;
static HBRUSH WINAPI h_CreateSolidBrush(COLORREF c){ if(nb++<20) A->Log("CreateSolidBrush %06x",c); return o_CreateSolidBrush(c); }
static void __cdecl hello(void*c){ (void)c; MessageBoxW(A->GetMainWindow(),L"Vocahansments loader works",L"Test",0); }
static void __cdecl ui(HWND h, void*c){ (void)c;(void)h; A->AddMenuItem(L"Test mod: hello", hello, 0); }
__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api){ A=api; api->HookImport("gdi32.dll","CreateSolidBrush",h_CreateSolidBrush,(void**)&o_CreateSolidBrush); api->OnUiReady(ui,0); return 1; }
