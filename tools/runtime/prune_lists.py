#!/usr/bin/env python3
"""prune_lists.py : the files of a KH2 + Nobody May Cry data tree that the game can no longer load because the cutscenes that
named them cannot start (task 32).  Writes two manifests of relative names and NEVER touches the tree (it only reads it).

  prune_lists.py --data DATA_TREE --out-a prune-A.list --out-b prune-B.list [--out-actors FILE] [--exe kh2fm.exe] [--keep-opened FILELOG ...]

Profile A "as shipped": nothing that an event some area program still NAMES could ask for is listed (the widest reading of "can
  still start": tools/event_refs.py's classes `reachable` and `named-never-run`; only events no program names are treated as
  gone).  No change of behaviour.
Profile B "no cutscenes": every event is treated as the mod's empty stub (the runtime's KHNMC_EVENTS=stub), so everything only
  events ask for is listed.
What can be listed at all (the candidates), and why nothing else can load it:
  anm/**                  the only code that builds an anm/ name is the event player's (exe 0x2d1a70, 0x2d1e30); it takes the name
                          from an event's commands.  anm/fm/** is only used in the Japanese regions (0, 7); this port is English.
  event/** and remastered/event/**        named only by exe 0x2d9130 / 0x2d9490 (effects) and 0x2dae30 / 0x2dae90 / 0x2daf90
                          (layouts), all inside the event player.
  voice/<lang>/event/**   the path "voice/%s/event/" (0x1405b9020) is built only by exe 0x2d7d00 / 0x2d7e80 (and the two fixed names
                          tt_titleL / tt_titleR, which are kept).  That does not cover a sound that an ORIGIN entry names by its base
                          name (see ORIGIN SOUND ENTRIES below): those are kept.
  obj/H_* and remastered/obj/H_*          the high-detail actors are NOT in the two manifests.  An object is loaded by its objentry
                          id, and ids also come from places this tool cannot read exhaustively (constants in the exe's code, the
                          system tables).  `--out-actors FILE` writes the CANDIDATES under profile B instead: the H_ objects whose id
                          no spawn file of any area holds, no other object attaches, no script hands to one of its object-by-id
                          calls (trap_sysobj_appear, trap_obj_search_by_entry, trap_obj_count_entry, trap_enemy_summon_entry, the
                          mod's LoadObjentry / SpawnObjentry), and whose name stands in neither the exe nor a mission / area
                          file.  That list is UNPROVEN: delete from it only after a play-through with KHNMC_DATA_DENY_FATAL=1.
  gumibattle/**, remastered/gumibattle/**, voice/<lang>/gumibattle/**     the gummi ship's stages.  Every name under gumibattle/ is
                          built by the gummi task's code; this port ends a run that starts that task with the named fatal
                          `gummi-gate-contract` (runtime/mod_hooks.c), so none of it can be read.
  NOT candidates: the gummi garage (gumimenu/, gumiblock/, obj/G_*, 50gumientry.bin ...): its menu is a separate mode that the
  port does not gate, so it is not shown dead here.
The 3,616 events outside the area files (reaction commands in obj/*.a.<lang>, limits, mission banners, game over) are read too:
whatever they name is kept in both profiles (they name no files in this tree; the tool checks instead of assuming).
The Theater (eventviewer.bar; event_refs.py Graph.theater) replays scenes by running area programs: every event a Theater program
names is kept in profile A (they are all named by a program, so this changes nothing today; it is spelled out so that a narrower A
cannot drop them).  Profile B stubs them like every other event: the Theater then replays nothing.
ORIGIN SOUND ENTRIES (verification of task 32): a BAR's sound entry can be the 16-byte tag "ORIGIN" plus a name ("voice/wm/us/
  wm30101sr", "se/obj/se304", "voice/battle/us/<name>").  exe 0x3a64f0 (tag 0x1405b4b24) resolves it by looking the name's last part
  + ".win32.scd" up in a heap's list of registered sound resources (0x12db50: a name compare over the list at +0x670; it opens no
  file itself); which loader registers e.g. the 58 world-map voices voice/us/event/wm3*.win32.scd was not traced.  So nothing an
  ORIGIN entry of data that stays can name is shown dead: every candidate whose base name is such a name is KEPT, in both profiles,
  as a fixed point (an ORIGIN entry of a file this tool lists does not count; one of a file it keeps does).  The libretto files
  libretto/<lang>/wm*.bar (world-map dialogue, loaded by 0x38f270 'libretto/%s/wm%d.bar') hold the only such entries that hit a
  candidate today: the 58 files voice/us/event/wm3*.win32.scd.
Sizes: MiB = 2^20 bytes in the per-folder lines, GB = 10^9 bytes, both given in the first line.
MUSIC IS NEVER LISTED (task 32, item 6a): nothing under bgm/ or vagstream/, no sound-info table, nothing an event, an area
program or a room's table names as a music track.  `add()` refuses such a name with a named error, so a later change of the rules
above cannot list one by accident; the header of both manifests says so.
`--keep-opened FILELOG` (repeatable): file logs of real runs (KHNMC_FILELOG lines `HIT <source> <size> <path>`); a candidate that
a run really opened and that this tool would list is a contradiction: reported and the tool exits 1.
Python 3.6+, standard library, plus tools/event_refs.py and tools/bdxscan.py beside it.
"""
import argparse, collections, os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_refs as E

