#!/usr/bin/env bash
# tools/fill_h2h4_test.sh : the GPU check of the "fill" mode with H2 (culling), H4 (fades, event bars) and H5 (full-height scissors) on
# (task 40; NATIVE-FILL-PLAN.md section 8 steps 5 and 7) on the Mac mini, driven from the Linux worktree with tools/darwin_route.sh.
# A sibling of tools/fill_first_test.sh (task 37's first test).  Written for the MAIN session: run it after the task-40 sources are
# merged and built there.  GPU runs are NOT frame-identical past the field (fence timing: task 36 on lavapipe, the first test on the
# Mac), so no condition here needs two runs to be the same run: the pictures are compared by CONTENT (KHNMC_GFX_BANDLOG, per present
# the hash of the centred 16:9 band and of the strips above / below it, and the step across each band edge; KHNMC_GFX_HASHLOG, the
# hash of the whole picture), and by STATE (each run's own report lines: which hooks wrote what, which test aid was on).
#
#   export KHNMC_MAC_DIR=khnmc-work/build-<x>      # REQUIRED: the Mac build folder that has the merged task-40 sources
#   export KHNMC_RUN_DIR=$PWD/run-fill40           # optional (default $PWD/run-fill40): results here
#   tools/fill_h2h4_test.sh build                  # = JOBS=${JOBS:-4} MAKE_ARGS="GFX_VKD3D=1" tools/darwin_build.sh  (not part of `all`)
#   tools/fill_h2h4_test.sh all [TAG]              # selftest, run, fetch, check; stops at the first failing step.  TAG default fill40
#   (or the steps one by one: selftest | run | fetch | check [TAG];  show [TAG] prints the six `run` commands and runs nothing)
#
# The route (task-40 review, findings 1 and 3): input/route-story-h2.txt, the first part of the story route.  It holds the
# map-transition fade at the field arrival (band pure black about presents 880-934, then the fade-in) and the window of presents about
# 1103-1165 in which an effect lies in the top strip of a tall screen: the 16:9 cull removes it (H2 off: not drawn), H2 draws it.  The
# default census route (route-field-battle.txt) has no such moment (task 40: H2 changed no strip pixel there), so it cannot show H2 at
# work.  The route is done at about game frame 1,382; runs of FILL_GAME_FRAMES (1600) game frames finish it.  Its file accesses on the
# full Linux tree (HIT / MISS by name) are a subset of the census route's, so the Mac's route-data subset holds what it reads.
#
# Steps (TAG=fill40):
#   selftest  tools/darwin_route.sh selftest fill40-null ; ... selftest fill40-gfx KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system ...
#             both must print "kh: aspect selftest: N checks, 0 failures" with N >= 378 (the task-40 suite after its review)
#   run       six runs of input/route-story-h2.txt, difficulty 3, FILL_GAME_FRAMES game frames, the frames clock, the vkd3d bridge on
#             the Metal device, the hash log and the band log of every present, the size records, dumps of presents FILL_FROM..FILL_TO
#             every FILL_EVERY (920..1160 every 40: the fade-in, the field, the effect window; pictures only):
#     G="--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn"
#     P="--env KHNMC_CLOCK=frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_GFX_BANDLOG=@OUT@/band.tsv --env KHNMC_ASPECT_LOG=1
#        --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_DUMP_FROM=920 --env KHNMC_GFX_DUMP_TO=1160 --env KHNMC_GFX_DUMP_EVERY=40"
#     fill40-tall    $G $P --env KHNMC_SCREEN=1920x1350 --env KHNMC_ASPECT=fill                    (the mode as played: H1 gated, H2/H4/H5 on)
#     fill40-nocull  ... the same + --env KHNMC_ASPECT_TEST_NOCULL=1    (test aid: H2 divides m11 by 2047/208: nothing culled vertically)
#     fill40-offh2   ... the same + --env KHNMC_ASPECT_TEST_OFF=h2      (test aid: H2b and H2c pass through: the 16:9 cull, the H2 control)
#     fill40-offh4   ... the same + --env KHNMC_ASPECT_TEST_OFF=h4      (test aid: H4 passes through: the fades over the band only, the H4 control)
#     fill40-ref     $G $P                                                  (1920x1080, nothing set; another width W: KHNMC_SCREEN=Wx(9W/16))
#     fill40-f169    $G $P --env KHNMC_ASPECT=fill                         (fill on the 16:9 screen; KHNMC_SCREEN as for ref)
#   fetch     tools/darwin_route.sh fetch <run> hash.tsv (all six) and band.tsv (tall, nocull, offh2, offh4); the frame index and the dumps
#             of tall, nocull, offh2 and offh4 (7 each at 7.8 MB with the defaults)
#   check     (exit 0 only when every line holds; tools/fill_compare.py --help has the definitions; nothing pairs runs by present number
#             except 5, which compares only while both runs have drawn the same)
#     1 fill_compare.py runcheck tall/run.out fill 1920x1350     H1 gated and rewriting, H2b/H2c dividing by r and restoring at every
#                                                              call, H4 rewriting, no TEST AID line, the built size right, no WARNING
#       fill_compare.py runcheck nocull/run.out nocull 1920x1350 the same, but the TEST AID line names the no-cull reference and every H2
#                                                              call divided by 2047/208 (a run whose aid did not take effect fails)
#       fill_compare.py runcheck offh2/run.out off 1920x1350 h2  the TEST AID line is "H2b off; H2c off;", every H2 call passed through
#       fill_compare.py runcheck offh4/run.out off 1920x1350 h4  the TEST AID line is "H4 off;", H4 rewrote nothing, H2 at work
#       fill_compare.py runcheck f169/run.out fill169           fill at 16:9: no narrow call, every H2 call "r = 1, nothing written",
#                                                              H4 / H5 never wrote (the 16:9 identity, by state)
#       fill_compare.py runcheck ref/run.out ref                pc, no narrow call, no KHNMC_SCREEN (1920)
#     2 fill_compare.py tallcheck tall/band.tsv   PASS: (f) every present with a pure black band has pure black strips (the fade covers
#                                                 the whole frame), (s) no hard line within W = ceil(2 x 1080 / 416) rows at or beyond a
#                                                 band edge where band and strips are lit
#     3 fill_compare.py tallcheck offh4/band.tsv  must FAIL on (f) AND on (s): the control that the window holds a full fade and a
#                                                 partial one (without H4 the strips keep the scene, and the fade-in draws its line about
#                                                 3 rows outside the band); if either passes, that half of check 2 says nothing: failure
#     4 fill_compare.py h2pictures tall/band.tsv nocull/band.tsv offh2/band.tsv
#                                                 pairs by band hash (the same moment, any present number).  C = moments where the
#                                                 control's strips differ from TALL's (H2 shows what the 16:9 cull removes), E = moments
#                                                 where TALL's strips differ from the no-cull reference's.  Exit 0 PASS (C not empty, E
#                                                 empty) or 4 NOT EXACT (C not empty, E only within 2 presents of C) are accepted, 4 with a
#                                                 loud line: on lavapipe this window gives NOT EXACT, because the reference draws the
#                                                 effect from the field arrival on while it is off the screen, so its animation runs from
#                                                 another start (implementation notes review section); judge the dumps.  3 NOT DEMONSTRATED (C
#                                                 empty: H2 not seen at work) and 1 FAIL (E elsewhere, or too few pairs) fail the check.
#     5 fill_compare.py hashprefix ref/hash.tsv f169/hash.tsv
#                                                 16:9 pictures unchanged: compared by present number only up to the first present whose
#                                                 cumulative draw count differs.  0 PASS (equal through present 970 or later: the title,
#                                                 menus and the fade) and 3 UNDECIDED (the runs parted earlier; the identity then rests on
#                                                 check 1's fill169, printed as a NOTE) are accepted; 1 FAIL (a picture differs while
#                                                 both runs drew the same) fails the check.  (The old `hashcontent` share is not used: it
#                                                 fails correct runs that diverge.)
#     Measured on Linux (lavapipe, task-40 review, run-Z40/gpu/h2-*, this route and these six runs; implementation notes): see the review
#     section of implementation notes for each line's result.
# Environment knobs: FILL_SCREEN (default 1920x1350; narrower than 16:9 with a whole-row band offset, as fill_first_test.sh),
# FILL_FROM / FILL_TO / FILL_EVERY, FILL_GAME_FRAMES (default 1600), KHNMC_MAC, KHNMC_MAC_MIN_FREE, KHNMC_MAC_NICE (darwin_route.sh).
# Rules: one khnmc run at a time on the Mac mini (darwin_route.sh refuses otherwise); everything there stays in ~/$KHNMC_MAC_DIR; nothing
# is deleted there; a TAG whose results exist here or there is refused; nothing is retried.  KHNMC_GFX_DRIVER=system is the MAC's Metal
# device: on Linux the runtime's lavapipe guard stays as it is (this script only drives the Mac).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
die(){ echo "fill_h2h4_test: $*" >&2; exit 1; }
[ -n "${KHNMC_MAC_DIR:-}" ] || { echo "fill_h2h4_test: set KHNMC_MAC_DIR to the Mac build folder with the merged task-40 sources (e.g. khnmc-work/build-l21b); it is never guessed" >&2; exit 2; }
export KHNMC_RUN_DIR=${KHNMC_RUN_DIR:-$PWD/run-fill40}
RUN=$KHNMC_RUN_DIR
MAC=${KHNMC_MAC:-user@mac-mini}
SCREEN=${FILL_SCREEN:-1920x1350}; FROM=${FILL_FROM:-920}; TO=${FILL_TO:-1160}; EVERY=${FILL_EVERY:-40}; GFR=${FILL_GAME_FRAMES:-1600}
for v in "$FROM" "$TO" "$EVERY" "$GFR"; do case "$v" in ''|*[!0-9]*) echo "fill_h2h4_test: FILL_FROM / FILL_TO / FILL_EVERY / FILL_GAME_FRAMES must be numbers" >&2; exit 2;; esac; done
[[ $SCREEN =~ ^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}$ ]] || { echo "fill_h2h4_test: FILL_SCREEN=$SCREEN is not WxH (decimal, no leading zero)" >&2; exit 2; }
SW=${SCREEN%x*}; SH=${SCREEN#*x}
[ $(( (16*SH - 9*SW) % 32 )) -eq 0 ] && [ $((16*SH)) -gt $((9*SW)) ] || { echo "fill_h2h4_test: FILL_SCREEN=$SCREEN: narrower than 16:9 with a whole-row band offset (H - 9W/16) / 2 is required" >&2; exit 2; }
RH=$(( 9*SW/16 )); REFSCREEN=(); RCHK=()
[ "$SW" -eq 1920 ] || { REFSCREEN=(--env "KHNMC_SCREEN=${SW}x$RH"); RCHK=("${SW}x$RH"); }
[ "$TO" -ge "$FROM" ] && [ "$EVERY" -ge 1 ] && [ "$GFR" -gt "$TO" ] || { echo "fill_h2h4_test: need FILL_FROM <= FILL_TO < FILL_GAME_FRAMES and FILL_EVERY >= 1" >&2; exit 2; }
STEP=${1:-}; TAG=${2:-fill40}
case "$TAG" in ''|*[!A-Za-z0-9._-]*) echo "fill_h2h4_test: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac
RUNS=(tall nocull offh2 offh4 ref f169)
TALLRUNS=(tall nocull offh2 offh4)
DR="$HERE/tools/darwin_route.sh"; FC="$HERE/tools/fill_compare.py"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
G=(--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn)
P=(--env KHNMC_CLOCK=frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_GFX_BANDLOG=@OUT@/band.tsv --env KHNMC_ASPECT_LOG=1
   --env KHNMC_GFX_DUMP=@OUT@/frames --env "KHNMC_GFX_DUMP_FROM=$FROM" --env "KHNMC_GFX_DUMP_TO=$TO" --env "KHNMC_GFX_DUMP_EVERY=$EVERY")
