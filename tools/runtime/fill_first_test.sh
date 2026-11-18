#!/usr/bin/env bash
# tools/fill_first_test.sh : the FIRST TEST of the "fill" mode (task 37; NATIVE-FILL-PLAN.md section 8 step 1: S1 + H1 with H1
# unconditional) on the Mac mini's GPU, driven from the Linux worktree with tools/darwin_route.sh.  Written for the MAIN session (task 37
# itself ran no vkd3d, no Mac): run it after the task-37 sources are merged and built there.
#
#   export KHNMC_MAC_DIR=khnmc-work/build-<x>      # REQUIRED: the Mac build folder that has the merged task-37 sources
#   export KHNMC_RUN_DIR=$PWD/run-fill37           # optional (default $PWD/run-fill37): results here
#   tools/fill_first_test.sh build                 # = JOBS=${JOBS:-4} MAKE_ARGS="GFX_VKD3D=1" tools/darwin_build.sh  (not part of `all`)
#   tools/fill_first_test.sh all [TAG]             # selftest, run, fetch, compare; stops at the first failure.  TAG default fill37
#   (or the steps one by one:  selftest [TAG] | run [TAG] | fetch [TAG] | compare [TAG];  show [TAG] prints the two `run` commands
#    with the current knobs and runs nothing)
#
# What each step does (exact commands, with TAG=fill37):
#   selftest  tools/darwin_route.sh selftest fill37-null
#             tools/darwin_route.sh selftest fill37-gfx KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system VKD3D_DEBUG=warn VKD3D_SHADER_DEBUG=warn
#             both outputs must hold "kh: aspect selftest: N checks, 0 failures" (the task-37 suite: proves the build has the code)
#   run       two runs of the default ("census") route, input/route-field-battle.txt, difficulty 3, frames clock, the vkd3d bridge on
#             the Metal device, frame dumps of presents FROM..TO every EVERY (default 880..1280 every 40: the first field from game
#             frame ~846, the first battle from ~1117), the hash log, and the size records (KHNMC_ASPECT_LOG=1):
#     G="--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn"
#     P="--env KHNMC_CLOCK=frames --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_GFX_DUMP_EVERY=40
#        --env KHNMC_GFX_DUMP_FROM=880 --env KHNMC_GFX_DUMP_TO=1280 --env KHNMC_ASPECT_LOG=1"
#     tools/darwin_route.sh run fill37-ref  --input input/route-field-battle.txt --difficulty 3 --game-frames 2400 $G $P
#           (with a FILL_SCREEN width W other than 1920 also --env KHNMC_SCREEN=Wx(9W/16): the REF must be W wide and 16:9)
#     tools/darwin_route.sh run fill37-tall --input input/route-field-battle.txt --difficulty 3 --game-frames 2400 $G $P \
#           --env KHNMC_SCREEN=1920x1350 --env KHNMC_ASPECT=fill --env KHNMC_ASPECT_TEST_H1=unconditional
#   fetch     tools/darwin_route.sh frames fill37-ref ; tools/darwin_route.sh frames fill37-tall          (the two index.tsv)
#             python3 tools/fill_compare.py pick <ref index> <tall index>   -> the presents both runs dumped at the same game frame
#             tools/darwin_route.sh frames fill37-ref <those presents> ; tools/darwin_route.sh frames fill37-tall <those presents>
#   compare   python3 tools/fill_compare.py pairs $RUN/mac-fill37-ref/frames $RUN/mac-fill37-tall/frames --out $RUN/fill37-report.txt
#             [--png $RUN/fill37-png when Pillow is there]; then the size records and route reports of both runs, and whether the two
#             runs made the same frames (the per-present counter lines and the route report must be equal: the task-37 null runs
#             show identical lines at 1920x1080 and 1920x1350); then the runs' own report lines:
#               python3 tools/fill_compare.py runcheck $RUN/mac-fill37-tall/run.out tall 1920x1350
#               python3 tools/fill_compare.py runcheck $RUN/mac-fill37-ref/run.out ref            (ref Wx(9W/16) when W is not 1920)
#             (tall: KHNMC_ASPECT=fill + unconditional, every H1 call rewritten and at least one, the built size checked at every
#             present and never another size, no screen-not-applied WARNING; ref: pc, no narrow call).  Pass conditions (a), (b), (c)
#             and their tolerances: tools/fill_compare.py --help.  Exit 0 only when every pair and both run checks pass.
# Environment knobs: FILL_SCREEN (default 1920x1350; narrower than 16:9 and the band offset (H - 9W/16) / 2 a whole number of rows,
# which also makes W a multiple of 16; with W other than 1920 the REF run gets KHNMC_SCREEN=Wx(9W/16)), FILL_FROM / FILL_TO /
# FILL_EVERY, FILL_GAME_FRAMES (default 2400), KHNMC_MAC, KHNMC_MAC_MIN_FREE, KHNMC_MAC_NICE (as tools/darwin_route.sh).
# Rules: one khnmc run at a time on the Mac mini (darwin_route.sh refuses otherwise), everything there stays in ~/$KHNMC_MAC_DIR, nothing
# is deleted there by this script.  A TAG whose results exist (here: $RUN/mac-TAG-*; there: run/TAG-ref or run/TAG-tall) is refused:
# the dump index is appended to, so an old index would mix two attempts.  Nothing is retried.  Raw dumps are 6.2 / 7.8 MB each.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
die(){ echo "fill_first_test: $*" >&2; exit 1; }
[ -n "${KHNMC_MAC_DIR:-}" ] || { echo "fill_first_test: set KHNMC_MAC_DIR to the Mac build folder with the merged task-37 sources (e.g. khnmc-work/build-l21b); it is never guessed" >&2; exit 2; }
export KHNMC_RUN_DIR=${KHNMC_RUN_DIR:-$PWD/run-fill37}
RUN=$KHNMC_RUN_DIR
MAC=${KHNMC_MAC:-user@mac-mini}
SCREEN=${FILL_SCREEN:-1920x1350}; FROM=${FILL_FROM:-880}; TO=${FILL_TO:-1280}; EVERY=${FILL_EVERY:-40}; GFR=${FILL_GAME_FRAMES:-2400}   # 2400: the route must also FINISH (its last battle step ran past 1400 on the Mac at difficulty 3, 11/18)
for v in "$FROM" "$TO" "$EVERY" "$GFR"; do case "$v" in ''|*[!0-9]*) echo "fill_first_test: FILL_FROM / FILL_TO / FILL_EVERY / FILL_GAME_FRAMES must be numbers" >&2; exit 2;; esac; done
[[ $SCREEN =~ ^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}$ ]] || { echo "fill_first_test: FILL_SCREEN=$SCREEN is not WxH (decimal, no leading zero)" >&2; exit 2; }
SW=${SCREEN%x*}; SH=${SCREEN#*x}
[ $(( (16*SH - 9*SW) % 32 )) -eq 0 ] && [ $((16*SH)) -gt $((9*SW)) ] || { echo "fill_first_test: FILL_SCREEN=$SCREEN: narrower than 16:9 with a whole-row band offset (H - 9W/16) / 2 is required" >&2; exit 2; }
# the REF is W x 9W/16 (fill_compare.py condition (a) needs the same width): 1920x1080 is the game with nothing set; any other width
# gets the screen seam at 16:9 (the seam at 16:9 changes nothing: plan step 2, task 37's null runs).  (16H - 9W) % 32 == 0 above
# already makes 9W a multiple of 16.
[ $(( (9*SW) % 16 )) -eq 0 ] || { echo "fill_first_test: FILL_SCREEN=$SCREEN: 9W/16 is not a whole number" >&2; exit 2; }
RH=$(( 9*SW/16 )); REFSCREEN=(); RCHK=()
[ "$SW" -eq 1920 ] || { REFSCREEN=(--env "KHNMC_SCREEN=${SW}x$RH"); RCHK=("${SW}x$RH"); }
[ "$TO" -ge "$FROM" ] && [ "$EVERY" -ge 1 ] && [ "$GFR" -gt "$TO" ] || { echo "fill_first_test: need FILL_FROM <= FILL_TO < FILL_GAME_FRAMES and FILL_EVERY >= 1" >&2; exit 2; }
STEP=${1:-}; TAG=${2:-fill37}
case "$TAG" in ''|*[!A-Za-z0-9._-]*) echo "fill_first_test: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac
REF=$TAG-ref; TALL=$TAG-tall
DR="$HERE/tools/darwin_route.sh"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
G=(--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn)
P=(--env KHNMC_CLOCK=frames --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env "KHNMC_GFX_DUMP_EVERY=$EVERY"
   --env "KHNMC_GFX_DUMP_FROM=$FROM" --env "KHNMC_GFX_DUMP_TO=$TO" --env KHNMC_ASPECT_LOG=1)
