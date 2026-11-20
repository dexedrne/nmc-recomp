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
# CUTSCENES=theater THEATER_PACK=FILE installs the isolated Theater pack.
# CUTSCENES=story (TASK50; play and route modes): the story scenes the Nobody May Cry mod emptied come back (runtime/cutscenes.c):
#   every configuration this call writes (play, GFX play, the route's) gets env KHNMC_CUTSCENES=story.  The game reads the pack of
#   original scenes from Documents/data/story-events.khcs (tools/cutscene_pack.py writes it on the build-host); CUTSCENES_PACK=FILE (required, a file
#   on this Mac) copies it there first.  Requires a fresh install and a matching reviewed pack digest in the signed app.
#   CUTSCENES=nmc or unset: no line (the mod's empty scenes, as before).  A later call without CUTSCENES writes its configurations
#   without the line, as for every other option.
#   SKIP_INSTALL=1 skips steps 1 and 2 apart from the fresh status file (the app, the exe and the conf are already there).
#   SOUND=1 (route mode only): the game's sound plays through the speaker; otherwise every test run is silent (KHNMC_AUDIO=null).
# The phone's device id is read from ~/Developer/KHNMC/state/device.env (DEVICE=...), never from this repository.  Never names a
# simulator.  Results in ~/Developer/KHNMC/run/device-<mode>-<time>/.  The app must stay in the foreground with the screen on:
# iOS suspends it otherwise.
# First runs 2026-11-18 (iPhone, A20 Pro, iOS 27.2): 169,479 checks, 0 failures, about 16 s; 381 checks need fork() and are NOT RUN.
# GFX=1 (TASK38; IOS-GRAPHICS-PLAN steps 4-5): the signed app has the graphics backend (GFX=1 mac/build.sh iphoneos, GFX=1
#   mac/sign_app.sh; checked here) and the selftest or route runs on it: khnmc.conf gets env KHNMC_GFX=vkd3dw (the wrapped bridge; in an
#   Apple app build MoltenVK is the default driver), VKD3D_DEBUG=warn and VKD3D_SHADER_DEBUG=warn (vkd3d's err / fixme / warn lines go to
#   khnmc.log and are counted here), MVK_CONFIG_PERFORMANCE_TRACKING=1 (MoltenVK counts gpuMemoryAllocated, the census reads it, only
#   with it).  The device report (`kh: gfx device report:` block, the presentation-extensions line, any `kh: FATAL: gfx-...` line,
#   e.g. gfx-no-bc) is copied out of the log into device-report.txt; a GFX run without a complete block fails.
#   Route mode with GFX=1 only (plan step 5, M1): DUMP_EVERY=n -> every n-th presented picture into Documents/frames
#   (KHNMC_GFX_DUMP=@DOCS@/frames, KHNMC_GFX_DUMP_EVERY=n; frame-NNNNNN.ppm, about 6 MB each at 1920x1080, overwritten by later runs, plus
#   index.tsv, which the runtime appends to: its lines from before this run are counted first and only this run's are used);
#   FETCH="240 920 2000 2600" copies those presents' pictures back into frames/ (each a multiple of n, each must be in this run's index);
#   HASHLOG=1 -> KHNMC_GFX_HASHLOG=@DOCS@/hash.tsv, fetched; CLOCK=frames -> KHNMC_CLOCK=frames.  Each of these without GFX=1 route is
#   refused, and GFX=1 play is refused (no picture reaches the screen before plan step 10).  Without GFX=1 every run is as before.
# PRESENT=1 (TASK41, IOS-GRAPHICS-PLAN step 10; needs GFX=1): the picture on the phone's screen.  khnmc.conf also gets
#   env KHNMC_GFX_PRESENT=layer (the presenter draws into the game view's CAMetalLayer), env KHNMC_SCREEN=layer (the game's screen is the
#   layer's drawable: the shell hands its size over before the game starts) and env KHNMC_ASPECT=fill (task 37: the game fills that
#   screen); PRESENT_CHECK=n adds env KHNMC_GFX_PRESENT_CHECK=n (every n-th present checked against a readback, when the surface reports
#   TRANSFER_SRC).  The surface caps (`kh: gfx present: surface caps`), the swapchain lines and the life-cycle lines (`kh: app:`) are copied
#   into present-report.txt.  GFX=1 PRESENT=1 play is allowed: that conf stays on the phone (the icon then starts the game with the
#   picture); every later test run puts the null-backend play conf back.  Without PRESENT=1 nothing of this is written.
# TASK43 (IOS-GRAPHICS-PLAN step 12, and the play-mode log):
#   The disk pipeline cache is ON by default in the graphics app (Library/Caches/khnmc/pipe.bin; runtime/gfx_pipecache.h).  For GFX=1
#   ROUTE runs: PIPECACHE=fresh -> env KHNMC_GFX_PIPECACHE_RESET=1 (the app removes the file at start-up: this run is a first run and
#   writes a new file at its exit; refused in every other mode: the runtime honours that line at EVERY start, and a play configuration
#   stays on the phone); PIPECACHE=off -> env KHNMC_GFX_PIPECACHE=off (no cache at all: the baseline); unset -> the default (a file
#   that is there is used).  HITCH=n (GFX=1 route) -> env KHNMC_GFX_HITCH=n: the Present-to-Present intervals above 25 ms over the
#   first n presents after the field came up.  The lines `kh: gfx pipeline cache:`, `kh: gfx pipelines`, `kh: gfx MoltenVK shader
#   statistics`, `kh: gfx frame times` and `kh: gfx vkd3d log` are copied into pipecache-report.txt, and the last of each is shown.
#   GFX=1 PRESENT=1 PLAY (the configuration an icon launch uses): the cache is used only once its picture checks have passed on this
#   phone (MAC-STEPS-43.md sections 2 and 3; then `touch $STATE/pipecache-verified` on this Mac).  Until that file exists the play
#   configuration gets env KHNMC_GFX_PIPECACHE=off, and the script says so; PIPECACHE=on (play only) or PIPECACHE=off decides for one call.
#   PLAY mode is quiet WITHOUT hiding vkd3d's failures (vkd3d logs them at class warn: "Failed to create Vulkan graphics pipeline",
#   "Failed to compile shader", "Failed to begin render pass, ignoring draw call"): VKD3D_DEBUG=warn with the runtime's own filter,
#   env KHNMC_GFX_VKD3D_LOG_REPEATS=8 (each vkd3d log site prints its first 8 lines, then a notice and one line at every power of ten:
#   `Aliasing barriers not implemented yet`, about 15,000 lines per story route in the test modes, becomes a dozen).  An app older
#   than that filter gets vkd3d's default level instead (VKD3D_DEBUG=fixme), as before.  The per-present
#   line every 600 presents (env KHNMC_GFX_FRAME_LINE_EVERY=600), and the log is size-bounded (`logcap 16M`: khnmc.log + khnmc.log.1,
#   at most 2 x 16 MiB; only when the signed app knows the directive).  MoltenVK's 108 x `Could not create MTLCounterSampleBuffer ...
#   VK_QUERY_TYPE_TIMESTAMP. Reverting to emulated behavior` at start-up stay (the game's own 140 two-query timestamp heaps; MoltenVK has
#   no setting for emulated timestamps): they are counted apart from other [mvk-error] lines below.
# GFX_APP=1|0 (review X38 n=1): the signed app must have (1) / must not have (0) the graphics backend, whatever backend the run uses;
#   checked with nm before anything is installed.  The plain (null-backend) selftest of the graphics app is `GFX_APP=1 device_run.sh
#   selftest`.  GFX=1 implies GFX_APP=1 (GFX=1 GFX_APP=0 is refused).  Unset: no check, as before.
. "$(dirname "$0")/lib.sh"
MODE=${1:-selftest}; INPUT=""; FRAMES=7000; DIFF=none
# task 39: SELFTEST_ONLY=membench|ordered|gate (environment, selftest mode only): one suite alone (KHNMC_SELFTEST_ONLY), e.g. the
# memory-ordering microbenchmark of an ORDERED_MEM=1 app against a plain one
SONLY=${SELFTEST_ONLY:-}; case "$SONLY" in ''|membench|ordered|gate|app) ;; *) echo "ios/mac: device_run: SELFTEST_ONLY must be membench, ordered, gate or app" >&2; exit 2;; esac
case "$MODE" in
  selftest) LIMIT=${2:-1500};;
  play) LIMIT=0;;
  route) INPUT=${2:-}; FRAMES=${3:-7000}; DIFF=${4:-none}; LIMIT=${5:-2400}
         [ -f "$INPUT" ] || { echo "ios/mac: device_run: route needs an input script of src/ (got '${INPUT}')" >&2; exit 2; }
         case "$DIFF" in none|0|1|2|3) ;; *) echo "ios/mac: device_run: DIFFICULTY is none or 0..3" >&2; exit 2;; esac;;
  *) sed -n '2,13p' "$0" >&2; exit 2;;
