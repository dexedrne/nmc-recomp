#!/usr/bin/env bash
# tools/darwin_queue.sh : the Mac mini steps of task 26 one after another, from this worktree (the build itself is
# tools/darwin_build.sh and comes first, by hand):
#   selftest; one field route per difficulty 0..3 (runtime/task20-field-input.txt, 7,000 game frames); one route with the software
#   mixer writing a WAV on the virtual frame clock (KHNMC_AUDIO=wav:..., KHNMC_CLOCK=frames) and its hashes; then the KH_IOS_PATHS
#   variant: build (IOS_PATHS=1: all + hostshell), its selftest started the way an app is, the same through the main-owning shell
#   (khnmc-iospaths-shell), and one route.
#   tools/darwin_queue.sh [STEP ...]     only these steps (names as in the list at the end of this file)
# The Mac mini is shared: every step reads `memory_pressure` first and refuses to start below KHNMC_MAC_MIN_FREE; a refused step is
# tried again every 60 s, up to 45 times, then the queue says GAVE UP for it and goes on.  Every wait is in the log.
# Progress: $RUN/mac-queue.log; per step $RUN/mac-q-<step>.log; results as tools/darwin_route.sh leaves them ($RUN/mac-*).
cd "$(dirname "$0")/.." || exit 1
RUN=${KHNMC_RUN_DIR:-$PWD/run-J26}; mkdir -p "$RUN"; LOG=$RUN/mac-queue.log
IN=runtime/task20-field-input.txt
step(){ local name=$1; shift; local tries=0 rc
  while :; do "$@" > "$RUN/mac-q-$name.log" 2>&1; rc=$?
    if [ $rc -eq 3 ] || grep -q "not starting\|giving up, exit 3" "$RUN/mac-q-$name.log"; then tries=$((tries+1))
      [ $tries -ge 45 ] && { echo "$(date +%T) $name: GAVE UP after $tries refusals (memory)" >> "$LOG"; return 3; }
      [ $tries -eq 1 ] && echo "$(date +%T) $name: refused (memory): $(grep -m1 'memory free' "$RUN/mac-q-$name.log" | cut -c1-120); waiting" >> "$LOG"
      sleep 60; continue; fi
    echo "$(date +%T) $name: rc=$rc (after $tries refusals, i.e. $tries min of waiting); $(grep -m1 'memory free' "$RUN/mac-q-$name.log" | cut -c1-110)" >> "$LOG"; return $rc; done; }
run_step(){ case "$1" in
  selftest)      step selftest tools/darwin_route.sh selftest j26;;
  route-d0|route-d1|route-d2|route-d3) d=${1#route-d}; step "$1" tools/darwin_route.sh run j26-d$d --difficulty "$d" --input $IN;;
  wav)           step wav tools/darwin_route.sh run j26-wav --input $IN --env KHNMC_AUDIO=wav:@OUT@/audio.wav --env KHNMC_CLOCK=frames
                 step wavsum tools/darwin_route.sh wavsum j26-wav;;
  wav2)          step wav2 tools/darwin_route.sh run j26-wav2 --input $IN --env KHNMC_AUDIO=wav:@OUT@/audio.wav --env KHNMC_CLOCK=frames
                 step wavsum2 tools/darwin_route.sh wavsum j26-wav2;;
  ios-build)     MAKE_ARGS=IOS_PATHS=1 step ios-build tools/darwin_build.sh all hostshell;;
  ios-selftest)  step ios-selftest tools/darwin_route.sh ios-selftest j26;;
  ios-shell)     KHNMC_IOS_BIN=./khnmc-iospaths-shell step ios-shell tools/darwin_route.sh ios-selftest j26-shell;;
  ios-route)     step ios-route tools/darwin_route.sh ios-run j26-ios-d1 --difficulty 1 --input $IN;;
  *) echo "darwin_queue: unknown step $1" >&2; return 2;; esac; }
[ $# -gt 0 ] || set -- selftest route-d0 route-d1 route-d2 route-d3 wav ios-build ios-selftest ios-shell ios-route
echo "$(date +%T) queue start: $*  (nice ${KHNMC_MAC_NICE:-10}, start at >= ${KHNMC_MAC_MIN_FREE:-30}% free, jobs ${JOBS:-4})" >> "$LOG"
for s in "$@"; do run_step "$s"; done
echo "$(date +%T) queue done" >> "$LOG"
