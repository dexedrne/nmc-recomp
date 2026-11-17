#!/usr/bin/env bash
# tools/darwin_route.sh : selftest and headless field routes of the arm64 macOS build on the Mac mini, driven from this worktree.
#
#   tools/darwin_route.sh sync [SUBSET_DIR]     copy the route data subset (tools/make_route_subset.sh; default ./route-subset) to
#                                               ~/$KHNMC_MAC_DIR/route-data and pc/kh2fm.exe to ~/$KHNMC_MAC_DIR/pc (rsync,
#                                               checksummed).  Files that an earlier task left in ~/khnmc-work/route-data are
#                                               reused by local copy when identical (--copy-dest); that directory is only read
#   tools/darwin_route.sh selftest [TAG]        ./khnmc --selftest there; output fetched to $RUN/mac-selftest-<TAG>.out
#   tools/darwin_route.sh run TAG [route_one.py options, e.g. --difficulty 2 --env KH_IMPORT_COUNTS=1]
#                                               ONE route (tools/route_one.py: the environment of route_batch.sh, niced, watchdog,
#                                               refused below the memory threshold); run.out / phase.log / files.log / summary.*
#                                               fetched to $RUN/mac-<TAG>/
#   tools/darwin_route.sh ios-selftest [TAG]    the KH_IOS_PATHS build (tools/darwin_build.sh with MAKE_ARGS=IOS_PATHS=1) started
#                                               the way an app is (tools/ios_paths_selftest.sh); $RUN/mac-iospaths-selftest-<TAG>.out
#   tools/darwin_route.sh ios-run TAG [...]     ONE route of that build with route_one.py --ios-paths; $RUN/mac-<TAG>/
#   tools/darwin_route.sh wavsum TAG [NAME]     size, format and sha256 of run/<TAG>/<NAME> (default audio.wav) there: the whole
#                                               file and the sample data alone (tools/wav_compare.py sum)
#   tools/darwin_route.sh fetch TAG NAME        copy run/<TAG>/<NAME> to $RUN/mac-<TAG>/<NAME>
#   tools/darwin_route.sh probe [TOUCH_MIB]     build and run the device probe's measuring part there as a plain macOS program
#                                               (tools/ios/probe/probe_host.c + probe_core.c; TOUCH_MIB default 64, at most 256
#                                               on this shared machine): a test of the PROGRAM on arm64 Darwin, not an answer
#                                               about an iPhone.  Result: $RUN/probe/mac-probe.txt
#   env: KHNMC_MAC, KHNMC_MAC_DIR (default khnmc-work/build-j26) as in tools/darwin_build.sh; KHNMC_RUN_DIR = where results go
#        here (default run-J26); KHNMC_MAC_NICE (default 10) and KHNMC_MAC_MIN_FREE (default 30: `memory_pressure` free
#        percentage below which nothing is started; the reading is printed before every run)
# Everything on the Mac stays inside ~/$KHNMC_MAC_DIR (build, runs, the data subset in route-data/, the exe in pc/; the
# selftests' scratch files in run/tmp through KH_TMPDIR, not in /tmp).  One run at a time:
# the script refuses to start while another khnmc of this user is running there.  After a route its own state directories
# (run/<TAG>/root, docs, home) are removed there; nothing else is deleted.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-j26}
RUN=${KHNMC_RUN_DIR:-$HERE/run-J26}
NICE=${KHNMC_MAC_NICE:-10}; MINFREE=${KHNMC_MAC_MIN_FREE:-30}
case "$RDIR" in khnmc-work/build-*) ;; *) echo "darwin_route: KHNMC_MAC_DIR must be khnmc-work/build-*" >&2; exit 2;; esac
case "$RDIR" in *..*|*" "*) echo "darwin_route: bad KHNMC_MAC_DIR" >&2; exit 2;; esac
for v in "$NICE" "$MINFREE"; do case "$v" in ''|*[!0-9]*) echo "darwin_route: KHNMC_MAC_NICE / KHNMC_MAC_MIN_FREE must be numbers" >&2; exit 2;; esac; done
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=10)
cmd=${1:-}; [ $# -gt 0 ] && shift
busy(){ "${SSH[@]}" "$MAC" 'pgrep -u "$(id -u)" -x khnmc.bin >/dev/null || pgrep -u "$(id -u)" -x khnmc >/dev/null || pgrep -u "$(id -u)" -x khnmc-iospaths >/dev/null' && { echo "darwin_route: a khnmc process is already running on the Mac mini: one run at a time" >&2; return 0; }; return 1; }
# the scripts that run there (route_one.py, ios_paths_selftest.sh, wav_compare.py) are this worktree's: same rsync as darwin_build.sh
push_tools(){ rsync -a --delete --exclude '__pycache__' -e "ssh -o BatchMode=yes -o ConnectTimeout=20" "$HERE/tools/" "$MAC:$RDIR/tools/" || { echo "darwin_route: cannot copy tools/ to the Mac mini" >&2; exit 1; }; }
tagok(){ case "$1" in ''|*[!A-Za-z0-9._-]*) echo "darwin_route: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac; }
# remote shell text: read memory_pressure, print it, refuse below the threshold (exit 3)
GATE="free=\$(memory_pressure | tail -1 | sed -n 's/.*percentage: \\([0-9]*\\)%.*/\\1/p'); echo \"darwin_route: memory free \${free:-?}% (start at >= $MINFREE%), load \$(sysctl -n vm.loadavg)\"; { [ -n \"\$free\" ] && [ \"\$free\" -ge $MINFREE ]; } || { echo \"darwin_route: memory free \${free:-?}% is below $MINFREE%: not starting\" >&2; exit 3; }"
route(){ # $1 = TAG, $2 = binary there, $3 = extra route_one option or "", rest = the caller's options
  local TAG=$1 BIN=$2 EXTRA=$3; shift 3
  busy && exit 3
  push_tools
  local args="" a; for a in "$@"; do case "$a" in *\'*) echo "darwin_route: an option with a single quote cannot be passed" >&2; exit 2;; esac; args="$args '$a'"; done
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && python3 tools/route_one.py --bin $BIN --exe pc/kh2fm.exe --data route-data/data --pkgdir route-data/pkgdir --out run/$TAG --label $TAG --nice $NICE --min-free-pct $MINFREE $EXTRA $args"; local rc=$?
  mkdir -p "$RUN/mac-$TAG"
  rsync -a -z -e "ssh -o BatchMode=yes" --include 'run.out' --include 'launch.out' --include 'phase.log' --include 'files.log' --include 'exit' --include 'env' --include 'summary.tsv' --include 'summary.json' --exclude '*' "$MAC:$RDIR/run/$TAG/" "$RUN/mac-$TAG/" || exit 1
  "${SSH[@]}" "$MAC" "rm -rf ~/$RDIR/run/$TAG/root ~/$RDIR/run/$TAG/docs ~/$RDIR/run/$TAG/home" || echo "darwin_route: NOTE: the run's state directories could not be removed on the Mac mini (run/$TAG/root, docs, home)" >&2
  return $rc
}
case "$cmd" in
sync)
  SUB=${1:-$HERE/route-subset}
  [ -d "$SUB/data" ] && [ -d "$SUB/pkgdir" ] && [ -f "$SUB/MANIFEST.tsv" ] || { echo "darwin_route: $SUB is not a route subset (data/, pkgdir/, MANIFEST.tsv)" >&2; exit 2; }
  "${SSH[@]}" "$MAC" "mkdir -p ~/$RDIR/route-data ~/$RDIR/pc" || exit 1
  rsync -a -L -z -e "ssh -o BatchMode=yes" "$HERE/pc/kh2fm.exe" "$MAC:$RDIR/pc/kh2fm.exe" || exit 1
  # --copy-dest is relative to the destination: ~/$RDIR/route-data/../../route-data = ~/khnmc-work/route-data (read only)
  rsync -a -z --delete --copy-dest=../../route-data -e "ssh -o BatchMode=yes" "$SUB/" "$MAC:$RDIR/route-data/" || {
    echo "darwin_route: rsync with --copy-dest failed (an rsync there that does not know the option?): plain copy" >&2
    rsync -a -z --delete -e "ssh -o BatchMode=yes" "$SUB/" "$MAC:$RDIR/route-data/" || exit 1; }
  L=$(cd "$SUB" && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done | sha256sum | cut -d' ' -f1)
  R=$("${SSH[@]}" "$MAC" "cd ~/$RDIR/route-data"' && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf "%s %s\n" "$(shasum -a 256 "$f" | cut -d" " -f1)" "$f"; done | shasum -a 256 | cut -d" " -f1')
  n=$(cd "$SUB" && find data pkgdir -type f | wc -l)
  [ "$L" = "$R" ] || { echo "darwin_route: the copy differs from the subset (content hash list here ${L:0:16}, there ${R:0:16})" >&2; exit 1; }
  "${SSH[@]}" "$MAC" "cd ~/$RDIR"' && echo "darwin_route: on the Mac mini: route-data $(du -sh route-data | cut -f1) on disk, exe sha256 $(shasum -a 256 pc/kh2fm.exe | cut -c1-16)"'
  echo "darwin_route: $n files, content hash list ${L:0:16} equal on both sides"
  ;;
