#!/usr/bin/env python3
"""event_refs.py : what the in-engine cutscenes ("events", BAR type 22 entries of ard/<lang>/*.ard) of a KH2 data tree are, which of
them anything can still start, which files each one asks for, and what each one does to the background music (task 32).

  event_refs.py --data DATA_TREE summary                      per world: real events the story graph reaches / never started / stubs
  event_refs.py --data DATA_TREE list [--set SET]             one line per event: area, name, project, class, who names it, actors
  event_refs.py --data DATA_TREE refs [--set SET | AREA/NAME ...] [--existing]
                                                              the files the events ask for (with the remastered counterparts)
  event_refs.py --data DATA_TREE music [--set SET]            every music command of the events, the area's own music, and what a
                                                              stub in the event's place would leave playing
  event_refs.py --data DATA_TREE selfcheck                    structure checks of the decoders against the tree (exit 1 on a miss)
  SET: reachable (default) | real | named | unnamed | stub | all | theater (real events the Theater replays) | theater-only (named,
       never run by the story, replayed by the Theater).      Output is analysis of the owner's own data: never commit it.

Everything here was read from the data and from the exe's own code (kh2fm.exe, RVAs below), nothing is guessed:

AREA FILE (ard): a BAR; type 13 = the three area scripts `map` / `btl` / `evt`, type 12 = spawn files, type 22 = one event each,
  type 3 = script bytecode.  An area script is a list of PROGRAMS (u16 id, u16 byte length incl. this header, id 0xffff ends the
  list); a program is a list of commands (s16 op, s16 argc, argc * 4 bytes), interpreted by exe 0x3a24c0 (jump table 0x3a2a48):
    op 0x0c AreaSettings: s16 trigger, s16 key, then sub-commands (s16 sub, s16 param) (jump table 0x3a2ac8):
        0 Event   param = type, + 4-byte name of the type-22 entry of THIS area file      (0x3f6360)
        1 Jump    + s16 type, world, area, entrance, u16 localset, u16 fade (12 bytes)    (0x3a4bd0: type 1 = to world/area/entrance
                  with the save's programs for that room; type 2 = to the area the world's table in 07localset.bin gives for the
                  program id `localset`, with map = btl = evt = that id (0x39fbc0); type 3 = back; type 4 = the world map; types 5
                  and 6: 0x3a4bd0 decodes only the type (and the fade) for them.  In this tree type 6 ends 46 records, 30 of them in
                  evt programs that eventviewer.bar's rows name (the Theater's return); type 5 is in the three ending programs es00
                  evt 0x3a / 0x3c / 0x3d.  Neither leads to a room, so the story graph follows neither)
        2 ProgressFlag param = flag   3 MenuFlag   4, 5, 7 a value   6 a list of param ints   8 a bit
      When the trigger fires (0x3f60d0: trigger 0 = on entering the room, others by missions / objects), the settings are applied
      first (0x3f61a0: the progress flag), then the event plays (0x3ac350 -> 0x3a1f70 finds the type-22 entry by name), then the jump.
    op 0x10 Bgm: u16 field track, u16 battle track (0x3ab490 -> the pair at 0x142a113b8)
    op 0x15 Mission (int, name)      op 0x19 Progress: argc flags, each set if it is not yet (0x39afc0)
    op 0x1c If entrance == arg0: the nested commands that follow inside this command
PROGRESS FLAG (0x39afc0): flag = world << 10 | index.  00progress.bin (BAR, one entry per world: u16 offset per index) holds a small
  script per flag: records (u8 op, u8 n words): op 1 = n/4 times (area, map, btl, evt program; 0xffff keeps) into the save's table
  of programs per room; op 6 = the world's music-set index (one word, stored minus 1); op 0xc = more flags to set; 7 = flags to
  clear; 2..5, 8, 9, 0xd = world map / menu state.
EVENT (type 22): commands (u16 byte length incl. header, u16 type), a zero length ends the list (walk: exe 0x2c8f40, types 0..0x53).
  Load pass 0x2cc180 (jump table 0x2cc31c), per-frame pass 0x2cb5d0 (jump table 0x2cbc2c).  Used here:
    0x00 project (name)   0x01 actor (objentry id, slot, name)   0x03 map (area number, world)   0x08 end frame
    0x23 voices: u16 count, 6 bytes, then count * 32 bytes (u16 frame, byte, name)        -> voice/<lang>/event/<name>.win32.scd
    0x24 ReadAssets: 8 bytes, then entries (u16 type, u16 length): 0x38 actor (objentry id, slot, name), 0x25 motion (objentry id,
         slot, word, "anm_<dir>/<file>"), 0x26 voice name, 0x39 effect (word, name), 0x3e layout name
    0x2c play animation (.., "anm_<dir>/<file>")   0x3d layout (4 bytes, name)   0x15 subtitle (u16 frame, u16 line, u16 message id,
         u16 0 show / 1 hide; the text is in msg/<lang>/<world>.bar)   0x2a sound bank (u16, u16 bank: se/se%03d.win32.scd, 0x2d8a90)
    0x2f read music (u16 frame, u16 track)   0x30 play music (u16 frame, u16 slot 0 / 1 / 2 = both, u16 track)   (0x2cf220, 0x2cf3b0)
    0x2e music volume (u16 frame, u16 flag, u8 a, u8 b, u8 time index, u8 slot)                                  (0x2cf490)
  Paths as the exe builds them: animation 0x2d1a70 = "anm/us/" (or "anm/fm/" only in the Japanese regions 0 and 7) + <dir> + "/" +
  <actor name> + "/" + <file> + ".anb"; effect 0x2d9130 / 0x2d9490 = "event/effect/" + name + ".pax", except that outside the
  Japanese regions (language global 0x140716750, read by 0x1501d0, not 0 and not 7) a name containing "tt_event_012" (strstr,
  0x140585540) gets "_2.pax" instead (effect_file()); layout "event/layout/<lang>/<name>.2ld" then
  "event/layout/<name>.2ld"; music 0x1df110 = "bgm/music%03d.win32.scd" with the track + 400 for tracks 106..109 and 113 outside
  the Japanese regions, and 117 -> 517 / 121 -> 521 in world 13; an actor is the objentry's model obj/<MODEL>.mdlx (+ its
  .mset / .a.<lang> when present in the tree).
MUSIC OF AN AREA: on loading a room (0x39c860 -> 0x3a5960) the pair (field, battle) number [music set of the world] of the room's
  record in 03system.bin `arif` (record 0x40 bytes, pairs at +0x10); the programs' Bgm command replaces it; 0x155840 applies the
  pair once the room's entry events are over (0x152680).

The STORY GRAPH (which programs can ever run) is a fixed point over: program 0 of every room and the new game's first program;
every Jump of a program that can run; every progress flag such a program, a chained progress script, a script (`trap_progress_set_flag`
with a constant) or the exe itself can set, and the programs those flags' scripts put into rooms.  It takes every branch and every
trigger: it can only err towards "can run".  `--flags all` applies EVERY progress script (the widest reading).

THE THEATER (KH2FM's event viewer, a title-menu mode) is a separate root, not part of the story graph (a replay advances no story):
  eventviewer.bar, BAR entry `even`: u32 1, u32 row count, rows of 6 u16 (kind: 0 = a chapter heading, 1 = a scene; message id of
  the title in msg/<lang>/title.bar; world index; program A; program B; 0).  A scene row plays evt program A and B of the room that
  07localset.bin gives the world for that program id; the events those programs' AreaSettings name are what the Theater replays
  (Graph.theater).  The exe reads the table at boot (every run's file log has eventviewer.bar; reader 0x2b9360, called from the
  title-side menu code 0x2b3e81 / 0x2b4e82 that also uses menu/eventviewer.2ld).  The title menu gets one more row when the byte
  0x140b0d54d is set (0x2b3400 sets it from 0x2e53a0 / 0x2b73f0; row count written at 0x2b4db5); which save state sets it was not
  traced, and no route opens the Theater.  The port does not gate it (no runtime hook names it).
"""
import argparse, collections, os, struct, sys
sys.dont_write_bytecode = True

