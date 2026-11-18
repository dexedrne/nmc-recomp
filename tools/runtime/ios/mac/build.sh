#!/bin/bash
# tools/ios/mac/build.sh SDK : steps 2 and 3 for one SDK (iphonesimulator | iphoneos).  UNRUN (see tools/ios/common.sh).
#   1. the game as two static libraries: `make IOS_SDK=<sdk> applib` (the 117 lifted shards through the rename-mode hook
#      generator, the runtime, gen/, forwarders): obj-<sdk>/libkhnmc-rt.a and obj-<sdk>/libkh2loose.a.  2 jobs, niced; the memory
#      reading is taken before the start, before every 20 shards that need compiling and before the runtime objects and the
#      archives, also when every shard is up to date (stop below the limit: exit 3, objects stay);
#   2. the app: tools/ios/make_project.py, then (after a fresh memory reading) xcodebuild -target KHNMC for that SDK, unsigned,
#      products and intermediates in ~/Developer/KHNMC/build/xc (SYMROOT, OBJROOT), Xcode's derived data (build logs, the
#      project's DerivedData folder) and the Clang module cache in ~/Developer/KHNMC/build/dd (-derivedDataPath,
#      MODULE_CACHE_DIR), so that nothing goes to ~/Library/Developer/Xcode/DerivedData.  Never run: whether Xcode still writes
#      anything of its own elsewhere under ~/Library (preferences, caches no option moves) is not known;
#   3. the link checks on the APP binary (`make IOS_SDK=<sdk> appcheck`: every hook, the mod's re-lifted functions, no fork /
#      system / popen / tmpfile import).
#   The device SDK build is compile + archive + link only: nothing is signed, nothing is installed, no device is contacted.
#   GFX=1 (TASK38; device SDK only): the libraries with the graphics backend (make GFX_VKD3D=1: vkd3d for iOS + MoltenVK, see lib.sh),
#   checked in the archive, and xcodebuild gets KHNMC_GFX_LDFLAGS (the Makefile's GFX_APP_LDFLAGS); the link checks then allow exactly
#   mktemp (MoltenVK imports it).  Without GFX=1 every step is as before.
. "$(dirname "$0")/lib.sh"
SDK=${1:-}; T=$(sdk_triple "$SDK") || exit 2
[ "$GFX" = 1 ] && [ "$SDK" != iphoneos ] && { echo "ios/mac: build: GFX=1 is for the device SDK only (MoltenVK's iOS library has no simulator slice): the $SDK app keeps the null backend" >&2; exit 2; }
LOG="$RUNS/build-$SDK.log"; : > "$LOG"
# every memory gate: mem_gate ends with `exit 3`, which in a pipeline ends only its subshell, hence the PIPESTATUS check
mem_gate "the $SDK build" | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || exit 3
OL="obj-$SDK"
# no build-machine paths in the objects (debug info, __FILE__): the same two variables in EVERY make call of this script
MK=(nice -n $NICE make IOS_SDK="$SDK" COREAUDIO=1 "EXTRA=-ffile-prefix-map=$SRC/=khnmc/" "LIFT_EXTRA=-ffile-prefix-map=$SRC/=khnmc/")
[ "$GFX" = 1 ] && MK+=(GFX_VKD3D=1)
"${MK[@]}" "$OL/shards/.stamp" >> "$LOG" 2>&1 || { echo "ios/mac: build: the renamed shard copies could not be generated (see $LOG)" >&2; exit 1; }
names=$(cd lifted/shards && ls shard_*.c | sed 's/\.c$//'); total=$(echo "$names" | wc -l | tr -d ' ')
# shellcheck disable=SC2086
set -- $names
n=0
while [ $# -gt 0 ]; do
  chunk=""; i=0
  while [ $# -gt 0 ] && [ $i -lt 20 ]; do chunk="$chunk $OL/lifted/$1.o"; shift; i=$((i+1)); n=$((n+1)); done
  # shellcheck disable=SC2086
  if "${MK[@]}" -q $chunk 2>/dev/null; then continue; fi
  mem_gate "shards up to $n of $total" | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || exit 3
  # shellcheck disable=SC2086
  "${MK[@]}" -j$JOBS $chunk >> "$LOG" 2>&1 || { echo "ios/mac: build: a shard failed to compile for $SDK (see $LOG)" >&2; exit 1; }
done
mem_gate "the runtime objects and the archives" | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || exit 3
"${MK[@]}" -j$JOBS applib >> "$LOG" 2>&1 || { echo "ios/mac: build: applib failed for $SDK (see $LOG)" >&2; grep " error: " "$LOG" | head -20 >&2; exit 1; }
ls -l "$OL/libkhnmc-rt.a" "$OL/libkh2loose.a" | tee -a "$LOG"
if [ "$GFX" = 1 ]; then
  gfx_lib_check "$OL/libkhnmc-rt.a" 2>&1 | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || exit 1
  GFXLD=$(gfx_ldflags 2>> "$LOG") || { echo "ios/mac: build: no KHNMC_GFX_LDFLAGS (see $LOG)" >&2; tail -3 "$LOG" >&2; exit 1; }
  GX=("KHNMC_GFX_LDFLAGS=$GFXLD")
else GX=(); fi
python3 tools/ios/make_project.py >> "$LOG" 2>&1 || { echo "ios/mac: build: make_project.py failed" >&2; exit 1; }
[ "$GFX" = 1 ] && KHNMC_GFX_LDFLAGS="$GFXLD" python3 tools/ios/make_project.py --show-ldflags | tee -a "$LOG"
XC="$KROOT/build/xc"; mkdir -p "$XC" "$KROOT/build/dd"
mem_gate "xcodebuild of the app" | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || exit 3
# DEAD_CODE_STRIPPING=NO: the link check (tools/check_hook_link.py) proves every hooked function was renamed by finding its
# orig_f_<rva>; a replaced original is never called, and dead stripping removed it from the first app (2026-11-17)
DEST="generic/platform=iOS"; [ "$SDK" = iphonesimulator ] && DEST="generic/platform=iOS Simulator"
nice -n $NICE xcodebuild -project tools/ios/KHNMC.xcodeproj -scheme KHNMC -sdk "$SDK" -destination "$DEST" -configuration Release \
    -derivedDataPath "$KROOT/build/dd" -jobs $JOBS SYMROOT="$XC/sym" OBJROOT="$XC/obj" MODULE_CACHE_DIR="$KROOT/build/dd/ModuleCache.noindex" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= DEAD_CODE_STRIPPING=NO ${GX[@]+"${GX[@]}"} build >> "$LOG" 2>&1 \
    || { echo "ios/mac: build: xcodebuild failed for $SDK (see $LOG)" >&2; grep -B2 -A6 "error:" "$LOG" | tail -40 >&2; exit 1; }
APP="$XC/sym/Release-$SDK/KHNMC.app"
[ -x "$APP/KHNMC" ] || { echo "ios/mac: build: no app binary at $APP/KHNMC" >&2; exit 1; }
"${MK[@]}" appcheck APP_BIN="$APP/KHNMC" 2>&1 | tee -a "$LOG"; [ "${PIPESTATUS[0]}" = 0 ] || { echo "ios/mac: build: a link check failed on the app binary" >&2; exit 1; }
if codesign -dv "$APP" > "$RUNS/codesign-$SDK.txt" 2>&1; then echo "ios/mac: build: NOTE: the app carries a signature (expected: none or ad hoc from the linker): $(head -3 "$RUNS/codesign-$SDK.txt" | tr '\n' ' ')" | tee -a "$LOG"; fi
echo "ios/mac: build: $SDK: $APP ($(du -sh "$APP" | cut -f1)), binary sha256 $(shasum -a 256 "$APP/KHNMC" | cut -c1-16), $(file "$APP/KHNMC" | cut -d: -f2)" | tee -a "$LOG"
# review X38 n=4: the expectation was wrong (the null app gave 2): runtime/core.c compiles in the synthetic Windows path
# "/Users/khnmc/Documents" (every app), and only an app WITHOUT the backend has the gfx-vkd3d-not-built message (runtime/gfx_vkd3d.c)
np=$(strings -a "$APP/KHNMC" | grep -c "$HOME\|/home/\|/Users/"); echo "ios/mac: build: strings of the app binary that look like a home-directory path: $np (expected $([ "$GFX" = 1 ] && echo 1 || echo 2): core.c's synthetic \"/Users/khnmc/Documents\"$([ "$GFX" = 1 ] || echo " and the text of the gfx-vkd3d-not-built message"))" | tee -a "$LOG"
strings -a "$APP/KHNMC" | grep "$HOME\|/home/\|/Users/" | head -5 | cut -c1-160 | sed 's/^/ios\/mac: build:   counted: /' | tee -a "$LOG"
disk_note | tee -a "$LOG"
