"""Relocation-based code pointers, RTTI vtables, init tables. Writes data_refs.pkl"""
import struct, pickle, collections, re
import x64scan as S
img=S.img; BASE=S.base if hasattr(S,'base') else S.BASE
P=S.P
# ---- relocs
va,sz=P.dd['BASERELOC']
relocs=[]; p=va; end=va+sz
tc=collections.Counter()
while p<end:
    page,bsz=struct.unpack_from('<II',img,p)
    if bsz<8: break
    for k in range((bsz-8)//2):
        w=struct.unpack_from('<H',img,p+8+2*k)[0]; t=w>>12; o=w&0xfff
        tc[t]+=1
        if t==10: relocs.append(page+o)
    p+=bsz
print('reloc types',dict(tc),'dir64',len(relocs))
ptrs=[]   # (site_rva, target_rva)
for r in relocs:
    v=struct.unpack_from('<Q',img,r)[0]
    t=v-BASE
    if 0<=t<len(img) and S.in_exec(t): ptrs.append((r,t))
print('reloc code pointers',len(ptrs),'by site section',collections.Counter(P.sec_of(r) for r,t in ptrs))
# ---- RTTI: complete object locators
def secrange(name):
    for n,va_,vs,po,ps,ch in P.secs:
        if n==name: return va_,va_+vs
rd=secrange('.rdata')
cols={}
a,b=rd
for r in range(a,b-24,4):
    sig=struct.unpack_from('<I',img,r)[0]
    if sig!=1: continue
    off,cd,td,ch,ob=struct.unpack_from('<IIIII',img,r+4)
    if ob!=r: continue
    if not(0<td<len(img)-32): continue
    if img[td+16:td+20]!=b'.?AV' and img[td+16:td+20]!=b'.?AU': continue
    nm=img[td+16:img.index(b'\0',td+16)].decode('latin1')
    cols[r]=dict(offset=off,cdoffset=cd,td=td,chd=ch,name=nm)
print('COLs',len(cols))
colva={BASE+r:r for r in cols}
vt=[]
relset=set(relocs)
for r in relocs:
    v=struct.unpack_from('<Q',img,r)[0]
    if v in colva:
        c=colva[v]
        slots=[]; q=r+8
        while q+8<=len(img):
            if q not in relset: break
            x=struct.unpack_from('<Q',img,q)[0]-BASE
            if not (0<=x<len(img) and S.in_exec(x)): break
            slots.append(x); q+=8
        vt.append(dict(vtable=r+8,col=c,name=cols[c]['name'],offset=cols[c]['offset'],slots=slots))
print('vtables',len(vt),'slots',sum(len(v['slots']) for v in vt),'unique targets',len(set(s for v in vt for s in v['slots'])))
# ---- code-pointer runs that are not vtables (init tables, dispatch tables)
vtslots=set()
for v in vt:
    for i in range(len(v['slots'])): vtslots.add(v['vtable']+8*i)
runs=[]; cur=[]
ps=sorted(ptrs)
for r,t in ps:
    if r in vtslots: 
        if cur: runs.append(cur); cur=[]
        continue
    if cur and r==cur[-1][0]+8: cur.append((r,t))
    else:
        if cur: runs.append(cur)
        cur=[(r,t)]
if cur: runs.append(cur)
big=sorted(runs,key=len,reverse=True)[:12]
print('top runs',[(hex(r[0][0]),len(r),P.sec_of(r[0][0])) for r in big])
pickle.dump(dict(relocs=relocs,ptrs=ptrs,cols=cols,vt=vt,runs=runs),open('data_refs.pkl','wb'))
