"""Add the published 0.1.0 files the installed overlay never carried: both restaged arenas, three room repairs and the menu text."""
import argparse
import json
import runpy
import shutil
import tempfile
from pathlib import Path

# recipe -> (number of entries used, accepted destinations)
ARENAS = {"R01": (15, "ca26"), "R03": (44, "nm11")}
ROOMS = {"R04": ("ard/us/ca26.ard", "ard/us/nm11.ard"), "R27": ("ard/us/tr01.ard",)}
TEXT = ("items", "R08")
TEXT_PATH, TEXT_MIRROR = "msg/us/sys.bar", "msg/us/sys.text"


def build(stock, overlay, recipes, output):
    api = runpy.run_path(str(recipes / "tools/rmc-build.py"))
    require, checked, archive, messages = api["require"], api["checked"], api["archive"], api["messages"]
    require(not output.exists() and not output.is_symlink(), "choose a fresh pack directory")
    require(all(root.resolve() not in output.resolve().parents for root in (stock, overlay, recipes)),
            "output must be outside all input directories")
    held = {p.relative_to(overlay).as_posix().casefold() for p in overlay.rglob("*") if p.is_file()}
    added = {}

    def load(recipe):
        return json.loads((recipes / f"patches/rmc/{recipe}.json").read_text())

    def publish(name, data):
        # An earlier repair of the same file must be merged by hand, never replaced.
        require(name.casefold() not in held, f"the overlay already carries {name}: merge it explicitly")
        require(name not in added, f"duplicate destination: {name}")
        added[name] = bytes(data)

    for recipe, (expected, area) in ARENAS.items():
        specs = load(recipe)
        prefixes = (f"remastered/map/{area}.map/", f"remastered/map/us/{area}.bar/")
        require(len(specs) == len({s["path"] for s in specs}) == expected and
                all(s["op"] == "copy" and s["old"] is None and
                    (s["path"] in (f"map/{area}.map", f"map/us/{area}.bar") or s["path"].startswith(prefixes))
                    for s in specs), f"unexpected {recipe} arena recipe")
        for spec in specs:
            require(not (stock / spec["path"]).exists(), f"the game data already has {spec['path']}")
            publish(spec["path"], checked(stock, spec["source"], spec["sha256"]))

    for recipe, names in ROOMS.items():
        specs = [s for s in load(recipe) if s["path"] in names]
        require(sorted(s["path"] for s in specs) == sorted(names), f"incomplete {recipe} recipe")
        for spec in specs:
            publish(spec["path"], archive(checked(stock, spec["path"], spec["sha256"]), spec))

    edits = [s for recipe in TEXT for s in load(recipe)]
    require(len(edits) == 3 and all((s["op"], s["path"], s["mirror"]) == ("wording", TEXT_PATH, TEXT_MIRROR) and
                                    (s["ordinal"], s["kind"], s["name"], s["link"]) == (0, 2, "sys", 0) and
                                    s["sha256"] == edits[0]["sha256"] for s in edits), "unexpected menu text recipe")
    checked(stock, TEXT_MIRROR, edits[0]["sha256"])
    text = messages(checked(stock, TEXT_PATH, edits[0]["sha256"]), edits)
    publish(TEXT_PATH, text)
    publish(TEXT_MIRROR, text)

    require(len(added) == 64, f"expected 64 added files, built {len(added)}")
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / "pack"
        shutil.copytree(overlay, stage)
        for name, data in sorted(added.items()):
            dest = stage / name
            require(not dest.exists(), f"case collision: {name}")
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        stage.rename(output)
    print(f"{len(held)} inherited + {len(added)} added files in {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("stock", "overlay", "recipes", "output"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    build(args.stock, args.overlay, args.recipes, args.output)
