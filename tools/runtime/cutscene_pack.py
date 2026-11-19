#!/usr/bin/env python3
"""cutscene_pack.py (task 50): classify every scene the Nobody May Cry mod replaced with its empty stub, and write the pack that
KHNMC_CUTSCENES=story serves in its place (runtime/cutscenes.c).

  cutscene_pack.py --data TREE --vanilla VANILLA_ARD_US --vanilla-objentry FILE --mod MOD_ROOT [--table OUT.tsv] [--pack OUT.khcs]
  cutscene_pack.py --theater --data TREE --vanilla VANILLA_ARD_US [--menu MENU.tsv] [--table INVENTORY.tsv] [--pack OUT.khtr]
  cutscene_pack.py --check-pack PACK --vanilla VANILLA_ARD_US      every pack entry decodes and equals the vanilla entry, byte for byte
  cutscene_pack.py --selftest                                      the rules and the pack format on synthetic data (no game data)

TREE = the game's data tree as the port loads it (NMC applied: ard/us has the stubs).  VANILLA_ARD_US = the original PC game's
ard/us folder (the 289 area files the mod replaces or keeps).  VANILLA objentry = the original PC 00objentry.bin.  MOD_ROOT = the
mod's own files (SillyBillyMod1): only its map/ folder is read, for the rooms it rebuilt.
Output is analysis of the owner's own data (the table) and the owner's own data (the pack): never commit either.

Story mode admits only the command forms reviewed in implementation notes and implementation notes.
Isolated Theater admits exact vanilla originals whose required assets are present. Each mode
pins its own pack including the sorted inventory, asset alternatives and exact stub templates.
"""
import hashlib, argparse, collections, os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_refs as E

MOVIE = 0x44
PACK_MAGIC = b'KHCS'
CLASS_SAFE, CLASS_EDIT, CLASS_THEATER = 1, 2, 3
THEATER_MAGIC = b'KHTR'
THEATER_SHA = '1420a4f2a81ec6ee7545a0fbd95b3bcfa2be12a64c968aface97c2507b9a9623'
THEATER_COUNT = 612


def die(msg):
    sys.stderr.write('cutscene_pack: ERROR: %s\n' % msg); sys.exit(1)


def fnv1a(b):
    h = 0x811c9dc5
    for c in b: h = ((h ^ c) * 0x01000193) & 0xffffffff
    return h


def events_of(folder):
    """(area, name) -> bytes: the first type-22 entry of each name of each .ard of the folder"""
    out = {}
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith('.ard'): continue
        for t, nm, d in E.bar(open(os.path.join(folder, f), 'rb').read()):
            if t == 22 and (f[:-4].lower(), nm) not in out: out[(f[:-4].lower(), nm)] = d
    return out


# SHA-256 of the complete BAR entries, including the two-byte terminator.
STUB_HASH = {56: '3e5404039aef14fe8ca2964e500f48fb98a2a8c3b7fa4a114292e42e5f1b80fa', 68: 'c3c55e752514bc643952cbc2d55e7b25d83991024650e2160bf8840616630c21'}
REVIEWED_SHA = '155fa7e650a5ba0fed00bfc49555aea940446ca1a11b3645d5b314521ba90ef6'
REVIEWED_COUNT = 0

def stub_like(data):
    return hashlib.sha256(data).hexdigest() == STUB_HASH.get(len(data))


# PC load table 2cc31c (type - 1), frame table 2cbc2c (type - 2).
# These records only supply the event end/start, camera key/interval or separator.
# No ReadAssets subtypes have been approved. Only its empty list is allowed.
# (decode/relocate, load, frame dispatch RVAs, exact payload length).
# None in a dispatch column means outside its index range; a None length is validated below.
PRESENTATION = {
    0x00: (0x2c9025, None, None, 20),          # exact NMC project only
    0x03: (0x2c914e, 0x2cc2b8, 0x2cbbed, 6), # same-room map descriptor
    0x06: (0x2cb0c5, 0x2cc2b8, 0x2cb781, 8), # camera interval, 2d2660
    0x08: (0x2cb0c5, 0x2cc232, 0x2cbbed, 4), # timeline end, 2d8dd0
    0x0f: (0x2cb0c5, 0x2cc239, 0x2cbbed, 0), # NMC setup, 2d8fe0
    0x10: (0x2cb0c5, 0x2cc243, 0x2cbbed, 4), # timeline start, 2d9020
    0x12: (0x2cb0c5, 0x2cc2b8, 0x2cb7ee, 6), # fade, 2da730
    0x13: (0x2c9575, 0x2cc24d, 0x2cbbed, None), # camera track, eight channels of 16-byte keys
    0x14: (0x2cb0c5, 0x2cc2b8, 0x2cbbed, 2), # advances all three cursors
    0x22: (0x2cb0c5, 0x2cc2b8, 0x2cba1c, 4), # render-layer fade, 2dab00
    0x24: (0x2cb0b8, 0x2cc277, 0x2cbbed, 8), # empty read-assets record
}

