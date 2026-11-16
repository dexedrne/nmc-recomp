#!/usr/bin/env python3
"""mkdrv.py --root RUNDIR --shards 0000,0045 --out DIR : files for tests/drv.c over a shard subset.
   DIR/drv_list.h   function table + stub address set      DIR/drv_stubs.c  deterministic stubs for every callee/import outside the subset"""
import sys, os, json, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from khlift import BASE, universe as U, imports as IM
import lift_all as LA
ap = argparse.ArgumentParser(); ap.add_argument('--root', required=True); ap.add_argument('--shards', required=True); ap.add_argument('--out', required=True)
ap.add_argument('--max-funcs', type=int, default=0); a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
sh = {s['name']: s for s in json.load(open(os.path.join(a.root, 'shards.json')))['shards']}
want = ['shard_%s.c' % x for x in a.shards.split(',')]
stats = {}
for l in open(os.path.join(a.root, 'lift_stats.jsonl')):
    r = json.loads(l); stats[r['rva']] = r
funcs = []
for w in want: funcs += sh[w]['funcs']
if a.max_funcs: funcs = funcs[:a.max_funcs]
own = set(funcs)
hooks = LA.load_hooks(); fs = U.load(); imps = IM.load_imports(); addr_of = {im.sym: im.addr for im in imps}
callees = set(); imports = set()
for rva in funcs:
    callees.update(stats[rva]['callees']); imports.update(stats[rva]['imports'])
H = ['#pragma once', '#include "khrt.h"']
for rva in funcs: H.append('void f_%x(KCtx *g);' % rva)
H.append('static const struct { u32 rva; void (*fn)(KCtx*); } drv_funcs[] = {')
for rva in funcs: H.append(' {0x%x,f_%x},' % (rva, rva))
H.append('};\n#define DRV_NFUNCS %d' % len(funcs))
allv = sorted({f.va for f in fs} | set(hooks) | set(addr_of.values()))
H.append('static const u64 stub_set[] = {' + ','.join('0x%xull' % v for v in allv) + '};\n#define stub_n %d' % len(allv))
open(os.path.join(a.out, 'drv_list.h'), 'w').write('\n'.join(H) + '\n')
S = ['#include "khrt.h"', 'void kh_test_stub(KCtx *g, u64 va);']
for c in sorted(callees - own):
    va = BASE + c; sym = hooks.get(va, 'f_%x' % c)
    S.append('void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (sym, va))
for sym in sorted(imports): S.append('void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (sym, addr_of[sym]))
open(os.path.join(a.out, 'drv_stubs.c'), 'w').write('\n'.join(S) + '\n')
print('functions', len(funcs), 'stubs', len(callees - own) + len(imports))
