#!/usr/bin/env bash
# tools/ios/sync.sh : copy what the iOS build needs from this worktree to ~/Developer/KHNMC/src on the MacBook Air.
# UNRUN: never executed (see tools/ios/common.sh).  Refuses to start without KHNMC_IOS_MACBOOK_OK=1.
#
#   tools/ios/sync.sh            sources: Makefile, runtime/ (with runtime/ios), lifter-rt/, tools/, input/, gen/*.{c,h}, lifted/gen/,
#                                lifted/shards/shard_*.c (117 files, 167 MB; rsync sends only what changed)
#   tools/ios/sync.sh exe        + pc/kh2fm.exe          -> ~/Developer/KHNMC/pc/kh2fm.exe
#   tools/ios/sync.sh route [D]  + the route data subset -> ~/Developer/KHNMC/route-data  (D: default ./route-subset, made by
#                                tools/make_route_subset.sh), content hash list compared on both sides
#   tools/ios/sync.sh all [D]    all three
# Game-derived material (lifted C, gen/, the exe, the route subset) goes to ~/Developer/KHNMC and nowhere else on that machine.
# rsync --delete acts inside the listed directories of ~/Developer/KHNMC/src only.  The Mac's shared lock is not taken (copying is
# neither a build nor a simulator step); this machine's driver lock is (common.sh ios_guard, shared with a calling driver), so
# no other tools/ios driver from any work tree copies or builds meanwhile.
# The source copies do NOT keep this machine's modification times and compare by content (-c): a changed file gets a fresh
# time on the Mac, so mac/build.sh's make rebuilds its objects even when its time here is older than an object compiled there
# from another work tree's copy; unchanged files keep their time there.
set -euo pipefail
. "$(dirname "$0")/common.sh"
ios_guard
cd "$IOS_HERE"
what=${1:-src}; SUB=${2:-$IOS_HERE/route-subset}
case "$what" in src|exe|route|all) ;; *) sed -n '2,11p' "$0" >&2; exit 2;; esac
RS=(rsync -rlpD -c --delete -e "$IOS_RSH")
for f in Makefile runtime lifter-rt tools input gen lifted/gen lifted/shards; do [ -e "$f" ] || { echo "ios/sync: missing $f in $IOS_HERE" >&2; exit 2; }; done
mac "mkdir -p ~/$IOS_RDIR/src/lifted/gen ~/$IOS_RDIR/src/lifted/shards ~/$IOS_RDIR/src/gen ~/$IOS_RDIR/pc ~/$IOS_RDIR/run"
"${RS[@]}" Makefile "$IOS_MAC:$IOS_RDIR/src/Makefile"
"${RS[@]}" runtime/ "$IOS_MAC:$IOS_RDIR/src/runtime/"
"${RS[@]}" lifter-rt/ "$IOS_MAC:$IOS_RDIR/src/lifter-rt/"
"${RS[@]}" --exclude '__pycache__' tools/ "$IOS_MAC:$IOS_RDIR/src/tools/"
"${RS[@]}" input/ "$IOS_MAC:$IOS_RDIR/src/input/"                # the route scripts (tools/ios/mac/device_run.sh route)
# the owner's app icon (docs/APPLE-PLAN.md "App icon"; derived from the mod's emblem, so never in the repository): into the Mac's copy
# of the asset catalog, after the tools/ copy above (whose --delete removes it there each time)
ICON=${KHNMC_APP_ICON:-$HOME/Documents/khnmc/app-icon/AppIcon-1024.png}
[ -f "$ICON" ] || { echo "ios/sync: no app icon at $ICON (the owner's 1024x1024 AppIcon-1024.png; or set KHNMC_APP_ICON)" >&2; exit 2; }
"${RS[@]}" "$ICON" "$IOS_MAC:$IOS_RDIR/src/tools/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"
"${RS[@]}" --include '*.c' --include '*.h' --exclude '*' gen/ "$IOS_MAC:$IOS_RDIR/src/gen/"
"${RS[@]}" lifted/gen/ "$IOS_MAC:$IOS_RDIR/src/lifted/gen/"
"${RS[@]}" -z --include 'shard_*.c' --exclude '*' lifted/shards/ "$IOS_MAC:$IOS_RDIR/src/lifted/shards/"
L=$( (cd lifted/shards && sha256sum shard_*.c) | sha256sum | cut -d' ' -f1)
R=$(mac "cd ~/$IOS_RDIR/src/lifted/shards && shasum -a 256 shard_*.c | shasum -a 256 | cut -d' ' -f1")
[ "$L" = "$R" ] || { echo "ios/sync: lifted shards differ after the copy (here ${L:0:16}, there ${R:0:16})" >&2; exit 1; }
echo "ios/sync: sources in ~/$IOS_RDIR/src; lifted shards: $(ls lifted/shards/shard_*.c | wc -l) files, list sha256 ${L:0:16} equal on both sides"
if [ "$what" = exe ] || [ "$what" = all ]; then
  rsync -a -L -z -e "$IOS_RSH" pc/kh2fm.exe "$IOS_MAC:$IOS_RDIR/pc/kh2fm.exe"
  L=$(sha256sum pc/kh2fm.exe | cut -c1-16); R=$(mac "shasum -a 256 ~/$IOS_RDIR/pc/kh2fm.exe | cut -c1-16")
  [ "$L" = "$R" ] || { echo "ios/sync: the exe differs after the copy" >&2; exit 1; }
  echo "ios/sync: pc/kh2fm.exe sha256 $L equal on both sides"
