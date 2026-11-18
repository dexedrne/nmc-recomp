#!/usr/bin/env python3
"""room_sweep.py : the helper of tools/room_sweep.sh (task 33): plan the warp lists, parse a run's output, print the room table.

  room_sweep.py plan default  DATA            one entry per area file of DATA/ard/us: `WORLD ROOM` (door 0, the save's programs)
  room_sweep.py plan localset DATA ROOMS...   one entry per 07localset id of each room (WORLD ROOM map ID btl ID evt ID: the record
                                              exe 0x39fbc0 builds from the mod's own table, all three programs = the id, door 0)
  room_sweep.py parse RUNDIR LISTFILE         the results of one khnmc run (RUNDIR/run.out, RUNDIR/exit) as JSON lines, one per list
                                              entry the run decided (finished, failed, or killed the process)
  room_sweep.py table OUTDIR [DATA]           the table of every entry of OUTDIR/state.L*.jsonl and the counts by outcome (Markdown);
                                              with DATA also the facts the exe decides by: an arif row (03system.bin) for the room, a
                                              loose map file of the room's own name, whether the room the warp ended in has a map / btl /
                                              evt program with the ids the place holds, and a cause for the failures those facts explain.
                                              A LOADING result whose place names no program of its room's area file is shown as NO-PROGRAM
                                              (decided here from the data: the runtime does not read area files)
  room_sweep.py programs DATA ROOM...         the map / btl / evt programs of area files and the missions their btl programs start
  room_sweep.py --selftest                    the parser on fixed log lines (from real runs and synthetic crash endings)

Every result after a debug warp says "this room loads from this state", not "the game reaches it".  Standard library only.
"""
import json
import os
import re
import struct
import sys

WORLDS = ['zz', 'es', 'tt', 'di', 'hb', 'bb', 'he', 'al', 'mu', 'po', 'lk', 'lm', 'dc', 'wi', 'nm', 'wm', 'ca', 'tr', 'eh']


# ---- data: BAR, area programs, 07localset --------------------------------------------------------------------------------------
def bar_entries(d):
    if d[:4] != b'BAR\x01':
        raise ValueError('not a BAR file')
    n = struct.unpack_from('<I', d, 4)[0]
    out = []
    for i in range(n):
        t, dup, name, off, size = struct.unpack_from('<HH4sII', d, 16 + 16 * i)
        out.append((t, name.rstrip(b'\0').decode('latin1'), off, size))
    return out


def area_programs(blob):
    """An area script entry (`map` / `btl` / `evt`, BAR type 13): programs [u16 id][u16 size incl. the header][commands], each command
    u16 opcode, u16 count, count u32 words; id 0xffff ends the list.  Returns [(id, [(opcode, bytes)])]."""
    out, o = [], 0
    while o + 4 <= len(blob):
        pid, size = struct.unpack_from('<HH', blob, o)
        if pid == 0xffff:
            break
        if size < 4 or o + size > len(blob):
            raise ValueError('bad program size at %#x' % o)
        cmds, c = [], o + 4
        while c + 4 <= o + size:
            op, n = struct.unpack_from('<HH', blob, c)
            cmds.append((op, blob[c + 4:c + 4 + 4 * n]))
            c += 4 + 4 * n
        out.append((pid, cmds))
        o += size
    return out


def ard_programs(path):
    d = open(path, 'rb').read()
    return {name: area_programs(d[off:off + size]) for t, name, off, size in bar_entries(d) if t == 13}


def missions(cmds):
    """opcode 0x15 (the mission): word 0 = mission id, then 32 bytes of name"""
    return [a[4:36].split(b'\0')[0].decode('latin1') for op, a in cmds if op == 0x15 and len(a) >= 36]


def localset(data):
    """07localset.bin: one `loca` entry per world (19); u32 version, u32 count, count x (u16 id, u16 room) sorted by id (exe 0x39fbc0
    bsearches it).  Returns {world: [(id, room)]} and the worlds whose count runs past their entry (read as far as the entry goes)."""
    d = open(os.path.join(data, '07localset.bin'), 'rb').read()
    res, short = {}, []
    for w, (t, name, off, size) in enumerate(bar_entries(d)):
        ver, cnt = struct.unpack_from('<II', d, off)
        fit = (size - 8) // 4
        if cnt > fit:
            short.append((WORLDS[w], cnt, fit))
        res[w] = [struct.unpack_from('<HH', d, off + 8 + 4 * i) for i in range(min(cnt, fit))]
    return res, short


def arif_rooms(data):
    """03system.bin 'arif' (exe 0x3f4e64 installs it at [0x142a10558]): u32 version, u32 worlds, per world (u16 rooms, u16 offset), 64-byte
    rows.  exe 0x3a5960 (and 0x3a58c0 / 0x3a5920) index it by the place's world and room; a room at or past the count gives NULL."""
    d = open(os.path.join(data, '03system.bin'), 'rb').read()
    for t, name, off, size in bar_entries(d):
        if name == 'arif':
            ver, nw = struct.unpack_from('<II', d, off)
            return {w: struct.unpack_from('<H', d, off + 8 + 4 * w)[0] for w in range(nw)}
    raise ValueError('03system.bin has no arif entry')


def facts(data, room):
    """(arif rows of the room's world, has an arif row, loose map file of its own name) for a room name like mu13"""
    try:
        w, r = room_of(room)
    except ValueError:
        return None
    ar = arif_rooms(data)
    return {'arif_rooms': ar.get(w, 0), 'arif_row': r < ar.get(w, 0), 'map_file': os.path.exists(os.path.join(data, 'map', room + '.map'))}


_ARD = {}


def room_programs(data, room):
    """{'map': {id: number of commands}, 'btl': ..., 'evt': ...} of DATA/ard/us/<room>.ard; None when there is no such area file"""
    if (data, room) not in _ARD:
        p = os.path.join(data, 'ard', 'us', room + '.ard')
        _ARD[(data, room)] = {k: {pid: len(c) for pid, c in v} for k, v in ard_programs(p).items()} if os.path.exists(p) else None
    return _ARD[(data, room)]


def place_room(place):
    """'0a:05' (the result line's place, hex world:room) -> 'lk05'; None when it is not a room"""
    m = re.fullmatch(r'([0-9a-f]{2}):([0-9a-f]{2})', place or '')
    if not m or int(m.group(1), 16) >= len(WORLDS):
        return None
    return '%s%02d' % (WORLDS[int(m.group(1), 16)], int(m.group(2), 16))


def program_fact(data, r):
    """Which of the place's three programs (at the result) the area file of the room the warp ended in has: {'room', 'ids', 'have':
    {'map': n commands or None, ...}, 'none': True when it has none of them}; None when the row has no place or programs"""
    room = place_room(r.get('place'))
    pr = (r.get('programs') or '').split('/')
    if room is None or len(pr) != 3:
        return None
    try:
        ids = [int(x, 0) for x in pr]
    except ValueError:
        return None
    P = room_programs(data, room)
    if P is None:
        return {'room': room, 'ids': ids, 'have': {}, 'none': True, 'no_area_file': True}
    have = {k: P.get(k, {}).get(i) for k, i in zip(('map', 'btl', 'evt'), ids)}
    return {'room': room, 'ids': ids, 'have': have, 'none': all(v is None for v in have.values())}


