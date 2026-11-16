#!/usr/bin/env python3
"""Assemble the canonical function universe from the intermediate pickles. Writes functions.tsv/json, ranges.tsv, gaps.tsv, coverage.json, overlaps.tsv, summary.json"""
import pickle, json, collections, bisect, csv, struct, sys
import x64scan as S
exec(open('u2_eh.py').read().split("if __name__=='__main__':")[0])   # pdata entries `out`, refs (EH funclet refs), HANDLERS, iat, root_idx
img=S.img; BASE=S.BASE
D=pickle.load(open('cfg_pdata.pkl','rb')); R=D['R']; LOG=D['LOG']
G=pickle.load(open('gapfn.pkl','rb')); GF=G['fn']; GT=G['tags']
CD=pickle.load(open('cand.pkl','rb')); cand=CD['cand']; vt_slot_of=CD['vt_slot_of']
DR=pickle.load(open('data_refs.pkl','rb'))
NAMES={int(r['rva'],16):r for r in json.load(open('../S7-name-db/name_db.json'))}
CLS={c['rtti_name']:c for c in json.load(open('../S7-name-db/class_db.json'))}
UFLAG=lambda f:'|'.join(n for b,n in ((1,'EHANDLER'),(2,'UHANDLER'),(4,'CHAININFO')) if f&b) or 'NONE'
entry_by_begin={r['b']:i for i,r in enumerate(out)}
root_begin_of_entry=lambda b: out[out[entry_by_begin[b]]['root_idx']]['b']
EXECSECS={n:(a,b) for n,a,b in P.exec_secs}
def secname(x):
    for n,(a,b) in EXECSECS.items():
        if a<=x<b: return n
IL=G['il_ranges']
# ---------------- records
F=[]   # list of dict
def new(start): 
    return dict(start=start)
# --- pdata logical functions
nop_cache={}
def is_nop_bytes(a,b):
    p=a
    while p<b:
        i=S.decode_one(p)
        if i is None or p+i.size>b or i.mnemonic not in('nop',): return False
        p+=i.size
    return True
for L in LOG:
    root=L['root']; r=R[root]
    ents=[out[i] for i in L['idxs']]
    rootent=out[out[entry_by_begin[root]]['root_idx']]
    ranges=[(a,b) for a,b in L['ranges']]
    prim=[x for x in ranges if x[0]==root][0]
    ranges=[prim]+[x for x in ranges if x!=prim]
    depth=collections.Counter()
    frags=[]
    for e in ents:
        if e['b']==root: continue
        touches=any((x[1]==e['b'] or x[0]==e['e']) and x!=(e['b'],e['e']) for x in L['ranges'])
        frags.append((e['b'],e['e'],'contiguous' if touches else 'split'))
    data=[(a,b,k) for a,b,k in r['data']]
    unr=[]
    for a,b in r['unreached']:
        d=img[a:b]; p=a; ok=True; lastm=None
        while p<b:
            i=S.decode_one(p)
            if i is None or p+i.size>b: ok=False; break
            lastm=i.mnemonic; p+=i.size
        if all(i.mnemonic in('nop','int3','xchg') for i in S.md.disasm(d,a)) and sum(i.size for i in S.md.disasm(d,a))==len(d): unr.append((a,b,'padding-nop'))
        elif ok: unr.append((a,b,'unreached-code'))
        else: unr.append((a,b,'unreached-data')); data.append((a,b,'unreached-data'))
    f=dict(start=root,ranges=ranges,has_pdata=True,n_pdata=len(ents),
           unwind_flags=UFLAG(rootent['flags']),handler_type=(HANDLERS[rootent['handler']] if rootent['handler'] else 'none'),
           prolog=rootent['prolog'],ncodes=rootent['ncodes'],frame_reg=rootent['frame'],
           frags=frags,data=data,r=r,gap=False,unr=unr)
    F.append(f)
for t,r in GF.items():
    ins=r['insn']
    iv=sorted([(a,a+s) for a,s in ins.items()]+[(a,b) for a,b,_ in r['data']])
    # union then merge touching, fill nop-only holes
    merged=[]
    for a,b in iv:
        if merged and a<=merged[-1][1]: merged[-1][1]=max(merged[-1][1],b)
        else: merged.append([a,b])
    out2=[]
    for a,b in merged:
        if out2 and 0<a-out2[-1][1]<=15 and is_nop_bytes(out2[-1][1],a): out2[-1][1]=b
        else: out2.append([a,b])
    ranges=[tuple(x) for x in out2]
    if t in G['nep_units']: ranges=[tuple(G['nep_units'][t])]     # whole 16-byte CLR native-entry stub unit
    prim=[x for x in ranges if x[0]<=t<x[1]][0]
    ranges=[prim]+[x for x in ranges if x!=prim]
    f=dict(start=t,ranges=ranges,has_pdata=False,n_pdata=0,unwind_flags='',handler_type='',prolog='',ncodes='',frame_reg='',frags=[],
           data=[(a,b,k) for a,b,k in r['data']],r=r,gap=True,unr=[])
    F.append(f)
