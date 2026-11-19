#!/usr/bin/env bash
# Run from the project root. One invocation per renderer; each gets a fresh output directory.
set -eu
renderer=${1:?expected null or vkd3dw}
out=${2:?expected new output directory under the worktree}
case "$renderer" in null|vkd3dw) ;; *) exit 2;; esac
[[ $out == run-S70/* && ! -e "$out" ]] || exit 2
mkdir -p "$out/tmp"
out=$(cd "$out" && pwd)
export KH_TMPDIR="$out/tmp" TMPDIR="$out/tmp" XDG_CACHE_HOME="$out/tmp" LP_NUM_THREADS=3
opts=()
for kv in KHNMC_SCREEN=1280x720 KHNMC_ASPECT=fill KHNMC_GFX_PRESENT=layer KHNMC_PRESENT=layer KHNMC_SAVESTATE=1 KHNMC_CUTSCENES=theater \
    KHNMC_PLAYER_LOOK=xion KHNMC_SCENE_ROXAS=1 KHNMC_XKB_MENU=1 KHNMC_AUTOSAVE=1 KHNMC_THEATER_UNLOCK=1 KHNMC_HUD_HIDE_COMBO=1 \
    KHNMC_TWEAK_DEBUG_DEBUG_ON=on KHNMC_TWEAK_RETRY_RETRY=on KHNMC_TWEAK_SAVEPOINT_SAVEPOINT_QOL=on KHNMC_CHEAT_INF_MP=1 \
    KHNMC_FOV_ZOOM=1.3 KHNMC_HUD_SCALE_CMD=1.25 KHNMC_HUD_SCALE_HP=1.5 KHNMC_EXP_MULT=2 KHNMC_DROP_MULT=2 KHNMC_CHEAT_INF_HP=1 KHNMC_TEST_SLOT_DIGEST=1 KHNMC_ROUTE_END=exit KHNMC_GFX="$renderer" KHNMC_AUDIO="pull-test:$out/device.wav"; do
    opts+=(-e "$kv")
done
unset KHNMC_CLOCK KHNMC_CLOCK_SYNC
/opt/build-tools/heavy.sh tools/route_batch.sh -n 1 -j 1 -s 900 -i input/route-phone-slots.txt -o "$out" "${opts[@]}" >"$out/route.log" 2>&1
python3 - "$out" "$renderer" <<'PY'
from pathlib import Path
import re, sys
out = Path(sys.argv[1])
log = (out / 'r01/run.out').read_text(errors='replace')
for i in range(1, 4):
    rows = re.findall(rf'^kh: slot-digest (save|load) \./slot{i}\.khss (.+)$', log, re.M)
    assert len(rows) == 2 and rows[0][0] == 'save' and rows[1] == ('load', rows[0][1]), (i, rows)
    branch = re.search(rf'^kh: slot-digest before \./slot{i}\.khss (.+)$', log, re.M)
    assert branch and branch[1].split('sha256=')[-1] != rows[0][1].split('sha256=')[-1], (i, branch)
    if i == 1:
        assert branch[1].split('pos=')[1].split()[0] != rows[0][1].split('pos=')[1].split()[0]
    if i == 3:
        assert branch[1].split('pos=')[0].split('world=')[1] != rows[0][1].split('pos=')[0].split('world=')[1]
    assert (out / f'r01/root/slot{i}.khss').stat().st_size > 0
if sys.argv[2] == 'vkd3dw':
    assert '(CPU) (wrapped)' in log and 'driver-mode=lavapipe' in log
    assert re.findall(r'^kh: savestate-gpu-capture (.+)$', log, re.M) == re.findall(r'^kh: savestate-gpu-restore (.+)$', log, re.M)
    assert log.count('kh: savestate-gpu-capture ') == 3
assert log.count('savestate-audio-cut attached=1 started=1') >= 6
assert 'all ' in log and 'DEBUG WARP' in log
print('All three numbered slots restored the captured frame, location, sample and GAME_SAVE digest.')
PY