def fact_text(pf):
    if pf is None:
        return ''
    if pf.get('no_area_file'):
        return '%s: no area file' % pf['room']
    return '%s: %s' % (pf['room'], ', '.join('%s %#x %s' % (k, i, 'no' if pf['have'][k] is None else 'yes (%d cmds)' % pf['have'][k])
                                          for k, i in zip(('map', 'btl', 'evt'), pf['ids'])))


def shown_outcome(r, pf):
    """the outcome the table shows: the runtime's, except LOADING (STILL-LOADING in older logs) whose place names no program of the
    room's area file: NO-PROGRAM (nothing of the room runs: no spawn, no event, no player)"""
    o = r['outcome']
    if o in ('LOADING', 'STILL-LOADING') and pf is not None and pf['none']:
        return 'NO-PROGRAM'
    return o


ARIF_FAULT_RIP = '1403a5993'          # exe 0x3a5960: movzx eax, word [rdx+0x32] with rdx = NULL when the room has no arif row


def cause(r, f, pf=None):
    """what the facts say about a failed row (empty when they say nothing); the STATUS file classifies the rest by hand"""
    o = r['outcome']
    if o in LOADED:
        return ''
    if f is None:
        return ''
    if o == 'FAULT' and ARIF_FAULT_RIP in r.get('detail', '') and not f['arif_row']:
        return 'no arif row (03system arif has %d rooms for this world): exe 0x3a5960 reads NULL+0x32' % f['arif_rooms']
    if o == 'MAP-MISSING' or (not f['map_file'] and o in ('STILL-LOADING', 'LOADING', 'SCENE')):
        return 'no map file of the room\'s own name (the exe asks for map/<room>.map)'
    if pf is not None and pf['none'] and o in ('STILL-LOADING', 'LOADING', 'JUMPED-ON-NO-FIELD'):
        return '%s%s has no map, btl or evt program %s (the ids the place holds): nothing of the room runs' % (
            'the room it jumped on to, ' if o == 'JUMPED-ON-NO-FIELD' else '', pf['room'], '/'.join('%#x' % i for i in pf['ids']))
    return ''


def target_programs(r):
    """the programs the warp asked for, from the result's target text ('... programs map 0x34 btl 0x34 evt 0x34'): [int or None] x3
    (None = the save's program); None when the row has no target"""
    m = re.search(r'programs map (\S+) btl (\S+) evt (\S+)', r.get('target') or '')
    if not m:
        return None
    return [None if x == 'save' else int(x, 0) for x in m.groups()]


def what_text(r, pf=None):
    """The `what` column: what the row itself says about the end of the warp (never a fixed text).  For a room that did not come
    back to the field: where the game was (place, programs), whether that room's map file was opened, the player, the mission, the
    parts of `field` at the end (result lines since the review) and the program fact (pf, from the data)."""
    o = r['outcome']
    d = r.get('detail', '')
    if o in ('FIELD', 'FIELD+MISSION'):
        return ''
    if o == 'JUMPED-ON':
        m = re.search(r'jumped on to (\w+)', d)
        return 'jumped on to %s' % (m.group(1) if m else '?')
    if o in ('FAULT', 'FATAL'):
        m = re.match(r'(SIGNAL \d+ \([^)]*\) addr=\S+|FATAL: [^;(]*)', d)
        rip = re.search(r'guest rip (\w+)', d)
        return '`%s`%s' % ((m.group(1) if m else d[:60]).strip(), (', guest rip %s' % rip.group(1)) if rip else '')
    if o == 'WATCHDOG':
        return d
    if o == 'MAP-MISSING':
        m = re.search(r'(map/\w+\.map) .*?\(MISS x(\d+)\)', d)
        return '%s MISS x%s' % (m.group(1), m.group(2)) if m else d[:80]
    if o == 'SCENE':
        return 'scene files %s (first %s)' % (r.get('scene_files', '?'), r.get('scene_first') or '?')
    out = []
    if o == 'JUMPED-ON-NO-FIELD':
        m = re.search(r'jumped on to (\w+)', d)
        out.append('moved on to %s' % (m.group(1) if m else '?'))
    place = place_room(r.get('place'))
    if place is not None:
        maps = r.get('maps', '')
        loaded = re.search(r'map/%s\.map\((?!MISS)' % place, maps) is not None          # opened from any source (LOOSE, a package)
        full = len(re.findall(r'\S+\([^)]*\)', maps)) >= 12                             # runtime/warp.c keeps the first 12 distinct names
        out.append('place %s programs %s, map %s' % (place, r.get('programs', '?'), 'opened' if loaded else
                                                     'not among the 12 map names the warp keeps' if full else 'not opened by this warp'))
        tp = target_programs(r)
        pr = (r.get('programs') or '').split('/')
        if tp and place == r.get('room') and len(pr) == 3 and all(t is not None for t in tp):
            try:
                if [int(x, 0) for x in pr] != tp:
                    out.append("the room's own program changed the programs (warp asked %s)" % '/'.join('%#x' % t for t in tp))
            except ValueError:
                pass
    pl = r.get('player')
    if pl is not None:
        out.append('player %s' % pl)
    ms = short_msn(r.get('mission', ''))
    if ms:
        out.append('mission %s' % ms)
    if 'end_field_cmds' in r:
        out.append('at the end: field cmds %s, windows %s (waiting %s), menu %s, jump pending %s' % (
            r['end_field_cmds'], r['end_windows'], r['end_waiting'], r['end_menu'], r['end_jump_pending']))
    if pf is not None:
        out.append(fact_text(pf))
    return '; '.join(out)


def room_of(name):
    m = re.fullmatch(r'([a-z]{2})(\d\d)', name)
    if not m or m.group(1) not in WORLDS:
        raise ValueError('not a room name: %s' % name)
    return WORLDS.index(m.group(1)), int(m.group(2))


def plan(kind, data, rooms):
    if kind == 'default':
        names = sorted(f[:-4] for f in os.listdir(os.path.join(data, 'ard', 'us')) if f.endswith('.ard'))
        return ['%s %d' % (n[:2], int(n[2:])) for n in names]
    if kind == 'localset':
        ls, short = localset(data)
        for w, cnt, fit in short:
            sys.stderr.write('room_sweep: 07localset world %s claims %d entries, its BAR entry holds %d (the exe bsearches %d)\n' % (w, cnt, fit, cnt))
        out = []
        for r in rooms:
            w, room = room_of(r)
            ids = [i for i, rm in ls[w] if rm == room]
            if not ids:
                out.append('# %s: no 07localset entry' % r)
            for i in ids:
                out.append('%s %d map %#x btl %#x evt %#x' % (WORLDS[w], room, i, i, i))
        return out
    raise ValueError('plan default|localset')


