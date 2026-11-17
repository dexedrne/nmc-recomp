#!/usr/bin/env python3
"""tools/ios/test_mac_rules.py [IOS_DIR] : checks, by READING them, that the UNRUN MacBook Air scripts encode that Mac's rules
(TASK26-original-macbook.md).  Nothing in tools/ios/ is executed; IOS_DIR defaults to the directory of this file.

  1. memory gate: `mem_gate` ends the script with `exit 3`, which inside a pipeline (`mem_gate X | tee`) ends only the
     pipeline's subshell.  Every piped call must check `${PIPESTATUS[0]}` and exit on the same line.
  2. mac/build.sh takes a fresh memory reading before EVERY heavy step (each `make -j$JOBS ...` and `xcodebuild`): a mem_gate
     call between the previous heavy step and this one.
  3. mac/sim_run.sh mutes the Mac before any simulator is created or booted, stops when muting fails, and reads the mute state
     back (`output muted of (get volume settings)` must be `true`) before `simctl create` / `simctl boot`.
  4. every xcodebuild call keeps Xcode's derived data and module cache inside ~/Developer/KHNMC (`-derivedDataPath "$KROOT/...`
     and `MODULE_CACHE_DIR="$KROOT/...`); mac/cleanup.sh removes that derived-data folder with the other intermediates.
Exit 0 when every check holds, 1 otherwise (each failure named)."""
import os, re, sys

D = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
fails, n = [], 0


def check(ok, what):
    global n
    n += 1
    if not ok:
        fails.append(what)


def lines(rel):
    """the script's logical lines (backslash continuations joined), comments and blank lines dropped: (first line no, text)"""
    out, buf, start = [], '', None
    for i, raw in enumerate(open(os.path.join(D, rel)).read().split('\n'), 1):
        if start is None:
            if raw.lstrip().startswith('#') or not raw.strip():
                continue
            start = i
        if raw.endswith('\\'):
            buf += raw[:-1] + ' '
            continue
        out.append((start, buf + raw))
        buf, start = '', None
    return out


macs = sorted(f for f in os.listdir(os.path.join(D, 'mac')) if f.endswith('.sh'))
check(bool(macs), 'no tools/ios/mac/*.sh found in %s' % D)

# 1. piped memory gates
for f in macs:
    for no, t in lines('mac/' + f):
        if re.search(r'(^|[;&|{(]\s*)mem_gate\s+"', t) and re.search(r'mem_gate\s+"[^"]*"[^;]*\|', t):
            check('PIPESTATUS[0]' in t and re.search(r'\|\|\s*\{?.*?\bexit\s+[1-9]', t) is not None,
                  'mac/%s:%d: mem_gate is piped and its exit status is not checked (exit 3 ends only the subshell): %s'
                  % (f, no, t.strip()[:120]))

# 2. a gate before every heavy step of build.sh
heavy_seen, gate_since = 0, False
for no, t in lines('mac/build.sh'):
    if re.search(r'(^|[;&|{(]\s*)mem_gate\s+"', t):
        gate_since = True
    if re.search(r'-j\$JOBS|\bxcodebuild\b', t) and not t.lstrip().startswith('mem_gate'):
        heavy_seen += 1
        check(gate_since, 'mac/build.sh:%d: heavy step without a fresh memory reading before it: %s' % (no, t.strip()[:100]))
        gate_since = False
check(heavy_seen >= 3, 'mac/build.sh: expected at least 3 heavy steps (shards, applib, xcodebuild), found %d' % heavy_seen)

# 3. mute first, fail closed, read back
sim = lines('mac/sim_run.sh')
idx = lambda pat: next((k for k, (no, t) in enumerate(sim) if re.search(pat, t)), None)
k_mute, k_read = idx(r"set volume with output muted"), idx(r"output muted of \(get volume settings\)")
k_create, k_boot = idx(r"simctl create\b"), idx(r"simctl boot\b")
check(None not in (k_mute, k_create, k_boot), 'mac/sim_run.sh: mute, simctl create or simctl boot not found')
if None not in (k_mute, k_create, k_boot):
    check(k_mute < k_create < k_boot, 'mac/sim_run.sh: the mute does not come before simctl create / boot')
    t = sim[k_mute][1]
    check(re.search(r"\|\|\s*\{.*?\bexit\s+[1-9]", t) is not None,
          'mac/sim_run.sh:%d: a failed mute does not stop the script: %s' % (sim[k_mute][0], t.strip()[:100]))
    check(k_read is not None and k_mute <= k_read < k_create,
          'mac/sim_run.sh: the mute state is not read back between the mute and simctl create')
    if k_read is not None and k_read < k_create:
        between = ' '.join(t for _, t in sim[k_read:k_create])
        check(re.search(r'=\s*"?true"?\s*\]\s*\|\|\s*\{.*?\bexit\s+[1-9]', between) is not None,
              'mac/sim_run.sh: the read-back mute state is not required to be "true" (exit otherwise) before simctl create')

# 4. xcodebuild stays inside ~/Developer/KHNMC
xc = 0
for f in sorted(os.listdir(D)) + ['mac/' + m for m in macs]:
    if not f.endswith('.sh'):
        continue
    for no, t in lines(f):
        if re.search(r'(^|[;&|{(]\s*|nice -n \S+\s+)xcodebuild\s', t):
            xc += 1
            check(re.search(r'-derivedDataPath\s+"\$KROOT/', t) is not None,
                  '%s:%d: xcodebuild without -derivedDataPath inside $KROOT (Xcode writes ~/Library/Developer/Xcode/DerivedData)' % (f, no))
            check(re.search(r'MODULE_CACHE_DIR="\$KROOT/', t) is not None,
                  '%s:%d: xcodebuild without MODULE_CACHE_DIR inside $KROOT (the shared ModuleCache.noindex)' % (f, no))
check(xc >= 1, 'no xcodebuild call found (expected one in mac/build.sh)')
dd = re.search(r'-derivedDataPath\s+"\$KROOT/([^"]+)"', ' '.join(t for _, t in lines('mac/build.sh')))
if dd:
    cl = ' '.join(t for _, t in lines('mac/cleanup.sh'))
    check(re.search(r'objects\)[^;]*rm -rf[^;]*"\$KROOT/%s"' % re.escape(dd.group(1)), cl) is not None,
          'mac/cleanup.sh: `objects` does not remove the derived-data folder $KROOT/%s' % dd.group(1))

for f in fails:
    print('test_mac_rules: FAIL: ' + f)
print('test_mac_rules: %d checks, %d failed (%s)' % (n, len(fails), D))
sys.exit(1 if fails else 0)
