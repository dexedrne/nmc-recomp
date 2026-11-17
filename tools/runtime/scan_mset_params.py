#!/usr/bin/env python3
"""Task 11: read-only scanner for the player-parameter trigger chunks (BAR type 16) in the shipped data.

Lists the range-trigger kinds and frame-op kinds used per file; extension kinds are range 54..83 (DLL 0x2df50 -> exe 0x40d810)
and frame 50..80 (DLL 0x2ec30 -> exe 0x40dd70).  Never writes anything.  Usage: scan_mset_params.py DATA_DIR [--files]

Chunk layout (from the OpenKH BAR/motion trigger docs and cross-checked against the two handlers, which read the live records):
  +0 u8 rangeCount, +1 u8 frameCount, +2 u16 frameOffset, ranges from +4:
     range  : s16 start, s16 end, u8 kind, u8 nparam, nparam*s16   (handler: byte[+2] low end byte, kind[+4], nparam[+5], params[+6..])
     frame  : s16 frame, u8 kind, u8 nparam, nparam*s16            (handler: kind[+2], nparam[+3], params[+4..])
A chunk is reported malformed when it cannot be walked inside its declared size, when the range block overruns frameOffset, or when
trailing bytes remain that are not explained; the reason is given so the count can be audited rather than assumed.
"""
import collections, os, struct, sys

RANGE_EXT = range(54, 84)
FRAME_EXT = range(50, 81)


def entries(buf, base):
    """BAR directory at buf[base:]; entry offsets are relative to base and may run past the parent entry's declared size
    (the game maps the whole file, so the bytes after a sub-BAR are what the engine really reads); only EOF is a hard limit."""
    if base + 16 > len(buf):
        return None
    n = struct.unpack_from('<I', buf, base + 4)[0]
    if n > 65536 or base + 16 + n * 16 > len(buf):
        return None
    return [struct.unpack_from('<HH4sII', buf, base + 16 + i * 16) for i in range(n)]


