"""Check the matched field and HB08 routes. Arguments are route_batch output directories."""
import argparse
import csv
import filecmp
import re
from pathlib import Path


def check(ok, message):
    if not ok:
        raise SystemExit(f"cheat-route check failed: {message}")


def run(directory):
    with (directory / "summary.tsv").open() as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    check(len(rows) == 1 and rows[0]["result"] == "ok", f"{directory}: incomplete run")
    return directory / rows[0]["run"]


def check_parity(before, after):
    files = [Path("files.log"), Path("phase.log")]
    for root in ["docs", "root"]:
        def manifest(directory):
            return sorted(p.relative_to(directory) for p in (directory / root).rglob("*") if p.is_file())
        original = manifest(before)
        check(original == manifest(after), f"{root}: different files")
        files.extend(original)
    for path in files:
        check(filecmp.cmp(before / path, after / path, shallow=False), f"byte mismatch: {path}")
    print(f"Field route: {len(files)} outputs byte-identical")


def battle(directory, live):
    log = (directory / "run.out").read_text(errors="replace")
    switches = re.findall(r"kh: settings frame (\d+) (\w+)=(\S+)", log)
    expected = {"KHNMC_CHEAT_INF_HP": "1", "KHNMC_CHEAT_INF_MP": "1", "KHNMC_EXP_MULT": "2", "KHNMC_DROP_MULT": "10"} if live else {}
    check({name: value for _, name, value in switches} == expected and all(n == "1250" for n, _, _ in switches), "live settings")
    hp = re.findall(r"cheat HP before=(\d+) delta=(-\d+) protected=(\d+)", log)
    check(hp and all(int(flag) == live for _, _, flag in hp), "player damage")
    check(f"test MP cost frame=1320 before=100 after={100 if live else 99}" in log, "player MP cost")
    awards = [tuple(map(int, p)) for p in re.findall(r"cheat EXP award=(\d+) scaled=(\d+)", log)]
    check(awards == [(12, 24 if live else 12)] * 4, "four known kill awards")
    totals = [int(x) for x in re.findall(r"cheat EXP save-total=(\d+)", log)]
    check(totals == [sum(v for _, v in awards[:i]) for i in range(1, 5)], "actual saved EXP")
    rolls = re.findall(r"cheat DROP item=(\d+) chance=([\d.]+) scaled=([\d.]+) rolled=([\d.]+) won=(\d)", log)
    check(len(rolls) == 8, "eight eligible item rolls")
    for _, chance, threshold, roll, won in rolls:
        check(float(threshold) == min(100., float(chance) * (10 if live else 1)), "drop threshold")
        check(int(won) == (float(roll) < float(threshold)), "drop decision")
    wins = sum(int(r[-1]) for r in rolls)
    print(f"{'Live' if live else 'Control'}: {len(awards)} kills, {totals[-1]} EXP, {wins} item(s) from {len(rolls)} rolls")
    return wins


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("baseline", "parity", "control", "live"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    check_parity(run(args.baseline), run(args.parity))
    control = battle(run(args.control), False)
    live = battle(run(args.live), True)
    check(live > control, "observed drop count did not increase on this route")
