import sys, struct, json, os, hashlib
which,idxp,outdir=sys.argv[1:4]
ch=json.load(open('idx/changeset.json')); want=set(ch['new'])|set(ch['modified'])
idx=open(idxp,'rb').read(); n=struct.unpack('<I',idx[:4])[0]
ents=[struct.unpack('<IHHII',idx[4+16*i:20+16*i]) for i in range(n)]
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
                    dst[di]=dst[di+ci]; di-=1   # dstData[dstIndex--] = dstData[dstIndex + copyIndex + 1] (index already decremented)
            else:
                dst[di]=d; di-=1
        else:
            dst[di]=d; di-=1
    return bytes(dst)
os.makedirs(outdir,exist_ok=True)
order=sorted(range(n),key=lambda i:ents[i][3]); pos=0; buf=sys.stdin.buffer; cnt=0
for i in order:
    h,h2,bd,so,ln=ents[i]; key=f'{h:08x}.{h2:04x}'
    comp=bool(bd&0x4000); bl=bd&0x3fff
    if ln>=0x1000000 and comp: bl|=0x1000
    start=so*2048; sz=(bl+1)*2048 if comp else ln
    if start<pos: continue
    if start>pos: buf.read(start-pos)
    data=buf.read(sz); pos=start+len(data)
    if key in want:
        out=decompress(data) if comp else data
        open(f'{outdir}/{key}.bin','wb').write(out); cnt+=1
print(which,'extracted',cnt,file=sys.stderr)
