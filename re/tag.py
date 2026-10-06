import re,sys,struct,pefile
src=open('vsq4.c',encoding='utf-8',errors='ignore').read()
body={}
for p in re.split(r'^(?=// ==== )',src,flags=re.M):
    m=re.match(r'// ==== (\S+) @ ([0-9a-f]+)',p)
    if m: body[int(m.group(2),16)]=p
pe=pefile.PE('Vsq4.dll'); B=pe.OPTIONAL_HEADER.ImageBase; img=pe.get_memory_mapped_image()
a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 80
for i in range(n):
    f=struct.unpack_from('<I',img,a-B+i*4)[0]
    if not (B+0x1000<=f<B+0x79000): break
    b=body.get(f,'')
    sig=re.search(r'\n(\w[^\n]*\(.*\))\n',b)
    tys=sorted(set(re.findall(r'&(\w+)::RTTI_Type_Descriptor',b))-{'IVSMutableVsqObject','IVSVsqObject'})
    errs=sorted(set(re.findall(r'0x830[0-9a-f]{5}',b)))
    print(f'[{i:2d}] +{i*4:#05x} {f:08x}', (sig.group(1)[:110] if sig else ''),'|',' '.join(tys),'|',' '.join(errs[:6]))
