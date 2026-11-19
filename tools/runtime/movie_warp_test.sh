#!/usr/bin/env bash
# tools/movie_warp_test.sh (task 44, review): every pre-rendered movie a program of the data names, played through the DEBUG WARP.
#
#   tools/movie_warp_test.sh [-b BINARY] [-o OUTDIR] [-j 1|2] [-g GAME_FRAMES] [-s SECONDS] [-e VAR=VALUE]... [-f] [-S] [NAME ...]
#
# For each NAME (default: all nine) a route file is written to OUTDIR/routes/: the head of input/route-movie-opening.txt (Initial
# Settings, title, New Game, the first field: every line up to the first `@expect area 2`) and one line
#     @warp list warp-NAME.list settle 60 timeout T        (the list: `WORLD ROOM evt PROGRAM`)
# i.e. the game's own map jump to the room with the evt program that starts the movie's event (tools/movie_table.py, column
# `named by`).  A WARP RESULT SAYS "THIS SCENE RUNS FROM THIS STATE", NOT "THE GAME REACHES IT" (input/README.md, task 33): the save
# is a fresh New Game's.  The list form is used because some of these scenes never come back to a field (ena ends in the BATTLE
# REPORT menu): a place of a list that does not come back within T game frames ENDS THE RUN IN ORDER with exit status 6 and a
# `kh: warp: ... result` line that names what was there instead (a scene, a menu); a plain `@warp` would be a named fatal.  So a
# run of this test ends with status 0 (the field came back; the run then idles to its game-frame limit) or 6 (it did not).
# What the run proves is read from the log by tools/movie_run_check.py: the movie was requested, its EVENT ended by itself (the
# natural-end site), its player was torn down, nothing was dropped or skipped, and the request line says whether the scene has
# a pause screen (ena / sec / sec2: it has none).
#   -b  binary (default ./khnmc)             -o  output directory (default d44/run/movie-warp)      -j  runs in parallel (1 or 2)
#   -g  game frames per run (default 2600)   -s  time limit per run in seconds (default 400)       -e  extra environment
#   -t  the warp's time-out T in game frames (default 1500)
#   -f  KHNMC_MOVIES=fail is expected (give it with -e): the check is then "the event did NOT end" (the old black wait), and for
#       ena / sec / sec2 "the request line says FOR EVER and promises no Skip"
#   -S  TWO MOVIES IN ONE SESSION, no return to the title in between: the head is input/route-story.txt up to its first
#       `@expect mission` instead (the story has then played the opening movie opn BY ITSELF in tt01 and stands in tt07 with a mission
#       running, about game frame 8,340), then the warp to NAME's scene.  NAME is the SECOND movie request of the run: the check wants
#       opn first, NAME second, both events ended by themselves through the task branch, nothing dropped (the game's guard in movie
#       command 0 drops a request while the player object of an earlier movie still exists), and that the second movie made no
#       new D3D12 resource (--steady-resources: the picture of the first one is handed out again).  Give -g 10200 or more
# Null backend, KHNMC_CLOCK=frames, nice -n 10.  Output: one line per movie, OUTDIR/table.tsv, exit status 0 only when every run
# ended in order and passed its check.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN="$HERE/khnmc"; OUT="$HERE/d44/run/movie-warp"; J=1; G=2600; S=400; EXTRA=(); FAILMODE=0; WT=1500; STORY=0
while getopts "b:o:j:g:s:e:ft:S" o; do case "$o" in
  b) BIN=$OPTARG;; o) OUT=$OPTARG;; j) J=$OPTARG;; g) G=$OPTARG;; s) S=$OPTARG;; e) EXTRA+=(-e "$OPTARG");; f) FAILMODE=1;; t) WT=$OPTARG;; S) STORY=1;;
  *) sed -n '2,26p' "$0" >&2; exit 2;; esac; done
