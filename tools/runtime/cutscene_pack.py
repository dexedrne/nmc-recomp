#!/usr/bin/env python3
"""cutscene_pack.py (task 50): classify every scene the Nobody May Cry mod replaced with its empty stub, and write the pack that
KHNMC_CUTSCENES=story serves in its place (runtime/cutscenes.c).

  cutscene_pack.py --data TREE --vanilla VANILLA_ARD_US --vanilla-objentry FILE --mod MOD_ROOT [--table OUT.tsv] [--pack OUT.khcs]
  cutscene_pack.py --check-pack PACK --vanilla VANILLA_ARD_US      every pack entry decodes and equals the vanilla entry, byte for byte
  cutscene_pack.py --selftest                                      the rules and the pack format on synthetic data (no game data)

TREE = the game's data tree as the port loads it (NMC applied: ard/us has the stubs).  VANILLA_ARD_US = the original PC game's
ard/us folder (the 289 area files the mod replaces or keeps).  VANILLA objentry = the original PC 00objentry.bin.  MOD_ROOT = the
mod's own files (SillyBillyMod1): only its map/ folder is read, for the rooms it rebuilt.
Output is analysis of the owner's own data (the table) and the owner's own data (the pack): never commit either.

A STUB is the mod's empty scene: the five commands project, setup, start, end frame, fade (event_refs.is_stub; the two eh03
703 / 704 entries carry one more volume command and have no original).  Every stub of TREE/ard/us is classified from the
commands of its ORIGINAL (the first type-22 entry of that name in the vanilla area file of the same name, as the game's lookup
exe 0x3a1f70 takes the first):

  KEEP-STUB no-original        no vanilla area file or no entry of that name (the mod's own rooms and names)
  KEEP-STUB movie              the scene starts a pre-rendered movie (command 0x44): the owner chose the in-engine scenes only
  KEEP-STUB unproven-command   a command type that no real scene the mod kept has (so this port never ran it with NMC's data)
  KEEP-STUB room-replaced      the scene plays in a room whose map the mod replaced (its own area or the map of its 0x03 command):
                               po (the Holo-Archives), the Atlantica slot (lm), the Pride Lands slot (lk), eh02 / eh03: the scene
                               was made for geometry and a story that are not there any more
  KEEP-STUB actor-changed      an actor (0x01, or a ReadAssets 0x38 entry) whose 00objentry row the mod changed in model, motion
                               set or object type (Beast -> a Roxas row, Donald -> BTLNPC helper, ...): the scene would show or
                               spawn something else than it was made for
  KEEP-STUB missing-file       a file the scene names (event_refs.Event.files: actors, motions, voices, effects, layouts, music,
                               sound banks, the map) is not in TREE
  SAFE                         none of the above.  An event entry holds no jump and no flag: those are sub-commands of the
                               AreaSettings record that names the event (tools/event_refs.py header), which stay NMC's; the
                               event player (exe 0x2c8000..0x2e0000) has no call to the progress setter 0x39afc0 and no direct
                               reference to the save block 0x1409a98b0..0x1409ba8b0 (implementation notes section 2)
  SAFE-AFTER-EDIT              none: no rule above is cured by editing a command (kept for the format: class 2 in the pack)

Each stub also gets its REACH from the story graph of tools/event_refs.py: started (a program that can run names it), named
(only programs the story never runs name it), theater (only the title menu's Theater replays it), none (no program names it).
The pack holds the SAFE scenes whose reach is not `none`; a scene no program names is never asked for and is not restored.

PACK format (little endian): "KHCS", u32 version 1, u32 count, u32 0; count * 24-byte records sorted by (area, name):
  char area[4] ("tt05"), char name[4] (the BAR entry name, NUL padded), u32 class (1 SAFE, 2 SAFE-AFTER-EDIT), u32 offset, u32 size,
  u32 FNV-1a of the bytes; then the entries' bytes (the original event entry, unchanged), each at a 16-byte aligned offset.
Exit status: 0 ok, 1 a check failed, 2 usage.  Python 3.6+, standard library.
"""
import argparse, collections, os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_refs as E

MOVIE = 0x44
PACK_MAGIC = b'KHCS'
CLASS_SAFE, CLASS_EDIT = 1, 2


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


