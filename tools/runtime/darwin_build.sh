#!/usr/bin/env bash
# tools/darwin_build.sh : reproducible arm64 macOS build of khnmc on the Mac mini, driven from this Linux worktree.
#
#   tools/darwin_build.sh [make-target ...]        default target: all  (./khnmc + the link checks)
#                                                  `rtobjs`: compile only the hand-written runtime and lifter-rt, with make -k
#   env: KHNMC_MAC (ssh target, default user@mac-mini), KHNMC_MAC_DIR (default khnmc-work/build-j26; must be
#        khnmc-work/build-*), JOBS (default 4; more than 4 is refused), MAKE_ARGS (extra make variables, e.g. "IOS_PATHS=1"),
#        KHNMC_MAC_NICE (default 10), KHNMC_MAC_MIN_FREE (default 30: no step starts below this `memory_pressure` reading),
#        KHNMC_MAC_STOP_FREE (default MIN_FREE: between two groups of shards the build waits when the reading is below this),
#        KHNMC_MAC_CHUNK (default 20: shards compiled between two readings)
#   Task 21b: MAKE_ARGS="GFX_VKD3D=1" builds the wrapped graphics bridge in (static vkd3d + MoltenVK from ~/khnmc-work/gfxprobe on the
#        Mac, see the Makefile's Darwin block; that directory is only read).  The input scripts (input/) are synced too.
#
# What it does, in order:
#   1. syncs Makefile, runtime/, lifter-rt/, tools/, lifted/gen/ and the four generated mod sources of gen/ (mod_static_lifted.c,
#      mod_static_table.h, mod_neverland_table.h, mod_moose_table.h: the mod's static edits, NeverlandFly and KH2Moose's two code
#      patches, made on this Linux host by `make`; the
#      Mac build takes them as inputs, GEN_PREBUILT=1) from this worktree to ~/$KHNMC_MAC_DIR (rsync --delete inside those
#      directories only; in gen/ nothing is deleted);
#   2. lifted shards: compares the sha256 list of this worktree's lifted/shards with ~/khnmc-work/lift/armfull/shards (the copy an
#      earlier arm64 compile left there).  Identical: the build reads them in place (SHARDS_DIR, never written).  Different or
#      absent: rsync to ~/$KHNMC_MAC_DIR/lifted/shards and use that;
#   3. runs tools/darwin_remote_build.sh there (see its header: the build is paced by `memory_pressure`).  On Darwin the Makefile
#      selects XLATE=2 (bias mode) and HOOKMODE=rename: tools/hook_shards.py writes the renamed shard copies to obj/shards, clang
#      compiles them with the lifter's flags, the runtime and gen/mod_static_lifted.c are compiled (the latter with the shards'
#      address mode), forwarders are generated, ./khnmc is linked, check_hook_link and check_mod_static_link run.
# Machine rules (another person's desktop): everything stays inside ~/khnmc-work, niced, at most 4 compile jobs, nothing installed,
# no GUI, no sudo.  The script never touches anything outside ~/$KHNMC_MAC_DIR except READING ~/khnmc-work/lift/armfull/shards.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
MAC=${KHNMC_MAC:-user@mac-mini}
RDIR=${KHNMC_MAC_DIR:-khnmc-work/build-j26}
JOBS=${JOBS:-4}
NICE=${KHNMC_MAC_NICE:-10}; MINFREE=${KHNMC_MAC_MIN_FREE:-30}; STOPFREE=${KHNMC_MAC_STOP_FREE:-$MINFREE}; CHUNK=${KHNMC_MAC_CHUNK:-20}
for v in "$NICE" "$MINFREE" "$STOPFREE" "$CHUNK"; do case "$v" in ''|*[!0-9]*) echo "darwin_build: KHNMC_MAC_NICE / _MIN_FREE / _STOP_FREE / _CHUNK must be numbers" >&2; exit 2;; esac; done
[ "$NICE" -le 20 ] && [ "$STOPFREE" -le "$MINFREE" ] && [ "$CHUNK" -ge 1 ] || { echo "darwin_build: need NICE <= 20, STOP_FREE <= MIN_FREE, CHUNK >= 1" >&2; exit 2; }
case "$RDIR" in khnmc-work/build-*) ;; *) echo "darwin_build: KHNMC_MAC_DIR must be khnmc-work/build-* (got $RDIR)" >&2; exit 2;; esac
case "$RDIR" in *..*|*" "*) echo "darwin_build: bad KHNMC_MAC_DIR" >&2; exit 2;; esac
case "$JOBS" in ''|*[!0-9]*) echo "darwin_build: JOBS must be a number" >&2; exit 2;; esac
[ "$JOBS" -ge 1 ] && [ "$JOBS" -le 4 ] || { echo "darwin_build: refusing JOBS=$JOBS (at most 4 compile jobs on the Mac mini)" >&2; exit 2; }
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=20)
RS=(rsync -a --delete -e "ssh -o BatchMode=yes -o ConnectTimeout=20")
cd "$HERE"
for f in Makefile runtime lifter-rt tools input lifted/gen lifted/shards gen/mod_static_lifted.c gen/mod_static_table.h gen/mod_neverland_table.h gen/mod_moose_table.h; do [ -e "$f" ] || { echo "darwin_build: missing $f in $HERE" >&2; exit 2; }; done

