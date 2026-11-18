#!/bin/bash
# tools/ios/mac/device_run.sh selftest [LIMIT_S]
# tools/ios/mac/device_run.sh route INPUT [GAME_FRAMES] [DIFFICULTY] [LIMIT_S]
# tools/ios/mac/device_run.sh play
#   run the signed app (mac/sign_app.sh) on the owner's own iPhone, over the network or the cable (the phone is paired with this
#   Mac; `xcrun devicectl list devices` shows it "available (paired)").  Run on the MacBook from ~/Developer/KHNMC/src, over ssh
#   is fine (devicectl needs no keychain).
#   route: the null-backend game with the scripted controller INPUT (a file of src/, e.g. input/route-story.txt) as Documents/input.txt,
#   KH_RUN_GAME_FRAMES (default 7000), DIFFICULTY none (default) or 0..3, on the game data that tools/ios/push_data.sh put into
#   Documents/data and Documents/pkgdir; phase.log and files.log come back with the log.
#   play: the game as the owner plays it: no script, the controller as pad 0, sound on, no time limit; started and left running.
#   After every selftest or route run the PLAY configuration is put back into Documents/khnmc.conf, so that tapping the app's icon
#   always starts the game, never the last test.
#   1. install the app; 2. put khnmc.conf (`arg --selftest`, log + status file), a fresh status file and kh2fm.exe into the app's
#      Documents; 3. start it, retrying every 20 s while iOS refuses because the phone is LOCKED (WAIT_UNLOCK_S, default 1800);
#   4. read Documents/khnmc.status every 15 s until "exit N" (LIMIT_S, default 1500), then copy khnmc.log back.
#   SKIP_INSTALL=1 skips steps 1 and 2 apart from the fresh status file (the app, the exe and the conf are already there).
#   SOUND=1 (route mode only): the game's sound plays through the speaker; otherwise every test run is silent (KHNMC_AUDIO=null).
# The phone's device id is read from ~/Developer/KHNMC/state/device.env (DEVICE=...), never from this repository.  Never names a
# simulator.  Results in ~/Developer/KHNMC/run/device-<mode>-<time>/.  The app must stay in the foreground with the screen on:
# iOS suspends it otherwise.
# First runs 2026-11-18 (iPhone, A20 Pro, iOS 27.2): 169,479 checks, 0 failures, about 16 s; 381 checks need fork() and are NOT RUN.
# GFX=1 (TASK38; IOS-GRAPHICS-PLAN steps 4-5): the signed app has the graphics backend (GFX=1 mac/build.sh iphoneos, GFX=1
#   mac/sign_app.sh; checked here) and the selftest or route runs on it: khnmc.conf gets env KHNMC_GFX=vkd3dw (the wrapped bridge; in an
#   Apple app build MoltenVK is the default driver), VKD3D_DEBUG=warn and VKD3D_SHADER_DEBUG=warn (vkd3d's err / fixme / warn lines go to
#   khnmc.log and are counted here), MVK_CONFIG_PERFORMANCE_TRACKING=1 (MoltenVK counts gpuMemoryAllocated, the census reads it, only
#   with it).  The device report (`kh: gfx device report:` block, the presentation-extensions line, any `kh: FATAL: gfx-...` line,
#   e.g. gfx-no-bc) is copied out of the log into device-report.txt; a GFX run without a complete block fails.
#   Route mode with GFX=1 only (plan step 5, M1): DUMP_EVERY=n -> every n-th presented picture into Documents/frames
#   (KHNMC_GFX_DUMP=@DOCS@/frames, KHNMC_GFX_DUMP_EVERY=n; frame-NNNNNN.ppm, about 6 MB each at 1920x1080, overwritten by later runs, plus
#   index.tsv, which the runtime appends to: its lines from before this run are counted first and only this run's are used);
#   FETCH="240 920 2000 2600" copies those presents' pictures back into frames/ (each a multiple of n, each must be in this run's index);
#   HASHLOG=1 -> KHNMC_GFX_HASHLOG=@DOCS@/hash.tsv, fetched; CLOCK=frames -> KHNMC_CLOCK=frames.  Each of these without GFX=1 route is
#   refused, and GFX=1 play is refused (no picture reaches the screen before plan step 10).  Without GFX=1 every run is as before.
# PRESENT=1 (TASK41, IOS-GRAPHICS-PLAN step 10; needs GFX=1): the picture on the phone's screen.  khnmc.conf also gets
#   env KHNMC_GFX_PRESENT=layer (the presenter draws into the game view's CAMetalLayer), env KHNMC_SCREEN=layer (the game's screen is the
#   layer's drawable: the shell hands its size over before the game starts) and env KHNMC_ASPECT=fill (task 37: the game fills that
#   screen); PRESENT_CHECK=n adds env KHNMC_GFX_PRESENT_CHECK=n (every n-th present checked against a readback, when the surface reports
#   TRANSFER_SRC).  The surface caps (`kh: gfx present: surface caps`), the swapchain lines and the life-cycle lines (`kh: app:`) are copied
#   into present-report.txt.  GFX=1 PRESENT=1 play is allowed: that conf stays on the phone (the icon then starts the game with the
#   picture); every later test run puts the null-backend play conf back.  Without PRESENT=1 nothing of this is written.
# GFX_APP=1|0 (review X38 n=1): the signed app must have (1) / must not have (0) the graphics backend, whatever backend the run uses;
#   checked with nm before anything is installed.  The plain (null-backend) selftest of the graphics app is `GFX_APP=1 device_run.sh
#   selftest`.  GFX=1 implies GFX_APP=1 (GFX=1 GFX_APP=0 is refused).  Unset: no check, as before.
. "$(dirname "$0")/lib.sh"
MODE=${1:-selftest}; INPUT=""; FRAMES=7000; DIFF=none
# task 39: SELFTEST_ONLY=membench|ordered|gate (environment, selftest mode only): one suite alone (KHNMC_SELFTEST_ONLY), e.g. the
# memory-ordering microbenchmark of an ORDERED_MEM=1 app against a plain one
SONLY=${SELFTEST_ONLY:-}; case "$SONLY" in ''|membench|ordered|gate|app) ;; *) echo "ios/mac: device_run: SELFTEST_ONLY must be membench, ordered, gate or app" >&2; exit 2;; esac
case "$MODE" in
  selftest) LIMIT=${2:-1500};;
  play) LIMIT=0;;
  route) INPUT=${2:-}; FRAMES=${3:-7000}; DIFF=${4:-none}; LIMIT=${5:-2400}
         [ -f "$INPUT" ] || { echo "ios/mac: device_run: route needs an input script of src/ (got '${INPUT}')" >&2; exit 2; }
         case "$DIFF" in none|0|1|2|3) ;; *) echo "ios/mac: device_run: DIFFICULTY is none or 0..3" >&2; exit 2;; esac;;
  *) sed -n '2,13p' "$0" >&2; exit 2;;