esac
DUMP_EVERY=${DUMP_EVERY:-}; FETCH=${FETCH:-}; HASHLOG=${HASHLOG:-0}; CLOCK=${CLOCK:-}
bad(){ echo "ios/mac: device_run: $*" >&2; exit 2; }
PIPECACHE=${PIPECACHE:-}; HITCH=${HITCH:-}                 # TASK43
case "$PIPECACHE" in ''|fresh|off|on) ;; *) bad "PIPECACHE is fresh (route), off, on (play) or unset (got '$PIPECACHE')";; esac
[ -z "$PIPECACHE" ] || [ "$GFX" = 1 ] || bad "PIPECACHE needs GFX=1 (the pipeline cache belongs to the vkd3d backend)"
[ "$PIPECACHE" != fresh ] || [ "$MODE" = route ] || bad "PIPECACHE=fresh is for GFX=1 route runs only: the app removes the cache file at EVERY start while that line is in its configuration, and a play configuration stays on the phone (every icon launch would be a first run)"
[ "$PIPECACHE" != on ] || [ "$MODE" = play ] || bad "PIPECACHE=on is for GFX=1 PRESENT=1 play only (test runs use the cache unless PIPECACHE=off or fresh)"
[ -z "$HITCH" ] || { [ "$GFX" = 1 ] && [ "$MODE" = route ] || bad "HITCH is for GFX=1 route runs only"; case "$HITCH" in *[!0-9]*|0*) bad "HITCH must be a positive number of presents (got '$HITCH')";; esac; }
PRESENT=${PRESENT:-0}; PRESENT_CHECK=${PRESENT_CHECK:-}
case "$PRESENT" in 0|1) ;; *) bad "PRESENT is 0 or 1 (got '$PRESENT')";; esac
[ "$PRESENT" = 1 ] && [ "$GFX" != 1 ] && bad "PRESENT=1 needs GFX=1 (the picture comes from the vkd3d backend)"
[ -z "$PRESENT_CHECK" ] || { [ "$PRESENT" = 1 ] || bad "PRESENT_CHECK needs PRESENT=1"; case "$PRESENT_CHECK" in *[!0-9]*|0*) bad "PRESENT_CHECK must be a positive number of presents (got '$PRESENT_CHECK')";; esac; }
[ "$GFX" = 1 ] && [ "$PRESENT" != 1 ] && [ "$MODE" = play ] && bad "GFX=1 play needs PRESENT=1 (without it no picture reaches the screen); play stays on the null backend otherwise"
if [ -n "$DUMP_EVERY$FETCH$CLOCK" ] || [ "$HASHLOG" != 0 ]; then [ "$GFX" = 1 ] && [ "$MODE" = route ] || bad "DUMP_EVERY, FETCH, HASHLOG and CLOCK are for GFX=1 route runs only"; fi
case "$HASHLOG" in 0|1) ;; *) bad "HASHLOG is 0 or 1 (got '$HASHLOG')";; esac
case "$CLOCK" in ''|frames) ;; *) bad "CLOCK is frames or unset (got '$CLOCK')";; esac
[ -z "$DUMP_EVERY" ] || case "$DUMP_EVERY" in *[!0-9]*|0*) bad "DUMP_EVERY must be a positive number of presents (got '$DUMP_EVERY')";; esac
GFX_APP=${GFX_APP:-}; case "$GFX_APP" in ''|0|1) ;; *) bad "GFX_APP is 0 or 1 (got '$GFX_APP')";; esac
CUTSCENES=${CUTSCENES:-}; CUTSCENES_PACK=${CUTSCENES_PACK:-}                 # TASK50
# EXTRA_ENV="KHNMC_TITLE_CREDIT=hide" hides the title credit on the next launch (default: show).
# EXTRA_ENV="KHNMC_A=x KHNMC_B=y" (play and route modes; 11/19): extra switches written as `env` lines with the cutscene line
#   (e.g. KHNMC_PLAYER_LOOK=xion KHNMC_XKB_MENU=1 KHNMC_SCENE_ROXAS=1); only KHNMC_* names, no spaces in values.
# LOOK_PACK=DIR (11/19, task 56): every file under DIR on this Mac goes to the app's Documents/looks/xion/<same relative path>
#   (the default KHNMC_LOOK_DIR on iOS), one devicectl copy per file; nothing on the phone is removed.
# SCENE_ROXAS_PACK is the directory containing obj/ and remastered/ (task 64).
EXTRA_ENV=${EXTRA_ENV:-}; LOOK_PACK=${LOOK_PACK:-}; HOODED_LOOK_PACK=${HOODED_LOOK_PACK:-}; SCENE_ROXAS_PACK=${SCENE_ROXAS_PACK:-}
# A copied private/ tree ships its logos alongside looks/. LOGO_PACK can point elsewhere.
# RMC_PACK is a sparse pack. An explicitly empty directory clears the phone overlay.
RMC_PACK=${RMC_PACK:-}
[ -z "$RMC_PACK" ] || [ -d "$RMC_PACK" ] || bad "RMC_PACK: no directory '$RMC_PACK' on this Mac"
LOGO_PACK=${LOGO_PACK:-}
if [ -z "$LOGO_PACK" ] && [ -d "$SRC/private/logos" ]; then LOGO_PACK=$SRC/private/logos; fi
if [ -n "$LOGO_PACK" ]; then
  for file in {roxas,xion,xkb}.{2ld,png}; do
    [ -s "$LOGO_PACK/$file" ] || bad "LOGO_PACK: missing $file"
  done
