#!/usr/bin/env python3
"""l21b_compare.py : how far two frame dumps of the same game picture are apart (task 21b: MoltenVK / Metal against lavapipe).

  tools/l21b_compare.py pair A.ppm B.ppm [--diff OUT.png] [--gain 8] [--label TEXT]
      per-pixel numbers for two pictures of the same size:
        mad            mean absolute difference over all pixels and the three channels (0..255)
        >T             share of pixels whose LARGEST channel difference is above T, for T = 0, 2, 8, 16, 32, 64
        max            the largest channel difference
        edge / flat    the pixels above 8 split by whether picture A has an edge there (a channel step of 24 or more to a
                       neighbour within one pixel): a rasterizer that places triangle edges or filters texels differently
                       differs on edges; a difference on flat areas is a different colour (blending, precision, a missing draw)
        grid           mean absolute difference per tile of an 8 x 4 grid (row by row), to see WHERE the pictures differ
      --diff writes |A-B| times --gain as a PNG (needs Pillow).
  tools/l21b_compare.py align A.ppm DIR [--top 5]
      A against every frame-*.ppm in DIR: the mad of each, best first (to find the same game moment in a run whose timeline
      is shifted; the dumps of DIR should be consecutive presents).
  tools/l21b_compare.py stats A.ppm
      mean colour, share of non-black pixels and number of distinct colours of one dump (what index.tsv holds).
Reads binary PPM (P6, maxval 255) as written by KHNMC_GFX_DUMP.  Python 3 with numpy.
"""
import sys
import os
import glob
import numpy as np


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'P6':
        raise SystemExit('%s: not a binary PPM (P6)' % path)
    # header: P6 <ws> width <ws> height <ws> maxval <single ws> data ; no comments in our dumps, but skip them if present
    pos, vals = 2, []
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while data[pos:pos + 1] != b'\n':
                pos += 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        vals.append(int(data[pos:end])); pos = end
    pos += 1
    w, h, mx = vals
    if mx != 255:
        raise SystemExit('%s: maxval %d, expected 255' % (path, mx))
    a = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos)
    if a.size != w * h * 3:
        raise SystemExit('%s: short file (%d of %d bytes of pixels)' % (path, a.size, w * h * 3))
    return a.reshape(h, w, 3)


def edges(a, step=24):
    """True where a pixel differs from its right or lower neighbour by `step` or more in some channel (and the neighbour too)."""
    s = a.astype(np.int16)
    e = np.zeros(a.shape[:2], dtype=bool)
    dx = np.abs(s[:, 1:] - s[:, :-1]).max(axis=2) >= step
    dy = np.abs(s[1:, :] - s[:-1, :]).max(axis=2) >= step
    e[:, 1:] |= dx; e[:, :-1] |= dx; e[1:, :] |= dy; e[:-1, :] |= dy
    return e


def pair(pa, pb, diff_out=None, gain=8, label=''):
    a, b = read_ppm(pa), read_ppm(pb)
    if a.shape != b.shape:
        raise SystemExit('sizes differ: %s is %dx%d, %s is %dx%d' % (pa, a.shape[1], a.shape[0], pb, b.shape[1], b.shape[0]))
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    dm = d.max(axis=2)
    n = dm.size
    out = []
    out.append('%s%s  vs  %s   (%dx%d)' % ((label + ': ') if label else '', pa, pb, a.shape[1], a.shape[0]))
    out.append('  mad %.4f   (per channel R %.4f G %.4f B %.4f)   max %d   identical pixels %.3f %%'
               % (d.mean(), d[..., 0].mean(), d[..., 1].mean(), d[..., 2].mean(), int(dm.max()), 100.0 * (dm == 0).sum() / n))
    out.append('  share of pixels above T:  ' + '   '.join('>%d: %.3f %%' % (t, 100.0 * (dm > t).sum() / n) for t in (0, 2, 8, 16, 32, 64)))
    e = edges(a); big = dm > 8
    ne, nf = int(e.sum()), int((~e).sum())
    be, bf = int((big & e).sum()), int((big & ~e).sum())
    out.append('  pixels above 8: %d = %d on edges of A (%.2f %% of the %d edge pixels) + %d on flat areas (%.3f %% of the %d flat pixels)'
               % (be + bf, be, 100.0 * be / max(ne, 1), ne, bf, 100.0 * bf / max(nf, 1), nf))
    gh, gw = 4, 8
    h, w = dm.shape
    rows = []
    for gy in range(gh):
        rows.append(' '.join('%6.2f' % d[gy * h // gh:(gy + 1) * h // gh, gx * w // gw:(gx + 1) * w // gw].mean() for gx in range(gw)))
    out.append('  mad per tile (8 x 4, top row first):\n    ' + '\n    '.join(rows))
    print('\n'.join(out))
    if diff_out:
        from PIL import Image
        Image.fromarray(np.clip(d.astype(np.int32) * gain, 0, 255).astype(np.uint8), 'RGB').save(diff_out)
        print('  difference picture (x%d): %s' % (gain, diff_out))
    return d.mean()


def align(pa, directory, top=5):
    a = read_ppm(pa).astype(np.int16)
    res = []
    for p in sorted(glob.glob(os.path.join(directory, 'frame-*.ppm'))):
        b = read_ppm(p)
        if b.shape != a.shape:
            continue
        # a quarter of the rows is enough to rank, the best ones are then measured in full
        res.append((float(np.abs(a[::4] - b[::4].astype(np.int16)).mean()), p))
    if not res:
        raise SystemExit('no frame-*.ppm of the same size in %s' % directory)
    res.sort()
    full = []
    for _, p in res[:max(top, 3)]:
        full.append((float(np.abs(a - read_ppm(p).astype(np.int16)).mean()), p))
    full.sort()
    print('%s against %d dumps in %s: best %s' % (pa, len(res), directory, '   '.join('%s mad %.4f' % (os.path.basename(p), m) for m, p in full[:top])))
    worst = res[-1]
    print('  (the farthest of them: %s, mad %.3f on every 4th row)' % (os.path.basename(worst[1]), worst[0]))
    return full[0][1]


def stats(pa):
    a = read_ppm(pa)
    flat = a.reshape(-1, 3)
    key = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    print('%s: %dx%d mean rgb (%.2f, %.2f, %.2f) non-black %.3f %% distinct colours %d'
          % (pa, a.shape[1], a.shape[0], flat[:, 0].mean(), flat[:, 1].mean(), flat[:, 2].mean(), 100.0 * (key != 0).sum() / key.size, np.unique(key).size))


def main():
    av = sys.argv[1:]
    if not av or av[0] in ('-h', '--help'):
        print(__doc__); return 0 if av else 2
    def opt(name, default=None, conv=str):
        if name in av:
            i = av.index(name); v = conv(av[i + 1]); del av[i:i + 2]; return v
        return default
    if av[0] == 'pair':
        diff = opt('--diff'); gain = opt('--gain', 8, int); label = opt('--label', '')
        if len(av) != 3:
            print(__doc__); return 2
        pair(av[1], av[2], diff, gain, label); return 0
    if av[0] == 'align':
        top = opt('--top', 5, int)
        if len(av) != 3:
            print(__doc__); return 2
        align(av[1], av[2], top); return 0
    if av[0] == 'stats' and len(av) == 2:
        stats(av[1]); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
