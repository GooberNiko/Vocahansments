import struct,re,sys
d=open('VOCALOID4_dump.exe','rb').read(); B=0x400000
src=open('exe.c',encoding='utf-8',errors='ignore').read()
sizes={int(m.group(1),16):int(m.group(2)) for m in re.finditer(r'^// ==== \S+ @ ([0-9a-f]+) size=(\d+)',src,re.M)}
a=int(sys.argv[1],16)
for i in range(int(sys.argv[2]) if len(sys.argv)>2 else 60):
    v=struct.unpack_from('<I',d,a-B+i*4)[0]
    if not 0x401000<=v<0x557000:
        if not 0x6b000000<=v<0x6d000000: break
    print(f'[{i:2d}] +{i*4:#05x} {v:08x} size={sizes.get(v,"?")}')
