#!/bin/bash
# tools/ios/mac/sim_run.sh MODE [DIFFICULTY] : step 4, run the app INSIDE an iOS Simulator of our own.  UNRUN (tools/ios/common.sh).
#   MODE selftest : `--selftest`; kh2fm.exe in the app's Documents
#   MODE route    : the null-backend field route, 7,000 game frames, the route data subset, runtime/task20-field-input.txt;
#                   DIFFICULTY = none (default) | 0..3
# Rules of that Mac, as this script keeps them:
#   * the system output is muted first and stays muted; if muting fails, or the mute state does not read back as `true`, the
#     script stops (exit 1) before any simulator is created or booted;
#   * a NEW simulator named "KHNMC Test" is created and every later command names it by the UDID that `simctl create` printed;
#     no other simulator is ever named (not "iPhone Duo", not a booted one), no physical device, no devicectl, no Simulator.app
#     (simctl boot is headless), no `open`;
#   * on every way out (trap: normal end, error, SIGTERM from maclock.py when the ssh session drops) the app is uninstalled and
#     the simulator is shut down and DELETED; the UDID is kept in ~/Developer/KHNMC/state/sim.udid until the delete succeeded, so
#     a later run (or cleanup.sh) can finish a delete that did not happen.
# What the app is told comes from Documents/khnmc.conf (runtime/host_env.c); the run's log, status file, phase log and file log
# are written into Documents and copied to ~/Developer/KHNMC/run/sim-<mode>-<time>/ ; the data is LINKED into the container, not
# copied (the Simulator is a macOS process and follows links out of the container), so no game data leaves ~/Developer/KHNMC.
# Exit status: the app's (from its status file); 124 = no status within the time limit; 1 = a simulator step failed.
. "$(dirname "$0")/lib.sh"
MODE=${1:-}; DIFF=${2:-none}
case "$MODE" in selftest) LIMIT=600;; route) LIMIT=1200;; *) sed -n '2,6p' "$0" >&2; exit 2;; esac
case "$DIFF" in none|0|1|2|3) ;; *) echo "ios/mac: sim_run: DIFFICULTY is none or 0..3" >&2; exit 2;; esac
APP="$KROOT/build/xc/sym/Release-iphonesimulator/KHNMC.app"
[ -x "$APP/KHNMC" ] || { echo "ios/mac: sim_run: no simulator app at $APP (run build.sh iphonesimulator first)" >&2; exit 2; }
[ -f "$KROOT/pc/kh2fm.exe" ] || { echo "ios/mac: sim_run: no $KROOT/pc/kh2fm.exe (tools/ios/sync.sh exe)" >&2; exit 2; }
[ "$MODE" != route ] || [ -d "$KROOT/route-data/data" ] || { echo "ios/mac: sim_run: no $KROOT/route-data (tools/ios/sync.sh route)" >&2; exit 2; }
OUT="$RUNS/sim-$MODE-$(date -u +%Y%m%dT%H%M%SZ)"; mkdir -p "$OUT"; UDID_FILE="$STATE/sim.udid"; UDID=""
say(){ echo "ios/mac: sim_run: $*" | tee -a "$OUT/steps.log"; }
ours(){ xcrun simctl list -j devices | python3 -c 'import json,sys
u,name=sys.argv[1],sys.argv[2]
for devs in json.load(sys.stdin)["devices"].values():
    for d in devs:
        if d["udid"]==u: sys.exit(0 if d["name"]==name else 3)
sys.exit(4)' "$1" "$SIM_NAME"; }                # 0: listed with our name; 3: listed with ANOTHER name (not ours: hands off); 4: gone
remove_sim(){ local u=$1
  ours "$u"; case $? in 4) rm -f "$UDID_FILE"; return 0;; 3) say "UDID $u is not named '$SIM_NAME' any more: NOT touching it"; return 1;; esac
  xcrun simctl terminate "$u" "$BUNDLE_ID" >/dev/null 2>&1
  xcrun simctl uninstall "$u" "$BUNDLE_ID" >/dev/null 2>&1
  xcrun simctl shutdown "$u" >/dev/null 2>&1
  if xcrun simctl delete "$u"; then rm -f "$UDID_FILE"; say "simulator $u shut down and deleted"; else say "FAILED to delete simulator $u: it is still recorded in $UDID_FILE"; return 1; fi; }
