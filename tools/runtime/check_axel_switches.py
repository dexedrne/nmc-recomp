"""Check four-look field and save-point routes, independent title character and difficulty.
mode0/1 use the Roxas title, mode2/3 the Xion title, axel/newgame the Axel title.
The optional --stand-in flag remains accepted for earlier route wrappers."""
import re
import sys
from pathlib import Path

LOOKS = 4
NAMES = {0: "Roxas", 1: "Xion", 2: "Xion (hooded)", 3: "Axel"}


def read(directory):
    path = directory / "r01" / "run.out"
    if not path.is_file():
        raise SystemExit(f"check_axel_switches: missing {path}")
    return path.read_text(errors="replace")


def steps_done(log):
    report = re.findall(r"route report: (\d+) of (\d+) steps done", log)
    assert report and report[-1][0] == report[-1][1], f"route incomplete: {report[-1:] or 'no route report'}"
    assert "FATAL" not in log, "a named fatal in the run"


def switches(log, start, what):
    """21 Left presses in the field panel, then 6 native save-point changes: every one accepted, in order, and bound."""
    expected = [(start - n) % LOOKS for n in range(1, 28)]
    choices = re.findall(r"(field|save)-character chose (\d)", log)
    assert choices == list(zip(["field"] * 21 + ["save"] * 6, map(str, expected))), f"{what}: choices {choices}"
    commits = re.findall(r"look-commit frame=(\d+) mode=(\d) .*?body=(\w+) face=(\w+) name=(\w+) gauge=(\d)", log)
    assert [int(c[1]) for c in commits] == expected, f"{what}: commits {[c[1] for c in commits]}"
    for column, thing in ((2, "body"), (3, "HUD face")):
        assert len({c[column] for c in commits[:21]}) == LOOKS, f"{what}: {thing} did not switch over {LOOKS} looks"
    names = {int(c[1]): c[4] for c in commits}
    assert names[1] == names[2] and len({names[0], names[1], names[3]}) == 3, f"{what}: displayed names {names}"
    assert all(c[5] == "1" for c in commits), f"{what}: a HUD gauge was not ready at its commit"
    assert "player appearance: dropped" not in log, f"{what}: a request was dropped"
    notices = re.findall(r'appearance notice: game frame \d+: "([^"]*)"', log)
    assert notices == [NAMES[m] for m in expected], f"{what}: notices {notices}"
    return expected


def check_mode(root, row):
    log = read(root / f"mode{row}")
    steps_done(log)
    selected = row % 2
    start = int(row >= 2)
    title = "xion" if start else "roxas"
    assert f"newgame-choice: row {selected} level {2+selected} title {title} look {start}" in log
    switches(log, start, f"mode{row}")


def check_axel(root):
    log = read(root / "axel")
    steps_done(log)
    assert "newgame-choice: row 0 level 2 title axel look 3" in log
    switches(log, 3, "axel")


def check_save(root):
    new = read(root / "newgame")
    steps_done(new)
    assert "newgame-choice: row 0 level 2 title axel look 3" in new
    assert re.search(r"xkb-card-write: \S+ 69568 XKB 0", new)
    again = read(root / "reload")
    steps_done(again)
    assert "save-look: restored look 3" in again
    assert "`@expect game-level 2` met" in again
    assert re.findall(r"field-character chose (\d)", again) == ["0"]
    print("Critical Axel restored after a native save and fresh process")


def check_hooded(root):
    """T103's case kept in the loop: hooded -> regular Xion (and back) twelve times, then Roxas and back, every one committed."""
    d = root / "hooded"
    if not d.is_dir():
        print("hooded: not run")
        return
    log = read(d)
    steps_done(log)
    expected = [2, 1] * 6 + [0, 1, 2, 1]
    commits = re.findall(r"look-commit frame=(\d+) mode=(\d) .*?body=(\w+) face=(\w+) name=(\w+) gauge=(\d)", log)
    assert [int(c[1]) for c in commits] == expected, f"hooded: commits {[c[1] for c in commits]}"
    bodies = {int(c[1]): set() for c in commits}
    for c in commits:
        bodies[int(c[1])].add(c[2])
    assert not (bodies[1] & bodies[2]), "hooded: hooded and regular Xion committed the same body"
    assert all(c[5] == "1" for c in commits), "hooded: a HUD gauge was not ready at its commit"
    assert "player appearance: dropped" not in log, "hooded: a request was dropped"
    print(f"Hooded <-> regular Xion: {expected.count(1)} changes to regular Xion ({sum(1 for a, b in zip([1] + expected, expected) if (a, b) == (2, 1))} "
          f"from hooded), 16 commits, no drop, no fatal")


def check_rows(root):
    for name, rows in (("rows5", 5), ("rows4", 4)):
        d = root / name
        if not d.is_dir():
            print(f"{name}: not run")
            continue
        log = read(d)
        steps_done(log)
        assert "newgame-rows: Critical / Less Than Nothing" in log
        assert "`@expect newgame-choice 1` met" in log
        print(f"{name}: two difficulty rows")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--stand-in"]
    if len(args) != 1:
        raise SystemExit(__doc__)
    root = Path(args[0])
    for row in range(4):
        check_mode(root, row)
    check_axel(root)
    check_save(root)
    check_hooded(root)
    check_rows(root)
