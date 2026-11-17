#!/usr/bin/env bash
# tools/route_batch.sh : run N headless field routes, each with its own state dirs, and summarise exits and counters.
#
#   tools/route_batch.sh [-n RUNS] [-j PARALLEL] [-s SECONDS] [-o OUTDIR] [-d none|cycle|0..3] [-i INPUT_SCRIPT]
#                        [-g GAME_FRAMES] [-c CPUS_PER_RUN] [-w "WRAPPER CMD"] [-e VAR=VALUE]... [-m MIN_AVAILABLE_MB] [-b BINARY]
#                        [-r field|none] [-t] [-k]
#
#   -n  number of runs (default 8)                 -j  runs in parallel (default 4; more than 4 is refused: live desktop machine)
#   -s  KH_RUN_SECONDS per run (default 120)       -o  output directory (default run-batch/<date>-<pid>)
#   -g  end every run after GAME_FRAMES frames of the game thread (KH_RUN_GAME_FRAMES): the same route length on a loaded and on
#       an idle machine; -s is then only the time limit (give it room: a loaded machine makes 20 game frames per second, not 60)
#   -d  KHNMC_DIFFICULTY: none = not set (default), cycle = run index mod 4, or a fixed 0..3
#   -i  KHNMC_INPUT_SCRIPT (default runtime/task6-field-input.txt)
#   -c  pin every run to its own block of CPUS_PER_RUN cpus (taskset; parallel slot k gets cpus k*C .. k*C+C-1): a stress
#       variant that forces the guest threads of one run to share few cpus (0 = no pinning, default)
#   -w  command prefix for every run, e.g. -w "nice -n 10"
#   -e  extra environment for every run (repeatable), e.g. -e KHNMC_BGLOAD_CHAOS=7
#   -m  MemAvailable (MB) required per run of a wave (default 1500): a wave starts as many runs as fit, waits up to 600 s for
#       room for one, then stops loudly; it never skips silently
#   -b  binary to test (default ./khnmc); it is copied into OUTDIR first, so rebuilding during a batch cannot mix binaries
#   -r  what a run must have reached to count as ok: field (default; the phase log shows FIELD_COMMAND.execute calls) or none
#       (for runs shorter than the route).  A run that exits 0 without reaching it is DIVERGED and counts as a failure: the
#       scripted input is clocked by presents, and a route that never leaves the settings menu tests nothing
#   -t  also write an import trace (KH_TRACE, first 50,000 calls)            -k  keep the binary copy even when all runs pass
#
# Per run R (r01, r02, ...): OUTDIR/R/{root,docs}/ (fresh KH_ROOT and KH_DOCS), phase.log, files.log, run.out, exit, time.
# Result: OUTDIR/summary.tsv + a printed table.  Exit status 0 only when every run exited 0 and reached the required point; 1 when
# any run failed (non-zero exit, signal, watchdog timeout, diverged route) or the batch was cut short; 2 for usage/setup errors.
# Nothing is retried.  Columns: gframes = frames the game thread made, late = presents by which the script's @hold delayed the
# input (- = no hold line or not released), wall_s / cpu_s / rss_mb = wall time, cpu time and peak RSS of the run.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
N=8; J=4; SECS=120; GFR=0; OUT=""; DIFF=none; INPUT="$HERE/runtime/task6-field-input.txt"; WRAP=""; MINMB=1500; TRACE=0; PIN=0; KEEP=0; REQ=field
BIN="$HERE/khnmc"; EXTRA=()
while getopts "n:j:s:g:o:d:i:c:w:e:m:b:r:tk" o; do case "$o" in
  n) N=$OPTARG;; j) J=$OPTARG;; s) SECS=$OPTARG;; g) GFR=$OPTARG;; o) OUT=$OPTARG;; d) DIFF=$OPTARG;; i) INPUT=$OPTARG;; c) PIN=$OPTARG;;
  w) WRAP=$OPTARG;; e) EXTRA+=("$OPTARG");; m) MINMB=$OPTARG;; b) BIN=$OPTARG;; r) REQ=$OPTARG;; t) TRACE=1;; k) KEEP=1;;
  *) sed -n '2,30p' "$0" >&2; exit 2;; esac; done
