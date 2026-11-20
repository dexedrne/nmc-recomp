"""Build the eight R20 ending PNG aliases into a fresh private overlay."""
import argparse
import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory


def build(base, output):
    owner = Path("remastered/menu/us/ending.2ld")
    recipe = Path(__file__).resolve().parents[1] / "patches/rmc/R20-ending.json"
    rows = json.loads(recipe.read_text())
    if len(rows) != 8 or output.exists():
        raise ValueError("expected eight entries and a fresh output directory")
    with TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / "overlay"
        (stage / owner).mkdir(parents=True)
        for index, row in enumerate(rows):
            source = owner / f"-{index}.png"
            target = owner / f"US_ending_2ld{index}.png"
            if (row["op"], row["old"], row["source"], row["path"]) != (
                "copy", None, str(source), str(target)
            ):
                raise ValueError(f"unexpected ending alias: {target}")
            data = (base / source).read_bytes()
            if hashlib.sha256(data).hexdigest() != row["sha256"]:
                raise ValueError(f"ending donor digest mismatch: {source}")
            (stage / target).write_bytes(data)
        stage.rename(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        build(args.base, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"build-f23: {error}\n")
