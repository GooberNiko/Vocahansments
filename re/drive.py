# Drive the VOCALOID4 musical editor with window messages (dev/test helper).
# usage: python drive.py "cmd(33330)" "drag(200,150,600)" ...
import ctypes, ctypes.wintypes as W, sys, time
u = ctypes.WinDLL('user32')
u.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_size_t)]


def tops():
    r = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, l):
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        if t.value.startswith('VOCALOID4 Editor'):
            r.append(h)
        return True
    u.EnumWindows(cb, 0)
    return r


MAIN = tops()[0]


def view(prefix='MUSICAL EDITOR'):
    res = []

    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, l):
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        if t.value.startswith(prefix):
            c = u.GetWindow(h, 5)
            while c:
                cn = ctypes.create_unicode_buffer(64)
                u.GetClassNameW(c, cn, 64)
                if cn.value.startswith('Afx:00400000:b'):
                    res.append(c)
                c = u.GetWindow(c, 2)
        return True
    u.EnumChildWindows(MAIN, cb, 0)
    return res[0]


V = view()


def send(h, m, w, l):
    r = ctypes.c_size_t()
    if not u.SendMessageTimeoutW(h, m, w, l, 2, 3000, ctypes.byref(r)):
        print('timeout', hex(m))


def lp(x, y): return ((y & 0xffff) << 16) | (x & 0xffff)
def post(m, w, l, h=None): u.PostMessageW(h or V, m, w, l); time.sleep(0.03)


def drag(x1, y, x2, k=0):
    post(0x200, k, lp(x1, y)); post(0x201, 1 | k, lp(x1, y))
    for x in range(x1, x2 + 1, 10):
        post(0x200, 1 | k, lp(x, y))
    post(0x202, k, lp(x2, y))


def pclick(x, y, k=0):
    post(0x200, k, lp(x, y)); post(0x201, 1 | k, lp(x, y)); post(0x202, k, lp(x, y))


def cmd(i): send(MAIN, 0x111, i, 0)
def key(vk): send(V, 0x100, vk, 0); send(V, 0x101, vk, 0)


for a in sys.argv[1:]:
    exec(a)
    time.sleep(0.3)