ROUTE=(--input input/route-field-battle.txt --difficulty 3 --game-frames "$GFR")

need_py(){ python3 -c 'import numpy' 2>/dev/null || die "python3 with numpy is needed here for tools/fill_compare.py"; }
step_build(){ echo "fill_first_test: build: JOBS=${JOBS:-4} MAKE_ARGS=GFX_VKD3D=1 tools/darwin_build.sh ($KHNMC_MAC_DIR)"
  JOBS=${JOBS:-4} MAKE_ARGS="GFX_VKD3D=1" "$HERE/tools/darwin_build.sh" || die "build failed"; }
step_selftest(){
  mkdir -p "$RUN"
  "$DR" selftest "$TAG-null" || die "selftest (null backend) failed: $RUN/mac-selftest-$TAG-null.out"
  "$DR" selftest "$TAG-gfx" KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system VKD3D_DEBUG=warn VKD3D_SHADER_DEBUG=warn || die "selftest (vkd3dw on the Metal device) failed: $RUN/mac-selftest-$TAG-gfx.out"
  for k in null gfx; do f="$RUN/mac-selftest-$TAG-$k.out"
    grep -aq "^kh: aspect selftest: [0-9]* checks, 0 failures" "$f" || die "$f has no 'kh: aspect selftest: N checks, 0 failures' line: the Mac build lacks the task-37 code, or the suite failed"
    echo "fill_first_test: $k: $(grep -a '^kh: aspect selftest:' "$f")"; done; }
