#!/usr/bin/env python3
"""pull_compare.py : the simulated device of KHNMC_AUDIO=pull-test:<file> against the WAV path of the same run (TASK28).

  tools/pull_compare.py FILE.wav          FILE.wav = what the simulated device pulled (underrun gaps written as zeros),
                                          FILE.wav.gaps = the gaps (device frame, zero frames), FILE.wav.mix.wav = the mixer's output
  tools/pull_compare.py DEVICE.wav MIX.wav [GAPS]

Removes the gap frames from the device stream (after checking that they ARE zero) and compares what is left with the mixer's output
sample for sample.  Prints the verdict, the frame counts, the gap count and sizes.  Exit 0 = identical, 1 = different, 2 = usage.
16-bit PCM as the runtime writes it.  Python 3.6+, standard library.
"""
import array
import struct
import sys


def load(path):
    b = open(path, 'rb').read()
    if len(b) < 44 or b[:4] != b'RIFF' or b[8:12] != b'WAVE':
        sys.stderr.write('pull_compare: %s is not a RIFF/WAVE file\n' % path); sys.exit(2)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack('<I', b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data': data = body; break
        pos += 8 + size + (size & 1)
    if fmt is None or data is None or fmt[0] != 1 or fmt[5] != 16:
        sys.stderr.write('pull_compare: %s: not 16-bit PCM\n' % path); sys.exit(2)
    a = array.array('h'); a.frombytes(data[:len(data) // 2 * 2])
    return a, fmt[1]


def main():
    if len(sys.argv) == 2:
        dev, mix, gaps = sys.argv[1], sys.argv[1] + '.mix.wav', sys.argv[1] + '.gaps'
    elif len(sys.argv) in (3, 4):
        dev, mix = sys.argv[1], sys.argv[2]; gaps = sys.argv[3] if len(sys.argv) == 4 else dev + '.gaps'
    else:
        sys.stderr.write(__doc__); return 2
    d, ch = load(dev); m, ch2 = load(mix)
    if ch != ch2:
        print('DIFFERENT: channel counts %d / %d' % (ch, ch2)); return 1
    gl = []
    for line in open(gaps):
        if line.startswith('#') or not line.strip(): continue
        a, n = line.split(); gl.append((int(a), int(n)))
    keep = array.array('h'); pos = 0; nonzero_gap = 0; zero_frames = 0
    for a, n in gl:
        keep.extend(d[pos * ch:a * ch])
        seg = d[a * ch:(a + n) * ch]
        nonzero_gap += sum(1 for x in seg if x)
        zero_frames += n; pos = a + n
    keep.extend(d[pos * ch:])
    fd, fm = len(d) // ch, len(m) // ch
    print('device: %d frames (%.3f s) with %d underrun gaps = %d zero frames (largest %d); mixer (WAV path): %d frames (%.3f s)'
          % (fd, fd / 48000.0, len(gl), zero_frames, max([n for _, n in gl] or [0]), fm, fm / 48000.0))
    if nonzero_gap:
        print('DIFFERENT: %d samples inside the listed gaps are not zero' % nonzero_gap); return 1
    if keep == m:
        print('IDENTICAL: the device stream without its underrun gaps is the WAV path sample for sample (%d frames)' % fm); return 0
    n = min(len(keep), len(m)); first = next((i for i in range(n) if keep[i] != m[i]), n)
    print('DIFFERENT: %d / %d frames after removing the gaps; first difference at mixer frame %d' % (len(keep) // ch, fm, first // ch))
    return 1


if __name__ == '__main__':
    sys.exit(main())
