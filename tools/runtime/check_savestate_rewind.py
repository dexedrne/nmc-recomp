"""Check each replay against the first pass in a frames-clock null route."""
import re
import sys
import wave
from pathlib import Path


def check(run):
    lines = (run / "run.out").read_bytes().splitlines()
    marks = []
    for i, line in enumerate(lines):
        m = re.match(rb"kh: savestate-(captured|loaded) at=(\d+) resumed-frame=(\d+) sample=(\d+)", line)
        if m:
            marks.append((i, int(m[4]), m[1]))
    assert len(marks) >= 2 and marks[0][2] == b"captured"
    assert all(m[2] == b"loaded" for m in marks[1:])
    with wave.open(str(run / "root/field.wav")) as wav:
        width = wav.getsampwidth() * wav.getnchannels()
        pcm = wav.readframes(wav.getnframes())
    a, b = marks[:2]
    data = pcm[a[1]*width:b[1]*width]
    assert data and any(data)
    for index, m in enumerate(marks[1:], 1):
        end = marks[index+1][0] if index+1 < len(marks) else len(lines)
        counts = []
        for prefix in (b"kh: savestate-frame ", b"kh: gfx frame="):
            ref = [x for x in lines[a[0]+1:b[0]] if x.startswith(prefix)]
            replay = [x for x in lines[m[0]+1:end] if x.startswith(prefix)]
            assert ref and ref == replay[:len(ref)], (run, index, prefix)
            counts.append(len(ref))
        start = m[1]*width
        assert pcm[start:start+len(data)] == data, (run, index, "PCM")
        print(run, "replay", index, "game records", counts[0], "presents", counts[1], "PCM bytes", len(data))
    cuts = []
    for line in lines:
        if line.startswith(b"kh: savestate-game-dispatch "):
            cuts.append([])
        if line.startswith(b"kh: savestate-chunk "):
            cuts[-1].append(line)
    hashes = [c for c in cuts if c]
    if hashes:
        assert len(hashes) == len(marks) and all(c == hashes[0] for c in hashes)
        print(run, len(hashes), "identical committed-arena images,", len(hashes[0]), "chunks each")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        check(Path(arg))