# ---- parsing a run ---------------------------------------------------------------------------------------------------------------
RE_REQ = re.compile(r'^kh: warp: #(\d+) DEBUG WARP \(a test tool, not play\) to (\S+) .*? at game frame (\d+), from ([0-9a-f]{2}:[0-9a-f]{2})')
RE_RES = re.compile(r'^kh: warp: #(\d+) result (\S+) ([A-Z+-]+): (.*?); requested at game frame (\d+), jump done at (\d+), field at (\d+) \((\d+) game frames after the request\); '
                    r'place now (\S+) door (-?\d+) programs map (\S+) btl (\S+) evt (\S+); player at (.*?); mission file (.*?); scene files (\d+)(?: first (\S+))?; '
                    r'map files: (.*?); data misses (\d+) \(probes (\d+)\); files opened (\d+); target (.*) \(list line (\d+)\)$')
# the parts of `field` at the end of the warp (runtime/warp.c since the review of task 33; older logs have none)
RE_END = re.compile(r'; at the end: field commands this frame (-?\d+), message windows (-?\d+) \(waiting (-?\d+)\), menu-open (\d+), jump pending (\d+); ')
RE_MISS = re.compile(r'^kh: warp: #(\d+) miss (.*)$')
RE_MAPREP = re.compile(r'^kh: map report \(warp #(\d+) result\): .*?id table (\d+) of 64 slots \(most ever (\d+)\); id zone high-water (\d+) MiB, in use (\d+) MiB; '
                       r'far zone high-water (\d+) MiB, in use (\d+) MiB; RSS (-?\d+) MiB \(peak (-?\d+) MiB\)')
RE_DURING = re.compile(r'^kh: warp: FAULT OR FATAL DURING DEBUG WARP #(\d+) to (.*?) \(list line (\d+)\): phase (\d+) \(([^)]*)\)')
RE_STOP = re.compile(r'^kh: warp: list stopped at entry (\d+) of (\d+) \(list line (\d+) ')
RE_FATAL = re.compile(r'^kh: (FATAL: .*|SIGNAL \d+ .*)$')
RE_RIP = re.compile(r'rip\(state\)=([0-9a-f]+)')
RE_SITE = re.compile(r'return-site rva 0x([0-9a-f]+)')
RE_WATCHDOG = re.compile(r'KH_RUN_SECONDS=(\d+) elapsed')


def parse_run(lines, list_lines, exit_code):
    """lines: run.out; list_lines: the entry text of each line of the list file (1-based dict); exit_code: int or None.
    Returns [dict] one per decided list line, in order, and a dict describing how the run ended."""
    res, order, misses, maprep = {}, [], {}, {}
    req_by_k, line_by_k = {}, {}
    during, fatal, rip, sites, stopped, watchdog = None, None, None, [], None, None
    for raw in lines:
        l = raw.rstrip('\n')
        m = RE_REQ.match(l)
        if m:
            req_by_k[int(m.group(1))] = {'requested': int(m.group(3)), 'from': m.group(4)}
            continue
        e = RE_END.search(l) if l.startswith('kh: warp: #') else None
        if e:
            l = l[:e.start()] + '; ' + l[e.end():]
        m = RE_RES.match(l)
        if m:
            k = int(m.group(1)); ln = int(m.group(23))
            line_by_k[k] = ln
            r = {'line': ln, 'k': k, 'room': m.group(2), 'outcome': m.group(3), 'detail': m.group(4), 'requested': int(m.group(5)),
                 'jumped': int(m.group(6)), 'field_at': int(m.group(7)),
                 # a failed warp's `field at` is only where the last short field run began (the old place for a frame or two): no time to field
                 'frames_to_field': int(m.group(8)) if int(m.group(7)) and m.group(3) in LOADED else None,
                 'place': m.group(9), 'door': int(m.group(10)), 'programs': '%s/%s/%s' % (m.group(11), m.group(12), m.group(13)),
                 'player': m.group(14), 'mission': m.group(15), 'scene_files': int(m.group(16)), 'scene_first': m.group(17) or '',
                 'maps': m.group(18), 'data_misses': int(m.group(19)), 'probes': int(m.group(20)), 'files': int(m.group(21)),
                 'target': m.group(22)}
            if e:
                r.update({'end_field_cmds': int(e.group(1)), 'end_windows': int(e.group(2)), 'end_waiting': int(e.group(3)),
                          'end_menu': int(e.group(4)), 'end_jump_pending': int(e.group(5))})
            res[ln] = r; order.append(ln)
            continue
        m = RE_MISS.match(l)
        if m:
            misses.setdefault(int(m.group(1)), []).append(m.group(2)); continue
        m = RE_MAPREP.match(l)
        if m:
            maprep[int(m.group(1))] = {'id_slots': int(m.group(2)), 'id_most': int(m.group(3)), 'idzone_hw': int(m.group(4)), 'idzone_use': int(m.group(5)),
                                       'farzone_hw': int(m.group(6)), 'farzone_use': int(m.group(7)), 'rss': int(m.group(8)), 'rss_peak': int(m.group(9))}
            continue
        m = RE_DURING.match(l)
        if m and during is None:
            during = {'k': int(m.group(1)), 'line': int(m.group(3)), 'phase': int(m.group(4)), 'phase_text': m.group(5)}
            continue
        m = RE_STOP.match(l)
        if m:
            stopped = int(m.group(3)); continue
        m = RE_FATAL.match(l)
        if m and fatal is None:
            fatal = m.group(1); continue
        m = RE_RIP.search(l)
        if m and fatal is not None and rip is None:
            rip = m.group(1)
        m = RE_SITE.search(l)
        if m and fatal is not None and len(sites) < 6 and rip is not None and not sites_done(sites):
            sites.append(m.group(1))
        m = RE_WATCHDOG.search(l)
        if m:
            watchdog = int(m.group(1))
    for k, ln in line_by_k.items():
        r = res[ln]
        r['misses'] = misses.get(k, [])
        r.update(maprep.get(k, {}))
    out = [res[ln] for ln in order]
    end = {'exit': exit_code, 'fatal': fatal, 'rip': rip, 'sites': sites, 'stopped_line': stopped, 'watchdog': watchdog}
    # the entry in progress when the process ended without a result for it.  Phase 1 = the warp had not been asked for yet (the game was
    # still in the PREVIOUS room, waiting for a safe moment): the death belongs to that room, noted on its row, and the entry stays open.
    if during is not None and during['line'] not in res and during['phase'] == 1:
        what = (fatal or 'the process ended') + (('; guest rip %s, return sites %s' % (rip, ' '.join(sites))) if rip else '')
        if out:
            out[-1]['died_after'] = what
            out[-1]['detail'] += '; THE PROCESS DIED AFTERWARDS, before the next warp: ' + what
        end['died_before_warp_line'] = during['line']
    elif during is not None and during['line'] not in res:
        what = fatal or 'the process ended'
        kind = 'FAULT' if fatal and fatal.startswith('SIGNAL') else 'FATAL'
        out.append({'line': during['line'], 'k': during['k'], 'room': list_room(list_lines.get(during['line'], '')), 'outcome': kind,
                    'detail': '%s (during the warp: phase %d, %s)%s' % (what, during['phase'], during['phase_text'], ('; guest rip %s, return sites %s' % (rip, ' '.join(sites))) if rip else ''),
                    'misses': misses.get(during['k'], []), 'target': list_lines.get(during['line'], '')})
    elif watchdog is not None:
        pend = [k for k in req_by_k if k not in line_by_k]
        if pend:
            k = max(pend)
            ln = next_line(list_lines, out)
            if ln is not None:
                out.append({'line': ln, 'k': k, 'room': list_room(list_lines.get(ln, '')), 'outcome': 'WATCHDOG',
                            'detail': 'KH_RUN_SECONDS=%d elapsed during the warp (requested at game frame %d)' % (watchdog, req_by_k[k]['requested']),
                            'misses': misses.get(k, []), 'target': list_lines.get(ln, '')})
    return out, end