def reviewed_command(t, pl):
    if t not in PRESENTATION or (PRESENTATION[t][3] is not None and len(pl) != PRESENTATION[t][3]):
        return False
    if t == 0x13:
        if len(pl) < 36 or (len(pl) - 36) % 16:
            return False
        slot, *channels, pad = struct.unpack_from('<18H', pl)
        keys = (len(pl) - 36) // 16
        return slot < 128 and pad == 0 and all(
            count > 0 and offset + count <= keys
            for offset, count in zip(channels[::2], channels[1::2]))
    if t == 0x00:
        # The name and project parameters select other player paths.
        return hashlib.sha256(pl).hexdigest() == '16a128eeacd36960d853aededfca52edf714c7650862a2c143f236d3a4f406ab'
    if t == 0x03:
        room, world, pad = struct.unpack('<H2sH', pl)
        return room < 100 and world.isalpha() and world.islower() and pad == 0
    if t == 0x22:
        frame, mode = struct.unpack('<HH', pl)
        return frame <= 0x7fff and mode in (0, 1)
    if t == 0x24:
        return pl == bytes.fromhex('0000008000000000')
    if t == 0x12:
        frame, duration, mode = struct.unpack('<3H', pl)
        return mode in (1, 4) or (frame <= 0x7fff and duration <= 0x7fff and mode < 8)
    if t == 0x06:
        camera, start, stop, pad = struct.unpack('<4H', pl)
        return camera < 128 and pad == 0 and start <= stop <= 0x7fff
    return True


def objentry_rows(b):
    n = struct.unpack_from('<I', b, 4)[0]; out = {}
    for i in range(n):
        o = 8 + 0x60 * i
        if o + 0x60 > len(b): break
        out[struct.unpack_from('<I', b, o)[0]] = (b[o + 4], E.cstr(b[o + 8:o + 40]), E.cstr(b[o + 40:o + 72]))   # type, model, mset
    return out


def actor_ids(ev):
    return sorted({oid for oid, sl, nm in ev.actors})


def asset_groups(ev):
    paths = {p for kind,p in ev.files() if not kind.endswith('-opt') and kind != 'layout'}
    for name in ev.layouts:
        paths.add('event/layout/%s/%s.2ld|event/layout/%s.2ld' % (ev.tree.lang, name, name))
    return sorted(paths)


def classify(ev_v, changed_rows, exists, replaced_rooms=frozenset()):
    """the original's Event -> (class, detail).  exists(rel) -> bool"""
    types = {t for t, _ in ev_v.cmds}
    if ev_v.tail != 2: return 'KEEP-STUB', 'no-original (the vanilla entry does not decode)'
    if MOVIE in types: return 'KEEP-STUB', 'movie'
    rooms = {ev_v.area.lower()} | ({('%s%02d' % (ev_v.map[0], ev_v.map[1])).lower()} if ev_v.map else set())
    if rooms & set(replaced_rooms): return 'KEEP-STUB', 'room-replaced ' + ','.join(sorted(rooms & set(replaced_rooms)))
    odd = sorted({t for t, pl in ev_v.cmds if not reviewed_command(t, pl)})
    if odd: return 'KEEP-STUB', 'unproven-command ' + ','.join('%#04x' % t for t in odd)
    camera_slots = {struct.unpack_from('<H', pl)[0] for t, pl in ev_v.cmds if t == 0x13}
    for t, pl in ev_v.cmds:
        if t == 0x03:
            room, world, _ = struct.unpack('<H2sH', pl)
            if ev_v.area != '%s%02d' % (world.decode('ascii'), room):
                return 'KEEP-STUB', 'different-event-map'
        if t == 0x06:
            slot = struct.unpack_from('<H', pl)[0]
            if (slot or camera_slots) and slot not in camera_slots:
                return 'KEEP-STUB', 'undefined-camera %d' % slot
    ch = [oid for oid in actor_ids(ev_v) if oid in changed_rows]
    if ch: return 'KEEP-STUB', 'actor-changed ' + ','.join(str(x) for x in ch)
    miss = [group for group in asset_groups(ev_v) if not any('/' in path and exists(path) for path in group.split('|'))]
    if miss: return 'KEEP-STUB', 'missing-file ' + ','.join(sorted(set(miss)))
    return 'SAFE', ''


