#!/usr/bin/env bash
# tools/fill_h3_test.sh : the GPU check of the "fill" mode's H3 (the HUD anchored to the edges of the tall frame; task 42,
# NATIVE-FILL-PLAN.md section 5 "H3 hook points (corrected)" and section 8 step 6) on the Mac mini, driven from the Linux worktree with
# tools/darwin_route.sh.  A sibling of tools/fill_h2h4_test.sh (task 40) with the same refusals and knobs.  Written for the MAIN
# session: run it after the task-42 sources are merged and built there.  GPU runs are NOT frame-identical past the field (fence timing:
# task 36 on lavapipe, the first test on the Mac), so no condition here needs two runs to be the same run: the pictures are paired by
# CONTENT (fill_compare.py h3pictures: a dump of the H3 run and a dump of its control are the same moment when the middle of the
# picture, where no anchored element is drawn at either place, is byte-identical, whatever their present numbers), and the hooks are
# checked by STATE (each run's own H3 report lines).
#
#   export KHNMC_MAC_DIR=khnmc-work/build-<x>      # REQUIRED: the Mac build folder that has the merged task-42 sources
#   export KHNMC_RUN_DIR=$PWD/run-fill42           # optional (default $PWD/run-fill42): results here
#   tools/fill_h3_test.sh build                    # = JOBS=${JOBS:-4} MAKE_ARGS="GFX_VKD3D=1" tools/darwin_build.sh  (not part of `all`)
#   tools/fill_h3_test.sh all [TAG]                # selftest, run, fetch, check; stops at the first failing step.  TAG default fill42
#   (or the steps one by one: selftest | run | fetch | check [TAG];  show [TAG] prints the `run` commands and runs nothing;
#    phone [TAG]: ONE extra run at the owner's screen, 2670x1878, with a few dumps as PNG, pictures only, not part of `all`)
#
# The route: the census route input/route-field-battle.txt (the default of darwin_route.sh), difficulty 3, FILL_GAME_FRAMES (2400, as
# tools/fill_first_test.sh: the route must also finish, route_one.py's verdict) game frames, the frames clock.  It holds the field
# arrival (from game frame ~846: the command menu, the player gauges and the face), the INFORMATION bar with its scrolling text (from
# ~930; its text is scissored), the battle (from ~1117: the enemy gauge, the lock-on, the mod's style rank).
#
# Steps (TAG=fill42):
#   selftest  tools/darwin_route.sh selftest fill42-null ; ... selftest fill42-gfx KHNMC_GFX=vkd3dw KHNMC_GFX_DRIVER=system ...
#             both must print "kh: aspect selftest: N checks, 0 failures" with N >= 733 (the task-42 suite with its review fixes)
#   run       three runs, the vkd3d bridge on the Metal device, the size records, dumps of presents FILL_FROM..FILL_TO every FILL_EVERY
#             (1100..1320 every present by default: 221 dumps of 7.8 MB per tall / offh3 run, ~1.7 GB each to fetch; the battle from ~1150
#             holds all four anchored elements; every present, because two drifting runs show one moment at present numbers whose
#             difference can be odd):
#     G="--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn"
#     P="--env KHNMC_CLOCK=frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_ASPECT_LOG=1
#        --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_DUMP_FROM=1100 --env KHNMC_GFX_DUMP_TO=1320 --env KHNMC_GFX_DUMP_EVERY=1"
#     fill42-tall    $G $P --env KHNMC_SCREEN=1920x1350 --env KHNMC_ASPECT=fill                    (the mode as played: H1..H5 on)
#     fill42-offh3   ... the same + --env KHNMC_ASPECT_TEST_OFF=h3      (test aid: H3 passes every dk pass through: the HUD in the band)
#     fill42-f169    $G --env KHNMC_CLOCK=frames --env KHNMC_ASPECT_LOG=1 --env KHNMC_ASPECT=fill   (fill on the 16:9 screen, no dumps)
#   fetch     tools/darwin_route.sh fetch <run> hash.tsv (all three); the frame index and the dumps of tall and offh3
#   check     (exit 0 only when every line holds; tools/fill_compare.py --help has the definitions)
#     1 fill_compare.py runcheck tall/run.out fill 1920x1350   H1 / H2 / H4 at work as in task 40 (no TEST AID line, no WARNING)
#       fill_compare.py runcheck tall/run.out h3              H3: anchored dk passes, objects at the top AND at the bottom, no
#                                                              h3-band-mismatch, no pass begun inside another, no class whose group
#                                                              disagrees, no test aid
#       fill_compare.py runcheck offh3/run.out off 1920x1350 h3   the TEST AID line is "H3 off;", H2 / H4 at work
#       fill_compare.py runcheck offh3/run.out h3off          every dk pass "test aid off", no object anchored
#       fill_compare.py runcheck f169/run.out fill169         fill at 16:9: H1..H5 wrote nothing (task 40)
#       fill_compare.py runcheck f169/run.out h3169           every dk pass "o = 0, nothing written": the 16:9 identity of H3, by state
#     2 fill_compare.py h3static tall/frames offh3/frames 1920x1350    PASS, WITHOUT shared moments (GPU runs drift apart): per element
#                                                              its footprint (the pixels OFF shows in at least half of its dumps and
#                                                              TALL does not show at that place) reappears in TALL exactly o rows
#                                                              higher (INFORMATION bar, enemy gauge: GS y 0 at frame row 0) or lower
#                                                              (command menu, player gauges / face: GS y 416 at the last row), the
#                                                              strict best of the shifts tried, 0 and the opposite edge included;
#                                                              (c) the INFORMATION text not cut: its white pixels (not the bar's
#                                                              static title) per dump where the bar is drawn, TALL's median at least
#                                                              0.8 of OFF's; (r) the centred style rank found at its place (shift 0),
#                                                              not o rows away (task 42 review finding 3: without these a drifted
#                                                              pair said nothing about a cut text or a moved centred element)
#     3 fill_compare.py h3pictures tall/frames offh3/frames 1920x1350   per SHARED moment (the same picture in the middle): the same
#                                                              edges per element, the INFORMATION text (its white pixels) not cut by
#                                                              its scissor, and everything else (lock-on, damage numbers, style rank,
#                                                              3D) unchanged.  0 PASS; 3 UNDECIDED (fewer than FILL_MIN_PAIRS (20)
#                                                              shared moments and nothing failing in them: the runs drifted apart)
#                                                              is accepted with a NOTE: the text and the centred element are then
#                                                              judged by check 2's (c) and (r) without shared moments; 1 FAIL fails
#     4 fill_compare.py h3static offh3/frames offh3/frames 1920x1350   must FAIL: the control (no element moved: no footprint), so
#                                                              check 2 cannot pass on pictures where nothing moved
#     Measured on Linux (lavapipe; implementation notes section 4): a pair that stayed in step (run-B42/gpu/g-*: h3pictures PASS with 183
#     shared moments) and a pair that drifted apart under load (g3-*: h3static PASS, h3pictures UNDECIDED with 0 shared moments);
#     the check emulation in implementation notes section 5.
# Environment knobs: FILL_SCREEN (default 1920x1350; narrower than 16:9 with a whole-row band offset), FILL_FROM / FILL_TO / FILL_EVERY,
# FILL_GAME_FRAMES (default 2400), FILL_MIN_PAIRS (default 20), KHNMC_MAC, KHNMC_MAC_MIN_FREE, KHNMC_MAC_NICE (darwin_route.sh).
# Rules: one khnmc run at a time on the Mac mini (darwin_route.sh refuses otherwise); everything there stays in ~/$KHNMC_MAC_DIR; nothing
# is deleted there; a TAG whose results exist here or there is refused; nothing is retried.  KHNMC_GFX_DRIVER=system is the MAC's Metal
# device: on Linux the runtime's lavapipe guard stays as it is (this script only drives the Mac).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
die(){ echo "fill_h3_test: $*" >&2; exit 1; }
[ -n "${KHNMC_MAC_DIR:-}" ] || { echo "fill_h3_test: set KHNMC_MAC_DIR to the Mac build folder with the merged task-42 sources (e.g. khnmc-work/build-l21b); it is never guessed" >&2; exit 2; }
export KHNMC_RUN_DIR=${KHNMC_RUN_DIR:-$PWD/run-fill42}
RUN=$KHNMC_RUN_DIR
MAC=${KHNMC_MAC:-user@mac-mini}
SCREEN=${FILL_SCREEN:-1920x1350}; FROM=${FILL_FROM:-1100}; TO=${FILL_TO:-1320}; EVERY=${FILL_EVERY:-1}; GFR=${FILL_GAME_FRAMES:-2400}; MINP=${FILL_MIN_PAIRS:-20}
for v in "$FROM" "$TO" "$EVERY" "$GFR" "$MINP"; do case "$v" in ''|*[!0-9]*) echo "fill_h3_test: FILL_FROM / FILL_TO / FILL_EVERY / FILL_GAME_FRAMES / FILL_MIN_PAIRS must be numbers" >&2; exit 2;; esac; done
[[ $SCREEN =~ ^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}$ ]] || { echo "fill_h3_test: FILL_SCREEN=$SCREEN is not WxH (decimal, no leading zero)" >&2; exit 2; }
SW=${SCREEN%x*}; SH=${SCREEN#*x}
[ $(( (16*SH - 9*SW) % 32 )) -eq 0 ] && [ $((16*SH)) -gt $((9*SW)) ] || { echo "fill_h3_test: FILL_SCREEN=$SCREEN: narrower than 16:9 with a whole-row band offset (H - 9W/16) / 2 is required (h3pictures compares exact pixels at a whole-row shift)" >&2; exit 2; }
RH=$(( 9*SW/16 )); REFSCREEN=()
[ "$SW" -eq 1920 ] || REFSCREEN=(--env "KHNMC_SCREEN=${SW}x$RH")
[ "$TO" -ge "$FROM" ] && [ "$EVERY" -ge 1 ] && [ "$GFR" -gt "$TO" ] || { echo "fill_h3_test: need FILL_FROM <= FILL_TO < FILL_GAME_FRAMES and FILL_EVERY >= 1" >&2; exit 2; }
STEP=${1:-}; TAG=${2:-fill42}
case "$TAG" in ''|*[!A-Za-z0-9._-]*) echo "fill_h3_test: TAG may hold letters, digits, . _ - only" >&2; exit 2;; esac
RUNS=(tall offh3 f169)
PICRUNS=(tall offh3)
DR="$HERE/tools/darwin_route.sh"; FC="$HERE/tools/fill_compare.py"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
G=(--env KHNMC_GFX=vkd3dw --env KHNMC_GFX_DRIVER=system --env VKD3D_DEBUG=warn --env VKD3D_SHADER_DEBUG=warn)
P=(--env KHNMC_CLOCK=frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_ASPECT_LOG=1
   --env KHNMC_GFX_DUMP=@OUT@/frames --env "KHNMC_GFX_DUMP_FROM=$FROM" --env "KHNMC_GFX_DUMP_TO=$TO" --env "KHNMC_GFX_DUMP_EVERY=$EVERY")
