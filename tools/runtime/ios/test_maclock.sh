#!/usr/bin/env bash
# tools/ios/test_maclock.sh : the lock wrapper's own logic against a scratch lock file, on this machine (Linux or macOS).
#   tools/ios/test_maclock.sh SCRATCH_DIR      (an existing empty directory; nothing outside it is touched)
# Cases: a free lock (no file / an EMPTY file = the leftover of an fcntl-style user) runs the command, holds our line in the
# file while it runs and removes the file afterwards; a description that is not ours = busy (75), file untouched; an fcntl
# lock held by another process = busy (75); our own stale line with no holder = taken over; the command's exit status is
# passed on; SIGTERM to the wrapper reaches the command's process group and the lock is released.
# It does not test the Mac's rule that lockf and flock exclude each other (measured there, see maclock.py), nor pgrep on a Mac.
set -u
D=${1:-}; [ -n "$D" ] && [ -d "$D" ] && [ -z "$(ls -A "$D")" ] || { echo "usage: $0 EMPTY_SCRATCH_DIR" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd); L="$D/lock"; export KHNMC_MACLOCK_FILE="$L"
ok=0; bad=0; t(){ if eval "$2"; then ok=$((ok+1)); else bad=$((bad+1)); echo "test_maclock: FAIL: $1" >&2; fi; }
W(){ python3 "$HERE/maclock.py" "$@"; }
W --what "t1" -- sh -c "cat '$L' > '$D/seen'"; rc=$?
t "free lock (no file): runs, exit 0" '[ $rc = 0 ]'
t "our line was in the file while the command ran" 'grep -q "^khnmc iOS build, started 20.*(t1)$" "$D/seen"'
t "the file is removed afterwards" '[ ! -e "$L" ]'
: > "$L"; W -- true; rc=$?
t "an EMPTY existing file means free" '[ $rc = 0 ] && [ ! -e "$L" ]'
W -- sh -c 'exit 7'; rc=$?
t "the exit status of the command is passed on" '[ $rc = 7 ]'
echo "someone else building the other app" > "$L"; W -- touch "$D/ran-busy" 2> "$D/err"; rc=$?
t "a description that is not ours: 75, nothing started" '[ $rc = 75 ] && [ ! -e "$D/ran-busy" ] && grep -q "BUSY" "$D/err"'
t "the foreign description is untouched" '[ "$(cat "$L")" = "someone else building the other app" ]'
rm -f "$L"; : > "$L"
python3 -c "import fcntl,os,sys,time; fd=os.open(sys.argv[1],os.O_RDWR); fcntl.lockf(fd,fcntl.LOCK_EX); open(sys.argv[2],'w').close(); time.sleep(8)" "$L" "$D/holder-ready" & hp=$!
for i in $(seq 50); do [ -e "$D/holder-ready" ] && break; sleep 0.1; done
W -- touch "$D/ran-locked" 2> "$D/err2"; rc=$?
t "an fcntl lock held by another process: 75, nothing started" '[ $rc = 75 ] && [ ! -e "$D/ran-locked" ] && grep -q "locked by another process" "$D/err2"'
kill $hp 2>/dev/null; wait $hp 2>/dev/null
echo "khnmc iOS build, started 2026-01-01T00:00:00Z (stale)" > "$L"; W -- true 2> "$D/err3"; rc=$?
t "our own stale line without a holder: taken over" '[ $rc = 0 ] && grep -q "taking over" "$D/err3" && [ ! -e "$L" ]'
# the wrapper itself in the background (not the shell function: $! must be the wrapper's pid)
python3 "$HERE/maclock.py" -- sh -c "trap 'echo cleaned > \"$D/cleaned\"; exit 0' TERM; echo \$\$ > \"$D/child\"; while :; do sleep 0.2; done" 2> "$D/err4" & wp=$!
for i in $(seq 50); do [ -e "$D/child" ] && break; sleep 0.1; done
t "the lock file exists while the long command runs" '[ -s "$L" ]'
kill -TERM $wp; wait $wp; rc=$?
t "SIGTERM: the command's own trap ran" '[ -e "$D/cleaned" ]'
t "SIGTERM: exit 143 and the lock is released" '[ $rc = 143 ] && [ ! -e "$L" ]'
W 2>/dev/null; rc=$?
t "usage error is 2" '[ $rc = 2 ]'
echo "test_maclock: $ok passed, $bad failed"
[ $bad = 0 ]
