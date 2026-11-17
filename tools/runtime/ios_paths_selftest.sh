#!/usr/bin/env bash
# tools/ios_paths_selftest.sh : `--selftest` of a KH_IOS_PATHS build (make IOS_PATHS=1) the way an app would run it, on this host.
#
#   tools/ios_paths_selftest.sh BIN EXE OUTDIR [NICE]
#
# BIN = ./khnmc-iospaths, EXE = kh2fm.exe, OUTDIR = a directory of this run (created), NICE = nice level (default 10).
# Builds OUTDIR/home as the app container: Documents/kh2fm.exe (a link to EXE), Documents/khnmc.conf (`arg --selftest`, a log
# file and a status file), tmp/ ; then starts BIN with an EMPTY environment except PATH, KH_APP_HOME and KH_TMPDIR (an app has no
# caller: what the run does must come from the conf and the container defaults), from OUTDIR as working directory.
# Afterwards it checks what only this kind of run can show:
#   * the status file says `exit N` with the N the process really ended with;
#   * the scratch directory is empty again (no fixture left behind), and nothing appeared in the launch directory;
#   * the log says which directories were used.
# Output: OUTDIR/selftest.out (the launch output, then the log file, then `selftest exit=N`), for tools/selftest_table.py.
# Exit status: 0 only when the selftest passed AND these checks hold.  OUTDIR/home is removed first only when an earlier run of this
# script made it (marker file).
set -u
[ $# -ge 3 ] || { sed -n '2,16p' "$0" >&2; exit 2; }
abspath(){ case "$1" in /*) printf '%s\n' "$1";; *) printf '%s/%s\n' "$PWD" "$1";; esac; }
BIN=$(abspath "$1"); EXE=$(abspath "$2"); OUT=$(abspath "$3"); NICE=${4:-10}
[ -x "$BIN" ] && [ -f "$EXE" ] || { echo "ios_paths_selftest: missing $BIN or $EXE" >&2; exit 2; }
case "$NICE" in ''|*[!0-9]*) echo "ios_paths_selftest: NICE must be a number" >&2; exit 2;; esac
H="$OUT/home"
if [ -e "$H" ]; then [ -f "$H/.khnmc-ios-paths-home" ] || { echo "ios_paths_selftest: $H exists and is not from this script: not touching it" >&2; exit 2; }; rm -rf "$H"; fi
mkdir -p "$H/Documents" "$H/tmp" || exit 2
: > "$H/.khnmc-ios-paths-home"
ln -s "$EXE" "$H/Documents/kh2fm.exe"
cat > "$H/Documents/khnmc.conf" <<CONF
# written by tools/ios_paths_selftest.sh: the selftest, the way the app shell would be told to run it
log selftest.log
status selftest.status
arg --selftest
CONF
before=$(cd "$OUT" && ls -A | grep -v -e '^launch\.out$' -e '^selftest\.out$' | LC_ALL=C sort | tr '\n' ' ')
( cd "$OUT" && env -i PATH=/usr/bin:/bin KH_APP_HOME="$H" KH_TMPDIR="$H/tmp" nice -n "$NICE" "$BIN" > "$OUT/launch.out" 2>&1 ); rc=$?
after=$(cd "$OUT" && ls -A | grep -v -e '^launch\.out$' -e '^selftest\.out$' | LC_ALL=C sort | tr '\n' ' ')
{ cat "$OUT/launch.out"; [ -f "$H/Documents/selftest.log" ] && cat "$H/Documents/selftest.log"; echo "selftest exit=$rc"; } > "$OUT/selftest.out"
bad=0
st=$(cat "$H/Documents/selftest.status" 2>/dev/null)
if [ "$st" = "exit $rc" ]; then echo "ios_paths_selftest: status file: '$st' = the process exit status"; else echo "ios_paths_selftest: FAIL: status file says '${st:-<missing>}', the process ended with $rc" >&2; bad=1; fi
left=$(ls -A "$H/tmp" | tr '\n' ' ')
if [ -z "$left" ]; then echo "ios_paths_selftest: scratch directory $H/tmp is empty after the run"; else echo "ios_paths_selftest: FAIL: left in the scratch directory: $left" >&2; bad=1; fi
if [ "$before" = "$after" ]; then echo "ios_paths_selftest: nothing new in the launch directory (only launch.out and selftest.out, written by this script)"; else echo "ios_paths_selftest: FAIL: the launch directory changed: before [$before] after [$after]" >&2; bad=1; fi
[ -s "$OUT/launch.out" ] && { echo "ios_paths_selftest: NOTE: $(wc -l < "$OUT/launch.out") lines went to the launch output instead of the log file (before the conf's log line took effect):"; sed -n '1,5p' "$OUT/launch.out"; }
grep -a -e "^kh: host " -e "NOT RUN:" -e "host env selftest" "$OUT/selftest.out" | cut -c1-260
[ "$rc" = 0 ] || { echo "ios_paths_selftest: FAIL: selftest exit status $rc" >&2; bad=1; }
exit $bad
