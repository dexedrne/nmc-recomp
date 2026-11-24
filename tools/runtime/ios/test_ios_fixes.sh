#!/usr/bin/env bash
# tools/ios/test_ios_fixes.sh SCRATCH_DIR : behaviour tests (stubs for ssh, rsync, xcodebuild, ... in SCRATCH_DIR/bin; nothing touches a Mac)
# for the 2026-11-23 review fixes: sign_app.sh (#3 #5), push_data.sh (#2 #4 #8), common.sh mac_step (#6), maclock.py (HELD marker).
set -u
D=${1:-}; [ -n "$D" ] && [ -d "$D" ] && [ -z "$(ls -A "$D")" ] || { echo "usage: $0 EMPTY_SCRATCH_DIR" >&2; exit 2; }
D=$(cd "$D" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); ok=0; bad=0
t(){ if eval "$2"; then ok=$((ok+1)); else bad=$((bad+1)); echo "test_ios_fixes: FAIL: $1" >&2; fi; }
mkdir -p "$D/bin"; export PATH="$D/bin:$PATH"
# ---------------------------------------------------------------- sign_app.sh
K="$D/k"; mkdir -p "$K/src/tools" "$K/state" "$K/run"; cp -r "$HERE" "$K/src/tools/ios"; mkdir -p "$K/src/obj-iphoneos-om"; : > "$K/src/obj-iphoneos-om/libkhnmc-rt.a"
echo "TEAM=TEAM1" > "$K/state/device.env"; : > "$K/src/tools/ios/probe/probe.entitlements.example" 2>/dev/null || true
cat > "$D/bin/memory_pressure" <<'S'
#!/bin/sh
echo "System-wide memory free percentage: ${FAKE_FREE:-80}%"
S
cat > "$D/bin/xcodebuild" <<'S'
#!/bin/sh
echo run >> "$FAKE_LOG/xcodebuild.ran"
for a in "$@"; do case "$a" in SYMROOT=*) sym=${a#SYMROOT=};; esac; done
if [ "${XC_RC:-0}" = 0 ]; then mkdir -p "$sym/Release-iphoneos/KHNMC.app"; printf '#!/bin/sh\n' > "$sym/Release-iphoneos/KHNMC.app/KHNMC"; chmod +x "$sym/Release-iphoneos/KHNMC.app/KHNMC"; fi
exit "${XC_RC:-0}"
S
cat > "$D/bin/codesign" <<'S'
#!/bin/sh
case "$*" in *entitlements*) ;; *) echo "TeamIdentifier=TEAM1" >&2;; esac
S
cat > "$D/bin/nm" <<'S'
#!/bin/sh
echo "0000000000000000 T _main"
S
cat > "$D/bin/launchctl" <<'S'
#!/bin/sh
exit 1
S
cat > "$D/bin/python3" <<'S'
#!/bin/sh
case "$1" in tools/ios/make_project.py) exit 0;; esac
exec /usr/bin/python3 "$@"
S
chmod +x "$D"/bin/*; export FAKE_LOG="$D/log"; mkdir -p "$FAKE_LOG"
export KHNMC_KROOT="$K" KHNMC_MACLOCK_FILE="$K/lock"
SIGN(){ rm -f "$FAKE_LOG/xcodebuild.ran"; ( cd "$K/src" && /bin/bash tools/ios/mac/sign_app.sh ent > "$D/sign.out" 2>&1 ); }
O="$K/run/app-signed"; APP="$K/build/app-signed/sym/Release-iphoneos/KHNMC.app"
mkdir -p "$APP"; : > "$APP/KHNMC"; chmod +x "$APP/KHNMC"       # an OLD signed app
XC_RC=1 SIGN; rc=$?
t "sign_app: failed xcodebuild: non-zero exit" '[ $rc != 0 ]'
t "sign_app: failed xcodebuild: no sign.done, sign.failed written" '[ ! -e "$O/sign.done" ] && [ -e "$O/sign.failed" ]'
t "sign_app: failed xcodebuild: the OLD app is gone (nothing to install)" '[ ! -e "$APP" ]'
t "sign_app: failed xcodebuild: sign.log says FAILED" 'grep -q "sign_app: FAILED: xcodebuild exited with status 1" "$O/sign.log"'
XC_RC=0 SIGN; rc=$?
t "sign_app: success: exit 0, sign.done, no sign.failed, app there" '[ $rc = 0 ] && [ -e "$O/sign.done" ] && [ ! -e "$O/sign.failed" ] && [ -x "$APP/KHNMC" ]'
FAKE_FREE=10 XC_RC=0 SIGN; rc=$?
t "sign_app: memory below 25 %: no xcodebuild, sign.failed, exit non-zero" '[ $rc != 0 ] && [ ! -e "$FAKE_LOG/xcodebuild.ran" ] && [ -e "$O/sign.failed" ] && [ ! -e "$O/sign.done" ]'
echo "someone else building" > "$K/lock"; XC_RC=0 SIGN; rc=$?
t "sign_app: the shared Mac lock is held by someone else: no xcodebuild, sign.failed, exit non-zero" '[ $rc != 0 ] && [ ! -e "$FAKE_LOG/xcodebuild.ran" ] && [ -e "$O/sign.failed" ] && [ ! -e "$O/sign.done" ] && [ "$(cat "$K/lock")" = "someone else building" ]'
rm -f "$K/lock"
XC_RC=0 SIGN; rc=$?
t "sign_app: free lock again: signs, and the lock file is gone afterwards" '[ $rc = 0 ] && [ ! -e "$K/lock" ] && [ -e "$O/sign.done" ]'
python3 "$HERE/maclock.py" -- sh -c 'echo "held=${KHNMC_MACLOCK_HELD:-}" > "'"$D"'/held"'; t "maclock.py: the step sees KHNMC_MACLOCK_HELD=1" '[ "$(cat "$D/held")" = held=1 ]'
# ---------------------------------------------------------------- push_data.sh
T="$D/tree"; mkdir -p "$T/tools" "$T/data-full" "$T/pkgdir"; cp -r "$HERE" "$T/tools/ios"; echo hello > "$T/data-full/a.bin"; : > "$T/prune-A.list"
cat > "$D/bin/ssh" <<'S'
#!/bin/bash
# stub for push_data.sh / mac_step tests: logs the remote command, answers like the Mac where the tests need it
cmd="${@: -1}"; echo "$*" >> "$FAKE_LOG/ssh.log"
case "$*" in *" -tt "*) echo started > "$FAKE_LOG/mac-started"; trap 'echo hup > "$FAKE_LOG/mac-hup"; exit 129' HUP; while :; do sleep 0.1; done;; esac
case "$cmd" in
  *"find . -type f"*) echo "1 6";;
  *"devicectl device copy to"*) [ "${FAIL_SEND:-0}" = 1 ] && exit 1; exit 0;;
  *"devicectl device info files"*) echo '{"result":{"files":[]}}';;
esac
exit 0
S
cat > "$D/bin/rsync" <<'S'
#!/bin/sh
exit 0
S
chmod +x "$D/bin/ssh" "$D/bin/rsync"
PD(){ ( cd "$T" && "$@" bash tools/ios/push_data.sh run > "$D/pd.out" 2>&1 ); }
: > "$FAKE_LOG/ssh.log"; PD env -u KHNMC_IOS_MACBOOK_OK KHNMC_IOS_DRIVER_LOCK_FILE="$D/dl"; rc=$?
t "push_data: without KHNMC_IOS_MACBOOK_OK=1: exit 2, the Mac is not contacted" '[ $rc = 2 ] && [ ! -s "$FAKE_LOG/ssh.log" ]'
: > "$FAKE_LOG/ssh.log"; PD env KHNMC_IOS_MACBOOK_OK=1 KHNMC_IOS_DRIVER_LOCK_FILE="$D/dl" KHNMC_IOS_MAC=who@where KHNMC_IOS_RDIR=work/x FAIL_SEND=1; rc=$?
t "push_data: a failed send: exit 1" '[ $rc = 1 ]'
t "push_data: KHNMC_IOS_MAC / KHNMC_IOS_RDIR are honoured" 'grep -q "who@where" "$FAKE_LOG/ssh.log" && grep -q "work/x/stage/chunk-000" "$FAKE_LOG/ssh.log" && ! grep -q "Developer/KHNMC" "$FAKE_LOG/ssh.log"'
t "push_data: a failed send removes the staged chunk on the Mac (the LAST remote command)" 'tail -1 "$FAKE_LOG/ssh.log" | grep -q "rm -rf ~/work/x/stage/chunk-000"'
t "push_data: the chunk is not marked done" '[ ! -e "$T/run-J26/ios/push/chunk-000.done" ]'
# every chunk marked done, the phone lists nothing: verify fails -> the script must NOT say success (it tested tee, not verify)
touch "$T/run-J26/ios/push/chunk-000.done"; : > "$FAKE_LOG/ssh.log"
PD env KHNMC_IOS_MACBOOK_OK=1 KHNMC_IOS_DRIVER_LOCK_FILE="$D/dl" FAIL_SEND=1; rc=$?
t "push_data: all chunks done locally but the phone has none: verify's failure is seen (sent again, not exit 0)" '[ $rc != 0 ] && grep -q "will be sent again" "$D/pd.out"'
# ---------------------------------------------------------------- mac_step dies with the driver
: > "$FAKE_LOG/ssh.log"; rm -f "$FAKE_LOG/mac-started" "$FAKE_LOG/mac-hup"
KHNMC_IOS_MACBOOK_OK=1 KHNMC_IOS_DRIVER_LOCK_FILE="$D/dl2" bash -c ". '$HERE/common.sh'; ios_guard; mac_step 'khnmc test' build.sh iphoneos; echo \"mac_step rc=\$?\"" > "$D/ms.out" 2>&1 & dp=$!
for i in $(seq 100); do [ -e "$FAKE_LOG/mac-started" ] && break; sleep 0.1; done
t "mac_step: the step is run over ssh -tt under maclock.py" 'grep -q -- " -tt " "$FAKE_LOG/ssh.log" && grep -q "maclock.py --what" "$FAKE_LOG/ssh.log"'
kill -TERM $dp; wait $dp
t "mac_step: a signal to the driver hangs the ssh up" '[ -e "$FAKE_LOG/mac-hup" ]'
t "mac_step: the driver then asks whether the Mac released its lock, and returns 130" 'grep -q "grep -q .\^khnmc iOS build" "$FAKE_LOG/ssh.log" && grep -q "mac_step rc=130" "$D/ms.out"'
echo "test_ios_fixes: $ok passed, $bad failed"
[ $bad = 0 ]
