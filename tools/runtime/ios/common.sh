# tools/ios/common.sh : shared by the tools/ios/*.sh drivers (sourced; bash).
#
# UNRUN.  Everything in tools/ios/ that talks to the MacBook Air was written on 2026-11-17 for a step that did not happen (the
# owner withdrew that machine for the night; the task was re-scoped to the Mac mini, which has no iOS SDK).  None of these
# scripts has ever been executed.  Expect to debug them on first use.  The one fact in here that was measured on that Mac is in
# maclock.py (how its file locks behave).
#
# The drivers run on THIS Linux machine and drive the MacBook Air over ssh.  The Mac is another person's laptop; its rules
# (TASK26-original-macbook.md) are built in here and in tools/ios/mac/*.sh:
#   * everything lives in ~/Developer/KHNMC (fixed, not configurable); nothing else on that machine is read or changed, except
#     what the brief itself asks for: the shared lock ~/Developer/.mac-build.lock, the system volume mute before a simulator boot,
#     and OUR OWN simulator (CoreSimulator keeps it under ~/Library/Developer/CoreSimulator until we delete it); xcodebuild is
#     given ~/Developer/KHNMC/build for its products, intermediates, derived data and module cache (mac/build.sh), but whether
#     Xcode still writes caches or preferences of its own under ~/Library has never been checked (nothing here has run);
#   * every build and every simulator step runs under tools/ios/maclock.py (the shared lock, both conventions; busy = stop);
#   * DEVELOPER_DIR is exported for every remote command; no GUI, no `open`, no Simulator.app, no signing, no installs;
#   * at most 2 compile jobs, nice -n 10, a memory check before every compile step and before xcodebuild (stop below 25 % free);
#     the mute before the first simulator boot is checked by reading it back, and no simulator is created when it fails;
#   * never the simulator named "iPhone Duo", never a simulator we did not create, never a physical device, never devicectl.
# GUARD: every driver refuses to do anything unless KHNMC_IOS_MACBOOK_OK=1 is in the environment.  That variable is the
# owner's word that the MacBook Air may be used now; scripts require explicit configuration.
IOS_MAC=${KHNMC_IOS_MAC:-user@macbook}
IOS_RDIR=Developer/KHNMC                       # relative to the remote home; fixed on purpose
IOS_HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IOS_SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=6)
IOS_RSH="ssh -o BatchMode=yes -o ConnectTimeout=20"
IOS_ENV='export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer; export LC_ALL=C;'

ios_guard(){
  [ "${KHNMC_IOS_MACBOOK_OK:-}" = 1 ] && return 0
  echo "tools/ios: NOT STARTED.  These scripts are UNRUN and use another person's MacBook Air; the owner withdrew it on 2026-11-17." >&2
  echo "tools/ios: they run only with KHNMC_IOS_MACBOOK_OK=1, which says the owner has allowed that machine for this run." >&2
  exit 2
}

# mac CMD            : one remote shell command (not a build or simulator step: no lock)
mac(){ "${IOS_SSH[@]}" "$IOS_MAC" "$IOS_ENV $1"; }

# mac_step WHAT SCRIPT [ARGS...] : run tools/ios/mac/SCRIPT (synced copy) on the Mac under the shared lock.  Exit status of the
# script; 75 = the Mac is busy (xcodebuild running, lock held, or a foreign description in the lock file): nothing was started.
mac_step(){
  local what=$1 script=$2; shift 2
  local args=""; local a
  for a in "$@"; do case "$a" in *[!A-Za-z0-9_./:=,+@%-]*) echo "tools/ios: argument '$a' has characters the remote command line does not carry" >&2; return 2;; esac; args="$args $a"; done
  case "$what" in *[!A-Za-z0-9_.,:\ -]*) echo "tools/ios: bad step description" >&2; return 2;; esac
  "${IOS_SSH[@]}" "$IOS_MAC" "$IOS_ENV cd ~/$IOS_RDIR/src && exec python3 tools/ios/maclock.py --what '$what' -- /bin/bash tools/ios/mac/$script$args"
}

# busy_note RC : explain exit status 75 once, the same way everywhere
busy_note(){ [ "$1" = 75 ] && echo "tools/ios: the MacBook Air is busy (see the BUSY line above): nothing was started; run this again later (no retry loop here)" >&2; return 0; }
