"""Check the T87 null routes and their cold-start option precedence."""
import re
import sys
from pathlib import Path


def read_run(directory):
    return (directory / "r01" / "run.out").read_text()


def config(directory):
    lines = (directory / "r01" / "docs" / "khnmc.conf").read_text().splitlines()
    settings = dict(line[4:].split("=", 1) for line in lines if line.startswith("env "))
    assert len(lines) == len(settings), "duplicate or unexpected settings directives"
    return settings


def values(log, prefix):
    return dict(re.findall(r"^kh: RMC " + prefix + r"(KHNMC_\w+)=(.+)$", log, re.M))


def complete(log):
    match = re.search(r"route report: (\d+) of (\d+) steps done;", log)
    assert match and match[1] == match[2], "route incomplete"


def verify(primary, advanced, cold):
    game, extra, reload = map(read_run, (primary, advanced, cold))
    for log in (game, extra, reload):
        complete(log)
    live = config(primary)
    assert len(live) == 24 and "KHNMC_PLAYER_LOOK" not in live
    assert values(game, "") == live
    assert "field-character chose" not in game, "opening forced form accepted an appearance change"
    assert game.count("save-character chose 1") == 1 and "look-commit " in game
    assert game.index("save-character chose 1") > game.index("Change Character")
    assert "menu 3 " in game
    for page in ("Character", "Cheats", "Controller", "Input test", "Display", "Gameplay", "About"):
        assert f"RMC section {page} frame=" in game, page
    saved = config(advanced)
    assert len(saved) == 74 and len(values(extra, "")) == 83
    assert values(extra, "").items() >= saved.items()
    assert saved["KHNMC_TWEAK_LEVELCAP_SET_LEVEL_CAP"] == "98"
    assert "title GAME SETTINGS entered" in extra and "RMC section Advanced" in extra
    restored = values(reload, "restored ")
    assert restored == (live | saved), "saved settings lost to install defaults"
    edits = values(reload, "")
    assert edits["KHNMC_CHEAT_INF_HP"] == "0" and edits["KHNMC_TITLE_CREDIT"] == "show"
    assert edits["KHNMC_HUD_SCALE_HP"] == "1.6" and edits["KHNMC_TUTORIAL_FIGHT"] == "real"
    print(f"{len(restored)} persisted options restored; all 9 action parameters selected; all routes complete")


if __name__ == "__main__":
    verify(*(Path(arg) for arg in sys.argv[1:]))
