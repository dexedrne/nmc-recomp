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
. "$(dirname "$0")/lib.sh"
MODE=${1:-selftest}; INPUT=""; FRAMES=7000; DIFF=none
case "$MODE" in
  selftest) LIMIT=${2:-1500};;
  play) LIMIT=0;;
  route) INPUT=${2:-}; FRAMES=${3:-7000}; DIFF=${4:-none}; LIMIT=${5:-2400}
         [ -f "$INPUT" ] || { echo "ios/mac: device_run: route needs an input script of src/ (got '${INPUT}')" >&2; exit 2; }
         case "$DIFF" in none|0|1|2|3) ;; *) echo "ios/mac: device_run: DIFFICULTY is none or 0..3" >&2; exit 2;; esac;;
  *) sed -n '2,13p' "$0" >&2; exit 2;;
esac
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
printf 'starting\n' > "$OUT/status.init"
if [ "${SKIP_INSTALL:-0}" != 1 ]; then
  [ -f "$KROOT/pc/kh2fm.exe" ] || { say "no $KROOT/pc/kh2fm.exe (tools/ios/sync.sh exe)"; exit 2; }
  say "install"; xcrun devicectl device install app --device "$DEVICE" --timeout 300 "$APP" >> "$OUT/devicectl.log" 2>&1 \
    || { say "install FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  cp_to "$KROOT/pc/kh2fm.exe" kh2fm.exe || { say "copy of kh2fm.exe FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
fi
play_conf(){ { echo "# the game (tools/ios/mac/device_run.sh play): the controller is pad 0, sound on, no script, no time limit"; echo "log khnmc.log"; echo "status khnmc.status"; } > "$OUT/play.conf"; }
play_conf
if [ "$MODE" = play ]; then
  cp_to "$OUT/play.conf" khnmc.conf || { say "copy of the play configuration FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1 || { say "launch FAILED (phone locked?)"; tail -5 "$OUT/devicectl.log"; exit 1; }
  say "the game is running on the phone (play mode); its log: Documents/khnmc.log"; echo "$OUT"; exit 0
fi
{ echo "# written by tools/ios/mac/device_run.sh ($MODE ${INPUT})"; echo "log khnmc.log"; echo "status khnmc.status"
  # no sound in test runs unless SOUND=1 (route mode): a selftest must never reach the speaker (2026-11-18: a test signal did)
  if [ "$MODE" = selftest ] || [ "${SOUND:-0}" != 1 ]; then echo "env KHNMC_AUDIO=null"; fi
  if [ "$MODE" = selftest ]; then echo "arg --selftest"
  else TAG=$(basename "$OUT"); echo "env KH_ROOT=@TMP@/$TAG/root"; echo "env KH_DOCS=@TMP@/$TAG/docs"   # fresh per run, as tools/route_batch.sh: a config the game saved in an earlier run changes how it boots
    echo "env KHNMC_INPUT_SCRIPT=@DOCS@/input.txt"; echo "env KH_RUN_GAME_FRAMES=$FRAMES"; echo "env KH_RUN_SECONDS=$((LIMIT-120))"
    echo "env KH_DUMP_THREADS=1"; echo "env KHNMC_PHASELOG=@DOCS@/phase.log"; echo "env KHNMC_FILELOG=@DOCS@/files.log"
    DV=KHNMC_DIFFICULTY; grep -qE '^[[:space:]]*@(wait|press|select|expect)([[:space:]]|$)' "$INPUT" && DV=KHNMC_ROUTE_DIFFICULTY   # as tools/route_batch.sh
    [ "$DIFF" = none ] || echo "env $DV=$DIFF"; fi
} > "$OUT/khnmc.conf"
cp_to "$OUT/khnmc.conf" khnmc.conf || { say "copy of khnmc.conf FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
if [ "$MODE" = route ]; then cp "$INPUT" "$OUT/input.txt"; cp_to "$OUT/input.txt" input.txt || { say "copy of the input script FAILED"; exit 1; }; fi
cp_to "$OUT/status.init" khnmc.status || { say "copy of the status file FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
tw=$SECONDS; n=0
until xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1; do
  n=$((n+1)); [ $n = 1 ] && say "launch refused (phone locked?): retrying every 20 s"
  [ $((SECONDS-tw)) -ge "${WAIT_UNLOCK_S:-1800}" ] && { say "launch FAILED for $((SECONDS-tw)) s"; tail -5 "$OUT/devicectl.log"; exit 1; }
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
cp_to "$OUT/play.conf" khnmc.conf || say "WARNING: the play configuration could not be put back: the icon would start this test again"
say "status after $((SECONDS-t0)) s: '${st:-<none>}'; log lines: $(wc -l < "$OUT/khnmc.log" 2>/dev/null | tr -d ' ')"
echo "$OUT"
case "$st" in "exit 0") exit 0;; "exit "*) exit 1;; *) exit 124;; esac
