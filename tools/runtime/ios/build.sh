#!/usr/bin/env bash
# tools/ios/build.sh [sim|device|both] : steps 2 and 3 of the MacBook Air plan, from this Linux machine: copy the sources, then
# build the game's static libraries and the app for the Simulator SDK and / or the device SDK (tools/ios/mac/build.sh, under the
# shared lock, one lock per SDK).  The device build is compile + archive + link only: nothing is signed or installed.
# UNRUN: never executed (see tools/ios/common.sh).  Refuses to start without KHNMC_IOS_MACBOOK_OK=1.
#   default: both.  Logs come back to run-J26/ios/build-<sdk>.log
set -u
. "$(dirname "$0")/common.sh"
ios_guard
what=${1:-both}; case "$what" in sim|device|both) ;; *) sed -n '2,7p' "$0" >&2; exit 2;; esac
"$IOS_HERE/tools/ios/sync.sh" src || exit 1
rc=0
if [ "$what" != device ]; then mac_step "khnmc iOS simulator build" build.sh iphonesimulator; r=$?; busy_note $r; [ $r = 0 ] || rc=$r; fi
if [ $rc = 0 ] && [ "$what" != sim ]; then mac_step "khnmc iOS device build" build.sh iphoneos; r=$?; busy_note $r; [ $r = 0 ] || rc=$r; fi
mkdir -p "$IOS_HERE/run-J26/ios"
rsync -a -z -e "$IOS_RSH" --include 'build-*.log' --include 'codesign-*.txt' --exclude '*' "$IOS_MAC:$IOS_RDIR/run/" "$IOS_HERE/run-J26/ios/" || echo "tools/ios: could not fetch the logs" >&2
exit $rc
