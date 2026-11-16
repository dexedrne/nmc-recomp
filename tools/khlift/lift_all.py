#!/usr/bin/env python3
"""Lift the function universe to sharded C.

  lift_all.py --out DIR [--all | --only RVA,RVA | --sample N [--seed S] | --rvas-file F] [--jobs J] [--shard-bytes B]

Writes DIR/shards/shard_NNNN.c, DIR/lift_stats.jsonl (one row per function), DIR/shards.json (manifest), DIR/summary.json.
"""
import sys, os, json, time, random, argparse, hashlib, collections, multiprocessing as mp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from khlift import BASE, universe as U
from khlift.emit import LiftEnv, FnLifter
from khlift import imports as IM

HERE = os.path.dirname(os.path.abspath(__file__))
G = {}

def load_hooks():
    h = {}
    for ln in open(os.path.join(HERE, 'hooks.tsv')):
        if ln.startswith('#') or not ln.strip(): continue
        p = ln.rstrip('\n').split('\t')
        h[BASE + int(p[0], 16)] = p[1]
    return h

def build_env():
    fs = U.load()
    img = U.image()
    hooks = load_hooks()
    imps = IM.load_imports()
    iat = {im.slot: im.sym for im in imps}
    liftable = [f for f in fs if not f.nep and f.va not in hooks]
    starts = {f.va: f for f in fs if not f.nep}
    nep = {f.va for f in fs if f.nep}
    env = LiftEnv(img, starts, hooks, iat, nep)
    return fs, liftable, env, imps

def init():
    fs, liftable, env, imps = build_env()
    G['fs'] = {f.rva: f for f in fs}; G['env'] = env

def work(rva):
    f = G['fs'][rva]
    t0 = time.time()
    try:
        L = FnLifter(G['env'], f, trace=G.get('trace', False)); r = L.lift()
    except Exception as e:
        return dict(rva=rva, status='fail', err='exc:%s:%s' % (type(e).__name__, str(e)[:80]), text='')
    row = dict(rva=rva, status='ok' if not r.err else 'fail', err=r.err, text=r.text, insns=r.insns, blocks=r.blocks,
               callees=sorted(c - BASE for c in r.callees), imports=sorted(r.imports), dcall=r.n_dispatch_call,
               djmp=r.n_dispatch_jmp, jt=r.n_jt, v256=r.v256, tails=r.tails, cpuid=r.cpuid, dup=r.dup_blocks,
               nunsup=len(r.unsup), unsup=r.unsup[:4], sec=round(time.time() - t0, 4))
    return row

def write_shards(rows, outdir, shard_bytes, hooks, fs_by_rva):
    sd = os.path.join(outdir, 'shards'); os.makedirs(sd, exist_ok=True)
    for fn in os.listdir(sd): os.unlink(os.path.join(sd, fn))
    rows = sorted((r for r in rows if r['status'] == 'ok'), key=lambda r: r['rva'])
    shards = []; cur = []; cursz = 0
    def flush():
        nonlocal cur, cursz
        if not cur: return
        idx = len(shards); name = 'shard_%04d.c' % idx
        own = {r['rva'] for r in cur}
        ext = set(); imps = set()
        for r in cur:
            ext.update(r['callees']); imps.update(r['imports'])
        # callee symbol: hook or f_
        decl = []
        for c in sorted(ext):
            va = BASE + c
            decl.append('void %s(KCtx *g);' % hooks.get(va, 'f_%x' % c))
        for s in sorted(imps): decl.append('void %s(KCtx *g);' % s)
        body = '/* kh2fm lifted shard %d: %d functions, rva 0x%x..0x%x (game-derived: never commit) */\n#include "khrt.h"\n%s\n\n%s' % (
            idx, len(cur), cur[0]['rva'], cur[-1]['rva'], '\n'.join(decl), '\n'.join(r['text'] for r in cur))
        data = body.encode()
        open(os.path.join(sd, name), 'wb').write(data)
        shards.append(dict(name=name, nfuncs=len(cur), bytes=len(data), rva_lo=cur[0]['rva'], rva_hi=cur[-1]['rva'],
                           sha256=hashlib.sha256(data).hexdigest(), funcs=[r['rva'] for r in cur],
                           avx=any(r['v256'] for r in cur), ijmp=any(r['djmp'] for r in cur)))
        cur = []; cursz = 0
    for r in rows:
        sz = len(r['text'])
        if cur and cursz + sz > shard_bytes: flush()
        cur.append(r); cursz += sz
    flush()
    return shards

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--only'); ap.add_argument('--rvas-file')
    ap.add_argument('--sample', type=int); ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--shard-bytes', type=int, default=1_500_000)
    ap.add_argument('--trace', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    fs, liftable, env, imps = build_env()
    if a.only: rvas = [int(x, 16) for x in a.only.split(',')]
    elif a.rvas_file: rvas = [int(x, 16) for x in open(a.rvas_file).read().split()]
    elif a.sample: rvas = sorted(random.Random(a.seed).sample([f.rva for f in liftable], a.sample))
    else: rvas = [f.rva for f in liftable]
    G['trace'] = a.trace
    t0 = time.time()
    rows = []
    with mp.Pool(a.jobs, initializer=init) as p:
        for i, r in enumerate(p.imap_unordered(work, rvas, chunksize=32)):
            rows.append(r)
    rows.sort(key=lambda r: r['rva'])
    el = time.time() - t0
    shards = write_shards(rows, a.out, a.shard_bytes, env.hooks, {f.rva: f for f in fs})
    with open(os.path.join(a.out, 'lift_stats.jsonl'), 'w') as fo:
        for r in rows:
            r2 = {k: v for k, v in r.items() if k != 'text'}; r2['cbytes'] = len(r['text']); fo.write(json.dumps(r2) + '\n')
    json.dump(dict(shards=shards), open(os.path.join(a.out, 'shards.json'), 'w'))
    ok = [r for r in rows if r['status'] == 'ok']; bad = [r for r in rows if r['status'] != 'ok']
    why = collections.Counter((r['err'] or '')[:70] for r in bad)
    summ = dict(requested=len(rows), lifted=len(ok), failed=len(bad), fail_reasons=why.most_common(), c_bytes=sum(len(r['text']) for r in ok),
                insns=sum(r['insns'] for r in ok), shards=len(shards), seconds=round(el, 1),
                dispatch_call_sites=sum(r['dcall'] for r in ok), dispatch_jmp_sites=sum(r['djmp'] for r in ok), jump_tables=sum(r['jt'] for r in ok),
                v256_functions=sum(1 for r in ok if r['v256']), cpuid_sites=sum(r['cpuid'] for r in ok), dup_blocks=sum(r['dup'] for r in ok))
    json.dump(summ, open(os.path.join(a.out, 'summary.json'), 'w'), indent=1)
    print(json.dumps(summ, indent=1))
    if bad: print('first failures:', [(hex(r['rva']), r['err']) for r in bad[:10]])

if __name__ == '__main__':
    main()