def sites_done(sites):
    return len(sites) >= 6


def next_line(list_lines, done):
    seen = {r['line'] for r in done}
    for ln in sorted(list_lines):
        if ln not in seen:
            return ln
    return None


def list_room(text):
    w = text.split()
    if len(w) >= 2:
        try:
            wi = WORLDS.index(w[0]) if w[0] in WORLDS else int(w[0], 0)
            return '%s%02d' % (WORLDS[wi], int(w[1], 0))
        except (ValueError, IndexError):
            pass
    return '?'


def read_list(path):
    out = {}
    for i, l in enumerate(open(path), 1):
        t = l.split('#', 1)[0].strip()
        if t:
            out[i] = t
    return out


def record(parsed, mapfile, rundir, state):
    """the decided entries of a parsed run, with their entry numbers (the list line -> entry map of the run), appended to the lane's
    state file; returns how many"""
    m = {int(a): int(b) for a, b in (l.split() for l in open(mapfile) if l.strip())}
    n = 0
    with open(state, 'a') as st:
        for l in open(parsed):
            r = json.loads(l)
            if 'end' in r:
                continue
            r['entry'] = m[r['line']]
            r['run'] = rundir
            st.write(json.dumps(r) + '\n')
            n += 1
    return n


# ---- the table ---------------------------------------------------------------------------------------------------------------------
KINDS = ['FIELD', 'FIELD+MISSION', 'JUMPED-ON', 'MESSAGE-WAITING', 'JUMPED-ON-NO-FIELD', 'MAP-MISSING', 'NO-PROGRAM', 'SCENE', 'JUMP-PENDING', 'MENU-OPEN', 'LOADING',
         'STILL-LOADING', 'JUMP-NOT-DONE', 'JUMP-NOT-STARTED', 'NO-SAFE-MOMENT', 'FATAL', 'FAULT', 'WATCHDOG', 'BOOT']
LOADED = ('FIELD', 'FIELD+MISSION', 'JUMPED-ON')


def table(outdir, data=None):
    """One line per entry.  The memory columns are the process's: a sweep process warps through up to -n rooms, and the id table, the
    zones and the RSS are read at each result: `in use` / `now` are the values at that moment, `high-water` / `peak` the marks of the whole
    process so far (every room before it in the process), not the room's own."""
    rows = []
    for f in sorted(os.listdir(outdir)):
        if re.fullmatch(r'state\.L\d+\.jsonl', f):
            rows += [json.loads(l) for l in open(os.path.join(outdir, f)) if l.strip()]
    entries = [l.rstrip('\n').split('\t', 1) for l in open(os.path.join(outdir, 'entries.tsv')) if l.strip()]
    by = {}
    for r in rows:
        by[r['entry']] = r
    counts = {}
    print('| # | room | warp | outcome | warp result | to field | mission | scene | map files | data misses | at the end: field cmds, windows (waiting), menu, jump pending '
          '| process (run, list line) | id slots now (process most) | id zone MiB: process high-water / now | far zone MiB: process high-water / now | RSS MiB now (process peak) '
          '| arif row / own map file | programs of the place in its room\'s area file | cause (from the facts) | what (from the row) | note |')
    print('|' + '---|' * 21)
    for no, text in entries:
        r = by.get(int(no))
        f = facts(data, list_room(text)) if data else None
        fc = ('%s / %s' % ('yes' if f['arif_row'] else 'NO (%d rooms)' % f['arif_rooms'], 'yes' if f['map_file'] else 'NO')) if f else ''
        if r is None:
            counts['NOT RUN'] = counts.get('NOT RUN', 0) + 1
            print('| %s | %s | `%s` | NOT RUN |' % (no, list_room(text), text) + ' |' * 14 + ' %s | | | | |' % fc)
            continue
        pf = program_fact(data, r) if data else None
        o = shown_outcome(r, pf); counts[o] = counts.get(o, 0) + 1
        mis = '; '.join(r.get('misses', []))
        note = r['detail'] if o not in ('FIELD', 'FIELD+MISSION') else r['detail'] if 'DIED AFTERWARDS' in r['detail'] else ''
        end = ('%s, %s (%s), %s, %s' % (r['end_field_cmds'], r['end_windows'], r['end_waiting'], r['end_menu'], r['end_jump_pending'])) if 'end_field_cmds' in r else ''
        print('| %s | %s | `%s` | %s | %s | %s | %s | %s | %s | %s%s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            no, r.get('room', '?'), text, o, r['outcome'], (r.get('frames_to_field') or '') if o in LOADED else '', short_msn(r.get('mission', '')), r.get('scene_files', ''),
            short_maps(r.get('maps', '')), r.get('data_misses', len(r.get('misses', []))), (': ' + mis) if mis else '', end,
            '%s %s' % (os.path.basename(r.get('run', '')), r.get('line', '')),
            '%s (%s)' % (r['id_slots'], r['id_most']) if 'id_slots' in r else '',
            '%s / %s' % (r['idzone_hw'], r['idzone_use']) if 'idzone_hw' in r else '', '%s / %s' % (r['farzone_hw'], r['farzone_use']) if 'farzone_hw' in r else '',
            '%s (%s)' % (r['rss'], r['rss_peak']) if 'rss' in r else '', fc, fact_text(pf), cause(r, f, pf), what_text(r, pf).replace('|', '/'), note.replace('|', '/')))
    print()
    print('| outcome | entries |')
    print('|---|---|')
    for k in KINDS + sorted(set(counts) - set(KINDS)):
        if k in counts:
            print('| %s | %d |' % (k, counts[k]))
    print('| total | %d |' % sum(counts.values()))
    print('| loaded (%s) | %d |' % (', '.join(LOADED), sum(counts.get(k, 0) for k in LOADED)))


def short_msn(s):
    if not s or s == 'none':
        return ''
    return ' '.join(re.sub(r'^msn/us/|\.bar\(.*?\)$', '', x) for x in re.findall(r'\S+\([^)]*\)', s))


def short_maps(s):
    if not s or s == 'none':
        return 'none'
    return ' '.join(re.sub(r' x\d+\)', ')', x) for x in re.findall(r'\S+\([^)]*\)', s))


