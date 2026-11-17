# tools/ios/mac/lib.sh : sourced by the scripts that run ON the MacBook Air (bash 3.2).  UNRUN (see tools/ios/common.sh).
# They are started by tools/ios/*.sh through tools/ios/maclock.py, i.e. with the shared build lock held, from ~/Developer/KHNMC/src.
set -u
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export LC_ALL=C
KROOT="$HOME/Developer/KHNMC"                 # the only directory of this project on this Mac
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
