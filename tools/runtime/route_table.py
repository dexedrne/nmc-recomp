#!/usr/bin/env python3
"""route_table.py : one table over many route runs (Linux tools/route_batch.sh run directories and tools/route_one.py output
directories), for comparing hosts and build variants.   route_table.py DIR [DIR ...]   (a DIR holds run.out + files.log)
Columns: outcome class of the route = the sorted game file log (323 lines: the scripted battle started; 317: it did not within the
run), then the counters of the exit report.  Python 3.6+, standard library."""
import glob, hashlib, os, re, sys

def last(pat, txt, conv=int):
    m = None
    for m in re.finditer(pat, txt): pass
    return conv(m.group(1)) if m else None

def row(d):
    txt = open(os.path.join(d, 'run.out'), errors='replace').read()
    fl = sorted(open(os.path.join(d, 'files.log'), errors='replace').read().splitlines()) if os.path.exists(os.path.join(d, 'files.log')) else []
    gl = [l for l in txt.splitlines() if 'kh: gfx: draws=' in l and 'gfx frame' not in l]
    gl = ' ' + (gl[-1] if gl else '')
    num = lambda n: last(r'[ =:]%s=(\d+)' % n, gl)
    ex = open(os.path.join(d, 'exit')).read().strip() if os.path.exists(os.path.join(d, 'exit')) else '?'
    t = None
    for cand in (os.path.join(d, 'time'),):
        if os.path.exists(cand):
            p = open(cand).read().split(); t = (int(float(p[3])), int(float(p[0]) + float(p[1])), int(p[2]))
    sj = os.path.join(d, 'summary.json')
    if t is None and os.path.exists(sj):
        import json; j = json.load(open(sj)); t = (j['wall_s'], j['cpu_s'], j['rss_mb'])
    cls = {323: 'battle', 317: 'no-battle'}.get(len(fl), '?%d' % len(fl))
    m = re.search(r'pools: (\d+)\+(\d+) MiB touched', txt)
    return [ex, cls, hashlib.sha256('\n'.join(fl).encode()).hexdigest()[:8], num('presents'), last(r'kh: input report:.* game-frames=(\d+)', txt), num('draws'), num('psos'), num('resources'),
            sum(1 for l in fl if l.startswith('HIT ')), sum(1 for l in fl if l.startswith('MISS ')),
            sum(int(x) for x in re.findall(r'FIELD_COMMAND\.execute=(\d+)', open(os.path.join(d, 'phase.log'), errors='replace').read())) if os.path.exists(os.path.join(d, 'phase.log')) else None,
            sum(int(x) for x in re.findall(r'kh: difficulty damage slot=\d+ calls=(\d+)', txt)), last(r'voices created: source=(\d+)', txt), last(r'assets-served=(\d+)', txt),
            last(r'kh: object-id table: (\d+) of 64', txt), last(r'id zone .*high-water 0x[0-9a-f]+ = (\d+) MiB', txt), last(r'far zone .*high-water 0x[0-9a-f]+ = (\d+) MiB', txt),
            last(r'kh: threads: (\d+) created', txt), last(r'InitialSetting state 3 first seen at present (\d+)', txt),
            ('%s+%s' % m.groups()) if m else None] + (list(t) if t else [None, None, None])

cols = ['run', 'exit', 'class', 'filelog', 'presents', 'gframes', 'draws', 'psos', 'res', 'hits', 'miss', 'fieldcmd', 'dmg', 'voices', 'hd', 'slots', 'idMiB', 'farMiB', 'thr', 'prompt', 'poolMiB', 'wall', 'cpu', 'rss']
rows = []
for a in sys.argv[1:]:
    for d in sorted(glob.glob(a)):
        if os.path.exists(os.path.join(d, 'run.out')):
            lab = d.replace('run-F20/', '').replace('run-subset/', 'subset:').replace('/r01', '')
            rows.append([lab] + ['-' if v is None else str(v) for v in row(d)])
w = [max(len(c), *(len(r[i]) for r in rows)) if rows else len(c) for i, c in enumerate(cols)]
print('  '.join(c.ljust(n) for c, n in zip(cols, w)))
for r in rows: print('  '.join(c.ljust(n) for c, n in zip(r, w)))