shift $((OPTIND-1))
case "$J" in 1|2) ;; *) echo "movie_warp_test: -j takes 1 or 2 (at most 2 null-backend runs in parallel on this machine)" >&2; exit 2;; esac
# name  world room evt-program  no-pause-screen (the event type of that program has bit 7: tools/movie_table.py `type 130`)
TABLE="opn tt 1 0x34 no
me2 tt 1 0x35 no
me4 tt 1 0x36 no
me6 tt 1 0x37 no
me3 tt 21 0x8c no
ena es 0 0x3a yes
sec es 0 0x3c yes
sec2 es 0 0x3d yes
lop es 1 0x38 no"
NAMES=("$@"); [ ${#NAMES[@]} -gt 0 ] || NAMES=(opn me2 me4 me6 me3 ena sec sec2 lop)
HEAD_SRC="$HERE/input/route-movie-opening.txt"; HEAD_END='@expect area 2'; K=1
if [ "$STORY" = 1 ]; then HEAD_SRC="$HERE/input/route-story.txt"; HEAD_END='@expect mission'; K=2
  [ "$FAILMODE" = 0 ] || { echo "movie_warp_test: -S and -f do not go together (with KHNMC_MOVIES=fail the story route skips its opening movie)" >&2; exit 2; }
  [ "$G" -ge 10200 ] || { echo "movie_warp_test: -S needs -g 10200 or more (the story route stands in tt07 at about game frame 8,340; the warp's time-out comes after)" >&2; exit 2; }; fi
[ -x "$BIN" ] && [ -f "$HEAD_SRC" ] || { echo "movie_warp_test: missing $BIN or $HEAD_SRC" >&2; exit 2; }
last=$(grep -n -m1 "^$HEAD_END\$" "$HEAD_SRC" | cut -d: -f1)
[ -n "$last" ] || { echo "movie_warp_test: $HEAD_SRC has no line '$HEAD_END' (the end of the route's head)" >&2; exit 2; }
mkdir -p "$OUT/routes" || exit 2
for n in "${NAMES[@]}"; do
  row=$(awk -v n="$n" '$1==n' <<<"$TABLE"); [ -n "$row" ] || { echo "movie_warp_test: unknown movie '$n' (opn me2 me4 me6 me3 ena sec sec2 lop)" >&2; exit 2; }
  read -r _ w r p _ <<<"$row"
  { echo "# route-movie-warp-$n.txt: written by tools/movie_warp_test.sh (task 44, review).  The head of $(basename "$HEAD_SRC") (to its first '$HEAD_END'), then a DEBUG"
    echo "# WARP (a test tool, not play) to $w room $r with evt program $p, whose event plays the pre-rendered movie $n."
    sed -n "1,${last}p" "$HEAD_SRC" | grep -v '^# '
    echo "@warp list warp-$n.list settle 60 timeout $WT"; } > "$OUT/routes/route-movie-warp-$n.txt"
  echo "$w $r evt $p" > "$OUT/routes/warp-$n.list"
done
one(){   # $1 = name
  local n=$1
  nice -n 10 "$HERE/tools/route_batch.sh" -n 1 -j 1 -d 3 -g "$G" -G -s "$S" -r none -i "$OUT/routes/route-movie-warp-$n.txt" -e KHNMC_CLOCK=frames "${EXTRA[@]}" \
       -w "nice -n 10" -b "$BIN" -o "$OUT/$n" > "$OUT/$n.batch.out" 2>&1
  echo $? > "$OUT/$n.batch.exit"
}
i=0
while [ $i -lt ${#NAMES[@]} ]; do
  one "${NAMES[$i]}" & [ "$J" = 2 ] && [ $((i+1)) -lt ${#NAMES[@]} ] && { sleep 3; one "${NAMES[$((i+1))]}" & }
  wait; i=$((i+J))
done
fails=0; T="$OUT/table.tsv"
printf 'movie\twarp\tbatch\tcheck\trequested at\tevent ended\tplayer torn down\tscene has a pause screen\twhat the log shows after the movie\n' > "$T"
for n in "${NAMES[@]}"; do
  row=$(awk -v n="$n" '$1==n' <<<"$TABLE"); read -r _ w r p nop <<<"$row"
  o="$OUT/$n/r01/run.out"; bx=$(cat "$OUT/$n/r01/exit" 2>/dev/null || echo missing)
  # status 6 = the warp list stopped at a place that did not come back to the field: in order only with its own line
  inorder=0; [ "$bx" = 0 ] && inorder=1; [ "$bx" = 6 ] && grep -aq 'kh: warp: list stopped at entry 1 of 1' "$o" 2>/dev/null && inorder=1
  # route_batch keeps its copy of the binary for a run that did not exit 0; status 6 in order is no failure here, and the copy is BIN
  [ "$inorder" = 1 ] && cmp -s "$OUT/$n/khnmc.bin" "$BIN" && rm -f "$OUT/$n/khnmc.bin"
  if [ "$STORY" = 1 ]; then python3 "$HERE/tools/movie_run_check.py" "$o" --first opn --second "$n" --movies 2 --nopause no --steady-resources > "$OUT/$n.check.out" 2>&1
  elif [ "$FAILMODE" = 1 ]; then python3 "$HERE/tools/movie_run_check.py" "$o" --mode fail-wait --first "$n" --nopause "$nop" > "$OUT/$n.check.out" 2>&1
  else python3 "$HERE/tools/movie_run_check.py" "$o" --first "$n" --nopause "$nop" > "$OUT/$n.check.out" 2>&1; fi
  cx=$?
  [ "$inorder" = 1 ] && [ "$cx" = 0 ] || fails=$((fails+1))
  rq=$(grep -a -m1 "kh: movie: #$K $n.mp4 requested" "$o" 2>/dev/null | sed -n 's/.*requested at game frame \([0-9]*\).*/\1/p')
  ev=$(grep -a -m1 "kh: movie: #$K $n.mp4: the event" "$o" 2>/dev/null | sed -n 's/.*the event \(ended by itself\|was ended by SKIP\|was ended\) at game frame \([0-9]*\).*/\1 at \2/p')
  td=$(grep -a -m1 "kh: movie: #$K $n.mp4: player torn down" "$o" 2>/dev/null | sed -n 's/.*released at game frame \([0-9]*\).*/\1/p')
  # after the movie: the other movies of the run, the last evt program / place the warp or the route state reported, a menu
  more=$(grep -a 'kh: movie: #[0-9]* .* requested' "$o" 2>/dev/null | sed -n 's/.*#\([0-9]*\) \(\S*\) requested at game frame \([0-9]*\).*/#\1 \2 at \3/p' | grep -v "^#$K " | paste -sd, -)
  wres=$(grep -a 'kh: warp: #1 result' "$o" 2>/dev/null | tail -1 | sed 's/^kh: warp: #1 result //' | cut -c1-150)
  menu=$(grep -a 'kh: menu ' "$o" 2>/dev/null | awk -v f="${rq:-0}" '{ if(match($0,/game frame [0-9]+/)){ g=substr($0,RSTART+11,RLENGTH-11)+0; if(g>=f) { print; exit } } }' | sed 's/^kh: //' | cut -c1-110)
  after="${more:+other movies of the run: $more; }${wres:+warp result: $wres; }${menu:+first menu after the request: $menu}"
  printf '%s\t%s %s evt %s\tstatus %s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$w" "$r" "$p" "$bx" "$([ "$cx" = 0 ] && echo OK || echo FAILED)" "${rq:--}" "${ev:-NOT ENDED}" "${td:--}" "$([ "$nop" = yes ] && echo "NO (event type bit 7)" || echo yes)" "${after:--}" >> "$T"
done
column -t -s "$(printf '\t')" "$T" 2>/dev/null || cat "$T"
echo "movie_warp_test: ${#NAMES[@]} movies, $(( ${#NAMES[@]} - fails )) ok, $fails failed (binary $(sha256sum "$BIN" | cut -c1-16)..., mode $([ "$FAILMODE" = 1 ] && echo "fail expected" || echo default)$([ "$STORY" = 1 ] && echo ", each as the SECOND movie of a story session (-S)")); routes, runs and checks in $OUT"
[ "$fails" -eq 0 ]
