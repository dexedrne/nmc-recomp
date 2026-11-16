import json,os,re
def h32(s):
    c=0xFFFFFFFF
    for b in s.encode():
        c^=(b<<24)&0xFFFFFFFF
        for _ in range(8):
            c=((c<<1)^0x4C11DB7)&0xFFFFFFFF if c&0x80000000 else (c<<1)&0xFFFFFFFF
    return (~c)&0xFFFFFFFF
def h16(s):
    d=s.encode(); s1=0xFFFF
    for i in range(len(d)-1,-1,-1):
        s1=(s1^(d[i]<<8))&0xFFFF
        for _ in range(8):
            s1=((s1<<1)^0x1021)&0xFFFF if s1&0x8000 else (s1<<1)&0xFFFF
    return (~s1)&0xFFFF
R=os.path.expanduser('~/projects/SillyBillyMod1')
cands=set()
for root,ds,fs in os.walk(R):
    if '.git' in root.split(os.sep): continue
    for f in fs:
        p=os.path.relpath(os.path.join(root,f),R).replace(os.sep,'/')
        cands.add(p)
        if p.startswith('remastered/'): cands.add(p[len('remastered/'):])
for l in open(R+'/mod.yml',errors='replace'):
    m=re.match(r'\s*-?\s*name:\s*(.+?)\s*$',l)
    if m: cands.add(m.group(1).replace('\\','/').strip('"\''))
base=set(cands)
for p in base:
    cands.add(p.replace('\\','/'))
idx={}
for n in cands:
    for pre in ('','_CCzDA/'):
        full=pre+n; idx.setdefault(f'{h32(full):08x}.{h16(full):04x}',[]).append(full)
ch=json.load(open('idx/changeset.json'))
a=json.load(open('idx/orig.json')); b=json.load(open('idx/nmc.json'))
res={}
for kind in ('new','modified'):
    named=0; ov=0
    for k in ch[kind]:
        nm=idx.get(k); 
        if nm: named+=1; ov+= any(x.startswith('_CCzDA/') for x in nm)
        res[k]=dict(kind=kind,names=nm)
    print(kind,len(ch[kind]),'named',named,'with _CCzDA prefix',ov)
json.dump(res,open('idx/named.json','w'))
print(len(cands),'candidate names')
