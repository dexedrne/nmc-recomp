#!/usr/bin/env python3
"""Build the sparse F8 Neverland overlay using the pinned PC/US data."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F8-neverland.json'


def append(data, payload):
    data.extend(bytes(-len(data) % 16))
    offset = len(data)
    data.extend(payload)
    return offset


def patched(data, spec, api):
    entries = api['bar'](data)
    result = bytearray(data)
    require = api['require']

    def member(key):
        return api['selected'](entries, *spec[key])

    def replace(entry, payload):
        api['directory_record'](result, entry.ordinal, append(result, payload), len(payload))

    if 'progress_member' in spec:
        entry = member('progress_member')
        payload = entry.data(data)
        flags = api['read_flags'](payload)
        edits = {}
        for change in spec['changes']:
            key, index = change['flag'], change['instruction']
            old = flags[key][index]
            require(old == ('ChangeArdProgram', *change['before']), 'unexpected room program selector')
            edits[key] = [[index, 1, [['ChangeArdProgram', *change['after']]]]]
        # Preserve the size and location of this boot-loaded BAR member.
        for duplicate, original in spec['empty_aliases']:
            require(flags[duplicate] == flags[original] == [], 'expected empty progress programs')
            edits[duplicate] = original
        rewritten = api['progress'](payload, {'flag_count': len(flags), 'flags': edits})
        require(len(rewritten) == len(payload), 'progress member size changed')
        result[entry.offset:entry.offset + entry.size] = rewritten
        return result

    group = bytearray(member('source').data(data))
    require(len(group) == 180 and struct.unpack_from('<II', group) == (2, 1), 'expected one exit group')
    require(struct.unpack_from('<hh', group, 12) == (0, 2), 'expected two activators')
    struct.pack_into('<h', group, 10, spec['group'])
    for i in range(2):
        at = 52 + i * 64
        require(struct.unpack_from('<hh', group, at) == (0, i), 'unexpected activator shape or option')
        center = struct.unpack_from('<3f', group, at + 4)
        struct.pack_into('<3f', group, at + 4, *(v + d for v, d in zip(center, spec['shift_xyz'], strict=True)))

    if 'append_to_spawn' in spec:
        target = member('append_to_spawn')
        payload = bytearray(target.data(data))
        require(struct.unpack_from('<II', payload) == (2, 1), 'unexpected arrival spawn')
        struct.pack_into('<I', payload, 4, 2)
        replace(target, payload + group[8:])
    else:
        name = spec['new_spawn'].encode('ascii')
        require(len(name) == 4 and all(e.name != name for e in entries), 'spawn name in use')
        event = member('event_member')
        payload = event.data(data)
        programs = api['programs'](payload)
        require(all(i != spec['event_program'] for i, _ in programs), 'event program in use')
        replace(event, payload[:-4] + struct.pack('<hHHH4s', spec['event_program'], 12, 0, 1, name) + payload[-4:])
        # The additional directory entry moves each original payload by one aligned record.
        directory_end = 16 + len(entries) * 16
        result[directory_end:directory_end] = bytes(16)
        struct.pack_into('<I', result, 4, len(entries) + 1)
        for e in entries:
            at = 24 + e.ordinal * 16
            offset, = struct.unpack_from('<I', result, at)
            struct.pack_into('<I', result, at, offset + 16)
        struct.pack_into('<HH4sII', result, directory_end, 12, 0, name, append(result, group), len(group))
    return result


def build(base, out, rmc):
    api = runpy.run_path(str(rmc/'tools/rmc-build.py'))
    from formats import programs
    api['programs'] = programs
    scripts = runpy.run_path(str(rmc/'upstream/tools/tables/scripts.py'))
    for name in ('read_flags', 'progress'):
        api[name] = scripts[name]
    api['require'](not out.is_symlink() and not out.exists() and
                   not out.resolve().is_relative_to(base.resolve()), 'choose a fresh output outside the base')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        stage = Path(tmp)/'overlay'
        for spec in json.loads(RECIPE.read_text()):
            data = api['checked'](base, spec['path'], spec['sha256'])
            result = patched(data, spec, api)
            api['require'](api['sha'](result) == spec['result_sha256'], 'wrong result for ' + spec['path'])
            dest = stage/spec['path']
            dest.parent.mkdir(exist_ok=True, parents=True)
            dest.write_bytes(result)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', required=True, type=Path)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
