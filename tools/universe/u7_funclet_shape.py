#!/usr/bin/env python3
"""Cross-check of the EH-table funclet evidence against the 'funclet shape' (funclets get the establisher frame in RDX).
The classifier of record is the EH tables; the shape is only a corroboration and a search for unreferenced candidates."""
import json, re, collections, pickle
import x64scan as S
F=pickle.load(open('F.pkl','rb'))
STRICT=re.compile(r'^(mov qword ptr \[rsp \+ 0x10\], rdx ; |mov qword ptr \[rsp \+ 8\], rdx ; )?push rbp ; sub rsp, 0x[0-9a-f]+ ; (mov rbp, rdx|lea rbp, \[rdx( \+ 0x[0-9a-f]+)?\])')
def head(a):
    ins=list(S.md.disasm(S.img[a:a+28],a))[:4]
    return ' ; '.join('%s %s'%(i.mnemonic,i.op_str) for i in ins)
res=collections.Counter(); cand=[]
for f in F:
    if not f['has_pdata']: continue
    m=bool(STRICT.match(head(f['start'])))
    other=sorted(t for t in f['tags'] if t!='pdata' and not t.startswith('eh-'))
    if f['kind']=='funclet':
        res['pdata_funclets']+=1; res['pdata_funclets_matching_strict_shape']+=int(m)
    elif m:
        if other: res['roots_matching_shape_but_called_or_virtual']+=1
        else: res['roots_matching_shape_and_unreferenced']+=1; cand.append(hex(f['start']))
json.dump(dict(res,unreferenced_shape_candidates=cand),open('funclet_shape_check.json','w'),indent=1)
print(dict(res),cand)