WORLDS = ['zz', 'es', 'tt', 'di', 'hb', 'bb', 'he', 'al', 'mu', 'po', 'lk', 'lm', 'dc', 'wi', 'nm', 'wm', 'ca', 'tr', 'eh']
WORLD_NAMES = {'zz': 'World ZZ', 'es': 'End of Sea', 'tt': 'Twilight Town', 'di': 'Destiny Island', 'hb': 'Hollow Bastion',
               'bb': "Beast's Castle", 'he': 'Olympus', 'al': 'Agrabah', 'mu': 'Land of Dragons', 'po': 'Pooh', 'lk': 'Pride Lands slot',
               'lm': 'Atlantica slot', 'dc': 'Disney Castle', 'wi': 'Timeless River', 'nm': 'Halloween Town', 'wm': 'World Map',
               'ca': 'Port Royal', 'tr': 'Space Paranoids', 'eh': 'The World That Never Was'}
# progress flags the exe sets from its own code (constants at the call sites of 0x39afc0 and the pair table 0x1405c3348)
EXE_FLAGS = [0x4848, 0x4849, 0x2c26, 0x2c27, 0x2c28, 0x2c2d, 0x1809, 0x1807, 0x2408, 0x2410, 0x2418, 0x2420, 0x2427]
NEW_GAME = [('tt', 1, 'evt', 1)]     # the first room of a new game runs evt program 1 (map report of every route: tt01, programs 0 0 0x1)
MUSIC_DIRS = ('bgm', 'vagstream')    # never removable (task 32, item 6a)


def die(msg):
    sys.stderr.write('event_refs: ERROR: %s\n' % msg); sys.exit(1)


def bar(b):
    if b[:4] != b'BAR\x01' or len(b) < 16: return []
    n = struct.unpack_from('<I', b, 4)[0]
    if 16 + 16 * n > len(b): return []
    out = []
    for i in range(n):
        t, dup, nm, o, s = struct.unpack_from('<HH4sII', b, 16 + 16 * i)
        out.append((t, nm.rstrip(b'\0').decode('latin1'), b[o:o + s]))
    return out


def cstr(b): return b.split(b'\0')[0].decode('latin1')


# ---------------------------------------------------------------------------------------------------------------- area scripts
def programs(d):
    p = 0; out = []
    while p + 4 <= len(d):
        pid, ln = struct.unpack_from('<HH', d, p)
        if pid == 0xffff or ln < 4 or p + ln > len(d): break
        out.append((pid, d[p + 4:p + ln])); p += ln
    return out


def settings(a):
    """AreaSettings payload -> (trigger, key, [sub-commands])"""
    trig, key = struct.unpack_from('<hh', a, 0); p = 4; out = []
    while p + 4 <= len(a):
        so, par = struct.unpack_from('<hh', a, p); p += 4
        if so == 0:
            out.append(('event', par, cstr(a[p:p + 4]))); p += 4
        elif so == 1:
            ty, w, ar, ent, ls, fade = struct.unpack_from('<hhhhHH', a, p); p += 12
            out.append(('jump', ty, w, ar, ent, ls, fade))
        elif so == 2: out.append(('flag', par & 0xffff))
        elif so == 6:
            out.append(('list', [struct.unpack_from('<I', a, p + 4 * i)[0] for i in range(par)])); p += 4 * par
        elif 3 <= so <= 8: out.append(('set%d' % so, par))
        else: out.append(('bad', so, par)); break
    return trig, key, out


def commands(pd):
    """program body -> flat list of (op, payload), the nested commands of `If entrance` (0x1c) included (every branch is taken)"""
    p = 0; out = []
    while p + 4 <= len(pd):
        op, argc = struct.unpack_from('<hh', pd, p); p += 4
        if argc < 0 or p + 4 * argc > len(pd): out.append((-1, pd[p - 4:])); break
        a = pd[p:p + 4 * argc]; p += 4 * argc
        out.append((op, a))
        if op == 0x1c and len(a) >= 4: out += commands(a[4:])
    return out


class Area:
    def __init__(self, name, b):
        self.name = name; self.world = name[:2]; self.num = int(name[2:]) if name[2:].isdigit() else -1
        self.scripts = {}; self.events = collections.OrderedDict(); self.spawns = []; self.bdx = []
        for t, nm, d in bar(b):
            if t == 13 and nm in ('map', 'btl', 'evt'):
                self.scripts[nm] = collections.OrderedDict((pid, commands(pd)) for pid, pd in programs(d))
            elif t == 22: self.events[nm] = d          # a later entry of the same name replaces: the game's lookup takes the first;
            elif t == 12: self.spawns.append((nm, d))   # see first_event()
            elif t == 3: self.bdx.append((nm, d))
        self.first = {}
        for t, nm, d in bar(b):
            if t == 22 and nm not in self.first: self.first[nm] = d