MUSIC_PREFIXES = ('bgm/', 'vagstream/')
MUSIC_FILES = ('12soundinfo.bar',)


class Refused(Exception): pass


def is_music(rel, music_names):
    r = rel.lower()
    return r.startswith(MUSIC_PREFIXES) or ('/' + r).find('/bgm/') >= 0 or ('/' + r).find('/vagstream/') >= 0 or \
        r in MUSIC_FILES or r.startswith(tuple('remastered/' + p for p in MUSIC_PREFIXES)) or r in music_names


class Manifest:
    def __init__(self, tree, music_names):
        self.tree = tree; self.music = music_names; self.files = collections.OrderedDict(); self.refused = 0

    def add(self, rel, why):
        """one FILE (relative name as on disk).  A music file is refused, loudly."""
        if is_music(rel, self.music):
            self.refused += 1
            raise Refused('prune_lists: REFUSED: %s is music (bgm/, vagstream/, a sound table or a named track) and is never removable; rule: %s' % (rel, why))
        p = os.path.join(self.tree.root, rel)
        if not os.path.isfile(p): raise Refused('prune_lists: REFUSED: %s is not a file of the tree; rule: %s' % (rel, why))
        self.files[rel] = os.path.getsize(p)

    def add_tree(self, rel, why):
        p = os.path.join(self.tree.root, rel)
        if os.path.isfile(p): self.add(rel, why); return
        for dp, dn, fn in os.walk(p, followlinks=True):
            dn.sort()
            for f in sorted(fn): self.add(os.path.relpath(os.path.join(dp, f), self.tree.root), why)


ORIGIN_TAG = b'ORIGIN\x00'       # exe 0x1405b4b24, compared over 7 bytes; the name follows at +0x10 (0x3a64f0: lea rbx,[rsi+0x10])


def origin_key(name):
    """the registry name exe 0x3a64f0 looks an ORIGIN entry up by: the name's last part + ".win32.scd" (0x1405c43a0), lower case"""
    return name.replace('\\', '/').rsplit('/', 1)[-1].lower() + '.win32.scd'


