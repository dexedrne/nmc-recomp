#!/usr/bin/env bash
# tools/float_probe.sh : the float-policy probe of task 20 on both hosts, and the comparison.
#
#   tools/float_probe.sh [OUTDIR [LIBM_ARGS_FILE]]      (default OUTDIR run-F20/float; no replay without the second argument)
#   env: KHNMC_MAC, KHNMC_MAC_DIR as in tools/darwin_build.sh; NANMICRO = path of the lifter's tests/nanmicro.c
#        (default: the repository copy /home/user/projects/khnmc-decomp-edition/tools/khlift/tests/nanmicro.c)
#
# Builds, with the lifter's policy flags and THIS worktree's lifter-rt on both machines,
#   nanmicro     the lifter's own micro-test: NaN propagation, min / max, sqrt, conversions, denormal operands and results, scalar and
#                packed, 10,998 lines "<op> <a> <b> <host_raw> <policy> [<x86_ref>]"; the policy column must be the same on every host
#   float_probe  tools/float_probe.c: rcpps / rsqrtps model over all 2^32 inputs, every CRT math import through its wrapper,
#                gradual underflow on two threads, out-of-range float -> int
# runs them on x86-64 Linux (here) and on arm64 macOS (the Mac mini: one process at a time, niced, inside ~/$KHNMC_MAC_DIR/float),
# and compares.  LIBM_ARGS_FILE = the deduplicated arguments a real route passed to the transcendental CRT math imports
# (KH_LIBM_ARGS=file on a run, see runtime/core.c): float_probe --replay runs exactly those inputs through the wrappers on both
# hosts and the result bits are compared one by one.  Exit 0: everything that the policy promises to be host-independent is equal.  Exit 1: a difference (printed).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HERE/run-F20/float}
ARGS=${2:-}
[ -z "$ARGS" ] || [ -f "$ARGS" ] || { echo "float_probe: missing $ARGS" >&2; exit 2; }
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-f20}
NANMICRO=${NANMICRO:-/home/user/projects/khnmc-decomp-edition/tools/khlift/tests/nanmicro.c}
case "$RDIR" in khnmc-work/build-*) ;; *) echo "float_probe: KHNMC_MAC_DIR must be khnmc-work/build-*" >&2; exit 2;; esac
FLAGS="-O1 -w -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
mkdir -p "$OUT" || exit 2
[ -f "$NANMICRO" ] || { echo "float_probe: missing $NANMICRO" >&2; exit 2; }
cd "$HERE" || exit 2

echo "float_probe: x86-64 Linux"
# shellcheck disable=SC2086
cc $FLAGS -I lifter-rt "$NANMICRO" -lm -o "$OUT/nanmicro.linux" && cc $FLAGS -I lifter-rt tools/float_probe.c lifter-rt/rt_libm.c lifter-rt/khrt_rcp_tab.c -lm -lpthread -o "$OUT/float_probe.linux" || exit 1
nice -n 10 "$OUT/nanmicro.linux" > "$OUT/nanmicro.linux.txt" || exit 1
nice -n 10 "$OUT/float_probe.linux" "$OUT/libm.linux.bin" > "$OUT/float_probe.linux.txt" || exit 1
rm -f "$OUT"/replay.*.bin "$OUT"/replay.*.txt
[ -z "$ARGS" ] || nice -n 10 "$OUT/float_probe.linux" --replay "$ARGS" "$OUT/replay.linux.bin" > "$OUT/replay.linux.txt" || exit 1