class Tree:
    def __init__(self, root, lang='us'):
        self.root = root; self.lang = lang
        d = os.path.join(root, 'ard', lang)
        if not os.path.isdir(d): die('no %s' % d)
        self.areas = collections.OrderedDict()
        for f in sorted(os.listdir(d)):
            if f.lower().endswith('.ard'):
                self.areas[f[:-4].lower()] = Area(f[:-4].lower(), open(os.path.join(d, f), 'rb').read())
        self.localset = self._localset(); self.progress = self._progress(); self.objentry = self._objentry()
        self.arif = self._arif(); self.eventviewer = self._eventviewer(); self._ci = {}

    def _eventviewer(self):
        """eventviewer.bar (the Theater): [(kind, message id, world index, program A, program B, word)], see the module text"""
        ents = [d for t, nm, d in bar(self._read('eventviewer.bar')) if nm == 'even']
        if len(ents) != 1 or len(ents[0]) < 8: die('eventviewer.bar: no single `even` entry')
        d = ents[0]; n = struct.unpack_from('<I', d, 4)[0]
        if 8 + 12 * n > len(d): die('eventviewer.bar: %d rows do not fit in %d bytes' % (n, len(d)))
        return [struct.unpack_from('<6H', d, 8 + 12 * i) for i in range(n)]

    def _read(self, rel):
        p = os.path.join(self.root, rel)
        if not os.path.isfile(p): die('missing %s' % p)
        return open(p, 'rb').read()

    def _localset(self):
        out = {}
        for i, (t, nm, d) in enumerate(bar(self._read('07localset.bin'))):
            if len(d) < 8: continue
            n = min(struct.unpack_from('<I', d, 4)[0], (len(d) - 8) // 4)      # hb's table says 192 and holds 191
            out[i] = {struct.unpack_from('<HH', d, 8 + 4 * k)[0]: struct.unpack_from('<HH', d, 8 + 4 * k)[1] for k in range(n)}
        return out

    def _progress(self):
        """world index -> {flag index: [(op, [words])]}"""
        out = {}
        for i, (t, nm, d) in enumerate(bar(self._read('00progress.bin'))):
            if i >= len(WORLDS) or len(d) < 2: continue
            first = struct.unpack_from('<H', d, 0)[0]; tab = {}
            for k in range(first // 2):
                o = struct.unpack_from('<H', d, 2 * k)[0]; recs = []
                while o + 2 <= len(d) and d[o]:
                    op, n = d[o], d[o + 1]
                    recs.append((op, list(struct.unpack_from('<%dH' % n, d, o + 2)) if o + 2 + 2 * n <= len(d) else []))
                    o += 2 + 2 * n
                if recs: tab[k] = recs
            out[i] = tab
        return out

    def _objentry(self):
        b = self._read('00objentry.bin'); n = struct.unpack_from('<I', b, 4)[0]; out = {}
        for i in range(n):
            o = 8 + 0x60 * i
            if o + 0x60 > len(b): break
            oid = struct.unpack_from('<I', b, o)[0]
            out[oid] = (cstr(b[o + 8:o + 40]), cstr(b[o + 40:o + 72]), struct.unpack_from('<4H', b, o + 0x58))
        return out

    def _arif(self):
        """(world index, area) -> list of (field, battle) music pairs"""
        out = {}
        for t, nm, d in bar(self._read('03system.bin')):
            if nm != 'arif': continue
            nworld = struct.unpack_from('<I', d, 4)[0]
            for w in range(nworld):
                cnt, off = struct.unpack_from('<HH', d, 8 + 4 * w)
                for a in range(cnt):
                    r = off + 0x40 * a
                    if r + 0x40 <= len(d): out[(w, a)] = [struct.unpack_from('<HH', d, r + 0x10 + 4 * k) for k in range(8)]
        return out

    # case-insensitive existence, as the runtime's file layer resolves names
    def find(self, rel):
        cur = self.root; real = []
        for part in rel.split('/'):
            names = self._ci.get(cur)
            if names is None:
                try: names = {n.lower(): n for n in os.listdir(cur)}
                except OSError: names = {}
                self._ci[cur] = names
            n = names.get(part.lower())
            if n is None: return None
            real.append(n); cur = os.path.join(cur, n)
        return '/'.join(real)


# ---------------------------------------------------------------------------------------------------------------- events
STUB_LEN = 56


def ev_cmds(d):
    p = 0; out = []
    while p + 2 <= len(d):
        ln = struct.unpack_from('<H', d, p)[0]
        if ln == 0: return out, len(d) - p          # the terminator: a zero length (2 bytes at the end of every entry)
        if ln < 4 or p + ln > len(d): return out, -1
        out.append((struct.unpack_from('<H', d, p + 2)[0], d[p + 4:p + ln])); p += ln
    return out, -1


def is_stub(d):
    """the mod's empty scene: project, setup, start, end frame, one fade and nothing else"""
    c, tail = ev_cmds(d)
    return [t for t, _ in c] == [0x00, 0x0f, 0x10, 0x08, 0x12]


class Event:
    def __init__(self, tree, area, name, d):
        self.area = area; self.name = name; self.data = d; self.stub = is_stub(d)
        self.cmds, self.tail = ev_cmds(d)
        self.project = ''; self.actors = []; self.end = 0; self.map = None
        self.anims = set(); self.voices = set(); self.effects = set(); self.layouts = set(); self.movies = set(); self.sounds = set()
        self.bgm = []                # (kind, frame, ...)
        self.subs = 0
        slot = {}
        for ty, pl in self.cmds:
            if ty == 0x00: self.project = cstr(pl[6:])
            elif ty == 0x01 and len(pl) >= 5:
                oid, sl = struct.unpack_from('<HH', pl, 0); nm = cstr(pl[4:]); slot[sl] = nm; self.actors.append((oid, sl, nm))
            elif ty == 0x03 and len(pl) >= 4: self.map = (cstr(pl[2:4]), struct.unpack_from('<H', pl, 0)[0])
            elif ty == 0x08: self.end = struct.unpack_from('<H', pl, 0)[0]
            elif ty == 0x15: self.subs += 1
            elif ty == 0x23 and len(pl) >= 8:
                n = struct.unpack_from('<H', pl, 0)[0]
                for k in range(n):
                    e = pl[8 + 32 * k:8 + 32 * k + 32]
                    if len(e) == 32 and cstr(e[3:]): self.voices.add(cstr(e[3:]))
            elif ty == 0x24:
                q = 8
                while q + 4 <= len(pl):
                    t, l = struct.unpack_from('<HH', pl, q)
                    if l < 4 or q + l > len(pl): break
                    e = pl[q + 4:q + l]; q += l
                    if t == 0x38 and len(e) >= 5:
                        oid, sl = struct.unpack_from('<HH', e, 0); slot.setdefault(sl, cstr(e[4:]))
                        if (oid, sl, cstr(e[4:])) not in self.actors: self.actors.append((oid, sl, cstr(e[4:])))
                    elif t == 0x25 and len(e) >= 7:
                        oid, sl = struct.unpack_from('<HH', e, 0); self.anims.add((sl, cstr(e[6:])))
                    elif t == 0x26: self.voices.add(cstr(e))
                    elif t == 0x39 and len(e) >= 3: self.effects.add(cstr(e[2:]))
                    elif t == 0x3e: self.layouts.add(cstr(e))
            elif ty == 0x2c and len(pl) >= 15:
                sl = struct.unpack_from('<H', pl, 10)[0]; nm = cstr(pl[14:])
                if nm.startswith('anm_'): self.anims.add((sl, nm))
            elif ty == 0x3d and len(pl) >= 5: self.layouts.add(cstr(pl[4:]))
            elif ty == 0x44 and len(pl) >= 3: self.movies.add(cstr(pl[2:]))
            elif ty == 0x2a and len(pl) >= 4: self.sounds.add(struct.unpack_from('<H', pl, 2)[0])      # load pass 0x2d8a90: "se/" + "se%03d.win32.scd"
            elif ty == 0x2f and len(pl) >= 4: self.bgm.append(('read',) + struct.unpack_from('<HH', pl, 0))
            elif ty == 0x30 and len(pl) >= 6: self.bgm.append(('play',) + struct.unpack_from('<HHH', pl, 0))
            elif ty == 0x2e and len(pl) >= 8: self.bgm.append(('volume',) + struct.unpack_from('<HHBBBB', pl, 0))
        self.slot = slot
        self.tree = tree

    def world(self): return self.project[:2] if len(self.project) > 2 and self.project[2] == '_' else self.area[:2]

    def models(self):
        out = []
        for oid, sl, nm in self.actors:
            e = self.tree.objentry.get(oid)
            out.append(e[0] if e else '?%#x' % oid)
        return out

    def has_roxas(self):
        """Roxas' own objects: the player object P_EX100* of this mod, and the game's Roxas models *EX110* / *EX510*"""
        return any(('EX110' in m) or ('EX510' in m) or m.startswith('P_EX100') for m in self.models())

    def files(self):
        """-> list of (kind, relative name as the game asks for it)"""
        t = self.tree; lang = t.lang; out = []
        for oid, sl, nm in self.actors:
            e = t.objentry.get(oid)
            if not e: out.append(('actor-unknown-objentry', '%#x' % oid)); continue
            seen = set()
            def obj(model, mset):
                if not model or model in seen: return
                seen.add(model)
                out.append(('actor', 'obj/%s.mdlx' % model))
                if mset: out.append(('actor', 'obj/%s' % mset))
                for ext in ('.a.%s' % lang, '.a.fm'): out.append(('actor-opt', 'obj/%s%s' % (model, ext)))
            obj(e[0], e[1])
            for sub in e[2]:
                if sub and sub in t.objentry: obj(t.objentry[sub][0], t.objentry[sub][1])
        for sl, nm in sorted(self.anims):
            if '/' not in nm or not nm.startswith('anm_'): out.append(('anim-odd', nm)); continue
            d, f = nm[4:].split('/', 1); actor = self.slot.get(sl)
            if actor is None: out.append(('anim-no-actor', nm)); continue
            out.append(('anim', 'anm/us/%s/%s/%s.anb' % (d, actor, f)))
        for v in sorted(self.voices): out.append(('voice', 'voice/%s/event/%s.win32.scd' % (lang, v)))
        for e in sorted(self.effects): out.append(('effect', effect_file(e, lang)))
        for l in sorted(self.layouts):
            out.append(('layout', 'event/layout/%s/%s.2ld' % (lang, l))); out.append(('layout', 'event/layout/%s.2ld' % l))
        for tr in sorted({b[2] for b in self.bgm if b[0] == 'read'} | {b[3] for b in self.bgm if b[0] == 'play'}):
            out.append(('music', bgm_file(tr, self.world())))
        for n in sorted(self.sounds): out.append(('sound', 'se/se%03d.win32.scd' % n))
        if self.map: out.append(('map', 'map/%s%02d.map' % self.map))
        return out


JAPANESE_LANGS = ('jp',)              # the language global's values 0 and 7; every language folder of this tree (us fr gr it sp) is not
EFFECT_2_KEY = 'tt_event_012'        # exe 0x140585540


def effect_file(name, lang):
    """exe 0x2d9130 (and the unreferenced twin 0x2d9490): "event/effect/" + name, then, outside the Japanese regions (0x1501d0 is
    neither 0 nor 7), strstr(name, "tt_event_012") ? "_2.pax" : ".pax"; in the Japanese regions always ".pax"."""
    if lang not in JAPANESE_LANGS and EFFECT_2_KEY in name: return 'event/effect/%s_2.pax' % name
    return 'event/effect/%s.pax' % name


def bgm_file(track, world=''):
    t = track
    if world == 'wi': t = {0x75: 0x205, 0x79: 0x209}.get(t, t)
    if 106 <= t <= 109 or t == 113: t += 400          # outside the Japanese regions (exe 0x1df110)
    return 'bgm/music%03d.win32.scd' % t


# ---------------------------------------------------------------------------------------------------------------- scripts' flags
def bdx_flags(tree, verbose=False):
    """progress flags that script bytecode sets with a constant: `push imm ; syscall 0,14` (trap_progress_set_flag).
    Returns (set of flags, number of calls, number of calls whose argument is not a constant)."""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    try: import bdxscan
    except Exception as e: die('tools/bdxscan.py is needed for the scripts\' progress flags: %s' % e)
    flags = set(); calls = 0; unknown = 0
    roots = ['obj', 'msn', 'ard', 'map', '00common.bdx', 'limit', 'magic', 'minigame', 'file', 'menu', 'field2d', 'libretto', 'npack']
    for r in roots:
        p = os.path.join(tree.root, r)
        files = [p] if os.path.isfile(p) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(p, followlinks=True) for f in fn)
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    if fh.read(4) != b'BAR\x01' and not f.lower().endswith('.bdx'): continue
            except OSError: continue
            b = open(f, 'rb').read()
            scripts = []
            if f.lower().endswith('.bdx'): scripts.append(b)
            def walk(bb, depth=0):
                if depth > 6: return
                for t, nm, d in bar(bb):
                    if t == 3: scripts.append(d)
                    elif d[:4] == b'BAR\x01': walk(d, depth + 1)
            walk(b)
            for d in scripts:
                if b'\x0a\x00\x0e\x00' not in d: continue
                r = bdxscan.parse_bdx(d)
                if not r: continue
                for tbl, trap, pc, cls in r['syscalls']:
                    if (tbl, trap) != (0, 14) or cls == 'raw': continue
                    calls += 1
                    # the instruction before the syscall: a `push imm32` is 3 words, opcode word & 0x3f == 0
                    w = lambda i: struct.unpack_from('<H', d, 16 + 2 * i)[0]
                    if pc >= 3 and (w(pc - 3) & 0x3f) == 0:
                        v = struct.unpack_from('<i', d, 16 + 2 * (pc - 2))[0]
                        if 0 <= v < 0x10000: flags.add(v); continue
                    unknown += 1
    return flags, calls, unknown


# ---------------------------------------------------------------------------------------------------------------- story graph
class Graph:
    def __init__(self, tree, flags_mode='story'):
        self.t = tree; self.mode = flags_mode
        self.run = {}                 # (world code, area number, kind, program) -> how it became possible (text)
        self.flags = {}               # flag -> how
        self.started = {}             # (area name, event name) -> [(kind, program, trigger, type)] in programs that can run
        self.named = collections.defaultdict(list)    # (area name, event name) -> every (kind, program, trigger, type) that names it
        self.theater = collections.defaultdict(list)  # (area name, event name) -> [(world, program, title message id)]: the Theater
        self.theater_stats = collections.Counter()    # replays it (not part of the story graph: a replay advances no story)
        self.script_flags = None
        self.compute()
        self.compute_theater()

    def compute_theater(self):
        """the Theater's roots: per scene row of eventviewer.bar, the programs A and B (every kind: a row is entered like a type-2
        jump, map = btl = evt = the id) of the room 07localset.bin gives for that id; every event their AreaSettings name"""
        t = self.t
        for kind, mid, w, pa, pb, _x in t.eventviewer:
            if kind != 1: self.theater_stats['heading rows'] += 1; continue
            self.theater_stats['scene rows'] += 1
            for p in sorted({pa, pb}):
                n = t.localset.get(w, {}).get(p) if w < len(WORLDS) else None
                a = self.area(WORLDS[w], n) if n is not None else None
                progs = [(k, a.scripts[k][p]) for k in ('map', 'btl', 'evt') if a is not None and p in a.scripts.get(k, {})]
                if not progs: self.theater_stats['program ids without a room or program'] += 1; continue
                self.theater_stats['programs mapped'] += 1
                for k, cmds in progs:
                    for op, pl in cmds:
                        if op != 0x0c: continue
                        for s in settings(pl)[2]:
                            if s[0] == 'event' and (WORLDS[w], p, mid) not in self.theater[(a.name, s[2])]:
                                self.theater[(a.name, s[2])].append((WORLDS[w], p, mid))

    def area(self, w, n): return self.t.areas.get('%s%02d' % (w, n))

    def compute(self):
        t = self.t; work = []
        def add_run(w, n, kind, p, why):
            k = (w, n, kind, p)
            if k in self.run: return
            a = self.area(w, n)
            if a is None or p not in a.scripts.get(kind, {}):
                self.run[k] = why; return                 # nothing to interpret, but remember that it was asked for
            self.run[k] = why; work.append(('run', k))
        def add_flag(f, why):
            if f in self.flags or f == 0xffff: return
            self.flags[f] = why; work.append(('flag', f))
        for a in t.areas.values():
            for kind, progs in a.scripts.items():
                for pid, cmds in progs.items():
                    for op, pl in cmds:
                        if op == 0x0c:
                            trig, key, subs = settings(pl)
                            for s in subs:
                                if s[0] == 'event': self.named[(a.name, s[2])].append((kind, pid, trig, s[1]))
        for a in t.areas.values():
            if a.num < 0: continue
            for kind in ('map', 'btl', 'evt'): add_run(a.world, a.num, kind, 0, 'program 0 (a room nothing has set yet)')
        for w, n, kind, p in NEW_GAME: add_run(w, n, kind, p, 'new game')
        for f in EXE_FLAGS: add_flag(f, 'set by the exe\'s own code')
        fl, calls, unknown = bdx_flags(t)
        self.script_flags = (fl, calls, unknown)
        for f in sorted(fl): add_flag(f, 'set by a script (trap_progress_set_flag)')
        if self.mode == 'all':
            for wi, tab in t.progress.items():
                for idx in tab: add_flag(wi << 10 | idx, 'every progress script applied (--flags all)')
        while work:
            what, k = work.pop()
            if what == 'flag':
                wi, idx = k >> 10, k & 0x3ff
                if wi >= len(WORLDS): continue
                for op, words in t.progress.get(wi, {}).get(idx, []):
                    if op == 1:
                        for i in range(0, len(words) - 3, 4):
                            ar, m, b, e = words[i:i + 4]
                            for kind, p in (('map', m), ('btl', b), ('evt', e)):
                                if p != 0xffff: add_run(WORLDS[wi], ar, kind, p, 'progress flag %#x' % k)
                    elif op == 0xc:
                        for f in words: add_flag(f, 'chained from progress flag %#x' % k)
                continue
            w, n, kind, p = k; a = self.area(w, n)
            for op, pl in a.scripts[kind][p]:
                if op == 0x19:
                    for i in range(0, len(pl), 4): add_flag(struct.unpack_from('<I', pl, i)[0] & 0xffff, '%s %s program %#x' % (a.name, kind, p))
                elif op == 0x0c:
                    trig, key, subs = settings(pl)
                    for s in subs:
                        if s[0] == 'flag': add_flag(s[1], '%s %s program %#x, trigger %d' % (a.name, kind, p, trig))
                        elif s[0] == 'event': self.started.setdefault((a.name, s[2]), []).append((kind, p, trig, s[1]))
                        elif s[0] == 'jump' and s[1] == 2:
                            wi = s[2] if 0 <= s[2] < len(WORLDS) else WORLDS.index(a.world)
                            ar = t.localset.get(wi, {}).get(s[5])
                            if ar is not None:
                                for kd in ('map', 'btl', 'evt'):
                                    add_run(WORLDS[wi], ar, kd, s[5], 'jump from %s %s program %#x (trigger %d)' % (a.name, kind, p, trig))

    def klass(self, area, name, ev):
        if ev.stub: return 'stub'
        if (area, name) in self.started: return 'reachable'
        if (area, name) in self.named: return 'named-never-run'
        return 'not-named'


def all_events(tree):
    for a in tree.areas.values():
        for nm, d in a.first.items(): yield Event(tree, a.name, nm, d)


OTHER_ROOTS = ('msn', 'obj', 'limit', 'magic', 'minigame', 'file', 'menu', 'field2d', 'libretto', 'npack', 'map', 'effect', 'event', 'gumibattle', 'gumimenu',
               'gumiblock', 'dbg', 'msg', 'itempic')


def other_events(tree):
    """the type-22 entries OUTSIDE the area files: reaction commands (obj/*.a.<lang>), limits, mission banners (msn/), game over
    (00battle.bin) ...  They run through the same player (exe 0x3ad2d0 -> 0x2cc4b0) whatever the story does, in every language
    folder; the caller keeps everything they name.  Yields Event objects whose `area` is the file's relative name."""
    def scan(rel, b, depth=0, lab=''):
        if depth > 5: return
        for t, nm, d in bar(b):
            if t == 22: yield Event(tree, rel + lab, nm, d)
            elif d[:4] == b'BAR\x01':
                for e in scan(rel, d, depth + 1, lab + '::' + nm): yield e
    tops = [f for f in sorted(os.listdir(tree.root)) if os.path.isfile(os.path.join(tree.root, f))] + [r for r in OTHER_ROOTS]
    for top in tops:
        p = os.path.join(tree.root, top)
        files = [p] if os.path.isfile(p) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(p, followlinks=True) for f in fn)
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    if fh.read(4) != b'BAR\x01': continue
                b = open(f, 'rb').read()
            except OSError: continue
            for e in scan(os.path.relpath(f, tree.root), b): yield e


