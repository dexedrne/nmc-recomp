#!/usr/bin/env python3
"""music_compare.py : compare the music logs of two runs (KHNMC_MUSICLOG, runtime/music_log.c; task 32, item 6c).

  music_compare.py REFERENCE.log OTHER.log [--label-a NAME] [--label-b NAME] [--frames]

Two logs are THE SAME when they hold the same lines in the same order once the game-frame numbers are taken out (the frame at
which a load ends differs by a few frames from run to run with the host clock; the largest difference is printed).  `--frames`
also demands equal frame numbers.  Otherwise every difference is listed: lines only in one log, in order (a line diff), each with
the game frame and map of its own run.  Also printed for both: the final state of the two music slots, and the sequence of tracks
that reached full volume (0x3000) in slot 0 or slot 1, per map.  Exit status 0 = same, 1 = different, 2 = usage.
Python 3.6+, standard library."""
import argparse, difflib, re, sys

FR = re.compile(r' game-frame=(\d+)')


def load(path):
    rows = []
    for ln in open(path, errors='replace'):
        ln = ln.rstrip('\n')
        if not ln.startswith('kh: music'): continue
        m = FR.search(ln)
        rows.append((int(m.group(1)) if m else -1, FR.sub('', ln)))
    return rows


def final(rows):
    for fr, l in reversed(rows):
        if ' slot0 ' in l: return l[l.index('slot0'):]
    return '(no state line)'


def audible(rows):
    out = []
    for fr, l in rows:
        m = re.search(r'map (\S+) slot0 track=(\d+) file=(\d+) vol=(0x[0-9a-f]+) slot1 track=(\d+) file=(\d+) vol=(0x[0-9a-f]+)', l)
        if not m: continue
        cur = (m.group(1), m.group(3) if m.group(4) == '0x3000' else '-', m.group(6) if m.group(7) == '0x3000' else '-')
        if not out or out[-1][1:] != cur[1:]: out.append(cur)
    return ' -> '.join('%s/%s' % (a[1], a[2]) for a in out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a'); ap.add_argument('b'); ap.add_argument('--label-a', default='reference'); ap.add_argument('--label-b', default='other'); ap.add_argument('--frames', action='store_true')
    a = ap.parse_args()
    A, B = load(a.a), load(a.b)
    if not A or not B: sys.stderr.write('music_compare: ERROR: a log holds no `kh: music` line\n'); return 2
    la, lb = [l for f, l in A], [l for f, l in B]
    same = la == lb
    print('%s: %d lines; %s: %d lines' % (a.label_a, len(A), a.label_b, len(B)))
    if same:
        d = [abs(fa - fb) for (fa, _), (fb, _) in zip(A, B)]
        print('SAME sequence of music changes and music file requests (frame numbers apart); largest frame difference %d, %d of %d lines at the same frame' % (max(d), sum(1 for x in d if x == 0), len(d)))
        if a.frames and max(d): print('DIFFERENT frame numbers (--frames)'); same = False
    else:
        print('DIFFERENT: lines of %s only are marked -, lines of %s only +' % (a.label_a, a.label_b))
        sm = difflib.SequenceMatcher(None, la, lb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal': continue
            for i in range(i1, i2): print('  - frame %-6d %s' % (A[i][0], A[i][1][10:]))
            for j in range(j1, j2): print('  + frame %-6d %s' % (B[j][0], B[j][1][10:]))
    print('final state  %-10s %s' % (a.label_a, final(A)))
    print('final state  %-10s %s' % (a.label_b, final(B)))
    print('tracks at full volume, slot0/slot1  %-10s %s' % (a.label_a, audible(A)))
    print('tracks at full volume, slot0/slot1  %-10s %s' % (a.label_b, audible(B)))
    return 0 if same else 1


if __name__ == '__main__':
    sys.exit(main())