case "$N$J$SECS$MINMB$PIN$GFR" in *[!0-9]*) echo "route_batch: -n -j -s -g -m -c take numbers" >&2; exit 2;; esac
[ "$N" -ge 1 ] && [ "$J" -ge 1 ] || { echo "route_batch: -n and -j must be at least 1" >&2; exit 2; }
[ "$J" -le 4 ] || { echo "route_batch: refusing -j $J (at most 4 routes in parallel on this machine)" >&2; exit 2; }
[ $((J*PIN)) -le "$(nproc)" ] || { echo "route_batch: -j $J x -c $PIN needs more cpus than the $(nproc) present" >&2; exit 2; }
case "$DIFF" in none|cycle|0|1|2|3) ;; *) echo "route_batch: -d takes none, cycle or 0..3" >&2; exit 2;; esac
case "$REQ" in field|none) ;; *) echo "route_batch: -r takes field or none" >&2; exit 2;; esac
EXE="$HERE/pc/kh2fm.exe"
for f in "$BIN" "$EXE" "$INPUT" "$HERE/data-full" "$HERE/pkgdir"; do [ -e "$f" ] || { echo "route_batch: missing $f" >&2; exit 2; }; done
[ -n "$OUT" ] || OUT="$HERE/run-batch/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$OUT" || exit 2; OUT=$(cd "$OUT" && pwd)
cp -f "$BIN" "$OUT/khnmc.bin" || { echo "route_batch: cannot copy $BIN" >&2; exit 2; }
BINSUM=$(sha256sum "$OUT/khnmc.bin" | cut -c1-16)
avail_mb(){ awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo; }
echo "route_batch: $N runs, $J parallel, ${SECS}s each, game frames=$GFR, difficulty=$DIFF, cpus/run=$PIN, must reach=$REQ, wrapper='${WRAP}', extra env='${EXTRA[*]:-}'"
echo "route_batch: binary $BIN sha256 $BINSUM..., out=$OUT, MemAvailable $(avail_mb) MB (need $MINMB MB per started run)"

# child cpu seconds (user, system) and peak RSS in MB into the file named by argv[1]; exit status = the command's (128+n for signal n)
RUSAGE_PY='import sys,resource,subprocess
import time; t0=time.time(); r=subprocess.call(sys.argv[2:]); u=resource.getrusage(resource.RUSAGE_CHILDREN)
open(sys.argv[1],"w").write("%.0f %.0f %d %.0f\n"%(u.ru_utime,u.ru_stime,u.ru_maxrss//1024,time.time()-t0)); sys.exit(r if r>=0 else 128-r)'
one_run(){   # $1 = run index (1-based), $2 = parallel slot (0-based)
  local i=$1 slot=$2 tag d pin=(); tag=$(printf 'r%02d' "$i"); d="$OUT/$tag"
  rm -rf "$d"; mkdir -p "$d/root" "$d/docs" || { mkdir -p "$d"; echo 97 > "$d/exit"; return; }
  local envv=( "KH_EXE=$EXE" "KH_ROOT=$d/root" "KH_DOCS=$d/docs" "KHNMC_DATA=$HERE/data-full" "KHNMC_PKGDIR=$HERE/pkgdir"
               "KHNMC_INPUT_SCRIPT=$INPUT" "KH_RUN_SECONDS=$SECS" "KH_DUMP_THREADS=1"
               "KHNMC_PHASELOG=$d/phase.log" "KHNMC_FILELOG=$d/files.log" )
  case "$DIFF" in none) ;; cycle) envv+=("KHNMC_DIFFICULTY=$(( (i-1) % 4 ))");; *) envv+=("KHNMC_DIFFICULTY=$DIFF");; esac
  [ "$TRACE" = 1 ] && envv+=("KH_TRACE=$d/trace.txt" "KH_TRACE_MAX=50000")
  [ "$GFR" -gt 0 ] && envv+=("KH_RUN_GAME_FRAMES=$GFR")
  [ "${#EXTRA[@]}" -gt 0 ] && envv+=("${EXTRA[@]}")
  [ "$PIN" -gt 0 ] && pin=(taskset -c "$((slot*PIN))-$((slot*PIN+PIN-1))")
  printf '%s\n' "${envv[@]}" "wrapper=${pin[*]:-} $WRAP" > "$d/env"
  # watchdog: a run that is still alive 90 s after its own time limit is a hang (exit 124), reported as a failure
  # shellcheck disable=SC2086
  ( cd "$d/root" && env "${envv[@]}" python3 -c "$RUSAGE_PY" "$d/time" timeout -k 10 $((SECS+90)) "${pin[@]}" $WRAP "$OUT/khnmc.bin" > "$d/run.out" 2>&1 ); echo $? > "$d/exit"
}

started=0; cut=0
while [ "$started" -lt "$N" ]; do            # waves of up to J runs: a slot (and its cpu block) is never shared by two live runs
  wave=$((N-started)); [ "$wave" -le "$J" ] || wave=$J
  waited=0
  while fit=$(( $(avail_mb) / MINMB )); [ "$fit" -lt 1 ]; do      # as many runs of the wave as memory allows, at least one
    [ "$waited" -lt 600 ] || { echo "route_batch: MemAvailable $(avail_mb) MB stayed under $MINMB MB; stopping after $started of $N runs" >&2; cut=1; break 2; }
    sleep 10; waited=$((waited+10))
  done
  [ "$fit" -ge "$wave" ] || { echo "route_batch: MemAvailable $(avail_mb) MB: this wave runs $fit in parallel instead of $wave"; wave=$fit; }
  for slot in $(seq 0 $((wave-1))); do started=$((started+1)); one_run "$started" "$slot" & sleep 2; done   # staggered starts
  wait
done
[ "$started" -ge 1 ] || { echo "route_batch: no run was started" >&2; exit 1; }