def pack_bytes(items, inventory, plain, music, magic=PACK_MAGIC):
    # Paths are NUL-terminated groups; | offers alternative locations for one file.
    records, body = [], bytearray()
    start = (16 + 24 * len(items) + 15) & ~15
    for area, name, cl, data, assets in sorted(items):
        key = area.encode('ascii') + name.encode('ascii').ljust(4, b'\0')
        if len(key) != 8: die('cutscenes-pack: invalid key')
        records.append(key + struct.pack('<4I', cl, start + len(body), len(data), fnv1a(data)))
        body.extend(data)
        for group in assets: body.extend(group.encode('ascii') + b'\0')
        body.append(0)
        body.extend(bytes(-len(body) % 16))
    head = magic + struct.pack('<3I', 2, len(items), start + len(body))
    catalog = b''.join(a.encode('ascii') + n.encode('ascii').ljust(4, b'\0') for a,n in sorted(inventory))
    return (head + b''.join(records)).ljust(start, b'\0') + body + plain + music + catalog


def decode_pack(raw, count, digest, magic=PACK_MAGIC):
    if len(raw) < 16 or raw[:4] != magic: die('cutscenes-pack: bad header')
    ver, n, end = struct.unpack_from('<3I', raw, 4)
    if ver != 2 or count != n: die('cutscenes-pack: version or reviewed count mismatch')
    if hashlib.sha256(raw).hexdigest() != digest: die('cutscenes-pack: reviewed manifest mismatch')
    if end < 16 + 24*n or end+124+8 > len(raw) or (len(raw)-end-124) % 8:
        die('cutscenes-pack: incomplete catalog')
    keys = [bytes(raw[i:i+8]) for i in range(end+124, len(raw), 8)]
    if keys != sorted(set(keys)): die('cutscenes-pack: duplicate or unsorted catalog')
    out = {}
    previous = b''
    for i in range(n):
        a, k, cl, off, size, h = struct.unpack_from('<4s4s4I', raw, 16 + 24*i)
        if a+k <= previous or a+k not in keys: die('cutscenes-pack: invalid reviewed key')
        if cl not in ((CLASS_THEATER,) if magic == THEATER_MAGIC else (CLASS_SAFE, CLASS_EDIT)): die('cutscenes-pack: invalid class')
        previous = a+k
        if off % 16 or off < 16+24*n or off+size >= end: die('cutscenes-pack: invalid bounds')
        data = raw[off:off+size]
        if fnv1a(data) != h: die('cutscenes-pack: entry checksum')
        out[(a.decode(), k.rstrip(b'\0').decode())] = cl, data
    return out

def read_pack(path, theater=False):
    with open(path, 'rb') as f:
        raw = f.read()
    if theater: return decode_pack(raw, THEATER_COUNT, THEATER_SHA, THEATER_MAGIC)
    return decode_pack(raw, REVIEWED_COUNT, REVIEWED_SHA)


