"""Build the Memory's Skyscraper post-battle RMC overlay."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F13-neoshadow.json'


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    from records import SCHEMA, tokens

    require = api['require']
    spec = json.loads(RECIPE.read_text())
    require(not output.exists() and not output.is_symlink(), 'choose a fresh output directory')
    require(not output.resolve().is_relative_to(base.resolve()), 'output inside read-only base')
    data = bytearray(api['checked'](base, spec['path'], spec['sha256']))
    entry = api['selected'](api['bar'](data), spec['ordinal'], spec['kind'], spec['name'], spec['link'])
    rows = tokens(entry.kind, entry.data(data))
    count = spec['remove_empty_returns']
    require([tag for tag, _ in rows] == ['spawn', 'group', 'entity'] + ['return'] * count,
            'unexpected rik1 placement layout')
    group = struct.unpack('<' + SCHEMA['group'][0], rows[1][1])
    actor = struct.unpack('<' + SCHEMA['entity'][0], rows[2][1])
    require(group[0] == 1 and group[2] == spec['group'] and group[6] == count and
            actor[0] == spec['actor'] and actor[9] == spec['serial'], 'wrong interaction placement')
    require(all(not any(record) for _, record in rows[3:]), 'nonempty return parameter')

    # Shorten the member in place so every other extent and all relative offsets survive.
    struct.pack_into('<H', data, entry.offset + len(rows[0][1]) + 10, 0)
    size = entry.size - sum(len(record) for _, record in rows[3:])
    api['directory_record'](data, entry.ordinal, entry.offset, size)
    require(api['sha'](data) == spec['result_sha256'], 'unexpected repaired archive digest')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'data'
        dest = stage / spec['path']
        dest.parent.mkdir(parents=True)
        dest.write_bytes(data)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', type=Path, required=True)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