esac
DUMP_EVERY=${DUMP_EVERY:-}; FETCH=${FETCH:-}; HASHLOG=${HASHLOG:-0}; CLOCK=${CLOCK:-}
bad(){ echo "ios/mac: device_run: $*" >&2; exit 2; }
PRESENT=${PRESENT:-0}; PRESENT_CHECK=${PRESENT_CHECK:-}
case "$PRESENT" in 0|1) ;; *) bad "PRESENT is 0 or 1 (got '$PRESENT')";; esac
[ "$PRESENT" = 1 ] && [ "$GFX" != 1 ] && bad "PRESENT=1 needs GFX=1 (the picture comes from the vkd3d backend)"
[ -z "$PRESENT_CHECK" ] || { [ "$PRESENT" = 1 ] || bad "PRESENT_CHECK needs PRESENT=1"; case "$PRESENT_CHECK" in *[!0-9]*|0*) bad "PRESENT_CHECK must be a positive number of presents (got '$PRESENT_CHECK')";; esac; }
[ "$GFX" = 1 ] && [ "$PRESENT" != 1 ] && [ "$MODE" = play ] && bad "GFX=1 play needs PRESENT=1 (without it no picture reaches the screen); play stays on the null backend otherwise"
if [ -n "$DUMP_EVERY$FETCH$CLOCK" ] || [ "$HASHLOG" != 0 ]; then [ "$GFX" = 1 ] && [ "$MODE" = route ] || bad "DUMP_EVERY, FETCH, HASHLOG and CLOCK are for GFX=1 route runs only"; fi
case "$HASHLOG" in 0|1) ;; *) bad "HASHLOG is 0 or 1 (got '$HASHLOG')";; esac
case "$CLOCK" in ''|frames) ;; *) bad "CLOCK is frames or unset (got '$CLOCK')";; esac
[ -z "$DUMP_EVERY" ] || case "$DUMP_EVERY" in *[!0-9]*|0*) bad "DUMP_EVERY must be a positive number of presents (got '$DUMP_EVERY')";; esac
GFX_APP=${GFX_APP:-}; case "$GFX_APP" in ''|0|1) ;; *) bad "GFX_APP is 0 or 1 (got '$GFX_APP')";; esac
[ "$GFX" = 1 ] && [ "$GFX_APP" = 0 ] && bad "GFX=1 runs the graphics backend: GFX_APP=0 (an app without it) contradicts that"
[ "$GFX" = 1 ] && GFX_APP=1
for n in $FETCH; do case "$n" in *[!0-9]*|0*) bad "FETCH holds '$n', not a present number";; esac
  [ -n "$DUMP_EVERY" ] || bad "FETCH needs DUMP_EVERY"; [ $((n % DUMP_EVERY)) = 0 ] || bad "FETCH present $n is not a multiple of DUMP_EVERY=$DUMP_EVERY: it is never dumped"; done