def select(tree, graph, which):
    for ev in all_events(tree):
        k = graph.klass(ev.area, ev.name, ev)
        if which == 'all' or (which == 'real' and not ev.stub) or (which == 'stub' and ev.stub) or \
           (which == 'reachable' and k == 'reachable') or (which == 'named' and k in ('reachable', 'named-never-run')) or \
           (which == 'unnamed' and k == 'not-named') or (which == 'never' and k in ('named-never-run', 'not-named')) or \
           (which == 'theater' and not ev.stub and (ev.area, ev.name) in graph.theater) or \
           (which == 'theater-only' and k == 'named-never-run' and (ev.area, ev.name) in graph.theater):
            yield ev, k


def remastered_of(tree, rel):
    """the remastered counterpart of a file: the folder remastered/<same name>/ with its textures"""
    r = tree.find('remastered/' + rel)
    return r if r and os.path.isdir(os.path.join(tree.root, r)) else None


# ---------------------------------------------------------------------------------------------------------------- commands
def cmd_summary(tree, graph, a):
    by = collections.OrderedDict((w, collections.Counter()) for w in WORLDS)
    tot = collections.Counter()
    for ev in all_events(tree):
        k = graph.klass(ev.area, ev.name, ev); w = ev.world() if a.by == 'project' else ev.area[:2]
        by.setdefault(w, collections.Counter())[k] += 1; tot[k] += 1
        if (ev.area, ev.name) in graph.theater:                 # the Theater's roots, counted apart from the story graph
            for c in (by[w], tot): c['theater ' + k] += 1
    fl, calls, unknown = graph.script_flags
    print('events by %s (story graph: %d programs can run, %d progress flags can be set; scripts set %d flags by constant in %d calls, %d calls with a computed flag)' %
          ('project name' if a.by == 'project' else 'area file', len(graph.run), len(graph.flags), len(fl), calls, unknown))
    print('Theater (eventviewer.bar, not part of the story graph): %s; the two Theater columns count the real events a Theater program names' %
          ', '.join('%s %d' % kv for kv in sorted(graph.theater_stats.items())))
    print('%-28s %9s %15s %9s %6s %17s %21s' % ('world', 'reachable', 'named-never-run', 'not-named', 'stubs', 'Theater:reachable', 'Theater:named-never-run'))
    for w, c in by.items():
        if sum(c.values()): print('%-28s %9d %15d %9d %6d %17d %21d' % ('%s (%s)' % (WORLD_NAMES.get(w, w), w), c['reachable'], c['named-never-run'], c['not-named'],
                                                                   c['stub'], c['theater reachable'], c['theater named-never-run']))
    print('%-28s %9d %15d %9d %6d %17d %21d   = %d events, %d real' % ('TOTAL', tot['reachable'], tot['named-never-run'], tot['not-named'], tot['stub'],
                                                         tot['theater reachable'], tot['theater named-never-run'],
                                                         sum(v for k, v in tot.items() if not k.startswith('theater')),
                                                         sum(v for k, v in tot.items() if not k.startswith('theater')) - tot['stub']))
    return 0