REFRUN=(run "$REF" "${ROUTE[@]}" "${G[@]}" "${P[@]}" "${REFSCREEN[@]}")
TALLRUN=(run "$TALL" "${ROUTE[@]}" "${G[@]}" "${P[@]}" --env "KHNMC_SCREEN=$SCREEN" --env KHNMC_ASPECT=fill --env KHNMC_ASPECT_TEST_H1=unconditional)
step_show(){ echo "fill_first_test: REF ${SW}x$RH, TALL $SCREEN (band offset $(( (SH-RH)/2 )) rows); nothing is run"
  echo "tools/darwin_route.sh ${REFRUN[*]}"; echo "tools/darwin_route.sh ${TALLRUN[*]}"; }
step_run(){
  for t in "$REF" "$TALL"; do [ ! -e "$RUN/mac-$t" ] || die "$RUN/mac-$t exists: choose another TAG (an old frame index would be appended to)"; done
  "${SSH[@]}" "$MAC" "test ! -e ~/$KHNMC_MAC_DIR/run/$REF && test ! -e ~/$KHNMC_MAC_DIR/run/$TALL" || die "run/$REF or run/$TALL exists on the Mac mini (or it cannot be reached): choose another TAG"
  echo "fill_first_test: run $REF: ${SW}x$RH ($([ "$SW" -eq 1920 ] && echo "nothing set but the size records" || echo "KHNMC_SCREEN=${SW}x$RH and the size records")), dumps $FROM..$TO every $EVERY, $GFR game frames"
  "$DR" "${REFRUN[@]}" || die "run $REF failed ($RUN/mac-$REF/run.out)"
  echo "fill_first_test: run $TALL: KHNMC_SCREEN=$SCREEN KHNMC_ASPECT=fill KHNMC_ASPECT_TEST_H1=unconditional"
  "$DR" "${TALLRUN[@]}" || die "run $TALL failed ($RUN/mac-$TALL/run.out)"; }