"${SSH[@]}" "$MAC" "mkdir -p ~/$RDIR/lifted/gen ~/$RDIR/gen"
"${RS[@]}" Makefile "$MAC:$RDIR/Makefile"
"${RS[@]}" runtime/ "$MAC:$RDIR/runtime/"
"${RS[@]}" lifter-rt/ "$MAC:$RDIR/lifter-rt/"
"${RS[@]}" --exclude '__pycache__' tools/ "$MAC:$RDIR/tools/"
"${RS[@]}" input/ "$MAC:$RDIR/input/"
"${RS[@]}" lifted/gen/ "$MAC:$RDIR/lifted/gen/"
rsync -a -z -e "ssh -o BatchMode=yes -o ConnectTimeout=20" gen/mod_static_lifted.c gen/mod_static_table.h gen/mod_neverland_table.h gen/mod_moose_table.h "$MAC:$RDIR/gen/"
GL=$(sha256sum gen/mod_static_lifted.c gen/mod_static_table.h gen/mod_neverland_table.h gen/mod_moose_table.h | cut -d' ' -f1 | tr '\n' ' ')
GR=$("${SSH[@]}" "$MAC" "cd ~/$RDIR && shasum -a 256 gen/mod_static_lifted.c gen/mod_static_table.h gen/mod_neverland_table.h gen/mod_moose_table.h | cut -d' ' -f1 | tr '\n' ' '")
[ "$GL" = "$GR" ] || { echo "darwin_build: gen/ differs after the copy" >&2; exit 1; }
echo "darwin_build: gen/: mod_static_lifted.c ${GL:0:16}, mod_static_table.h $(echo "$GL" | cut -d' ' -f2 | cut -c1-16), mod_neverland_table.h $(echo "$GL" | cut -d' ' -f3 | cut -c1-16), mod_moose_table.h $(echo "$GL" | cut -d' ' -f4 | cut -c1-16): equal on both sides"

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
case "$TARGETS" in *[!A-Za-z0-9_./\ -]*) echo "darwin_build: bad make target" >&2; exit 2;; esac
case "${MAKE_ARGS:-}" in *[!A-Za-z0-9_./=\ -]*) echo "darwin_build: MAKE_ARGS may hold VAR=value words only" >&2; exit 2;; esac
if [ "$TARGETS" = rtobjs ]; then
  "${SSH[@]}" "$MAC" "cd ~/$RDIR && bash tools/darwin_remote_build.sh $JOBS $NICE $MINFREE $STOPFREE $CHUNK \"$SHARDS\" '${MAKE_ARGS:-}' rtobjs"
else
  "${SSH[@]}" -o ServerAliveInterval=30 -o ServerAliveCountMax=10 "$MAC" "cd ~/$RDIR && bash tools/darwin_remote_build.sh $JOBS $NICE $MINFREE $STOPFREE $CHUNK \"$SHARDS\" '${MAKE_ARGS:-}' $TARGETS"
fi