# ---- selftest -----------------------------------------------------------------------------------------------------------------------
FIX_OK = [
    'kh: warp: #1 DEBUG WARP (a test tool, not play) to eh03 (world 18 room 3) door 0 programs map save btl save evt save: exe 0x152990({12 03 00 00 ffff ffff ffff}, 1, 0, 0, 0) at game frame 1594, from 12:02',
    'kh: map report (map 12:02 -> 12:03): game frame 1595, world 12 area 03 (door 0, programs map 0 btl 0 evt 0x3); id table 16 of 64 slots (most ever 16); id zone high-water 1013 MiB, in use 964 MiB; far zone high-water 78 MiB, in use 51 MiB; RSS 421 MiB (peak 422 MiB); files opened 147 (distinct paths); threads created 59, ended 39 (stacks released 39)',
    'kh: warp: #1 result eh03 FIELD+MISSION: the field is up and stayed up for 60 game frames; a mission started; requested at game frame 1594, jump done at 1595, field at 1660 (66 game frames after the request); place now 12:03 door 0 programs map 0x1300 btl 0x1300 evt 0x1300; player at (0, 0, 0); mission file msn/us/EH03_INTRO1.bar(LOOSE x2); scene files 0; map files: map/us/eh03.bar(LOOSE x4) map/eh03.map(LOOSE x4); data misses 0 (probes 50); files opened 137; target eh03 (world 18 room 3) door 0 programs map save btl save evt save (list line 1)',
    'kh: map report (warp #1 result): game frame 1719, world 12 area 03 (door 0, programs map 0x1300 btl 0x1300 evt 0x1300); id table 16 of 64 slots (most ever 17); id zone high-water 1014 MiB, in use 978 MiB; far zone high-water 78 MiB, in use 51 MiB; RSS 443 MiB (peak 446 MiB); files opened 152 (distinct paths); threads created 62, ended 42 (stacks released 42)',
    'kh: warp: #2 DEBUG WARP (a test tool, not play) to mu13 (world 8 room 13) door 0 programs map 0x4f btl 0x4f evt 0x4f: exe 0x152990({08 0d 00 00 004f 004f 004f}, 1, 0, 0, 0) at game frame 1962, from 12:03',
    'kh: warp: #2 result mu13 STILL-LOADING: the place is set but no field within 2400 game frames (still loading, or no player control); requested at game frame 1962, jump done at 1963, field at 0 (0 game frames after the request); place now 08:0d door 0 programs map 0x4f btl 0x4f evt 0x4f; player at none; mission file none; scene files 0; map files: map/us/mu13.bar(MISS x1) map/mu13.map(MISS x3); data misses 4 (probes 20); files opened 41; target mu13 (world 8 room 13) door 0 programs map 0x4f btl 0x4f evt 0x4f (list line 3)',
    'kh: warp: #2 miss map/us/mu13.bar (callers 3c32fc <- 2bdedd)',
    'kh: warp: #2 miss map/mu13.map (callers fddab <- 4ad3fd <- 3a0cff)',
    'kh: map report (warp #2 result): game frame 7962, world 08 area 0d (door 0, programs map 0x4f btl 0x4f evt 0x4f); id table 16 of 64 slots (most ever 16); id zone high-water 1042 MiB, in use 927 MiB; far zone high-water 79 MiB, in use 49 MiB; RSS 252 MiB (peak 464 MiB); files opened 181 (distinct paths); threads created 76, ended 56 (stacks released 56)',
    'kh: warp: list stopped at entry 2 of 3 (list line 3 `mu 13 map 0x4f btl 0x4f evt 0x4f`): STILL-LOADING: ...; the 1 entries after it are not warped in this run',
]
FIX_CRASH = [
    'kh: warp: #1 DEBUG WARP (a test tool, not play) to eh03 (world 18 room 3) door 0 programs map save btl save evt save: exe 0x152990({12 03 00 00 ffff ffff ffff}, 1, 0, 0, 0) at game frame 1414, from 12:02',
    '',
    'kh: SIGNAL 11 (Segmentation fault) addr=0x2 host-pc=0x564765a36d92',
    '---- guest: rsp=16cbe1cf0 rip(state)=14039da55 rax=0 rcx=ffff rdx=1 ----',
    '  [rsp+0x8] return-site rva 0x142f12',
    '  [rsp+0x28] return-site rva 0x39c827',
    'kh: warp: FAULT OR FATAL DURING DEBUG WARP #1 to eh03 (world 18 room 3) door 0 programs map save btl save evt save (list line 2): phase 2 (jump requested, not done), requested at game frame 1414, jump done at 0; map files so far: none; data misses so far: 0',
]
FIX_WATCHDOG = [
    'kh: warp: #4 DEBUG WARP (a test tool, not play) to tt05 (world 2 room 5) door 0 programs map save btl save evt save: exe 0x152990({02 05 00 00 ffff ffff ffff}, 1, 0, 0, 0) at game frame 9000, from 02:04',
    'kh: warn: shell: KH_RUN_SECONDS=900 elapsed, ending the run',
]


