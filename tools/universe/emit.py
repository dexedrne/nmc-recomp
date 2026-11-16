#!/usr/bin/env python3
"""Write functions.tsv/json, ranges.tsv, gaps.tsv, funclets.tsv, fragments.tsv, overlaps.tsv, coverage.json, reconcile.json from F.pkl"""
import pickle, json, collections, bisect, struct, sys
import x64scan as S
exec(open('build.py').read().split("pickle.dump(F,open('F.pkl','wb'))")[0])   # rebuild in-memory F (single source of truth)
pickle.dump(F,open('F.pkl','wb'))
H=lambda x:'0x%x'%x
def conf(f):
    if f['has_pdata']: return 'certain'
    if f['primary']!='gap-scan': return 'high'
    return 'medium' if 'rip-ref' in f['tags'] else 'low'
for f in F: f["confidence"]=conf(f)
pickle.dump(F,open("F.pkl","wb"))
COLS=['id','start_rva','start_va','size','nranges','ranges','nextents','extents','kind','confidence','primary_source','sources','has_pdata','n_pdata_entries','unwind_flags','handler_type',
      'prolog_size','funclet_kind','funclet_parents','is_virtual_target','virtual_slots','ncalls','calls','ntail','tail_targets','n_import_calls','n_ind_calls','n_ind_jmp',
      'n_jumptables','data_in_code_bytes','unreached_bytes','subsystem','subsystem_basis','name','class','name_tier','s7_size','flags']
def row(f):
    dic=sum(b-a for a,b,k in f['data'])
    return [f['id'],H(f['start']),H(f['start']+BASE),f['size'],len(f['ranges']),';'.join('%s-%s'%(H(a),H(b)) for a,b in f['ranges']),len(f['extents']),';'.join('%s-%s'%(H(a),H(b)) for a,b in f['extents']),f['kind'],f['confidence'],f['primary'],','.join(sorted(f['tags'])),
            int(f['has_pdata']),f['n_pdata'],f['unwind_flags'],f['handler_type'],f['prolog'],f['funclet_kind'],';'.join(H(x) for x in f['funclet_parents']),int(f['is_virtual']),';'.join(f['virtual']),
            len(f['calls']),';'.join(H(x) for x in f['calls']),len(f['tails']),';'.join(H(x) for x in f['tails']),f['n_import_calls'],f['n_ind_calls_reg'],f['n_ind_jmp'],
            f['n_jumptables'],dic,f['unreached_bytes'],f['subsystem'] or '',f['subsystem_basis'] or '',f['name'] or '',f['class'] or '',f['name_tier'] or '',
            f['old_size'] if f['old_size'] is not None else '',','.join(f['flags'])]
with open('functions.tsv','w') as fh:
    fh.write('\t'.join(COLS)+'\n')
    for f in F: fh.write('\t'.join(str(x) for x in row(f))+'\n')
J=[]
for f in F:
    J.append(dict(id=f['id'],start_rva=f['start'],start_va=f['start']+BASE,size=f['size'],ranges=[[a,b] for a,b in f['ranges']],extents=[[a,b] for a,b in f['extents']],kind=f['kind'],confidence=f['confidence'],
        primary_source=f['primary'],sources=sorted(f['tags']),has_pdata=f['has_pdata'],n_pdata_entries=f['n_pdata'],unwind_flags=f['unwind_flags'],handler_type=f['handler_type'],
        prolog_size=f['prolog'],funclet_kind=f['funclet_kind'],funclet_parents=f['funclet_parents'],is_virtual_target=f['is_virtual'],virtual_slots=f['virtual'],
        calls=f['calls'],tail_targets=f['tails'],n_import_calls=f['n_import_calls'],n_ind_calls=f['n_ind_calls_reg'],n_ind_jmp=f['n_ind_jmp'],imports=f['imports'],
        data_in_code=[[a,b,k] for a,b,k in f['data']],unreached_ranges=[[a,b,k] for a,b,k in f['unr']],unreached_bytes=f['unreached_bytes'],
        fragments=[dict(begin=a,end=b,role=k) for a,b,k in f['frags']],
        subsystem=f['subsystem'],subsystem_basis=f['subsystem_basis'],name=f['name'],cls=f['class'],name_tier=f['name_tier'],s7_size=f['old_size'],flags=f['flags']))
json.dump(dict(exe_sha256=P.sha256,image_base=BASE,n_functions=len(J),functions=J),open('functions.json','w'))
# ---- ranges.tsv
with open('ranges.tsv','w') as fh:
    fh.write('func_id\tstart_rva\tend_rva\tsize\trole\tsection\tpdata_entry_rva\tchain_parent_rva\n')
    for f in F:
        fr={a:(b,k) for a,b,k in f['frags']}
        for n,(a,b) in enumerate(f['ranges']):
            if f['has_pdata']:
                if a==f['start']: role='primary'; par=''
                else:
                    role='chained-'+fr[a][1] if a in fr else 'chained'
                    ent=out[entry_by_begin[a]]; par=H(ent['chain'][0]) if ent['chain'] else ''
                fh.write('\t'.join([f['id'],H(a),H(b),str(b-a),role,secname(a),H(a),par])+'\n')
            else:
                fh.write('\t'.join([f['id'],H(a),H(b),str(b-a),'primary' if n==0 else 'secondary',secname(a),'',''])+'\n')
# ---- overlaps
allr=sorted((a,b,f['id']) for f in F for a,b in f['ranges'])
ov=[]
mx=(0,0,'')
for a,b,i in allr:
    if a<mx[1]: ov.append((mx[2],i,a,min(b,mx[1])))
    if b>mx[1]: mx=(a,b,i)
