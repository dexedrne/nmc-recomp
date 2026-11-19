#!/usr/bin/env python3
"""movie_table.py (task 44): every pre-rendered movie the game can play, from the data, and what a movie FILE says about itself.

  tools/movie_table.py --data DATA_TREE [--movies DIR ...]     the table (tab-separated) and, per file found in a DIR, its header facts
  tools/movie_table.py --selftest                              the container reader on synthetic files (no game data)

Where movies come from (exe RVAs; implementation notes): the ONLY caller of the movie module's start command (0x5375d0 case 0) is the
event command 0x44 (0x2db1c0; payload: u16 frame, then the name).  The game plays "<BasePath>/zmovie/<en|jp>/<name>.mp4".  So the
movies are the names in the 0x44 commands of the event entries (BAR type 22) of ard/<lang>/*.ard; tools/event_refs.py says which evt
program names each event and whether the story graph or the Theater reaches it.  The event's end frame (command 0x08) is the movie's
length in event frames (30 a second) as the data states it; a fade command (0x12) of type 2 with 0 frames near the end is an instant
white-out that the timeline does before the movie's end.

How a movie's event is STARTED decides two things the movie seam depends on (review of task 44; exe RVAs).  The event starter
0x3ac350(event, TYPE, ...) looks the record of TYPE & 0x3f up in 03system.bin `evtp` (0x3ead50: a bsearch over 8-byte records, key
u8 +0, fade kind u8 +1, flags u8 +2) and, for a TYPE with bit 7, sets pause-disable bit 1 (0x3ac402 -> 0x157250): the scene has NO
pause screen, so no Skip.  Flag bit 0 of the record sends the event runner through its state 1 (0x3ac86e..0x3ac907), which sets the
byte 0x140728440 (0x1dcd00); at the event's end 0x2cbd80 runs its clean-up 0x2cbe30 AS A TASK only when that byte is set (0x2cbdba),
and only the task form reaches movie command 2 (0x2db150 returns at once for a null task), which stops the movie and destroys the
player object.  Without it the player survives and the game's own guard in movie command 0 (0x537632..0x537649) drops the NEXT movie
request without queuing anything: that scene then waits for ever, on the PC too.  So the table prints, per program that names a
movie's event, the type, its `evtp` key and flags, the end branch and whether the scene has a pause screen, and the tool FAILS
(exit status 1, one line per case on stderr) when a movie event is started with a type whose key is not in `evtp` or lacks flag
bit 0: no movie of the shipped data does, a new NMC version could.

A movie FILE is read WITHOUT decoding and WITHOUT the engine's cipher step: the engine (Movie::open 0x13da10) treats a file whose
bytes 4..7 are not `ftyp` as having its first 0x100 bytes ciphered and rewrites them in place before it opens the file.  This tool
does not do that and does not look at how it is done; when the first bytes are not a plain header it says so and reads only what is
plain in the rest of the file: the `moov` box (found by its type and checked by its `mvhd`), its tracks' `tkhd` (size), `mdhd`
(duration), `hdlr` (video / sound) and the first sample entry of `stsd` (codec fourcc; channels and sample rate for sound).
Standard library; reads at most the last and first 64 MiB of a file for the `moov` search plus the box itself."""
import argparse, os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
CONTAINERS = (b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'udta')

def boxes(b, lo, hi):
    """(type, payload start, end) of the boxes in b[lo:hi]; stops at the first size that does not fit"""
    p = lo
    while p + 8 <= hi:
        n, t = struct.unpack_from('>I4s', b, p); h = 8
        if n == 1:
            if p + 16 > hi: return
            n = struct.unpack_from('>Q', b, p + 8)[0]; h = 16
        elif n == 0: n = hi - p
        if n < h or p + n > hi: return
        yield t, p + h, p + n; p += n

def find1(b, lo, hi, t):
    for ty, a, e in boxes(b, lo, hi):
        if ty == t: return a, e
    return None