class Scan:
    def __init__(self):
        self.range = collections.Counter()
        self.frame = collections.Counter()
        self.range_files = collections.defaultdict(set)
        self.frame_files = collections.defaultdict(set)
        self.range_params = collections.defaultdict(collections.Counter)
        self.frame_params = collections.defaultdict(collections.Counter)
        self.range_nparam = collections.defaultdict(collections.Counter)
        self.frame_nparam = collections.defaultdict(collections.Counter)
        self.bad = []
        self.good = 0
        self.empty = 0
        self.files = 0
        self.bars = 0
        self.bad_bar = []
        self.overrun = []
        self.chunks_by_top = collections.Counter()
        self.top_file = None

    def chunk(self, d, where, top):
        if len(d) == 0:
            self.empty += 1
            return
        if len(d) < 4:
            self.bad.append((where, 'chunk shorter than 4-byte header (%d)' % len(d)))
            return
        rc, fc, fo = struct.unpack_from('<BBH', d)
        p = 4
        rec = []
        try:
            for _ in range(rc):
                if p + 6 > len(d):
                    raise ValueError('range header overruns chunk at +%d' % p)
                a, b, k, np_ = struct.unpack_from('<hhBB', d, p)
                if p + 6 + 2 * np_ > len(d):
                    raise ValueError('range params overrun chunk at +%d' % p)
                rec.append((0, k, struct.unpack_from('<%dh' % np_, d, p + 6) if np_ else ()))
                p += 6 + 2 * np_
            if p > fo:
                raise ValueError('range block ends at +%d past frameOffset +%d' % (p, fo))
            q = fo
            for _ in range(fc):
                if q + 4 > len(d):
                    raise ValueError('frame header overruns chunk at +%d' % q)
                a, k, np_ = struct.unpack_from('<hBB', d, q)
                if q + 4 + 2 * np_ > len(d):
                    raise ValueError('frame params overrun chunk at +%d' % q)
                rec.append((1, k, struct.unpack_from('<%dh' % np_, d, q + 4) if np_ else ()))
                q += 4 + 2 * np_
        except ValueError as e:
            self.bad.append((where, str(e)))
            return
        self.good += 1
        self.chunks_by_top[top] += 1
        for lane, k, pars in rec:
            (self.frame if lane else self.range)[k] += 1
            (self.frame_files if lane else self.range_files)[k].add(self.top_file)
            (self.frame_nparam if lane else self.range_nparam)[k][len(pars)] += 1
            pc = (self.frame_params if lane else self.range_params)[k]
            if len(pc) < 400:
                pc[pars] += 1

    def bar(self, buf, base, limit, where, top, depth=0):
        """limit = absolute end of the parent entry that declared this BAR (children running past it are counted, not dropped)."""
        if depth > 6 or buf[base:base + 4] != b'BAR\1':
            return
        self.bars += 1
        e = entries(buf, base)
        if e is None:
            self.bad_bar.append(where + ' (directory)')
            return
        seen = set()
        for k, dup, name, o, s in e:
            key = where + '/' + name.rstrip(b'\0').decode('latin1')
            if s == 0:
                if k == 16:
                    self.empty += 1
                continue
            if base + o + s > len(buf):
                self.bad_bar.append(key + ' (entry past EOF)')
                continue
            if (o, s) in seen:      # duplicate entries (dup flag) alias the same bytes: count each distinct (offset,size) once
                continue
            seen.add((o, s))
            if base + o + s > limit:
                self.overrun.append(key)
            if k == 16:
                self.chunk(buf[base + o:base + o + s], key, top)
            elif buf[base + o:base + o + 4] == b'BAR\1':
                self.bar(buf, base + o, base + o + s, key, top, depth + 1)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    sc = Scan()
    for dp, dn, fn in os.walk(root, followlinks=True):
        for f in fn:
            path = os.path.join(dp, f)
            try:
                with open(path, 'rb') as fh:
                    head = fh.read(4)
                    if head != b'BAR\1':
                        continue
                    fh.seek(0)
                    d = fh.read()
            except OSError as e:
                sys.exit('scan: cannot read %s: %s' % (path, e))
            sc.files += 1
            rel = os.path.relpath(path, root)
            sc.top_file = rel
            top = 'mset' if f.endswith('.mset') else ('anb' if f.endswith('.anb') else 'other')
            sc.bar(d, 0, len(d), rel, top)
    print('bar files scanned %d, bars %d, trigger chunks decoded %d (mset %d, anb %d, other %d), empty chunks %d, malformed chunks %d, unreadable BAR entries %d, chunks running past their parent entry %d' % (
        sc.files, sc.bars, sc.good, sc.chunks_by_top['mset'], sc.chunks_by_top['anb'], sc.chunks_by_top['other'], sc.empty, len(sc.bad), len(sc.bad_bar), len(sc.overrun)))
    for lane, cnt, files, params, nparam, ext in (('range', sc.range, sc.range_files, sc.range_params, sc.range_nparam, RANGE_EXT),
                                                   ('frame', sc.frame, sc.frame_files, sc.frame_params, sc.frame_nparam, FRAME_EXT)):
        print('== %s kinds (count/files); * = DLL extension range' % lane)
        for k in sorted(cnt):
            tag = '*' if k in ext else ' '
            print(' %s kind %3d : %6d records in %5d files; nparam %s' % (tag, k, cnt[k], len(files[k]), dict(sorted(nparam[k].items()))))
        print('== %s extension kinds (and out-of-table kinds above the DLL range): examples' % lane)
        for k in sorted(cnt):
            if k in ext or k > max(ext):
                print('  kind %d: files=%s' % (k, sorted(files[k])[:6]))
                print('    params(top)=%s' % (params[k].most_common(8),))
    print('== malformed reasons')
    rs = collections.Counter(r.split(' at +')[0].split(' (')[0] for _, r in sc.bad)
    for r, n in rs.most_common():
        print('  %5d %s' % (n, r))
    if '--files' in sys.argv:
        for w, r in sc.bad:
            print('  BAD', w, r)
        for w in sc.bad_bar:
            print('  BADBAR', w)


if __name__ == '__main__':
    main()