def theater_pack(a):
    tree = E.Tree(a.data)
    graph = E.Graph(tree)
    vanilla = events_of(a.vanilla)
    nmc = events_of(os.path.join(a.data, 'ard', 'us'))
    stubs = {k: data for k, data in nmc.items() if stub_like(data)}
    def available(path):
        rel = tree.find(path)
        if not rel: return False
        full = os.path.join(tree.root, rel)
        return os.path.isfile(full) and os.path.getsize(full) > 0

    items, reasons = [], {}
    for key in sorted(stubs):
        data = vanilla.get(key)
        if not data or stub_like(data):
            reasons[key] = 'no-original'
            continue
        ev = E.Event(tree, *key, data)
        if ev.tail != 2:
            reasons[key] = 'malformed-original'
            continue
        groups = sorted(set(asset_groups(ev)) | {'zmovie/en/%s.mp4' % m for m in ev.movies})
        missing = [group for group in groups if not any(available(path) for path in group.split('|'))]
        if missing:
            reasons[key] = 'missing ' + ','.join(missing)
            continue
        items.append((*key, CLASS_THEATER, data, groups))
    if a.table:
        with open(a.table, 'w') as f:
            print('area', 'event', 'decision', 'menu_programs', sep='\t', file=f)
            for key in sorted(stubs):
                print(*key, reasons.get(key, 'packed'), ','.join('%s/%d' % (w,p) for w,p,_ in graph.theater.get(key, [])), sep='\t', file=f)
    if a.menu:
        by_program = collections.defaultdict(set)
        for key, refs in graph.theater.items():
            for ref in refs:
                by_program[ref].add(key)
        with open(a.menu, 'w') as f:
            print('chapter', 'heading_id', 'row', 'message_id', 'world', 'variant', 'program', 'progress_flag', 'events', sep='\t', file=f)
            chapter = row = heading = 0
            for kind, mid, w, pa, pb, flag in tree.eventviewer:
                if kind != 1:
                    chapter += 1
                    row, heading = 0, mid
                    for variant, program in zip('AB', (pa,pb)):
                        print(chapter, heading, row, mid, E.WORLDS[w], variant, program, flag, 'heading', sep='\t', file=f)
                    continue
                row += 1
                world = E.WORLDS[w]
                for variant, program in zip('AB', (pa,pb)):
                    keys = sorted(by_program[world, program, mid])
                    entries = ['%s/%s=%s' % (*key, reasons.get(key, 'packed' if key in stubs else 'retained' if key in nmc else 'absent')) for key in keys]
                    print(chapter, heading, row, mid, world, variant, program, flag, ';'.join(entries) or 'unmapped', sep='\t', file=f)
    templates = {len(data): data for data in stubs.values()}
    raw = pack_bytes(items, stubs, templates[56], templates[68], THEATER_MAGIC)
    print('Theater: %d scenes, %d bytes, SHA-256 %s' % (len(items), len(raw), hashlib.sha256(raw).hexdigest()))
    back = decode_pack(raw, THEATER_COUNT, THEATER_SHA, THEATER_MAGIC)
    if any(data != vanilla[key] for key, (_, data) in back.items()):
        die('theater original mismatch')
    if a.pack:
        with open(a.pack + '.tmp', 'wb') as f: f.write(raw)
        os.replace(a.pack + '.tmp', a.pack)
    return 0


def run(a):
    tree = E.Tree(a.data)
    nmc = events_of(os.path.join(a.data, 'ard', 'us'))
    van = events_of(a.vanilla)
    graph = E.Graph(tree)
    vrows = objentry_rows(open(a.vanilla_objentry, 'rb').read())
    mapdir = os.path.join(a.mod, 'map')
    if not os.path.isdir(mapdir): die('%s has no map/ folder (the mod root is needed for the rooms it rebuilt)' % a.mod)
    replaced = {f[:-4].lower() for f in os.listdir(mapdir) if f.lower().endswith('.map')}
    nrows = objentry_rows(tree._read('00objentry.bin'))
    changed = {k for k in nrows if k in vrows and nrows[k] != vrows[k]}
    exists = lambda rel: tree.find(rel) is not None
    rows = []; counts = collections.Counter(); reach_c = collections.Counter(); pack = []
    for k in sorted(nmc):
        if not stub_like(nmc[k]): continue
        area, name = k
        if area not in tree.areas: die('area %s of the tree is not in event_refs\' tree' % area)
        named = graph.named.get(k); started = k in graph.started; theater = k in graph.theater
        reach = 'started' if started else 'named' if named else 'theater' if theater else 'none'
        if k not in van or stub_like(van[k]):
            cl, det = 'KEEP-STUB', 'no-original'; size = 0
        else:
            size = len(van[k])
            try:
                ev = E.Event(tree, area, name, van[k])
                cl, det = classify(ev, changed, exists, replaced)
            except struct.error:
                cl, det = 'KEEP-STUB', 'malformed-command'
            if len(nmc[k]) == 68:
                cl, det = 'KEEP-STUB', 'nmc-volume-stub' 
        rows.append((area, name, cl, det.split(' ')[0] if det else '', det, reach, size))
        counts[(cl, det.split(' ')[0] if det else '')] += 1; reach_c[(cl, reach)] += 1
        if cl == 'SAFE' and reach != 'none': pack.append((area, name, CLASS_SAFE, van[k], asset_groups(ev)))
    if a.table:
        with open(a.table, 'w') as f:
            f.write('area\tname\tclass\treason\tdetail\treach\tvanilla_bytes\n')
            for r in rows: f.write('%s\t%s\t%s\t%s\t%s\t%s\t%d\n' % r)
    print('exact stubs %d; changed object rows %d' % (len(rows), len(changed)))
    for (cl, why), n in sorted(counts.items()): print('  %-10s %-18s %4d' % (cl, why or '-', n))
    for cl in ('SAFE', 'KEEP-STUB'):
        print('  %s by reach: %s' % (cl, ' '.join('%s %d' % (r, reach_c[(cl, r)]) for r in ('started', 'named', 'theater', 'none'))))
    if a.pack:
        templates = {len(d): d for d in nmc.values() if stub_like(d)}
        if set(templates) != {56, 68}: die('cutscenes-pack: missing reviewed NMC stub')
        raw = pack_bytes(pack, [r[:2] for r in rows], templates[56], templates[68])
        back = decode_pack(raw, REVIEWED_COUNT, REVIEWED_SHA)
        if any(data != van[k] for k, (cl, data) in back.items()):
            die('cutscenes-pack: original data mismatch')
        with open(a.pack + '.tmp', 'wb') as f: f.write(raw)
        os.replace(a.pack + '.tmp', a.pack)
        print('reviewed pack: %d scenes, %d bytes' % (len(pack), len(raw)))
    return 0


