#!/usr/bin/env bash
# tools/room_sweep.sh (task 33): enter every room of the game with the scripted controller's DEBUG WARP and record what happens.
#
#   tools/room_sweep.sh [-o OUTDIR] [-p default|localset|list] [-L LISTFILE] [-j LANES] [-t TIMEOUT] [-s SETTLE] [-S SECONDS] [-R "ROOMS"]
#                       [-b BINARY] [-w "WRAPPER"] [-m MIN_AVAILABLE_MB] [-n ENTRIES_PER_RUN] [-T BUDGET_SECONDS]
#
#   -p  default (the default): one entry per area file of data-full/ard/us (311), `WORLD ROOM`: door 0, the save's programs.
#       localset: for the rooms of -R (default: mu13 ca26 nm11, the Neverland rooms lk00..lk16, the Traverse Town rooms lm00..lm11),
#       one entry per id of the mod's 07localset.bin for that room: `WORLD ROOM map ID btl ID evt ID` (the record exe 0x39fbc0 builds).
#       list: the entries of -L LISTFILE as they are (warp-list lines: `WORLD ROOM [door N] [map N] [btl N] [evt N]`; # comments)
#   -j  lanes: processes in parallel (default 2; at most 2: the owner's live desktop)
#   -t  game frames a warped room has to come back to the field (default 2400)   -s  game frames the field must stay up (default 60)
#   -S  KH_RUN_SECONDS of one process (default 1800): the watchdog of a run that stops making progress
#   -b  binary (default ./khnmc; copied into OUTDIR once, so a rebuild cannot mix binaries; a resumed sweep keeps its copy)
#   -w  command prefix of every run (default "nice -n 10")      -m  MemAvailable needed to start a run (default 1500 MB)
#   -n  entries one process warps through at most (default 15): bounds a process's length; the next process boots afresh
#   -T  start no new process after BUDGET_SECONDS (default 0 = no budget): the call ends PARTIAL (exit 3, resumable) when entries are
#       left; a running process is never cut short
#
# How: every process boots like the default route (the steps of input/route-field-battle.txt up to the first field), then runs
# `@warp list LIST settle S timeout T` over the entries of its lane that are still open.  The game warps from room to room in one
# process; a room that does not come back to the field ends the process (exit 6, `kh: warp: list stopped`), a room that kills it ends it
# with its fatal or fault; either way the runner records that room and starts a new process with the next one.  State: one JSON line per
# decided entry in OUTDIR/state.L<lane>.jsonl; a second call with the same OUTDIR resumes with the entries that are not decided yet.
# A process that decides nothing (it died before its first warp) is retried once; a second such process stops its lane LOUDLY.
# Result: OUTDIR/table.md (tools/room_sweep.py table): one line per entry, the counts by outcome.  Exit 0 when every entry is decided.
# Bookkeeping: every call appends its parameters to OUTDIR/params.txt, every run directory gets env.txt (the exact environment, wrapper,
# list and binary of that process).  A run directory without an `exit` file (its runner was interrupted) is marked INTERRUPTED at the
# start of the next call (its game root and docs copies removed; no entry was recorded from it, so its entries stay open unless
# another run decided them); a call refuses to start while a process still runs in one.
#
# THIS IS A TEST TOOL: every result says "this room loads from this state" (a new game warped there from the first field, after the
# rooms before it in the lane), not "the game reaches it".
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=""; LIST=""; PASS=default; J=2; TMO=2400; SETTLE=60; SECS=1800; BIN="$HERE/khnmc"; WRAP="nice -n 10"; MINMB=1500; PER=15; BUDGET=0; T0=$(date +%s)
ROOMS="mu13 ca26 nm11 $(for i in $(seq 0 16); do printf 'lk%02d ' "$i"; done)$(for i in $(seq 0 11); do printf 'lm%02d ' "$i"; done)"
while getopts "o:p:L:j:t:s:S:R:b:w:m:n:T:" o; do case "$o" in
  o) OUT=$OPTARG;; L) LIST=$OPTARG;; p) PASS=$OPTARG;; j) J=$OPTARG;; t) TMO=$OPTARG;; s) SETTLE=$OPTARG;; S) SECS=$OPTARG;; R) ROOMS=$OPTARG;; b) BIN=$OPTARG;;
  w) WRAP=$OPTARG;; m) MINMB=$OPTARG;; n) PER=$OPTARG;; T) BUDGET=$OPTARG;; *) sed -n '2,30p' "$0" >&2; exit 2;; esac; done