def selftest():
    checks = fails = 0

    def check(c, what):
        nonlocal checks, fails
        checks += 1
        if not c:
            fails += 1
            print('room_sweep selftest FAIL: %s' % what)
    lst = {1: 'eh 3', 2: 'mu 12', 3: 'mu 13 map 0x4f btl 0x4f evt 0x4f', 4: 'tt 5'}
    out, end = parse_run(FIX_OK, lst, 6)
    check(len(out) == 2, 'two results')
    a, b = out[0], out[1]
    check(a['line'] == 1 and a['room'] == 'eh03' and a['outcome'] == 'FIELD+MISSION' and a['frames_to_field'] == 66, 'first result fields')
    check(a['mission'] == 'msn/us/EH03_INTRO1.bar(LOOSE x2)' and a['maps'].startswith('map/us/eh03.bar(LOOSE x4)'), 'mission and maps')
    check(a['id_slots'] == 16 and a['id_most'] == 17 and a['idzone_hw'] == 1014 and a['farzone_use'] == 51 and a['rss_peak'] == 446, 'map report of warp #1 (not the map-change report)')
    check(a['target'].startswith('eh03 (world 18 room 3)'), 'target')
    check(b['line'] == 3 and b['outcome'] == 'STILL-LOADING' and b['frames_to_field'] is None and b['data_misses'] == 4 and len(b['misses']) == 2, 'second result and its misses')
    check(b['misses'][1] == 'map/mu13.map (callers fddab <- 4ad3fd <- 3a0cff)' and b['programs'] == '0x4f/0x4f/0x4f', 'miss text and programs')
    check(end['stopped_line'] == 3 and end['exit'] == 6 and end['fatal'] is None, 'list stopped at line 3')
    out, end = parse_run(FIX_CRASH, lst, 5)
    check(len(out) == 1 and out[0]['outcome'] == 'FAULT' and out[0]['line'] == 2 and out[0]['room'] == 'mu12', 'crash attributed to the entry in progress')
    check('SIGNAL 11' in out[0]['detail'] and 'phase 2' in out[0]['detail'] and '14039da55' in out[0]['detail'] and '142f12 39c827' in out[0]['detail'], 'crash detail')
    check(end['fatal'].startswith('SIGNAL 11') and end['rip'] == '14039da55' and end['sites'] == ['142f12', '39c827'], 'end of the crashed run')
    out, end = parse_run(FIX_WATCHDOG, lst, 0)
    check(len(out) == 1 and out[0]['outcome'] == 'WATCHDOG' and out[0]['line'] == 1 and end['watchdog'] == 900, 'watchdog during a warp: the first undecided line')
    crash1 = FIX_OK[:4] + ['kh: warp: FAULT OR FATAL DURING DEBUG WARP #2 to mu12 (world 8 room 12) door 0 programs map save btl save evt save (list line 2): phase 1 (waiting for a safe moment), requested at game frame 0, jump done at 0; map files so far: none; data misses so far: 0',
                            'kh: FATAL: something broke']
    crash1 = [crash1[i] for i in (0, 1, 2, 3, 5, 4)]
    out, end = parse_run(crash1, lst, 3)
    check(len(out) == 1 and out[0]['line'] == 1 and out[0].get('died_after') == 'FATAL: something broke' and 'THE PROCESS DIED AFTERWARDS' in out[0]['detail'] and end['died_before_warp_line'] == 2,
          'a death before the next warp is noted on the previous room, the next entry stays open')
    out, end = parse_run([], lst, 3)
    check(out == [] and end['exit'] == 3, 'a run with no warp decides nothing')
    check(list_room('mu 13 map 0x4f') == 'mu13' and list_room('8 5') == 'mu05' and list_room('xx 1') == '?', 'list_room')
    check(short_maps('map/us/mu13.bar(MISS x1) map/mu13.map(MISS x3)') == 'map/us/mu13.bar(MISS) map/mu13.map(MISS)', 'short_maps')
    check(short_msn('msn/us/EH03_INTRO1.bar(LOOSE x2)') == 'EH03_INTRO1', 'short_msn')
    check(room_of('lk16') == (10, 16) and room_of('lm03') == (11, 3), 'room_of')
    try:
        room_of('qq01'); check(False, 'room_of refuses an unknown world')
    except ValueError:
        check(True, '')
    progs = area_programs(bytes.fromhex('4f00 1800 0500 0100 00003000 1500 0200 97010000 4d553133 ffff0000'.replace(' ', '')))
    check(progs == [(0x4f, [(5, bytes.fromhex('00003000')), (0x15, bytes.fromhex('970100004d553133'))])], 'area program decoding')
    try:
        area_programs(bytes.fromhex('01004000ffff0000')); check(False, 'a program running past its entry is refused')
    except ValueError:
        check(True, '')
    # the outcomes of a room that jumps on by itself and of a room without its map file
    fix = ['kh: warp: #3 result bb00 JUMPED-ON: the room loaded and its own program jumped on to bb01 (05:01); the field is up there and stayed up for 60 game frames; a mission started; '
           'requested at game frame 2688, jump done at 2689, field at 2900 (212 game frames after the request); place now 05:01 door 50 programs map 0 btl 0 evt 0x14; player at (0, 0, -75); '
           'mission file msn/us/BB01_MS101.bar(LOOSE x2); scene files 0; map files: map/us/bb00.bar(LOOSE x2) map/bb00.map(LOOSE x2) map/bb01.map(LOOSE x6); data misses 0 (probes 9); files opened 327; '
           'target bb00 (world 5 room 0) door 0 programs map save btl save evt save (list line 1)',
           'kh: warp: #4 result mu13 MAP-MISSING: the place is set, the exe asked for the room\'s map file map/mu13.map (named after the room by 0x152ed0) and neither the loose tree nor the packages have it (MISS x3); '
           'no field within 2400 game frames; requested at game frame 3561, jump done at 3562, field at 3562 (1 game frames after the request); place now 08:0d door 0 programs map 0 btl 0 evt 0; player at none; '
           'mission file none; scene files 0; map files: map/us/mu13.bar(MISS x1) map/mu13.map(MISS x3); data misses 4 (probes 20); files opened 41; target mu13 (world 8 room 13) door 0 programs map save btl save evt save (list line 2)']
    out, end = parse_run(fix, {1: 'bb 0', 2: 'mu 13'}, 6)
    check(len(out) == 2 and out[0]['outcome'] == 'JUMPED-ON' and out[0]['place'] == '05:01' and out[0]['frames_to_field'] == 212 and out[1]['outcome'] == 'MAP-MISSING'
          and out[1]['frames_to_field'] is None and 'map/mu13.map' in out[1]['detail'], 'JUMPED-ON and MAP-MISSING results')
    # the facts the exe decides by, on a synthetic data tree: arif (03system.bin) and the map directory
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        arif = struct.pack('<II', 1, 19) + b''.join(struct.pack('<HH', (14 if w == 8 else 10 if w == 9 else 2), 0) for w in range(19))
        hdr = b'BAR\x01' + struct.pack('<I', 2) + b'\0' * 8
        ent = struct.pack('<HH4sII', 2, 0, b'xxxx', 48, 4) + struct.pack('<HH4sII', 2, 0, b'arif', 52, len(arif))
        open(os.path.join(td, '03system.bin'), 'wb').write(hdr + ent + b'\0' * 4 + arif)
        os.mkdir(os.path.join(td, 'map')); open(os.path.join(td, 'map', 'mu12.map'), 'wb').close()
        ar = arif_rooms(td)
        check(ar[8] == 14 and ar[9] == 10 and ar[18] == 2, 'arif room counts')
        f13, f12, f10 = facts(td, 'mu13'), facts(td, 'mu12'), facts(td, 'po10')
        check(f13 == {'arif_rooms': 14, 'arif_row': True, 'map_file': False} and f12['map_file'] and not f10['arif_row'] and facts(td, 'qq01') is None, 'facts')
        crash = {'outcome': 'FAULT', 'detail': 'SIGNAL 11 (Segmentation fault) addr=0x32 (during the warp: phase 3); guest rip 1403a5993, return sites 39c942 1526e8'}
        check(cause(crash, f10).startswith('no arif row (03system arif has 10 rooms') and cause(crash, f13) == '' and cause(out[1], f13).startswith('no map file')
              and cause(out[0], f12) == '' and cause({'outcome': 'STILL-LOADING', 'detail': ''}, f12) == '', 'causes from the facts')
        # the table: one line per entry (NOT RUN when undecided), the counts, the loaded count
        od = os.path.join(td, 'sweep'); os.mkdir(od)
        open(os.path.join(od, 'entries.tsv'), 'w').write('1\tbb 0\n2\tmu 13\n3\tpo 10\n')
        with open(os.path.join(od, 'state.L0.jsonl'), 'w') as st:
            for i, r in enumerate(out):
                r = dict(r); r['entry'] = i + 1; st.write(json.dumps(r) + '\n')
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            table(od, td)
        t = buf.getvalue()
        check('| 1 | bb00 | `bb 0` | JUMPED-ON | JUMPED-ON | 212 |' in t and '| 3 | po10 | `po 10` | NOT RUN |' in t and '| NO (10 rooms) / NO |' in t and '| JUMPED-ON | 1 |' in t
              and '| MAP-MISSING | 1 |' in t and '| NOT RUN | 1 |' in t and '| total | 3 |' in t and '| loaded (%s) | 1 |' % ', '.join(LOADED) in t
              and "no map file of the room's own name" in t, 'table lines and counts')
        if 'NOT RUN | 1' not in t:
            print(t)
        # the parts of `field` at the end (new result lines) and an older line without them
        new = fix[1].replace('; player at none; ', '; player at (45, -0, -4014); at the end: field commands this frame 60, message windows 2 (waiting 1), menu-open 0, '
                                                    'jump pending 0; ').replace('MAP-MISSING', 'MESSAGE-WAITING')
        o2, _ = parse_run([new], {1: 'bb 0', 2: 'mu 13'}, 6)
        check(len(o2) == 1 and o2[0]['outcome'] == 'MESSAGE-WAITING' and o2[0]['player'] == '(45, -0, -4014)' and o2[0]['end_field_cmds'] == 60 and o2[0]['end_windows'] == 2
              and o2[0]['end_waiting'] == 1 and o2[0]['end_menu'] == 0 and o2[0]['end_jump_pending'] == 0 and o2[0]['mission'] == 'none', 'the parts of the field at the end')
        o3, _ = parse_run([fix[1]], {2: 'mu 13'}, 6)
        check(len(o3) == 1 and 'end_field_cmds' not in o3[0] and o3[0]['player'] == 'none', 'an older result line without the parts still parses')
        # the program fact: does the room the warp ended in have a program with the ids the place holds (exe 0x3a2170 runs them)
        def prog(pid, ncmd):
            body = b''.join(struct.pack('<HHI', 1, 1, 0) for _ in range(ncmd))
            return struct.pack('<HH', pid, 4 + len(body)) + body
        blobs = [('map', prog(0x10, 2) + prog(0x3b, 1) + b'\xff\xff\0\0'), ('evt', prog(0x3b, 3) + b'\xff\xff\0\0')]
        hdr2 = b'BAR\x01' + struct.pack('<I', len(blobs)) + b'\0' * 8
        off = 16 + 16 * len(blobs); ents = b''; body = b''
        for name, b in blobs:
            ents += struct.pack('<HH4sII', 13, 0, name.encode(), off + len(body), len(b)); body += b
        os.makedirs(os.path.join(td, 'ard', 'us'))
        open(os.path.join(td, 'ard', 'us', 'lk00.ard'), 'wb').write(hdr2 + ents + body)
        check(room_programs(td, 'lk00') == {'map': {0x10: 2, 0x3b: 1}, 'evt': {0x3b: 3}} and room_programs(td, 'lk01') is None, 'programs of an area file')
        check(place_room('0a:05') == 'lk05' and place_room('12:1d') == 'eh29' and place_room('ff:ff') is None and place_room(None) is None, 'place_room')
        lo = {'outcome': 'LOADING', 'place': '0a:00', 'programs': '0x45/0x45/0x45', 'detail': ''}
        pf = program_fact(td, lo)
        check(pf['room'] == 'lk00' and pf['none'] and shown_outcome(lo, pf) == 'NO-PROGRAM' and 'lk00 has no map, btl or evt program 0x45/0x45/0x45' in cause(lo, f12, pf), 'NO-PROGRAM')
        lo2 = dict(lo, programs='0x10/0x45/0x45'); pf2 = program_fact(td, lo2)
        check(not pf2['none'] and shown_outcome(lo2, pf2) == 'LOADING' and fact_text(pf2) == 'lk00: map 0x10 yes (2 cmds), btl 0x45 no, evt 0x45 no', 'a program present: LOADING stays')
        pf3 = program_fact(td, dict(lo, programs='0x45/0x45/0x3b'))
        check(not pf3['none'] and fact_text(pf3) == 'lk00: map 0x45 no, btl 0x45 no, evt 0x3b yes (3 cmds)', 'each id looked up in its own kind')
        st = dict(lo, outcome='STILL-LOADING'); ms = dict(lo, outcome='MESSAGE-WAITING')
        check(shown_outcome(st, pf) == 'NO-PROGRAM' and shown_outcome(ms, pf) == 'MESSAGE-WAITING' and shown_outcome(lo, None) == 'LOADING', 'only LOADING becomes NO-PROGRAM')
        jn = {'outcome': 'JUMPED-ON-NO-FIELD', 'place': '0a:00', 'programs': '0x56/0x56/0x56', 'detail': ''}
        check(cause(jn, f13, program_fact(td, jn)).startswith('the room it jumped on to, lk00 has no map, btl or evt program 0x56') and shown_outcome(jn, program_fact(td, jn)) == 'JUMPED-ON-NO-FIELD', 'a jump on into a room without the program')
        check(program_fact(td, {'outcome': 'FAULT', 'detail': ''}) is None and program_fact(td, dict(lo, place='0a:01'))['no_area_file'], 'no place / no area file')
        # the table shows NO-PROGRAM and counts it
        open(os.path.join(od, 'entries.tsv'), 'a').write('4\tlk 0 map 0x45 btl 0x45 evt 0x45\n')
        with open(os.path.join(od, 'state.L1.jsonl'), 'w') as st2:
            st2.write(json.dumps(dict(lo, entry=4, room='lk00', line=1, run='/x/runs/L1-002')) + '\n')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            table(od, td)
        t = buf.getvalue()
        check('| 4 | lk00 | `lk 0 map 0x45 btl 0x45 evt 0x45` | NO-PROGRAM | LOADING |' in t and '| NO-PROGRAM | 1 |' in t and '| total | 4 |' in t
              and 'lk00: map 0x45 no, btl 0x45 no, evt 0x45 no' in t and '| L1-002 1 |' in t, 'the table: NO-PROGRAM from the data')
        if '| NO-PROGRAM | 1 |' not in t:
            print(t)
        # the memory columns are the process's marks (a process warps through several rooms), and say so
        check('id zone MiB: process high-water / now' in t and 'RSS MiB now (process peak)' in t, 'memory columns labelled as the process\'s')
        # the `what` column is built from the row: lm02 0x34 of the sweep (player there, mission open, a window waiting) is not
        # "player none"; an old STILL-LOADING row says what it holds; a program changed inside the room is named
        mw = {'outcome': 'MESSAGE-WAITING', 'room': 'lm02', 'detail': 'the place is set but a field message window waits', 'place': '0b:02',
              'programs': '0x34/0x34/0x34', 'player': '(45, -0, -4014)', 'mission': 'msn/us/LM02_MS101.bar(LOOSE x2)',
              'maps': 'map/us/lm02.bar(LOOSE x2) map/lm02.map(LOOSE x2)', 'end_field_cmds': 60, 'end_windows': 2, 'end_waiting': 1, 'end_menu': 0,
              'end_jump_pending': 0, 'target': 'lm02 (world 11 room 2) door 0 programs map 0x34 btl 0x34 evt 0x34'}
        w1 = what_text(mw)
        check('player (45, -0, -4014)' in w1 and 'player none' not in w1 and 'mission LM02_MS101' in w1 and 'place lm02 programs 0x34/0x34/0x34, map opened' in w1
              and 'at the end: field cmds 60, windows 2 (waiting 1), menu 0, jump pending 0' in w1 and 'changed the programs' not in w1, 'what: from the row (%s)' % w1)
        old = {'outcome': 'STILL-LOADING', 'room': 'lk00', 'detail': '', 'place': '0a:00', 'programs': '0x45/0x45/0x45', 'player': 'none', 'mission': 'none',
               'maps': 'map/us/lk00.bar(LOOSE x2)', 'target': 'lk00 (world 10 room 0) door 0 programs map 0x45 btl 0x45 evt 0x45'}
        w2 = what_text(old, pf)
        check('map not opened by this warp' in w2 and 'player none' in w2 and 'mission' not in w2 and 'at the end' not in w2
              and 'lk00: map 0x45 no, btl 0x45 no, evt 0x45 no' in w2, 'what: an old row without the parts (%s)' % w2)
        inr = dict(old, outcome='LOADING', room='lm04', place='0b:04', programs='0x67/0x67/0x67', maps='map/lm04.map(LOOSE x2)',
                   target='lm04 (world 11 room 4) door 0 programs map 0x64 btl 0x64 evt 0x64')
        check("the room's own program changed the programs (warp asked 0x64/0x64/0x64)" in what_text(inr) and 'map opened' in what_text(inr), 'what: programs changed inside the room')
        twelve = ' '.join('map/lm%02d.map(LOOSE x2)' % i for i in range(1, 13) if i != 9)
        check('map not among the 12 map names the warp keeps' in what_text(dict(inr, place='0b:09', maps=twelve + ' map/us/lm01.bar(LOOSE x1)')), 'what: a full map list')
        jn2 = dict(jn, room='lk13', detail='the room\'s own program jumped on to lk00, and no field came there', player='none', maps='map/lk13.map(LOOSE x2) map/lk00.map(LOOSE x2)',
                   target='lk13 (world 10 room 13) door 0 programs map 0x54 btl 0x54 evt 0x54')
        w4 = what_text(jn2, program_fact(td, jn2))
        check(w4.startswith('moved on to lk00; place lk00 programs 0x56/0x56/0x56, map opened') and 'lk00: map 0x56 no' in w4 and 'changed the programs' not in w4, 'what: moved on (%s)' % w4)
        check(what_text({'outcome': 'FIELD', 'detail': ''}) == '' and what_text({'outcome': 'JUMPED-ON', 'detail': 'jumped on to bb01 (05:01)'}) == 'jumped on to bb01'
              and what_text(out[1]) == 'map/mu13.map MISS x3', 'what: the short outcomes')
    sweep_sh_selftest(check)
    print('kh: room_sweep selftest: %d checks, %d failures' % (checks, fails))
    return 1 if fails else 0