# ---- summary ---------------------------------------------------------------------------------------------------------------
num(){ sed -n "s/.*[ =:]$2=\([0-9][0-9]*\).*/\1/p" <<<"$1" | tail -1; }
S="$OUT/summary.tsv"; fails=0; diverged=0
printf 'run\texit\tresult\tpresents\tgframes\tdraws\tpsos\tresources\tfile_hits\tfile_miss\tfield_cmds\tlate\twall_s\tcpu_s\trss_mb\tsignature\n' > "$S"
for i in $(seq 1 "$started"); do
  tag=$(printf 'r%02d' "$i"); d="$OUT/$tag"; ex=$(cat "$d/exit" 2>/dev/null || echo missing); o="$d/run.out"
  gl=$(grep -a 'kh: gfx: draws=' "$o" 2>/dev/null | grep -av 'gfx frame' | tail -1)
  [ -n "$gl" ] || gl=$(grep -a 'gfx frame=' "$o" 2>/dev/null | tail -1)       # failed runs: the last per-frame line
  pres=$(num "$gl" presents); draws=$(num "$gl" draws); psos=$(num "$gl" psos); res=$(num "$gl" resources)
  hits=$(grep -ac '^HIT ' "$d/files.log" 2>/dev/null); miss=$(grep -ac '^MISS ' "$d/files.log" 2>/dev/null)
  cpu=$(awk 'END{if(NF>=3) printf "%d", $1+$2}' "$d/time" 2>/dev/null); rss=$(awk 'END{if(NF>=3) printf "%d", $3}' "$d/time" 2>/dev/null)
  fld=$(grep -ao 'FIELD_COMMAND.execute=[0-9]*' "$d/phase.log" 2>/dev/null | awk -F= '{s+=$2} END{print s+0}')
  late=$(grep -a -m1 'scripted XInput: hold at script frame' "$o" 2>/dev/null | sed -n 's/.*script delayed by \([0-9]*\) presents.*/\1/p')
  gfr=$(grep -a 'kh: input report:' "$o" 2>/dev/null | tail -1 | sed -n 's/.* game-frames=\([0-9]*\).*/\1/p'); wall=$(awk 'END{if(NF>=4) printf "%d", $4}' "$d/time" 2>/dev/null)
  sig=""; result=ok
  if [ "$ex" = 0 ] && [ "$REQ" = field ] && [ "${fld:-0}" -eq 0 ]; then
    fails=$((fails+1)); diverged=$((diverged+1)); result=DIVERGED
    sig="exit 0 but the route never reached the field | last $(grep -a 'InitialSetting frame=' "$o" 2>/dev/null | tail -1 | sed 's/^kh: //')"
  elif [ "$ex" != 0 ]; then
    fails=$((fails+1)); result=FAIL
    line=$(grep -a -m1 -E '^kh: SIGNAL |^kh: FATAL: |^kh_fault: ' "$o" 2>/dev/null)
    frames=$(awk '/^---- host backtrace ----/{b=1;next} b' "$o" 2>/dev/null | grep -ao '(f_[0-9a-f]*+0x[0-9a-f]*)' | head -4 | tr -d '()' | paste -sd'<' -)
    grip=$(grep -a -m1 -o 'rip(state)=[0-9a-f]*' "$o" 2>/dev/null)
    [ "$ex" = 124 ] && line="TIMEOUT (watchdog: still running $((SECS+90)) s after start)"
    [ "$ex" = 137 ] && line="KILLED (SIGKILL: watchdog or out of memory)"
    sig="${line:-no report line in run.out} | ${grip:-} | ${frames:-no lifted frame}"
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$tag" "$ex" "$result" "${pres:--}" "${gfr:--}" "${draws:--}" "${psos:--}" "${res:--}" "${hits:--}" "${miss:--}" "${fld:--}" "${late:--}" "${wall:--}" "${cpu:--}" "${rss:--}" "$sig" >> "$S"
done
column -t -s "$(printf '\t')" "$S" 2>/dev/null || cat "$S"
grep -ah '^kh: bgload' "$OUT"/r*/run.out 2>/dev/null | grep -a 'report' | sort | uniq -c | sort -rn | head -3
# object-id table (64 slots) and the id zone's high-water mark of every run: the numbers task 17 keeps bounded
for i in $(seq 1 "$started"); do tag=$(printf 'r%02d' "$i"); o="$OUT/$tag/run.out"
  printf '%s  %s | %s\n' "$tag" "$(grep -a '^kh: object-id table' "$o" 2>/dev/null | tail -1 | sed 's/^kh: //')" "$(grep -a '^kh: guest arena: id zone' "$o" 2>/dev/null | tail -1 | sed -n 's/.*high-water \(0x[0-9a-f]*\) = \([0-9]* MiB\).*in use \([0-9]* MiB\).*/id zone high-water \2 (\1), in use \3/p')"
done
echo "route_batch: $started of $N runs made, $((started-fails)) ok, $fails failed of which $diverged diverged (binary sha256 $BINSUM...); details in $OUT (summary.tsv, rNN/run.out)"
if [ "$fails" -eq 0 ] && [ "$cut" -eq 0 ]; then [ "$KEEP" = 1 ] || rm -f "$OUT/khnmc.bin"; exit 0; fi
exit 1