P169=(--env KHNMC_CLOCK=frames --env KHNMC_GFX_HASHLOG=@OUT@/hash.tsv --env KHNMC_ASPECT_LOG=1)
ROUTE=(--difficulty 3 --game-frames "$GFR")
TALLENV=(--env "KHNMC_SCREEN=$SCREEN" --env KHNMC_ASPECT=fill)
runargs(){ case "$1" in
  tall)  echo "${P[*]} ${TALLENV[*]}";;
  offh3) echo "${P[*]} ${TALLENV[*]} --env KHNMC_ASPECT_TEST_OFF=h3";;
  f169)  echo "${P169[*]} ${REFSCREEN[*]} --env KHNMC_ASPECT=fill";;
  esac; }
need_py(){ python3 -c 'import numpy' 2>/dev/null || die "python3 with numpy is needed here for tools/fill_compare.py"; }
step_build(){ echo "fill_h3_test: build: JOBS=${JOBS:-4} MAKE_ARGS=GFX_VKD3D=1 tools/darwin_build.sh ($KHNMC_MAC_DIR)"
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
    [ "$n" -ge 733 ] || die "$f: the aspect suite has $n checks (task 42's with its review fixes has 733): the Mac build lacks the task-42 code or its review fixes"
    echo "fill_h3_test: $k: $l"; done; }