def origin_index(tree, roots=None):
    """every ORIGIN sound entry of every BAR file of the tree (nested BARs included, by a byte scan: wider than the entry table)
    -> {lower-case relative name of the file: set of origin_key()}"""
    out = {}
    tops = roots if roots is not None else sorted(os.listdir(tree.root))
    for top in tops:
        p = os.path.join(tree.root, top)
        files = [p] if os.path.isfile(p) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(p, followlinks=True) for f in fn)
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    if fh.read(4) != b'BAR\x01': continue
                b = open(f, 'rb').read()
            except OSError: continue
            i = b.find(ORIGIN_TAG); keys = set()
            while i >= 0:
                nm = b[i + 0x10:i + 0x50].split(b'\0')[0].decode('latin1')
                if nm: keys.add(origin_key(nm))
                i = b.find(ORIGIN_TAG, i + 1)
            if keys: out[os.path.relpath(f, tree.root).replace(os.sep, '/').lower()] = keys
    return out


def origin_base(rel):
    """the file whose base name decides for a listed name: the file itself, or for remastered/<file>/<texture> that <file>"""
    return (os.path.dirname(rel[len('remastered/'):]) if rel.lower().startswith('remastered/') else rel).rsplit('/', 1)[-1].lower()


def keep_origin_named(m, index):
    """drop from manifest m every file an ORIGIN entry of a file m does NOT list names (fixed point).
    Returns {kept rel: sorted naming files}"""
    kept = {}
    while True:
        listed = {r.lower() for r in m.files}
        live = collections.defaultdict(set)
        for f, keys in index.items():
            if f in listed: continue
            for k in keys: live[k].add(f)
        drop = [r for r in m.files if origin_base(r) in live and r not in kept]
        if not drop: return kept
        for r in drop:
            kept[r] = sorted(live[origin_base(r)])
            del m.files[r]


def walk_files(tree, rel):
    p = os.path.join(tree.root, rel)
    if not os.path.isdir(p): return
    for dp, dn, fn in os.walk(p, followlinks=True):
        dn.sort()
        for f in sorted(fn): yield os.path.relpath(os.path.join(dp, f), tree.root)


def spawn_entities(e):
    """the objentry ids of one spawn file, or None when the file does not parse to its exact end.  Layout, fitted on the tree (every
    one of the 2,479 single-point files without walk paths has exactly this size): u32 type, u32 points; per point a 0x2c-byte header
    (u8 type, u8 flag, u16 id, then u16 counts: entities, event activators, walk paths, return parameters, signals), entities
    (0x40 bytes, objentry id first), event activators (0x40), walk paths (8 bytes with the point count at +2, then 12 per point),
    return parameters (0x10), signals (4)."""
    if len(e) < 8: return None
    cnt = struct.unpack_from('<I', e, 4)[0]; p = 8; ids = []
    for _ in range(cnt):
        if p + 0x2c > len(e): return None
        ne, na, nw, nr, ns = struct.unpack_from('<5H', e, p + 4); p += 0x2c
        if p + 0x40 * ne > len(e): return None
        ids += [struct.unpack_from('<I', e, p + 0x40 * k)[0] for k in range(ne)]
        p += 0x40 * (ne + na)
        for _w in range(nw):
            if p + 8 > len(e): return None
            p += 8 + 12 * struct.unpack_from('<H', e, p + 2)[0]
        p += 0x10 * nr + 4 * ns
    return ids if p == len(e) else None


def spawn_ids(tree):
    """objentry ids that a spawn file of any area (every language folder) holds.  A file that does not parse to its exact end is
    read conservatively: every aligned dword of it counts as an id."""
    ids = set(); loose = 0; n = 0
    d = os.path.join(tree.root, 'ard')
    for lang in sorted(os.listdir(d)):
        for f in sorted(os.listdir(os.path.join(d, lang))):
            if not f.lower().endswith('.ard'): continue
            for t, nm, e in E.bar(open(os.path.join(d, lang, f), 'rb').read()):
                if t != 12 or len(e) < 8: continue
                n += 1
                got = spawn_entities(e)
                if got is None:
                    loose += 1
                    for k in range(0, len(e) - 3, 4): ids.add(struct.unpack_from('<I', e, k)[0])
                else: ids.update(got)
    return ids, n, loose


