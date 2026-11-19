#!/usr/bin/env python3
"""Generate the Xion field table and its evidence from the NMC story graph."""
import argparse
from functools import lru_cache
from collections import Counter, defaultdict
from pathlib import Path
import struct

import event_refs as E
from bdxscan import parse_bdx

KINDS = ('map', 'btl', 'evt')
LEGACY = {('tt', 2, 1, 0, 0), ('hb', 8, 0, 1, 0), ('tt', 7, 95, 95, 95)}
# Ordinary Heartless and Nobodies damageable with normal attacks. Use exact
# objentry IDs: arena, scripted, crowd and minigame variants have separate IDs.
ENEMIES = {*range(1, 21), 76, 77, *range(120, 126), *range(301, 307),
           73, 119, 308, 310, 317, 318, 367, 368, 1833, 1834, 1838, 1840, 1849, 1852, 2238, *range(1827, 1833), 1836, 1837, 1843, 1845, 1847, 1887, 1889, 2025}
FIELD = {'PLAYER', 'FRIEND_1', 'FRIEND_2', 'SAVE_POINT', 'F_EX260', 'F_HB020',
         'F_EH000', 'F_EH010', 'F_EH020', 'F_EH040'}
# Op 0x14 sets the actor AI no_leave query (f_3a27e1, syscall 1/171 at f_430820).
PASSIVE = {1, 4, 5, 6, 7, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x14, 0x18, 0x1a, 0x1b, 0x1c, 0x1e}
SPECIAL = {9:'alternate spawn script', 10:'map AI', 11:'barrier', 19:'special camera',
           22:'external BAR', 23:'status flag', 25:'progress writer',
           29:'recovery script', 31:'gacha'}


def entities(data):
    if not data:  # NMC also has zero-byte type-12 entries, e.g. bb00/z_70
        return [], set()
    if len(data) < 8 or struct.unpack_from('<I', data)[0] != 2:
        raise ValueError('unrecognized spawn structure')
    ids, reactions = [], set()
    pos = 8
    for _ in range(struct.unpack_from('<I', data, 4)[0]):
        if pos + 44 > len(data):
            raise ValueError('short spawn descriptor')
        ne, na, nw, nr, ns = struct.unpack_from('<4HI', data, pos+4)
        pos += 44
        if pos + 64*(ne+na) > len(data):
            raise ValueError('short spawn entity or activator')
        for i in range(ne):
            entity = pos+64*i
            ids.append(struct.unpack_from('<I', data, entity)[0])
            reaction, = struct.unpack_from('<H', data, entity+44)
            if reaction:
                reactions.add(reaction)
        pos += 64*(ne+na)
        for _ in range(nw):
            if pos+8 > len(data):
                raise ValueError('short walk path')
            pos += 8+12*struct.unpack_from('<H', data, pos+2)[0]
        pos += 16*nr+8*ns
    if pos != len(data):
        raise ValueError('spawn groups do not end at entry boundary')
    return ids, reactions


MISSION_OP = {4:'timer',5:'counter',6:'gauge',7:'combo counter',8:'score',9:'watch',
              10:'limit cost',11:'drive refill',12:'drive award'}
TRAPS = {(0,75):'scripted weapon', (0,76):'form weapon', (1,49):'enable reaction',
         (1,165):'override command', (1,237):'override top command',
         (1,242):'capture form', (1,308):'capture command', (1,325):'reaction callback',
         (1,345):'override command slot', (4,16):'boss death', (4,38):'score check',
         (6,48):'skateboard score'}


@lru_cache(None)
def mission(tree, name):
    path = tree.find('msn/us/'+name+'.bar')
    if not path:
        return 'mission '+name+' (no MSN file)'
    entries = E.bar(tree._read(path))
    notes = set()
    if not entries or entries[0][0] != 2 or len(entries[0][2]) < 32:
        return 'mission '+name+' (unrecognized header)'
    header = entries[0][2]
    mid, flags = struct.unpack_from('<HH', header, 2)
    objective, = struct.unpack_from('<H', header, 10)
    if flags & 1: notes.add('boss battle')
    if flags & 0x20: notes.add('no leave')
    for op,_ in E.commands(header[28:]):
        if op in MISSION_OP: notes.add(MISSION_OP[op])
    for typ,_,data in entries:
        if typ != 3: continue
        script = parse_bdx(data)
        if not script:
            notes.add('unresolved mission AI')
            continue
        for table,trap,_,kind in script['syscalls']:
            if kind != 'raw' and (table,trap) in TRAPS:
                notes.add(TRAPS[table,trap])
    return 'mission %s (id %#x objective %#x%s)' % (
        name,mid,objective,''.join('; '+v for v in sorted(notes)))


