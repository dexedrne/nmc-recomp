#!/usr/bin/env python3
"""ccwrap.py LOG OUT CMD...   run a compile, append {out, rc, sec, maxrss_kb, stderr head} to LOG (one JSON line), return its rc."""
import sys, os, json, time, subprocess, resource, platform
log, out = sys.argv[1], sys.argv[2]; cmd = sys.argv[3:]
t0 = time.time()
p = subprocess.run(cmd, capture_output=True, text=True)
el = time.time() - t0
ru = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
if platform.system() == 'Darwin': ru //= 1024
row = dict(out=os.path.basename(out), rc=p.returncode, sec=round(el, 2), maxrss_kb=ru, err=(p.stderr or '')[:1500], host=platform.node(), arch=platform.machine())
with open(log, 'a') as f: f.write(json.dumps(row) + '\n')
sys.stderr.write(p.stderr[:3000]); sys.exit(p.returncode)