ROUTE=(--input input/route-story-h2.txt --difficulty 3 --game-frames "$GFR")
TALLENV=(--env "KHNMC_SCREEN=$SCREEN" --env KHNMC_ASPECT=fill)
runargs(){ case "$1" in
  tall)   echo "${TALLENV[*]}";;
  nocull) echo "${TALLENV[*]} --env KHNMC_ASPECT_TEST_NOCULL=1";;
  offh2)  echo "${TALLENV[*]} --env KHNMC_ASPECT_TEST_OFF=h2";;
  offh4)  echo "${TALLENV[*]} --env KHNMC_ASPECT_TEST_OFF=h4";;
  ref)    echo "${REFSCREEN[*]}";;
  f169)   echo "${REFSCREEN[*]} --env KHNMC_ASPECT=fill";;
  esac; }
need_py(){ python3 -c 'import numpy' 2>/dev/null || die "python3 with numpy is needed here for tools/fill_compare.py"; }
step_build(){ echo "fill_h2h4_test: build: JOBS=${JOBS:-4} MAKE_ARGS=GFX_VKD3D=1 tools/darwin_build.sh ($KHNMC_MAC_DIR)"
  JOBS=${JOBS:-4} MAKE_ARGS="GFX_VKD3D=1" "$HERE/tools/darwin_build.sh" || die "build failed"; }