def object_reason(model, kind):
    # Requirements found in the placed model's own type-3 script.
    if model == 'F_MU070': return 'wind ride reaction '+model
    if model in ('F_CA030_DARK','F_CA030_LIGHT'): return 'prize requires attack 571/572 '+model
    if model == 'F_CA010': return 'command 431 motion capture '+model
    if model in ('F_BB050','F_BB060','F_BB090'): return 'Shadow Stalker encounter prop '+model
    if model in ('N_PO010_RTN','N_PO010_BTL','N_PO030_AIR_RTN'): return 'ally command override '+model
    if model == 'P_EX020': return 'scripted command groups '+model
    if model == 'WORLD_POINT': return 'world map interaction '+model
    role = {0:'alternate player', 2:'NPC interaction', 3:'boss', 5:'scripted weapon',
            10:'chest reaction', 11:'submenu', 12:'boss', 15:'interaction script',
            16:'minigame', 17:'world map object', 20:'shop', 22:'crowd spawner'}
    if kind in role:
        return role[kind]+' '+model
    if model.startswith(('F_EX030','F_EX031','F_EX040')) or model in ('F_EX050','F_EX060'):
        return 'chest reaction '+model
    if model.startswith(('F_TT010','F_TT070')):
        return 'skateboard '+model
    if model.startswith('W_'): return 'scripted weapon '+model
    if model.startswith('B_'): return 'boss '+model
    if 'CARPET' in model or 'LIGHTCYCLE' in model: return 'special movement '+model
    return 'unreviewed object '+model


def program(tree, types, a, kind, pid):
    cmds = a.scripts.get(kind, {}).get(pid)
    bad, names, groups = set(), set(), set()
    if cmds is None:
        return bad, names, {'absent'}
    for op, data in cmds:
        if op in (0, 2, 3):
            for off in range(4 if op == 3 else 0, len(data), 4):
                group = E.cstr(data[off:off+4])
                matches = [d for n,d in a.spawns if n == group]
                if not matches:
                    # f_3a2030 requests the first type-12 match. f_3a4e80 skips null.
                    groups.add(group+'(absent)')
                    continue
                groups.add(group)
                try:
                    ids, reactions = entities(matches[0])
                except ValueError:
                    bad.add('undecoded spawn '+group)
                    continue
                bad.update('reaction %#x'%r for r in reactions)
                for oid in ids:
                    model = tree.objentry.get(oid, ('unknown-%d'%oid,))[0]
                    names.add(model)
                    if model in FIELD or (oid in ENEMIES and types.get(oid) in (4,21)):
                        continue
                    if model == 'F_EX000' and a.name in ('hb08','tt02'):
                        continue  # K54 exercised these rooms' examine reticles
                    bad.add(object_reason(model, types.get(oid)))
        elif op == 0x15:
            bad.add(mission(tree,E.cstr(data[4:])))
        elif op == 0x0c:
            trigger, _, subs = E.settings(data)
            bad.add('trigger %d '%trigger + '/'.join('event '+s[2] if s[0]=='event' else s[0] for s in subs))
        elif op not in PASSIVE:
            bad.add(SPECIAL.get(op, 'unreviewed opcode %#x'%op))
    return bad, names, groups


