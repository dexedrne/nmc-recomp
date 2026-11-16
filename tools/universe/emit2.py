#!/usr/bin/env python3
"""fragments.tsv, funclets.tsv, reconcile.json, summary.json (reads F.pkl; run after emit.py)"""
import pickle, json, collections
import x64scan as S
F=pickle.load(open('F.pkl','rb'))
G=pickle.load(open('gapfn.pkl','rb'))
H=lambda x:'0x%x'%x
BASE=S.BASE
S7=json.load(open('../S7-name-db/name_db.json'))
s7=set(int(r['rva'],16) for r in S7); s7leaf=set(int(r['rva'],16) for r in S7 if not r['pdata']); s7root=s7-s7leaf
by={f['start']:f for f in F}
# ---- fragments
nf=0
with open('fragments.tsv','w') as fh:
    fh.write('id\tstart_rva\told_size_first_fragment\tmerged_size\tdelta\tnranges\tranges\tn_contiguous\tn_split\n')
    for f in F:
        if f['has_pdata'] and len(f['ranges'])>1:
            nf+=1
            old=f['old_size']
            adj=sum(1 for a,b,k in f['frags'] if k=='contiguous'); cold=sum(1 for a,b,k in f['frags'] if k=='split')
            fh.write('\t'.join([f['id'],H(f['start']),str(old),str(f['size']),str(f['size']-(old or 0)),str(len(f['ranges'])),';'.join('%s-%s'%(H(a),H(b)) for a,b in f['ranges']),str(adj),str(cold)])+'\n')
# ---- funclets
fk=collections.Counter()
with open('funclets.tsv','w') as fh:
    fh.write('id\tstart_rva\tsize\thas_pdata\tfunclet_kind\tparents\tkind\tother_refs\n')
    for f in F:
        if f['eh_ref']:
            fk[(f['funclet_kind'],f['has_pdata'])]+=1
            fh.write('\t'.join([f['id'],H(f['start']),str(f['size']),str(int(f['has_pdata'])),f['funclet_kind'],';'.join(H(x) for x in f['funclet_parents']),f['kind'],','.join(sorted(t for t in f['tags'] if not t.startswith('eh-') and t!='pdata'))])+'\n')
# ---- reconcile with S7
ours=set(by); gap_ours=set(f['start'] for f in F if not f['has_pdata'])
rec=dict(
  s7_universe=len(s7),s7_roots=len(s7root),s7_leaf=len(s7leaf),
  ours_total=len(F),ours_pdata_logical=sum(1 for f in F if f['has_pdata']),ours_leaf_no_pdata=len(gap_ours),
  s7_roots_missing_in_ours=len(s7root-ours),s7_leaf_missing_in_ours=len(s7leaf-ours),
  s7_leaf_found_in_ours=len(s7leaf&gap_ours),
  s7_leaf_now_pdata_or_other=sorted(H(x) for x in (s7leaf-gap_ours))[:50],
  ours_only_by_primary_source=dict(collections.Counter(by[x]['primary'] for x in gap_ours-s7leaf)),
  ours_only_by_kind=dict(collections.Counter(by[x]['kind'] for x in gap_ours-s7leaf)),
  ours_only_total=len(gap_ours-s7leaf),
  ours_only_unreferenced=sum(1 for x in gap_ours-s7leaf if by[x]['confidence']=='low'),
  ours_only_funclets=sum(1 for x in gap_ours-s7leaf if by[x]['kind']=='funclet'),
)
rec['s7_leaf_set_by_ours_primary']=dict(collections.Counter(by[x]['primary'] for x in s7leaf&gap_ours))
rec['s7_universe_vs_ours_explained']=dict(s7=len(s7),plus_ours_only=len(gap_ours-s7leaf),minus_s7_leaf_not_leaf_here=len(s7leaf-gap_ours),equals=len(s7)+len(gap_ours-s7leaf)-len(s7leaf-gap_ours))
json.dump(rec,open('reconcile.json','w'),indent=1)
print(json.dumps(rec,indent=1))
print('funclet kinds',dict(fk))
print('fragment funcs',nf)
# ---- summary
kind=collections.Counter(f['kind'] for f in F)
summ=dict(
  exe_sha256_prefix=S.P.sha256[:16],n_functions=len(F),kinds=dict(kind),
  by_primary_source=dict(collections.Counter(f['primary'] for f in F)),
  by_confidence=dict(collections.Counter(f['confidence'] for f in F)),
  pdata_entries=len(S.P.pe.DIRECTORY_ENTRY_EXCEPTION) if hasattr(S.P.pe,'DIRECTORY_ENTRY_EXCEPTION') else None,
  logical_pdata_functions=sum(1 for f in F if f['has_pdata']),
  roots_with_fragments=nf,
  fragments_merged=sum(len(f['frags']) for f in F),
  funclets_total=sum(1 for f in F if f['kind']=='funclet'),
  funclets_with_pdata=sum(1 for f in F if f['kind']=='funclet' and f['has_pdata']),
  funclets_no_pdata=sum(1 for f in F if f['kind']=='funclet' and not f['has_pdata']),
  virtual_target_functions=sum(1 for f in F if f['is_virtual']),
  vtables=1066,
)
json.dump(summ,open('summary.json','w'),indent=1)
print(json.dumps(summ,indent=1))
