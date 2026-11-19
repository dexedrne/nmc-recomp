#!/bin/bash
# Run from the project root. The destination must be a fresh on-disk directory.
set -eu
world=${1:?world required}
out=${2:?output directory required}
door=0
target=""
default_difficulty=1
case "$world" in
 al) place="4 0 0";; bb) place="0 0 1"; target="-400 -1000";; dc) place="2 0 0";;
 di) place="1 0 0";; eh) default_difficulty=0; place="7 0 1"; target="-400 50";; lk) place="10 0 0";;
 lm) place="1 0 0";; mu) place="7 0 0";; nm) place="3 0 1"; target="1000 -800";;
 po) place="7 0 0";; tr) place="6 0 1"; door=1; target="5 -726";; tt) default_difficulty=0; place="19 0 7"; target="400 400";; wi) default_difficulty=0; place="3 0 1"; target="100 -200";;
 hb) place="2 0 11"; target="-1138 -681";;
 ca) place="1 5 1"; door=50; target="1180 -695";;
 he) place="14 0 0";;
 *) echo "xion-world-route: unknown or non-field world $world" >&2; exit 2;;
esac
root=$PWD
mkdir "$out"
out=$(cd "$out" && pwd)
mkdir -p "$out/tmp" "$out/reload/root"
export TMPDIR="$out/tmp" KH_TMPDIR="$out/tmp"
read -r area map btl <<<"$place"
warp="@warp $world $area door $door map $map btl $btl evt 0 settle 30 timeout 5000"
# Fight locally when this tuple has enemies. Empty fields use K54's HB08 encounter.
awk -v world="$world" -v warp="$warp" -v target="${target:-}" '
world == "eh" && /^@warp tt 2 / && !intro++ {
    # The EH intro uses sheet 12 until the second encounter sets flag 0x4801.
    while ((getline line < "input/route-story.txt") > 0) {
        if (line == "@expect game-level difficulty") copying=1
        if (copying) print line
        if (line == "@expect btl-program 0x61") break
    }
    close("input/route-story.txt")
    print "@expect progress-flag 0x4801"
}
/^@warp hb 8 / {
    print warp
    if (world == "nm") print "@goto -700 200 radius 90 until player-char 4 stall 400 timeout 1500"
    if (target == "") {
        print "@wait player-char 4 timeout 300"
        print "@wait frames 60"
        print "@expect player-char 4"
    } else next
}
world == "nm" && /^@fight until enemies-at-most/ {
    print "@goto object M_EX200_NM radius 10 until enemies-at-most 0 stall 300 timeout 600"
    next
}
world == "eh" && /^@fight until save-exp-at-least/ {
    print "@fight until frames 2600 every 8 hold 3 timeout 3000"
    next
}
world == "eh" && (/^@fight until enemies-at-most/ || /^@wait frames 120$/) { next }
world == "eh" && /^@expect save-exp-at-least/ { next }
/^@goto 762 489 / && target != "" {sub("762 489",target)}
{print}
' input/route-xion-fight.txt >"$out/field.txt"
gate=/opt/build-tools/heavy.sh
export KHNMC_CLOCK=frames KHNMC_CLOCK_PACE=free KHNMC_XION_SWAP=1 KHNMC_ROUTE_END=exit
"$gate" tools/route_batch.sh -n 1 -j 1 -s 300 -g 25000 -d "${3:-$default_difficulty}" -i "$out/field.txt" -o "$out/field" >"$out/field.log" 2>&1
log="$out/field/r01/run.out"
grep -q 'xion-swap: Xion dealt' "$log" || { echo "xion-world-route: no Xion hit" >&2; exit 1; }
grep -q 'xion-swap: collected native prize' "$log" || { echo "xion-world-route: no native pickup" >&2; exit 1; }
room=$(printf '%s%02d' "$world" "$area")
awk -v room="$room" '/warp: #[0-9]+ result/ { here=($5==room) } here && /Roxas \(area departure\)/ { ok=1 } END { exit !ok }' "$log" || { echo "xion-world-route: no area handback" >&2; exit 1; }
cd "$out/reload/root"
export KH_ROOT="$PWD" KH_DOCS="$out/field/r01/docs" KH_EXE="$root/pc/kh2fm.exe" KHNMC_DATA="$root/data-full" KHNMC_PKGDIR="$root/pkgdir"
# A pickup during a fight can precede the first EXP award. Verify the actual saved value.
saved=$(sed -n 's/.*xion-swap: Roxas (save point).*save EXP \([0-9]*\) munny \([0-9]*\)$/\1 \2/p' "$log" | tail -1)
[[ "$saved" =~ ^[0-9]+\ [0-9]+$ ]] || { echo "xion-world-route: missing save handback" >&2; exit 1; }
sed "s/^@expect save-exp-at-least .*/@expect save-exp-at-least ${saved%% *}/" "$root/input/route-xion-reload.txt" >"$out/reload.txt"
export KHNMC_PHASELOG="$out/reload/phase.log" KHNMC_FILELOG="$out/reload/files.log" KH_RUN_GAME_FRAMES=7500 KH_RUN_SECONDS=240 KHNMC_INPUT_SCRIPT="$out/reload.txt"
"$gate" timeout 330 "$root/khnmc" >"$out/reload/run.out" 2>&1
grep -q 'route report: 7 of 7 steps done' "$out/reload/run.out" || { echo "xion-world-route: reload incomplete" >&2; exit 1; }
grep -Fxq "kh: warn: xion-swap: loaded Roxas, save EXP ${saved% *} munny ${saved#* }" "$out/reload/run.out" || { echo "xion-world-route: saved progress changed on load" >&2; exit 1; }