step_selftest(){
  mkdir -p "$RUN"
  "$DR" selftest "$TAG-null" || die "selftest (null backend) failed: $RUN/mac-selftest-$TAG-null.out"
  "$DR" selftest "$TAG-gfx" KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system VKD3D_DEBUG=warn VKD3D_SHADER_DEBUG=warn || die "selftest (vkd3dw on the Metal device) failed: $RUN/mac-selftest-$TAG-gfx.out"
  local f l n
  for k in null gfx; do f="$RUN/mac-selftest-$TAG-$k.out"
    l=$(grep -a "^kh: aspect selftest: [0-9]* checks, 0 failures" "$f" | tail -1) || true
    [ -n "$l" ] || die "$f has no 'kh: aspect selftest: N checks, 0 failures' line: the suite failed or is missing"
    n=$(echo "$l" | sed 's/^kh: aspect selftest: \([0-9]*\) checks.*/\1/')
    [ "$n" -ge 378 ] || die "$f: the aspect suite has $n checks (task 40's after its review has 378): the Mac build lacks the task-40 code"
    echo "fill_h2h4_test: $k: $l"; done; }
step_show(){ echo "fill_h2h4_test: TALL $SCREEN (band offset $(( (SH-RH)/2 )) rows), REF ${SW}x$RH; nothing is run"
  for r in "${RUNS[@]}"; do echo "tools/darwin_route.sh run $TAG-$r ${ROUTE[*]} ${G[*]} ${P[*]} $(runargs $r)"; done; }