cleanup(){ [ -n "$UDID" ] && remove_sim "$UDID"; }
trap cleanup EXIT; trap 'exit 143' TERM HUP INT
if [ -s "$UDID_FILE" ]; then say "an earlier run left simulator $(cat "$UDID_FILE") recorded: removing it first"; remove_sim "$(cat "$UDID_FILE")" || exit 1; fi
if xcrun simctl list devices | grep -q "^ *$SIM_NAME ("; then say "a simulator named '$SIM_NAME' exists that this project has no record of: not touching it, not starting"; exit 1; fi
osascript -e 'set volume with output muted' >/dev/null 2>&1 || { say "could not mute the Mac (osascript failed): no simulator is created or booted"; exit 1; }
muted=$(osascript -e 'output muted of (get volume settings)' 2>/dev/null)
[ "$muted" = "true" ] || { say "the system output reads as muted='${muted:-<nothing>}', not 'true': no simulator is created or booted"; exit 1; }
say "system output muted and read back as muted (left muted)"
# `simctl list` takes ONE category (a second word is only a search term), so no category here: the JSON then holds every section
PICK=$(xcrun simctl list -j | python3 -c 'import json,sys
j=json.load(sys.stdin)
if "runtimes" not in j: sys.exit("simctl list -j printed no runtimes section")
rts=[r for r in j["runtimes"] if r.get("isAvailable") and r.get("platform","iOS")=="iOS" and "iOS" in r.get("name","")]
if not rts: sys.exit("no available iOS simulator runtime")
key=lambda r: tuple(int(x) for x in r["version"].split(".") if x.isdigit())
rt=max(rts,key=key)
sup=[d["identifier"] for d in rt.get("supportedDeviceTypes",[]) if d.get("productFamily")=="iPhone"]
if not sup: sup=[d["identifier"] for d in j.get("devicetypes",[]) if d.get("productFamily")=="iPhone"]
if not sup: sys.exit("no iPhone device type")
print(sup[-1],rt["identifier"],rt["version"])') || { say "cannot choose a device type and runtime"; exit 1; }
set -- $PICK; DT=$1; RT=$2
say "device type $DT, runtime $RT (iOS $3)"
UDID=$(xcrun simctl create "$SIM_NAME" "$DT" "$RT") || { say "simctl create failed"; UDID=""; exit 1; }
echo "$UDID" > "$UDID_FILE"; say "created simulator '$SIM_NAME' $UDID"
xcrun simctl boot "$UDID" || { say "simctl boot failed"; exit 1; }
xcrun simctl bootstatus "$UDID" -b > "$OUT/bootstatus.txt" 2>&1 || { say "the simulator did not finish booting (see bootstatus.txt)"; exit 1; }
xcrun simctl install "$UDID" "$APP" || { say "simctl install failed"; exit 1; }
DATA=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data) || { say "no data container"; exit 1; }
DOCS="$DATA/Documents"; mkdir -p "$DOCS"
ln -s "$KROOT/pc/kh2fm.exe" "$DOCS/kh2fm.exe"
{ echo "# written by tools/ios/mac/sim_run.sh ($MODE)"; echo "log khnmc.log"; echo "status khnmc.status"
  if [ "$MODE" = selftest ]; then echo "arg --selftest"
  else ln -s "$KROOT/route-data/data" "$DOCS/data"; ln -s "$KROOT/route-data/pkgdir" "$DOCS/pkgdir"; cp runtime/task20-field-input.txt "$DOCS/input.txt"
    echo "env KHNMC_INPUT_SCRIPT=@DOCS@/input.txt"; echo "env KH_RUN_GAME_FRAMES=7000"; echo "env KH_RUN_SECONDS=900"; echo "env KH_DUMP_THREADS=1"
    echo "env KHNMC_PHASELOG=@DOCS@/phase.log"; echo "env KHNMC_FILELOG=@DOCS@/files.log"; [ "$DIFF" = none ] || echo "env KHNMC_DIFFICULTY=$DIFF"; fi
} > "$DOCS/khnmc.conf"
cp "$DOCS/khnmc.conf" "$OUT/khnmc.conf"
say "container $DATA; launching $BUNDLE_ID"
xcrun simctl launch "$UDID" "$BUNDLE_ID" > "$OUT/launch.txt" 2>&1 || { say "simctl launch failed: $(cat "$OUT/launch.txt")"; exit 1; }
t0=$SECONDS; st=""
while [ $((SECONDS-t0)) -lt $LIMIT ]; do
  st=$(cat "$DOCS/khnmc.status" 2>/dev/null); case "$st" in "exit "*) break;; esac
  sleep 5
done
for f in khnmc.log khnmc.status phase.log files.log; do [ -f "$DOCS/$f" ] && cp "$DOCS/$f" "$OUT/$f"; done
[ -f "$OUT/khnmc.log" ] && cp "$OUT/khnmc.log" "$OUT/run.out"
ls -la "$DOCS" > "$OUT/documents-listing.txt" 2>&1
case "$st" in
  "exit "*) rc=${st#exit }; say "status file: '$st' after $((SECONDS-t0)) s";;
  *) rc=124; say "NO exit status within $LIMIT s (status file: '${st:-<none>}'): the app hangs, was killed, or never started; last log lines:"; tail -5 "$OUT/khnmc.log" 2>/dev/null | cut -c1-200
     xcrun simctl spawn "$UDID" log show --last 3m --style compact --predicate 'process == "KHNMC"' 2>/dev/null | tail -200 > "$OUT/oslog-tail.txt";;
esac
grep -a "guest space\|host dirs\|NOT RUN:\|selftest: [0-9]* checks\|SKIPPED\|FAIL\|process exit\|kh: gfx: draws" "$OUT/run.out" 2>/dev/null | cut -c1-240 | tail -80
if [ "$MODE" = selftest ]; then echo "selftest exit=$rc" >> "$OUT/run.out"; fi
say "results in $OUT"; disk_note
exit "$rc"
