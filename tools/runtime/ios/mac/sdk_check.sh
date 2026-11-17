#!/bin/bash
# tools/ios/mac/sdk_check.sh : step 1, the iOS-SDK compile check.  UNRUN (see tools/ios/common.sh).
#   Every hand-written runtime source, lifter-rt, the generated mod code and the tables are COMPILED for the device SDK and for the
#   simulator SDK with the Darwin build's flags (make -k IOS_SDK=<sdk> rtobjs: xcrun clang, arm64, -DKH_XLATE_MODE=2, the policy
#   flags; KH_IOS_PATHS is on by itself for an iOS target), then the two Objective-C files and the probe.  -k: every error of the
#   port in one pass.  Nothing is linked, no shard is compiled (they hold no system call; the full build is build.sh).
#   Result: $RUNS/sdk-check-<sdk>.log and one summary line per SDK; exit 1 when anything failed to compile.
. "$(dirname "$0")/lib.sh"
mem_gate "the compile check"
rc=0
for sdk in iphoneos iphonesimulator; do
  T=$(sdk_triple "$sdk"); LOG="$RUNS/sdk-check-$sdk.log"
  echo "ios/mac: sdk_check: $sdk $(xcrun --sdk "$sdk" --show-sdk-version), $(xcrun --sdk "$sdk" clang --version | head -1), target $T" | tee "$LOG"
  nice -n $NICE make -k -j$JOBS IOS_SDK="$sdk" rtobjs >> "$LOG" 2>&1; mrc=$?
  mkdir -p "obj-$sdk/objc"
  for f in runtime/ios/ios_main.m tools/ios/probe/probe_main.m tools/ios/probe/probe_core.c; do
    nice -n $NICE xcrun --sdk "$sdk" clang -target "$T" -fobjc-arc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Iruntime -c "$f" -o "obj-$sdk/objc/$(basename "$f").o" >> "$LOG" 2>&1 || { echo "ios/mac: sdk_check: $f FAILED for $sdk" | tee -a "$LOG"; mrc=1; }
  done
  nerr=$(grep -c " error: " "$LOG"); nobj=$(ls "obj-$sdk"/*.o 2>/dev/null | wc -l | tr -d ' ')
  echo "ios/mac: sdk_check: $sdk: make exit $mrc, $nerr error lines, $nobj objects compiled" | tee -a "$LOG"
  [ "$mrc" = 0 ] || { rc=1; grep " error: " "$LOG" | cut -c1-220 | sort | uniq -c | sort -rn | head -40; }
done
disk_note
exit $rc
