"""Collect every reference to code addresses, classify against .pdata ranges. Writes cand.pkl"""
import pickle, collections, bisect, struct, csv
import x64scan as S
exec(open('u2_eh.py').read().split("if __name__=='__main__':")[0])
D=pickle.load(open('cfg_pdata.pkl','rb')); R=D['R']; LOG=D['LOG']
DR=pickle.load(open('data_refs.pkl','rb'))
img=S.img
pd=sorted((r['b'],r['e']) for r in out)
pds=[a for a,b in pd]
def in_pdata(x):
    k=bisect.bisect_right(pds,x)-1
    return k>=0 and x<pd[k][1]
cand=collections.defaultdict(set)    # target rva -> set(source tags)
def add(t,tag):
    if S.in_exec(t): cand[t].add(tag)
for root,v in R.items():
    for s,t in v['calls']: add(t,'call')
    for s,t,c in v['jmp_out']: add(t,'jcc-out' if c else 'tailjmp')
    for s,t,m in v['rip_code']: add(t,'rip-ref')
    for jt in v['jtabs']:
        for t in jt['targets']: add(t,'jumptable')
vt_slot_of=collections.defaultdict(list)
for v in DR['vt']:
    for k,s in enumerate(v['slots']):
        add(s,'vtable'); vt_slot_of[s].append((v['name'],v['offset'],k,v['vtable']))
xc=[r for r in DR['runs'] if len(r)>1000][0]
xcsites=set(s for s,t in xc)
xi_sites=set(s for s,t in DR['ptrs'] if 0x584e28<=s<0x584e40)
for s,t in DR['ptrs']:
    add(t,'init-xc' if s in xcsites else ('init-xi' if s in xi_sites else 'reloc-ptr'))
# exports
exports=[]
if hasattr(P.pe,'DIRECTORY_ENTRY_EXPORT'):
    for e in P.pe.DIRECTORY_ENTRY_EXPORT.symbols:
        exports.append((e.address,e.name.decode() if e.name else None,e.ordinal))
        if not getattr(e,'forwarder',None): add(e.address,'export')
add(P.entry,'entry')
for a,rr in refs.items():
    for k,owner in rr: add(a,'eh-'+k)
# TLS callbacks
tls=P.dd['TLS'][0]
cb=struct.unpack_from('<Q',img,tls+24)[0]-S.BASE
tlscbs=[]
q=cb
while True:
    v=struct.unpack_from('<Q',img,q)[0]
    if v==0: break
    tlscbs.append(v-S.BASE); add(v-S.BASE,'tls-callback'); q+=8
print('tls callbacks',tlscbs,'exports',len(exports),'in exec',sum(1 for e in exports if S.in_exec(e[0])))
# nep stubs: tiled by 16-byte units (verified separately)
cls=collections.Counter(); 
begins=set(pds)
for t,tags in cand.items():
    if t in begins: cls['pdata-begin']+=1
    elif in_pdata(t): cls['pdata-interior']+=1
    else: cls['gap']+=1
print(cls)
gapc={t:tags for t,tags in cand.items() if t not in begins and not in_pdata(t)}
print('gap candidates',len(gapc),'source combos',collections.Counter(tuple(sorted(t)) for t in gapc.values()).most_common(15))
srcs=collections.Counter(x for tags in gapc.values() for x in tags); print(srcs)
pickle.dump(dict(cand=dict(cand),vt_slot_of=dict(vt_slot_of),exports=exports,tlscbs=tlscbs),open('cand.pkl','wb'))
