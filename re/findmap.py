import ctypes, ctypes.wintypes as W, subprocess, struct, sys
k = ctypes.WinDLL('kernel32')
out = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq VOCALOID4.exe', '/FO', 'CSV', '/NH']).decode()
pid = int(out.split('","')[1]); h = k.OpenProcess(0x410, 0, pid)
def rd(a, n):
    b = ctypes.create_string_buffer(n); r = ctypes.c_size_t()
    k.ReadProcessMemory(h, ctypes.c_void_p(a), b, n, ctypes.byref(r)); return b.raw[:r.value]
class MBI(ctypes.Structure):
    _fields_ = [('Base', ctypes.c_void_p), ('AllocBase', ctypes.c_void_p), ('AllocProt', W.DWORD), ('Size', ctypes.c_size_t), ('State', W.DWORD), ('Prot', W.DWORD), ('Type', W.DWORD)]
def find_all(vt):
    res = []; a = 0x10000; m = MBI(); pat = struct.pack('<I', vt)
    while a < 0x7fff0000 and k.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        base = m.Base or 0
        if m.State == 0x1000 and m.Type == 0x20000 and m.Prot in (4, 0x40):
            d = rd(base, m.Size); i = d.find(pat)
            while i >= 0:
                if i % 4 == 0: res.append(base + i)
                i = d.find(pat, i + 1)
        a = base + m.Size
    return res
view = find_all(0x56280c)[0]
maps = find_all(0x563774)
print('view', hex(view), 'maps', [hex(x) for x in maps], 'offsets', [hex(x - view) for x in maps])
for mp in maps:
    vt, table, size, count, free, blocks, bsize = struct.unpack('<7I', rd(mp, 28))
    print('map', hex(mp), 'table', hex(table), 'size', size, 'count', count)
    if table and count:
        buckets = struct.unpack(f'<{size}I', rd(table, size * 4))
        for b in buckets:
            p = b
            while p:
                raw = rd(p, 0x60)
                nxt, hv, key = struct.unpack_from('<3I', raw)
                vals = struct.unpack_from('<20i', raw, 12)
                print(' key', key, vals)
                p = nxt
