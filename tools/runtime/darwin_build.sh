#!/usr/bin/env bash
# tools/darwin_build.sh : reproducible arm64 macOS build of khnmc on the Mac mini, driven from this Linux worktree.
#
#   tools/darwin_build.sh [make-target ...]        default target: all  (./khnmc + the hook link check)
#   env: KHNMC_MAC (ssh target, default user@mac-mini), KHNMC_MAC_DIR (default khnmc-work/build-f20; must be
#        khnmc-work/build-*), JOBS (default 4; more than 4 is refused), MAKE_ARGS (extra make variables, e.g. "OPT=-O1")
#
# What it does, in order:
#   1. syncs Makefile, runtime/, lifter-rt/, tools/ and lifted/gen/ from this worktree to ~/$KHNMC_MAC_DIR (rsync --delete inside
#      those directories only);
#   2. lifted shards: compares the sha256 list of this worktree's lifted/shards with ~/khnmc-work/lift/armfull/shards (the copy an
#      earlier arm64 compile left there).  Identical: the build reads them in place (SHARDS_DIR, never written).  Different or
#      absent: rsync to ~/$KHNMC_MAC_DIR/lifted/shards and use that;
#   3. checks `memory_pressure` (refuses below 30 % free), then runs `nice -n 10 make -j$JOBS` there.  On Darwin the Makefile selects
#      XLATE=2 (bias mode) and HOOKMODE=rename: tools/hook_shards.py writes the renamed shard copies to obj/shards, clang compiles
#      them with the lifter's flags, the runtime is compiled, forwarders are generated, ./khnmc is linked and check_hook_link runs.
# Machine rules (another person's desktop): everything stays inside ~/khnmc-work, niced, at most 4 compile jobs, nothing installed,
# no GUI, no sudo.  The script never touches anything outside ~/$KHNMC_MAC_DIR except READING ~/khnmc-work/lift/armfull/shards.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-f20}
JOBS=${JOBS:-4}
case "$RDIR" in khnmc-work/build-*) ;; *) echo "darwin_build: KHNMC_MAC_DIR must be khnmc-work/build-* (got $RDIR)" >&2; exit 2;; esac
case "$RDIR" in *..*|*" "*) echo "darwin_build: bad KHNMC_MAC_DIR" >&2; exit 2;; esac
case "$JOBS" in ''|*[!0-9]*) echo "darwin_build: JOBS must be a number" >&2; exit 2;; esac
[ "$JOBS" -ge 1 ] && [ "$JOBS" -le 4 ] || { echo "darwin_build: refusing JOBS=$JOBS (at most 4 compile jobs on the Mac mini)" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
RS=(rsync -a --delete -e "ssh -o BatchMode=yes -o ConnectTimeout=20")
cd "$HERE"
for f in Makefile runtime lifter-rt tools lifted/gen lifted/shards; do [ -e "$f" ] || { echo "darwin_build: missing $f in $HERE" >&2; exit 2; }; done

"${SSH[@]}" "$MAC" "mkdir -p ~/$RDIR/lifted/gen"
"${RS[@]}" Makefile "$MAC:$RDIR/Makefile"
"${RS[@]}" runtime/ "$MAC:$RDIR/runtime/"
"${RS[@]}" lifter-rt/ "$MAC:$RDIR/lifter-rt/"
"${RS[@]}" --exclude '__pycache__' tools/ "$MAC:$RDIR/tools/"
"${RS[@]}" lifted/gen/ "$MAC:$RDIR/lifted/gen/"

LOCAL_SUM=$( (cd lifted/shards && sha256sum shard_*.c) | sha256sum | cut -d' ' -f1)
REMOTE_SUM=$("${SSH[@]}" "$MAC" 'cd ~/khnmc-work/lift/armfull/shards 2>/dev/null && shasum -a 256 shard_*.c | shasum -a 256 | cut -d" " -f1 || echo absent')
if [ "$LOCAL_SUM" = "$REMOTE_SUM" ]; then
  SHARDS='$HOME/khnmc-work/lift/armfull/shards'
  echo "darwin_build: lifted shards: ~/khnmc-work/lift/armfull/shards is byte-identical to lifted/shards ($(ls lifted/shards/shard_*.c | wc -l) files, list sha256 ${LOCAL_SUM:0:16}): read in place"
else
  echo "darwin_build: lifted shards: ~/khnmc-work/lift/armfull/shards differs or is absent (here ${LOCAL_SUM:0:16}, there ${REMOTE_SUM:0:16}): syncing to ~/$RDIR/lifted/shards"
  "${SSH[@]}" "$MAC" "mkdir -p ~/$RDIR/lifted/shards"
  "${RS[@]}" -z --include 'shard_*.c' --exclude '*' lifted/shards/ "$MAC:$RDIR/lifted/shards/"
  SHARDS='$HOME/'"$RDIR/lifted/shards"
fi

TARGETS=${*:-all}
"${SSH[@]}" "$MAC" "set -e; cd ~/$RDIR
free=\$(memory_pressure | tail -1 | sed -n 's/.*percentage: \\([0-9]*\\)%.*/\\1/p')
echo \"darwin_build: \$(sw_vers -productVersion) \$(uname -m), \$(clang --version | head -1), memory free \${free:-?}%, load \$(sysctl -n vm.loadavg)\"
[ -n \"\$free\" ] && [ \"\$free\" -ge 30 ] || { echo 'darwin_build: memory free percentage is below 30 % (or unreadable): not starting the build' >&2; exit 3; }
nice -n 10 make -j$JOBS SHARDS_DIR=\"$SHARDS\" ${MAKE_ARGS:-} $TARGETS
if [ -x khnmc ]; then ls -l khnmc; shasum -a 256 khnmc | cut -c1-16; file khnmc; fi"
