import ctypes, struct, subprocess, sys, json
exec(open('re/findmap.py').read().split('view = find_all')[0])
view = find_all(0x56280c)[0]
data = rd(view, 0x1600)
json.dump(list(struct.unpack('<%di' % (len(data)//4), data)), open(sys.argv[1], 'w'))
print('view', hex(view))