def script_constants(tree):
    """every 32-bit constant a script pushes (`push imm32`), over all script bytecode of the tree: an object id used by a script
    is among them"""
    import bdxscan
    vals = set()
    for r in ('obj', 'msn', 'ard', 'map', '00common.bdx', 'limit', 'magic', 'minigame', 'file', 'menu', 'field2d', 'libretto', 'npack'):
        p = os.path.join(tree.root, r)
        files = [p] if os.path.isfile(p) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(p, followlinks=True) for f in fn)
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    if fh.read(4) != b'BAR\x01' and not f.lower().endswith('.bdx'): continue
                b = open(f, 'rb').read()
            except OSError: continue
            scripts = [b] if f.lower().endswith('.bdx') else []
            def walk(bb, depth=0):
                if depth > 6: return
                for t, nm, d in E.bar(bb):
                    if t == 3: scripts.append(d)
                    elif d[:4] == b'BAR\x01': walk(d, depth + 1)
            walk(b)
            for d in scripts:
                r2 = bdxscan.parse_bdx(d)
                if not r2: continue
                # every word pair that decodes as push imm32 anywhere in the script (reached or not): cheap and wider than needed
                n = (len(d) - 16) // 2
                for i in range(0, n - 2):
                    if struct.unpack_from('<H', d, 16 + 2 * i)[0] & 0x3f == 0:
                        v = struct.unpack_from('<I', d, 16 + 2 * (i + 1))[0]
                        if v < 0x10000: vals.add(v)
    return vals


OBJECT_CALLS = {(1, 0), (1, 114), (1, 323), (7, 33), (3, 20), (3, 21), (3, 76)}


def script_call_ids(tree):
    """constants pushed within the 24 code words before one of the scripts' object-by-id calls (OBJECT_CALLS): the objentry ids
    a script can name to the engine.  obj/G_* (the gummi objects) have their own table 3 and are left out for table 3."""
    import bdxscan
    vals = set(); calls = 0
    for r in ('obj', 'msn', 'ard', 'map', '00common.bdx', 'limit', 'magic', 'minigame', 'file', 'menu', 'field2d', 'libretto', 'npack'):
        p = os.path.join(tree.root, r)
        files = [p] if os.path.isfile(p) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(p, followlinks=True) for f in fn)
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    if fh.read(4) != b'BAR\x01' and not f.lower().endswith('.bdx'): continue
                b = open(f, 'rb').read()
            except OSError: continue
            scripts = [b] if f.lower().endswith('.bdx') else []
            def walk(bb, depth=0):
                if depth > 6: return
                for t, nm, d in E.bar(bb):
                    if t == 3: scripts.append(d)
                    elif d[:4] == b'BAR\x01': walk(d, depth + 1)
            walk(b)
            isg = os.path.basename(f).upper().startswith('G_')
            for d in scripts:
                r2 = bdxscan.parse_bdx(d)
                if not r2: continue
                for tb, tr, pc, cls in r2['syscalls']:
                    if (tb, tr) not in OBJECT_CALLS or cls == 'raw' or (isg and tb == 3): continue
                    calls += 1
                    for q in range(max(0, pc - 24), pc - 2):
                        if struct.unpack_from('<H', d, 16 + 2 * q)[0] & 0x3f == 0:
                            v = struct.unpack_from('<I', d, 16 + 2 * (q + 1))[0]
                            if v < 0x10000: vals.add(v)
    return vals, calls


def name_blobs(tree, exe):
    """bytes in which an object's NAME could stand: the exe, every mission file, and every area file without its event entries
    (an event names its actors too, but events are judged by the kept set, not here)"""
    blobs = []
    if exe: blobs.append(open(exe, 'rb').read().upper())
    for rel in walk_files(tree, 'msn'): blobs.append(open(os.path.join(tree.root, rel), 'rb').read().upper())
    for rel in walk_files(tree, 'ard'):
        b = open(os.path.join(tree.root, rel), 'rb').read()
        blobs.append(b''.join(d for t, nm, d in E.bar(b) if t != 22).upper())
    return blobs


