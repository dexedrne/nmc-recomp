#!/usr/bin/env python3
"""report_static.py RUNDIR : static lift statistics + coverage versus the universe (insn counts, data, nep, skipped ranges)."""
import sys, json, pickle, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from khlift import universe as U
root = sys.argv[1]
rows = [json.loads(l) for l in open(os.path.join(root, 'lift_stats.jsonl'))]
F = pickle.load(open(os.path.join(U.UNIVERSE_DIR, 'F.pkl'), 'rb')); FR = {x['start']: x for x in F}
ok = {r['rva']: r for r in rows if r['status'] == 'ok'}
less = []; tot_u = 0; tot_l = 0
for rva, r in ok.items():
    x = FR[rva]['r']; n = len(x.get('insn_starts') or x.get('insn') or [])
    tot_u += n; tot_l += r['insns']
    if r['insns'] < n: less.append((rva, n, r['insns']))
nfun = len(F); nep = sum(1 for x in F if x['r'].get('nep_unit'))
out = dict(universe_functions=nfun, nep_units_skipped=nep, hooks_not_lifted=2, lifted=len(ok), failed=len(rows) - len(ok),
           insns_lifted=tot_l, insns_universe_for_lifted=tot_u, functions_with_fewer_insns_than_universe=len(less),
           call_sites_dispatch=sum(r['dcall'] for r in ok.values()), jmp_sites_dispatch=sum(r['djmp'] for r in ok.values()),
           functions_with_dispatch_jmp=sum(1 for r in ok.values() if r['djmp']), functions_with_dispatch_call=sum(1 for r in ok.values() if r['dcall']),
           jump_tables=sum(r['jt'] for r in ok.values()), functions_with_jt=sum(1 for r in ok.values() if r['jt']),
           direct_callee_edges=sum(len(r['callees']) for r in ok.values()), functions_with_direct_calls=sum(1 for r in ok.values() if r['callees']),
           import_symbols_used=len({s for r in ok.values() for s in r['imports']}), functions_with_import_calls=sum(1 for r in ok.values() if r['imports']),
           tail_jump_sites=sum(r['tails'] for r in ok.values()), v256_functions=sum(1 for r in ok.values() if r['v256']), c_bytes=sum(r['cbytes'] for r in ok.values()),
           universe_ind_jmp_functions=sum(1 for x in F if x['r']['unres_ijmp'] or x['n_ind_jmp']))
print(json.dumps(out, indent=1))
print('examples fewer-insns:', [(hex(a), b, c) for a, b, c in less[:5]])
json.dump(out, open(os.path.join(root, 'static_report.json'), 'w'), indent=1)
