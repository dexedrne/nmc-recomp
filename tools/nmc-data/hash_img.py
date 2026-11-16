import sys, struct, hashlib, json
idx=open(sys.argv[1],'rb').read(); n=struct.unpack('<I',idx[:4])[0]
ents=[struct.unpack('<IHHII',idx[4+16*i:20+16*i]) for i in range(n)]
def span(e):
    h,h2,bd,so,ln=e; comp=bool(bd&0x4000); bl=bd&0x3fff
    if ln>=0x1000000 and comp: bl|=0x1000
    return (so*2048, (bl+1)*2048 if comp else ln, comp)
order=sorted(range(n),key=lambda i:ents[i][3])
pos=0; cache={}; out={}; buf=sys.stdin.buffer
for i in order:
    h,h2,bd,so,ln=ents[i]; start,sz,comp=span(ents[i]); key=(start,sz,comp)
    if key not in cache:
        if start<pos: raise SystemExit(f'overlap at entry {h:08x}.{h2:04x}: start {start} < pos {pos}')
        if start>pos: buf.read(start-pos)
        data=buf.read(sz); pos=start+len(data)
        if comp: data=data.rstrip(b'\0')
        cache[key]=hashlib.sha1(data).hexdigest()
    out[f'{h:08x}.{h2:04x}']={'bd':bd,'comp':comp,'streamed':bool(bd&0x8000),'sector':so,'length':ln,'sha1':cache[key]}
json.dump(out,open(sys.argv[2],'w'))
print('done',len(out),'entries',file=sys.stderr)
