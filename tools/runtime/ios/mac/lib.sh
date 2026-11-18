# tools/ios/mac/lib.sh : sourced by the scripts that run ON the MacBook Air (bash 3.2).  UNRUN (see tools/ios/common.sh).
# They are started by tools/ios/*.sh through tools/ios/maclock.py, i.e. with the shared build lock held, from ~/Developer/KHNMC/src.
set -u
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export LC_ALL=C
KROOT="${KHNMC_KROOT:-$HOME/Developer/KHNMC}"  # the only directory of this project on this Mac; KHNMC_KROOT (owner 2026-11-18):
                                              # another Mac that only installs and runs a signed app (the Mac mini: ~/khnmc-work/phone)
SRC="$KROOT/src"; RUNS="$KROOT/run"; STATE="$KROOT/state"
JOBS=2; NICE=10; MINFREE=25                   # 8 GB RAM: 2 compile jobs, niced, no compile below 25 % free memory
SIM_NAME="KHNMC Test"                         # OUR simulator; found again only through the UDID we wrote down
BUNDLE_ID=beer.vyvanse.khnmc
case "$PWD" in "$SRC"|"$SRC"/*) ;; *) echo "ios/mac: must run inside $SRC (here: $PWD)" >&2; exit 2;; esac
mkdir -p "$RUNS" "$STATE"
memfree(){ memory_pressure | tail -1 | sed -n 's/.*percentage: \([0-9]*\)%.*/\1/p'; }
mem_gate(){ local f; f=$(memfree); echo "ios/mac: memory free ${f:-?}% before $1 (limit ${MINFREE}%)"
  [ -n "$f" ] && [ "$f" -ge "$MINFREE" ] || { echo "ios/mac: memory free ${f:-?}% is below ${MINFREE}% (or unreadable): not starting $1" >&2; exit 3; }; }
disk_note(){ echo "ios/mac: ~/Developer/KHNMC holds $(du -sh "$KROOT" 2>/dev/null | cut -f1) (limit 6 GB); free on the volume: $(df -h "$KROOT" | tail -1 | awk '{print $4}')"; }
sdk_triple(){ case "$1" in iphoneos) echo arm64-apple-ios17.0;; iphonesimulator) echo arm64-apple-ios17.0-simulator;; *) echo "ios/mac: SDK must be iphoneos or iphonesimulator" >&2; exit 2;; esac; }
# TASK38 (IOS-GRAPHICS-PLAN step 3): GFX=1 (environment) = the app WITH the graphics backend (vkd3d 2.1 for iOS and MoltenVK, both linked
# statically): mac/build.sh builds the libraries with GFX_VKD3D=1, mac/build.sh and mac/sign_app.sh hand xcodebuild KHNMC_GFX_LDFLAGS,
# mac/device_run.sh runs the game on it.  The app force-loads libkhnmc-rt.a, so the archive's backend and GFX must agree (gfx_lib_check):
# a graphics library links only with those flags, and one without the backend would make a null-backend app.  Default 0: as before.
GFX=${GFX:-0}; case "$GFX" in 0|1) ;; *) echo "ios/mac: GFX must be 0 or 1 (got '$GFX')" >&2; exit 2;; esac
# gfx_has FILE : does FILE (the archive or the app binary) define the graphics backend (kh_gfxc_device_new)?  0 = yes, 1 = no,
# 2 = nm could not read it (a non-zero exit status, or no symbol line at all): a named error, never taken for "no" (review X38 n=3:
# `nm ... 2>/dev/null | grep -q` made an unreadable archive pass as "absent, as GFX=0 asks")
gfx_has(){ local r rc n h
  r=$( { nm -g "$1" 2>/dev/null; echo "nm-exit=$?"; } | awk '/^nm-exit=/{rc=substr($0,9); next} {n++} / T _kh_gfxc_device_new$/{h=1} END{print rc" "n+0" "h+0}')
  rc=${r%% *}; r=${r#* }; n=${r%% *}; h=${r#* }
  if [ "$rc" != 0 ] || [ "$n" = 0 ]; then echo "ios/mac: nm could not read $1 (nm exit status $rc, $n symbol lines): whether it has the graphics backend is unknown" >&2; return 2; fi
  [ "$h" = 1 ]; }
# gfx_lib_check LIB : the archive has the graphics backend (kh_gfxc_device_new is defined) exactly when GFX=1
gfx_lib_check(){ local lib=$1 has
  [ -f "$lib" ] || { echo "ios/mac: $lib does not exist (mac/build.sh first)" >&2; return 1; }
  gfx_has "$lib"; case $? in 0) has=1;; 1) has=0;; *) echo "ios/mac: GFX=$GFX: $lib could not be checked for the graphics backend (nm, above): nothing is built or signed from it" >&2; return 1;; esac
  if [ "$has" = "$GFX" ]; then echo "ios/mac: $lib: graphics backend $([ "$has" = 1 ] && echo present || echo absent), as GFX=$GFX asks"; return 0; fi
  echo "ios/mac: GFX=$GFX, but $lib was built $([ "$has" = 1 ] && echo WITH || echo WITHOUT) the graphics backend: run GFX=$GFX tools/ios/mac/build.sh iphoneos first" >&2; return 1; }
# gfx_ldflags : KHNMC_GFX_LDFLAGS for xcodebuild, from the Makefile (the one list of what the backend links: GFX_APP_LDFLAGS); every
# file it names must exist (the Makefile already refuses a missing piece by name)
gfx_ldflags(){ local v w; v=$(make -s IOS_SDK=iphoneos GFX_VKD3D=1 print-GFX_APP_LDFLAGS) || return 1
  [ -n "$v" ] || { echo "ios/mac: the Makefile gives an empty GFX_APP_LDFLAGS" >&2; return 1; }
  for w in $v; do case "$w" in /*) [ -f "$w" ] || { echo "ios/mac: KHNMC_GFX_LDFLAGS names $w, which does not exist" >&2; return 1; };; esac; done
  printf '%s\n' "$v"; }
