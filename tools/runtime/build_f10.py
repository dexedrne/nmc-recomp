#!/usr/bin/env python3
"""Build the late book menus and R27 Day 96 placement as a sparse PC overlay."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F10-day96.json'


def restore_menu(data, spec, api):
    from formats import programs
    from records import cmd, commands, values

    entries = api['bar'](data)
    target = api['selected'](entries, *spec['map_member'])
    event = api['selected'](entries, *spec['event_member'])
    donor = dict(programs(event.data(data)))[spec['donor_program']]
    controls = [(tag, body) for tag, body in commands(donor) if tag == 'AreaSettings'
                or (tag == 'Spawn' and values(tag, body) == ['m_53'])]
    api['require']([tag for tag, _ in controls] == ['AreaSettings'] * 3 + ['Spawn'],
                   'expected three mission triggers and the three-choice terminal')
    extra = b''.join(cmd(tag, body) for tag, body in controls)
    payload = bytearray()
    found = []
    for key, body in programs(target.data(data)):
        if key in spec['late_maps']:
            found.append(key)
            body += extra
        payload.extend(struct.pack('<hH', key, len(body) + 4) + body)
    api['require'](sorted(found) == spec['late_maps'], 'late maps missing or duplicated')
    payload.extend(struct.pack('<hH', -1, 0))
    result = bytearray(data)
    result.extend(bytes(-len(result) % 16))
    api['directory_record'](result, target.ordinal, len(result), len(payload))
    return result + payload


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    require = api['require']
    require(not output.exists() and not output.is_symlink(), 'choose a fresh output directory')
    require(base != output.resolve() and base not in output.resolve().parents, 'output inside base')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'overlay'
        for spec in json.loads(RECIPE.read_text()):
            path = Path(spec['path'])
            data = api['checked'](base, path, spec['sha256'])
            result = (restore_menu(data, spec, api) if spec['op'] == 'restore-menu'
                      else api['archive'](data, spec))
            require(api['sha'](result) == spec['result_sha256'], f'unexpected digest for {path}')
            dest = stage / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(result)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', required=True, type=Path)
    args = parser.parse_args()
    build(args.base.resolve(), args.output, args.rmc.resolve())