def sweep_sh_selftest(check):
    """tools/room_sweep.sh's bookkeeping, without running the game (a call with every entry decided only marks, records and tables):
    a call refuses to start while a process still runs in a run directory without an exit file and then changes nothing; otherwise such
    a directory (an interrupted call) is marked INTERRUPTED and its game root / docs copies are removed; every call that starts appends
    its own parameters to params.txt.  ROOM_SWEEP_SH names the script (default: room_sweep.sh next to this file)."""
    import subprocess
    import tempfile
    sh = os.environ.get('ROOM_SWEEP_SH') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'room_sweep.sh')
    if not os.path.exists(sh):
        check(False, 'room_sweep.sh not found at %s (set ROOM_SWEEP_SH)' % sh)
        return
    with tempfile.TemporaryDirectory() as td:
        td = os.path.realpath(td)
        out = os.path.join(td, 'sweep')
        os.makedirs(os.path.join(out, 'runs'))
        lst = os.path.join(td, 'list.txt')
        open(lst, 'w').write('bb 0\n')
        open(os.path.join(out, 'entries.tsv'), 'w').write('1\tbb 0\n')
        open(os.path.join(out, 'params.txt'), 'w').write('call earlier: seconds=1800\n')
        done = os.path.join(out, 'runs', 'L0-001')
        os.makedirs(done)
        open(os.path.join(done, 'exit'), 'w').write('6\n')
        cut = os.path.join(out, 'runs', 'L1-002')
        os.makedirs(os.path.join(cut, 'root', 'x'))
        os.makedirs(os.path.join(cut, 'docs'))
        open(os.path.join(out, 'state.L0.jsonl'), 'w').write(json.dumps({'entry': 1, 'line': 1, 'k': 1, 'room': 'bb00', 'outcome': 'FIELD',
                                                                         'detail': 'the field is up', 'run': done}) + '\n')
        cmd = ['bash', sh, '-o', out, '-p', 'list', '-L', lst, '-j', '1']
        p = subprocess.Popen(['sleep', '60'], cwd=cut)          # a process of the interrupted run still there (this test's own, killed below)
        try:
            r = subprocess.run(cmd + ['-S', '400'], capture_output=True, text=True, timeout=300)
        finally:
            p.kill()
            p.wait()
        check(r.returncode == 2 and 'still runs in %s' % cut in r.stderr and not os.path.exists(os.path.join(cut, 'INTERRUPTED'))
              and os.path.isdir(os.path.join(cut, 'root')) and len(open(os.path.join(out, 'params.txt')).read().splitlines()) == 1,
              'room_sweep.sh refuses while a process runs in a run directory without an exit file, and changes nothing (exit %d: %s)' % (r.returncode, r.stderr.strip()[-200:]))
        r = subprocess.run(cmd + ['-S', '500'], capture_output=True, text=True, timeout=300)
        par = open(os.path.join(out, 'params.txt')).read().splitlines()
        check(r.returncode == 0 and os.path.exists(os.path.join(cut, 'INTERRUPTED')) and not os.path.exists(os.path.join(cut, 'root'))
              and not os.path.exists(os.path.join(cut, 'docs')) and not os.path.exists(os.path.join(done, 'INTERRUPTED')) and 'LOUD: %s has no exit file' % cut in r.stderr,
              'room_sweep.sh marks the interrupted run directory and only that one (exit %d: %s)' % (r.returncode, r.stderr.strip()[-200:]))
        check(len(par) == 2 and par[0] == 'call earlier: seconds=1800' and par[1].startswith('call ') and 'seconds=500' in par[1] and 'rooms=list %s' % lst in par[1],
              'every call appends its own parameters (%s)' % par)
        check(not os.path.exists(os.path.join(out, 'khnmc.bin')) and os.path.exists(os.path.join(out, 'table.md'))
              and '| 1 | bb00 | `bb 0` | FIELD |' in open(os.path.join(out, 'table.md')).read(), 'nothing to run: no binary copied, the table written')