_MSG = {}


def subtitles(tree, ev, n, msgdump):
    """the first n subtitle texts the scene shows: command 0x15 = (u16 frame, u16 line, u16 message id, u16 0 show / 1 hide), the
    message from msg/<lang>/<world of the project>.bar (decoded by the project's tools/mod/msgdump.py)"""
    if n <= 0: return []
    w = ev.world()
    if w not in _MSG:
        _MSG[w] = {}
        rel = tree.find('msg/%s/%s.bar' % (tree.lang, w))
        if rel and msgdump:
            import importlib.util
            spec = importlib.util.spec_from_file_location('msgdump', msgdump); md = importlib.util.module_from_spec(spec); spec.loader.exec_module(md)
            b = open(os.path.join(tree.root, rel), 'rb').read()
            for t, nm, d in bar(b):
                if t != 2 or len(d) < 8: continue
                cnt = struct.unpack_from('<I', d, 4)[0]
                for i in range(cnt):
                    if 8 + 8 * i + 8 > len(d): break
                    mid, off = struct.unpack_from('<II', d, 8 + 8 * i)
                    try: _MSG[w][mid & 0xffff] = md.decode(d, off)[0] if isinstance(md.decode(d, off), tuple) else md.decode(d, off)
                    except Exception: pass
    out = []
    for ty, pl in ev.cmds:
        if ty == 0x15 and len(pl) >= 8:
            fr, line, mid, hide = struct.unpack_from('<HHHH', pl, 0)
            txt = _MSG[w].get(mid)
            if not hide and txt and txt.strip() and txt not in out: out.append(txt.strip())
            if len(out) >= n: break
    return out


