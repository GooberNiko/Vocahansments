# Real mouse drag inside the lab editor's piano roll (view client coords), with mid-drag screenshot.
import ctypes, ctypes.wintypes as W, time, sys
from PIL import ImageGrab
u = ctypes.WinDLL('user32'); u.SetProcessDPIAware()
exec(open('re/drive.py').read().split('def send(')[0].replace('V = view()', 'V = view()'))
def to_screen(x, y):
    p = W.POINT(x, y); u.ClientToScreen(V, ctypes.byref(p)); return p.x, p.y
def mv(x, y): sx, sy = to_screen(x, y); u.SetCursorPos(sx, sy)
def btn(down): u.mouse_event(0x0002 if down else 0x0004, 0, 0, 0, 0)
u.SetForegroundWindow(MAIN); time.sleep(0.5)
x0, y0, x1, y1 = [int(v) for v in sys.argv[1:5]]
mv(x0, y0); time.sleep(0.2); btn(True); time.sleep(0.2)
steps = 25
for i in range(1, steps + 1):
    mv(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps); time.sleep(0.03)
time.sleep(0.4)
r = W.RECT(); u.GetWindowRect(MAIN, ctypes.byref(r))
ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(sys.argv[5])
btn(False); time.sleep(0.8)
ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True).save(sys.argv[6])
