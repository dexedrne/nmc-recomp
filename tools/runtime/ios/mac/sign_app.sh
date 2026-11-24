#!/bin/bash
# tools/ios/mac/sign_app.sh [plain|ent] : the KHNMC app for the owner's own iPhone, SIGNED with his team.  Run on the MacBook from
# ~/Developer/KHNMC/src after `mac/build.sh iphoneos` made the static libraries.  ent (default): with both memory entitlements
# (tools/ios/probe/probe.entitlements.example: increased-memory-limit, extended-virtual-addressing); plain: none.
#   * Signing needs the login keychain, which belongs to the GUI session: start it with
#       launchctl submit -l khnmc.app.sign -- /bin/bash -c 'cd ~/Developer/KHNMC/src && tools/ios/mac/sign_app.sh ent'
#     (windowless; no Terminal window, no Xcode window).  The job removes itself at the end.
#   * The team id is read from ~/Developer/KHNMC/state/device.env (TEAM=..., DEVICE=...), a file of that Mac only: neither value
#     is in this repository.
#   * Products in ~/Developer/KHNMC/build/app-signed (separate from the unsigned build), log in ~/Developer/KHNMC/run/app-signed/.
# First run 2026-11-18: signed with automatic signing and -allowProvisioningUpdates, no click needed; the entitlements are in the
# signature (`codesign -d --entitlements -`).
#   GFX=1 (TASK38): the app with the graphics backend.  The libraries must come from `GFX=1 mac/build.sh iphoneos` (checked: the
#   archive's backend must match GFX, either way); xcodebuild then gets KHNMC_GFX_LDFLAGS.  launchctl does not pass the caller's
#   environment: put it into the job's command, `... && GFX=1 tools/ios/mac/sign_app.sh ent'`.  A failed check is written to sign.log
#   ("sign_app: FAILED ...") and nothing is built or signed.
# ORDERED_MEM (environment; task 39): DEFAULT 1 (owner 2026-11-18): sign the variant whose libraries mac/build.sh iphoneos made in
# obj-iphoneos-om/; ORDERED_MEM=0 signs the plain variant (obj-iphoneos/, built with `ORDERED_MEM=0 mac/build.sh iphoneos`).
# REVIEW FIXES (2026-11-23): (1) the step takes the shared Mac lock itself (tools/ios/maclock.py; launchctl does not go through
#   mac_step) and a fresh memory reading before xcodebuild, like every other compiling step; busy = no signing, `sign.failed`.
#   (2) A failed signing never reports success: the outcome is ONE marker in run/app-signed/, `sign.done` (signed app verified:
#   xcodebuild exit 0, the app binary there, signed with TEAM, GFX backend as asked) or `sign.failed` (anything else; sign.log says
#   why), never both; the previous signed app is removed before xcodebuild, so a failed run leaves nothing for device_run.sh to
#   install, and device_run.sh refuses while sign.failed exists.  The exit status is 0 only with sign.done.  Callers wait for either.

. "$(dirname "$0")/lib.sh"
V=${1:-ent}; case "$V" in ent|plain) ;; *) sed -n '2,4p' "$0" >&2; exit 2;; esac
B="$KROOT/build/app-signed"; OUT="$RUNS/app-signed"; mkdir -p "$OUT" "$B"
if [ "${KHNMC_MACLOCK_HELD:-}" != 1 ]; then
  # not started by maclock.py (launchctl submit, or by hand): take the shared build lock now and run the real step under it
  python3 tools/ios/maclock.py --what "khnmc iOS signing" -- /bin/bash "$0" "$@"; rc=$?
  if [ $rc = 75 ]; then rm -f "$OUT/sign.done"; echo "sign_app: BUSY: the shared Mac lock is held (maclock.py, above): nothing was signed" > "$OUT/sign.log"; fi
  [ -f "$OUT/sign.done" ] || { [ $rc != 0 ] || rc=1; touch "$OUT/sign.failed"; }
  launchctl list khnmc.app.sign >/dev/null 2>&1 && exec launchctl remove khnmc.app.sign
  exit $rc
