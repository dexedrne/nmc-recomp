#!/usr/bin/env python3
"""Bind final Riku and the NMC controller in the two PC final battles."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F22-final.json'


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    require = api['require']
    spec = json.loads(RECIPE.read_text())
    path = Path(spec['path'])
    require(not output.exists() and not output.is_symlink(), 'choose a fresh output directory')
    dest = output.resolve()
    require(base != dest and base not in dest.parents, 'output inside base')
    data = bytearray(api['checked'](base, path, spec['sha256']))
    entries = api['bar'](data)
    for member in spec['members']:
        entry = api['selected'](entries, *member['selector'])
        rows = api['tokens'](12, entry.data(data))
        tag, group = rows[1]
        require(tag == 'group' and struct.unpack_from('<h', group, 4) == (4,), 'unexpected party group')
        tag, partner = rows[3]
        require(tag == 'entity' and struct.unpack_from('<I', partner)[0] == spec['party_slot']
                and struct.unpack_from('<h', partner, 30)[0] == member['partner_serial'], 'unexpected party slot')
        rows[1] = 'group', group[:4] + struct.pack('<h', 5) + group[6:]
        rows[3] = 'entity', struct.pack('<I', spec['riku']) + partner[4:]
        controller = bytearray(partner)
        struct.pack_into('<I', controller, 0, spec['controller'])
        struct.pack_into('<h', controller, 30, member['controller_serial'])
        rows.insert(6, ('entity', controller))
        payload = api['encode'](12, rows)
        data.extend(bytes(-len(data) % 16))
        api['directory_record'](data, entry.ordinal, len(data), len(payload))
        data.extend(payload)
    require(api['sha'](data) == spec['result_sha256'], 'unexpected repaired room digest')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'overlay'
        (stage / path.parent).mkdir(parents=True)
        (stage / path).write_bytes(data)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', required=True, type=Path)
    args = parser.parse_args()
    build(args.base.resolve(), args.output.absolute(), args.rmc.resolve())