selftest)
  TAG=${1:-run}; tagok "$TAG"; busy && exit 3
  mkdir -p "$RUN"; push_tools
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && mkdir -p run/tmp && $GATE && rm -rf run/docs-selftest && (KH_EXE=\$PWD/pc/kh2fm.exe KH_DOCS=\$PWD/run/docs-selftest KH_TMPDIR=\$PWD/run/tmp nice -n $NICE ./khnmc --selftest > run/selftest-$TAG.out 2>&1; echo \"selftest exit=\$?\" | tee -a run/selftest-$TAG.out; echo \"darwin_route: left in run/tmp: \$(ls -A run/tmp | tr '\\n' ' ')\")" || exit 1
  scp -q -o BatchMode=yes "$MAC:$RDIR/run/selftest-$TAG.out" "$RUN/mac-selftest-$TAG.out" || exit 1
  grep -a "SKIPPED\|FAIL\|transcript\|guest space\|selftest exit" "$RUN/mac-selftest-$TAG.out" | cut -c1-220
  grep -aq "^selftest exit=0" "$RUN/mac-selftest-$TAG.out"
  ;;
ios-selftest)
  TAG=${1:-run}; tagok "$TAG"; busy && exit 3
  IBIN=${KHNMC_IOS_BIN:-./khnmc-iospaths}; case "$IBIN" in ./khnmc-iospaths|./khnmc-iospaths-shell) ;; *) echo "darwin_route: KHNMC_IOS_BIN must be ./khnmc-iospaths or ./khnmc-iospaths-shell" >&2; exit 2;; esac
  push_tools
  mkdir -p "$RUN"
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && mkdir -p run && $GATE && bash tools/ios_paths_selftest.sh $IBIN pc/kh2fm.exe run/iospaths-selftest-$TAG $NICE"; rc=$?
  scp -q -o BatchMode=yes "$MAC:$RDIR/run/iospaths-selftest-$TAG/selftest.out" "$RUN/mac-iospaths-selftest-$TAG.out" || exit 1
  grep -a "SKIPPED\|FAIL\|transcript\|guest space\|selftest exit" "$RUN/mac-iospaths-selftest-$TAG.out" | cut -c1-220
  exit $rc
  ;;
