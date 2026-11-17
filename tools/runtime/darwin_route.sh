#!/usr/bin/env bash
# tools/darwin_route.sh : selftest and headless field routes of the arm64 macOS build on the Mac mini, driven from this worktree.
#
#   tools/darwin_route.sh sync [SUBSET_DIR]     copy the route data subset (tools/make_route_subset.sh; default ./route-subset) to
#                                               ~/khnmc-work/route-data and pc/kh2fm.exe to ~/khnmc-work/pc (rsync, checksummed)
#   tools/darwin_route.sh selftest [TAG]        ./khnmc --selftest there; output fetched to run-F20/mac-selftest-<TAG>.out
#   tools/darwin_route.sh run TAG [route_one.py options, e.g. --difficulty 2 --env KH_IMPORT_COUNTS=1]
#                                               ONE route (tools/route_one.py: the environment of route_batch.sh, niced, watchdog,
#                                               refused below 30 % free memory); run.out / phase.log / files.log / summary.* fetched to
#                                               run-F20/mac-<TAG>/
#   env: KHNMC_MAC, KHNMC_MAC_DIR as in tools/darwin_build.sh
# Everything on the Mac stays inside ~/khnmc-work (build and runs in ~/$KHNMC_MAC_DIR, data in ~/khnmc-work/route-data, the exe in
# ~/khnmc-work/pc).  One run at a time: the script refuses to start while another khnmc of this user is running there.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-f20}
case "$RDIR" in khnmc-work/build-*) ;; *) echo "darwin_route: KHNMC_MAC_DIR must be khnmc-work/build-*" >&2; exit 2;; esac
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
cmd=${1:-}; [ $# -gt 0 ] && shift
busy(){ "${SSH[@]}" "$MAC" 'pgrep -u "$(id -u)" -x khnmc.bin >/dev/null || pgrep -u "$(id -u)" -x khnmc >/dev/null' && { echo "darwin_route: a khnmc process is already running on the Mac mini: one run at a time" >&2; return 0; }; return 1; }
case "$cmd" in
sync)
  SUB=${1:-$HERE/route-subset}
  [ -d "$SUB/data" ] && [ -d "$SUB/pkgdir" ] && [ -f "$SUB/MANIFEST.tsv" ] || { echo "darwin_route: $SUB is not a route subset (data/, pkgdir/, MANIFEST.tsv)" >&2; exit 2; }
  "${SSH[@]}" "$MAC" 'mkdir -p ~/khnmc-work/route-data ~/khnmc-work/pc' || exit 1
  rsync -a -L -z -e "ssh -o BatchMode=yes" "$HERE/pc/kh2fm.exe" "$MAC:khnmc-work/pc/kh2fm.exe" || exit 1
  rsync -a -z --delete -e "ssh -o BatchMode=yes" "$SUB/" "$MAC:khnmc-work/route-data/" || exit 1
  L=$(cd "$SUB" && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done | sha256sum | cut -d' ' -f1)
  R=$("${SSH[@]}" "$MAC" 'cd ~/khnmc-work/route-data && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf "%s %s\n" "$(shasum -a 256 "$f" | cut -d" " -f1)" "$f"; done | shasum -a 256 | cut -d" " -f1')
  n=$(cd "$SUB" && find data pkgdir -type f | wc -l)
  [ "$L" = "$R" ] || { echo "darwin_route: the copy differs from the subset (content hash list here ${L:0:16}, there ${R:0:16})" >&2; exit 1; }
  "${SSH[@]}" "$MAC" 'cd ~/khnmc-work && echo "darwin_route: on the Mac mini: route-data $(du -sh route-data | cut -f1) on disk, exe sha256 $(shasum -a 256 pc/kh2fm.exe | cut -c1-16)"'
  echo "darwin_route: $n files, content hash list ${L:0:16} equal on both sides"
  ;;
selftest)
  TAG=${1:-run}; busy && exit 3
  mkdir -p "$HERE/run-F20"
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && mkdir -p run && free=\$(memory_pressure | tail -1 | sed -n 's/.*percentage: \\([0-9]*\\)%.*/\\1/p') && { [ -n \"\$free\" ] && [ \"\$free\" -ge 30 ] || { echo \"darwin_route: memory free \${free:-?}% is below 30 %: not starting\" >&2; exit 3; }; } && rm -rf run/docs-selftest && (KH_EXE=\$HOME/khnmc-work/pc/kh2fm.exe KH_DOCS=\$PWD/run/docs-selftest nice -n 10 ./khnmc --selftest > run/selftest-$TAG.out 2>&1; echo \"selftest exit=\$?\" | tee -a run/selftest-$TAG.out)" || exit 1
  scp -q -o BatchMode=yes "$MAC:$RDIR/run/selftest-$TAG.out" "$HERE/run-F20/mac-selftest-$TAG.out" || exit 1
  grep -a "selftest.*checks\|SKIPPED\|FAIL\|transcript\|guest space\|selftest exit" "$HERE/run-F20/mac-selftest-$TAG.out" | cut -c1-220
  grep -aq "^selftest exit=0" "$HERE/run-F20/mac-selftest-$TAG.out"
  ;;
run)
  TAG=${1:-}; [ -n "$TAG" ] || { echo "darwin_route: run needs a TAG" >&2; exit 2; }; shift
  case "$TAG" in *[!A-Za-z0-9._-]*) echo "darwin_route: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac
  busy && exit 3
  args=""; for a in "$@"; do args="$args '$a'"; done
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && python3 tools/route_one.py --bin ./khnmc --exe \$HOME/khnmc-work/pc/kh2fm.exe --data \$HOME/khnmc-work/route-data/data --pkgdir \$HOME/khnmc-work/route-data/pkgdir --out run/$TAG --label $TAG $args"; rc=$?
  mkdir -p "$HERE/run-F20/mac-$TAG"
  rsync -a -z -e "ssh -o BatchMode=yes" --include 'run.out' --include 'phase.log' --include 'files.log' --include 'exit' --include 'env' --include 'summary.tsv' --include 'summary.json' --exclude '*' "$MAC:$RDIR/run/$TAG/" "$HERE/run-F20/mac-$TAG/" || exit 1
  "${SSH[@]}" "$MAC" "rm -rf ~/$RDIR/run/$TAG/root ~/$RDIR/run/$TAG/docs"            # the run's own state directories: scratch
  exit $rc
  ;;
*) sed -n '2,13p' "$0" >&2; exit 2;;
esac