fi
if [ "$what" = route ] || [ "$what" = all ]; then
  [ -d "$SUB/data" ] && [ -d "$SUB/pkgdir" ] && [ -f "$SUB/MANIFEST.tsv" ] || { echo "ios/sync: $SUB is not a route subset (data/, pkgdir/, MANIFEST.tsv): run tools/make_route_subset.sh" >&2; exit 2; }
  nl=$(cd "$SUB" && find data pkgdir -type f | wc -l | tr -d ' '); nm=$(awk -F'\t' 'NR>1 && ($2=="full" || $2=="ranges") {n++} END {print n+0}' "$SUB/MANIFEST.tsv")
  [ "$nl" -gt 0 ] && [ "$nl" -ge "$nm" ] || { echo "ios/sync: $SUB has $nl data files, its MANIFEST.tsv lists $nm with content: refusing to mirror it over the Mac's copy (review r9: an empty subset would erase it and still verify)" >&2; exit 2; }
  mac "mkdir -p ~/$IOS_RDIR/route-data"
  rsync -a -z --delete -e "$IOS_RSH" "$SUB/" "$IOS_MAC:$IOS_RDIR/route-data/"
  L=$(cd "$SUB" && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done | sha256sum | cut -d' ' -f1)
  R=$(mac "cd ~/$IOS_RDIR/route-data && find data pkgdir -type f | LC_ALL=C sort | while read -r f; do printf '%s %s\n' \"\$(shasum -a 256 \"\$f\" | cut -d' ' -f1)\" \"\$f\"; done | shasum -a 256 | cut -d' ' -f1")
  n=$(cd "$SUB" && find data pkgdir -type f | wc -l)
  [ "$L" = "$R" ] || { echo "ios/sync: the route data copy differs from the subset (content hash list here ${L:0:16}, there ${R:0:16})" >&2; exit 1; }
  echo "ios/sync: route data: $n files, content hash list ${L:0:16} equal on both sides; on the Mac: $(mac "du -sh ~/$IOS_RDIR/route-data | cut -f1")"
fi
mac "du -sh ~/$IOS_RDIR | sed 's|^|ios/sync: ~/$IOS_RDIR/ now holds |'"    # '|': the path holds '/' (BSD sed refused the old form)
