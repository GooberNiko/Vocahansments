#include "../../common/vh_api.h"
static const VHApi *A;
typedef void (__thiscall *OnLBD_t)(void *self, UINT flags, int x, int y);
static OnLBD_t o_OnLBD;
typedef int (__thiscall *YToNote_t)(void *self, int y);
static YToNote_t o_YToNote;
static void *g_lastPane;
static void dumpw(const char *tag, BYTE *p, int n){
    char buf[2048]; int k=0;
    for(int i=0;i<n;i+=4){ k+=wsprintfA(buf+k,"%03x:%08x ",i,*(DWORD*)(p+i)); if((i/4)%8==7){A->Log("%s %s",tag,buf);k=0;} }
    if(k) A->Log("%s %s",tag,buf);
}
static int __thiscall h_YToNote(void *self, int y){ int r=o_YToNote(self,y); if(self!=g_lastPane){g_lastPane=self; A->Log("YToNote this=%p y=%d -> %d",self,y,r);} return r; }
static void __thiscall h_OnLBD(void *self, UINT flags, int x, int y){
    BYTE *w=(BYTE*)self;
    A->Log("OnLButtonDown this=%p x=%d y=%d mgr=%p", self, x, y, A->GetVsqManager());
    dumpw("W+ed0", w+0xed0, 0x100);
    void *ctl58=*(void**)(w+0xed0+0x58);
    A->Log("ctl+58=%p vt=%p", ctl58, ctl58?*(void**)ctl58:0);
    o_OnLBD(self,flags,x,y);
    if(g_lastPane){ dumpw("pane",(BYTE*)g_lastPane,0x120); BYTE*ctx=*(BYTE**)((BYTE*)g_lastPane+8); dumpw("ctx",ctx,0xd0);}
}
__declspec(dllexport) int __cdecl VH_ModInit(const VHApi *api){
    A=api;
    static const BYTE s1[]={0x55,0x8b,0xec,0x53,0x56,0x57};
    static const BYTE s2[]={0x55,0x8b,0xec,0x8b,0x41,0x2c};
    api->HookCode((void*)0x47ba30,s1,6,h_OnLBD,(void**)&o_OnLBD);
    api->HookCode((void*)0x489cb0,s2,6,h_YToNote,(void**)&o_YToNote);
    return 1;
}