step_show(){ echo "fill_h3_test: TALL $SCREEN (band offset $(( (SH-RH)/2 )) rows), 16:9 ${SW}x$RH; nothing is run"
  for r in "${RUNS[@]}"; do echo "tools/darwin_route.sh run $TAG-$r ${ROUTE[*]} ${G[*]} $(runargs $r)"; done
  echo "(phone: tools/darwin_route.sh run $TAG-phone ${ROUTE[*]} ${G[*]} --env KHNMC_CLOCK=frames --env KHNMC_ASPECT_LOG=1 --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_DUMP_FROM=990 --env KHNMC_GFX_DUMP_TO=1210 --env KHNMC_GFX_DUMP_EVERY=110 --env KHNMC_SCREEN=2670x1878 --env KHNMC_ASPECT=fill)"; }
step_run(){
  for r in "${RUNS[@]}"; do [ ! -e "$RUN/mac-$TAG-$r" ] || die "$RUN/mac-$TAG-$r exists: choose another TAG (an old frame index would be appended to)"; done
  local t=""; for r in "${RUNS[@]}"; do t="$t test ! -e ~/$KHNMC_MAC_DIR/run/$TAG-$r &&"; done
  "${SSH[@]}" "$MAC" "$t true" || die "run/$TAG-* exists on the Mac mini (or it cannot be reached): choose another TAG"
  for r in "${RUNS[@]}"; do
    echo "fill_h3_test: run $TAG-$r: $(runargs $r)"
    # shellcheck disable=SC2046
    "$DR" run "$TAG-$r" "${ROUTE[@]}" "${G[@]}" $(runargs $r) || die "run $TAG-$r failed ($RUN/mac-$TAG-$r/run.out)"
  done; }
