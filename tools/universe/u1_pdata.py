"""Step 1: parse .pdata RUNTIME_FUNCTION + UNWIND_INFO. Output pdata.json (raw entries, chain parents)."""
import json, struct, collections
from pecore import PEImg
P=PEImg()
img=P.img
va,sz=P.dd['EXCEPTION']
n=sz//12
ents=[]
for i in range(n):
    b,e,u=struct.unpack_from('<III',img,va+12*i)
    ents.append((b,e,u))
UWOP_NAMES={0:'PUSH_NONVOL',1:'ALLOC_LARGE',2:'ALLOC_SMALL',3:'SET_FPREG',4:'SAVE_NONVOL',5:'SAVE_NONVOL_FAR',6:'EPILOG',7:'SPARE',8:'SAVE_XMM128',9:'SAVE_XMM128_FAR',10:'PUSH_MACHFRAME'}
def parse_unwind(u):
    ver=img[u]&7; flags=img[u]>>3; psz=img[u+1]; nc=img[u+2]; fr=img[u+3]
    codes=[]; pos=u+4; i=0
    # decode ops to know slot usage (validate ncodes); keep raw
    raw=img[u+4:u+4+nc*2]
    j=0
    while j<nc:
        off,opinfo=raw[2*j],raw[2*j+1]; op=opinfo&15; info=opinfo>>4
        slots=1
        if op==1: slots=2 if info==0 else 3
        elif op in (4,6,8): slots=2
        elif op in (5,7,9): slots=3
        codes.append((off,op,info)); j+=slots
    tail=u+4+((nc+1)&~1)*2
    chain=None; handler=None; hdata=None
    if flags&4:
        chain=struct.unpack_from('<III',img,tail)
    elif flags&3:
        handler=struct.unpack_from('<I',img,tail)[0]; hdata=tail+4
    return dict(ver=ver,flags=flags,prolog=psz,ncodes=nc,frame=fr,chain=chain,handler=handler,hdata=hdata,codes=codes,codes_ok=(j==nc))
out=[]
for b,e,u in ents:
    d=parse_unwind(u&~1)
    out.append(dict(b=b,e=e,u=u,**d))
begins={r['b']:i for i,r in enumerate(out)}
import bisect
starts=[r['b'] for r in out]
def entry_containing(a):
    i=bisect.bisect_right(starts,a)-1
    return i if i>=0 and out[i]['b']<=a<out[i]['e'] else None
cc=collections.Counter()
for i,r in enumerate(out):
    if r['chain']:
        cb,ce,cu=r['chain']
        j=entry_containing(cb)
        r['parent_idx']=j
        if j is None: cc['parent_not_in_pdata']+=1; continue
        p=out[j]
        cc['parent_begin_exact' if p['b']==cb else 'parent_mid']+=1
        cc['parent_end_match' if p['e']==ce else 'parent_end_mismatch']+=1
        cc['parent_unw_match' if p['u']==cu else 'parent_unw_mismatch']+=1
        cc['parent_is_chained' if p['chain'] else 'parent_root']+=1
        cc['frag_adjacent_after_parent' if r['b']==p['e'] else ('frag_before_parent' if r['e']==p['b'] else 'frag_distant')]+=1
print(len(out),dict(cc))
print('codes_ok all',all(r['codes_ok'] for r in out),'ver',collections.Counter(r['ver'] for r in out))
