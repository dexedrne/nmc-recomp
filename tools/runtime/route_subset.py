#!/usr/bin/env python3
"""route_subset.py : build the subset of the data tree that a route really touches, from open logs of tools/openlog_shim.c.

  route_subset.py --data DATA_ROOT --pkgdir PKG_ROOT --out OUTDIR LOG [LOG ...]

DATA_ROOT / PKG_ROOT are the paths the logged runs had in KHNMC_DATA / KHNMC_PKGDIR (as given there: the log holds them literally).
Writes OUTDIR/data and OUTDIR/pkgdir with the same relative paths, and OUTDIR/MANIFEST.tsv.  The union of all logs decides:

  opened file                       copied whole (content), unless it is a "large" file for which the logs hold byte ranges and no
                                    `whole` line: then a sparse file of the same size with exactly those ranges (rounded out to
                                    64 KiB) filled from the source.  Ranges are the read() / pread() calls the shim saw on the
                                    descriptor; a file read any other way is taken whole.
  listed directory (opendir)        every entry exists with its name and kind; an entry nobody opened or stat()ed is an empty file
                                    or an empty directory (its name is all a listing shows)
  path that was only stat()ed       directory: created; file: a sparse file of the SAME size (the size is all stat shows)
  path whose open / stat failed     must not exist in the subset either: checked
Refuses (exit 1) when two names in one output directory differ only by case (a case-folding file system, macOS by default, would
merge them), when a logged path under a root no longer exists, or when the result would contradict a logged failure.
Paths outside both roots are listed in the summary (the exe, the input script, the run's own state directories) and not copied.
Python 3.6+, standard library.
"""
import argparse
import collections
import hashlib
import os
import shutil
import sys

ROUND = 1 << 16