def check_pack(a):
    van = events_of(a.vanilla); back = read_pack(a.check_pack, a.theater); bad = 0
    for k, (cl, d) in back.items():
        c, tail = E.ev_cmds(d)
        if tail < 0 or van.get(k) != d or cl not in (CLASS_SAFE, CLASS_EDIT, CLASS_THEATER): bad += 1; print('BAD %s/%s' % k)
    print('pack %s: %d entries, %d bad' % (a.check_pack, len(back), bad)); return 1 if bad else 0


# ---------------------------------------------------------------------------------------------------------- selftest (no game data)
def selftest():
    import contextlib, io
    tests = 0
    for t in range(256):
        if t not in PRESENTATION:
            assert not reviewed_command(t, bytes(8))
            tests += 1
    for frame, mode, allowed in ((0, 0, True), (32767, 1, True), (32768, 0, False), (0, 2, False)):
        assert reviewed_command(0x22, struct.pack('<HH', frame, mode)) == allowed
    for mode in range(8):
        assert reviewed_command(0x12, struct.pack('<3H', 1, 30, mode))
    assert not reviewed_command(0x12, struct.pack('<3H', 0x8000, 30, 7))
    assert not reviewed_command(0x12, struct.pack('<3H', 1, 0x8000, 7))
    for mode in (8, 0xffff):
        assert not reviewed_command(0x12, struct.pack('<3H', 1, 30, mode))
    camera = struct.pack('<18H', 127, *([0, 1] * 8), 0) + bytes(16)
    assert reviewed_command(0x13, camera)
    assert not reviewed_command(0x13, camera[:-1])
    assert not reviewed_command(0x13, b'\x80\0' + camera[2:])
    assert not reviewed_command(0x13, camera[:2] + b'\x01\0' + camera[4:])
    for arg in (100, 101):
        assert not reviewed_command(0x51, struct.pack('<HHI', 0, 0, arg))
    empty = bytes.fromhex('0000008000000000')
    assert reviewed_command(0x24, empty)
    assert not reviewed_command(0x24, bytes([1]) + empty[1:])
    for sub in (0x25, 0x33, 0x4d, 0xff):
        assert not reviewed_command(0x24, empty + struct.pack('<HH', sub, 4))
    class Fixture:
        tail = 2
        area = 'aa00'
        map = None
        layouts = set()
        actors = []
        cmds = [(0x08, bytes(4))]
        def files(self): return [('map', 'map/aa00.map')]
    fixture = Fixture()
    def result():
        return classify(fixture, set(), lambda path: True)[0]
    assert result() == 'SAFE'
    # H50C observed exit-table bits survive both completion and skip for these project names.
    for name in (b'he_event_301', b'he_event_302'):
        fixture.cmds = [(0x00, struct.pack('<6x14s', name))]
        assert result() == 'KEEP-STUB', name
    fixture.cmds = [(0x03, struct.pack('<H2sH', 0, b'aa', 0))]
    assert result() == 'SAFE'
    fixture.cmds.append((0x03, struct.pack('<H2sH', 1, b'aa', 0)))
    assert result() == 'KEEP-STUB'
    assert not reviewed_command(0x03, struct.pack('<H2sH', 0, b'aa', 1))
    interval = struct.pack('<4H', 127, 0, 30, 0)
    fixture.cmds = [(0x06, interval)]
    assert result() == 'KEEP-STUB'
    fixture.cmds.append((0x13, camera))
    assert result() == 'SAFE'
    fixture.cmds[0] = (0x06, struct.pack('<4H', 0, 0, 30, 0))
    assert result() == 'KEEP-STUB'
    assert not reviewed_command(0x06, struct.pack('<4H', 128, 0, 30, 0))
    for t in (0x33, 0x51, 0x4d, 0x2e, 0x2f, 0x30, 0xffff):
        fixture.cmds = [(t, bytes(8))]
        assert result() == 'KEEP-STUB', hex(t)
    fixture.cmds = [(0x24, empty + struct.pack('<HH', 0x26, 4))]
    assert result() == 'KEEP-STUB'
    fixture.cmds = [(0x08, bytes(4))]
    assert classify(fixture, set(), lambda path: False)[1].startswith('missing-file')
    assert classify(fixture, set(), lambda p: True, {'aa00'})[1].startswith('room-replaced')
    fixture.actors = [(42, 0, 'actor')]
    assert classify(fixture, {42}, lambda path: True)[1].startswith('actor-changed')
    fixture.cmds = [(MOVIE, b'')]
    assert classify(fixture, set(), lambda path: True)[1] == 'movie'
    key = ('aa00','101')
    entry = b'123456'
    raw = pack_bytes([(*key, 1, entry, ['map/aa00.map'])], [key], bytes(56), bytes(68))
    digest = hashlib.sha256(raw).hexdigest()
    assert decode_pack(raw, 1, digest)[key] == (1, entry)
    viewer = pack_bytes([(*key, CLASS_THEATER, entry, [])], [key], bytes(56), bytes(68), THEATER_MAGIC)
    viewer_sha = hashlib.sha256(viewer).hexdigest()
    assert decode_pack(viewer, 1, viewer_sha, THEATER_MAGIC)[key] == (CLASS_THEATER, entry)
    for data, sha, magic in ((viewer, viewer_sha, PACK_MAGIC), (raw, digest, THEATER_MAGIC),
                             (viewer, digest, THEATER_MAGIC)):
        with contextlib.redirect_stderr(io.StringIO()):
            try: decode_pack(data, 1, sha, magic)
            except SystemExit: continue
        raise AssertionError('pack format or SHA confusion')
    for i in (8, 16, 24, 48, len(raw)-1):
        mutated = bytearray(raw); mutated[i] ^= 1
        with contextlib.redirect_stderr(io.StringIO()):
            try: decode_pack(mutated, 1, digest)
            except SystemExit: continue
        raise AssertionError('cutscenes-pack accepted mutation at %d' % i)
    print('cutscene_pack: rejected %d unknown opcode values; operand, subcommand and manifest mutations passed' % tests)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data'); ap.add_argument('--vanilla'); ap.add_argument('--vanilla-objentry'); ap.add_argument('--mod')
    ap.add_argument('--table'); ap.add_argument('--pack'); ap.add_argument('--check-pack'); ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--theater', action='store_true', help='build or verify the isolated Theater pack')
    ap.add_argument('--menu', help='write the Theater chapter, row and variant inventory')
    a = ap.parse_args()
    if a.selftest: return selftest()
    if a.check_pack:
        if not a.vanilla: ap.error('--check-pack needs --vanilla')
        return check_pack(a)
    if a.theater:
        if not (a.data and a.vanilla): ap.error('--theater needs --data and --vanilla')
        return theater_pack(a)
    if not (a.data and a.vanilla and a.vanilla_objentry and a.mod): ap.error('--data, --vanilla, --vanilla-objentry and --mod are needed')
    return run(a)


if __name__ == '__main__':
    sys.exit(main())
