"""Compare consecutive dump pairs from KHNMC_TEST_SAVESTATE_PICTURES."""
import argparse
import csv
from collections import defaultdict
from pathlib import Path

from PIL import Image


def check(frames, hashes, count, pairs):
    if count <= 0 or pairs <= 0:
        raise ValueError("count and pairs must be positive")
    by_present = defaultdict(list)
    with hashes.open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            by_present[int(row["present"])].append(row)
    passes = defaultdict(dict)
    with (frames / "index.tsv").open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            name = row["file"]
            if name.startswith("cut-"):
                branch = int(name.split("-")[1])
                n = int(row["present"])
                if n in passes[branch]:
                    raise ValueError(f"duplicate present {n} in pass {branch}")
                passes[branch][n] = row
    if sorted(passes) != list(range(1, 2 * pairs + 1)):
        raise ValueError(f"missing picture pass, saw {sorted(passes)}")
    for branch in range(1, 2 * pairs + 1, 2):
        before, after = passes[branch], passes[branch + 1]
        seq = sorted(before)
        if len(seq) != count or seq != list(range(seq[0], seq[0] + count)) or before.keys() != after.keys():
            raise ValueError(f"missing matching consecutive presents in pass {branch}")
        for n in seq:
            rows = by_present[n]
            if len(rows) != 2 or rows[0] != rows[1]:
                raise ValueError(f"present {n} BGRA/hash or presentation record differs")
            pictures = []
            for row in (before[n], after[n]):
                if int(row["distinct_rgb"]) < 2 or float(row["nonblack_pct"]) <= 0:
                    raise ValueError(f"present {n} lacks a visible picture")
                path = frames / row["file"]
                with Image.open(path if path.exists() else path.with_suffix(".png")) as im:
                    if im.mode != "RGB" or im.size != (int(row["width"]), int(row["height"])):
                        raise ValueError(f"present {n} format or size differs")
                    pictures.append(im.tobytes())
            if pictures[0] != pictures[1]:
                diff = sum(pictures[0][i:i+3] != pictures[1][i:i+3] for i in range(0,len(pictures[0]),3))
                raise ValueError(f"present {n} has {diff} different RGB pixels")
        print(f"passes {branch}/{branch+1}, presents {seq[0]}..{seq[-1]}: "
              f"{len(seq)} exact RGB pictures, {len(seq)*len(pictures[0])} bytes; all BGRA hashes equal")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("frames", type=Path)
    ap.add_argument("hashes", type=Path)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--pairs", type=int, required=True)
    args = ap.parse_args()
    check(args.frames, args.hashes, args.count, args.pairs)
