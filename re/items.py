import ctypes, struct, subprocess, sys
exec(open('re/findmap.py').read().split('view = find_all')[0])
view = find_all(0x56280c)[0]; pane = view + 0xca0
model = struct.unpack('<I', rd(pane + 0x100, 4))[0]
head, tail, cnt = struct.unpack('<3I', rd(model + 0x3c, 12))
print('model', hex(model), 'count?', cnt)
p = head
while p:
    nxt, prv, data = struct.unpack('<3I', rd(p, 12))
    v = struct.unpack('<24i', rd(data, 96))
    print(hex(data), 'vt', hex(v[0] & 0xffffffff), v[1:24])
    p = nxt
