#!/usr/bin/env python3
"""i21_compare.py - compare the pictures of task-21 runs (tools/i21_run.sh) numerically.

  i21_compare.py REF RUN [RUN ...] [--dir i21] [--window 150] [--hash rgb|bgra] [--ppm]

Every run has hash.tsv (KHNMC_GFX_HASHLOG: one line per present with a 64-bit hash of the colour bytes and one of all four channels
of the presented 1920x1080 picture) and frames/frame-NNNNNN.ppm for some presents.  Two runs of this game do not show the same
picture at the same present number even on the same binary: with KHNMC_CLOCK=frames the game clock is the display, but loading runs
on host time, so a screen can come up a few presents earlier or later (seen: the Initial Settings page first drawn at present 82,
96, 98 or 103 in four runs).  So each RUN is compared with REF in three ways:

  1. same present number:   how many presents have the identical picture (hash) in both runs.
  2. aligned by content:    every present n of RUN is looked for in REF at n+k, |k| <= window; the shift k of the previous present is
                            kept as long as it still matches, so a run of identical pictures at a constant shift is one segment.
                            Reported: presents with an identical picture in REF, the segments (first present, shift, length), the
                            presents whose picture occurs nowhere in REF inside the window, and the presents that lie past the end
                            of REF at the current shift (a run that is ahead of REF shows pictures REF never reached).
  3. --ppm:                 the dumped PPM files of equal present numbers, pixel by pixel: identical, or the number of differing
                            pixels, the largest channel difference and the mean absolute difference.
  4. --pool A,B,...:        the presents of RUN whose picture was shown by NONE of the pooled runs (REF and A, B, ...) at any
                            present: with the reference runs of all routes in the pool, what is left are pictures that no
                            reference run ever produced.

Exit status 0; the numbers are the result (nothing is judged here)."""
import argparse, os, sys


def load(path):
    rows = {}
    with open(path) as f:
        head = f.readline().rstrip('\n').split('\t')
        if head[:5] != ['present', 'game_frame', 'vblank', 'rgb_hash', 'bgra_hash']:
            sys.exit(f'{path}: not a KHNMC_GFX_HASHLOG file')
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) < 6:
                sys.exit(f'{path}: short line {line!r}')
            rows[int(p[0])] = (p[3], p[4], int(p[1]), int(p[5]))
    return rows


def ranges(xs):
    out, start, prev = [], None, None
    for x in xs:
        if start is None:
            start = prev = x
        elif x == prev + 1:
            prev = x
        else:
            out.append((start, prev)); start = prev = x
    if start is not None:
        out.append((start, prev))
    return ', '.join(f'{a}' if a == b else f'{a}..{b}' for a, b in out)


def align(ref, run, col, window):
    """-> matched, unmatched presents, segments [(first present, shift, length)], presents past the end of the reference run"""
    shift, seg, segs, unmatched, matched, beyond, last = 0, None, [], [], 0, [], max(ref)
    for n in sorted(run):
        h = run[n][col]
        if ref.get(n + shift, (None, None))[col] == h:
            k = shift
        else:
            k = None
            for d in range(1, window + 1):                       # nearest other shift first
                for c in (shift + d, shift - d):
                    if abs(c) <= window and ref.get(n + c, (None, None))[col] == h:
                        k = c; break
                if k is not None:
                    break
        if k is None:
            (beyond if n + shift > last else unmatched).append(n); seg = None      # at the current shift the reference run has ended
            continue
        matched += 1
        if seg is None or k != shift:
            seg = [n, k, 0]; segs.append(seg)
        seg[2] += 1; shift = k
    return matched, unmatched, segs, beyond


def ppm(path):
    import numpy as np
    with open(path, 'rb') as f:
        d = f.read()
    parts = d.split(b'\n', 3)
    if parts[0] != b'P6' or parts[2] != b'255':
        sys.exit(f'{path}: not a binary PPM with 8-bit samples')
    w, h = map(int, parts[1].split())
    a = np.frombuffer(parts[3], dtype=np.uint8)
    if a.size != w * h * 3:
        sys.exit(f'{path}: {a.size} sample bytes for {w}x{h}')
    return a.reshape(h, w, 3)