step_run(){
  for r in "${RUNS[@]}"; do [ ! -e "$RUN/mac-$TAG-$r" ] || die "$RUN/mac-$TAG-$r exists: choose another TAG (an old frame index would be appended to)"; done
  local t=""; for r in "${RUNS[@]}"; do t="$t test ! -e ~/$KHNMC_MAC_DIR/run/$TAG-$r &&"; done
  "${SSH[@]}" "$MAC" "$t true" || die "run/$TAG-* exists on the Mac mini (or it cannot be reached): choose another TAG"
  for r in "${RUNS[@]}"; do
    echo "fill_h2h4_test: run $TAG-$r: $(runargs $r)"
    # shellcheck disable=SC2046
    "$DR" run "$TAG-$r" "${ROUTE[@]}" "${G[@]}" "${P[@]}" $(runargs $r) || die "run $TAG-$r failed ($RUN/mac-$TAG-$r/run.out)"
  done; }
step_fetch(){
  for r in "${RUNS[@]}"; do "$DR" fetch "$TAG-$r" hash.tsv || die "no hash log of $TAG-$r"; done
  for r in "${TALLRUNS[@]}"; do "$DR" fetch "$TAG-$r" band.tsv || die "no band log of $TAG-$r"; done
  local ps
  for r in "${TALLRUNS[@]}"; do "$DR" frames "$TAG-$r" || die "no frame index of $TAG-$r"
    ps=$(awk -F'\t' 'NR>1{print $1}' "$RUN/mac-$TAG-$r/frames/index.tsv" | tr '\n' ' ')
    # shellcheck disable=SC2086
    [ -z "$ps" ] || "$DR" frames "$TAG-$r" $ps || die "fetching the $TAG-$r dumps failed"; done; }
