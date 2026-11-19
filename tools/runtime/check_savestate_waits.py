"""Check field/battle and optional title wait stacks. Arguments: matching Linux binary, route run.out."""
from collections import Counter
from functools import lru_cache
from pathlib import Path
import re
import subprocess
import sys


def check(binary, path):
    @lru_cache(None)
    def symbol(frame):
        name, offset = frame.rsplit("+", 1)
        if name == "image":
            result = subprocess.check_output(["addr2line", "-f", "-e", binary, offset], text=True)
            name = result.splitlines()[0]
        return name.split(".")[0]

    # A positive match distinguishes a host timer from a guest Sleep on the same CV.
    expect = {
        "timer": (3, "clock-client", "timer_proc_virtual"),
        "mixer": (1, "clock-deadline", "mix_thread_virtual"),
        "sleeper": (2, "clock-client", "kh_vclock_vsleep"),
        "handle": (11, "handle-wait", "kh_wait"),
        "handoff": (1, "wake-handoff", "vc_after_wake"),
        "stream": (1, "stream-idle", "kh_cut_stream_loop"),
        "shell": (1, "shell-loop", "manage_run"),
    }
    stacks, sites, owners = {}, {}, {}
    frame = None
    verified = set()
    requested = {900, 1160}
    for line in Path(path).read_text().splitlines():
        if m := re.fullmatch(r"kh: savestate-game-dispatch frame=(\d+) rebuilt=(\d)", line):
            if int(m[1]) == 407:
                requested.add(407)
            frame = int(m[1]) if m[2] == "1" else None
            stacks.clear()
            sites.clear()
            owners.clear()
        if frame not in requested:
            continue
        if m := re.fullmatch(r"kh: savestate-callers tid=(\d+) count=(\d+) (.+)", line):
            frames = m[3].split()
            if len(frames) != int(m[2]) or len(frames) >= 64:
                raise ValueError(f"frame {frame}: truncated backtrace for tid {m[1]}")
            # kh_call is the outer host trampoline. Resolve even non-exported
            # code before it. A nested host-to-guest callback is a second kh_call.
            stops = [i for i, f in enumerate(frames) if f.startswith(("kh_call+", "kh_call5+"))]
            if len(stops) != 1:
                raise ValueError(f"frame {frame}: unknown call boundary for tid {m[1]}")
            outer = frames[stops[0] + 1:]
            if not outer or symbol(outer[0]) not in ("thread_main", "guest_main"):
                raise ValueError(f"frame {frame}: kh_call outside the host thread entry for tid {m[1]}")
            stack = [symbol(f) for f in frames[:stops[0]]]
            if any(n == "??" or re.search(r"(?:^|_)f_[0-9a-f]+$|^kh_resume_|^rt_imp_", n) for n in stack):
                raise ValueError(f"frame {frame}: unresolved or lifted caller for tid {m[1]}: {stack}")
            stacks[m[1]] = stack
        if m := re.fullmatch(r"kh: savestate-park tid=(\d+) start=([0-9a-f]+) at=(\S+)", line):
            sites[m[1]] = (m[2], m[3])
        if m := re.fullmatch(r"kh: savestate-wait tid=(\d+) owner=(\S+)", line):
            tid, owner = m.groups()
            if tid in owners or owner not in expect:
                raise ValueError(f"frame {frame}: duplicate or unknown wait {tid} {owner}")
            _, site, required = expect[owner]
            stack = stacks.get(tid, [])
            entry, parked = sites.get(tid, ("", ""))
            if owner == "sleeper":
                body = "i_kernel32__SleepEx" if entry == "1400fab10" else "i_kernel32__Sleep"
                if body not in stack:
                    raise ValueError(f"frame {frame}: missing {body} for tid {tid}")
            if parked != site or required not in stack:
                raise ValueError(f"frame {frame}: {owner} missing its {site} host body: {stack}")
            if owner not in ("timer", "mixer", "stream") and "kh_cut_worker_loop" not in stack:
                raise ValueError(f"frame {frame}: {owner} outside worker dispatcher")
            owners[tid] = owner
        if m := re.fullmatch(r"kh: savestate-rendezvous frame=(\d+) epoch=\d+ workers=(\d+) missing=0 gate=1 .+", line):
            if int(m[1]) != frame or int(m[2]) != len(owners) or set(owners) != set(sites):
                raise ValueError(f"frame {frame}: incomplete registry")
            counts = {k: v[0] for k, v in expect.items()}
            if frame == 407:
                counts.update(handle=4, sleeper=3)
            if Counter(owners.values()) != counts:
                raise ValueError(f"frame {frame}: wait census differs: {owners}")
            verified.add(frame)
    if verified != requested:
        raise ValueError(f"missing field or battle evidence: verified {sorted(verified)}")
    print(f"Verified {sorted(verified)}: every registered worker has a resolved host wait and no lifted callers.")


if __name__ == "__main__":
    check(*sys.argv[1:])
