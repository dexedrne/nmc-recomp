#!/usr/bin/env bash
# tools/make_route_subset.sh : the part of the data tree the headless field route touches, as a tree with the same relative paths
# (for a machine that must not get the 60 GB tree: the Mac mini), and the proof on Linux that the route behaves the same on it.
#
#   tools/make_route_subset.sh [-o OUTDIR] [-b BINARY] [-g GAME_FRAMES] [-d "none 0 1 2 3"] [-L "open.log ..."] [-n] [-k] [-c]
#
#   -o  output directory (default ./route-subset; must be empty or absent): OUTDIR/data, OUTDIR/pkgdir, OUTDIR/MANIFEST.tsv
#   -b  binary (default ./khnmc)          -g  game frames per run (default 7000)
#   -d  difficulty settings to cover, one run each (default "none 0 1 2 3": the union of what they touch goes into the subset)
#   -L  reuse these open logs instead of making the logging runs (step 2 is skipped)
#   -n  build only, no verification runs  -k  keep the run directories of passed runs
#   -c  check only: evaluate the runs that are already in ./run-subset against the existing OUTDIR (nothing is run or built)
#
# Steps:
#   1. build tools/openlog_shim.c (LD_PRELOAD: logs every open / opendir / stat with its result, and the byte ranges read from
#      files of 64 MiB or more);
#   2. one route per difficulty setting on the FULL tree under the shim (tools/route_batch.sh, 2 at a time, niced);
#   3. tools/route_subset.py builds OUTDIR from the union of the logs (see its header for the rules);
#   4. verification: the same runs on the SUBSET under the shim.  Passed only when every run is ok AND its game file log
#      (KHNMC_FILELOG: HIT / MISS, source, size, name; sorted) is identical to the file log of a full-tree run AND no path under
#      the two roots has another outcome than in the full-tree logs (an open or stat that succeeded there and fails here, or
#      the reverse, or a path the full-tree runs never touched) AND every byte range read from a large file was copied.
#      "A full-tree run", not "the full-tree run of the same difficulty": the route has two outcomes on a loaded machine (the
#      scripted battle starts, 323 log lines, or does not within the 7,000 frames, 317 lines; the difficulty setting does not
#      change the list), so each subset run is matched to the outcome class it landed in, and the classes are printed.  Counters
#      of all runs are printed side by side; thread timing moves draws / field commands between any two runs: shown, not compared.
# Exit status: 0 built (and verified unless -n), 1 a run failed or the verification found a difference, 2 usage.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/route-subset"; BIN="$HERE/khnmc"; GFR=7000; DIFFS="none 0 1 2 3"; LOGS=""; VERIFY=1; KEEP=0; CHECKONLY=0
while getopts "o:b:g:d:L:nkc" o; do case "$o" in o) OUT=$OPTARG;; b) BIN=$OPTARG;; g) GFR=$OPTARG;; d) DIFFS=$OPTARG;; L) LOGS=$OPTARG;; n) VERIFY=0;; k) KEEP=1;; c) CHECKONLY=1;;
  *) sed -n '2,25p' "$0" >&2; exit 2;; esac; done
DATA="$HERE/data-full"; PKG="$HERE/pkgdir"; SHIM="$HERE/obj-tools/openlog_shim.so"; WORK="$HERE/run-subset"
for f in "$BIN" "$DATA" "$PKG" "$HERE/tools/openlog_shim.c" "$HERE/tools/route_subset.py" "$HERE/tools/route_batch.sh"; do [ -e "$f" ] || { echo "make_route_subset: missing $f" >&2; exit 2; }; done
if [ "$CHECKONLY" = 0 ]; then
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "make_route_subset: $OUT exists and is not empty" >&2; exit 2; fi
mkdir -p "$HERE/obj-tools" "$WORK" || exit 2
cc -shared -fPIC -O1 -o "$SHIM" "$HERE/tools/openlog_shim.c" -ldl || { echo "make_route_subset: cannot build the shim" >&2; exit 1; }
fi

# runs: $1 = tag, further arguments = extra route_batch options.  One run per difficulty setting, 2 in parallel; per-run shim log
# = <run dir>/open.log (the shim opens the relative name from the run's working directory <run dir>/root).
run_set(){ local tag=$1; shift; local pids=() d fail=0 n=0
  for d in $DIFFS; do
    ( "$HERE/tools/route_batch.sh" -n 1 -j 1 -g "$GFR" -s 600 -d "$d" -k -b "$BIN" -o "$WORK/$tag-$d" \
        -w "nice -n 10 env LD_PRELOAD=$SHIM KH_OPENLOG=../open.log" "$@" > "$WORK/$tag-$d.log" 2>&1 ) &
    pids+=($!); n=$((n+1)); sleep 3
    if [ $((n%2)) -eq 0 ]; then for p in "${pids[@]}"; do wait "$p" || fail=1; done; pids=(); fi
  done
  for p in "${pids[@]:-}"; do [ -n "$p" ] && { wait "$p" || fail=1; }; done
  for d in $DIFFS; do sed -n '3,4p' "$WORK/$tag-$d.log" | cut -c1-200; done
  return $fail; }

if [ "$CHECKONLY" = 1 ]; then
  [ -n "$LOGS" ] || for d in $DIFFS; do LOGS="$LOGS $WORK/full-$d/r01/open.log"; done
  for d in $DIFFS; do [ -f "$WORK/subset-$d/r01/open.log" ] || { echo "make_route_subset: -c: no run $WORK/subset-$d" >&2; exit 2; }; done
else
if [ -z "$LOGS" ]; then
  echo "make_route_subset: step 2: logging runs on the full tree ($DIFFS)"
  run_set full || { echo "make_route_subset: a logging run on the full tree failed (see $WORK/full-*.log)" >&2; exit 1; }
  for d in $DIFFS; do LOGS="$LOGS $WORK/full-$d/r01/open.log"; done
