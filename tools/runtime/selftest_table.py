#!/usr/bin/env python3
"""selftest_table.py : per-suite check counts of several `khnmc --selftest` outputs side by side, and what differs.

  tools/selftest_table.py LABEL=FILE [LABEL=FILE ...]

Reads the lines `kh: <suite> selftest: N checks, M failures` (the unnamed core suite is called "core"; the two bgload scenario
lines are parts of the bgload line and are not counted twice), `... selftest: SKIPPED (...)`, the `NOT RUN` lines of a
KH_IOS_PATHS build (runtime/host_env.c) and the three hashes the selftest prints.  The first file is the reference.  For every
other file and every suite: the difference to the reference, and whether the NOT RUN lines of that suite explain it exactly
(reference count - this count == checks reported as not run).  A suite whose difference is not explained that way is listed
under UNEXPLAINED, and the exit status is 1; SKIPPED suites and suites that exist in one file only are listed by name.
Python 3.6+, standard library.
"""
import re
import sys


def parse(path):
    txt = open(path, 'r', errors='replace').read()
    suites, order, skipped, notrun = {}, [], {}, {}
    for m in re.finditer(r'^kh: (?:(.+?) )?selftest: (\d+) checks, (\d+) failures', txt, re.M):
        name = m.group(1) or 'core'
        if name not in suites:
            order.append(name)
        suites[name] = (int(m.group(2)), int(m.group(3)))
    for m in re.finditer(r'^kh: (.+?) selftest: SKIPPED \((.*)\)', txt, re.M):
        skipped[m.group(1)] = m.group(2)
        if m.group(1) not in order:
            order.append(m.group(1))
    for m in re.finditer(r'^kh: selftest: NOT RUN \([^)]*\): (.+?): .*: (\d+) checks? not executed', txt, re.M):
        notrun[m.group(1)] = notrun.get(m.group(1), 0) + int(m.group(2))
    total = re.search(r'^kh: selftest: NOT RUN: (\d+) checks in (\d+) fatal-path cases', txt, re.M)
    hashes = {
        'mod-params hash, range cases': (re.search(r'transcript range cases=\d+ hash=([0-9a-f]{16})', txt) or [None, None])[1],
        'mod-params hash, frame cases': (re.search(r'transcript range cases=\d+ hash=[0-9a-f]{16} frame cases=\d+ hash=([0-9a-f]{16})', txt) or [None, None])[1],
        'audio mix determinism hash': (re.search(r'determinism hash ([0-9a-f]{16})', txt) or [None, None])[1],
    }
    ex = re.search(r'^selftest exit=(-?\d+)', txt, re.M)
    return {'suites': suites, 'order': order, 'skipped': skipped, 'notrun': notrun,
            'notrun_total': (int(total.group(1)), int(total.group(2))) if total else None, 'hashes': hashes,
            'exit': int(ex.group(1)) if ex else None, 'fail_lines': len(re.findall(r'selftest FAIL', txt))}


def main():
    runs = []
    for a in sys.argv[1:]:
        if '=' not in a:
            sys.stderr.write(__doc__); return 2
        label, path = a.split('=', 1); runs.append((label, parse(path)))
    if not runs:
        sys.stderr.write(__doc__); return 2
    order = []
    for _, r in runs:
        for s in r['order']:
            if s not in order:
                order.append(s)
    ref = runs[0][1]
    rows = [['suite'] + [l for l, _ in runs]]
    for s in order:
        row = [s]
        for _, r in runs:
            if s in r['suites']:
                c, f = r['suites'][s]; cell = '%d' % c + (' (%d FAILED)' % f if f else '')
                if s in r['notrun']: cell += ' [+%d not run]' % r['notrun'][s]
            elif s in r['skipped']: cell = 'SKIPPED'
            else: cell = '-'
            row.append(cell)
        rows.append(row)
    rows.append(['total checks / failures'] + ['%d / %d' % (sum(c for c, _ in r['suites'].values()), sum(f for _, f in r['suites'].values())) for _, r in runs])
    rows.append(['not run (KH_IOS_PATHS)'] + ['%d checks in %d cases' % r['notrun_total'] if r['notrun_total'] else '0' for _, r in runs])
    rows.append(['exit status'] + [str(r['exit']) for _, r in runs])
    for h in ('mod-params hash, range cases', 'mod-params hash, frame cases', 'audio mix determinism hash'):
        rows.append([h] + [r['hashes'][h] or '-' for _, r in runs])
    print('| ' + ' | '.join(rows[0]) + ' |'); print('|' + '---|' * len(rows[0]))
    for row in rows[1:]:
        print('| ' + ' | '.join(row) + ' |')
    bad = 0
    for label, r in runs[1:]:
        print('\n%s against %s:' % (label, runs[0][0]))
        same = 0
        for s in order:
            a, b = ref['suites'].get(s), r['suites'].get(s)
            if a and b:
                d = b[0] - a[0]; nr = r['notrun'].get(s, 0) - ref['notrun'].get(s, 0)
                if d == 0 and nr == 0: same += 1
                elif d == -nr: print('  %-20s %+d: exactly the %d checks reported NOT RUN (%d + %d = %d)' % (s, d, nr, b[0], nr, a[0]))
                else: print('  %-20s %+d: UNEXPLAINED (%d not-run checks reported)' % (s, d, nr)); bad += 1
            elif a and not b:
                print('  %-20s %s' % (s, 'SKIPPED here (%s): -%d' % (r['skipped'][s], a[0]) if s in r['skipped'] else 'MISSING here: -%d' % a[0])); bad += 0 if s in r['skipped'] else 1
            elif b and not a:
                print('  %-20s only here: +%d%s' % (s, b[0], ' (SKIPPED in the reference: %s)' % ref['skipped'][s] if s in ref['skipped'] else ''))
        fa = sum(f for _, f in r['suites'].values())
        print('  %d suites with the same count; failures %d, FAIL lines %d, exit %s' % (same, fa, r['fail_lines'], r['exit']))
        for h, v in r['hashes'].items():
            if v != ref['hashes'][h]: print('  %s DIFFERS: %s against %s' % (h, v, ref['hashes'][h])); bad += 1
        if fa or r['fail_lines'] or r['exit'] != 0: bad += 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