def ppm_compare(dref, drun):
    import numpy as np
    names = sorted(set(os.listdir(dref)) & set(os.listdir(drun)))
    names = [n for n in names if n.endswith('.ppm')]
    same, rows = 0, []
    for n in names:
        a, b = ppm(os.path.join(dref, n)), ppm(os.path.join(drun, n))
        if a.shape != b.shape:
            rows.append((n, 'size differs', 0, 0, 0.0)); continue
        if np.array_equal(a, b):
            same += 1; continue
        d = np.abs(a.astype(np.int16) - b.astype(np.int16))
        rows.append((n, '', int(np.count_nonzero(d.max(axis=2))), int(d.max()), float(d.mean())))
    return len(names), same, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ref'); ap.add_argument('runs', nargs='+')
    ap.add_argument('--dir', default='i21'); ap.add_argument('--window', type=int, default=150)
    ap.add_argument('--hash', choices=('rgb', 'bgra'), default='bgra'); ap.add_argument('--ppm', action='store_true')
    ap.add_argument('--segments', type=int, default=40, help='how many segments to list per run')
    ap.add_argument('--pool', default='', help='comma-separated further runs whose pictures count as reference pictures (item 4)')
    a = ap.parse_args()
    col = 0 if a.hash == 'rgb' else 1
    ref = load(os.path.join(a.dir, a.ref, 'hash.tsv'))
    print(f'reference {a.ref}: {len(ref)} presents hashed, {len(set(v[col] for v in ref.values()))} distinct pictures ({a.hash} hash)')
    pool, pool_names = set(v[col] for v in ref.values()), [a.ref]
    for name in filter(None, a.pool.split(',')):
        pool |= set(v[col] for v in load(os.path.join(a.dir, name, 'hash.tsv')).values()); pool_names.append(name)
    for r in a.runs:
        run = load(os.path.join(a.dir, r, 'hash.tsv'))
        common = sorted(set(ref) & set(run))
        same = sum(1 for n in common if ref[n][col] == run[n][col])
        same_rgb = sum(1 for n in common if ref[n][0] == run[n][0])
        both = sum(1 for n in common if (ref[n][0] == run[n][0]) != (ref[n][1] == run[n][1]))
        refset = set(v[col] for v in ref.values())
        distinct = set(v[col] for v in run.values())
        matched, unmatched, segs, beyond = align(ref, run, col, a.window)
        print(f'\n{r} against {a.ref}: {len(run)} presents hashed, {len(distinct)} distinct pictures')
        print(f'  1. same present number: {same} of {len(common)} presents show the identical picture ({100.0 * same / max(1, len(common)):.1f}%); '
              f'colour-only hash: {same_rgb}; presents where the two hashes disagree about equality: {both}')
        print(f'  2. aligned by content (shift up to +-{a.window} presents): {matched} of {len(run)} presents have the identical picture in the reference '
              f'({100.0 * matched / max(1, len(run)):.2f}%), in {len(segs)} segments; {len(unmatched)} presents have a picture that is nowhere in the reference window')
        print(f'     distinct pictures of this run that occur anywhere in the reference run: {len(distinct & refset)} of {len(distinct)}')
        shifts = sorted(set(s[1] for s in segs))
        print(f'     shifts used: {shifts[:30]}{" ..." if len(shifts) > 30 else ""}')
        big = sorted(segs, key=lambda s: -s[2])[:a.segments]
        print('     longest segments (first present: shift x length): ' + ', '.join(f'{s[0]}: {s[1]:+d} x {s[2]}' for s in sorted(big)))
        if unmatched:
            print(f'     presents without a match: {ranges(unmatched)}')
        if beyond:
            print(f'     presents past the end of the reference run at the current shift (no counterpart, not counted as matched): {ranges(beyond)}')
        if a.pool:
            missing = sorted(n for n in run if run[n][col] not in pool)
            print(f'  4. presents whose picture none of the {len(pool_names)} pooled runs ({", ".join(pool_names)}) ever showed: {len(missing)} of {len(run)}'
                  + (f': {ranges(missing)}' if missing else ''))
        if a.ppm:
            n, same, rows = ppm_compare(os.path.join(a.dir, a.ref, 'frames'), os.path.join(a.dir, r, 'frames'))
            print(f'  3. PPM dumps of equal present numbers: {n} files in both runs, {same} identical pixel for pixel, {len(rows)} different')
            if rows:
                worst = max(rows, key=lambda x: (x[4], x[3]))
                print(f'     largest difference: {worst[0]}: {worst[2]} of 2073600 pixels differ, largest channel difference {worst[3]}, mean absolute difference {worst[4]:.3f}')
                print('     different files (pixels, largest channel difference, mean absolute difference): ' +
                      ', '.join(f'{x[0][6:12]}({x[2]},{x[3]},{x[4]:.2f})' for x in rows))
    return 0


if __name__ == '__main__':
    sys.exit(main())