fi
if [ -n "$SCENE_ROXAS_PACK" ]; then
  for rel in obj/H_EX500.mdlx remastered/obj/H_EX500.mdlx/-{0,1,2}.dds; do
    [ -s "$SCENE_ROXAS_PACK/$rel" ] || bad "SCENE_ROXAS_PACK: missing $rel"
  done
fi
for kv in $EXTRA_ENV; do case "$kv" in KHNMC_*=*) k=${kv%%=*}; case "$k" in *[!A-Z0-9_]*) bad "EXTRA_ENV: bad name '$k'";; esac;; *) bad "EXTRA_ENV: '$kv' is not KHNMC_NAME=value";; esac; done
[ -z "$HOODED_LOOK_PACK" ] || [ -d "$HOODED_LOOK_PACK" ] || bad "HOODED_LOOK_PACK: missing directory"
[ -z "$LOOK_PACK" ] || [ -d "$LOOK_PACK" ] || bad "LOOK_PACK: no directory '$LOOK_PACK' on this Mac"
case "$CUTSCENES" in ''|nmc|story|theater) ;; *) bad "CUTSCENES is nmc, story or theater (got '$CUTSCENES')";; esac
uses_pack(){ [ "$CUTSCENES" = story ] || [ "$CUTSCENES" = theater ]; }
pack_name=story-events.khcs
pack_option=CUTSCENES_PACK
if [ "$CUTSCENES" = theater ]; then
  [ -z "$CUTSCENES_PACK" ] || bad "use THEATER_PACK for Theater"
  CUTSCENES_PACK=${THEATER_PACK:-}
  pack_option=THEATER_PACK
  pack_name=theater-events.khtr
