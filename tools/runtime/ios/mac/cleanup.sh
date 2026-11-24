#!/bin/bash
# tools/ios/mac/cleanup.sh [objects|all] : what is left of this project on the MacBook Air, and remove what is no longer needed.
# UNRUN (see tools/ios/common.sh).
#   objects (default)  remove the build intermediates: src/obj-<sdk> AND src/obj-<sdk>-om (the ORDERED_MEM=1 variant, the default: its
#                      libraries and shard objects), build/xc/obj, build/dd (Xcode's derived data and module cache, see mac/build.sh)
#                      and the signing job's build/app-signed/dd and build/app-signed/obj, so the project stays under its 6 GB limit.
#                      Keeps the sources, the apps in build/xc/sym and build/app-signed/sym, the logs in run/.
#   all                remove everything of this project except run/ (the logs): src/, build/, pc/, route-data/, state/.
# In both modes: a simulator of ours that is still recorded (state/sim.udid) is deleted (by UDID, and only while it still carries
# our name); nothing outside ~/Developer/KHNMC is removed.  Prints what is left and its size.
. "$(dirname "$0")/lib.sh"
MODE=${1:-objects}
case "$MODE" in objects|all) ;; *) sed -n '2,11p' "$0" >&2; exit 2;; esac
if [ -s "$STATE/sim.udid" ]; then u=$(cat "$STATE/sim.udid")
  if xcrun simctl list devices | grep -q "^ *$SIM_NAME ($u)"; then xcrun simctl shutdown "$u" >/dev/null 2>&1; xcrun simctl delete "$u" && rm -f "$STATE/sim.udid" && echo "ios/mac: cleanup: deleted our simulator $u"
  else echo "ios/mac: cleanup: recorded simulator $u is not listed under our name any more: record removed, nothing deleted"; rm -f "$STATE/sim.udid"; fi; fi
n=$(xcrun simctl list devices | grep -c "^ *$SIM_NAME (")
[ "$n" = 0 ] && echo "ios/mac: cleanup: no simulator named '$SIM_NAME' exists" || echo "ios/mac: cleanup: WARNING: $n simulator(s) named '$SIM_NAME' exist without a record of ours: left alone, look at them"
cd "$KROOT" || exit 1
case "$MODE" in
  objects) rm -rf "$KROOT/src/obj-iphoneos" "$KROOT/src/obj-iphonesimulator" "$KROOT/src/obj-iphoneos-om" "$KROOT/src/obj-iphonesimulator-om" "$KROOT/build/xc/obj" "$KROOT/build/dd" "$KROOT/build/app-signed/dd" "$KROOT/build/app-signed/obj";;
  all)     cd "$KROOT/run" && rm -rf "$KROOT/src" "$KROOT/build" "$KROOT/pc" "$KROOT/route-data" "$KROOT/state";;
esac
echo "ios/mac: cleanup ($MODE): left in ~/Developer/KHNMC:"; du -sh "$KROOT"/* 2>/dev/null | sed 's/^/    /'
echo "ios/mac: cleanup: total $(du -sh "$KROOT" | cut -f1); the lock file ~/Developer/.mac-build.lock is released by maclock.py when this step ends"
