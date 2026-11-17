#!/usr/bin/env bash
# tools/l21b_linux_run.sh : ONE Linux reference run for task 21b (the wrapped bridge on the CPU Vulkan driver), with a free choice
# of input script and dump window.  tools/i21_run.sh with its four fixed routes is the model; the machine rules are the same.
#
#   tools/l21b_linux_run.sh TAG INPUT GAME_FRAMES EVERY FROM TO [VAR=VALUE ...]
#     TAG          everything goes to l21b/linux-TAG/ (run.out, phase.log, files.log, hash.tsv, env, exit, frames/*.ppm + index.tsv)
#     INPUT        input script (e.g. input/title-i21-hold1.txt)
#     GAME_FRAMES  KH_RUN_GAME_FRAMES
#     EVERY FROM TO   frame dumps: every EVERY-th present inside FROM..TO (inclusive).  1 FROM TO = every present of a window,
#                  which is what a comparison with another renderer needs: the other run's timeline is shifted by its load times,
#                  and the same game picture has to be FOUND (tools/l21b_compare.py align)
#     VAR=VALUE    more environment for the run (after the defaults, so it can override them)
# Binary: ./khnmc-x2-objcopy (bias addresses, the address model of the Mac build), KHNMC_GFX=vkd3dw, KHNMC_CLOCK=frames.
# Rules enforced here: ONE vkd3d run of this worktree at a time (lock i21/.lock, shared with tools/i21_run.sh; a second start is
# refused, never queued), nice -n 10, LP_NUM_THREADS=6, at least 3 GiB MemAvailable at the start, the CPU Vulkan driver only
# (KHNMC_GFX_DRIVER and KHNMC_GFX_VULKAN_LIB are cleared: the runtime selects lavapipe and refuses any device that is not a CPU),
# no display variables, no audio device.  Nothing is retried.  Raw dumps are 6.2 MB each: delete them when the numbers are taken.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 6 ] || { sed -n '2,19p' "$0" >&2; exit 2; }
TAG=$1; INPUT=$2; GFR=$3; EVERY=$4; FROM=$5; TO=$6; shift 6
case "$TAG" in ''|*[!A-Za-z0-9._-]*) echo "l21b_linux_run: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac
for v in "$GFR" "$EVERY" "$FROM" "$TO"; do case "$v" in ''|*[!0-9]*) echo "l21b_linux_run: GAME_FRAMES EVERY FROM TO must be numbers" >&2; exit 2;; esac; done
case "$INPUT" in /*) ;; *) INPUT="$HERE/$INPUT";; esac
BIN="$HERE/khnmc-x2-objcopy"
for f in "$BIN" "$HERE/pc/kh2fm.exe" "$INPUT" "$HERE/data-full" "$HERE/pkgdir"; do [ -e "$f" ] || { echo "l21b_linux_run: missing $f" >&2; exit 2; }; done
D="$HERE/l21b/linux-$TAG"; mkdir -p "$HERE/l21b" "$HERE/i21" || exit 2
[ ! -e "$D" ] || { echo "l21b_linux_run: $D exists already (choose another TAG; nothing is overwritten)" >&2; exit 2; }
exec 9>"$HERE/i21/.lock"
flock -n 9 || { echo "l21b_linux_run: another vkd3d run of this worktree holds i21/.lock: refusing to start a second one" >&2; exit 3; }
AVAIL=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
[ "$AVAIL" -ge 3072 ] || { echo "l21b_linux_run: MemAvailable is $AVAIL MB, under the 3072 MB the rules ask for: not starting" >&2; exit 3; }
mkdir -p "$D/root" "$D/docs" "$D/frames" || exit 2
cp -f "$BIN" "$D/khnmc.bin" || exit 2
SUM=$(sha256sum "$D/khnmc.bin" | cut -c1-16)
ENVV=( "KH_EXE=$HERE/pc/kh2fm.exe" "KH_ROOT=$D/root" "KH_DOCS=$D/docs" "KHNMC_DATA=$HERE/data-full" "KHNMC_PKGDIR=$HERE/pkgdir"
       "KHNMC_INPUT_SCRIPT=$INPUT" "KHNMC_PHASELOG=$D/phase.log" "KHNMC_FILELOG=$D/files.log" "KHNMC_CLOCK=frames" "KH_RUN_GAME_FRAMES=$GFR" "KH_RUN_SECONDS=3000"
       "KHNMC_GFX=vkd3dw" "KHNMC_GFX_DUMP=$D/frames" "KHNMC_GFX_DUMP_EVERY=$EVERY" "KHNMC_GFX_DUMP_FROM=$FROM" "KHNMC_GFX_DUMP_TO=$TO" "KHNMC_GFX_HASHLOG=$D/hash.tsv"
       "LP_NUM_THREADS=6" "VKD3D_DEBUG=warn" "VKD3D_SHADER_DEBUG=warn" "$@" )
printf '%s\n' "${ENVV[@]}" "binary=$BIN sha256=$SUM" "started=$(date '+%F %T') load=$(cut -d' ' -f1-3 /proc/loadavg) MemAvailable=${AVAIL}MB" > "$D/env"
echo "l21b_linux_run: $TAG: vkd3dw on khnmc-x2-objcopy ($SUM), $(basename "$INPUT") to game frame $GFR, dumps every $EVERY presents in $FROM..$TO; MemAvailable $AVAIL MB"
T0=$(date +%s)
( cd "$D/root" && env -u KHNMC_GFX_DRIVER -u KHNMC_GFX_VULKAN_LIB -u DISPLAY -u WAYLAND_DISPLAY "${ENVV[@]}" nice -n 10 "$D/khnmc.bin" > "$D/run.out" 2>&1 ); EX=$?
echo "$EX" > "$D/exit"; echo "ended=$(date '+%F %T') wall=$(( $(date +%s)-T0 ))s exit=$EX" >> "$D/env"
rm -f "$D/khnmc.bin"
N=$(ls "$D/frames" 2>/dev/null | grep -c '\.ppm$')
echo "l21b_linux_run: $TAG: exit $EX, $N dumps, $(du -sh "$D/frames" | cut -f1) of raw frames; $(grep -a 'kh: gfx: draws=' "$D/run.out" | grep -av 'gfx frame' | tail -1)"
exit "$EX"