elif [ -n "${THEATER_PACK:-}" ]; then
  bad "THEATER_PACK needs CUTSCENES=theater"
fi
! uses_pack || [ "$MODE" != selftest ] || bad "CUTSCENES=$CUTSCENES is for play and route runs (the selftest tests the switch itself)"
[ -z "$CUTSCENES_PACK" ] || { uses_pack || bad "CUTSCENES_PACK needs CUTSCENES=story"; [ -f "$CUTSCENES_PACK" ] || bad "$pack_option: no file '$CUTSCENES_PACK' on this Mac"; }
if uses_pack; then
  [ "${SKIP_INSTALL:-0}" != 1 ] || bad "$CUTSCENES mode requires installation of the verified signed app"
  [ -s "$CUTSCENES_PACK" ] || bad "$CUTSCENES mode needs $pack_option on this Mac"
fi
[ "$GFX" = 1 ] && [ "$GFX_APP" = 0 ] && bad "GFX=1 runs the graphics backend: GFX_APP=0 (an app without it) contradicts that"
[ "$GFX" = 1 ] && GFX_APP=1
for n in $FETCH; do case "$n" in *[!0-9]*|0*) bad "FETCH holds '$n', not a present number";; esac
  [ -n "$DUMP_EVERY" ] || bad "FETCH needs DUMP_EVERY"; [ $((n % DUMP_EVERY)) = 0 ] || bad "FETCH present $n is not a multiple of DUMP_EVERY=$DUMP_EVERY: it is never dumped"; done
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
if [ -n "$GFX_APP" ]; then gfx_has "$APP/KHNMC"; hb=$?
  [ $hb = 2 ] && { say "nm could not read the signed app $APP/KHNMC: whether it has the graphics backend is unknown (GFX_APP=$GFX_APP): not run"; exit 2; }
  [ $hb = 1 ] && [ "$GFX_APP" = 1 ] && { say "GFX$([ "$GFX" = 1 ] && echo =1 || echo _APP=1), but the signed app has no graphics backend (GFX=1 mac/build.sh iphoneos, then GFX=1 mac/sign_app.sh)"; exit 2; }
  [ $hb = 0 ] && [ "$GFX_APP" = 0 ] && { say "GFX_APP=0, but the signed app HAS the graphics backend (mac/build.sh iphoneos, then mac/sign_app.sh, both without GFX=1)"; exit 2; }
  say "the signed app $([ $hb = 0 ] && echo 'has the' || echo 'has no') graphics backend, as GFX_APP=$GFX_APP asks; this run uses the $([ "$GFX" = 1 ] && echo 'vkd3d backend (KHNMC_GFX=vkd3dw)' || echo 'null backend')"; fi
app_has_cutscenes(){ grep -a -q "$pack_digest" "$APP/KHNMC"; }
if uses_pack; then
  if [ "$CUTSCENES" = theater ]; then
    grep -a -q 'theater-entry-without-snapshot' "$APP/KHNMC" || { say "signed app lacks Theater isolation"; exit 2; }
  fi
  expected_magic=KHCS
  [ "$CUTSCENES" != theater ] || expected_magic=KHTR
  [ "$(head -c 4 "$CUTSCENES_PACK")" = "$expected_magic" ] || { say "wrong pack format for $CUTSCENES"; exit 2; }
  pack_digest=$(shasum -a 256 < "$CUTSCENES_PACK") || { say "cannot hash $CUTSCENES pack"; exit 2; }
  pack_digest=${pack_digest%% *}
  [ "${#pack_digest}" = 64 ] && app_has_cutscenes || { say "signed app and selected pack do not match"; exit 2; }
