#!/usr/bin/env python3
"""Build the Armored II party placement in a fresh F20-style data tree."""
import argparse
import json
from pathlib import Path
import runpy
import struct
import tempfile

from build_f8 import append

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F21-armored.json'


def repair(data, spec, api):
    from records import encode, tokens
    require = api['require']
    entries = api['bar'](data)
    source = api['selected'](entries, *spec['spawn'])
    battle = api['selected'](entries, *spec['battle'])
    name = spec['new_spawn'].encode('ascii')
    require(len(name) == 4 and all(e.name != name for e in entries), 'occupied spawn name')
    rows = tokens(12, source.data(data))
    group, = (i for i, (k, b) in enumerate(rows) if k == 'group' and
              struct.unpack_from('<h', b, 2)[0] == spec['group'])
    header = bytearray(rows[group][1])
    count, = struct.unpack_from('<h', header, 4)
    actors = rows[group + 1:group + 1 + count]
    require(all(k == 'entity' for k, _ in actors), 'invalid group entities')
    donor, = (b for _, b in actors if struct.unpack_from('<I', b)[0] == spec['beside'])
    actor = bytearray(donor)
    struct.pack_into('<I', actor, 0, spec['partner'])
    struct.pack_into('<h', actor, 30, spec['serial'])
    require(all(struct.unpack_from('<h', b, 30)[0] != spec['serial']
                for k, b in rows if k == 'entity'), 'occupied actor serial')
    struct.pack_into('<h', header, 4, count + 1)
    rows[group] = ('group', header)
    rows.insert(group + 1 + count, ('entity', actor))
    spawn = encode(12, rows)

    rows = tokens(13, battle.data(data))
    program = None
    matches = []
    for i, (kind, value) in enumerate(rows):
        if kind == 'program':
            program, = struct.unpack('<h', value)
        elif program == spec['program'] and (kind, value) == ('Spawn', source.name):
            matches.append(i)
    index, = matches
    rows[index] = ('Spawn', name)
    script = encode(13, rows)
    # Insert one aligned BAR record. Every original payload retains its alignment.
    end = 16 + 16 * len(entries)
    result = bytearray(data[:end] + bytes(16) + data[end:])
    struct.pack_into('<I', result, 4, len(entries) + 1)
    for entry in entries:
        offset = append(result, script) if entry == battle else entry.offset + 16
        api['directory_record'](result, entry.ordinal, offset, entry.size)
    struct.pack_into('<HH4sII', result, end, 12, 0, name, append(result, spawn), len(spawn))
    return result


def build(base, out, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    import sys
    sys.path.insert(0, str(rmc / 'upstream/tools/rooms'))
    api['require'](not out.is_symlink() and not out.exists() and
                   not out.resolve().is_relative_to(base), 'choose a fresh output outside the base')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as directory:
        stage = Path(directory) / 'data'
        for spec in json.loads(RECIPE.read_text()):
            data = api['checked'](base, spec['path'], spec['sha256'])
            result = repair(data, spec, api)
            api['require'](api['sha'](result) == spec['result_sha256'], 'unexpected patched room')
            dst = stage / spec['path']
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(result)
        for folder in [stage, *[p for p in stage.rglob('*') if p.is_dir()]]:
            for item in (base / folder.relative_to(stage)).iterdir():
                if not (folder / item.name).exists():
                    (folder / item.name).symlink_to(item)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', required=True, type=Path)
    args = parser.parse_args()
    build(args.base.resolve(), args.output, args.rmc.resolve())