def stub_like(d):
    """the mod's stub, also with music commands added (eh03 703 / 704)"""
    c, tail = E.ev_cmds(d)
    return tail >= 0 and [t for t, _ in c if t not in (0x2e, 0x2f, 0x30)] == [0x00, 0x0f, 0x10, 0x08, 0x12]


def objentry_rows(b):
    n = struct.unpack_from('<I', b, 4)[0]; out = {}
    for i in range(n):
        o = 8 + 0x60 * i
        if o + 0x60 > len(b): break
        out[struct.unpack_from('<I', b, o)[0]] = (b[o + 4], E.cstr(b[o + 8:o + 40]), E.cstr(b[o + 40:o + 72]))   # type, model, mset
    return out


def actor_ids(ev):
    return sorted({oid for oid, sl, nm in ev.actors})


def classify(ev_v, proven, changed_rows, exists, replaced_rooms=frozenset()):
    """the original's Event -> (class, detail).  exists(rel) -> bool"""
    types = {t for t, _ in ev_v.cmds}
    if ev_v.tail < 0: return 'KEEP-STUB', 'no-original (the vanilla entry does not decode)'
    if MOVIE in types: return 'KEEP-STUB', 'movie'
    rooms = {ev_v.area.lower()} | ({('%s%02d' % (ev_v.map[0], ev_v.map[1])).lower()} if ev_v.map else set())
    if rooms & set(replaced_rooms): return 'KEEP-STUB', 'room-replaced ' + ','.join(sorted(rooms & set(replaced_rooms)))
    odd = sorted(types - proven)
    if odd: return 'KEEP-STUB', 'unproven-command ' + ','.join('%#04x' % t for t in odd)
    ch = [oid for oid in actor_ids(ev_v) if oid in changed_rows]
    if ch: return 'KEEP-STUB', 'actor-changed ' + ','.join(str(x) for x in ch)
    miss = []
    for kind, rel in ev_v.files():
        if kind.endswith('-opt') or kind == 'layout': continue        # optional files; a layout is tried in two places (refs_of)
        if '/' not in rel or not exists(rel): miss.append(rel)
    lay = sorted(ev_v.layouts)
    for l in lay:
        if not (exists('event/layout/%s/%s.2ld' % (ev_v.tree.lang, l)) or exists('event/layout/%s.2ld' % l)): miss.append('event/layout/%s.2ld' % l)
    if miss: return 'KEEP-STUB', 'missing-file ' + ','.join(sorted(set(miss)))
    return 'SAFE', ''


def write_pack(path, items):
    """items: [(area, name, class, bytes)] -> the pack file"""
    items = sorted(items, key=lambda x: (x[0], x[1]))
    head = PACK_MAGIC + struct.pack('<III', 1, len(items), 0)
    off = 16 + 24 * len(items); off = (off + 15) & ~15; recs = []; body = []
    pos = off
    for area, name, cl, b in items:
        a = area.encode('latin1'); n = name.encode('latin1')
        if len(a) != 4 or not 1 <= len(n) <= 4: die('pack: bad key %s/%s' % (area, name))
        recs.append(a + n.ljust(4, b'\0') + struct.pack('<IIII', cl, pos, len(b), fnv1a(b)))
        pad = (-len(b)) % 16; body.append(b + b'\0' * pad); pos += len(b) + pad
    data = head + b''.join(recs)
    data += b'\0' * (off - len(data)) + b''.join(body)
    with open(path + '.tmp', 'wb') as f: f.write(data)
    os.replace(path + '.tmp', path)
    return len(data)