def build(tree, graph, profile, exe, log, origins):
    """-> Manifest"""
    area_events = list(E.all_events(tree))
    if profile == 'A': kept = [ev for ev in area_events if not ev.stub and (graph.klass(ev.area, ev.name, ev) in ('reachable', 'named-never-run') or
                                                                           (ev.area, ev.name) in graph.theater)]
    else: kept = []
    others = list(E.other_events(tree))
    keep = collections.OrderedDict()          # lower-case relative name -> why
    music = set()
    for ev in area_events + others:           # every track any event names, whatever the profile
        for kind, rel in ev.files():
            if kind == 'music': music.add(rel.lower())
    for (wi, ar), pairs in tree.arif.items():
        for f, b in pairs:
            for t in (f, b):
                if t: music.add(E.bgm_file(t, E.WORLDS[wi] if wi < len(E.WORLDS) else '').lower())
    for a in tree.areas.values():
        for kind, progs in a.scripts.items():
            for pid, cmds in progs.items():
                for op, pl in cmds:
                    if op == 0x10 and len(pl) >= 4:
                        for t in struct.unpack_from('<HH', pl, 0):
                            if t: music.add(E.bgm_file(t, a.world).lower())
    for ev in kept + others:
        for kind, rel in ev.files():
            real = tree.find(rel) if '/' in rel else None
            if real: keep.setdefault(real.lower(), '%s/%s' % (ev.area, ev.name))
    for nm in ('tt_titleL', 'tt_titleR'):     # the two fixed names of exe 0x2d79d0 / 0x2d7f90 / 0x2d82d0
        for f in walk_files(tree, 'voice'):
            if os.path.basename(f).lower().startswith(nm.lower()) and '/event/' in f.lower(): keep.setdefault(f.lower(), 'fixed name in the exe')
    m = Manifest(tree, music)
    kept_ids = set()
    for ev in kept + others:
        for oid, sl, nm in ev.actors:
            kept_ids.add(oid)
            e = tree.objentry.get(oid)
            if e: kept_ids.update(x for x in e[2] if x)
    # ---- animations, effects, layouts, speech
    for top, why in (('anm', 'no kept event names this animation'), ('event', 'no kept event names this effect / layout'),
                     ('remastered/event', 'the textures of an effect / layout no kept event names')):
        for rel in walk_files(tree, top):
            base = rel[len('remastered/'):] if rel.startswith('remastered/') else rel
            if rel.startswith('remastered/'):          # remastered/<file name>/<texture>: the counterpart of the file <file name>
                base = os.path.dirname(base)
            if base.lower() in keep: continue
            m.add(rel, why)
    vroot = os.path.join(tree.root, 'voice')
    for lang in sorted(os.listdir(vroot)) if os.path.isdir(vroot) else []:
        for rel in walk_files(tree, 'voice/%s/event' % lang):
            if rel.lower() in keep: continue
            m.add(rel, 'no kept event names this speech file')
        m.add_tree('voice/%s/gumibattle' % lang, 'gummi stage speech: the gummi task is refused by the port') if os.path.isdir(os.path.join(vroot, lang, 'gumibattle')) else None
    # ---- the gummi stages
    for top in ('gumibattle', 'remastered/gumibattle'):
        if os.path.isdir(os.path.join(tree.root, top)): m.add_tree(top, 'gummi stage data: the gummi task is refused by the port')
    # ---- high-detail actors: never in the proven manifest; the candidates go to `actors`
    spawn, nspawn, loose = spawn_ids(tree)
    callids, ncalls = script_call_ids(tree)
    consts = script_constants(tree)
    blobs = name_blobs(tree, exe)
    by_model = collections.defaultdict(list)
    for oid, (model, mset, subs) in tree.objentry.items(): by_model[model.upper()].append(oid)
    attached = set()
    for oid, (model, mset, subs) in tree.objentry.items():
        if not model.upper().startswith('H_') or oid in kept_ids: attached.update(x for x in subs if x)
    stats = collections.Counter(); actors = Manifest(tree, music)
    hfiles = collections.defaultdict(list)
    for f in sorted(os.listdir(os.path.join(tree.root, 'obj'))):
        if f.upper().startswith('H_'): hfiles[f.split('.')[0].upper()].append('obj/' + f)
    rdir = os.path.join(tree.root, 'remastered', 'obj'); rfiles = collections.defaultdict(list)
    if os.path.isdir(rdir):
        for f in sorted(os.listdir(rdir)):
            if f.upper().startswith('H_'): rfiles[f.split('.')[0].upper()].append('remastered/obj/' + f)
    for model in sorted(set(hfiles) | set(rfiles)):
        ids = by_model.get(model, [])
        if any(i in kept_ids for i in ids): why = 'a kept event names it'
        elif any(i in spawn for i in ids): why = 'a spawn file holds its id'
        elif any(i in attached for i in ids): why = 'another object attaches it'
        elif any(i in callids for i in ids): why = 'a script hands its id to an object call'
        elif any(model.encode() in bl for bl in blobs): why = 'its name is in the exe or a mission / area file'
        else: why = None
        if why: stats[why] += 1; continue
        stats['candidate' + (' (some script pushes a constant equal to its id)' if any(i in consts for i in ids) else '') + ('' if ids else ' (no objentry)')] += 1
        for rel in hfiles.get(model, []): actors.add(rel, 'high-detail actor that only events name')
        for rel in rfiles.get(model, []): actors.add_tree(rel, 'textures of a high-detail actor that only events name')
    # ---- what an ORIGIN sound entry of data that stays names is kept (fixed point; see the module text)
    m.origin_kept = keep_origin_named(m, origins)
    actors.origin_kept = keep_origin_named(actors, origins)
    m.actors = actors
    log.append('profile %s: %d area events kept (%d of them named by a Theater program), %d events outside the area files read' % (
               profile, len(kept), sum(1 for ev in kept if (ev.area, ev.name) in graph.theater), len(others)))
    by_namer = collections.Counter(d for namers in m.origin_kept.values() for d in {os.path.dirname(f) for f in namers})
    by_dir = collections.Counter(os.path.dirname(r) for r in m.origin_kept)
    log.append('KEPT by ORIGIN sound entries of data that stays (exe 0x3a64f0): %d files (%s), %d bytes, named by the files of %s' % (
               len(m.origin_kept), ', '.join('%s %d' % kv for kv in sorted(by_dir.items())) or '-',
               sum(os.path.getsize(os.path.join(tree.root, r)) for r in m.origin_kept),
               ', '.join('%s (%d)' % kv for kv in sorted(by_namer.items())) or 'nothing'))
    log.append('high-detail actors (obj/H_*, NOT in this list; candidates: %d files, %.0f MiB): %s; %d spawn files (%d read conservatively), %d object-by-id script calls' %
               (len(actors.files), sum(actors.files.values()) / 1048576.0, ', '.join('%s %d' % kv for kv in sorted(stats.items())), nspawn, loose, ncalls))
    return m, music


