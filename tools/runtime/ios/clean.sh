#!/usr/bin/env bash
# tools/ios/clean.sh [objects|all] : last step of the MacBook Air plan, from this Linux machine: remove what is no longer needed
# in ~/Developer/KHNMC there and print what is left (tools/ios/mac/cleanup.sh, under the shared lock; it also deletes a simulator
# of ours that a broken run left behind).  `all` keeps only the logs; to remove those too, and the then empty folder, the owner
# runs `rm -rf ~/Developer/KHNMC` himself.
# UNRUN: never executed (see tools/ios/common.sh).  Refuses to start without KHNMC_IOS_MACBOOK_OK=1.
set -u
. "$(dirname "$0")/common.sh"
ios_guard
mode=${1:-objects}; case "$mode" in objects|all) ;; *) sed -n '2,6p' "$0" >&2; exit 2;; esac
mac_step "khnmc iOS cleanup" cleanup.sh "$mode"; rc=$?; busy_note $rc
exit $rc
