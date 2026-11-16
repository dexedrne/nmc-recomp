#!/usr/bin/env python3
"""import_status.py NM_OUTPUT CENSUS_MD OUT_TSV : join the runtime's symbol table (nm of the linked khnmc: T = real implementation,
W = weak default = named fatal) with docs/platform/IMPORT-CENSUS.md (call sites per import) -> one row per import."""
import sys, re
nm, census, out = sys.argv[1:4]
status = {}
for ln in open(nm):
    p = ln.split()
    if len(p) == 3 and p[2].startswith('rt_imp_'): status[p[2]] = 'implemented' if p[1] in 'TtDd' else 'fatal-by-name'
san = lambda s: re.sub(r'[^0-9A-Za-z]', '_', s)
rows = []; dll = None
for ln in open(census):
    m = re.match(r'^## (\S+\.dll)', ln)
    if m: dll = m.group(1); continue
    m = re.match(r'^\| (\S.*?) \| (\d+) \| (\d+) \| (\S+) \| ([^|]+) \| (.*) \|$', ln)
    if m and dll and m.group(1) != 'function':
        name, sites = m.group(1), int(m.group(2))
        short = san(re.sub(r'\.dll$', '', dll, flags=re.I).lower())
        base = san(name) if not re.match(r'^ord\d+$', name) else name
        sym = 'rt_imp_%s__%s' % (short, base)
        rows.append((dll, name, sites, status.get(sym, 'MISSING(%s)' % sym)))
rows.sort(key=lambda r: -r[2])
with open(out, 'w') as f:
    f.write('dll\timport\tcall_sites\tstatus\n')
    for r in rows: f.write('%s\t%s\t%d\t%s\n' % r)
tot = len(rows); imp = sum(1 for r in rows if r[3] == 'implemented'); fat = [r for r in rows if r[3] == 'fatal-by-name']; miss = [r for r in rows if r[3].startswith('MISSING')]
print('imports in census: %d, implemented: %d (%d sites), fatal-by-name: %d (%d sites), unmatched: %d' % (tot, imp, sum(r[2] for r in rows if r[3] == 'implemented'), len(fat), sum(r[2] for r in fat), len(miss)))
for r in miss[:10]: print('  unmatched', r)
