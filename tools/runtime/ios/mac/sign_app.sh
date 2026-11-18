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
. "$(dirname "$0")/lib.sh"
V=${1:-ent}; case "$V" in ent|plain) ;; *) sed -n '2,4p' "$0" >&2; exit 2;; esac
[ -f "$STATE/device.env" ] && . "$STATE/device.env"
: "${TEAM:?ios/mac: sign_app: no TEAM in $STATE/device.env}"
B="$KROOT/build/app-signed"; OUT="$RUNS/app-signed"; mkdir -p "$OUT" "$B"; rm -f "$OUT/sign.done"
{
  echo "sign_app: start $(date) ($V)"
  ENT=(); if [ "$V" = ent ]; then cp tools/ios/probe/probe.entitlements.example "$B/app.entitlements"; ENT=("CODE_SIGN_ENTITLEMENTS=$B/app.entitlements"); fi
  python3 tools/ios/make_project.py
  nice -n $NICE xcodebuild -project tools/ios/KHNMC.xcodeproj -scheme KHNMC -sdk iphoneos -destination "generic/platform=iOS" \
     -configuration Release -derivedDataPath "$KROOT/build/app-signed/dd" -jobs $JOBS -allowProvisioningUpdates \
     SYMROOT="$B/sym" OBJROOT="$B/obj" MODULE_CACHE_DIR="$KROOT/build/app-signed/dd/ModuleCache.noindex" DEAD_CODE_STRIPPING=NO \
     DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic CODE_SIGNING_ALLOWED=YES CODE_SIGNING_REQUIRED=YES \
     "CODE_SIGN_IDENTITY=Apple Development" ${ENT[@]+"${ENT[@]}"} build
  echo "sign_app: xcodebuild exit=$?"
  APP="$B/sym/Release-iphoneos/KHNMC.app"
  codesign -dv "$APP" 2>&1 | grep -E "TeamIdentifier|not signed"
  codesign -d --entitlements - "$APP" 2>/dev/null | grep -o "com.apple.developer.kernel[a-z.-]*" | sort -u
  echo "sign_app: end $(date)"
} > "$OUT/sign.log" 2>&1
touch "$OUT/sign.done"
launchctl list khnmc.app.sign >/dev/null 2>&1 && exec launchctl remove khnmc.app.sign
exit 0
