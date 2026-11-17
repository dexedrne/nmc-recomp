#!/usr/bin/env bash
# tools/ios/sim.sh selftest | route [none|0|1|2|3] : step 4 of the MacBook Air plan, from this Linux machine: copy the exe (and
# for a route the data subset), then run the app inside an iOS Simulator of our own (tools/ios/mac/sim_run.sh, under the shared
# lock: mute, create "KHNMC Test", boot headless, install, launch, wait for the status file, collect, uninstall, shut down,
# delete).  The results come back to run-J26/ios/sim-<mode>-<time>/ (run.out, khnmc.status, phase.log, files.log, steps.log).
# UNRUN: never executed (see tools/ios/common.sh).  Refuses to start without KHNMC_IOS_MACBOOK_OK=1.
# After a selftest:  python3 tools/selftest_table.py linux=run-J26/linux-iospaths-selftest/selftest.out sim=run-J26/ios/sim-selftest-*/run.out
# (the Linux file is the KH_IOS_PATHS build's selftest: same fatal cases NOT RUN, so every suite must have the same count.)
set -u
. "$(dirname "$0")/common.sh"
ios_guard
mode=${1:-}; diff=${2:-none}
case "$mode" in selftest) "$IOS_HERE/tools/ios/sync.sh" exe || exit 1;; route) "$IOS_HERE/tools/ios/sync.sh" all || exit 1;; *) sed -n '2,9p' "$0" >&2; exit 2;; esac
mac_step "khnmc iOS simulator $mode" sim_run.sh "$mode" "$diff"; rc=$?; busy_note $rc
mkdir -p "$IOS_HERE/run-J26/ios"
rsync -a -z -e "$IOS_RSH" --include 'sim-*/' --include 'sim-*/*' --exclude '*' "$IOS_MAC:$IOS_RDIR/run/" "$IOS_HERE/run-J26/ios/" || echo "tools/ios: could not fetch the results" >&2
exit $rc