fi
printf 'starting\n' > "$OUT/status.init"
if [ "${SKIP_INSTALL:-0}" != 1 ]; then
  [ -f "$KROOT/pc/kh2fm.exe" ] || { say "no $KROOT/pc/kh2fm.exe (tools/ios/sync.sh exe)"; exit 2; }
  say "install"; xcrun devicectl device install app --device "$DEVICE" --timeout 300 "$APP" >> "$OUT/devicectl.log" 2>&1 \
    || { say "install FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  cp_to "$KROOT/pc/kh2fm.exe" kh2fm.exe || { say "copy of kh2fm.exe FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
fi
# TASK43: the play configurations are quiet.  `logcap` is a conf DIRECTIVE: an app older than TASK43 would stop at it (host-conf: unknown
# directive), so it is written only when the signed app knows it; the env lines are harmless for an older app (it ignores them).
app_has_logcap(){ grep -a -q 'logcap needs a log directive' "$APP/KHNMC" 2>/dev/null; }
quiet_lines(){ if app_has_logcap; then echo "logcap 16M"; fi; echo "env KHNMC_GFX_FRAME_LINE_EVERY=600"; }
# TASK50: the cutscenes line, in every configuration this call writes (the app must know the variable: else it plays the stubs silently)
cut_lines(){ if uses_pack; then echo "env KHNMC_CUTSCENES=$CUTSCENES"; fi; for kv in $EXTRA_ENV; do echo "env $kv"; done; }
if [ -n "$SCENE_ROXAS_PACK" ]; then
  for rel in obj/H_EX500.mdlx remastered/obj/H_EX500.mdlx/-{0,1,2}.dds; do
    cp_to "$SCENE_ROXAS_PACK/$rel" "scene-roxas/$rel" || { say "scene Roxas copy failed at $rel"; exit 1; }
  done
  say "scene Roxas model and three textures copied to Documents/scene-roxas"
fi
if [ -n "${LOGO_PACK:-}" ]; then
  for logo in roxas xion xkb; do
    for ext in 2ld png; do
      file=$logo.$ext
      cp_to "$LOGO_PACK/$file" "logos/$file" || { say "title logo copy failed at $file"; exit 1; }
    done
  done
fi
if [ -n "$RMC_PACK" ]; then
  grep -a -q 'rmc-manifest-incomplete' "$APP/KHNMC" || { say "RMC_PACK needs an app built with RMC support"; exit 2; }
  (cd "$RMC_PACK" && find . ! -type f ! -type d -print0) > "$OUT/rmc.unsupported" || { say "cannot enumerate RMC_PACK"; exit 1; }
  [ ! -s "$OUT/rmc.unsupported" ] || { say "RMC_PACK must contain regular files and directories; use a sparse pack"; exit 2; }
  (cd "$RMC_PACK" && find . -type f -print0) > "$OUT/rmc.list" || { say "cannot enumerate RMC_PACK"; exit 1; }
  : > "$OUT/rmc-pack.pending" || exit 1
  n=0
  while IFS= read -r -d '' rel; do
    rel=${rel#./}
    cp_to "$RMC_PACK/$rel" "rmc/$rel" || { say "RMC copy failed at $rel"; exit 1; }
    printf '%s\0' "$rel" >> "$OUT/rmc-pack.pending" || exit 1
    n=$((n+1))
  done < "$OUT/rmc.list"
  printf '/\0' >> "$OUT/rmc-pack.pending" || exit 1
  cp_to "$OUT/rmc-pack.pending" rmc-pack.pending || { say "RMC inventory copy failed"; exit 1; }
  say "RMC pack: $n files copied to Documents/rmc; launch applies the inventory"
fi
for costume in xion xion-hooded; do
  pack=$LOOK_PACK
  [ "$costume" != xion-hooded ] || pack=$HOODED_LOOK_PACK
  [ -n "$pack" ] || continue
  n=0; ( cd "$pack" && find . -type f | sed 's#^\./##' ) > "$OUT/$costume.list"
  while IFS= read -r rel; do cp_to "$pack/$rel" "looks/$costume/$rel" || { say "copy of $costume FAILED at $rel"; exit 1; }; n=$((n+1)); done < "$OUT/$costume.list"
  say "look pack: $n files copied to Documents/looks/$costume"
done
if uses_pack; then
  cp_to "$CUTSCENES_PACK" "data/$pack_name" || { say "copy of the cutscene pack FAILED"; exit 1; }
fi
play_conf(){ { echo "# the game (tools/ios/mac/device_run.sh play): the controller is pad 0, sound on, no script, no time limit"; echo "log khnmc.log"; echo "status khnmc.status"; quiet_lines; cut_lines; } > "$OUT/play.conf"; }
[ -x "$APP/KHNMC" ] && { app_has_logcap || say "the signed app does not know the conf directive logcap (older than TASK43): the play configuration is written without a log size cap"; }
play_conf
# TASK41: the conf lines of a run with the picture on the screen (GFX=1 PRESENT=1)
pc_lines(){ case "$PIPECACHE" in fresh) echo "env KHNMC_GFX_PIPECACHE_RESET=1";; off) echo "env KHNMC_GFX_PIPECACHE=off";; esac; }
gfx_lines(){ :; }; if [ "$GFX" = 1 ]; then gfx_lines(){ echo "env KHNMC_GFX=vkd3dw"; echo "env VKD3D_DEBUG=warn"; echo "env VKD3D_SHADER_DEBUG=warn"; echo "env MVK_CONFIG_PERFORMANCE_TRACKING=1"; pc_lines; }; fi
# Review C43 n=1, n=4, n=7: the cache line of the PLAY configuration, the one that stays on the phone.  Never the RESET line (the app
# honours it at every start).  The cache is used by an icon launch only once its picture checks passed on this phone (MAC-STEPS-43
# sections 2 and 3, then `touch $STATE/pipecache-verified`); PIPECACHE=on|off decides for one call.
pc_play_lines(){ case "$PIPECACHE" in off) echo "env KHNMC_GFX_PIPECACHE=off";; on) :;; *) [ -f "$STATE/pipecache-verified" ] || echo "env KHNMC_GFX_PIPECACHE=off";; esac; }
# Review C43 n=2: play mode keeps vkd3d at warn (its failures are logged at that class) behind the runtime's de-duplicating filter; an
# app that does not know the filter gets vkd3d's default level, as before (the repeat of a FIXME_ONCE site is class warn and stays off)
app_has_vklog(){ grep -a -q 'KHNMC_GFX_VKD3D_LOG_REPEATS' "$APP/KHNMC" 2>/dev/null; }
gfx_play_lines(){ :; }; if [ "$GFX" = 1 ]; then gfx_play_lines(){ echo "env KHNMC_GFX=vkd3dw"; if app_has_vklog; then echo "env VKD3D_DEBUG=warn"; echo "env VKD3D_SHADER_DEBUG=warn"; echo "env KHNMC_GFX_VKD3D_LOG_REPEATS=8"; else echo "env VKD3D_DEBUG=fixme"; echo "env VKD3D_SHADER_DEBUG=fixme"; fi; echo "env MVK_CONFIG_PERFORMANCE_TRACKING=1"; pc_play_lines; }; fi
# HUD=1 (owner 2026-11-18, default 1 for runs with the picture; HUD=0 turns it off): Apple's Metal performance HUD (fps, frame and GPU time)
present_lines(){ echo "env KHNMC_GFX_PRESENT=layer"; echo "env KHNMC_SCREEN=layer"; echo "env KHNMC_ASPECT=fill"; [ "${HUD:-1}" = 1 ] && echo "env KHNMC_HUD=1"; [ -z "$PRESENT_CHECK" ] || echo "env KHNMC_GFX_PRESENT_CHECK=$PRESENT_CHECK"; }
if [ "$MODE" = play ] && [ "$PRESENT" = 1 ]; then
  { echo "# the game with its picture (tools/ios/mac/device_run.sh GFX=1 PRESENT=1 play): the controller is pad 0, sound on, no script"; echo "log khnmc.log"; echo "status khnmc.status"; quiet_lines; gfx_play_lines; present_lines; cut_lines; } > "$OUT/gfx-play.conf"
  if grep -q '^env KHNMC_GFX_PIPECACHE=off$' "$OUT/gfx-play.conf"; then say "the disk pipeline cache is OFF in this play configuration (PIPECACHE=off, or no $STATE/pipecache-verified yet: MAC-STEPS-43.md sections 2 and 3 first, then touch that file; PIPECACHE=on decides for this call)"
  else say "the disk pipeline cache is ON in this play configuration (the app's own file, Library/Caches/khnmc/pipe.bin)"; fi
  if grep -q '^env KHNMC_GFX_VKD3D_LOG_REPEATS=' "$OUT/gfx-play.conf"; then say "vkd3d logs at warn behind the runtime's filter (8 lines per site, then counted)"; else say "the signed app does not know KHNMC_GFX_VKD3D_LOG_REPEATS (older than the TASK43 review): vkd3d logs at its default level, its warn-class failure lines are NOT in the play log"; fi
  cp_to "$OUT/gfx-play.conf" khnmc.conf || { say "copy of the GFX play configuration FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  # NOLAUNCH=1 (owner not playing right now): leave the configuration on the phone and do not start the game; the icon starts it
  if [ "${NOLAUNCH:-0}" = 1 ]; then say "the play configuration with the picture is on the phone; NOT launched (NOLAUNCH=1): the icon starts it"; echo "$OUT"; exit 0; fi
  xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1 || { say "launch FAILED (phone locked?)"; tail -5 "$OUT/devicectl.log"; exit 1; }
  say "the game is running on the phone with its picture (GFX=1 PRESENT=1 play); its log: Documents/khnmc.log (fetch it with SKIP_INSTALL=1 ... or devicectl copy from)"; echo "$OUT"; exit 0
fi
if [ "$MODE" = play ]; then
  cp_to "$OUT/play.conf" khnmc.conf || { say "copy of the play configuration FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
  xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1 || { say "launch FAILED (phone locked?)"; tail -5 "$OUT/devicectl.log"; exit 1; }
  say "the game is running on the phone (play mode); its log: Documents/khnmc.log"; echo "$OUT"; exit 0
fi
{ echo "# written by tools/ios/mac/device_run.sh ($MODE ${INPUT})"; echo "log khnmc.log"; echo "status khnmc.status"
  # no sound in test runs unless SOUND=1 (route mode): a selftest must never reach the speaker (2026-11-18: a test signal did)
  if [ "$MODE" = selftest ] || [ "${SOUND:-0}" != 1 ]; then echo "env KHNMC_AUDIO=null"; fi
  if [ "$MODE" = selftest ]; then echo "arg --selftest"; if [ -n "$SONLY" ]; then echo "env KHNMC_SELFTEST_ONLY=$SONLY"; fi
  else TAG=$(basename "$OUT"); echo "env KH_ROOT=@TMP@/$TAG/root"; echo "env KH_DOCS=@TMP@/$TAG/docs"   # fresh per run, as tools/route_batch.sh: a config the game saved in an earlier run changes how it boots
    echo "env KHNMC_INPUT_SCRIPT=@DOCS@/input.txt"; echo "env KH_RUN_GAME_FRAMES=$FRAMES"; echo "env KH_RUN_SECONDS=$((LIMIT-120))"
    echo "env KH_DUMP_THREADS=1"; echo "env KHNMC_PHASELOG=@DOCS@/phase.log"; echo "env KHNMC_FILELOG=@DOCS@/files.log"
    DV=KHNMC_DIFFICULTY; grep -qE '^[[:space:]]*@(wait|press|select|expect)([[:space:]]|$)' "$INPUT" && DV=KHNMC_ROUTE_DIFFICULTY   # as tools/route_batch.sh
    [ "$DIFF" = none ] || echo "env $DV=$DIFF"; cut_lines; fi
  if [ "$GFX" = 1 ]; then gfx_lines; [ "$PRESENT" = 1 ] && present_lines
    [ -z "$DUMP_EVERY" ] || { echo "env KHNMC_GFX_DUMP=@DOCS@/frames"; echo "env KHNMC_GFX_DUMP_EVERY=$DUMP_EVERY"; }
    [ "$HASHLOG" = 0 ] || echo "env KHNMC_GFX_HASHLOG=@DOCS@/hash.tsv"
    [ -z "$HITCH" ] || echo "env KHNMC_GFX_HITCH=$HITCH"
    [ -z "$CLOCK" ] || echo "env KHNMC_CLOCK=$CLOCK"; fi
} > "$OUT/khnmc.conf"
IX0=0
if [ -n "$DUMP_EVERY" ]; then mkdir -p "$OUT/frames"
  if cp_from frames/index.tsv; then IX0=$(wc -l < "$OUT/frames/index.tsv" | tr -d ' '); mv "$OUT/frames/index.tsv" "$OUT/frames/index-before.tsv"; fi
  say "Documents/frames/index.tsv held $IX0 lines before this run (this run's are appended after them)"; fi
cp_to "$OUT/khnmc.conf" khnmc.conf || { say "copy of khnmc.conf FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
if [ "$MODE" = route ]; then cp "$INPUT" "$OUT/input.txt"; cp_to "$OUT/input.txt" input.txt || { say "copy of the input script FAILED"; exit 1; }; fi
cp_to "$OUT/status.init" khnmc.status || { say "copy of the status file FAILED"; tail -5 "$OUT/devicectl.log"; exit 1; }
tw=$SECONDS; n=0
until xcrun devicectl device process launch --device "$DEVICE" --timeout 60 --terminate-existing "$BUNDLE_ID" >> "$OUT/devicectl.log" 2>&1; do
  n=$((n+1)); [ $n = 1 ] && say "launch refused (phone locked?): retrying every 20 s"
  [ $((SECONDS-tw)) -ge "${WAIT_UNLOCK_S:-1800}" ] && { say "launch FAILED for $((SECONDS-tw)) s"; tail -5 "$OUT/devicectl.log"
    cp_to "$OUT/play.conf" khnmc.conf || say "WARNING: the play configuration could not be put back"; exit 1; }   # review r9: the icon starts the game, not this test
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
xrc=0
if [ "$GFX" = 1 ]; then
  grep -a -e '^kh: gfx device report: ' -e '^kh: gfx vkd3d: presentation extensions:' -e '^kh: FATAL: gfx-' "$OUT/khnmc.log" > "$OUT/device-report.txt" 2>/dev/null
  if grep -q '^kh: gfx device report: END (' "$OUT/device-report.txt"; then
    say "device report: $(grep -c '^kh: gfx device report: ' "$OUT/device-report.txt") lines in device-report.txt; $(grep -a 'BC decision' "$OUT/device-report.txt" | sed 's/^kh: gfx device report: //' | cut -c1-150)"
  else say "GFX=1, but khnmc.log has no complete device report block (device-report.txt holds what there is)"; xrc=1; fi
  f=$(grep -a -m1 '^kh: FATAL: gfx-' "$OUT/khnmc.log"); [ -z "$f" ] || say "$f"
  # TASK43: the known start-up lines of MoltenVK's timestamp-pool fallback are counted apart, so any OTHER [mvk-error] stands out
  mvk_all=$(grep -a -c '\[mvk-error\]' "$OUT/khnmc.log"); mvk_ts=$(grep -a '\[mvk-error\]' "$OUT/khnmc.log" | grep -c 'MTLCounterSampleBuffer.*VK_QUERY_TYPE_TIMESTAMP.*Reverting to emulated behavior')
  say "vkd3d lines in khnmc.log: err $(grep -a -c -E '^vkd3d:([0-9]+:)?err:' "$OUT/khnmc.log"), fixme $(grep -a -c -E '^vkd3d:([0-9]+:)?fixme:' "$OUT/khnmc.log"), warn $(grep -a -c -E '^vkd3d:([0-9]+:)?warn:' "$OUT/khnmc.log"); MoltenVK errors $mvk_all (of them $mvk_ts the known timestamp-pool fallback at start-up, OTHER: $((mvk_all-mvk_ts)))"
  grep -a -e '^kh: gfx pipeline cache:' -e '^kh: gfx pipelines ' -e '^kh: gfx pipelines: ' -e '^kh: gfx MoltenVK shader statistics' -e '^kh: gfx frame times' -e '^kh: gfx vkd3d log' -e '^kh: gfx pipecache device selftest' "$OUT/khnmc.log" > "$OUT/pipecache-report.txt" 2>/dev/null
  say "pipeline creations that FAILED (pipeline-create-failed lines; each is a dropped draw or dispatch): $(grep -a -c 'pipeline-create-failed' "$OUT/khnmc.log")"
  for k in 'kh: gfx pipeline cache: loaded ' 'kh: gfx pipeline cache: saved ' 'kh: gfx pipeline cache: file ' 'kh: gfx pipelines ' 'kh: gfx MoltenVK shader statistics' 'kh: gfx frame times'; do
    l=$(grep -a -F "$k" "$OUT/pipecache-report.txt" | tail -1); [ -z "$l" ] || say "$(printf '%s' "$l" | cut -c1-600)"; done
  if [ "$PRESENT" = 1 ]; then
    grep -a -e '^kh: gfx present: ' -e '^kh: app: ' -e '^kh: ios: ' -e '^kh: screen: ' -e '^kh: vsync: ' -e '^kh: gfx gate: ' "$OUT/khnmc.log" > "$OUT/present-report.txt" 2>/dev/null
    say "present report: $(grep -c 'surface caps' "$OUT/present-report.txt") surface-caps line(s), swapchains made/rebuilt $(grep -c -e 'swapchain made' -e 'swapchain rebuilt' "$OUT/present-report.txt"), $(grep -a -m1 '^kh: gfx present: mode=' "$OUT/khnmc.log" | cut -c1-160)"
    grep -q 'surface caps' "$OUT/present-report.txt" || { say "PRESENT=1, but the log has no surface-caps line: the presenter never made its swapchain"; xrc=1; }; fi
  if [ "$HASHLOG" = 1 ]; then if cp_from hash.tsv; then say "hash.tsv: $(( $(wc -l < "$OUT/hash.tsv") - 1 )) presents"; else say "hash.tsv could not be fetched"; xrc=1; fi; fi
  if [ -n "$DUMP_EVERY" ]; then
    if cp_from frames/index.tsv; then
      tail -n +$((IX0+1)) "$OUT/frames/index.tsv" | grep -v '^present' > "$OUT/frames/index-this-run.tsv"
      got=0; miss=""
      for n in $FETCH; do pf=$(printf 'frame-%06d.ppm' "$n")
        if awk -F'\t' -v n="$n" -v f="$pf" '$1==n && $2==f {ok=1} END {exit !ok}' "$OUT/frames/index-this-run.tsv" && cp_from "frames/$pf" && [ -s "$OUT/frames/$pf" ]; then got=$((got+1)); else miss="$miss $n"; fi; done
      say "this run dumped $(wc -l < "$OUT/frames/index-this-run.tsv" | tr -d ' ') pictures (every $DUMP_EVERY presents); fetched $got of $(echo $FETCH | wc -w | tr -d ' ') into frames/${miss:+; NOT fetched (not dumped by this run, or the copy failed):$miss}"
      [ -z "$miss" ] || xrc=1
    else say "Documents/frames/index.tsv could not be fetched: no picture of this run is known"; xrc=1; fi
  fi
fi
cp_to "$OUT/play.conf" khnmc.conf || say "WARNING: the play configuration could not be put back: the icon would start this test again"
say "status after $((SECONDS-t0)) s: '${st:-<none>}'; log lines: $(wc -l < "$OUT/khnmc.log" 2>/dev/null | tr -d ' ')"
echo "$OUT"
case "$st" in "exit 0") exit $xrc;; "exit "*) exit 1;; *) exit 124;; esac