F.sort(key=lambda f:f['start'])
for i,f in enumerate(F): f['id']='F%05d'%(i+1); f['idx']=i
by_start={f['start']:f for f in F}
frag_begin={}
for f in F:
    for a,b,k in f['frags']: frag_begin[a]=f
def merge_ranges(rs):
    out_=[]
    for a,b in sorted(rs):
        if out_ and a<=out_[-1][1]: out_[-1][1]=max(out_[-1][1],b)
        else: out_.append([a,b])
    return [tuple(x) for x in out_]
for f in F:
    f['size']=sum(b-a for a,b in f['ranges'])
    f['extents']=merge_ranges(f['ranges'])
# ---------------- discovery tags
for f in F:
    t=f['start']
    if f['gap']: tg=set(GT[t])
    else: tg=set(cand.get(t,set()))-{'jumptable','rip-ref','jcc-out'}
    tg=set(tg)
    if not f['gap']: tg.add('pdata')
    f['tags']=tg
# ---------------- funclets
EHKIND={'cleanup':'cleanup','catch':'catch','seh-filter':'seh-filter','seh-finally':'seh-finally'}
for f in F:
    t=f['start']; f['funclet_kind']=''; f['funclet_parents']=[]; f['eh_ref']=False
    if t in refs:
        ks=sorted(set(k for k,o in refs[t]))
        par=sorted(set(root_begin_of_entry(o) if o in entry_by_begin else o for k,o in refs[t]))
        f['eh_ref']=True; f['funclet_kind']='+'.join(ks); f['funclet_parents']=par
    others=set(x for x in f['tags'] if not x.startswith('eh-') and x not in('pdata','gap-scan'))
    f['eh_only']=f['eh_ref'] and not others
EHTAGS=lambda tg: any(x.startswith('eh-') for x in tg)
# ---------------- classify no-pdata shapes
def decode_all(f):
    res=[]
    for a,b in f['ranges']:
        p=a
        while p<b:
            i=S.decode_one(p)
            if i is None: break
            res.append(i); p+=i.size
    return res
def mem_rip_target(i):
    for o in i.operands:
        if o.type==S.CS_OP_MEM and o.mem.base==S.RIP: return i.address+i.size+o.mem.disp
IAT=S.IATRANGE
ARGREGS=None
def classify_leaf(f):
    # only decode the code part (exclude data), small functions only
    if f['size']>48: return 'leaf-no-pdata'
    code=[]
    for a,b in f['ranges']:
        p=a
        dr=[(x,y) for x,y,k in f['data']]
        while p<b:
            if any(x<=p<y for x,y in dr): p=[y for x,y in dr if x<=p<y][0]; continue
            i=S.decode_one(p)
            if i is None: return 'leaf-no-pdata'
            code.append(i); p+=i.size
    code=[i for i in code if i.mnemonic not in('int3',)]
    ms=[i.mnemonic for i in code]
    if not code: return 'leaf-no-pdata'
    last=code[-1]
    if len(code)==1 and last.mnemonic=='jmp' and last.operands[0].type==S.CS_OP_MEM and last.operands[0].mem.base==S.RIP:
        tg=mem_rip_target(last)
        if IAT[0]<=tg<IAT[1]: return 'import-stub'
        return 'trampoline'
    if f['tags']&{'nep-unit'} and last.mnemonic=='jmp': return 'trampoline'
    if last.mnemonic=='jmp' and all(m in('jmp','nop','ud2') for m in ms) : return 'trampoline'
    if last.mnemonic=='jmp' and len(code)<=6 and 'ret' not in ms:
        simple=True
        for i in code[:-1]:
            if i.mnemonic in('sub','add','mov','lea','movsxd','neg','cdqe','xor'):
                ok=True
                for o in i.operands:
                    if o.type==S.CS_OP_MEM and o.mem.base not in(S.X.X86_REG_RCX,S.X.X86_REG_RAX,S.X.X86_REG_RDX,S.X.X86_REG_RIP): ok=False
                    if o.type==S.CS_OP_REG and o.reg not in(S.X.X86_REG_RCX,S.X.X86_REG_RAX,S.X.X86_REG_RDX,S.X.X86_REG_ECX,S.X.X86_REG_EAX,S.X.X86_REG_EDX,S.X.X86_REG_R8,S.X.X86_REG_R11): ok=False
                if not ok: simple=False
            else: simple=False
        if simple:
            if len(code)==1 and last.operands[0].type==S.CS_OP_IMM: return 'trampoline'
            return 'thunk'
    return 'leaf-no-pdata'
for f in F:
    if f['gap']:
        f['shape']=classify_leaf(f)
    else: f['shape']='root'
    if f['eh_only']: f['kind']='funclet'
    elif f['gap']: f['kind']=f['shape']
    else: f['kind']='root'
# ---------------- edges
def target_class(t):
    if t in by_start: return 'start',by_start[t]
    if t in frag_begin: return 'fragment',frag_begin[t]
    return None,None