case "$J$TMO$SETTLE$SECS$MINMB$PER$BUDGET" in *[!0-9]*) echo "room_sweep: -j -t -s -S -m -n -T take numbers" >&2; exit 2;; esac
[ "$PER" -ge 1 ] || { echo "room_sweep: -n is at least 1" >&2; exit 2; }
[ "$J" -ge 1 ] && [ "$J" -le 2 ] || { echo "room_sweep: -j is 1 or 2 (at most 2 game runs in parallel on this machine)" >&2; exit 2; }
case "$PASS" in default|localset) ;; list) [ -s "$LIST" ] || { echo "room_sweep: -p list needs -L LISTFILE (a non-empty warp list)" >&2; exit 2; };; *) echo "room_sweep: -p default|localset|list" >&2; exit 2;; esac
PY="python3 -I $HERE/tools/room_sweep.py"
EXE="$HERE/pc/kh2fm.exe"; DATA="$HERE/data-full"; BOOT="$HERE/input/route-field-battle.txt"
for f in "$EXE" "$DATA/ard/us" "$HERE/pkgdir" "$BOOT"; do [ -e "$f" ] || { echo "room_sweep: missing $f" >&2; exit 2; }; done
[ -n "$OUT" ] || OUT="$HERE/run-sweep/$PASS-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/runs" || exit 2; OUT=$(cd "$OUT" && pwd)
if [ ! -s "$OUT/entries.tsv" ]; then
  if [ "$PASS" = default ]; then $PY plan default "$DATA" > "$OUT/plan.txt" || exit 2
  elif [ "$PASS" = list ]; then sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^[[:space:]]*$/d' "$LIST" > "$OUT/plan.txt" || exit 2
  else $PY plan localset "$DATA" $ROOMS > "$OUT/plan.txt" 2> "$OUT/plan.err" || { cat "$OUT/plan.err" >&2; exit 2; }; cat "$OUT/plan.err" >&2; fi
  grep -v '^#' "$OUT/plan.txt" | awk '{printf "%d\t%s\n", NR, $0}' > "$OUT/entries.tsv"
fi
# run directories left without an exit file by an interrupted call: refuse while a process still runs in one (nothing is marked or
# recorded then), else mark them (no entry was recorded from them: their entries stay open, unless another run decided them, and get a
# new run directory)
for d in "$OUT"/runs/L*-[0-9]*; do
  [ -d "$d" ] && [ ! -e "$d/exit" ] && [ ! -e "$d/INTERRUPTED" ] || continue
  for p in /proc/[0-9]*; do [ "$(readlink "$p/cwd" 2>/dev/null)" = "$d" ] && { echo "room_sweep: process ${p#/proc/} still runs in $d (no exit file yet); not starting" >&2; exit 2; }; done
done
for d in "$OUT"/runs/L*-[0-9]*; do
  [ -d "$d" ] && [ ! -e "$d/exit" ] && [ ! -e "$d/INTERRUPTED" ] || continue
  printf 'INTERRUPTED: no exit file at the start of the call of %s (the runner stopped while this process ran); no entry was recorded from it (its entries stay open unless a later run decided them)\n' "$(date '+%Y-%m-%d %H:%M:%S')" > "$d/INTERRUPTED"
  rm -rf "$d/root" "$d/docs"
  echo "room_sweep: LOUD: $d has no exit file (an interrupted call); marked INTERRUPTED (no entry was recorded from it)" >&2
done
# every call's parameters (a resumed call may use other -t -s -S -n -j values: the run directories' env.txt say which run had which)
printf 'call %s: pass=%s timeout=%s settle=%s seconds=%s lanes=%s per-run=%s budget=%s wrapper="%s" min-mb=%s rooms=%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$PASS" "$TMO" "$SETTLE" "$SECS" "$J" "$PER" "$BUDGET" "$WRAP" "$MINMB" \
  "$(case "$PASS" in localset) echo "$ROOMS";; list) echo "list $LIST";; *) echo all;; esac)" >> "$OUT/params.txt"