def read_pack(path):
    b = open(path, 'rb').read()
    if len(b) < 16 or b[:4] != PACK_MAGIC: die('pack %s: not a KHCS pack' % path)
    ver, n, z = struct.unpack_from('<III', b, 4)
    if ver != 1 or 16 + 24 * n > len(b): die('pack %s: version %d / count %d does not fit' % (path, ver, n))
    out = collections.OrderedDict()
    for i in range(n):
        r = b[16 + 24 * i:40 + 24 * i]; area = r[:4].decode('latin1'); name = r[4:8].rstrip(b'\0').decode('latin1')
        cl, off, size, h = struct.unpack_from('<IIII', r, 8)
        if off + size > len(b) or off % 16: die('pack %s: entry %s/%s out of range' % (path, area, name))
        d = b[off:off + size]
        if fnv1a(d) != h: die('pack %s: entry %s/%s checksum' % (path, area, name))
        out[(area, name)] = (cl, d)
    return out


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
    proven = set()
    for k, d in nmc.items():
        if not stub_like(d): proven |= {t for t, _ in E.ev_cmds(d)[0]}
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
            ev = E.Event(tree, area, name, van[k]); cl, det = classify(ev, proven, changed, exists, replaced); size = len(van[k])
        rows.append((area, name, cl, det.split(' ')[0] if det else '', det, reach, size))
        counts[(cl, det.split(' ')[0] if det else '')] += 1; reach_c[(cl, reach)] += 1
        if cl == 'SAFE' and reach != 'none': pack.append((area, name, CLASS_SAFE, van[k]))
    if a.table:
        with open(a.table, 'w') as f:
            f.write('area\tname\tclass\treason\tdetail\treach\tvanilla_bytes\n')
            for r in rows: f.write('%s\t%s\t%s\t%s\t%s\t%s\t%d\n' % r)
    print('stubs %d (proven command types: %d, objentry rows the mod changed: %d)' % (len(rows), len(proven), len(changed)))
    for (cl, why), n in sorted(counts.items()): print('  %-10s %-18s %4d' % (cl, why or '-', n))
    for cl in ('SAFE', 'KEEP-STUB'):
        print('  %s by reach: %s' % (cl, ' '.join('%s %d' % (r, reach_c[(cl, r)]) for r in ('started', 'named', 'theater', 'none'))))
    if a.pack:
        size = write_pack(a.pack, pack)
        back = read_pack(a.pack)
        bad = [k for k, (cl, d) in back.items() if van.get(k) != d]
        if len(back) != len(pack) or bad: die('pack read-back: %d entries, %d differ from the vanilla bytes' % (len(back), len(bad)))
        print('pack %s: %d scenes, %d bytes (read back: every entry equals its vanilla entry)' % (a.pack, len(pack), size))
    return 0


def check_pack(a):
    van = events_of(a.vanilla); back = read_pack(a.check_pack); bad = 0
    for k, (cl, d) in back.items():
        c, tail = E.ev_cmds(d)
        if tail < 0 or van.get(k) != d or cl not in (CLASS_SAFE, CLASS_EDIT): bad += 1; print('BAD %s/%s' % k)
    print('pack %s: %d entries, %d bad' % (a.check_pack, len(back), bad)); return 1 if bad else 0