fi
OM=${ORDERED_MEM:-1}; case "$OM" in 0|1) ;; *) echo "ios/mac: sign_app: ORDERED_MEM must be 0 or 1" >&2; touch "$OUT/sign.failed"; exit 2;; esac
XLIBS=(); [ "$OM" = 1 ] && XLIBS=("KHNMC_LIBS=$PWD/obj-iphoneos-om")
[ -f "$STATE/device.env" ] && . "$STATE/device.env"
rm -f "$OUT/sign.done" "$OUT/sign.failed"
trap '[ -f "$OUT/sign.done" ] || touch "$OUT/sign.failed"' EXIT      # any exit without success (also an unset variable) is a failure
: "${TEAM:?ios/mac: sign_app: no TEAM in $STATE/device.env}"
{
  echo "sign_app: start $(date) ($V, GFX=$GFX, ORDERED_MEM=$OM)"
  ok=1; GX=()
  fail(){ echo "sign_app: FAILED: $*"; ok=0; }
  gfx_lib_check "obj-iphoneos$([ "$OM" = 1 ] && echo -om)/libkhnmc-rt.a" || ok=0
  if [ $ok = 1 ] && [ "$GFX" = 1 ]; then GFXLD=$(gfx_ldflags) && GX=("KHNMC_GFX_LDFLAGS=$GFXLD") || ok=0; fi
  APP="$B/sym/Release-iphoneos/KHNMC.app"
  if [ $ok = 1 ]; then
  ENT=(); if [ "$V" = ent ]; then cp tools/ios/probe/probe.entitlements.example "$B/app.entitlements"; ENT=("CODE_SIGN_ENTITLEMENTS=$B/app.entitlements"); fi
  python3 tools/ios/make_project.py || fail "make_project.py failed"
  [ "$GFX" = 1 ] && { KHNMC_GFX_LDFLAGS="$GFXLD" python3 tools/ios/make_project.py --show-ldflags || fail "make_project.py --show-ldflags failed"; }
  ( mem_gate "xcodebuild of the signed app" ) || fail "memory gate: not starting xcodebuild"
  fi
  if [ $ok = 1 ]; then
  rm -rf "$APP"      # the previous signed app must not survive a failed build: device_run.sh would install it
  nice -n $NICE xcodebuild -project tools/ios/KHNMC.xcodeproj -scheme KHNMC -sdk iphoneos -destination "generic/platform=iOS" \
     -configuration Release -derivedDataPath "$KROOT/build/app-signed/dd" -jobs $JOBS -allowProvisioningUpdates \
     SYMROOT="$B/sym" OBJROOT="$B/obj" MODULE_CACHE_DIR="$KROOT/build/app-signed/dd/ModuleCache.noindex" DEAD_CODE_STRIPPING=NO \
     DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES \
     "CODE_SIGN_IDENTITY=Apple Development" ${ENT[@]+"${ENT[@]}"} ${GX[@]+"${GX[@]}"} ${XLIBS[@]+"${XLIBS[@]}"} build
  xrc=$?
  echo "sign_app: xcodebuild exit=$xrc"
  [ $xrc = 0 ] || fail "xcodebuild exited with status $xrc"
  [ -x "$APP/KHNMC" ] || fail "no app binary at $APP/KHNMC"
  if [ $ok = 1 ]; then
    sig=$(codesign -dv "$APP" 2>&1 | grep -E "TeamIdentifier|not signed"); echo "$sig"
    case "$sig" in *"TeamIdentifier=$TEAM"*) ;; *) fail "the app is not signed with the team from device.env";; esac
    codesign -d --entitlements - "$APP" 2>/dev/null | grep -o "com.apple.developer.kernel[a-z.-]*" | sort -u
    if [ "$GFX" = 1 ]; then gfx_has "$APP/KHNMC"; case $? in 0) echo "sign_app: the signed app has the graphics backend";; 1) fail "GFX=1 but the signed app has no graphics backend";;
                            *) fail "GFX=1 but nm could not read the signed app (above): its graphics backend is unknown";; esac; fi
  fi
  [ $ok = 1 ] || rm -rf "$APP"     # a half-built or wrongly built app is never left to be installed
  else echo "sign_app: FAILED before xcodebuild (see above): nothing was built or signed"; fi
  [ $ok = 1 ] && echo "sign_app: OK" || echo "sign_app: FAILED (no sign.done is written)"
  echo "sign_app: end $(date)"
  [ $ok = 1 ]
} > "$OUT/sign.log" 2>&1
rc=$?
[ $rc = 0 ] && touch "$OUT/sign.done" || touch "$OUT/sign.failed"
exit $rc
