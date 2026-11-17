#!/usr/bin/env python3
"""wav_compare.py : is the WAV of one run (KHNMC_AUDIO=wav:<file>) the same as another's, and if not, where and by how much.

  tools/wav_compare.py sum FILE            format, length, sha256 of the file and of the sample data alone, peak, and one sha256
                                           per second of sample data condensed to "seconds whose hash is listed" (so two machines
                                           can be compared without copying the audio: --blocks prints the list)
  tools/wav_compare.py diff A B            identical or not; if not: the first sample frame that differs (and its time), how many
                                           samples differ, the largest difference, the RMS of the difference against the RMS of the
                                           signal, how many whole seconds are identical, and the lengths
16-bit PCM as the runtime writes it (runtime/audio_mix.c).  Python 3.6+, standard library (no numpy: the Mac mini has none).
Exit status of diff: 0 identical sample data, 1 different, 2 usage / not a WAV.
"""
import array
import hashlib
import math
import struct
import sys


def load(path):
    b = open(path, 'rb').read()
    if len(b) < 44 or b[:4] != b'RIFF' or b[8:12] != b'WAVE':
        sys.stderr.write('wav_compare: %s is not a RIFF/WAVE file\n' % path); sys.exit(2)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack('<I', b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data': data = body; break
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.stderr.write('wav_compare: %s has no fmt or data chunk\n' % path); sys.exit(2)
    tag, ch, rate, _, align, bits = fmt
    if tag != 1 or bits != 16:
        sys.stderr.write('wav_compare: %s: format tag %d with %d bits; only 16-bit PCM is handled\n' % (path, tag, bits)); sys.exit(2)
    return {'path': path, 'file': b, 'data': data, 'ch': ch, 'rate': rate, 'align': align, 'frames': len(data) // align}


def samples(w):
    a = array.array('h'); a.frombytes(w['data'][:len(w['data']) // 2 * 2])
    if sys.byteorder != 'little': a.byteswap()
    return a


def blocks(w):
    n = w['rate'] * w['align']
    return [hashlib.sha256(w['data'][i:i + n]).hexdigest()[:12] for i in range(0, len(w['data']), n)]


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('sum', 'diff'):
        sys.stderr.write(__doc__); return 2
    if sys.argv[1] == 'sum':
        w = load(sys.argv[2]); s = samples(w)
        peak = max(max(s), -min(s)) if len(s) else 0
        nz = len(s) - s.count(0)
        bl = blocks(w)
        print('wav: %s: %d bytes, %d channels, %d Hz, 16-bit PCM, %d sample frames (%.3f s)' % (w['path'], len(w['file']), w['ch'], w['rate'], w['frames'], w['frames'] / float(w['rate'])))
        print('wav: sha256 file %s' % hashlib.sha256(w['file']).hexdigest())
        print('wav: sha256 data %s' % hashlib.sha256(w['data']).hexdigest())
        print('wav: peak %d (%.2f dBFS), %d of %d samples are not zero (%.1f %%)' % (peak, 20 * math.log10(peak / 32768.0) if peak else -999.0, nz, len(s), 100.0 * nz / max(1, len(s))))
        print('wav: per-second data hashes: %d blocks, sha256 of the list %s' % (len(bl), hashlib.sha256(' '.join(bl).encode()).hexdigest()[:16]))
        if '--blocks' in sys.argv[3:]:
            for i, h in enumerate(bl): print('wav: block %4d %s' % (i, h))
        return 0
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__); return 2
    a, b = load(sys.argv[2]), load(sys.argv[3])
    print('A: %s: %d ch, %d Hz, %d frames (%.3f s), data sha256 %s' % (a['path'], a['ch'], a['rate'], a['frames'], a['frames'] / float(a['rate']), hashlib.sha256(a['data']).hexdigest()[:16]))
    print('B: %s: %d ch, %d Hz, %d frames (%.3f s), data sha256 %s' % (b['path'], b['ch'], b['rate'], b['frames'], b['frames'] / float(b['rate']), hashlib.sha256(b['data']).hexdigest()[:16]))
    if (a['ch'], a['rate']) != (b['ch'], b['rate']):
        print('DIFFERENT: the formats differ'); return 1
    if a['data'] == b['data']:
        print('IDENTICAL: the sample data of both files is the same, byte for byte (%d bytes)' % len(a['data'])); return 0
    sa, sb = samples(a), samples(b); n = min(len(sa), len(sb)); ch = a['ch']
    first, ndiff, maxd, sq, sqa = -1, 0, 0, 0, 0
    for i in range(n):
        x = sa[i]; d = x - sb[i]; sqa += x * x
        if d:
            if first < 0: first = i
            ndiff += 1; sq += d * d
            if d < 0: d = -d
            if d > maxd: maxd = d
    ba, bb = blocks(a), blocks(b); same_sec = sum(1 for x, y in zip(ba, bb) if x == y)
    lead = 0
    for x, y in zip(ba, bb):
        if x != y: break
        lead += 1
    print('DIFFERENT sample data:')
    if len(sa) != len(sb): print('  lengths: A %d frames, B %d frames (%+d frames = %+.3f s); compared over the common %d frames' % (a['frames'], b['frames'], b['frames'] - a['frames'], (b['frames'] - a['frames']) / float(a['rate']), n // ch))
    if first < 0:
        print('  the common part is identical; only the length differs'); return 1
    rms_d = math.sqrt(sq / float(n)); rms_a = math.sqrt(sqa / float(n))
    print('  first difference: sample frame %d (%.3f s), channel %d: A %d, B %d' % (first // ch, first // ch / float(a['rate']), first % ch, sa[first], sb[first]))
    print('  %d of %d samples differ (%.2f %%); largest difference %d of 32768 (%.2f dBFS)' % (ndiff, n, 100.0 * ndiff / n, maxd, 20 * math.log10(maxd / 32768.0)))
    print('  RMS of the difference %.1f (%.2f dBFS); RMS of A %.1f (%.2f dBFS); difference relative to the signal %.2f dB'
          % (rms_d, 20 * math.log10(max(rms_d, 1e-9) / 32768.0), rms_a, 20 * math.log10(max(rms_a, 1e-9) / 32768.0), 20 * math.log10(max(rms_d, 1e-9) / max(rms_a, 1e-9))))
    print('  whole seconds with identical data: %d of %d; identical from the start for %d s' % (same_sec, min(len(ba), len(bb)), lead))
    # Is B the same sound at another time?  Take 0.1 s pieces of A after the first difference and look for the same bytes in B
    # within 2 s either way.  A piece that is found means: from there on B carries bit-identical samples, moved in time (a run
    # whose game timeline slipped by whole frames), not different arithmetic.  Silent pieces are not counted.
    al, rate = a['align'], a['rate']; piece = rate // 10 * al; f0 = first // ch
    tried = found = 0; lags = {}
    for k in range(0, 600):
        start = (f0 + k * rate // 5) * al
        if start + piece > len(a['data']): break
        chunk = a['data'][start:start + piece]
        if chunk.count(b'\0') > piece * 9 // 10: continue
        tried += 1
        lo = max(0, start - 2 * rate * al); hi = min(len(b['data']), start + 2 * rate * al + piece)
        pos = b['data'].find(chunk, lo, hi)
        while pos >= 0 and (pos - lo) % al: pos = b['data'].find(chunk, pos + 1, hi)
        if pos >= 0:
            found += 1; lag = (pos - start) // al; lags[lag] = lags.get(lag, 0) + 1
    if tried:
        top = sorted(lags.items(), key=lambda kv: -kv[1])[:6]
        print('  after the first difference: %d of %d non-silent 0.1 s pieces of A occur bit-identical in B within 2 s%s'
              % (found, tried, ('; time shifts in sample frames (pieces): ' + ', '.join('%+d (%d)' % kv for kv in top)) if found else ''))
    return 1


if __name__ == '__main__':
    sys.exit(main())