# ---------------------------------------------------------------------------------------------------------- selftest (no game data)
def selftest():
    import tempfile
    ok = [0, 0]
    def T(c, what):
        ok[0] += 1
        if not c: ok[1] += 1; print('cutscene_pack selftest FAIL: %s' % what)
    def cmd(t, pl=b''): return struct.pack('<HH', 4 + len(pl), t) + pl
    stub = cmd(0, b'\0' * 20) + cmd(0x0f) + cmd(0x10, b'\0' * 4) + cmd(0x08, b'\x32\0\x12\0') + cmd(0x12, b'\1\0\0\0\1\0') + b'\0\0'
    T(stub_like(stub) and E.is_stub(stub), 'the stub shape')
    T(stub_like(stub[:-2] [:36] + cmd(0x2e, b'\0' * 8) + stub[36:]), 'stub with a volume command')
    T(not stub_like(stub[:-2][:36] + cmd(0x14, b'\0\0') + stub[36:]), 'stub with another command is a scene')

    class FakeTree:
        lang = 'us'; objentry = {0x10: ('H_EX500', 'H_EX500.mset', (0, 0, 0, 0)), 0x11: ('P_BB000', 'P_BB000.mset', (0, 0, 0, 0))}
    def scene(*extra):
        body = cmd(0, b'\0' * 6 + b'zz_event_001\0\0') + cmd(0x0f) + cmd(0x10, b'\0' * 4) + cmd(0x08, b'\x10\0\0\0')
        body += cmd(0x01, struct.pack('<HH', 0x10, 0) + b'h_ex500\0') + b''.join(extra)
        return E.Event(FakeTree(), 'zz00', '101', body + b'\0\0')
    files_all = lambda rel: True
    proven = {0x00, 0x0f, 0x10, 0x08, 0x01, 0x15, 0x12}
    T(classify(scene(), proven, set(), files_all) == ('SAFE', ''), 'a plain scene is SAFE')
    T(classify(scene(), proven, set(), files_all, {'zz00'})[1] == 'room-replaced zz00', 'a scene in a room the mod rebuilt keeps the stub')
    T(classify(scene(cmd(0x03, b'\x07\0zz')), proven | {0x03}, set(), files_all, {'zz07'})[1] == 'room-replaced zz07', 'a scene whose map the mod rebuilt keeps the stub')
    T(classify(scene(cmd(0x44, b'\0\0mv\0')), proven | {0x44}, set(), files_all)[1] == 'movie', 'a movie command keeps the stub')
    T(classify(scene(cmd(0x4e, b'\0\0')), proven, set(), files_all)[1].startswith('unproven-command 0x4e'), 'an unproven command keeps the stub')
    T(classify(scene(), proven, {0x10}, files_all)[1] == 'actor-changed 16', 'an actor whose row the mod changed keeps the stub')
    T(classify(scene(), proven, {0x11}, files_all) == ('SAFE', ''), 'a changed row the scene does not use')
    miss = classify(scene(), proven, set(), lambda rel: rel != 'obj/H_EX500.mdlx')
    T(miss[0] == 'KEEP-STUB' and miss[1] == 'missing-file obj/H_EX500.mdlx', 'a missing actor model keeps the stub')
    voice = cmd(0x23, struct.pack('<H', 1) + b'\0' * 6 + struct.pack('<HB', 5, 0) + b'zz101a\0'.ljust(29, b'\0'))
    T(classify(scene(voice), proven | {0x23}, set(), lambda rel: 'voice' not in rel)[1].startswith('missing-file voice/us/event/zz101a'), 'a missing voice keeps the stub')
    bad = scene(); bad.tail = -1
    T(classify(bad, proven, set(), files_all)[0] == 'KEEP-STUB', 'an original that does not decode keeps the stub')
    d = tempfile.mkdtemp(prefix='cutscene-pack-st-')
    p = os.path.join(d, 'p.khcs')
    items = [('tt05', '406', 1, b'\x01' * 30), ('al00', '101', 1, b'\x02' * 17), ('tt05', '12a', 2, b'')]
    write_pack(p, items); back = read_pack(p)
    T(list(back) == [('al00', '101'), ('tt05', '12a'), ('tt05', '406')], 'pack records sorted by key')
    T(back[('tt05', '406')] == (1, b'\x01' * 30) and back[('al00', '101')] == (1, b'\x02' * 17) and back[('tt05', '12a')] == (2, b''), 'pack round trip')
    raw = bytearray(open(p, 'rb').read()); raw[96] ^= 0xff          # 96 = the first data byte: 16 + 3 * 24 = 88, aligned to 16
    open(p, 'wb').write(bytes(raw))
    import subprocess
    r = subprocess.run([sys.executable, '-I', '-c', 'import sys; sys.path.insert(0, %r); import cutscene_pack as C; C.read_pack(%r)' % (os.path.dirname(os.path.abspath(__file__)), p)],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    T(r.returncode == 1 and b'checksum' in r.stderr, 'a damaged pack entry is a named error')
    for f in os.listdir(d): os.unlink(os.path.join(d, f))
    os.rmdir(d)
    print('cutscene_pack selftest: %d checks, %d failures' % (ok[0], ok[1]))
    return 1 if ok[1] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--data'); ap.add_argument('--vanilla'); ap.add_argument('--vanilla-objentry'); ap.add_argument('--mod')
    ap.add_argument('--table'); ap.add_argument('--pack'); ap.add_argument('--check-pack'); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    if a.check_pack:
        if not a.vanilla: ap.error('--check-pack needs --vanilla')
        return check_pack(a)
    if not (a.data and a.vanilla and a.vanilla_objentry and a.mod): ap.error('--data, --vanilla, --vanilla-objentry and --mod are needed')
    return run(a)


if __name__ == '__main__':
    sys.exit(main())