[ -f "$STATE/device.env" ] && . "$STATE/device.env"
: "${DEVICE:?ios/mac: device_run: no DEVICE in $STATE/device.env}"
APP="$KROOT/build/app-signed/sym/Release-iphoneos/KHNMC.app"
OUT="$RUNS/device-$MODE-$(date -u +%Y%m%dT%H%M%SZ)"; mkdir -p "$OUT"
say(){ echo "ios/mac: device_run: $*" | tee -a "$OUT/steps.log"; }
cp_to(){ xcrun devicectl device copy to --device "$DEVICE" --timeout 300 --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
           --source "$1" --destination "Documents/$2" >> "$OUT/devicectl.log" 2>&1; }
cp_from(){ rm -f "$OUT/$1"; xcrun devicectl device copy from --device "$DEVICE" --timeout 120 --domain-type appDataContainer \
           --domain-identifier "$BUNDLE_ID" --source "Documents/$1" --destination "$OUT/$1" >> "$OUT/devicectl.log" 2>&1; }
[ -x "$APP/KHNMC" ] || { say "no signed app at $APP (mac/sign_app.sh first)"; exit 2; }
if [ -n "$GFX_APP" ]; then gfx_has "$APP/KHNMC"; hb=$?
  [ $hb = 2 ] && { say "nm could not read the signed app $APP/KHNMC: whether it has the graphics backend is unknown (GFX_APP=$GFX_APP): not run"; exit 2; }
  [ $hb = 1 ] && [ "$GFX_APP" = 1 ] && { say "GFX$([ "$GFX" = 1 ] && echo =1 || echo _APP=1), but the signed app has no graphics backend (GFX=1 mac/build.sh iphoneos, then GFX=1 mac/sign_app.sh)"; exit 2; }
  [ $hb = 0 ] && [ "$GFX_APP" = 0 ] && { say "GFX_APP=0, but the signed app HAS the graphics backend (mac/build.sh iphoneos, then mac/sign_app.sh, both without GFX=1)"; exit 2; }
  say "the signed app $([ $hb = 0 ] && echo 'has the' || echo 'has no') graphics backend, as GFX_APP=$GFX_APP asks; this run uses the $([ "$GFX" = 1 ] && echo 'vkd3d backend (KHNMC_GFX=vkd3dw)' || echo 'null backend')"; fi