step_fetch(){
  for r in "${RUNS[@]}"; do "$DR" fetch "$TAG-$r" hash.tsv || die "no hash log of $TAG-$r"; done
  local ps
  for r in "${PICRUNS[@]}"; do "$DR" frames "$TAG-$r" || die "no frame index of $TAG-$r"
    ps=$(awk -F'\t' 'NR>1{print $1}' "$RUN/mac-$TAG-$r/frames/index.tsv" | tr '\n' ' ')
    # shellcheck disable=SC2086
    [ -z "$ps" ] || "$DR" frames "$TAG-$r" $ps || die "fetching the $TAG-$r dumps failed"; done; }
step_check(){
  need_py
  local rep="$RUN/$TAG-check.txt" rc=0 d="$RUN/mac-$TAG"
  : > "$rep" || die "cannot write $rep"
  say(){ echo "$*" | tee -a "$rep"; }
  # chk WANT ARGS...: WANT pass = exit 0; fail = exit 1 with the first line ending ': FAIL' (a control); pairs = exit 0, or 3 with a NOTE
  local notes=""
  chk(){ local want=$1; shift; local out r; out=$(python3 "$FC" "$@" 2>&1); r=$?; echo "$out" >> "$rep"
    case "$want" in
    pass) if [ $r -ne 0 ]; then say "FAIL (exit $r): fill_compare.py $*"; rc=1; else say "ok (PASS as required): fill_compare.py $*"; fi;;
    fail) if [ $r -eq 1 ] && echo "$out" | head -1 | grep -q ': FAIL$'; then say "ok (FAIL, as the control must): fill_compare.py $*"
          else say "FAIL (the control did not fail (exit $r): check 2 would pass on pictures where nothing moved): fill_compare.py $*"; rc=1; fi;;
    pairs) case $r in
          0) say "ok (PASS at the shared moments): fill_compare.py $*";;
          3) say "ok, UNDECIDED (the runs share too few moments; the edges, the INFORMATION text (c) and the centred style rank (r) rest on h3static, check 2): fill_compare.py $*"
             notes="$notes h3pictures-UNDECIDED";;
          *) say "FAIL (exit $r): fill_compare.py $*"; rc=1;;
        esac;;
    esac; }
  chk pass runcheck "$d-tall/run.out" fill "$SCREEN"
  chk pass runcheck "$d-tall/run.out" h3
  chk pass runcheck "$d-offh3/run.out" off "$SCREEN" h3
  chk pass runcheck "$d-offh3/run.out" h3off
  chk pass runcheck "$d-f169/run.out" fill169
  chk pass runcheck "$d-f169/run.out" h3169
  chk pass h3static "$d-tall/frames" "$d-offh3/frames" "$SCREEN"
  chk pairs h3pictures "$d-tall/frames" "$d-offh3/frames" "$SCREEN" --min-pairs "$MINP" --png "$RUN/$TAG-png"
  chk fail h3static "$d-offh3/frames" "$d-offh3/frames" "$SCREEN"
  say "fill_h3_test: pictures of the first shared moments and of any failing one (h3-tall-pNNN = as played, h3-off-pNNN = H3 off) in $RUN/$TAG-png/"
  say "fill_h3_test: report $rep; $([ $rc = 0 ] && echo "every check PASSED${notes:+ (with:$notes; see above)}" || echo "NOT PASSED (see the report)")"
  return $rc; }