def by_folder(files):
    c = collections.Counter(); n = collections.Counter()
    for rel, size in files.items():
        parts = rel.split('/')
        if parts[0] == 'remastered': key = 'remastered/' + parts[1] + ('/H_*' if parts[1] == 'obj' else '')
        elif parts[0] == 'voice': key = 'voice/*/' + parts[2]
        elif parts[0] == 'anm': key = 'anm/' + parts[1]
        elif parts[0] == 'obj': key = 'obj/H_*'
        elif parts[0] == 'event': key = 'event/' + parts[1]
        else: key = parts[0]
        c[key] += size; n[key] += 1
    return c, n


def write(path, m, profile, tree, music, log):
    total = sum(m.files.values()); c, n = by_folder(m.files)
    with open(path, 'w') as f:
        f.write('# prune-%s: %d files, %d bytes = %.0f MiB = %.2f GB removable under profile %s (%s)\n' % (profile, len(m.files), total, total / 1048576.0,
                total / 1e9, profile, 'as shipped: nothing a still-named event asks for is listed' if profile == 'A' else 'no cutscenes: every event is the stub (KHNMC_EVENTS=stub)'))
        f.write('# MUSIC IS NEVER IN THIS LIST: nothing under bgm/ or vagstream/, no sound table, none of the %d tracks an event, an area program or a room table names; the tool refuses such a name.\n' % len(music))
        f.write('# NOR anything an ORIGIN sound entry of data that stays names (%d candidates kept that way, e.g. the world-map voices voice/us/event/wm3*.win32.scd named by libretto/us/wm*.bar).\n' % len(m.origin_kept))
        f.write('# relative names of FILES in the data tree; written by tools/prune_lists.py, which only reads the tree.  Nothing was deleted.  MiB = 2^20 bytes, GB = 10^9 bytes.\n')
        for k in sorted(c): f.write('#   %-28s %6d files %8.1f MiB\n' % (k, n[k], c[k] / 1048576.0))
        for l in log: f.write('# %s\n' % l)
        for rel in m.files: f.write(rel + '\n')
    return total, c, n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True); ap.add_argument('--out-a', required=True); ap.add_argument('--out-b', required=True)
    ap.add_argument('--exe'); ap.add_argument('--keep-opened', action='append', default=[]); ap.add_argument('--lang', default='us')
    ap.add_argument('--out-actors')
    ap.add_argument('--self-test', action='store_true', help='check that add() refuses music and a missing file, then exit')
    a = ap.parse_args()
    tree = E.Tree(a.data, a.lang)
    if a.self_test:
        m = Manifest(tree, {'bgm/music118.win32.scd'}); bad = 0
        for rel in ('bgm/music118.win32.scd', 'vagstream/Title.win32.scd', '12soundinfo.bar', 'BGM/Music052.win32.scd', 'remastered/bgm/x', 'no/such/file'):
            try: m.add(rel, 'self test'); print('FAIL: %s was accepted' % rel); bad += 1
            except Refused: pass
        probe = next(iter(walk_files(tree, 'anm')), None)
        if probe:
            try: m.add(probe, 'self test')
            except Refused as e: print('FAIL: %s' % e); bad += 1
        # the ORIGIN rule, on the tree's own world-map dialogue: libretto/<lang>/wm*.bar name voice/wm/<lang>/wm30101sr and others;
        # a manifest that lists their voices and an anm file keeps the voices (named by data it does not list) and only them, and
        # an ORIGIN entry of a LISTED file keeps nothing
        idx = origin_index(tree, ['libretto'])
        if origin_key('voice/wm/us/wm30101sr') != 'wm30101sr.win32.scd': print('FAIL: origin_key'); bad += 1
        wm = [r for r in walk_files(tree, 'voice/%s/event' % a.lang) if origin_base(r).startswith('wm3')]
        if len(wm) != 58 or not any('wm30101sr.win32.scd' in k for k in idx.values()):
            print('FAIL: the tree\'s world-map voices (%d of 58) or libretto ORIGIN entries (%d files) were not found' % (len(wm), len(idx))); bad += 1
        m2 = Manifest(tree, set())
        for r in wm + ([probe] if probe else []): m2.add(r, 'self test')
        kept = keep_origin_named(m2, idx)
        if sorted(kept) != sorted(wm) or any(r in m2.files for r in wm) or (probe and probe not in m2.files):
            print('FAIL: ORIGIN rule kept %d (want the %d world-map voices) and left %d listed' % (len(kept), len(wm), len(m2.files))); bad += 1
        m3 = Manifest(tree, set()); lib = sorted(idx)[0]
        m3.files[lib] = 0; m3.files[next((r for r in wm if origin_base(r) in idx[lib]), 'x')] = 0       # a voice that this libretto names
        if keep_origin_named(m3, {lib: idx[lib]}): print('FAIL: an ORIGIN entry of a listed file kept a file'); bad += 1
        print('prune_lists self test: %s' % ('FAILED' if bad else 'ok (5 music names and a missing file refused, a plain file accepted; ORIGIN rule: '
                                              '%d world-map voices kept, the anm file listed, an ORIGIN entry of a listed file keeps nothing)' % len(kept)))
        return 1 if bad else 0
    graph = E.Graph(tree, 'all')
    origins = origin_index(tree)
    res = {}
    try:
        for prof, out in (('A', a.out_a), ('B', a.out_b)):
            log = []
            m, music = build(tree, graph, prof, a.exe, log, origins)
            res[prof] = (m, music, log, out)
    except Refused as e:
        sys.stderr.write(str(e) + '\n'); return 1
    opened = set()
    for lg in a.keep_opened:
        for ln in open(lg, errors='replace'):
            p = ln.rstrip('\n').split(' ', 3)
            if len(p) == 4 and p[0] == 'HIT':
                rel = p[3].replace('\\', '/').lower()          # the game's own name: <BasePath>/<relative name>, BasePath ends in kh2resource
                for mark in ('/kh2resource/', '/data-full/'):
                    i = rel.find(mark)
                    if i >= 0: rel = rel[i + len(mark):]; break
                opened.add(rel)
    rc = 0
    for prof in ('A', 'B'):
        m, music, log, out = res[prof]
        low = {r.lower(): r for r in m.files}
        hits = [low[o] for o in opened if o in low] + [r for o in opened for lr, r in low.items() if lr.startswith('remastered/' + o + '/')]
        if hits and prof == 'A':
            sys.stderr.write('prune_lists: CONTRADICTION: profile A lists %d files that a logged run opened, e.g. %s\n' % (len(hits), ', '.join(sorted(hits)[:5]))); rc = 1
        log.append('checked against %d names opened by %d logged runs: %d listed files were opened%s' % (len(opened), len(a.keep_opened), len(hits),
                   ' (expected under B: the scenes those runs played)' if prof == 'B' and hits else ''))
        total, c, n = write(out, m, prof, tree, music, log)
        print('prune-%s: %d files, %.0f MiB = %.2f GB -> %s' % (prof, len(m.files), total / 1048576.0, total / 1e9, out))
        for k in sorted(c): print('    %-28s %6d files %8.1f MiB' % (k, n[k], c[k] / 1048576.0))
        for l in log: print('    ' + l)
        if prof == 'B' and a.out_actors:
            act = m.actors; tot = sum(act.files.values())
            with open(a.out_actors, 'w') as f:
                f.write('# UNPROVEN candidates under profile B: %d files, %d bytes = %.0f MiB = %.2f GB: the high-detail actors (obj/H_*) that only events name.\n' % (
                        len(act.files), tot, tot / 1048576.0, tot / 1e9))
                f.write('# Not shown dead: objentry ids also come from the exe\'s code and the system tables.  Delete from this list only after a play-through with KHNMC_DATA_DENY_FATAL=1.\n')
                f.write('# MUSIC IS NEVER IN THIS LIST (the tool refuses it).  Nothing was deleted.\n')
                for rel in act.files: f.write(rel + '\n')
            print('actors (unproven): %d files, %.0f MiB = %.2f GB -> %s' % (len(act.files), tot / 1048576.0, tot / 1e9, a.out_actors))
    return rc


if __name__ == '__main__':
    sys.exit(main())
