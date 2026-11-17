#!/usr/bin/env bash
# tools/i21_run.sh : ONE picture run of task 21 (a vkd3d-backed route with frame dumps), with the machine rules built in.
#
#   tools/i21_run.sh TAG BINARY BACKEND ROUTE [GAME_FRAMES]
#     TAG       name of the run; everything goes to i21/TAG/ (run.out, phase.log, env, exit, frames/*.ppm + index.tsv, and hash.tsv:
#               two hashes of the picture of EVERY present, which tools/i21_compare.py aligns between runs)
#     BINARY    ./khnmc (identity addresses) or ./khnmc-x2-objcopy (bias addresses)
#     BACKEND   vkd3d  = host vkd3d objects through the Windows-ABI trampoline (identity build only: the reference)
#               vkd3dw = wrapped bridge (guest-side COM objects, runtime/gfx_wrap.c)
#     ROUTE     title  = input/title-i21.txt, dumps every 40 presents
#               field  = input/field-battle-d18.txt, dumps every 20 presents from present 1800 on (menus, field, battle)
#               titlehold = input/title-i21-hold1.txt (the title route with the presses anchored to the Initial Settings page)
#               fieldidle = input/field-idle-i21.txt (the field route without input in the field), dumps every 20 presents from 2400
#     GAME_FRAMES  where the run ends (KH_RUN_GAME_FRAMES; default 2700 for the title routes, 3700 for the field routes)
#
# Rules enforced here: at most ONE vkd3d run at a time (lock file i21/.lock: a second start is refused, never queued), nice -n 10,
# LP_NUM_THREADS=6, at least 3 GiB MemAvailable at the start (refused otherwise), KHNMC_CLOCK=frames, the CPU Vulkan driver (the runtime
# selects lavapipe itself and refuses anything that is not a CPU device; KHNMC_GFX_DRIVER is cleared here so nothing can ask for the
# system driver), no window, no audio device (the default audio backend is the null device).  Nothing is retried.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 4 ] || { sed -n '2,17p' "$0" >&2; exit 2; }
TAG=$1; BIN=$2; BACKEND=$3; ROUTE=$4; GFR=${5:-}
case "$BACKEND" in vkd3d|vkd3dw) ;; *) echo "i21_run: BACKEND must be vkd3d or vkd3dw" >&2; exit 2;; esac
case "$ROUTE" in
  title) INPUT="$HERE/input/title-i21.txt"; EVERY=40; FROM=0; : "${GFR:=2700}";;
  field) INPUT="$HERE/input/field-battle-d18.txt"; EVERY=20; FROM=1800; : "${GFR:=3700}";;
  titlehold) INPUT="$HERE/input/title-i21-hold1.txt"; EVERY=40; FROM=0; : "${GFR:=2700}";;
  fieldidle) INPUT="$HERE/input/field-idle-i21.txt"; EVERY=20; FROM=2400; : "${GFR:=3700}";;
  *) echo "i21_run: ROUTE must be title, field, titlehold or fieldidle" >&2; exit 2;;
esac
case "$BIN" in /*) ;; *) BIN="$HERE/$BIN";; esac
for f in "$BIN" "$HERE/pc/kh2fm.exe" "$INPUT" "$HERE/data-full" "$HERE/pkgdir"; do [ -e "$f" ] || { echo "i21_run: missing $f" >&2; exit 2; }; done
D="$HERE/i21/$TAG"; mkdir -p "$HERE/i21" || exit 2
[ ! -e "$D" ] || { echo "i21_run: $D exists already (choose another TAG; nothing is overwritten)" >&2; exit 2; }
exec 9>"$HERE/i21/.lock"
flock -n 9 || { echo "i21_run: another vkd3d run of this worktree holds i21/.lock: refusing to start a second one" >&2; exit 3; }
AVAIL=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
[ "$AVAIL" -ge 3072 ] || { echo "i21_run: MemAvailable is $AVAIL MB, under the 3072 MB the rules ask for: not starting" >&2; exit 3; }
mkdir -p "$D/root" "$D/docs" "$D/frames" || exit 2
cp -f "$BIN" "$D/khnmc.bin" || exit 2
SUM=$(sha256sum "$D/khnmc.bin" | cut -c1-16)
ENVV=( "KH_EXE=$HERE/pc/kh2fm.exe" "KH_ROOT=$D/root" "KH_DOCS=$D/docs" "KHNMC_DATA=$HERE/data-full" "KHNMC_PKGDIR=$HERE/pkgdir"
       "KHNMC_INPUT_SCRIPT=$INPUT" "KHNMC_PHASELOG=$D/phase.log" "KHNMC_CLOCK=frames" "KH_RUN_GAME_FRAMES=$GFR" "KH_RUN_SECONDS=3000"
       "KHNMC_GFX=$BACKEND" "KHNMC_GFX_DUMP=$D/frames" "KHNMC_GFX_DUMP_EVERY=$EVERY" "KHNMC_GFX_DUMP_FROM=$FROM" "KHNMC_GFX_HASHLOG=$D/hash.tsv" "LP_NUM_THREADS=6"
       "VKD3D_DEBUG=warn" "VKD3D_SHADER_DEBUG=warn" )
printf '%s\n' "${ENVV[@]}" "binary=$BIN sha256=$SUM" "started=$(date '+%F %T') load=$(cut -d' ' -f1-3 /proc/loadavg) MemAvailable=${AVAIL}MB" > "$D/env"
echo "i21_run: $TAG: $BACKEND on $(basename "$BIN") ($SUM), route $ROUTE to game frame $GFR, dumps every $EVERY presents from $FROM; MemAvailable $AVAIL MB"
T0=$(date +%s)
( cd "$D/root" && env -u KHNMC_GFX_DRIVER -u DISPLAY -u WAYLAND_DISPLAY "${ENVV[@]}" nice -n 10 "$D/khnmc.bin" > "$D/run.out" 2>&1 ); EX=$?
echo "$EX" > "$D/exit"; echo "ended=$(date '+%F %T') wall=$(( $(date +%s)-T0 ))s exit=$EX" >> "$D/env"
rm -f "$D/khnmc.bin"
N=$(ls "$D/frames" 2>/dev/null | grep -c '\.ppm$')
echo "i21_run: $TAG: exit $EX, $N dumps, $(du -sh "$D/frames" | cut -f1) of raw frames; $(grep -a 'kh: gfx: draws=' "$D/run.out" | grep -av 'gfx frame' | tail -1)"
exit "$EX"