step_phone(){
  [ ! -e "$RUN/mac-$TAG-phone" ] || die "$RUN/mac-$TAG-phone exists: choose another TAG"
  "$DR" run "$TAG-phone" "${ROUTE[@]}" "${G[@]}" --env KHNMC_CLOCK=frames --env KHNMC_ASPECT_LOG=1 --env KHNMC_GFX_DUMP=@OUT@/frames --env KHNMC_GFX_DUMP_FROM=990 \
    --env KHNMC_GFX_DUMP_TO=1210 --env KHNMC_GFX_DUMP_EVERY=110 --env KHNMC_SCREEN=2670x1878 --env KHNMC_ASPECT=fill || die "run $TAG-phone failed ($RUN/mac-$TAG-phone/run.out)"
  "$DR" frames "$TAG-phone" || die "no frame index of $TAG-phone"
  local ps; ps=$(awk -F'\t' 'NR>1{print $1}' "$RUN/mac-$TAG-phone/frames/index.tsv" | tr '\n' ' ')
  # shellcheck disable=SC2086
  [ -z "$ps" ] || "$DR" frames "$TAG-phone" $ps || die "fetching the $TAG-phone dumps failed"
  python3 "$FC" runcheck "$RUN/mac-$TAG-phone/run.out" h3 || die "the phone run's H3 report does not pass runcheck h3"
  if python3 -c 'import PIL' 2>/dev/null; then mkdir -p "$RUN/$TAG-png"
    for f in "$RUN/mac-$TAG-phone"/frames/frame-*.ppm; do [ -e "$f" ] || continue
      python3 -c 'import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])' "$f" "$RUN/$TAG-png/phone-2670x1878-$(basename "$f" .ppm).png"; done
    echo "fill_h3_test: the owner's screen shape: $RUN/$TAG-png/phone-2670x1878-*.png"
  else echo "fill_h3_test: NOTE: no Pillow here: the dumps stay PPM in $RUN/mac-$TAG-phone/frames/"; fi; }
case "$STEP" in
build) step_build;;
selftest) step_selftest;;
run) step_run;;
fetch) step_fetch;;
check) step_check; exit $?;;
show) step_show;;
phone) step_phone;;
all) step_selftest; step_run; step_fetch; step_check; exit $?;;
*) sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2;;
esac