def tuples(tree, graph):
    # Apply each reachable progress write atomically. Kept words inherit the
    # saved tuple, never the temporary place selected by a localset jump.
    writes = defaultdict(set)
    saved = defaultdict(set)
    out = defaultdict(set)
    for a in tree.areas.values():
        if a.num >= 0:
            saved[a.world,a.num].add((0,0,0))
    for flag in graph.flags:
        for op, words in tree.progress.get(flag >> 10, {}).get(flag & 1023, []):
            if op == 1:
                for i in range(0,len(words),4):
                    area, *update = words[i:i+4]
                    writes[E.WORLDS[flag >> 10], area].add(tuple(update))
    for room, states in saved.items():
        todo = list(states)
        while todo:
            state = todo.pop()
            for row in writes[room]:
                updated = tuple(a if b==65535 else b for a,b in zip(state,row))
                if updated not in states:
                    states.add(updated)
                    todo.append(updated)
        out[room].update(states)
    for w,n,k,p in graph.run:
        a = graph.area(w,n)
        if a is None:
            continue
        for op,data in a.scripts.get(k,{}).get(p,[]):
            if op == 12:
                for s in E.settings(data)[2]:
                    if s[:2] == ('jump',2):
                        wi = s[2] if 0 <= s[2] < len(E.WORLDS) else E.WORLDS.index(w)
                        area = tree.localset.get(wi,{}).get(s[5])
                        if area is not None:
                            out[E.WORLDS[wi],area].add((s[5],)*3)
    out['tt',1].add((0,0,1))
    for w,n,*ps in LEGACY:
        out[w,n].add(tuple(ps))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('data')
    ap.add_argument('report', type=Path)
    ap.add_argument('header', type=Path)
    args = ap.parse_args()
    tree = E.Tree(args.data)
    obj = tree._read('00objentry.bin')
    types = {oid:typ for oid,typ in struct.iter_unpack('<IB91x', obj[8:])}
    graph = E.Graph(tree)
    enemies = {tree.objentry[oid][0] for oid in ENEMIES if oid in tree.objentry}
    counts = defaultdict(Counter)
    table = ['/* Generated by tools/xion_fields.py. */']
    evidence = ['room\tmap\tbtl\tevt\tclass\treasons\tobjects\tspawn groups']
    cache = {}
    covered = set()
    for (w,n), states in sorted(tuples(tree,graph).items()):
        a = graph.area(w,n)
        for ps in sorted(states):
            bad, names, groups = set(), set(), set()
            for k,p in zip(KINDS,ps):
                key = (w,n,k,p)
                covered.add(key)
                if a is None:
                    bad.add('no ARD')
                    continue
                if key not in cache:
                    cache[key] = program(tree,types,a,k,p)
                b,m,s = cache[key]
                bad.update(b)
                names.update(m)
                groups.update(k+'='+v for v in s)
            if a and not tree.find('map/'+a.name+'.map'): bad.add('missing room map')
            if w=='al' and n==5: bad.add('Above Agrabah carpet traversal')
            if w=='wm': bad.add('world map/gummi')
            if (w,n,ps) == ('mu',9,(0,10,0)):
                bad.add('P_EX220 ptya table 28 lacks key 1 required by f_402d20')
            if 'PLAYER' not in names: bad.add('no field player spawn')
            if w=='hb' and 21 <= n <= 25: bad.add('Cavern of Remembrance traversal')
            legacy = (w,n,*ps) in LEGACY
            if legacy:
                cls, why = 'K54', 'K54 tested tuple'
            elif bad:
                cls, why = 'Roxas', ', '.join(sorted(bad))
            elif names & enemies:
                cls, why = 'battle', 'ordinary attack targets: '+', '.join(sorted(names & enemies))
            else:
                cls, why = 'field', 'field movement and pickups'
            counts[w][cls] += 1
            room = '%s%02d'%(w,n)
            if cls != 'Roxas':
                table.append('    {%d,%d,%d,%d,%d}, /* %s: %s */' %
                             (E.WORLDS.index(w),n,*ps,room,why if legacy else ', '.join(sorted(names))))
            evidence.append('\t'.join([room,*map(str,ps),cls,why,','.join(sorted(names)),','.join(sorted(groups))]))
    missing = graph.run.keys() - covered
    if missing:
        raise ValueError('uncovered graph selections: %r'%sorted(missing))
    args.report.write_text('\n'.join(evidence)+'\n')
    args.header.write_text('\n'.join(table)+'\n')
    for w,c in sorted(counts.items()):
        print('| %s | %d | %d | %d | %d |'%(w,c['field'],c['battle'],c['K54'],c['Roxas']))


if __name__ == '__main__':
    main()
