"""Build the Crooked Ascension first-ascent overlay against the installed PC data."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F14-ascent.json'


def repair(data, spec, api):
    require = api['require']
    entry = api['selected'](api['bar'](data), *spec['member'])
    rows = api['tokens'](entry.kind, entry.data(data))
    program = None
    kept = []
    for tag, body in rows:
        if tag == 'program':
            program = int.from_bytes(body, 'little', signed=True)
        if (program, tag, body) != (spec['program'], 'Spawn', spec['omit_spawn'].encode('ascii')):
            kept.append((tag, body))
    require(len(kept) + 1 == len(rows), 'expected exactly one first-ascent spawn command')
    payload = api['encode'](entry.kind, kept)
    require(api['tokens'](entry.kind, payload) == kept, 'map programs failed round trip')
    result = bytearray(data)
    result[entry.offset:entry.offset + entry.size] = payload.ljust(entry.size, b'\0')
    api['directory_record'](result, entry.ordinal, entry.offset, len(payload))
    return result


def reward(data, spec, api):
    entry = api['selected'](api['bar'](data), *spec['member'])
    table = bytearray(entry.data(data))
    version, count = struct.unpack_from('<HH', table)
    codec = struct.Struct('<HH4Bhh')
    rows = list(codec.iter_unpack(table[4:]))
    key = spec['event_row'][0]
    api['require'](version == 3 and count == len(rows) and
                   [r for r in rows if r[0] == key] == [tuple(spec['chest_row'])],
                   'unexpected treasure definition')
    table.extend(codec.pack(*spec['event_row']))
    struct.pack_into('<H', table, 2, count + 1)
    end = entry.offset + entry.size
    result = bytearray(data[:entry.offset] + table + data[end:])
    api['directory_record'](result, entry.ordinal, entry.offset, len(table))
    for other in api['bar'](data):
        if other.offset >= end:
            api['directory_record'](result, other.ordinal, other.offset + codec.size, other.size)
    return result


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    require = api['require']
    spec = json.loads(RECIPE.read_text())
    require(not output.exists() and not output.is_symlink() and
            not output.resolve().is_relative_to(base.resolve()), 'choose a fresh output outside the base')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / 'data'
        for entry, edit in zip(spec, (repair, reward), strict=True):
            data = (base / entry['path']).read_bytes()
            digest = api['sha'](data)
            require(digest in entry['pins'], f"unexpected input digest for {entry['path']}")
            result = edit(data, entry, api)
            require(api['sha'](result) == entry['pins'][digest], 'unexpected repaired archive digest')
            dest = stage / entry['path']
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(result)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', type=Path, required=True)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