def die(msg):
    sys.stderr.write('route_subset: ERROR: %s\n' % msg)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True); ap.add_argument('--pkgdir', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('logs', nargs='+')
    a = ap.parse_args()
    roots = [('data', os.path.normpath(a.data)), ('pkgdir', os.path.normpath(a.pkgdir))]
    opened, listed, stated, failed, wholes = set(), set(), set(), set(), set()
    ranges = collections.defaultdict(list)
    outside = collections.Counter()
    nlines = 0

    def rel(path):
        p = os.path.normpath(path)
        for name, root in roots:
            if p == root:
                return name, ''
            if p.startswith(root + '/'):
                return name, p[len(root) + 1:]
        return None

    for lg in a.logs:
        with open(lg, errors='surrogateescape') as f:
            for ln in f:
                ln = ln.rstrip('\n'); nlines += 1
                if ln.startswith('range '):
                    _, span, _ok, path = ln.split(' ', 3)
                    off, length = span.split('+')
                    r = rel(path)
                    if r: ranges[r].append((int(off), int(length)))
                    continue
                parts = ln.split(' ', 2)
                if len(parts) < 3: continue
                op, res, path = parts
                r = rel(path)
                if not r:
                    outside[path if len(path) < 90 else path[:87] + '...'] += 1
                    continue
                if res != 'ok': failed.add(r)
                elif op == 'open': opened.add(r)
                elif op == 'dir': listed.add(r)
                elif op == 'stat': stated.add(r)
                elif op == 'whole': wholes.add(r)
    if not opened:
        die('the logs name no opened file under %s or %s' % (roots[0][1], roots[1][1]))
    src = dict(roots)
    plan = {}                                   # (root, relpath) -> kind

    def srcpath(r): return os.path.join(src[r[0]], r[1]) if r[1] else src[r[0]]

    for r in sorted(opened | stated | listed):
        s = srcpath(r)
        if not os.path.exists(s): die('logged as existing but gone now: %s' % s)
        if os.path.isdir(s):
            plan[r] = 'dir'
        elif r in opened:
            plan[r] = 'ranges' if (r in ranges and r not in wholes) else 'full'
        else:
            plan[r] = 'size'
    for r in sorted(listed):
        s = srcpath(r)
        for name in sorted(os.listdir(s)):
            e = (r[0], os.path.join(r[1], name) if r[1] else name)
            if e not in plan:
                plan[e] = 'dir' if os.path.isdir(srcpath(e)) else 'name'
    for r in ranges:
        if r not in opened: die('byte ranges logged for a file that was never opened: %s' % srcpath(r))
    # a logged failure must stay a failure; ancestors of everything must be directories
    for r in sorted(failed):
        if r in plan: die('logged both as missing and as existing (it changed between runs?): %s' % srcpath(r))
    # case collisions per output directory
    seen = {}
    for r in plan:
        comps = r[1].split('/') if r[1] else []
        for i in range(1, len(comps) + 1):
            key = (r[0], '/'.join(comps[:i - 1]), comps[i - 1].lower())
            prev = seen.setdefault(key, comps[i - 1])
            if prev != comps[i - 1]:
                die('names differ only by case in %s/%s: %s and %s' % (r[0], key[1], prev, comps[i - 1]))
    out = os.path.abspath(a.out)
    if os.path.exists(out) and os.listdir(out):
        die('output directory %s is not empty' % out)
    os.makedirs(out, exist_ok=True)
    stats = collections.Counter(); nbytes = collections.Counter(); manifest = []
    for r in sorted(plan):
        kind = plan[r]; s = srcpath(r); d = os.path.join(out, r[0], r[1]) if r[1] else os.path.join(out, r[0])
        if kind == 'dir':
            os.makedirs(d, exist_ok=True); stats['dir'] += 1; manifest.append((r[0] + '/' + r[1], 'dir', 0, 0, '-')); continue
        os.makedirs(os.path.dirname(d), exist_ok=True)
        size = os.path.getsize(s)
        if kind == 'full':
            h = hashlib.sha256()
            with open(s, 'rb') as fi, open(d, 'wb') as fo:
                while True:
                    b = fi.read(1 << 20)
                    if not b: break
                    h.update(b); fo.write(b)
            stats['full'] += 1; nbytes['full'] += size; manifest.append((r[0] + '/' + r[1], 'full', size, size, h.hexdigest()))
        elif kind == 'ranges':
            spans = sorted((o // ROUND * ROUND, min(size, -(-(o + n) // ROUND) * ROUND)) for o, n in ranges[r])
            merged = []
            for lo, hi in spans:
                if merged and lo <= merged[-1][1]: merged[-1][1] = max(merged[-1][1], hi)
                else: merged.append([lo, hi])
            kept = 0
            with open(s, 'rb') as fi, open(d, 'wb') as fo:
                fo.truncate(size)
                for lo, hi in merged:
                    fi.seek(lo); fo.seek(lo); left = hi - lo
                    while left:
                        b = fi.read(min(left, 1 << 20))
                        if not b: die('short read of %s' % s)
                        fo.write(b); left -= len(b)
                    kept += hi - lo
            stats['ranges'] += 1; nbytes['ranges'] += kept
            manifest.append((r[0] + '/' + r[1], 'ranges', size, kept, ' '.join('%d-%d' % (lo, hi) for lo, hi in merged)))
        elif kind == 'size':
            with open(d, 'wb') as fo: fo.truncate(size)
            stats['size'] += 1; manifest.append((r[0] + '/' + r[1], 'size-only', size, 0, '-'))
        else:
            open(d, 'wb').close(); stats['name'] += 1; manifest.append((r[0] + '/' + r[1], 'name-only', 0, 0, '-'))
    for r in sorted(failed):
        d = os.path.join(out, r[0], r[1])
        if os.path.lexists(d): die('logged as missing but present in the subset: %s' % d)
    with open(os.path.join(out, 'MANIFEST.tsv'), 'w') as f:
        f.write('path\tkind\tsize\tbytes_kept\tsha256_or_ranges\n')
        for row in manifest: f.write('%s\t%s\t%d\t%d\t%s\n' % row)
    print('route_subset: %d log lines from %d logs' % (nlines, len(a.logs)))
    print('route_subset: %d files copied whole (%.1f MB), %d large files by byte ranges (%.1f MB kept), %d size-only files, %d name-only entries, %d directories; %d paths logged as missing stay missing'
          % (stats['full'], nbytes['full'] / 1e6, stats['ranges'], nbytes['ranges'] / 1e6, stats['size'], stats['name'], stats['dir'], len(failed)))
    for row in manifest:
        if row[1] == 'ranges': print('route_subset:   ranges of %s (%d bytes): %s' % (row[0], row[2], row[4]))
    print('route_subset: paths outside both roots (not copied): %d distinct; the most frequent:' % len(outside))
    for p, n in outside.most_common(6): print('route_subset:   %6d  %s' % (n, p))
    print('route_subset: wrote %s (data/, pkgdir/, MANIFEST.tsv)' % out)


if __name__ == '__main__':
    main()
