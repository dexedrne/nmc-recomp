#!/usr/bin/env bash
# tools/darwin_queue.sh : the Mac mini steps of task 20 one after another, from this worktree: build, selftest, one field route
# per difficulty setting (unset, 0..3) with the task20 input script, the float probe with the libm replay, and one route with the
# original task6 script (expected: DIVERGED on the M4, see implementation notes 3.2).  The Mac mini is shared: every step refuses to
# start below 30 % free memory; such a step is retried every 60 s, up to 70 times, then the queue says GAVE UP and goes on.
# Progress: run-F20/mac-queue.log; per step run-F20/mac-q-<step>.log; results as tools/darwin_route.sh leaves them (run-F20/mac-*).
cd "$(dirname "$0")/.." || exit 1
LOG=run-F20/mac-queue.log; : > $LOG
step(){ local name=$1; shift; local tries=0 rc
  while :; do "$@" > run-F20/mac-q-$name.log 2>&1; rc=$?
    if [ $rc -eq 3 ] || grep -q "below 30 %\|is below 30%\|not starting" run-F20/mac-q-$name.log; then tries=$((tries+1)); [ $tries -ge 70 ] && { echo "$(date +%T) $name: GAVE UP after $tries refusals (memory)" >> $LOG; return 3; }; sleep 60; continue; fi
    echo "$(date +%T) $name: rc=$rc (after $tries refusals)" >> $LOG; return $rc; done; }
step build tools/darwin_build.sh || exit 1
step selftest tools/darwin_route.sh selftest final
step route-none tools/darwin_route.sh run f-none --input runtime/task20-field-input.txt --env KH_IMPORT_COUNTS=1 --env KHNMC_GFX_PSOLOG=1
for d in 0 1 2 3; do step route-d$d tools/darwin_route.sh run f-d$d --difficulty $d --input runtime/task20-field-input.txt --env KH_IMPORT_COUNTS=1 --env KHNMC_GFX_PSOLOG=1; done
step float tools/float_probe.sh run-F20/float run-F20/libm-args-route.bin
step route-task6 tools/darwin_route.sh run f-task6 --env KH_IMPORT_COUNTS=1
echo "$(date +%T) queue done" >> $LOG