printf 'starting\n' > "$OUT/status.init"
if [ "${SKIP_INSTALL:-0}" != 1 ]; then
  [ -f "$KROOT/pc/kh2fm.exe" ] || { say "no $KROOT/pc/kh2fm.exe (tools/ios/sync.sh exe)"; exit 2; }
  say "install"; xcrun devicectl device install app --device "$DEVICE" --timeout 300 "$APP" >> "$OUT/devicectl.log" 2>&1 \
    || { say "install FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  cp_to "$KROOT/pc/kh2fm.exe" kh2fm.exe || { say "copy of kh2fm.exe FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
fi
play_conf(){ { echo "# the game (tools/ios/mac/device_run.sh play): the controller is pad 0, sound on, no script, no time limit"; echo "log khnmc.log"; echo "status khnmc.status"; } > "$OUT/play.conf"; }
play_conf
# TASK41: the conf lines of a run with the picture on the screen (GFX=1 PRESENT=1)
gfx_lines(){ :; }; if [ "$GFX" = 1 ]; then gfx_lines(){ echo "env KHNMC_GFX=vkd3dw"; echo "env VKD3D_DEBUG=warn"; echo "env VKD3D_SHADER_DEBUG=warn"; echo "env MVK_CONFIG_PERFORMANCE_TRACKING=1"; }; fi
present_lines(){ echo "env KHNMC_GFX_PRESENT=layer"; echo "env KHNMC_SCREEN=layer"; echo "env KHNMC_ASPECT=fill"; [ -z "$PRESENT_CHECK" ] || echo "env KHNMC_GFX_PRESENT_CHECK=$PRESENT_CHECK"; }
if [ "$MODE" = play ] && [ "$PRESENT" = 1 ]; then
  { echo "# the game with its picture (tools/ios/mac/device_run.sh GFX=1 PRESENT=1 play): the controller is pad 0, sound on, no script"; echo "log khnmc.log"; echo "status khnmc.status"; gfx_lines; present_lines; } > "$OUT/gfx-play.conf"
  cp_to "$OUT/gfx-play.conf" khnmc.conf || { say "copy of the GFX play configuration FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1 || { say "launch FAILED (phone locked?)"; tail -5 "$OUT/devicectl.log"; exit 1; }
  say "the game is running on the phone with its picture (GFX=1 PRESENT=1 play); its log: Documents/khnmc.log (fetch it with SKIP_INSTALL=1 ... or devicectl copy from)"; echo "$OUT"; exit 0
fi
if [ "$MODE" = play ]; then
  cp_to "$OUT/play.conf" khnmc.conf || { say "copy of the play configuration FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1 || { say "launch FAILED (phone locked?)"; tail -5 "$OUT/devicectl.log"; exit 1; }
  say "the game is running on the phone (play mode); its log: Documents/khnmc.log"; echo "$OUT"; exit 0
fi
{ echo "# written by tools/ios/mac/device_run.sh ($MODE ${INPUT})"; echo "log khnmc.log"; echo "status khnmc.status"
  # no sound in test runs unless SOUND=1 (route mode): a selftest must never reach the speaker (2026-11-18: a test signal did)
  if [ "$MODE" = selftest ] || [ "${SOUND:-0}" != 1 ]; then echo "env KHNMC_AUDIO=null"; fi
  if [ "$MODE" = selftest ]; then echo "arg --selftest"; if [ -n "$SONLY" ]; then echo "env KHNMC_SELFTEST_ONLY=$SONLY"; fi
  else TAG=$(basename "$OUT"); echo "env KH_ROOT=@TMP@/$TAG/root"; echo "env KH_DOCS=@TMP@/$TAG/docs"   # fresh per run, as tools/route_batch.sh: a config the game saved in an earlier run changes how it boots
    echo "env KHNMC_INPUT_SCRIPT=@DOCS@/input.txt"; echo "env KH_RUN_GAME_FRAMES=$FRAMES"; echo "env KH_RUN_SECONDS=$((LIMIT-120))"
    echo "env KH_DUMP_THREADS=1"; echo "env KHNMC_PHASELOG=@DOCS@/phase.log"; echo "env KHNMC_FILELOG=@DOCS@/files.log"
    DV=KHNMC_DIFFICULTY; grep -qE '^[[:space:]]*@(wait|press|select|expect)([[:space:]]|$)' "$INPUT" && DV=KHNMC_ROUTE_DIFFICULTY   # as tools/route_batch.sh
    [ "$DIFF" = none ] || echo "env $DV=$DIFF"; fi
  if [ "$GFX" = 1 ]; then gfx_lines; [ "$PRESENT" = 1 ] && present_lines
    [ -z "$DUMP_EVERY" ] || { echo "env KHNMC_GFX_DUMP=@DOCS@/frames"; echo "env KHNMC_GFX_DUMP_EVERY=$DUMP_EVERY"; }
    [ "$HASHLOG" = 0 ] || echo "env KHNMC_GFX_HASHLOG=@DOCS@/hash.tsv"
    [ -z "$CLOCK" ] || echo "env KHNMC_CLOCK=$CLOCK"; fi
} > "$OUT/khnmc.conf"
IX0=0
if [ -n "$DUMP_EVERY" ]; then mkdir -p "$OUT/frames"
  if cp_from frames/index.tsv; then IX0=$(wc -l < "$OUT/frames/index.tsv" | tr -d ' '); mv "$OUT/frames/index.tsv" "$OUT/frames/index-before.tsv"; fi
  say "Documents/frames/index.tsv held $IX0 lines before this run (this run's are appended after them)"; fi
cp_to "$OUT/khnmc.conf" khnmc.conf || { say "copy of khnmc.conf FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
if [ "$MODE" = route ]; then cp "$INPUT" "$OUT/input.txt"; cp_to "$OUT/input.txt" input.txt || { say "copy of the input script FAILED"; exit 1; }; fi
cp_to "$OUT/status.init" khnmc.status || { say "copy of the status file FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
tw=$SECONDS; n=0
until xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1; do
  n=$((n+1)); [ $n = 1 ] && say "launch refused (phone locked?): retrying every 20 s"
  [ $((SECONDS-tw)) -ge "${WAIT_UNLOCK_S:-1800}" ] && { say "launch FAILED for $((SECONDS-tw)) s"; tail -5 "$OUT/devicectl.log"
    cp_to "$OUT/play.conf" khnmc.conf || say "WARNING: the play configuration could not be put back"; exit 1; }   # review r9: the icon starts the game, not this test
  sleep 20
done
say "launched after $n refused attempts"
t0=$SECONDS; st=""
while [ $((SECONDS-t0)) -lt "$LIMIT" ]; do
  sleep 15; cp_from khnmc.status; st=$(cat "$OUT/khnmc.status" 2>/dev/null)
  case "$st" in "exit "*) break;; esac
done
cp_from khnmc.log
if [ "$MODE" = route ]; then cp_from phase.log; cp_from files.log; fi
xrc=0
if [ "$GFX" = 1 ]; then
  grep -a -e '^kh: gfx device report: ' -e '^kh: gfx vkd3d: presentation extensions:' -e '^kh: FATAL: gfx-' "$OUT/khnmc.log" > "$OUT/device-report.txt" 2>/dev/null
  if grep -q '^kh: gfx device report: END (' "$OUT/device-report.txt"; then
    say "device report: $(grep -c '^kh: gfx device report: ' "$OUT/device-report.txt") lines in device-report.txt; $(grep -a 'BC decision' "$OUT/device-report.txt" | sed 's/^kh: gfx device report: //' | cut -c1-150)"
  else say "GFX=1, but khnmc.log has no complete device report block (device-report.txt holds what there is)"; xrc=1; fi
  f=$(grep -a -m1 '^kh: FATAL: gfx-' "$OUT/khnmc.log"); [ -z "$f" ] || say "$f"
  say "vkd3d lines in khnmc.log: err $(grep -a -c -E '^vkd3d:([0-9]+:)?err:' "$OUT/khnmc.log"), fixme $(grep -a -c -E '^vkd3d:([0-9]+:)?fixme:' "$OUT/khnmc.log"), warn $(grep -a -c -E '^vkd3d:([0-9]+:)?warn:' "$OUT/khnmc.log"); MoltenVK errors $(grep -a -c '\[mvk-error\]' "$OUT/khnmc.log")"
  if [ "$PRESENT" = 1 ]; then
    grep -a -e '^kh: gfx present: ' -e '^kh: app: ' -e '^kh: ios: ' -e '^kh: screen: ' -e '^kh: vsync: ' -e '^kh: gfx gate: ' "$OUT/khnmc.log" > "$OUT/present-report.txt" 2>/dev/null
    say "present report: $(grep -c 'surface caps' "$OUT/present-report.txt") surface-caps line(s), swapchains made/rebuilt $(grep -c -e 'swapchain made' -e 'swapchain rebuilt' "$OUT/present-report.txt"), $(grep -a -m1 '^kh: gfx present: mode=' "$OUT/khnmc.log" | cut -c1-160)"
    grep -q 'surface caps' "$OUT/present-report.txt" || { say "PRESENT=1, but the log has no surface-caps line: the presenter never made its swapchain"; xrc=1; }; fi
  if [ "$HASHLOG" = 1 ]; then if cp_from hash.tsv; then say "hash.tsv: $(( $(wc -l < "$OUT/hash.tsv") - 1 )) presents"; else say "hash.tsv could not be fetched"; xrc=1; fi; fi
  if [ -n "$DUMP_EVERY" ]; then
    if cp_from frames/index.tsv; then
      tail -n +$((IX0+1)) "$OUT/frames/index.tsv" | grep -v '^present' > "$OUT/frames/index-this-run.tsv"
      got=0; miss=""
      for n in $FETCH; do pf=$(printf 'frame-%06d.ppm' "$n")
        if awk -F'\t' -v n="$n" -v f="$pf" '$1==n && $2==f {ok=1} END {exit !ok}' "$OUT/frames/index-this-run.tsv" && cp_from "frames/$pf" && [ -s "$OUT/frames/$pf" ]; then got=$((got+1)); else miss="$miss $n"; fi; done
      say "this run dumped $(wc -l < "$OUT/frames/index-this-run.tsv" | tr -d ' ') pictures (every $DUMP_EVERY presents); fetched $got of $(echo $FETCH | wc -w | tr -d ' ') into frames/${miss:+; NOT fetched (not dumped by this run, or the copy failed):$miss}"
      [ -z "$miss" ] || xrc=1
    else say "Documents/frames/index.tsv could not be fetched: no picture of this run is known"; xrc=1; fi
  fi
fi
cp_to "$OUT/play.conf" khnmc.conf || say "WARNING: the play configuration could not be put back: the icon would start this test again"
say "status after $((SECONDS-t0)) s: '${st:-<none>}'; log lines: $(wc -l < "$OUT/khnmc.log" 2>/dev/null | tr -d ' ')"
echo "$OUT"
case "$st" in "exit 0") exit $xrc;; "exit "*) exit 1;; *) exit 124;; esac
