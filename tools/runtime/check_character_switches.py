"""Check the four null-port controller runs of input/route-character-quick.txt."""
import re
import sys
from pathlib import Path


def check(directory, row):
    log = (directory / "r01" / "run.out").read_text()
    assert "all 49 steps done" in log
    assert f"xkb-choice: row {row} level {2 + row % 2} XKB {int(row >= 2)}" in log
    start = int(row >= 2)
    expected = [(start - n) % 3 for n in range(1, 28)]
    choices = re.findall(r"(field|save)-character chose (\d)", log)
    assert choices == list(zip(["field"] * 21 + ["save"] * 6, map(str, expected)))
    commits = re.findall(r"look-commit frame=(\d+) mode=(\d) .*?body=(\w+) face=(\w+) name=(\w+) gauge=(\d)", log)
    assert [int(c[1]) for c in commits] == expected
    for column in (2, 3):
        assert len({c[column] for c in commits[:21]}) == 3, "body or HUD failed to switch"
    assert all(c[5] == "1" for c in commits)
    assert "player appearance: dropped" not in log
    assert log.count("appearance notice: game frame") == len(expected)
    print(f"Mode {row}: 21 field changes and 6 native changes completed, with 3 bodies and 3 faces")


if __name__ == "__main__":
    root = Path(sys.argv[1])
    for row in range(4):
        check(root / f"mode{row}", row)