NENT=$(wc -l < "$OUT/entries.tsv")
# the boot prefix: the default route's steps up to and including the first field (`@expect game-level difficulty`)
sed -n '1,/^@expect game-level difficulty/p' "$BOOT" > "$OUT/boot.txt"
grep -q '^@expect game-level difficulty' "$OUT/boot.txt" || { echo "room_sweep: $BOOT has no '@expect game-level difficulty' line to end the boot prefix" >&2; exit 2; }
avail_mb(){ awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo; }
decided(){ cat "$OUT"/state.L*.jsonl 2>/dev/null | python3 -I -c 'import sys,json
for l in sys.stdin:
    l=l.strip()
    if l: print(json.loads(l)["entry"])' | sort -n -u; }
# the open entries, cut once into J contiguous slices (a lane works only on its own slice: no entry is warped twice)
OPEN=$(comm -23 <(cut -f1 "$OUT/entries.tsv" | sort) <(decided | sort) | sort -n)
NOPEN=$(printf '%s\n' "$OPEN" | grep -c .)
# the binary: copied once, when there is something to run (a call with every entry decided only marks, records and tables)
if [ "$NOPEN" -gt 0 ]; then [ -e "$OUT/khnmc.bin" ] || cp -f "$BIN" "$OUT/khnmc.bin" || { echo "room_sweep: cannot copy $BIN" >&2; exit 2; }; fi
echo "room_sweep: pass $PASS, $NENT entries, $J lanes, timeout $TMO, settle $SETTLE, KH_RUN_SECONDS $SECS, binary $([ -e "$OUT/khnmc.bin" ] && sha256sum "$OUT/khnmc.bin" | cut -c1-16 || echo none)..., out $OUT"
for L in $(seq 0 $((J-1))); do printf '%s\n' "$OPEN" | grep . | awk -v L="$L" -v J="$J" -v N="$NOPEN" 'BEGIN{per=int((N+J-1)/J)} {i=NR-1; if(int(i/per)==L) print}' > "$OUT/lane$L.txt"; done
echo "room_sweep: $NOPEN entries open: $(for L in $(seq 0 $((J-1))); do printf 'lane %d %d  ' "$L" "$(grep -c . "$OUT/lane$L.txt")"; done)"
lane(){   # $1 = lane index: the entries of OUTDIR/lane<L>.txt that are not decided yet, in order
  local L=$1 k=0 empty=0 st="$OUT/state.L$1.jsonl"
  touch "$st"
  # a resumed sweep numbers its processes after the ones already there (the state rows name their run directory: never reuse one)
  local last; last=$(ls -d "$OUT/runs/L$L-"[0-9]* 2>/dev/null | sed 's/.*-//' | sort -n | tail -1); k=$((10#${last:-0}))
  while :; do
    local mine; mine=$(comm -23 <(sort "$OUT/lane$L.txt") <(decided | sort) | sort -n | head -n "$PER")
    [ -z "$mine" ] && return 0
    if [ "$BUDGET" -gt 0 ] && [ $(( $(date +%s) - T0 )) -ge "$BUDGET" ]; then echo "room_sweep: lane $L: budget of $BUDGET s used, no new process (entries left: resume with the same -o)"; return 0; fi
    k=$((k+1)); local d="$OUT/runs/L$L-$(printf '%03d' $k)"
    [ -e "$d" ] && { echo "room_sweep: lane $L: $d exists already; refusing to overwrite a run the state rows may name" >&2; return 1; }
    mkdir -p "$d/root" "$d/docs"
    : > "$d/list.txt"; : > "$d/map.tsv"; local ln=0
    for e in $mine; do ln=$((ln+1)); awk -F'\t' -v e="$e" '$1==e{print $2}' "$OUT/entries.tsv" >> "$d/list.txt"; printf '%d\t%d\n' "$ln" "$e" >> "$d/map.tsv"; done
    { cat "$OUT/boot.txt"; printf '\n# task 33: the debug warp over the open entries of lane %d (a TEST TOOL: this room loads from this state)\n@warp list list.txt settle %s timeout %s\n' "$L" "$SETTLE" "$TMO"; } > "$d/route.txt"
    printf 'started %s\nwrapper %s\nbinary %s (%s)\nKH_EXE=%s KH_ROOT=%s KH_DOCS=%s KHNMC_DATA=%s KHNMC_PKGDIR=%s KHNMC_INPUT_SCRIPT=%s KH_RUN_SECONDS=%s KHNMC_ROUTE_END=exit KHNMC_PHASELOG=%s KHNMC_FILELOG=%s KH_DUMP_THREADS=1\nwarp list list.txt settle %s timeout %s (%s entries)\n' \
      "$(date '+%Y-%m-%d %H:%M:%S')" "$WRAP" "$OUT/khnmc.bin" "$(sha256sum "$OUT/khnmc.bin" | cut -c1-16)" "$EXE" "$d/root" "$d/docs" "$DATA" "$HERE/pkgdir" "$d/route.txt" "$SECS" "$d/phase.log" "$d/files.log" \
      "$SETTLE" "$TMO" "$(wc -l < "$d/list.txt")" > "$d/env.txt"
    local waited=0; while [ "$(avail_mb)" -lt "$MINMB" ]; do [ $waited -ge 600 ] && { echo "room_sweep: lane $L: MemAvailable below $MINMB MB for 600 s, stopping the lane LOUDLY" >&2; return 1; }; sleep 10; waited=$((waited+10)); done
    echo "room_sweep: lane $L run $k: $(wc -l < "$d/list.txt") entries from entry $(head -1 "$d/map.tsv" | cut -f2) ($(head -1 "$d/list.txt"))"
    ( cd "$d" && env KH_EXE="$EXE" KH_ROOT="$d/root" KH_DOCS="$d/docs" KHNMC_DATA="$DATA" KHNMC_PKGDIR="$HERE/pkgdir" KHNMC_INPUT_SCRIPT="$d/route.txt" \
        KH_RUN_SECONDS="$SECS" KHNMC_ROUTE_END=exit KHNMC_PHASELOG="$d/phase.log" KHNMC_FILELOG="$d/files.log" KH_DUMP_THREADS=1 \
        timeout -k 30 $((SECS+300)) $WRAP "$OUT/khnmc.bin" > "$d/run.out" 2>&1; echo $? > "$d/exit" )
    rm -rf "$d/root" "$d/docs"
    $PY parse "$d" "$d/list.txt" > "$d/parsed.jsonl" || { echo "room_sweep: lane $L: cannot parse $d" >&2; return 1; }
    local got; got=$($PY record "$d/parsed.jsonl" "$d/map.tsv" "$d" "$st") || { echo "room_sweep: lane $L: cannot record $d" >&2; return 1; }
    echo "room_sweep: lane $L run $k: exit $(cat "$d/exit"), $got entries decided ($(grep -c 'kh: warp: #[0-9]* result' "$d/run.out") results)"
    if [ "${got:-0}" -eq 0 ]; then
      empty=$((empty+1))
      if [ $empty -ge 2 ]; then echo "room_sweep: lane $L: two processes in a row decided nothing (last: $d, exit $(cat "$d/exit"): $(grep -m1 -E 'kh: (FATAL|SIGNAL)' "$d/run.out")); stopping the lane LOUDLY" >&2; return 1; fi
    else empty=0; fi
  done
}
pids=(); for L in $(seq 0 $((J-1))); do lane "$L" & pids+=($!); done
rc=0; for p in "${pids[@]}"; do wait "$p" || rc=1; done
$PY table "$OUT" "$DATA" > "$OUT/table.md" || rc=1
ndec=$(decided | grep -c .)
echo "room_sweep: $ndec of $NENT entries decided; table in $OUT/table.md"
tail -n 20 "$OUT/table.md" | sed -n '/^| outcome/,$p'
[ $rc -eq 0 ] || exit 1
[ "$ndec" -eq "$NENT" ] || { echo "room_sweep: PARTIAL: $((NENT-ndec)) entries not decided yet (call again with -o $OUT to resume)"; exit 3; }