all_ranges=[]
for f in F:
    for a,b in f['ranges']: all_ranges.append((a,b,f['idx']))
all_ranges.sort()
rstarts=[a for a,b,i in all_ranges]
def func_containing(x):
    k=bisect.bisect_right(rstarts,x)-1
    if k>=0 and all_ranges[k][0]<=x<all_ranges[k][1]: return F[all_ranges[k][2]]
    return None
ext_stats=collections.Counter(); interior=[]; badt=[]
for f in F:
    r=f['r']
    calls=sorted(set(t for s,t in r['calls']))
    tails=sorted(set(t for s,t,c in r['jmp_out']))
    extra_c=set(); extra_t=set()
    for a,b,k in f['unr']:
        if k!='unreached-code': continue
        p=a
        while p<b:
            i=S.decode_one(p)
            if i.mnemonic in('call','jmp') or i.mnemonic.startswith('j'):
                o=i.operands[0]
                if o.type==S.CS_OP_IMM:
                    t=o.imm
                    if not any(x<=t<y for x,y in f['ranges']): (extra_c if i.mnemonic=='call' else extra_t).add(t)
            p+=i.size
    calls=sorted(set(calls)|extra_c); tails=sorted(set(tails)|extra_t)
    f['calls']=calls; f['tails']=tails
    f['n_import_calls']=len(r['icall_mem']); f['n_ind_calls_reg']=r['icall_reg']
    f['n_ind_jmp']=len(r.get('unres_ijmp',[]))+len(r.get('ijmp_nonrip',[]))+len(r.get('ijmp_mem',[]))
    f['imports']=sorted(set('%s!%s'%iat[t] for s,t in r['icall_mem']+r.get('ijmp_mem',[]) if t in iat))
    f['n_jumptables']=len(r['jtabs'])
    for t in calls+tails:
        k,g=target_class(t)
        if k=='start': ext_stats['start']+=1
        elif k=='fragment': ext_stats['fragment-entry']+=1; g.setdefault('entered_via_fragment',set()).add(t)
        else:
            g2=func_containing(t)
            if g2 is not None and g2 is not f: ext_stats['interior-of-other']+=1; interior.append((f['start'],t,g2['start']))
            elif g2 is f: ext_stats['self-interior']+=1
            elif not S.in_exec(t): ext_stats['not-exec']+=1; badt.append((f['start'],t,'not-exec'))
            else: ext_stats['no-function']+=1; badt.append((f['start'],t,'no-function'))
print('edge target classes',dict(ext_stats))
# ---------------- virtual targets
def cname(raw): 
    c=CLS.get(raw); return c['name'] if c else raw
for f in F:
    vs=vt_slot_of.get(f['start'],[])
    f['virtual']=sorted(set('%s+%d:%d'%(cname(n),off,k) for n,off,k,v in vs))
    f['is_virtual']=bool(vs)
# ---------------- names / subsystem carry-over
for f in F:
    n=NAMES.get(f['start'])
    f['name']=n['name'] if n else None; f['class']=n['class'] if n else None
    f['subsystem']=n['subsystem'] if n else None; f['subsystem_basis']=n['subsystem_basis'] if n else None
    f['name_tier']=n['tier'] if n else None
    f['old_size']=n['size'] if n else None; f['in_s7']=bool(n)
# ---------------- flags
for f in F:
    fl=[]
    if f['gap'] and f['start']%16!=0: fl.append('unaligned16')
    if f.get('entered_via_fragment'): fl.append('entered-via-fragment')
    if f['eh_ref'] and not f['eh_only']: fl.append('eh-target-also-referenced')
    if f['n_ind_jmp']: fl.append('indirect-jmp')
    if f['gap'] and f['tags']=={'gap-scan'}: fl.append('unreferenced')
    if f['gap'] and 'gap-scan' in f['tags'] and f['tags']-{'gap-scan','rip-ref','jcc-out'}: fl.append('discovered-via-orphan-callees')
    if not f['gap']:
        ub=sum(b-a for a,b,k in f['unr'] if k!='padding-nop')
        # padding-only unreached bytes are benign
        f['unreached_bytes']=ub
    else: f['unreached_bytes']=0
    f['flags']=fl
def primary(f):
    order=['pdata','entry','export','vtable','init-xc','init-xi','reloc-ptr','clr-native','call','nep-unit','eh-cleanup','eh-catch','eh-seh-filter','eh-seh-finally','tailjmp','tls-callback','gap-scan']
    for o in order:
        if o in f['tags']: return o
    return ','.join(sorted(f['tags']))
for f in F: f['primary']=primary(f)
pickle.dump(F,open('F.pkl','wb'))
print('functions',len(F))
if __name__=='__main__' and '--dbg' in sys.argv:
    print('interior',[(hex(a),hex(b),hex(c)) for a,b,c in interior]); print('bad',[(hex(a),hex(b),c) for a,b,c in badt])
