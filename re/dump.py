# Dump unpacked VOCALOID4.exe image from a running process + resolve IAT slots
import ctypes, ctypes.wintypes as W, struct, sys, subprocess, time, pefile, os
k=ctypes.WinDLL('kernel32',use_last_error=True)
psapi=ctypes.WinDLL('psapi')
def pid_of(name):
    out=subprocess.check_output(['tasklist','/FI',f'IMAGENAME eq {name}','/FO','CSV','/NH']).decode(errors='ignore')
    for l in out.splitlines():
        p=l.strip('"').split('","')
        if len(p)>1 and p[0].lower()==name.lower(): return int(p[1])
pid=pid_of('VOCALOID4.exe')
if not pid: sys.exit("not running")
h=k.OpenProcess(0x0410|0x0008,False,pid)
def rd(a,n):
    b=ctypes.create_string_buffer(n); r=ctypes.c_size_t()
    if not k.ReadProcessMemory(h,ctypes.c_void_p(a),b,n,ctypes.byref(r)): return None
    return b.raw[:r.value]
BASE=0x400000; SIZE=0x508000
img=bytearray(SIZE)
for off in range(0,SIZE,0x1000):
    d=rd(BASE+off,0x1000)
    if d: img[off:off+0x1000]=d
# fix section headers: raw = virtual
pe_off=struct.unpack_from('<I',img,0x3c)[0]
nsec=struct.unpack_from('<H',img,pe_off+6)[0]
optsz=struct.unpack_from('<H',img,pe_off+20)[0]
sh=pe_off+24+optsz
for i in range(nsec):
    o=sh+i*40
    va,vs=struct.unpack_from('<II',img,o+12)
    struct.pack_into('<II',img,o+16,max(vs,0x1000+0) if vs else 0x1000,va)
    struct.pack_into('<I',img,o+8,vs)
# kill import directory (packer's), we'll label IAT manually
struct.pack_into('<II',img,pe_off+24+104,0,0)
open('VOCALOID4_dump.exe','wb').write(img)
# modules
hm=(W.HMODULE*1024)(); need=W.DWORD()
psapi.EnumProcessModulesEx(h,hm,ctypes.sizeof(hm),ctypes.byref(need),3)
mods=[]
for m in hm[:need.value//4]:
    if not m: continue
    nm=ctypes.create_unicode_buffer(260); psapi.GetModuleFileNameExW(h,W.HMODULE(m),nm,260)
    mods.append((m,nm.value))
exp={}
for base,path in mods:
    if base==BASE: continue
    try:
        import re as _re; path=_re.sub(r'(?i)\\system32\\', r'\\SysWOW64\\', path); p=pefile.PE(path,fast_load=True); p.parse_data_directories(directories=[0])
        for s in getattr(p,'DIRECTORY_ENTRY_EXPORT',type('x',(),{'symbols':[]})).symbols:
            nm=s.name.decode() if s.name else f'ord{s.ordinal}'
            exp[base+s.address]=os.path.basename(path)+'!'+nm
    except Exception as e: print('fail',path,e)
# scan .rdata for pointers that are exports (IAT)
out=open('iat.txt','w')
cnt=0
for off in range(0x157000,0x191000,4):
    v=struct.unpack_from('<I',img,off)[0]
    if v in exp: out.write(f'{BASE+off:08x} {exp[v]}\n'); cnt+=1
print('mods',len(mods),'iat',cnt)
for b,p in mods: print(hex(b),p)
