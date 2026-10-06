import ctypes, ctypes.wintypes as W, sys
from PIL import ImageGrab
u=ctypes.WinDLL('user32')
u.SetProcessDPIAware()
res=[]
@ctypes.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
def cb(h,l):
    pid=W.DWORD(); u.GetWindowThreadProcessId(h,ctypes.byref(pid))
    t=ctypes.create_unicode_buffer(256); u.GetWindowTextW(h,t,256)
    if u.IsWindowVisible(h) and t.value.startswith('VOCALOID4 Editor'): res.append((h,t.value))
    return True
u.EnumWindows(cb,0)
print(res)
h=res[0][0]
u.ShowWindow(h,9); u.SetForegroundWindow(h)
import time; time.sleep(0.6)
r=W.RECT(); u.GetWindowRect(h,ctypes.byref(r))
img=ImageGrab.grab(bbox=(r.left,r.top,r.right,r.bottom),all_screens=True)
img.save(sys.argv[1] if len(sys.argv)>1 else 'shot.png'); print(img.size)
