"""Verify that the field and battle cuts parked the stream worker outside the lift."""
import re
import sys
from pathlib import Path


def check(path):
    frame = None
    callers = {}
    checked = []
    for line in Path(path).read_text().splitlines():
        if match := re.fullmatch(r"kh: savestate-game-dispatch frame=(\d+) rebuilt=1", line):
            frame = int(match[1])
            callers.clear()
        if match := re.fullmatch(r"kh: savestate-callers tid=(\d+) count=\d+ (.+)", line):
            callers[match[1]] = match[2]
        if match := re.fullmatch(r"kh: savestate-park tid=(\d+) start=1400fddd0 at=stream-idle", line):
            chain = callers.get(match[1], "")
            if "kh_cut_stream_loop+" not in chain or re.search(r"\b(?:f_[0-9a-f]+|rt_imp_\w+)\+", chain):
                raise ValueError(f"frame {frame}: invalid stream worker backtrace")
            checked.append(frame)
    if [frame for frame in checked if frame in (900, 1160)] != [900, 1160]:
        raise ValueError(f"expected a stackless worker at both scene cuts, found {checked}")
    print("stream worker parked without lifted callers at field 900 and battle 1160")


if __name__ == "__main__":
    check(sys.argv[1])
