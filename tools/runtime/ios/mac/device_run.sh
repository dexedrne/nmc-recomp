#!/bin/bash
# tools/ios/mac/device_run.sh [selftest] [LIMIT_S] : run the signed app (mac/sign_app.sh) on the owner's own iPhone, over the
# network (the phone is paired with this Mac; `xcrun devicectl list devices` shows it "available (paired)").  Run on the MacBook
# from ~/Developer/KHNMC/src, over ssh is fine (devicectl needs no keychain).
#   1. install the app; 2. put khnmc.conf (`arg --selftest`, log + status file), a fresh status file and kh2fm.exe into the app's
#      Documents; 3. start it, retrying every 20 s while iOS refuses because the phone is LOCKED (WAIT_UNLOCK_S, default 1800);
#   4. read Documents/khnmc.status every 15 s until "exit N" (LIMIT_S, default 1500), then copy khnmc.log back.
#   SKIP_INSTALL=1 skips steps 1 and 2 apart from the fresh status file (the app, the exe and the conf are already there).
# The phone's device id is read from ~/Developer/KHNMC/state/device.env (DEVICE=...), never from this repository.  Never names a
# simulator.  Results in ~/Developer/KHNMC/run/device-<mode>-<time>/.  The app must stay in the foreground with the screen on:
# iOS suspends it otherwise.
# First runs 2026-11-18 (iPhone, A20 Pro, iOS 27.2): 169,479 checks, 0 failures, about 16 s; 381 checks need fork() and are NOT RUN.
. "$(dirname "$0")/lib.sh"
MODE=${1:-selftest}; LIMIT=${2:-1500}; case "$MODE" in selftest) ;; *) sed -n '2,6p' "$0" >&2; exit 2;; esac
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
  { echo "# written by tools/ios/mac/device_run.sh ($MODE)"; echo "log khnmc.log"; echo "status khnmc.status"; echo "arg --selftest"; } > "$OUT/khnmc.conf"
  cp_to "$OUT/khnmc.conf" khnmc.conf || { say "copy of khnmc.conf FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  cp_to "$KROOT/pc/kh2fm.exe" kh2fm.exe || { say "copy of kh2fm.exe FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
fi
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
say "status after $((SECONDS-t0)) s: '${st:-<none>}'; log lines: $(wc -l < "$OUT/khnmc.log" 2>/dev/null | tr -d ' ')"
echo "$OUT"
case "$st" in "exit 0") exit 0;; "exit "*) exit 1;; *) exit 124;; esac
