import ctypes, ctypes.wintypes as W, sys, time
u=ctypes.WinDLL('user32')
def find_view(title_sub):
    res=[]
    @ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def cb(h,l):
        t=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,t,256)
        if title_sub in t.value:
            c=u.GetWindow(h,5)
            while c:
                cn=ctypes.create_unicode_buffer(64); u.GetClassNameW(c,cn,64)
                if cn.value.startswith('Afx:00400000:b'): res.append(c)
                c=u.GetWindow(c,2)
        return True
    @ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def top(h,l):
        u.EnumChildWindows(h,cb,0); return True
    u.EnumWindows(top,0)
    return res[0] if res else None
v=find_view(sys.argv[1] if len(sys.argv)>1 else 'MUSICAL EDITOR')
x=int(sys.argv[2]); y=int(sys.argv[3]); mods=int(sys.argv[4]) if len(sys.argv)>4 else 0
lp=(y<<16)|x
u.PostMessageW(v,0x200,mods,lp); time.sleep(0.05)
u.PostMessageW(v,0x201,1|mods,lp); time.sleep(0.05)
u.PostMessageW(v,0x202,mods,lp)
print(hex(v))
