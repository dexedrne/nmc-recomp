"""Restore the published Zexion arena repair over an installed NMC overlay."""
import argparse
import json
import runpy
import shutil
import tempfile
from pathlib import Path


def build(stock, overlay, recipes, output):
    api = runpy.run_path(str(recipes / "tools/rmc-build.py"))
    require = api["require"]
    require(not output.exists() and not output.is_symlink(), "choose a fresh pack directory")
    require(all(root.resolve() not in output.resolve().parents for root in (stock, overlay, recipes)),
            "output must be outside all input directories")
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / "pack"
        shutil.copytree(overlay, stage)
        for recipe in ("R02", "R04"):
            specs = json.loads((recipes / f"patches/rmc/{recipe}.json").read_text())
            edits = [s for s in specs if s["path"] in ("map/mu13.map", "map/us/mu13.bar", "ard/us/mu13.ard") or
                     s["path"].startswith(("remastered/map/mu13.map/", "remastered/map/us/mu13.bar/"))]
            expected = 46 if recipe == "R02" else 1
            require(len({s["path"] for s in edits}) == len(edits) == expected, f"incomplete {recipe} mu13 recipe")
            for spec in edits:
                name = spec["path"]
                dest = stage / name
                if spec["op"] == "copy":
                    result = api["checked"](stock, spec["source"], spec["sha256"])
                    for root in (stock, overlay):
                        old = root / name
                        require(not old.exists() or old.read_bytes() == result, f"conflicting arena asset: {name}")
                else:
                    require(name == "ard/us/mu13.ard" and spec["op"] == "scene-props", f"unexpected edit: {name}")
                    base = api["checked"](stock, name, spec["sha256"])
                    patched = api["archive"](base, spec)
                    actual = dest.read_bytes() if dest.exists() else base
                    result = bytearray(actual)
                    def scene(data):
                        return api["selected"](api["bar"](data), spec["ordinal"],
                                               spec["kind"], spec["name"], spec["link"])
                    original = scene(base)
                    current = scene(actual)
                    expected = original.data(base)
                    desired = scene(patched).data(patched)
                    existing = current.data(actual)
                    require(len(expected) == len(desired) == len(existing), f"scene layout changed: {name}")
                    # Each reference is a 16-bit actor ID. Merge it as one value.
                    for i in range(0, len(expected), 2):
                        prior, new = expected[i:i+2], desired[i:i+2]
                        if prior != new:
                            at = current.offset + i
                            require(existing[i:i+2] in (prior, new), f"conflicting scene edit: {name}+{at:#x}")
                            result[at:at+2] = new
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(result)
        stage.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("stock", "overlay", "recipes", "output"):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    build(args.stock, args.overlay, args.recipes, args.output)
