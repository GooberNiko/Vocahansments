import pefile,sys,re
pe=pefile.PE('Vsq4.dll'); B=pe.OPTIONAL_HEADER.ImageBase
img=pe.get_memory_mapped_image()
import struct
src=open('vsq4.c',encoding='utf-8',errors='ignore').read()
funcs={}
for m in re.finditer(r'^// ==== (\S+) @ ([0-9a-f]+) size=(\d+)\n',src,re.M):
    funcs[int(m.group(2),16)]=(m.group(1),m.end(),int(m.group(3)))
def vt(addr,n=80):
    out=[]
    for i in range(n):
        v=struct.unpack_from('<I',img,addr-B+i*4)[0]
        if not (B+0x1000<=v<B+0x79000): break
        out.append(v)
    return out
a=int(sys.argv[1],16)
for i,f in enumerate(vt(a)):
    nm,pos,sz=funcs.get(f,('?',0,0))
    print(f'[{i:2d}] +{i*4:#04x} {f:08x} {nm} size={sz}')
