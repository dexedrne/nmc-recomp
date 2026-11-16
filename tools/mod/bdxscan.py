#!/usr/bin/env python3
"""Part C: scan KH2 BDX (AI/script) bytecode for `syscall` instructions and report (table, trap) usage.
Instruction table + header layout re-derived from the OpenKH Bdxio model (Apache-2.0) and cross-checked against the exe VM
(0x41b400: opcode = code&0xF, table = code>>6, trap index = next halfword).
usage: bdxscan.py OUT.json PATH [PATH...]   (files or directories)"""
import struct,sys,os,json,collections
sys.dont_write_bytecode=True
# (code, mask, size, kind) kind: j=uncond jump, jc=cond jump, g=gosub16, g32=gosub32, s=syscall, r=ret/exit/halt-stop, n=normal
DESC=[]
def add(code,mask,size,kind='n'): DESC.append((code,mask,size,kind))
add(0x0000,0x003F,3);add(0x0010,0x003F,3)
for c in (0x20,0x60,0xA0,0xE0,0x30,0x70,0xB0,0xF0): add(c,0xFFFF,2)
for c in (0x01,0x41,0x81,0xC1): add(c,0xFFCF,2)
for c in (0x02,0x42,0x82,0xC2): add(c,0xFFCF,3)
add(0x0003,0x000F,2); add(0x0004,0x000F,1)
for c in (0x5,0x85,0xC5,0x105,0x145,0x185,0x1C5,0x205,0x245,0x285,0x2C5,0x55,0x95,0x155,0x195,0x1D5,0x215,0x255,0x295,0x2D5,
          0x6,0x46,0x86,0xC6,0x106,0x146,0x186,0x1C6,0x206,0x246,0x286,0x2C6,0x16,0x56,0x96,0xD6,0x116): add(c,0xFFFF,1)
add(0x0007,0xFFCF,2,'j'); add(0x0047,0xFFCF,2,'jc'); add(0x0087,0xFFCF,2,'jc')
add(0x0008,0x000F,2,'g')
add(0x0009,0xFFCF,1); add(0x0049,0xFFCF,1,'r'); add(0x0089,0xFFCF,1,'r')
for c in (0xC9,0x149,0x189,0x1C9,0x209,0x249): add(c,0xFFCF,1)
add(0x000A,0x000F,2,'s'); add(0x000B,0x000F,3,'g32')
def find(code):
    for c,m,sz,k in DESC:
        if code&m==c: return sz,k
    return None
def parse_bdx(d):
    """returns dict(ok,syscalls=[(table,trap,pc)],ninstr) or None if not a plausible BDX"""
    if len(d)<0x20: return None
    name=d[:16]
    if not all(32<=b<127 or b==0 for b in name) or name[0]==0: return None
    work,stack,temp=struct.unpack_from('<iii',d,16)
    if not(0<=work<0x100000 and 0<=stack<0x100000 and 0<=temp<0x100000): return None
    trig=[];p=28
    while p+8<=len(d):
        k,a=struct.unpack_from('<ii',d,p); p+=8
        if k==0 and a==0: break
        trig.append((k,a))
        if len(trig)>4096: return None
    else: return None
    n=(len(d)-16)//2
    seen=set(); sysc=[]; work_l=[a for k,a in trig if 0<=a<n]
    ninstr=0
    while work_l:
        pc=work_l.pop()
        while 0<=pc<n and pc not in seen:
            code=struct.unpack_from('<H',d,16+2*pc)[0]
            r=find(code)
            if r is None: break
            sz,kind=r
            if pc+sz>n: break
            seen.add(pc); ninstr+=1
            if kind=='s':
                sysc.append(((code>>6)&0x3ff,struct.unpack_from('<H',d,16+2*(pc+1))[0],pc))
            if kind in('j','jc'):
                rel=struct.unpack_from('<h',d,16+2*(pc+1))[0]; work_l.append(pc+sz+rel)
                if kind=='j': break
            elif kind=='g':
                rel=struct.unpack_from('<h',d,16+2*(pc+1))[0]; work_l.append(pc+sz+rel)
            elif kind=='g32':
                rel=struct.unpack_from('<i',d,16+2*(pc+1))[0]; work_l.append(pc+sz+rel)
            elif kind=='r': break
            pc+=sz
    return dict(name=name.split(b'\0')[0].decode('latin1'),ntrig=len(trig),ninstr=ninstr,syscalls=sysc)
def bar_entries(b):
    if b[:4]!=b'BAR\x01' or len(b)<16: return []
    n=struct.unpack_from('<I',b,4)[0]
    if n>5000 or 16+16*n>len(b): return []
    out=[]
    for i in range(n):
        t,dup,nm,o,s=struct.unpack_from('<HH4sII',b,16+16*i)
        if o+s>len(b) or o<16: continue
        out.append((t,nm.rstrip(b'\0').decode('latin1'),b[o:o+s]))
    return out
def walk(path,b,depth=0):
    """yield (label,bdx_result) for every BDX found"""
    if depth>6: return
    ents=bar_entries(b)
    for t,nm,d in ents:
        lab='%s/%s'%(path,nm)
        if t==3:
            r=parse_bdx(d)
            if r: yield lab,r
        elif d[:4]==b'BAR\x01': yield from walk(lab,d,depth+1)
def scan_file(p):
    try: b=open(p,'rb').read()
    except Exception: return []
    out=[]
    if p.lower().endswith('.bdx'):
        r=parse_bdx(b)
        if r: out.append((os.path.basename(p),r))
    out+=list(walk(os.path.basename(p),b))
    return out
if __name__=='__main__':
    outp=sys.argv[1]; res={}; nfiles=0; nbdx=0
    for root in sys.argv[2:]:
        files=[root] if os.path.isfile(root) else [os.path.join(dp,f) for dp,dn,fn in os.walk(root) if '.git' not in dp.split(os.sep) for f in fn]
        for p in files:
            try:
                with open(p,'rb') as fh: magic=fh.read(4)
            except Exception: continue
            if magic!=b'BAR\x01' and not p.lower().endswith('.bdx'): continue
            nfiles+=1
            for lab,r in scan_file(p):
                nbdx+=1
                if r['syscalls']: res[os.path.relpath(p,root if os.path.isdir(root) else os.path.dirname(root))+'::'+lab]=r
    tabs=collections.Counter()
    for k,r in res.items():
        for t,tr,pc in r['syscalls']: tabs[t]+=1
    print('bar/bdx files',nfiles,'bdx scripts decoded',nbdx,'scripts with syscalls',len(res),'syscalls by table',dict(tabs))
    json.dump(dict(files=nfiles,bdx=nbdx,by_table={str(k):v for k,v in tabs.items()},scripts=res),open(outp,'w'))