def cmd_list(tree, graph, a):
    for ev, k in select(tree, graph, a.set):
        who = graph.started.get((ev.area, ev.name)) or graph.named.get((ev.area, ev.name)) or []
        names = ' '.join('%s:%#x/t%d/type%d' % w for w in who[:4]) + (' +%d' % (len(who) - 4) if len(who) > 4 else '')
        how = ''
        if k == 'reachable':
            kind, p, trig, ty = who[0]; ar = tree.areas[ev.area]
            how = graph.run.get((ar.world, ar.num, kind, p), '')
        txt = subtitles(tree, ev, a.text, a.msgdump)
        print('%s/%s\t%s\t%s\tframes=%d\troxas=%s\tsubtitles=%d\t%s\t%s\t%s%s' % (ev.area, ev.name, ev.project, k, ev.end, 'yes' if ev.has_roxas() else 'no',
              ev.subs, names or '-', how or '-', ','.join(ev.models()) or '-', ('\t' + ' | '.join(txt)) if a.text else ''))
    return 0


def refs_of(tree, evs, existing_only=False):
    """-> ordered dict relative name (as on disk when it exists) -> (kind, exists, set of events)"""
    out = collections.OrderedDict()
    for ev in evs:
        for kind, rel in ev.files():
            real = tree.find(rel) if '/' in rel else None
            if kind.endswith('-opt') and not real: continue
            if kind == 'layout' and not real: continue          # the game tries the language folder first, then the common one
            key = real or rel
            if existing_only and not real: continue
            e = out.setdefault(key, [kind, bool(real), set()]); e[2].add('%s/%s' % (ev.area, ev.name))
    return out


def cmd_refs(tree, graph, a):
    if a.events:
        want = set(x.lower() for x in a.events); evs = [ev for ev in all_events(tree) if '%s/%s' % (ev.area, ev.name) in want]
        missing = want - {'%s/%s' % (ev.area, ev.name) for ev in evs}
        if missing: die('no such event: %s' % ' '.join(sorted(missing)))
    else: evs = [ev for ev, k in select(tree, graph, a.set)]
    refs = refs_of(tree, evs, a.existing); nmiss = 0; total = 0
    for rel, (kind, real, who) in refs.items():
        size = os.path.getsize(os.path.join(tree.root, rel)) if real else -1
        rm = remastered_of(tree, rel) if real else None
        rsize = sum(os.path.getsize(os.path.join(dp, f)) for dp, dn, fn in os.walk(os.path.join(tree.root, rm)) for f in fn) if rm else 0
        if not real: nmiss += 1
        total += max(size, 0) + rsize
        print('%s\t%s\t%d\t%s\t%d\t%d' % (kind, rel, size, rm or '-', rsize, len(who)))
    sys.stderr.write('event_refs: %d events, %d files named, %d not in the tree, %.1f MiB with the remastered counterparts\n' % (len(evs), len(refs), nmiss, total / 1048576.0))
    return 0


def room_music(tree, area, kind_progs):
    """what the room's own programs say about music: {(kind, program): (field, battle)} from the Bgm command"""
    out = {}
    for kind, progs in area.scripts.items():
        for pid, cmds in progs.items():
            for op, pl in cmds:
                if op == 0x10 and len(pl) >= 4: out[(kind, pid)] = struct.unpack_from('<HH', pl, 0)
    return out


def next_music(tree, area, kind, pid, trig):
    """the Jump that follows the event in the same AreaSettings and the music command of the program it leads to"""
    for op, pl in area.scripts[kind][pid]:
        if op != 0x0c: continue
        t, key, subs = settings(pl)
        if t != trig: continue
        for s in subs:
            if s[0] == 'jump':
                ty, w, ar, ent, ls, fade = s[1:]
                if ty == 2:
                    wi = w if 0 <= w < len(WORLDS) else WORLDS.index(area.world)
                    n = tree.localset.get(wi, {}).get(ls); dst = tree.areas.get('%s%02d' % (WORLDS[wi], n)) if n is not None else None
                    if dst is None: return 'jump to program %#x of world %s (no such room)' % (ls, WORLDS[wi]), None
                    m = room_music(tree, dst, None); found = [m[(k, ls)] for k in ('map', 'btl', 'evt') if (k, ls) in m]
                    pairs = tree.arif.get((wi, n), [])
                    return '%s program %#x' % (dst.name, ls), (found[-1] if found else None, pairs)
                if ty == 1:
                    wi = w if 0 <= w < len(WORLDS) else WORLDS.index(area.world)
                    return '%s%02d with the save\'s programs' % (WORLDS[wi], ar), (None, tree.arif.get((wi, ar), []))
                return 'jump type %d' % ty, None
    return 'no jump', None


def cmd_music(tree, graph, a):
    n = 0; touched = 0
    for ev, k in select(tree, graph, a.set):
        n += 1
        area = tree.areas[ev.area]; who = graph.started.get((ev.area, ev.name)) or graph.named.get((ev.area, ev.name)) or []
        plays = [b for b in ev.bgm if b[0] == 'play']; vols = [b for b in ev.bgm if b[0] == 'volume']; reads = [b for b in ev.bgm if b[0] == 'read']
        if not ev.bgm and not a.all: continue
        touched += 1
        print('%s/%s %s (%s, %d frames): reads %s; plays %s; %d volume commands%s' % (ev.area, ev.name, ev.project, k, ev.end,
              ','.join(str(b[2]) for b in reads) or '-', ' '.join('track %d slot %d at frame %d' % (b[3], b[2], b[1]) for b in plays) or '-', len(vols),
              (' (last: frame %d flag %d a=%d b=%d time %d slot %d)' % vols[-1][1:]) if vols else ''))
        rm = room_music(tree, area, None)
        for kind, pid, trig, ty in who[:6]:
            here = rm.get((kind, pid)); dst, nm = next_music(tree, area, kind, pid, trig)
            line = '    named by %s program %#x trigger %d type %d; the program\'s own Bgm command: %s; then %s' % (kind, pid, trig, ty,
                   '%d/%d' % here if here else 'none', dst)
            if nm:
                cmdp, pairs = nm
                line += ': its Bgm command %s, the room\'s table pairs %s' % ('%d/%d' % cmdp if cmdp else 'none',
                        ' '.join('%d/%d' % p for p in pairs if p != (0, 0)) or 'none')
            print(line)
    sys.stderr.write('event_refs: %d events in the set, %d with music commands\n' % (n, touched))
    return 0


# ---------------------------------------------------------------------------------------------------------------- music after a scene
# What the exe does with the music around a scene (read from kh2fm.exe; task 32 item 6b):
#   music manager, two slots of 16 bytes at 0x140abad40: +0 the track the slot is to play, +4 the track whose file it holds, +8 the
#   volume word, +0xc flags (1 = load the file, 2 = a new track was set).
#   0x155840 (called by the room loader 0x152680 when a room's FIELD starts, after the room's area file was read): the room's pair
#     (u16 field, u16 battle at 0x142a113b8: 03system.bin `arif` by the world's music set, replaced by a program's Bgm command 0x10)
#     goes into the slots.  For each slot: a track different from what the slot plays or holds -> fade the slot out (0xf0) and, if
#     not 0, request its file and mark it new; track 0 -> the slot's file is dropped (silence).  The same track -> nothing happens.
#   0x155980 (right after): files are loaded; a new slot-0 track starts at full volume (0x3000).  An unchanged slot-0 track keeps
#     playing: if its volume is not full it is brought back to full (0x1dc2c0 / 0x1dc290) - UNLESS 0x2cf120 (byte 0x140b64f9d, the
#     event's "keep my volume" flag) is set, then it stays at the volume the scene left.  Slot-0 track 0: silence.
#   Event commands: 0x2f read music (0x2cf220: loads bgm/musicNNN into the scene's memory), 0x30 play (0x2cf3b0: slot 0, 1 or 2 =
#     both: slot track = file = T, volume word 0, playback starts), 0x2e volume (0x2cf490 per frame; 0x2cf5d0 in the load pass):
#     target = 0x3000 for a=b=0, 0x1800 for a=b=1, else {0x3000, 0x1800, 0}[b], fade time {0x14, 0x28, 0x50, 1}[time] * 8; a
#     non-zero flag word sets the keep flag (cleared when an event starts: 0x2c8794, 0x2cc42c, 0x2cc5af, 0x2ce9e0).  The load pass
#     records each slot's LAST target; at the scene's end or skip, 0x2cf640 writes those into the slots when the keep flag is set.
# Consequence: after a scene that is followed by a jump, the next field start applies the destination room's pair, whatever the
# scene played; a stub in its place reaches the same room and the same pair.  The track after the scene is the same in both
# profiles.  What can differ: (1) the VOLUME, when the scene sets the keep flag and the destination keeps the slot's track;
# (2) continuity: when the scene played the destination's track, today it goes on from where the scene was, with a stub it starts
# at the field (or goes on from before the scene); (3) scenes after which no jump follows (no room load: the field's music is
# not re-applied by 0x155840).
VOL_TABLE = (0x3000, 0x1800, 0, 0x3f7fc505)
TIME_TABLE = (0x14, 0x28, 0x50, 1)