# IL / non-function overlaps
ilr=[(a,b) for a,b,_,_ in IL]
for a,b in ilr:
    k=bisect.bisect_right([x[0] for x in allr],a)-1
    for x in allr[max(0,k-1):k+3]:
        if x[0]<b and a<x[1]: ov.append(('IL',x[2],max(a,x[0]),min(b,x[1])))
with open('overlaps.tsv','w') as fh:
    fh.write('a\tb\toverlap_start\toverlap_end\n')
    for a,b,c,d in ov: fh.write('%s\t%s\t%s\t%s\n'%(a,b,H(c),H(d)))
print('range overlaps',len(ov),'ranges',len(allr))
# ---- coverage
EXE_END=max(b for n,(a,b) in EXECSECS.items())
cls=bytearray(EXE_END+16)   # 0 unknown,1 pdata fn,2 leaf fn,3 IL,4 il-pad,5 int3,6 nop
for f in F:
    for a,b in f['ranges']:
        for k in range(a,b): cls[k]=1 if f['has_pdata'] else 2
for a,b,_,_ in IL:
    for k in range(a,b): cls[k]=3
def mark_unknown_runs(sec):
    a,b=EXECSECS[sec]; runs=[]
    p=a
    while p<b:
        if cls[p]: p+=1; continue
        q=p
        while q<b and not cls[q]: q+=1
        runs.append((p,q)); p=q
    return runs
ilstarts=sorted(a for a,b,_,_ in IL)
def classify_run(a,b):
    d=img[a:b]
    if all(x==0xcc for x in d): return 'padding-int3'
    # decode as nop/int3 mix
    p=a; ok=True
    while p<b:
        i=S.decode_one(p)
        if i is None or p+i.size>b or i.mnemonic not in('nop','int3'): ok=False; break
        p+=i.size
    if ok: return 'padding-nop'
    if all(x==0 for x in d) and b-a<=3 and b in ilstarts: return 'il-align-pad'
    # leading nops/int3 then il-align? mixed int3 + zero pad directly before IL body
    q=a
    while q<b and img[q]==0xcc: q+=1
    if all(x==0 for x in img[q:b]) and b-q<=3 and b in ilstarts: return 'padding-int3+il-align-pad'
    return 'unknown'
gaps=[]
gstat=collections.Counter(); gbytes=collections.Counter()
for sec in EXECSECS:
    for a,b in mark_unknown_runs(sec):
        # split runs further into maximal same-class pieces (int3 run vs nop run vs other)
        k=classify_run(a,b)
        if k=='unknown':
            # try splitting on int3 runs
            p=a
            while p<b:
                if img[p]==0xcc:
                    q=p
                    while q<b and img[q]==0xcc: q+=1
                    gaps.append((p,q,'padding-int3',sec)); gstat['padding-int3']+=1; gbytes['padding-int3']+=q-p; p=q
                else:
                    q=p
                    while q<b and img[q]!=0xcc: q+=1
                    kk=classify_run(p,q); gaps.append((p,q,kk,sec)); gstat[kk]+=1; gbytes[kk]+=q-p; p=q
        else:
            gaps.append((a,b,k,sec)); gstat[k]+=1; gbytes[k]+=b-a
ilname={a:'%s.%s'%(t,n) for a,b,t,n in IL}
for a,b,t,n in IL: gaps.append((a,b,'clr-il-body',secname(a))); gstat['clr-il-body']+=1; gbytes['clr-il-body']+=b-a
gaps.sort()
with open('gaps.tsv','w') as fh:
    fh.write('start_rva\tend_rva\tsize\tclass\tsection\tdetail\n')
    for a,b,k,sec in gaps: fh.write('%s\t%s\t%d\t%s\t%s\t%s\n'%(H(a),H(b),b-a,k,sec,ilname.get(a,'')))
cov={}
for sec,(a,b) in EXECSECS.items():
    seg=cls[a:b]; tot=b-a
    c=collections.Counter(seg)
    pdb=c[1]; lb=c[2]
    cov[sec]=dict(rva_start=a,rva_end=b,bytes=tot,pdata_function_bytes=pdb,leaf_function_bytes=lb,function_bytes=pdb+lb,function_fraction=(pdb+lb)/tot)
tot=sum(v['bytes'] for v in cov.values())
# in-function data
dic=sum(b-a for f in F for a,b,k in f['data']); pdpad=0
for f in F:
    if f['has_pdata']:
        for a,b in f['ranges']:
            t=b
            while t>a and img[t-1]==0xcc: t-=1
            pdpad+=b-t
tot_fn=sum(v['function_bytes'] for v in cov.values())
cov_all=dict(total_exec_bytes=tot,function_bytes=tot_fn,function_fraction=tot_fn/tot,
             non_function_classes={k:dict(runs=gstat[k],bytes=gbytes[k],fraction=gbytes[k]/tot) for k in gstat},
             jump_table_bytes_inside_function_ranges=dic,trailing_int3_inside_pdata_ranges=pdpad)
unk=gbytes['unknown']
accounted=tot_fn+sum(gbytes.values())
cov_all['accounted_bytes']=accounted; cov_all['accounted_fraction']=accounted/tot
cov_all['by_section']=cov
cov_all['pdata_only_fraction_of_text']=cov['.text']['pdata_function_bytes']/cov['.text']['bytes']
json.dump(cov_all,open('coverage.json','w'),indent=1)
print(json.dumps(cov_all,indent=1)[:2500])
pickle.dump(dict(gaps=gaps),open('gaps.pkl','wb'))