step_check(){
  need_py
  local rep="$RUN/$TAG-check.txt" rc=0 d="$RUN/mac-$TAG" notes=""
  : > "$rep" || die "cannot write $rep"
  say(){ echo "$*" | tee -a "$rep"; }
  # chk WANT ARGS...: WANT pass = exit 0; failfs = exit 1 with the first line ': FAIL' and both the (f) and the (s) line failing
  # (tallcheck's control); h2 = exit 0, or 4 with a NOTE (h2pictures); prefix = exit 0, or 3 with a NOTE (hashprefix)
  chk(){ local want=$1; shift; local out r; out=$(python3 "$FC" "$@" 2>&1); r=$?; echo "$out" >> "$rep"
    case "$want" in
    pass) if [ $r -ne 0 ]; then say "FAIL (exit $r): fill_compare.py $*"; rc=1; else say "ok (PASS as required): fill_compare.py $*"; fi;;
    failfs) if [ $r -eq 1 ] && echo "$out" | head -1 | grep -q ': FAIL$' && echo "$out" | grep -q '^  (f) .*: FAIL' && echo "$out" | grep -q '^  (s) .*: FAIL'; then
              say "ok (FAIL on (f) and on (s), as the control must): fill_compare.py $*"
            else say "FAIL (the control did not fail on both (f) and (s) (exit $r): the window holds no full fade or no partial one, or the input is missing; check 2 then says nothing): fill_compare.py $*"; rc=1; fi;;
    h2) case $r in
          0) say "ok (PASS: H2 shows what the 16:9 cull removes, and the strips equal the no-cull reference): fill_compare.py $*";;
          4) say "ok, NOT EXACT (H2 visibly at work; the strips differ from the no-cull reference only where H2 works: judge the dumps $RUN/$TAG-png/): fill_compare.py $*"
             notes="$notes H2-picture-NOT-EXACT";;
          *) say "FAIL (exit $r: $(echo "$out" | head -1 | sed 's/.*: //')): fill_compare.py $*"; rc=1;;
        esac;;
    prefix) case $r in
          0) say "ok (PASS): fill_compare.py $*";;
          3) say "ok, UNDECIDED (the runs parted before present 970: the 16:9 identity rests on 'runcheck f169 fill169' above): fill_compare.py $*"
             notes="$notes 16:9-pictures-UNDECIDED";;
          *) say "FAIL (exit $r): fill_compare.py $*"; rc=1;;
        esac;;
    esac; }
  chk pass runcheck "$d-tall/run.out" fill "$SCREEN"
  chk pass runcheck "$d-nocull/run.out" nocull "$SCREEN"
  chk pass runcheck "$d-offh2/run.out" off "$SCREEN" h2
  chk pass runcheck "$d-offh4/run.out" off "$SCREEN" h4
  chk pass runcheck "$d-f169/run.out" fill169 "${RCHK[@]}"
  chk pass runcheck "$d-ref/run.out" ref "${RCHK[@]}"
  chk pass tallcheck "$d-tall/band.tsv"
  chk failfs tallcheck "$d-offh4/band.tsv"
  chk h2 h2pictures "$d-tall/band.tsv" "$d-nocull/band.tsv" "$d-offh2/band.tsv"
  chk prefix hashprefix "$d-ref/hash.tsv" "$d-f169/hash.tsv"
  if python3 -c 'import PIL' 2>/dev/null; then mkdir -p "$RUN/$TAG-png"
    for r in "${TALLRUNS[@]}"; do for f in "$d-$r"/frames/frame-*.ppm; do [ -e "$f" ] || continue
      python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$f" "$RUN/$TAG-png/$r-$(basename "$f" .ppm).png" || say "fill_h2h4_test: NOTE: $f could not be converted"; done; done
    say "fill_h2h4_test: pictures (tall = as played, nocull = the no-cull reference, offh2 = without H2, offh4 = without H4) in $RUN/$TAG-png/"
  else say "fill_h2h4_test: NOTE: no Pillow here: no PNG copies (the checks are complete)"; fi
  say "fill_h2h4_test: report $rep; $([ $rc = 0 ] && echo "every check PASSED${notes:+ (with:$notes; see above)}" || echo "NOT PASSED (see the report)")"
  return $rc; }
case "$STEP" in
build) step_build;;
selftest) step_selftest;;
run) step_run;;
fetch) step_fetch;;
check) step_check; exit $?;;
show) step_show;;
all) step_selftest; step_run; step_fetch; step_check; exit $?;;
*) sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2;;
esac