def vol_target(a, b):
    if a == 0 and b == 0: return 0x3000
    if a == 1 and b == 1: return 0x1800
    return VOL_TABLE[b] if b < 3 else None


def scene_music_end(ev):
    """the music manager after the scene, played to its end: (track per slot or None = unchanged, volume per slot or None, keep,
    recorded last target per slot or None)"""
    trk = [None, None]; vol = [None, None]; rec = [None, None]; keep = False
    for b in ev.bgm:
        if b[0] == 'play':
            fr, slot, t = b[1], b[2], b[3]
            for s in ((0, 1) if slot == 2 else (slot,)):
                if s < 2: trk[s] = t; vol[s] = 0
        elif b[0] == 'volume':
            fr, flag, va, vb, ti, slot = b[1:]
            if flag: keep = True
            if slot < 2:
                v = vol_target(va, vb); vol[slot] = v; rec[slot] = v
    return trk, vol, keep, rec


def naming_settings(area, kind, pid, name):
    """the AreaSettings record of program (kind, pid) that names event `name` -> (trigger, [sub-commands])"""
    for op, pl in area.scripts[kind][pid]:
        if op != 0x0c: continue
        t, key, subs = settings(pl)
        if any(s[0] == 'event' and s[2] == name for s in subs): return t, subs
    return None, []


def destination(tree, area, subs):
    """the jump of an AreaSettings record -> (text, [candidate (field, battle) pairs] or None when not derivable from the data)"""
    for s in subs:
        if s[0] != 'jump': continue
        ty, w, ar, ent, ls, fade = s[1:]
        wi = w if 0 <= w < len(WORLDS) else WORLDS.index(area.world)
        if ty == 2:
            n = tree.localset.get(wi, {}).get(ls); dst = tree.areas.get('%s%02d' % (WORLDS[wi], n)) if n is not None else None
            if dst is None: return 'program %#x of world %s (no such room)' % (ls, WORLDS[wi]), None
            m = room_music(tree, dst, None); found = sorted({m[(k, ls)] for k in ('map', 'btl', 'evt') if (k, ls) in m})
            if found: return '%s program %#x (its Bgm command)' % (dst.name, ls), found
            pairs = sorted({p for p in tree.arif.get((wi, n), []) if p != (0, 0)}) or [(0, 0)]
            return '%s program %#x (no Bgm command: the room\'s table by music set)' % (dst.name, ls), pairs
        if ty == 1:
            dst = tree.areas.get('%s%02d' % (WORLDS[wi], ar))
            pairs = set(p for p in tree.arif.get((wi, ar), []) if p != (0, 0))
            if dst:
                for (k, p), v in room_music(tree, dst, None).items(): pairs.add(v)
            return '%s%02d with the save\'s programs (the room\'s table or one of its programs\' Bgm commands)' % (WORLDS[wi], ar), sorted(pairs) or [(0, 0)]
        if ty == 3: return 'back to the previous room', None
        if ty == 4: return 'the world map', None
        return 'jump type %d' % ty, None
    return None, None


def hexv(v): return '-' if v is None else '0x%x' % v


def cmd_after(tree, graph, a):
    """item 6b: per scene that touches the music, what plays after it today and with a stub in its place (profile B)"""
    rows = 0; diff = collections.Counter(); out_diff = []
    for ev, k in select(tree, graph, a.set):
        if not ev.bgm: continue
        trk, vol, keep, rec = scene_music_end(ev)
        area = tree.areas[ev.area]; who = graph.started.get((ev.area, ev.name)) or graph.named.get((ev.area, ev.name)) or []
        mod_stub = [t for t, _ in ev.cmds if t not in (0x2e, 0x2f, 0x30)] == [0x00, 0x0f, 0x10, 0x08, 0x12]
        for kind, pid, trig, ty in who:
            rows += 1
            t, subs = naming_settings(area, kind, pid, ev.name)
            dtext, pairs = destination(tree, area, subs)
            fields = sorted({p[0] for p in pairs}) if pairs else None
            if mod_stub:
                verdict = 'SAME: the mod\'s own stub with a music command; KHNMC_EVENTS=stub passes it through unchanged'; diff['same (mod stub with music, passed through)'] += 1
            elif dtext is None:
                if trk[0] is not None or trk[1] is not None or keep or vol[0] is not None:
                    verdict = 'DIFFERS: no jump follows (no room load re-applies the room\'s music): today the field goes on with what the scene left (slot0 %s at %s), a stub leaves the music as it was before the scene' % (trk[0], hexv(vol[0]))
                    diff['differs: no jump'] += 1; out_diff.append((ev, kind, pid, trig, verdict))
                else: verdict = 'SAME: no jump, and the scene changes no track and no volume'; diff['same'] += 1
            elif pairs is None:
                verdict = 'SAME track: %s chooses the music the same way after a scene and after a stub' % dtext; diff['same (world map / back)'] += 1
            else:
                kept_vol = keep and rec[0] is not None and rec[0] != 0x3000
                same_trk = trk[0] is not None and fields and all(f == trk[0] for f in fields)
                can_trk = trk[0] is not None and fields and trk[0] in fields          # one of the room's candidate tracks (music set / save)
                maybe_trk = trk[0] is None
                if kept_vol and (can_trk or maybe_trk):
                    verdict = ('DIFFERS in volume: the scene sets the keep flag and leaves slot 0 at %s; today the room\'s track stays at that volume %s, '
                               'with a stub it plays at full (0x3000)') % (hexv(rec[0]),
                               ('(track %d)' % trk[0]) if same_trk else ('when the room\'s track is %d, the scene\'s own (candidates %s)' % (trk[0], '/'.join(str(f) for f in fields)))
                               if can_trk else ('when the room\'s track (%s) is the one that played before the scene' % '/'.join(str(f) for f in fields)))
                    diff['differs: kept volume'] += 1; out_diff.append((ev, kind, pid, trig, verdict))
                elif can_trk:
                    verdict = 'SAME track %d at full%s; today it goes on from the scene, with a stub the room starts it (or it goes on from before the scene)' % (
                              trk[0], '' if same_trk else ' (when the room\'s track is %d; candidates %s)' % (trk[0], '/'.join(str(f) for f in fields)))
                    diff['same track (continuity differs)'] += 1
                else:
                    verdict = 'SAME: the room\'s track %s starts at the field in both' % '/'.join(str(f) for f in fields) if fields else 'SAME'; diff['same'] += 1
            if a.verbose or verdict.startswith('DIFFERS'):
                print('%s/%s %s [%s %#x trigger %d]: scene end slot0 track %s vol %s, slot1 track %s vol %s, keep %s; then %s %s\n    -> %s' % (
                      ev.area, ev.name, ev.project, kind, pid, trig, trk[0], hexv(vol[0]), trk[1], hexv(vol[1]), 'yes (last %s/%s)' % (hexv(rec[0]), hexv(rec[1])) if keep else 'no',
                      dtext or 'no jump', ' '.join('%d/%d' % p for p in pairs) if pairs else '', verdict))
    print('scene/program pairs that touch the music: %d; %s' % (rows, ', '.join('%s %d' % kv for kv in sorted(diff.items()))))
    return 0


