import ctypes, ctypes.wintypes as W
u=ctypes.WinDLL('user32'); u.SetProcessDPIAware()
tops=[]
@ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
def cb(h,l):
    t=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,t,256)
    if 'VOCALOID4 Editor' in t.value: tops.append(h)
    return True
u.EnumWindows(cb,0)
def dump(h,d=0):
    t=ctypes.create_unicode_buffer(128); c=ctypes.create_unicode_buffer(128)
    u.GetWindowTextW(h,t,128); u.GetClassNameW(h,c,128)
    r=W.RECT(); u.GetWindowRect(h,ctypes.byref(r))
    print('  '*d+f'{h:#x} id={u.GetDlgCtrlID(h)} [{c.value}] "{t.value}" {r.left},{r.top} {r.right-r.left}x{r.bottom-r.top} vis={u.IsWindowVisible(h)}')
    ch=u.GetWindow(h,5)
    while ch:
        dump(ch,d+1); ch=u.GetWindow(ch,2)
for h in tops: dump(h)