fi
# shellcheck disable=SC2086
python3 "$HERE/tools/route_subset.py" --data "$DATA" --pkgdir "$PKG" --out "$OUT" $LOGS || exit 1
du -sh "$OUT" | sed 's/^/make_route_subset: on disk: /'
[ "$VERIFY" = 1 ] || { echo "make_route_subset: built, not verified (-n)"; exit 0; }

echo "make_route_subset: step 4: the same runs on the subset"
run_set subset -e "KHNMC_DATA=$OUT/data" -e "KHNMC_PKGDIR=$OUT/pkgdir" || { echo "make_route_subset: a run on the subset failed (see $WORK/subset-*.log)" >&2; exit 1; }
fi
# shellcheck disable=SC2086
python3 - "$WORK" "$DATA" "$PKG" "$OUT" "$DIFFS" $LOGS <<'EOF'
import os, sys
work, data, pkg, out, diffs = sys.argv[1:6]; full_logs = sys.argv[6:]
def rel(path, roots):
    p = os.path.normpath(path)
    for name, root in roots:
        if p == root: return name + '/'
        if p.startswith(root + '/'): return name + '/' + p[len(root) + 1:]
    return None
def outcomes(logs, roots):
    res, rng = {}, {}
    for lg in logs:
        for ln in open(lg, errors='surrogateescape'):
            ln = ln.rstrip('\n')
            if ln.startswith('range '):
                _, span, _ok, path = ln.split(' ', 3); r = rel(path, roots)
                if r: o, n = span.split('+'); rng.setdefault(r, set()).add((int(o), int(n)))
                continue
            p = ln.split(' ', 2)
            if len(p) < 3: continue
            r = rel(p[2], roots)
            if r and p[0] != 'whole': res.setdefault((p[0], r), set()).add(p[1])
    return res, rng
full, frng = outcomes(full_logs, [('data', os.path.normpath(data)), ('pkgdir', os.path.normpath(pkg))])
bad = 0
kept = {}
for ln in open(os.path.join(out, 'MANIFEST.tsv')).read().splitlines()[1:]:
    f = ln.split('\t')
    if f[1] == 'ranges': kept[f[0]] = [tuple(int(x) for x in s.split('-')) for s in f[4].split()]
import glob, hashlib
def flog(path): return sorted(open(path, errors='replace').read().splitlines())
classes = {}                                                    # sorted game file log of a full-tree run -> the runs that produced it
for f in sorted(glob.glob(os.path.join(work, 'full-*', 'r01', 'files.log'))):
    classes.setdefault(tuple(flog(f)), []).append(os.path.basename(os.path.dirname(os.path.dirname(f))))
for k, v in classes.items(): print('VERIFY full-tree outcome class: %d file-log lines (list sha256 %s): %s' % (len(k), hashlib.sha256('\n'.join(k).encode()).hexdigest()[:12], ' '.join(v)))
for d in diffs.split():
    sub_log = os.path.join(work, 'subset-%s' % d, 'r01', 'open.log')
    sub, srng = outcomes([sub_log], [('data', os.path.join(out, 'data')), ('pkgdir', os.path.join(out, 'pkgdir'))])
    for key, r in sorted(sub.items()):
        if key not in full: print('VERIFY difficulty %s: %s %s happened on the subset (%s) but never on the full tree' % (d, key[0], key[1], ','.join(sorted(r)))); bad += 1
        elif not r <= full[key]: print('VERIFY difficulty %s: %s %s: outcome %s on the subset, %s on the full tree' % (d, key[0], key[1], ','.join(sorted(r)), ','.join(sorted(full[key])))); bad += 1
    for path, spans in sorted(srng.items()):
        for o, n in sorted(spans):
            if not any(lo <= o and o + n <= hi for lo, hi in kept.get(path, [])): print('VERIFY difficulty %s: bytes %d+%d of %s were read but are not in the subset' % (d, o, n, path)); bad += 1
    fl = tuple(flog(os.path.join(work, 'subset-%s' % d, 'r01', 'files.log')))
    if not classes: print('VERIFY difficulty %s: no full-tree file log to compare with (logs were given with -L): file logs not compared' % d)
    elif fl not in classes:
        near = min(classes, key=lambda k: len(set(k) ^ set(fl)))
        print('VERIFY difficulty %s: the game file log of the subset run (%d lines) equals NO full-tree run; against the nearest (%d lines):' % (d, len(fl), len(near))); bad += 1
        for x in sorted(set(near) ^ set(fl))[:8]: print('    ' + x)
    else: print('VERIFY difficulty %s: game file log identical to the full-tree run(s) %s (%d lines), %d host paths with the same outcome as on the full tree' % (d, ' '.join(classes[fl]), len(fl), len(sub)))
print('make_route_subset: verification %s' % ('PASSED' if not bad else 'FAILED: %d differences' % bad))
sys.exit(1 if bad else 0)
EOF
rc=$?
echo "make_route_subset: counters (full tree | subset), per difficulty setting:"
for d in $DIFFS; do for t in full subset; do [ -f "$WORK/$t-$d/summary.tsv" ] && printf '%-7s %-5s %s\n' "$t" "$d" "$(sed -n 2p "$WORK/$t-$d/summary.tsv" | cut -f2-15 | tr '\t' ' ')"; done; done
if [ $rc -eq 0 ] && [ "$KEEP" = 0 ]; then for d in $DIFFS; do rm -rf "$WORK/subset-$d/r01/root" "$WORK/subset-$d/r01/docs" "$WORK/full-$d/r01/root" "$WORK/full-$d/r01/docs" "$WORK"/*/khnmc.bin; done; fi
exit $rc
