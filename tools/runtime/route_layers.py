#!/usr/bin/env python3
"""route_layers.py : one table over several route runs (tools/route_one.py output directories): the counters, and whether the
mod's layers were at work in each run, read from the run's own log lines.

  tools/route_layers.py [--md] LABEL=RUNDIR [LABEL=RUNDIR ...]

Counters (summary.json of the run): exit / result, presents, draws, PSOs, resources, game file log hits / misses and the sha256 of
its sorted lines, field commands, damage calls and their sum, source voices, HD assets served, peak RSS, wall and cpu seconds.
Layers (run.out), each with the numbers its own report line prints:
  static edits   `kh: mod static: N edits verified and applied ... M re-lifted functions linked` + how many still hold at exit
  NeverlandFly   `kh: mod neverland: ... E edits / B bytes applied` + calls of the re-lifted row-step function 15b6d0
  KH2Moose       `kh: moose: ... cache C MiB ... data heap D MiB` + the most the cache held (KiB from the bottom)
  HD provenance  `kh: hd: provenance list ...: F mod files, R nohd rules` + tables shaped by it and vanilla files left out
  loader lock    `kh: bgload report: lock=on ... enqueues=Q` + contended acquisitions
  difficulty     `kh: difficulty forced=N` (the setting reached the game)
A layer whose line is missing shows MISSING, and the exit status is 1.  Python 3.6+, standard library.
"""
import hashlib
import json
import os
import re
import sys


def one(d):
    txt = open(os.path.join(d, 'run.out'), 'r', errors='replace').read()
    try: c = json.load(open(os.path.join(d, 'summary.json')))
    except (OSError, ValueError): c = {}
    try: fl = sorted(open(os.path.join(d, 'files.log'), 'r', errors='replace').read().splitlines())
    except OSError: fl = []
    def g(pat, fmt):
        m = re.search(pat, txt, re.M)
        return fmt(m) if m else 'MISSING'
    r = {
        'exit': '%s %s' % (c.get('exit', '?'), c.get('result', '?')),
        'presents': c.get('presents'), 'draws': c.get('draws'), 'PSOs': c.get('psos'), 'resources': c.get('resources'),
        'files hit/miss': '%s / %s' % (c.get('file_hits'), c.get('file_miss')),
        'file log sha': hashlib.sha256('\n'.join(fl).encode()).hexdigest()[:8] if fl else '-',
        'field cmds': c.get('field_cmds'), 'damage calls (sum)': '%s (%s)' % (c.get('damage_calls'), c.get('damage_sum')),
        'source voices': c.get('source_voices'), 'HD assets': c.get('hd_assets'), 'peak RSS MB': c.get('rss_mb'),
        'wall / cpu s': '%s / %s' % (c.get('wall_s'), c.get('cpu_s')),
        'static edits': g(r'^kh: mod static: (\d+) edits verified and applied .*?, (\d+) re-lifted functions linked',
                          lambda m: '%s applied, %s functions' % (m.group(1), m.group(2)))
                        + g(r'^kh: mod static: at exit (\d+) of (\d+) edits still hold', lambda m: '; %s of %s hold at exit' % (m.group(1), m.group(2))).replace('MISSING', ''),
        'NeverlandFly': g(r'^kh: mod neverland: 5 signatures verified .*?, (\d+) edits / (\d+) bytes applied', lambda m: '%s edits / %s bytes' % (m.group(1), m.group(2)))
                        + g(r'^kh: mod neverland 15b6d0 .*calls=(\d+)', lambda m: '; row step %s calls' % m.group(1)).replace('MISSING', ''),
        'KH2Moose pools': g(r'^kh: moose: .*?: cache (\d+) MiB .*?data heap (\d+) MiB', lambda m: 'cache %s + data %s MiB' % (m.group(1), m.group(2)))
                          + g(r'^kh: moose report: heaps=on; .*?most used from the bottom (\d+) KiB', lambda m: '; used %s KiB' % m.group(1)).replace('MISSING', ''),
        'HD provenance': g(r'^kh: hd: provenance list .*?: (\d+) mod files, (\d+) nohd rules', lambda m: '%s mod files, %s nohd rules' % (m.group(1), m.group(2)))
                         + g(r'tables shaped by it: (.*?); vanilla files left out=(\d+)', lambda m: '; left out %s' % m.group(2)).replace('MISSING', ''),
        'loader lock': g(r'^kh: bgload report: lock=(\w+) .*?enqueues=(\d+) .*?lock-contended=(\d+)', lambda m: '%s, %s enqueues, %s contended' % (m.group(1), m.group(2), m.group(3))),
        'difficulty': g(r'^kh: difficulty forced=(\d+)', lambda m: 'forced=%s' % m.group(1)) if c.get('difficulty', 'none') != 'none' else 'not set',
        'guest space': g(r'^kh: guest space: bias mode, .*? is host (0x[0-9a-f]+)\.\.', lambda m: 'bias, host ' + m.group(1)).replace('MISSING', 'identity'),
        'app container': 'yes (KH_IOS_PATHS)' if re.search(r'^kh: host dirs: KH_IOS_PATHS build', txt, re.M) else 'no',
    }
    return r


def main():
    args = sys.argv[1:]; md = False
    if args and args[0] == '--md': md = True; args = args[1:]
    runs = []
    for a in args:
        if '=' not in a:
            sys.stderr.write(__doc__); return 2
        label, d = a.split('=', 1)
        if not os.path.exists(os.path.join(d, 'run.out')):
            sys.stderr.write('route_layers: no run.out in %s\n' % d); return 2
        runs.append((label, one(d)))
    if not runs:
        sys.stderr.write(__doc__); return 2
    keys = list(runs[0][1].keys()); bad = 0
    rows = [['run'] + keys] + [[label] + ['-' if r[k] is None else str(r[k]) for k in keys] for label, r in runs]
    for label, r in runs:
        miss = [k for k in keys if 'MISSING' in str(r[k])]
        if miss: bad += 1; sys.stderr.write('route_layers: %s: MISSING %s\n' % (label, ', '.join(miss)))
    if md:
        # transposed: one column per run reads better in a status file
        print('| | ' + ' | '.join(label for label, _ in runs) + ' |'); print('|---|' + '---|' * len(runs))
        for i, k in enumerate(keys): print('| %s | ' % k + ' | '.join(rows[j + 1][i + 1] for j in range(len(runs))) + ' |')
    else:
        w = [max(len(row[i]) for row in rows) for i in range(len(rows[0]))]
        for row in rows: print('  '.join(x.ljust(n) for x, n in zip(row, w)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
