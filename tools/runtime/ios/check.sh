#!/usr/bin/env bash
# tools/ios/check.sh : step 1 of the MacBook Air plan, from this Linux machine: copy the sources, then compile every runtime
# source for the iPhoneOS and the iPhoneSimulator SDK (tools/ios/mac/sdk_check.sh, under the shared lock).
# UNRUN: never executed (see tools/ios/common.sh).  Refuses to start without KHNMC_IOS_MACBOOK_OK=1.
#   tools/ios/check.sh            logs come back to run-J26/ios/sdk-check-<sdk>.log
set -u
. "$(dirname "$0")/common.sh"
ios_guard
"$IOS_HERE/tools/ios/sync.sh" src || exit 1
mac_step "khnmc iOS SDK compile check" sdk_check.sh; rc=$?; busy_note $rc
mkdir -p "$IOS_HERE/run-J26/ios"
rsync -a -z -e "$IOS_RSH" --include 'sdk-check-*.log' --exclude '*' "$IOS_MAC:$IOS_RDIR/run/" "$IOS_HERE/run-J26/ios/" || echo "tools/ios: could not fetch the logs" >&2
exit $rc