step_fetch(){
  need_py
  "$DR" frames "$REF" || die "no frame index of $REF"
  "$DR" frames "$TALL" || die "no frame index of $TALL"
  local pk; pk=$(python3 "$HERE/tools/fill_compare.py" pick "$RUN/mac-$REF/frames/index.tsv" "$RUN/mac-$TALL/frames/index.tsv") || die "the two runs have no dump at the same game frame"
  local rp=${pk%%;*} tp=${pk#*;}
  echo "fill_first_test: same game frames dumped by both runs: REF presents $rp; TALL presents $tp"
  # shellcheck disable=SC2086
  "$DR" frames "$REF" $rp || die "fetching the $REF dumps failed"
  # shellcheck disable=SC2086
  "$DR" frames "$TALL" $tp || die "fetching the $TALL dumps failed"; }
step_compare(){
  need_py
  local rep="$RUN/$TAG-report.txt" png=() rc
  python3 -c 'import PIL' 2>/dev/null && png=(--png "$RUN/$TAG-png") || echo "fill_first_test: NOTE: no Pillow here: no PNG copies of the pairs (the numbers are complete)"
  python3 "$HERE/tools/fill_compare.py" pairs "$RUN/mac-$REF/frames" "$RUN/mac-$TALL/frames" --out "$rep" "${png[@]}"; rc=$?
  local extra="$RUN/$TAG-runs.txt" o t a b c d
  : > "$extra" || die "cannot write $extra"
  echo >> "$extra"; echo "== the two runs (frames clock: they must make the same frames; only the screen and H1 differ)" >> "$extra"
  for t in "$REF" "$TALL"; do o="$RUN/mac-$t/run.out"
    if [ ! -f "$o" ]; then echo "$t: NO run.out at $o" >> "$extra"; rc=1; continue; fi
    echo "-- $t: $(grep -a 'kh: route report' "$o" | tail -1 | cut -c1-200)" >> "$extra"
    grep -a -E '^kh: screen|^kh: aspect( report)?:' "$o" | grep -av 'rt census: new kind' | cut -c1-900 >> "$extra"
    grep -a '^kh: gfx vkd3d census: render-target' "$o" | tail -1 | cut -c1-900 >> "$extra"          # the bridge's own render-target census (the last report)
  done
  a=$(grep -a '^kh: gfx frame=' "$RUN/mac-$REF/run.out" 2>/dev/null | md5sum | cut -c1-16); b=$(grep -a '^kh: gfx frame=' "$RUN/mac-$TALL/run.out" 2>/dev/null | md5sum | cut -c1-16)
  c=$(grep -a 'kh: route report' "$RUN/mac-$REF/run.out" 2>/dev/null | md5sum | cut -c1-16); d=$(grep -a 'kh: route report' "$RUN/mac-$TALL/run.out" 2>/dev/null | md5sum | cut -c1-16)
  if [ "$a" = "$b" ]; then echo "-- per-present counter lines: EQUAL ($a)" >> "$extra"
  else echo "-- per-present counter lines: DIFFERENT ($a / $b): the runs diverged; pairs by game frame may not be the same moment" >> "$extra"; rc=1; fi
  if [ "$c" = "$d" ]; then echo "-- route reports: EQUAL" >> "$extra"; else echo "-- route reports: DIFFERENT: the runs diverged" >> "$extra"; rc=1; fi
  # the runs' own report lines (H1 really rewrote every call of the TALL run; the game built the asked size; REF is pc at 16:9)
  echo "-- run checks (tools/fill_compare.py runcheck):" >> "$extra"
  python3 "$HERE/tools/fill_compare.py" runcheck "$RUN/mac-$TALL/run.out" tall "$SCREEN" >> "$extra" 2>&1 || rc=1
  python3 "$HERE/tools/fill_compare.py" runcheck "$RUN/mac-$REF/run.out" ref "${RCHK[@]}" >> "$extra" 2>&1 || rc=1
  cat "$extra" >> "$rep" && rm -f "$extra"
  sed -n '/^== the two runs/,$p' "$rep"
  echo "fill_first_test: report $rep$([ ${#png[@]} -gt 0 ] && echo ", pictures $RUN/$TAG-png/"); $([ "$rc" = 0 ] && echo "every pair PASSED and the runs made the same frames" || echo "NOT PASSED (see the report)")"
  return $rc; }
case "$STEP" in
build) step_build;;
selftest) step_selftest;;
run) step_run;;
fetch) step_fetch;;
compare) step_compare; exit $?;;
show) step_show;;
all) step_selftest; step_run; step_fetch; step_compare; exit $?;;
*) sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2;;
esac
