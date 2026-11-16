#!/usr/bin/env python3
"""select_sets.py STATS OUTDIR : choose differential-test candidates by structure from lift_stats.jsonl (seeded, deterministic)."""
import sys, json, random, os
stats, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
rows = [json.loads(l) for l in open(stats)]
ok = [r for r in rows if r['status'] == 'ok' and 6 <= r['insns'] <= 600 and r['rva'] not in [c for c in r['callees'] if c == r['rva']]]
def pick(name, pred, n, seed):
    c = [r['rva'] for r in ok if pred(r)]
    random.Random(seed).shuffle(c); c = sorted(c[:n])
    open(os.path.join(out, name + '.txt'), 'w').write(','.join('%x' % x for x in c)); print(name, len(c), 'of', len([1 for r in ok if pred(r)]))
pick('direct', lambda r: r['callees'] and not r['dcall'] and not r['djmp'] and not r['jt'] and not r['imports'], 220, 11)
pick('indirect', lambda r: r['dcall'] >= 1 and not r['jt'], 110, 12)
pick('jt', lambda r: r['jt'] >= 1, 60, 13)
pick('tail', lambda r: r['tails'] >= 1 and r['insns'] <= 80, 60, 14)
pick('imports', lambda r: bool(r['imports']), 60, 15)
pick('leaf', lambda r: not r['callees'] and not r['imports'] and not r['dcall'] and not r['djmp'] and not r['jt'], 120, 16)
