#!/usr/bin/env python3
"""report_diff.py RESULT.json... : aggregate difftest results (functions, valid tests, mismatches, stub/dispatch coverage)."""
import sys, json, collections
tot = collections.Counter(); mm = []
for fn in sys.argv[1:]:
    d = json.load(open(fn)); rows = d['rows']
    c = collections.Counter()
    for r in rows:
        c['functions'] += 1
        if r['valid'] > 0:
            c['with_valid'] += 1
            if not r['mismatch'] and not r['crash']: c['all_match'] += 1
        if r['mismatch']: c['mismatching'] += 1; mm.append((fn, r['rva'], r['valid'], r['mismatch'], str(r['examples'])[:300]))
        c['valid_tests'] += r['valid']; c['stub_calls'] += r.get('stub_calls', 0); c['dispatch_calls'] += r.get('dispatch_calls', 0)
        c['mismatch_tests'] += r['mismatch']
        if r['info'].get('callees'): c['fn_direct'] += 1
        if r['info'].get('dcall'): c['fn_dispatch_call'] += 1
        if r['info'].get('jt'): c['fn_jt'] += 1
        if r['info'].get('v256'): c['fn_v256'] += 1
    c['lift_excluded'] = len(d['lift_fail'])
    print(fn.split('/')[-1], dict(c))
    tot.update(c)
print('TOTAL', dict(tot))
for m in mm: print('MISMATCH', m)
