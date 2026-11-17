#!/usr/bin/env bash
# tools/darwin_catalyst_check.sh : the nearest thing to an iOS-SDK compile check that a Mac WITHOUT Xcode can make (task 26).
#
#   tools/darwin_catalyst_check.sh           result: $RUN/mac-catalyst-check.log, one summary line; exit 1 when a file failed
#   env as tools/darwin_build.sh (KHNMC_MAC, KHNMC_MAC_DIR, JOBS, KHNMC_MAC_NICE, KHNMC_MAC_MIN_FREE), KHNMC_RUN_DIR
#
# The Command Line Tools on the Mac mini have no iPhoneOS SDK.  Their macOS SDK does carry the iOS-family API of Mac Catalyst
# (System/iOSSupport), and clang takes the target arm64-apple-ios17.0-macabi.  For that target TARGET_OS_IPHONE is 1 and
# TARGET_OS_OSX is 0, so in runtime/kh_compat.h KH_IOS is 1 and KH_IOS_PATHS is on BY DEFAULT (nothing is forced with -D), core.c
# takes its mincore branch, and the SDK's availability attributes are evaluated for the iOS family.  The script
#   1. compiles every hand-written runtime source, lifter-rt and the generated mod code for that target
#      (make -k rtobjs CC="clang -target arm64-apple-ios17.0-macabi" O=obj-macabi: objects only, nothing is linked or run);
#   2. checks the syntax of the two Objective-C files (runtime/ios/ios_main.m, tools/ios/probe/probe_main.m) and compiles the probe's
#      C part against the Catalyst UIKit headers (-fsyntax-only, -iframework .../System/iOSSupport/System/Library/Frameworks;
#      without -fmodules, so that clang writes no module cache outside the build directory).
# What this is NOT: the iPhoneOS or iPhoneSimulator SDK.  Catalyst is the iOS API on macOS: headers that differ between the two
# SDKs, and anything marked unavailable on iOS but allowed on Catalyst, are not covered.  tools/ios/mac/sdk_check.sh is the real
# check and is still UNRUN.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-j26}
RUN=${KHNMC_RUN_DIR:-$HERE/run-J26}
JOBS=${JOBS:-4}; NICE=${KHNMC_MAC_NICE:-10}; MINFREE=${KHNMC_MAC_MIN_FREE:-30}
case "$RDIR" in khnmc-work/build-*) ;; *) echo "darwin_catalyst_check: KHNMC_MAC_DIR must be khnmc-work/build-*" >&2; exit 2;; esac
for v in "$JOBS" "$NICE" "$MINFREE"; do case "$v" in ''|*[!0-9]*) echo "darwin_catalyst_check: JOBS / KHNMC_MAC_NICE / KHNMC_MAC_MIN_FREE must be numbers" >&2; exit 2;; esac; done
[ "$JOBS" -le 4 ] || { echo "darwin_catalyst_check: at most 4 jobs" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30)
RS=(rsync -a --delete -e "ssh -o BatchMode=yes -o ConnectTimeout=20")
cd "$HERE"; mkdir -p "$RUN"
"${RS[@]}" Makefile "$MAC:$RDIR/Makefile"; "${RS[@]}" runtime/ "$MAC:$RDIR/runtime/"; "${RS[@]}" --exclude '__pycache__' tools/ "$MAC:$RDIR/tools/"
T=arm64-apple-ios17.0-macabi
"${SSH[@]}" "$MAC" "cd ~/$RDIR && mkdir -p run && { 
free=\$(memory_pressure | tail -1 | sed -n 's/.*percentage: \\([0-9]*\\)%.*/\\1/p'); echo \"darwin_catalyst_check: memory free \${free:-?}% (start at >= $MINFREE%), \$(clang --version | head -1)\"
{ [ -n \"\$free\" ] && [ \"\$free\" -ge $MINFREE ]; } || { echo 'darwin_catalyst_check: memory too low: not starting' >&2; exit 3; }
SDK=\$(xcrun --show-sdk-path); FW=\$SDK/System/iOSSupport/System/Library/Frameworks
[ -d \"\$FW/UIKit.framework\" ] || { echo \"darwin_catalyst_check: this SDK has no Catalyst UIKit (\$FW): the check cannot be made here\" >&2; exit 4; }
echo \"darwin_catalyst_check: SDK \$SDK (\$(xcrun --show-sdk-version)), target $T\"
echo '#include <TargetConditionals.h>
#include \"kh_compat.h\"
KH_IOS=KH_IOS KH_IOS_PATHS=KH_IOS_PATHS TARGET_OS_IPHONE=TARGET_OS_IPHONE TARGET_OS_OSX=TARGET_OS_OSX TARGET_OS_MACCATALYST=TARGET_OS_MACCATALYST' | clang -target $T -Iruntime -E -P -x c - | tail -1 | sed 's/^/darwin_catalyst_check: the preprocessor says: /'
nice -n $NICE make -k -j$JOBS rtobjs CC='clang -target $T' O=obj-macabi; mrc=\$?
n=\$(ls obj-macabi/*.o 2>/dev/null | wc -l | tr -d ' ')
echo \"darwin_catalyst_check: make -k rtobjs for $T: exit \$mrc, \$n objects\"
orc=0
for f in runtime/ios/ios_main.m tools/ios/probe/probe_main.m tools/ios/probe/probe_core.c; do
  if nice -n $NICE clang -target $T -iframework \"\$FW\" -fobjc-arc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Iruntime -fsyntax-only \"\$f\"; then echo \"darwin_catalyst_check: syntax ok: \$f\"; else echo \"darwin_catalyst_check: FAILED: \$f\"; orc=1; fi
done
nm -u obj-macabi/host_env.o | grep -c '_fork\$' | sed 's/^/darwin_catalyst_check: references to fork in obj-macabi\\/host_env.o: /'
nm -u obj-macabi/*.o | grep -E '^ +U _(fork|vfork|system|popen|posix_spawn|tmpfile|tmpnam|mktemp)\$' | sort | uniq -c | sed 's/^/darwin_catalyst_check: FORBIDDEN import in the objects: /'
nm -u obj-macabi/core.o | grep -E '_(mincore|mach_vm_page_range_query)\$' | sed 's/^/darwin_catalyst_check: core.o imports: /'
[ \$mrc = 0 ] && [ \$orc = 0 ]; }" > "$RUN/mac-catalyst-check.log" 2>&1 && rc=0 || rc=$?
grep "darwin_catalyst_check\| error: " "$RUN/mac-catalyst-check.log" | cut -c1-260
exit $rc
