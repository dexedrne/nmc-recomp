#!/bin/bash
# tools/darwin_remote_build.sh : the part of tools/darwin_build.sh that runs ON the Mac mini, inside the build directory.
#
#   bash tools/darwin_remote_build.sh JOBS NICE MIN_FREE STOP_FREE CHUNK SHARDS_DIR MAKE_ARGS TARGET...
#
# On shared desktops the build is paced by `memory_pressure`:
#   * nothing starts while "System-wide memory free percentage" is below MIN_FREE (exit 3: the caller decides when to try again);
#   * the 117 lifted shards are compiled CHUNK at a time (make -jJOBS under nice -n NICE); the reading is taken again before every
#     chunk and printed.  Below STOP_FREE the build STOPS and waits (one reading a minute, at most WAIT_MIN minutes, default 45)
#     until the reading is back at MIN_FREE or above, and says so in a line that starts with `darwin_build: WAITED`; if it does not
#     come back in that time the script ends with exit 3 and the objects made so far stay (the next call continues from them);
#   * then the requested targets (default: all = link + the link checks) with the same rule.
# With TARGET = rtobjs only the hand-written runtime and lifter-rt objects are compiled, with make -k (every error of a port in
# one pass), and no shard.  Everything is inside the current directory (~/khnmc-work/build-*); nothing is deleted.
set -u
[ $# -ge 8 ] || { sed -n '2,16p' "$0" >&2; exit 2; }
JOBS=$1 NICE=$2 MIN=$3 STOP=$4 CHUNK=$5 SHARDS=$6 MARGS=$7; shift 7; TARGETS="$*"
WAIT_MIN=${WAIT_MIN:-45}
for v in "$JOBS" "$NICE" "$MIN" "$STOP" "$CHUNK"; do case "$v" in ''|*[!0-9]*) echo "darwin_build: numeric argument expected, got '$v'" >&2; exit 2;; esac; done
case "$PWD" in "$HOME"/khnmc-work/build-*) ;; *) echo "darwin_build: must run inside ~/khnmc-work/build-* (here: $PWD)" >&2; exit 2;; esac
memfree(){ memory_pressure | tail -1 | sed -n 's/.*percentage: \([0-9]*\)%.*/\1/p'; }
# gate WHAT : print the reading; below STOP wait for MIN; unreadable = stop (never start a heavy step blind)
gate(){
  local f; f=$(memfree)
  [ -n "$f" ] || { echo "darwin_build: cannot read memory_pressure before $1: not starting" >&2; exit 3; }
  echo "darwin_build: memory free ${f}% before $1 (load $(sysctl -n vm.loadavg))"
  if [ "$f" -lt "$STOP" ]; then
    local t0=$SECONDS n=0
    while [ "$f" -lt "$MIN" ]; do
      n=$((n+1)); [ $n -gt "$WAIT_MIN" ] && { echo "darwin_build: WAITED $(( (SECONDS-t0)/60 )) min before $1: memory free still ${f}% (below ${MIN}%): giving up, exit 3" >&2; exit 3; }
      sleep 60; f=$(memfree); [ -n "$f" ] || f=0
    done
    echo "darwin_build: WAITED $(( (SECONDS-t0)/60 )) min before $1: memory free had fallen below ${STOP}%, now ${f}%"
  fi
}
f=$(memfree)
echo "darwin_build: $(sw_vers -productVersion) $(uname -m), $(clang --version | head -1), memory free ${f:-?}%, load $(sysctl -n vm.loadavg); rule: start at >= ${MIN}%, stop below ${STOP}%, $JOBS jobs, nice -n $NICE, $CHUNK shards per reading"
[ -n "$f" ] && [ "$f" -ge "$MIN" ] || { echo "darwin_build: memory free percentage ${f:-?}% is below ${MIN}% (or unreadable): not starting the build" >&2; exit 3; }
MK=(nice -n "$NICE" make "SHARDS_DIR=$SHARDS")
# shellcheck disable=SC2206
[ -n "$MARGS" ] && MK+=($MARGS)
if [ "$TARGETS" = rtobjs ]; then
  "${MK[@]}" -k -j"$JOBS" rtobjs; rc=$?
  echo "darwin_build: make -k rtobjs: exit $rc"; exit $rc
fi
OL=$("${MK[@]}" -s print-OL) && [ -n "$OL" ] || { echo "darwin_build: cannot read OL from the Makefile" >&2; exit 1; }
BIN=$("${MK[@]}" -s print-BIN) && [ -n "$BIN" ] || { echo "darwin_build: cannot read BIN from the Makefile" >&2; exit 1; }
"${MK[@]}" "$OL/shards/.stamp" || { echo "darwin_build: the renamed shard copies could not be generated" >&2; exit 1; }
names=$(cd "$SHARDS" && ls shard_*.c | sed 's/\.c$//'); total=$(echo "$names" | wc -l | tr -d ' ')
# shellcheck disable=SC2086
set -- $names
k=0; done_n=0
while [ $# -gt 0 ]; do
  chunk=""; i=0
  while [ $# -gt 0 ] && [ $i -lt "$CHUNK" ]; do chunk="$chunk $OL/lifted/$1.o"; shift; i=$((i+1)); done_n=$((done_n+1)); done
  k=$((k+1))
  # shellcheck disable=SC2086
  if "${MK[@]}" -q $chunk 2>/dev/null; then echo "darwin_build: shards up to $done_n of $total: objects are up to date"; continue; fi
  gate "shards up to $done_n of $total"
  # shellcheck disable=SC2086
  "${MK[@]}" -j"$JOBS" $chunk || { echo "darwin_build: a shard failed to compile (chunk $k)" >&2; exit 1; }
done
gate "the runtime objects and the link"
# shellcheck disable=SC2086
"${MK[@]}" -j"$JOBS" $TARGETS; rc=$?
if [ $rc -eq 0 ] && [ -x "$BIN" ]; then ls -l "$BIN"; echo "darwin_build: $BIN sha256 $(shasum -a 256 "$BIN" | cut -c1-16)"; file "$BIN"; fi
f=$(memfree); echo "darwin_build: make $TARGETS: exit $rc; memory free ${f:-?}% after the build"
exit $rc
