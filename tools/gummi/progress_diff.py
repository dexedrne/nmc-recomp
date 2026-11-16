#!/usr/bin/env python3
"""Part D: decode 00progress.bin world-script subfiles (vanilla PC extract vs SillyBillyMod1 repo) and diff them.
Outputs progress_diff.json. Format: OpenKH docs 00progress.md (header of u16 flag pointers; ops = u8 opcode,u8 argc,argc*s16)."""
import struct,sys,json,os,difflib
sys.dont_write_bytecode=True
S6='/home/user/projects/kh2-nmc-analysis/scratch/S6-gummi-data/'
VAN=S6+'src/00progress.bin'; MOD='/home/user/projects/SillyBillyMod1/00progress.bin'
WN={'zz':0,'es':1,'tt':2,'di':3,'hb':4,'bb':5,'he':6,'al':7,'mu':8,'po':9,'lk':10,'lm':11,'dc':12,'wi':13,'nm':14,'wm':15,'ca':16,'tr':17,'eh':18}
IDW={v:k for k,v in WN.items()}
OP={0:'End',1:'ChangeArdProgram',2:'BlockArea',3:'UnblockArea',4:'AddWorldPoint',5:'RemoveWorldPoint',6:'ChangeBGM',7:'LowerProgress',8:'RaiseMenu',9:'LowerMenu',0xc:'RaiseProgress',0xd:'WorldMapStatus'}
def bar(b):
    n=struct.unpack_from('<I',b,4)[0]; out={}; order=[]
    for i in range(n):
        t,d,nm,o,s=struct.unpack_from('<HH4sII',b,16+i*16); nm=nm.rstrip(b'\0').decode()
        out[nm]=(t,o,s); order.append(nm)
    return out,order
def load(p):
    b=open(p,'rb').read(); d,o=bar(b); return b,d,o
def script(d):
    """flag index -> list of (opname,args) ; header = u16 pointers up to the lowest nonzero pointer"""
    hl=len(d); i=0
    while i<hl and i+2<=len(d):
        p=struct.unpack_from('<H',d,i)[0]
        if p and p<len(d): hl=min(hl,p)
        i+=2
    out={}
    for i in range(0,hl,2):
        p=struct.unpack_from('<H',d,i)[0]
        if not p or p>=len(d): continue
        q=p; l=[]
        while q+2<=len(d):
            oc,ac=d[q],d[q+1]
            if oc==0: break
            args=list(struct.unpack_from('<%dh'%ac,d,q+2)) if q+2+2*ac<=len(d) else []
            l.append((OP.get(oc,'op%x'%oc),args)); q+=2+2*ac
            if len(l)>500: break
        out[i//2]=l
    return out,hl
bv,dv,ov=load(VAN); bm,dm,om=load(MOD)
def sub(b,d,n): t,o,s=d[n]; return b[o:o+s]
dsa=sub(bv,dv,'dsa')
dsa_n=struct.unpack_from('<H',dsa,0)[0]; DSA=[(dsa[2+2*i],dsa[3+2*i]) for i in range(dsa_n)]
def area(code):
    if 0<=code<len(DSA): w,r=DSA[code]; return '%s%02d'%(IDW.get(w,'w%d'%w),r)
    return 'dsa%d'%code
def pretty(op,args):
    if op in('BlockArea','UnblockArea','AddWorldPoint','RemoveWorldPoint'): return '%s(%s%s)'%(op,area(args[0]),(', msg=%s'%args[1:] if op=='BlockArea' and len(args)>1 else ''))
    if op=='ChangeArdProgram':
        g=[args[i:i+4] for i in range(0,len(args),4)]
        return 'ChangeArdProgram(%s)'%'; '.join('%s map=%s btl=%s evt=%s'%(area(a[0]),a[1],a[2],a[3]) for a in g)
    return '%s(%s)'%(op,','.join(map(str,args)))
rep=dict(vanilla=VAN,mod=MOD,sizes=dict(vanilla=len(bv),mod=len(bm)),subfiles={},dsa_equal=sub(bv,dv,'dsa')==sub(bm,dm,'dsa'))
for n in ov:
    a,b=sub(bv,dv,n),sub(bm,dm,n)
    ent=dict(vanilla_size=len(a),mod_size=len(b),identical=a==b)
    if len(n)==2 and n in WN:
        sa,ha=script(a); sb,hb=script(b)
        flags=sorted(set(sa)|set(sb)); ch=[]
        for f in flags:
            la=[pretty(*x) for x in sa.get(f,[])]; lb=[pretty(*x) for x in sb.get(f,[])]
            if la!=lb: ch.append(dict(flag=f,vanilla=la,mod=lb))
        ent['world_script']=dict(flags_vanilla=len(sa),flags_mod=len(sb),changed_flags=ch)
    elif a!=b:
        ent['bytes_differ']=sum(1 for i in range(min(len(a),len(b))) if a[i]!=b[i])+abs(len(a)-len(b))
    rep['subfiles'][n]=ent
rep['changed_subfiles']=[n for n,e in rep['subfiles'].items() if not e['identical']]
json.dump(rep,open(os.path.dirname(os.path.abspath(__file__))+'/progress_diff.json','w'),indent=1)
print('changed subfiles:',rep['changed_subfiles'])
for n in rep['changed_subfiles']:
    e=rep['subfiles'][n]; print('\n==',n,e['vanilla_size'],'->',e['mod_size'])
    ws=e.get('world_script')
    if ws:
        for c in ws['changed_flags']:
            print(' flag',c['flag'])
            for x in c['vanilla']: print('   -',x)
            for x in c['mod']: print('   +',x)
    else: print('  ',e)