def cmd_selfcheck(tree, graph, a):
    bad = 0; n = stubs = 0; kinds = collections.Counter(); miss = collections.Counter()
    for ev in all_events(tree):
        n += 1
        if ev.tail != 2: print('FAIL: %s/%s does not end in a zero-length command at its end (tail %d)' % (ev.area, ev.name, ev.tail)); bad += 1
        if any(t > 0x53 for t, _ in ev.cmds): print('FAIL: %s/%s has a command type above 0x53' % (ev.area, ev.name)); bad += 1
        if ev.stub:
            stubs += 1
            if len(ev.data) != STUB_LEN: print('FAIL: stub %s/%s is %d bytes' % (ev.area, ev.name, len(ev.data))); bad += 1
            continue
        for kind, rel in ev.files():
            if kind.endswith('-opt') or kind == 'layout' or kind == 'map': continue
            kinds[kind] += 1
            if '/' not in rel or not tree.find(rel): miss[kind] += 1
    print('selfcheck: %d events, %d stubs (%d bytes each), %d real' % (n, stubs, STUB_LEN, n - stubs))
    for k in sorted(kinds): print('selfcheck: %-8s %6d references, %5d not in the tree' % (k, kinds[k], miss[k]))
    for k in ('anim-odd', 'anim-no-actor', 'actor-unknown-objentry'):
        if kinds[k]: print('FAIL: %d references of kind %s (a decoder miss)' % (kinds[k], k)); bad += 1
    for a2 in tree.areas.values():
        for kind, progs in a2.scripts.items():
            for pid, cmds in progs.items():
                for op, pl in cmds:
                    if op == -1: print('FAIL: %s %s program %#x has a command that runs past its end' % (a2.name, kind, pid)); bad += 1
                    if op == 0x0c and any(s[0] == 'bad' for s in settings(pl)[2]):
                        print('FAIL: %s %s program %#x: unknown AreaSettings sub-command' % (a2.name, kind, pid)); bad += 1
    # the effect rule of exe 0x2d9130 (effect_file): fixed cases; the file the rule gives for its own key must be in the tree; and
    # every effect name an event of the tree holds (area files and the rest), mapped by the rule, must be a file of the tree
    for name, lang, want in (('tt_event_012', 'us', 'event/effect/tt_event_012_2.pax'), ('tt_event_012', 'jp', 'event/effect/tt_event_012.pax'),
                             ('tt_event_101', 'us', 'event/effect/tt_event_101.pax'), ('tt_event_410_2', 'us', 'event/effect/tt_event_410_2.pax')):
        if effect_file(name, lang) != want: print('FAIL: effect_file(%s, %s) = %s, the exe builds %s' % (name, lang, effect_file(name, lang), want)); bad += 1
    if not tree.find(effect_file(EFFECT_2_KEY, tree.lang)):
        print('FAIL: the rule\'s own file %s is not in the tree' % effect_file(EFFECT_2_KEY, tree.lang)); bad += 1
    names = set(e for ev in list(all_events(tree)) + list(other_events(tree)) for e in ev.effects)
    rule = [n for n in sorted(names) if effect_file(n, tree.lang) != 'event/effect/%s.pax' % n]
    nmiss = 0
    for n in sorted(names):
        if not tree.find(effect_file(n, tree.lang)):
            print('FAIL: effect %s: the rule gives %s, which is not in the tree' % (n, effect_file(n, tree.lang))); bad += 1; nmiss += 1
    print('selfcheck: effect rule (0x2d9130): %d effect names held by events, %d take _2.pax%s, %d files missing; %s is in the tree' % (
          len(names), len(rule), (' (%s)' % ' '.join(rule)) if rule else '', nmiss, effect_file(EFFECT_2_KEY, tree.lang)))
    # the Theater table: every row decoded, every scene row's programs either mapped or counted
    st = graph.theater_stats
    if st['scene rows'] + st['heading rows'] != len(tree.eventviewer) or not st['scene rows']:
        print('FAIL: eventviewer.bar: %d rows, %d scene + %d heading counted' % (len(tree.eventviewer), st['scene rows'], st['heading rows'])); bad += 1
    # every event a Theater program names must be an entry of its area file, and the tree's first two scene rows (titles 0x4f0a: world
    # tt, programs 0xe5 / 0xe6; 0x4f15: tt, 0xe7, a program no heading row names) must reach tt01's scene 100 and its summary 100s, and
    # tt01/101 (decoded by hand from eventviewer.bar and tt01.ard); only checked when the tree has those rows
    absent = [k for k in graph.theater if k[1] not in tree.areas[k[0]].first]
    if absent: print('FAIL: %d events a Theater program names are not in their area file, e.g. %s' % (len(absent), absent[:3])); bad += 1
    for row, want in (((1, 0x4f0a, 2, 0xe5, 0xe6), (('tt01', '100'), ('tt01', '100s'))), ((1, 0x4f15, 2, 0xe7, 0xe7), (('tt01', '101'),))):
        if any(r[:5] == row for r in tree.eventviewer) and not all(k in graph.theater and any(m == row[1] for w, p, m in graph.theater[k]) for k in want):
            print('FAIL: the Theater row %#x (programs %#x / %#x) does not reach %s' % (row[1], row[3], row[4], ' '.join('/'.join(k) for k in want))); bad += 1
    print('selfcheck: Theater: %d rows (%d scenes), %d programs mapped, %d program ids without a room, %d events named' % (
          len(tree.eventviewer), st['scene rows'], st['programs mapped'], st['program ids without a room or program'], len(graph.theater)))
    print('selfcheck: %s' % ('FAILED (%d)' % bad if bad else 'ok'))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True); ap.add_argument('--lang', default='us'); ap.add_argument('--flags', default='story', choices=['story', 'all'])
    sub = ap.add_subparsers(dest='cmd')
    s = sub.add_parser('summary'); s.add_argument('--by', default='file', choices=['file', 'project'])
    sets = ['reachable', 'real', 'named', 'unnamed', 'never', 'stub', 'all', 'theater', 'theater-only']
    s = sub.add_parser('list'); s.add_argument('--set', default='reachable', choices=sets)
    s.add_argument('--text', type=int, default=0, help='also the first N subtitle lines of each scene (decoded with --msgdump)')
    s.add_argument('--msgdump', default='/home/user/projects/khnmc-decomp-edition/tools/mod/msgdump.py', help='the project\'s message decoder (read only)')
    s = sub.add_parser('refs'); s.add_argument('--set', default='reachable', choices=sets); s.add_argument('--existing', action='store_true'); s.add_argument('events', nargs='*')
    s = sub.add_parser('music'); s.add_argument('--set', default='reachable', choices=sets); s.add_argument('--all', action='store_true')
    s = sub.add_parser('after'); s.add_argument('--set', default='reachable', choices=sets); s.add_argument('--verbose', action='store_true')
    sub.add_parser('selfcheck')
    a = ap.parse_args()
    if not a.cmd: ap.print_usage(sys.stderr); return 2
    tree = Tree(a.data, a.lang); graph = Graph(tree, a.flags)
    return {'summary': cmd_summary, 'list': cmd_list, 'refs': cmd_refs, 'music': cmd_music, 'after': cmd_after, 'selfcheck': cmd_selfcheck}[a.cmd](tree, graph, a)


if __name__ == '__main__':
    try: sys.exit(main())
    except BrokenPipeError: sys.exit(0)
