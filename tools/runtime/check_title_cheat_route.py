"""Verify title-controller edits and their effects on the matched HB08 wave."""
import argparse
import csv
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("control", type=Path)
parser.add_argument("live", type=Path)
args = parser.parse_args()
options = {"KHNMC_CHEAT_INF_HP":"1", "KHNMC_CHEAT_INF_MP":"1",
           "KHNMC_EXP_MULT":"2", "KHNMC_DROP_MULT":"10"}
for directory, enabled in ((args.control, False), (args.live, True)):
    root = directory
    with (root / "summary.tsv").open() as summary:
        run, = csv.DictReader(summary, delimiter="\t")
    assert run["result"] == "ok", directory
    root = root / run["run"]
    log = (root / "run.out").read_text()
    prefix = "kh: RMC" if "kh: RMC KHNMC_" in log else "kh: title cheats"
    edits = re.findall(prefix + r" (\w+)=(\d+)", log)
    assert dict(edits) == (options if enabled else {}), edits
    if enabled:
        assert log.index("kh: title GAME SETTINGS entered") < log.index(prefix + " KHNMC_")
        conf = (root / "docs/khnmc.conf").read_text().splitlines()
        assert all(f"env {k}={v}" in conf for k,v in options.items())
        assert log.index(prefix + " KHNMC_") < log.index("kh: phase: first FIELD_COMMAND")
    hits = re.findall(r"cheat HP before=(\d+) delta=(-\d+) protected=(\d)", log)
    assert hits and all(int(flag) == enabled for _, _, flag in hits), hits
    if enabled:
        assert len(set(h for h, _, _ in hits)) == 1, hits
        assert "`@expect not hp-below-max` met" in log
        assert "`@expect not mp-below-max` met" in log
    costs = re.findall(r"test MP cost frame=\d+ before=(\d+) after=(\d+)",log)
    assert len(costs)==1 and int(costs[0][0])-int(costs[0][1])==int(not enabled), costs
    kills = re.findall(r"cheat EXP award=(\d+) scaled=(\d+)", log)
    award = 24 if enabled else 12
    assert kills == [("12", str(award))]*4, kills
    totals = list(map(int,re.findall(r"cheat EXP save-total=(\d+)",log)))
    assert totals == [i*award for i in range(1,5)], totals
    rolls = re.findall(r"cheat DROP item=\d+ chance=([\d.]+) scaled=([\d.]+) rolled=([\d.]+) won=(\d)",log)
    assert len(rolls)==8, rolls
    for chance,threshold,roll,win in rolls:
        assert float(threshold)==min(100, float(chance)*(10 if enabled else 1))
        assert int(win)==(float(roll)<float(threshold))
    print(directory, "HP before hits", [h for h,_,_ in hits], "MP", costs[0],
          "EXP", totals, "items", sum(int(r[-1]) for r in rolls), "/", len(rolls))