run)
  TAG=${1:-}; tagok "$TAG"; shift
  route "$TAG" ./khnmc "" "$@"; exit $?
  ;;
ios-run)
  TAG=${1:-}; tagok "$TAG"; shift
  route "$TAG" ./khnmc-iospaths --ios-paths "$@"; exit $?
  ;;
wavsum)
  TAG=${1:-}; tagok "$TAG"; NAME=${2:-audio.wav}; case "$NAME" in *[!A-Za-z0-9._-]*) echo "darwin_route: bad file name" >&2; exit 2;; esac
  push_tools
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && python3 tools/wav_compare.py sum run/$TAG/$NAME"
  ;;
fetch)
  TAG=${1:-}; tagok "$TAG"; NAME=${2:-}; case "$NAME" in ''|*[!A-Za-z0-9._-]*) echo "darwin_route: fetch TAG NAME" >&2; exit 2;; esac
  mkdir -p "$RUN/mac-$TAG"; rsync -a -z -e "ssh -o BatchMode=yes" "$MAC:$RDIR/run/$TAG/$NAME" "$RUN/mac-$TAG/$NAME"
  ;;
probe)
  TOUCH=${1:-64}; case "$TOUCH" in ''|*[!0-9]*) echo "darwin_route: probe TOUCH_MIB" >&2; exit 2;; esac
  [ "$TOUCH" -le 256 ] || { echo "darwin_route: probe: at most 256 MiB on the Mac mini" >&2; exit 2; }
  busy && exit 3
  push_tools; mkdir -p "$RUN/probe"
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && mkdir -p run/probe && $GATE && nice -n $NICE clang -O1 -Wall -Wextra -o run/probe/probe_host tools/ios/probe/probe_host.c tools/ios/probe/probe_core.c && nice -n $NICE run/probe/probe_host run/probe/mac-probe.txt $TOUCH 2>/dev/null; echo \"darwin_route: probe exit \$?\"" || exit 1
  scp -q -o BatchMode=yes "$MAC:$RDIR/run/probe/mac-probe.txt" "$RUN/probe/mac-probe.txt" || exit 1
  cat "$RUN/probe/mac-probe.txt"
  ;;
*) sed -n '2,28p' "$0" >&2; exit 2;;
esac
