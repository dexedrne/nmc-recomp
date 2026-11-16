#!/usr/bin/env python3
"""usage: path.py NAME [orig|nmc]  -> prints the extracted (decompressed) file path for a CHANGED game file name (e.g. obj/P_EX100.mset).
Also accepts a raw key like 9c3835f1.0000 for the 295 NMC-added files. orig only exists for MODIFIED files."""
import sys,json,os
D=os.path.dirname(os.path.abspath(__file__)); rows=json.load(open(f'{D}/idx/changed_table.json'))
n=sys.argv[1]; w=sys.argv[2] if len(sys.argv)>2 else 'nmc'
for r in rows:
    if n in (r['name'],r['key'],r.get('name_ps2')):
        if w=='orig' and r['kind']=='new': sys.exit('new file: no orig version')
        print(f"{D}/extract/{w}/{r['key']}.bin"); break
else: sys.exit('not a changed file (identical in both ISOs, or unknown)')