def main(a):
    if a[:1] == ['--selftest']:
        return selftest()
    if len(a) >= 3 and a[0] == 'plan':
        for l in plan(a[1], a[2], a[3:]):
            print(l)
        return 0
    if len(a) == 3 and a[0] == 'parse':
        ex = None
        p = os.path.join(a[1], 'exit')
        if os.path.exists(p):
            try:
                ex = int(open(p).read().split()[0])
            except (ValueError, IndexError):
                ex = None
        lines = open(os.path.join(a[1], 'run.out'), errors='replace').readlines() if os.path.exists(os.path.join(a[1], 'run.out')) else []
        out, end = parse_run(lines, read_list(a[2]), ex)
        for r in out:
            print(json.dumps(r))
        print(json.dumps({'end': end}))
        return 0
    if len(a) == 5 and a[0] == 'record':
        print(record(a[1], a[2], a[3], a[4])); return 0
    if len(a) in (2, 3) and a[0] == 'table':
        table(a[1], a[2] if len(a) == 3 else None); return 0
    if len(a) >= 3 and a[0] == 'programs':
        for r in a[2:]:
            p = ard_programs(os.path.join(a[1], 'ard', 'us', r + '.ard'))
            print(r)
            for kind in ('map', 'btl', 'evt'):
                for pid, cmds in p.get(kind, []):
                    ms = missions(cmds)
                    print('  %s %#x%s' % (kind, pid, (' mission ' + ', '.join(ms)) if ms else ''))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