def parse_moov(b, lo, hi):
    """facts of a moov payload b[lo:hi] -> dict, or None when it has no mvhd"""
    mv = find1(b, lo, hi, b'mvhd')
    if not mv: return None
    a = mv[0]; v = b[a]
    ts, dur = struct.unpack_from('>IQ' if v == 1 else '>II', b, a + (20 if v == 1 else 12))
    if not ts: return None
    out = {'duration_s': dur / ts, 'tracks': []}
    for ty, ta, te in boxes(b, lo, hi):
        if ty != b'trak': continue
        tr = {'kind': '?', 'codec': '?', 'width': 0, 'height': 0}
        tk = find1(b, ta, te, b'tkhd')
        if tk:
            w, h = struct.unpack_from('>II', b, tk[1] - 8); tr['width'] = w >> 16; tr['height'] = h >> 16
        md = find1(b, ta, te, b'mdia')
        if md:
            mh = find1(b, md[0], md[1], b'mdhd')
            if mh:
                v = b[mh[0]]; mts, mdur = struct.unpack_from('>IQ' if v == 1 else '>II', b, mh[0] + (20 if v == 1 else 12))
                tr['duration_s'] = mdur / mts if mts else 0.0; tr['timescale'] = mts
            hd = find1(b, md[0], md[1], b'hdlr')
            if hd: tr['kind'] = {b'vide': 'video', b'soun': 'sound'}.get(b[hd[0] + 8:hd[0] + 12], b[hd[0] + 8:hd[0] + 12].decode('latin1'))
            mi = find1(b, md[0], md[1], b'minf'); sb = mi and find1(b, mi[0], mi[1], b'stbl'); sd = sb and find1(b, sb[0], sb[1], b'stsd')
            if sd and sd[0] + 16 <= sd[1]:
                e = sd[0] + 8; tr['codec'] = b[e + 4:e + 8].decode('latin1')
                if tr['kind'] == 'sound' and e + 36 <= sd[1]:
                    tr['channels'] = struct.unpack_from('>H', b, e + 24)[0]; tr['sample_rate'] = struct.unpack_from('>I', b, e + 32)[0] >> 16
                if tr['kind'] == 'video' and e + 36 <= sd[1]:
                    tr['coded_width'], tr['coded_height'] = struct.unpack_from('>HH', b, e + 32)
            st = sb and find1(b, sb[0], sb[1], b'stts')
            if st and tr['kind'] == 'video' and tr.get('duration_s'):
                n = struct.unpack_from('>I', b, st[0] + 4)[0]; frames = sum(struct.unpack_from('>I', b, st[0] + 8 + 8 * i)[0] for i in range(min(n, (st[1] - st[0] - 8) // 8)))
                tr['frames'] = frames; tr['fps'] = frames / tr['duration_s']
        out['tracks'].append(tr)
    return out

def read_movie(path, window=64 << 20):
    """-> dict: size, plain (bytes 4..7 are ftyp), brand, and the moov facts when a moov box can be read as plain bytes"""
    size = os.path.getsize(path); out = {'size': size, 'plain': False, 'brand': '', 'moov': None, 'where': ''}
    with open(path, 'rb') as f:
        head = f.read(min(size, window)); out['plain'] = head[4:8] == b'ftyp'
        if out['plain']: out['brand'] = head[8:12].decode('latin1')
        parts = [(0, head)]
        if size > window: f.seek(size - window); parts.append((size - window, f.read(window)))
        for base, buf in parts:
            p = 0
            while True:
                p = buf.find(b'moov', p + 1)
                if p < 4: break
                n = struct.unpack_from('>I', buf, p - 4)[0]
                if n < 16 or base + p - 4 + n > size: continue
                f.seek(base + p - 4); box = f.read(n)
                facts = parse_moov(box, 8, n)
                if facts and facts['tracks']: out['moov'] = facts; out['where'] = 'offset %d, %d bytes' % (base + p - 4, n); return out
    return out

def describe(path):
    m = read_movie(path)
    head = 'plain MP4 header (ftyp %s)' % m['brand'] if m['plain'] else 'the first bytes are NOT a plain MP4 header (the engine ciphers the first 0x100 bytes; not read here)'
    if not m['moov']: return '%d bytes; %s; no plain moov box found: codecs, size and duration unknown from this file' % (m['size'], head)
    t = m['moov']; s = '%d bytes; %s; ISO base media (MP4), moov at %s; %.3f s' % (m['size'], head, m['where'], t['duration_s'])
    for tr in t['tracks']:
        if tr['kind'] == 'video': s += '; video %s %dx%d%s' % (tr['codec'], tr.get('coded_width') or tr['width'], tr.get('coded_height') or tr['height'], ', %d frames = %.3f a second' % (tr['frames'], tr['fps']) if tr.get('frames') else '')
        elif tr['kind'] == 'sound': s += '; sound %s %d ch %d Hz' % (tr['codec'], tr.get('channels', 0), tr.get('sample_rate', 0))
        else: s += '; track %s %s' % (tr['kind'], tr['codec'])
    return s

def parse_evtp(d):
    """the `evtp` subfile of 03system.bin -> {key: (fade kind, flags)}, or None when it is not that table (u32 version 1, u32 count,
    count records of 8 bytes sorted by their key byte, as the exe's bsearch 0x3ead50 needs)"""
    if d is None or len(d) < 8: return None
    ver, n = struct.unpack_from('<II', d, 0)
    if ver != 1 or n == 0 or 8 + 8 * n > len(d): return None
    out = {}; prev = -1
    for i in range(n):
        k, fade, flags = d[8 + 8 * i], d[9 + 8 * i], d[10 + 8 * i]
        if k <= prev: return None
        prev = k; out[k] = (fade, flags)
    return out

def start_facts(named, evtp):
    """named = [(program kind, program, trigger, event type)] of one movie event -> (text for the table, [problem, ...])"""
    parts = []; bad = []
    for kind, pid, trig, ty in named:
        key = ty & 0x3f; rec = evtp.get(key) if evtp else None; nopause = bool(ty & 0x80)
        who = '%s:%#x type %d' % (kind, pid, ty)
        if rec is None:
            parts.append('%s = key %#x: NOT IN evtp' % (who, key))
            bad.append('%s program %#x starts it with type %d = `evtp` key %#x, which %s' % (kind, pid, ty, key, 'the table does not have (the starter\'s lookup 0x3ead50 finds nothing)' if evtp else 'cannot be looked up: no `evtp` table was read'))
            continue
        task = bool(rec[1] & 1)
        parts.append('%s = key %#x flags %#04x: end clean-up %s; %s' % (who, key, rec[1], 'as a task (player destroyed)' if task else 'DIRECT (the player SURVIVES)', 'NO pause screen (type bit 7)' if nopause else 'pause screen'))
        if not task:
            bad.append('%s program %#x starts it with type %d = `evtp` key %#x whose flags %#04x lack bit 0: its end takes the direct branch of 0x2cbd80, movie command 2 is skipped, the player object survives and the game drops the NEXT movie request' % (kind, pid, ty, key, rec[1]))
    return ' | '.join(parts) or '-', bad

def table(a):
    import event_refs as E
    tree = E.Tree(a.data); graph = E.Graph(tree); rows = []
    files = {}
    for d in a.movies or []:
        for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if n.lower().endswith('.mp4'): files.setdefault(n.lower()[:-4], os.path.join(d, n))
    evtp = None
    for t, nm, d in E.bar(tree._read('03system.bin')):
        if nm == 'evtp': evtp = parse_evtp(d)
    print('movie\tarea/event\tproject\tend frame (event frames, 30 a second)\tseconds\tsubtitle commands\tend fade\tclass (story graph / Theater)\tnamed by\tfile\tstarted with (per naming program: type, evtp key and flags, end branch of 0x2cbd80, pause screen)')
    seen = set(); problems = []; started = 0
    for an, area in tree.areas.items():
        for nm, d in area.events.items():
            ev = E.Event(tree, an, nm, d)
            if not ev.movies: continue
            fades = [struct.unpack_from('<HhH', pl, 0) for ty, pl in ev.cmds if ty == 0x12 and len(pl) >= 6]
            fade = '; '.join('frame %d: type %d over %d frames%s' % (f, t, n, ' (instant white-out)' if t == 2 and n == 0 else '') for f, n, t in fades) or 'none'
            named = ' '.join('%s:%#x/trigger %d/type %d' % x for x in graph.named.get((an, nm), [])) if hasattr(graph, 'named') else ''
            start, bad = start_facts(graph.named.get((an, nm), []), evtp); started += len(graph.named.get((an, nm), []))
            problems += ['%s (%s/%s): %s' % ('/'.join(sorted(ev.movies)), an, nm, b) for b in bad]
            for mv in sorted(ev.movies):
                seen.add(mv.lower()); f = files.get(mv.lower())
                th = graph.theater.get((an, nm)); cls = graph.klass(an, nm, ev) + (' + Theater (%s)' % ', '.join('%s program %#x' % (w, p) for w, p, _ in th) if th else '')
                print('%s\t%s/%s\t%s\t%d\t%.1f\t%d\t%s\t%s\t%s\t%s\t%s' % (mv, an, nm, ev.project, ev.end, ev.end / 30.0, ev.subs, fade, cls, named or '-', describe(f) if f else 'no file in the given directories', start))
    for k, f in sorted(files.items()):
        if k not in seen: print('%s\t-\t-\t-\t-\t-\t-\tno event of this tree names it\t-\t%s\t-' % (k, describe(f)))
    if evtp is None: problems.insert(0, '03system.bin has no readable `evtp` table (version 1, sorted 8-byte records): how the movie events are started cannot be checked')
    for b in problems: sys.stderr.write('movie_table: MOVIE START CHECK FAILED: %s\n' % b)
    sys.stderr.write('movie_table: movie start check: %d naming programs of movie events looked up in `evtp` (%s records): %s\n'
                     % (started, len(evtp) if evtp else 'no', '%d FAILED' % len(problems) if problems else 'every one ends its event through the task branch of 0x2cbd80 (flag bit 0), so every movie\'s player is destroyed before the next request'))
    return 1 if problems else 0

def selftest():
    bad = [0]; n = [0]
    def T(c, what):
        n[0] += 1
        if not c: bad[0] += 1; print('movie_table selftest FAIL: ' + what)
    def box(t, payload): return struct.pack('>I4s', 8 + len(payload), t) + payload
    def full(t, payload, v=0): return box(t, bytes([v, 0, 0, 0]) + payload)
    mvhd = full(b'mvhd', struct.pack('>IIII', 0, 0, 600, 600 * 90) + bytes(80))
    def trak(kind, codec, w, h, ts, dur, entry_tail, stts=b''):
        tkhd = full(b'tkhd', bytes(72) + struct.pack('>II', w << 16, h << 16))
        mdhd = full(b'mdhd', struct.pack('>IIII', 0, 0, ts, dur) + bytes(4)); hdlr = full(b'hdlr', bytes(4) + kind + bytes(13))
        stsd = full(b'stsd', struct.pack('>I', 1) + struct.pack('>I4s', 16 + len(entry_tail), codec) + bytes(8) + entry_tail)
        return box(b'trak', tkhd + box(b'mdia', mdhd + hdlr + box(b'minf', box(b'stbl', stsd + stts))))
    vid = trak(b'vide', b'avc1', 1920, 1080, 30000, 30000 * 90, bytes(16) + struct.pack('>HH', 1920, 1080) + bytes(50), full(b'stts', struct.pack('>III', 1, 2700, 1000)))
    aud = trak(b'soun', b'mp4a', 0, 0, 48000, 48000 * 90, bytes(8) + struct.pack('>HHHHI', 2, 16, 0, 0, 48000 << 16))
    moov = box(b'moov', mvhd + vid + aud); ftyp = box(b'ftyp', b'isom' + bytes(4) + b'isomavc1'); mdat = box(b'mdat', bytes(1000))
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        for name, data, plain in (('fast.mp4', ftyp + moov + mdat, True), ('tail.mp4', ftyp + mdat + moov, True),
                                  ('hidden-head.mp4', b'\xa5' * 0x100 + (ftyp + mdat)[0x100:] + moov, False), ('no-moov.mp4', ftyp + mdat, True),
                                  ('hidden-all.mp4', b'\xa5' * 0x100 + bytes(900), False), ('wide.mp4', ftyp + box(b'mdat', bytes(8)) + struct.pack('>I4sQ', 1, b'free', 16 + 24) + bytes(24) + moov, True)):
            p = os.path.join(d, name); open(p, 'wb').write(data); m = read_movie(p, window=4096)
            T(m['size'] == len(data) and m['plain'] == plain, name + ': size / plain header')
            if 'no-moov' in name or 'hidden-all' in name:
                T(m['moov'] is None and 'no plain moov box found' in describe(p), name + ': no moov'); continue
            t = m['moov']; T(t is not None and abs(t['duration_s'] - 90.0) < 1e-9 and len(t['tracks']) == 2, name + ': moov, duration, two tracks')
            if not t: continue
            v, s = t['tracks']
            T(v['kind'] == 'video' and v['codec'] == 'avc1' and v['width'] == 1920 and v['height'] == 1080 and v['coded_width'] == 1920 and v['frames'] == 2700 and abs(v['fps'] - 30.0) < 1e-9, name + ': video track')
            T(s['kind'] == 'sound' and s['codec'] == 'mp4a' and s['channels'] == 2 and s['sample_rate'] == 48000 and abs(s['duration_s'] - 90.0) < 1e-9, name + ': sound track')
            ds = describe(p); T('video avc1 1920x1080, 2700 frames = 30.000 a second' in ds and 'sound mp4a 2 ch 48000 Hz' in ds and '90.000 s' in ds, name + ': description')
            T(('NOT a plain MP4 header' in ds) == (not plain), name + ': says whether the head is plain')
        big = os.path.join(d, 'far.mp4'); open(big, 'wb').write(ftyp + box(b'mdat', bytes(20000)) + moov + box(b'free', bytes(100)))
        T(read_movie(big, window=4096)['moov'] is not None, 'a moov in the last window of a file larger than the window')
        T(read_movie(big, window=64)['moov'] is None, 'a moov outside both windows is not found (and nothing is invented)')
    # how a movie's event is started (review of task 44): the `evtp` table and the check on it
    rec = lambda k, fade, fl: bytes([k, fade, fl, 0, 0, 0, 0, 0])
    blob = struct.pack('<II', 1, 4) + rec(2, 1, 0x65) + rec(0x0b, 0, 0x6c) + rec(0x10, 1, 0x67) + rec(0x11, 0, 0xe4)
    ev = parse_evtp(blob); T(ev == {2: (1, 0x65), 0x0b: (0, 0x6c), 0x10: (1, 0x67), 0x11: (0, 0xe4)}, 'evtp: four records read')
    T(parse_evtp(blob[:20]) is None and parse_evtp(b'') is None and parse_evtp(None) is None, 'evtp: a cut table is not a table')
    T(parse_evtp(struct.pack('<II', 2, 1) + rec(2, 1, 0x65)) is None, 'evtp: another version is not read')
    T(parse_evtp(struct.pack('<II', 1, 2) + rec(5, 1, 0x65) + rec(2, 1, 0x65)) is None, 'evtp: unsorted keys are not the table the exe searches')
    tx, pb = start_facts([('evt', 0x34, 0, 66), ('evt', 0xe5, 0, 2)], ev)
    T(pb == [] and tx.count('as a task (player destroyed)') == 2 and 'key 0x2 flags 0x65' in tx and 'NO pause screen' not in tx, 'start: types 66 and 2 = key 2, task branch, pause screen')
    tx, pb = start_facts([('evt', 0x3a, 0, 130)], ev); T(pb == [] and 'NO pause screen (type bit 7)' in tx and 'as a task' in tx, 'start: type 130 = key 2 with bit 7: no pause screen')
    tx, pb = start_facts([('evt', 0x40, 0, 0x4b)], ev); T(len(pb) == 1 and 'lack bit 0' in pb[0] and 'DIRECT (the player SURVIVES)' in tx, 'start: a key without flag bit 0 FAILS (the direct branch)')
    tx, pb = start_facts([('evt', 0x40, 0, 0x91)], ev); T(len(pb) == 1 and 'lack bit 0' in pb[0] and 'NO pause screen' in tx, 'start: key 0x11 (flags 0xe4) fails too')
    tx, pb = start_facts([('evt', 0x41, 0, 3)], ev); T(len(pb) == 1 and 'the table does not have' in pb[0] and 'NOT IN evtp' in tx, 'start: a key that is not in the table FAILS')
    tx, pb = start_facts([('evt', 0x34, 0, 66), ('evt', 0x40, 0, 0x0b)], ev); T(len(pb) == 1 and tx.count(' | ') == 1, 'start: one bad naming program among good ones fails')
    tx, pb = start_facts([('evt', 0x34, 0, 66)], None); T(len(pb) == 1 and 'no `evtp` table was read' in pb[0], 'start: no table = failed, not passed')
    tx, pb = start_facts([], ev); T(tx == '-' and pb == [], 'start: an event no program names has nothing to check')
    print('movie_table selftest: %d checks, %d failures' % (n[0], bad[0])); return 1 if bad[0] else 0

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data'); ap.add_argument('--movies', action='append'); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    if not a.data: ap.error('--data DATA_TREE (or --selftest)')
    return table(a)
sys.exit(main())
