# dump the piano roll's rendered pitch buffer (pane+0x224) from the running editor
import ctypes, ctypes.wintypes as W, subprocess, struct, sys
k = ctypes.WinDLL('kernel32')
out = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq VOCALOID4.exe', '/FO', 'CSV', '/NH']).decode()
pid = int(out.split('","')[1]); h = k.OpenProcess(0x410, 0, pid)
def rd(a, n):
    b = ctypes.create_string_buffer(n); r = ctypes.c_size_t()
    k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(r)); return b.raw[:r.value]
class MBI(ctypes.Structure):
    _fields_ = [('Base', ctypes.c_void_p), ('AllocBase', ctypes.c_void_p), ('AllocProt', W.DWORD), ('Size', ctypes.c_size_t), ('State', W.DWORD), ('Prot', W.DWORD), ('Type', W.DWORD)]
view = None; a = 0x10000; m = MBI(); pat = struct.pack('<I', 0x56280c)
while a < 0x7fff0000 and k.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
    base = m.Base or 0
    if m.State == 0x1000 and m.Type == 0x20000 and m.Prot in (4, 0x40):
        d = rd(base, m.Size); i = d.find(pat)
        while i >= 0:
            if i % 4 == 0: view = base + i; break
            i = d.find(pat, i + 1)
    if view: break
    a = base + m.Size
pane = view + 0xca0
b, e = struct.unpack('<II', rd(pane + 0x224, 8))
n = (e - b) // 24
print('view', hex(view), 'entries', n, 'part', struct.unpack('<I', rd(pane + 0xf0, 4))[0])
data = rd(b, n * 24)
step = int(sys.argv[1]) if len(sys.argv) > 1 else max(1, n // 40)
for i in range(0, n, step):
    t, *f = struct.unpack_from('<I5f', data, i * 24); print(t, [round(x, 4) for x in f])