echo "float_probe: arm64 macOS ($MAC:~/$RDIR/float)"
"${SSH[@]}" "$MAC" "mkdir -p ~/$RDIR/float/lifter-rt" || exit 1
rsync -a -e "ssh -o BatchMode=yes" lifter-rt/ "$MAC:$RDIR/float/lifter-rt/" || exit 1
rsync -a -e "ssh -o BatchMode=yes" tools/float_probe.c "$NANMICRO" "$MAC:$RDIR/float/" || exit 1
[ -z "$ARGS" ] || rsync -a -z -e "ssh -o BatchMode=yes" "$ARGS" "$MAC:$RDIR/float/libm-args.bin" || exit 1
"${SSH[@]}" "$MAC" "set -e; cd ~/$RDIR/float
free=\$(memory_pressure | tail -1 | sed -n 's/.*percentage: \\([0-9]*\\)%.*/\\1/p'); [ -n \"\$free\" ] && [ \"\$free\" -ge 30 ] || { echo 'float_probe: memory free below 30 %: not starting' >&2; exit 3; }
nice -n 10 clang $FLAGS -I lifter-rt nanmicro.c -o nanmicro
nice -n 10 clang $FLAGS -I lifter-rt float_probe.c lifter-rt/rt_libm.c lifter-rt/khrt_rcp_tab.c -o float_probe
nice -n 10 ./nanmicro > nanmicro.txt
nice -n 10 ./float_probe libm.bin > float_probe.txt
if [ -n '$ARGS' ]; then nice -n 10 ./float_probe --replay libm-args.bin replay.bin > replay.txt; fi" || exit 1
scp -q -o BatchMode=yes "$MAC:$RDIR/float/nanmicro.txt" "$OUT/nanmicro.mac.txt" || exit 1
scp -q -o BatchMode=yes "$MAC:$RDIR/float/float_probe.txt" "$OUT/float_probe.mac.txt" || exit 1
scp -q -o BatchMode=yes "$MAC:$RDIR/float/libm.bin" "$OUT/libm.mac.bin" || exit 1
if [ -n "$ARGS" ]; then scp -q -o BatchMode=yes "$MAC:$RDIR/float/replay.bin" "$OUT/replay.mac.bin" && scp -q -o BatchMode=yes "$MAC:$RDIR/float/replay.txt" "$OUT/replay.mac.txt" && cp "$ARGS" "$OUT/replay.args.bin" || exit 1; fi

python3 - "$OUT" <<'EOF'
import struct, sys, os
out = sys.argv[1]; bad = 0
# ---- nanmicro: the policy column (5th field) on every line ----
lx = open(os.path.join(out, 'nanmicro.linux.txt')).read().splitlines(); la = open(os.path.join(out, 'nanmicro.mac.txt')).read().splitlines()
ops = lambda ls: [l.split() for l in ls if len(l.split()) >= 5 and not l.startswith('#')]
px, pa = ops(lx), ops(la)
ref_mism = sum('REFMISMATCH' in l for l in lx); diff = 0; raw_arm = 0; raw_x86 = 0
if len(px) != len(pa): print('NANMICRO: line counts differ: %d on x86-64, %d on arm64' % (len(px), len(pa))); bad += 1
for a, b in zip(px, pa):
    if a[:3] != b[:3] or a[4] != b[4]:
        diff += 1
        if diff <= 10: print('NANMICRO differs: x86-64 %s | arm64 %s' % (' '.join(a[:6]), ' '.join(b[:5])))
    raw_arm += b[3] != b[4]; raw_x86 += a[3] != a[4]
bad += diff > 0
print('NANMICRO: %d result lines per host; policy column differs on %d; x86-64 policy vs the real SSE instruction: %d mismatches; plain C operator differs from the policy on %d lines on arm64 and %d on x86-64 (what the policy is for)'
      % (len(px), diff, ref_mism, raw_arm, raw_x86))
if ref_mism: bad += 1
for l in lx[-3:]:
    if l.startswith('#') or 'lines' in l: print('NANMICRO x86-64 summary: ' + l)
# ---- float_probe ----
fx = open(os.path.join(out, 'float_probe.linux.txt')).read().splitlines(); fa = open(os.path.join(out, 'float_probe.mac.txt')).read().splitlines()
for l in fx + fa:
    if l.startswith('HOST'): print('PROBE ' + l)
nx = [l for l in fx if not l.startswith('HOST')]; na = [l for l in fa if not l.startswith('HOST')]
names = [l.split()[1] for l in nx if l.startswith('LIBM')]; n = int([l for l in nx if l.startswith('LIBM')][0].split('n=')[1].split()[0])
dl = []
for a, b in zip(nx, na):
    if a != b: dl.append((a, b))
if len(nx) != len(na): print('PROBE: line counts differ'); bad += 1
same = len(nx) - len(dl)
print('PROBE: %d comparable lines, %d equal on both hosts' % (len(nx), same))
for tag in ('RCP', 'DEN', 'CONV'):
    t = [l for l in nx if l.startswith(tag)]; d = [x for x in dl if x[0].startswith(tag)]
    print('PROBE %-4s %d lines, %d differ%s' % (tag, len(t), len(d), '' if d else ' (identical: ' + '; '.join(l.split(None, 1)[1] for l in t[:2]) + (' ...' if len(t) > 2 else '') + ')'))
    for a, b in d: print('   x86-64: %s\n   arm64 : %s' % (a, b)); bad += 1
libm_diff = [x for x in dl if x[0].startswith('LIBM')]
print('PROBE LIBM %d imports x %d inputs: %d imports with identical hashes, %d differ' % (len(names), n, len(names) - len(libm_diff), len(libm_diff)))
if libm_diff:
    bx = open(os.path.join(out, 'libm.linux.bin'), 'rb').read(); ba = open(os.path.join(out, 'libm.mac.bin'), 'rb').read()
    for i, name in enumerate(names):
        vx = struct.unpack('<%dQ' % n, bx[i * n * 8:(i + 1) * n * 8]); va = struct.unpack('<%dQ' % n, ba[i * n * 8:(i + 1) * n * 8])
        d = [(k, x, y) for k, (x, y) in enumerate(zip(vx, va)) if x != y]
        if not d: continue
        single = name.endswith('f') and name not in ('modf',)
        mx = max(abs(x - y) for _, x, y in d) if all((x >> 63) == (y >> 63) for _, x, y in d) else -1
        print('   LIBM %-7s (%s): %d of %d results differ (%.4f %%), largest distance %s ulp; first: input #%d x86-64 %0*x arm64 %0*x'
              % (name, 'single: computed in double, rounded once' if single else 'double: the host libm result', len(d), n, 100.0 * len(d) / n, mx if mx >= 0 else 'sign', d[0][0], 8 if single else 16, d[0][1], 8 if single else 16, d[0][2]))
    print('   (single-precision imports must be equal: POLICY section 7; double imports are the host libm, equal only where both libms round correctly)')
    bad += sum(1 for a, _ in libm_diff if a.split()[1].endswith('f') and a.split()[1] != 'modf')
# ---- replay of a real route's libm inputs ----
rp = os.path.join(out, 'replay.args.bin')
if os.path.exists(rp):
    a = open(rp, 'rb').read(); rx = open(os.path.join(out, 'replay.linux.bin'), 'rb').read(); ra = open(os.path.join(out, 'replay.mac.bin'), 'rb').read()
    nrec = len(a) // 24
    if len(rx) != nrec * 8 or len(ra) != nrec * 8: print('REPLAY: result files have the wrong size'); bad += 1
    else:
        per = {}; dif = {}; first = {}
        for i in range(nrec):
            nm = a[i * 24:i * 24 + 8].rstrip(b'\0').decode(); per[nm] = per.get(nm, 0) + 1
            if rx[i * 8:i * 8 + 8] != ra[i * 8:i * 8 + 8]:
                dif[nm] = dif.get(nm, 0) + 1
                first.setdefault(nm, (a[i * 24 + 8:i * 24 + 24].hex(), rx[i * 8:i * 8 + 8][::-1].hex(), ra[i * 8:i * 8 + 8][::-1].hex()))
        print('REPLAY: %d distinct (import, arguments) taken from real field routes, run through the wrappers on both hosts: %d results differ' % (nrec, sum(dif.values())))
        for nm in sorted(per):
            print('   REPLAY %-7s %7d inputs, %d differ%s' % (nm, per[nm], dif.get(nm, 0), '' if nm not in dif else ' (first: args %s -> x86-64 %s, arm64 %s)' % first[nm]))
        bad += sum(dif.values()) > 0
print('float_probe: %s' % ('ALL host-independent results are equal on x86-64 Linux and arm64 macOS' if not bad else 'DIFFERENCES in results the policy promises to be host-independent (or in the route replay): %d' % bad))
sys.exit(1 if bad else 0)
EOF
