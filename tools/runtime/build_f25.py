"""Restore the optional US silhouette books and their native room triggers."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import sys
import tempfile

from build_f8 import append

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F25-silhouettes.json'


def repair(data, spec, api):
    from records import encode, tokens
    entries = api['bar'](data)
    result = bytearray(data)

    def member(kind, name):
        entry, = (e for e in entries if e.kind == kind and e.name.rstrip(b'\0').decode() == name)
        return entry

    book = member(12, spec['placement'])
    group = bytearray(book.data(data))
    if struct.unpack_from('<II', group) != (2, 1) or len(group) != 116:
        raise ValueError('unexpected book placement layout')
    if struct.unpack_from('<I', group, 52)[0] != spec['old_actor']:
        raise ValueError('unexpected book actor')
    # The second unit argument marks only these added actors for the postgame check.
    struct.pack_into('<I', group, 52, spec['book_actor'])
    struct.pack_into('<I', group, 52 + 36, spec['clear_flag'])
    spawn_name = b'f25_'
    if any(e.name == spawn_name for e in entries):
        raise ValueError('book placement name already exists')
    end = 16 + 16 * len(entries)
    result[end:end] = bytes(16)
    struct.pack_into('<I', result, 4, len(entries) + 1)
    for e in entries:
        api['directory_record'](result, e.ordinal, e.offset + 16, e.size)
    struct.pack_into('<HH4sII', result, end, 12, 0, spawn_name, append(result, group), len(group))
    event = member(13, 'evt')
    rows = tokens(13, event.data(data))
    trigger, = {raw for tag, raw in rows if tag == 'AreaSettings' and struct.unpack_from('<h', raw)[0] == 161}
    edited = []
    changed = set()
    for tag, raw in rows:
        edited.append((tag, raw))
        if tag != 'program':
            continue
        pid, = struct.unpack('<h', raw)
        if pid in spec['events']:
            changed.add(pid)
            edited.append(('Spawn', spawn_name))
            if pid in spec['add_trigger']:
                edited.append(('AreaSettings', trigger))
    if changed != set(spec['events']):
        raise ValueError('missing entrance event program')
    payload = encode(13, edited)
    api['directory_record'](result, event.ordinal, append(result, payload), len(payload))
    return result


def replace_member(data, entry, payload, api):
    result = bytearray(data)
    api['directory_record'](result, entry.ordinal, append(result, payload), len(payload))
    return result


def gate_actor(data, flag, api):
    from formats import bdx_triggers
    entry, = (e for e in api['bar'](data) if e.kind == 3)
    script = bytearray(entry.data(data))
    ordinal, pc = next((i, pc) for i, (key, pc) in enumerate(bdx_triggers(script)) if key == 1)
    if struct.unpack_from('<3H', script, 16 + 2*pc) != (0x60, 0, 0x108):
        raise ValueError('unexpected book field trigger')
    start = (len(script) - 16) // 2
    # Unit arguments and the W0 object binding are live when the field trigger starts.
    code = [0x60, 0, 0, 2, 0, 0x4a, 39, 0, flag, 0, 0x46, 0x87, 0]
    ordinary = len(code) - 1
    code += [0, 4283, 0, 0xa, 15, 0x47, 7,
             0, flag, 0, 0xa, 15, 0x47, 5,
             0x60, 0, 0x4a, 17, 0x89]
    code[ordinary] = len(code) - ordinary - 1
    code.extend([7, (pc - start - len(code) - 2) & 0xffff])
    struct.pack_into('<i', script, 32 + 8*ordinal, start)
    script.extend(struct.pack(f'<{len(code)}H', *code))
    return replace_member(data, entry, script, api)


def mark_wins(data, fights, api):
    from records import tokens, encode, values
    event, = (e for e in api['bar'](data) if e.kind == 13 and e.name == b'evt\0')
    rows = tokens(13, event.data(data))
    changed = set()
    program = None
    for i, (kind, raw) in enumerate(rows):
        if kind == 'program':
            program, = struct.unpack('<h', raw)
        elif program in fights and kind == 'AreaSettings' and raw[:4] == struct.pack('<hh', 0, -1):
            # Use the result scene's empty flag slot. A second flag on the battle trigger is overwritten.
            clear = fights[program]
            if any(op[0] == 'flag' for op in values(kind, raw)[2:]):
                raise ValueError('result scene already has a progress flag')
            rows[i] = (kind, raw[:4] + struct.pack('<HH', 2, clear) + raw[4:])
            changed.add(program)
    if changed != set(fights):
        raise ValueError('missing optional victory trigger')
    return replace_member(data, event, encode(13, rows), api)


def reserve_flags(data, first, count, api):
    from scripts import progress, read_flags
    entry, = (e for e in api['bar'](data) if e.name.rstrip(b'\0') == b'hb')
    payload = entry.data(data)
    old = read_flags(payload)
    if len(old) != first or first + count > 256:
        raise ValueError('unexpected Hollow Bastion progress allocation')
    empty = [i for i, script in enumerate(old) if script == []]
    # Share identical two-byte end instructions to keep the boot-loaded BAR in place.
    edits = {i: empty[0] for i in empty[1:1 + count]}
    edits.update({first + i: empty[0] for i in range(count)})
    patched = progress(payload, {'flag_count': first + count, 'flags': edits})

    def expanded(rows):
        return [rows[v] if isinstance(v, int) else v for v in rows]

    if len(patched) != len(payload) or expanded(read_flags(patched)) != expanded(old) + [[]]*count:
        raise ValueError('progress allocation changed existing scripts or length')
    result = bytearray(data)
    result[entry.offset:entry.offset + entry.size] = patched
    return result


def build(base, pack, out, rmc):
    api = runpy.run_path(str(rmc/'tools/rmc-build.py'))
    sys.path.insert(0, str(rmc/'upstream/tools/rooms'))
    if out.exists() or out.is_symlink() or any(out.resolve().is_relative_to(p) for p in (base, pack)):
        raise ValueError('choose a fresh output outside both inputs')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        stage = Path(tmp)/'pack'
        recipe = json.loads(RECIPE.read_text())
        fights = {}

        def load(rel):
            source = pack if (pack/rel).exists() else base
            return api['checked'](source, rel, recipe['inputs'][rel])

        def write(rel, data):
            if hashlib.sha256(data).hexdigest() != recipe['outputs'][rel]:
                raise ValueError(f'unexpected patched digest for {rel}')
            dst = stage/rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)

        for spec in recipe['books']:
            rel = spec['path']
            write(rel, repair(load(rel), spec, api))
            actor = spec['actor_path']
            write(actor, gate_actor(load(actor), spec['clear_flag'], api))
            fights.setdefault(spec['arena'], {})[spec['result']] = spec['clear_flag']
        for rel, battles in fights.items():
            write(rel, mark_wins(load(rel), battles, api))
        sys.path.insert(0, str(rmc/'upstream/tools/tables'))
        first = recipe['books'][0]['clear_flag'] - 4096
        flags = [b['clear_flag'] for b in recipe['books']]
        if flags != list(range(flags[0], flags[0] + len(flags))):
            raise ValueError('optional flags must be contiguous')
        write('00progress.bin', reserve_flags(load('00progress.bin'), first, len(recipe['books']), api))
        for directory in (Path('.'), Path('ard'), Path('ard/us'), Path('obj')):
            for p in (pack/directory).iterdir():
                target = stage/directory/p.name
                if not target.exists():
                    target.symlink_to(p)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('base', 'pack', 'output', 'rmc'):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    build(args.base.resolve(), args.pack.resolve(), args.output.absolute(), args.rmc.resolve())
