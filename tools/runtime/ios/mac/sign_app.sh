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
. "$(dirname "$0")/lib.sh"
V=${1:-ent}; case "$V" in ent|plain) ;; *) sed -n '2,4p' "$0" >&2; exit 2;; esac
OM=${ORDERED_MEM:-1}; case "$OM" in 0|1) ;; *) echo "ios/mac: sign_app: ORDERED_MEM must be 0 or 1" >&2; exit 2;; esac
XLIBS=(); [ "$OM" = 1 ] && XLIBS=("KHNMC_LIBS=$PWD/obj-iphoneos-om")
[ -f "$STATE/device.env" ] && . "$STATE/device.env"
: "${TEAM:?ios/mac: sign_app: no TEAM in $STATE/device.env}"
B="$KROOT/build/app-signed"; OUT="$RUNS/app-signed"; mkdir -p "$OUT" "$B"; rm -f "$OUT/sign.done"
{
  echo "sign_app: start $(date) ($V, GFX=$GFX, ORDERED_MEM=$OM)"
  ok=1; GX=()
  gfx_lib_check "obj-iphoneos$([ "$OM" = 1 ] && echo -om)/libkhnmc-rt.a" || ok=0
  if [ $ok = 1 ] && [ "$GFX" = 1 ]; then GFXLD=$(gfx_ldflags) && GX=("KHNMC_GFX_LDFLAGS=$GFXLD") || ok=0; fi
  if [ $ok = 1 ]; then
  ENT=(); if [ "$V" = ent ]; then cp tools/ios/probe/probe.entitlements.example "$B/app.entitlements"; ENT=("CODE_SIGN_ENTITLEMENTS=$B/app.entitlements"); fi
  python3 tools/ios/make_project.py
  [ "$GFX" = 1 ] && KHNMC_GFX_LDFLAGS="$GFXLD" python3 tools/ios/make_project.py --show-ldflags
  nice -n $NICE xcodebuild -project tools/ios/KHNMC.xcodeproj -scheme KHNMC -sdk iphoneos -destination "generic/platform=iOS" \
     -configuration Release -derivedDataPath "$KROOT/build/app-signed/dd" -jobs $JOBS -allowProvisioningUpdates \
     SYMROOT="$B/sym" OBJROOT="$B/obj" MODULE_CACHE_DIR="$KROOT/build/app-signed/dd/ModuleCache.noindex" DEAD_CODE_STRIPPING=NO \
     DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES \
     "CODE_SIGN_IDENTITY=Apple Development" ${ENT[@]+"${ENT[@]}"} ${GX[@]+"${GX[@]}"} ${XLIBS[@]+"${XLIBS[@]}"} build
  echo "sign_app: xcodebuild exit=$?"
  APP="$B/sym/Release-iphoneos/KHNMC.app"
  codesign -dv "$APP" 2>&1 | grep -E "TeamIdentifier|not signed"
  codesign -d --entitlements - "$APP" 2>/dev/null | grep -o "com.apple.developer.kernel[a-z.-]*" | sort -u
  if [ "$GFX" = 1 ]; then gfx_has "$APP/KHNMC"; case $? in 0) echo "sign_app: the signed app has the graphics backend";; 1) echo "sign_app: FAILED: GFX=1 but the signed app has no graphics backend";;
                          *) echo "sign_app: FAILED: GFX=1 but nm could not read the signed app (above): its graphics backend is unknown";; esac; fi
  else echo "sign_app: FAILED before xcodebuild (see above): nothing was built or signed"; fi
  echo "sign_app: end $(date)"
} > "$OUT/sign.log" 2>&1
touch "$OUT/sign.done"
launchctl list khnmc.app.sign >/dev/null 2>&1 && exec launchctl remove khnmc.app.sign
exit 0
