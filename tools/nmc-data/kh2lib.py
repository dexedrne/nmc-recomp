import struct, hashlib, json, sys
sys.path.insert(0,'.')
def _h32(s):
    c=0xFFFFFFFF
    for b in s.encode():
        c^=(b<<24)&0xFFFFFFFF
        for _ in range(8): c=((c<<1)^0x4C11DB7)&0xFFFFFFFF if c&0x80000000 else (c<<1)&0xFFFFFFFF
    return (~c)&0xFFFFFFFF
def _h16(s):
    d=s.encode(); s1=0xFFFF
    for i in range(len(d)-1,-1,-1):
        s1=(s1^(d[i]<<8))&0xFFFF
        for _ in range(8): s1=((s1<<1)^0x1021)&0xFFFF if s1&0x8000 else (s1<<1)&0xFFFF
    return (~s1)&0xFFFF
def iso_extent(f,name):
    f.seek(16*2048); pvd=f.read(2048); assert pvd[1:6]==b'CD001'
    root=pvd[156:190]; lba=struct.unpack('<I',root[2:6])[0]; ln=struct.unpack('<I',root[10:14])[0]
    f.seek(lba*2048); d=f.read(ln); p=0
    while p<len(d):
        l=d[p]
        if l==0: p=(p//2048+1)*2048; continue
        nl=d[p+32]; nm=d[p+33:p+33+nl].decode('latin1').split(';')[0]
        if nm==name: return struct.unpack('<I',d[p+2:p+6])[0]*2048, struct.unpack('<I',d[p+10:p+14])[0]
        p+=l
    raise KeyError(name)
def decompress(src):
    si=len(src)-1; key=0
    while si>0:
        key=src[si]; si-=1
        if key!=0: break
    if si==0: return b''
    dec=src[si]; si-=1; dec|=src[si]<<8; si-=1; dec|=src[si]<<16; si-=1; dec|=src[si]<<24; si-=1
    di=dec-1; dst=bytearray(dec)
    while di>=0 and si>=0:
        d=src[si]; si-=1
        if d==key and si>=0:
            ci=src[si]; si-=1
            if ci>0 and si>=0:
                cl=src[si]; si-=1
                for _ in range(cl+3):
                    if di<0: break
                    dst[di]=dst[di+ci]; di-=1
            else: dst[di]=d; di-=1
        else: dst[di]=d; di-=1
    return bytes(dst)
def parse_idx(b):
    n=struct.unpack('<I',b[:4])[0]
    return [struct.unpack('<IHHII',b[4+16*i:20+16*i]) for i in range(n)]
def span(e):
    h,h2,bd,so,ln=e; comp=bool(bd&0x4000); bl=bd&0x3fff
    if ln>=0x1000000 and comp: bl|=0x1000
    return so*2048, ((bl+1)*2048 if comp else ln), comp
def read_entry(f,imgoff,e,decomp=False):
    s,sz,comp=span(e); f.seek(imgoff+s); d=f.read(sz)
    return decompress(d) if (comp and decomp) else d
INTERNAL=["000al","000bb","000ca","000dc","000di","000eh","000es","000gumi","000hb","000he","000lk","000lm","000mu","000nm","000po","000tr","000tt","000wi","000wm","000zz"]
