#!/usr/bin/env python3
"""Build the sparse US Betwixt overlay from pinned PC data."""
import argparse
import json
import runpy
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F12-betwixt.json'


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    from records import arguments, encode, tokens, values

    spec = json.loads(RECIPE.read_text())
    require = api['require']
    require(not output.exists() and not output.is_symlink(), 'choose a fresh output directory')
    require(not output.resolve().is_relative_to(base.resolve()), 'output inside base')
    path = spec['path']
    data = api['checked'](base, path, spec['sha256'])
    entry = api['selected'](api['bar'](data), spec['ordinal'], spec['kind'], spec['name'], spec['link'])
    rows = tokens(entry.kind, entry.data(data))
    edited = []
    program = None
    for i, (tag, payload) in enumerate(rows):
        if tag == 'program':
            program = int.from_bytes(payload, 'little', signed=True)
        if tag != 'AreaSettings':
            continue
        for op in ('remove', 'award'):
            change = spec[op]
            if change['program'] != program:
                continue
            args = values(tag, payload)
            require(['flag', change['flag']] in args, 'unexpected story flag')
            reward = ['list', change['treasure']]
            if op == 'remove':
                require(args.count(reward) == 1, 'missing entry reward')
                args.remove(reward)
            else:
                require(all(row[0] != 'list' for row in args[2:]), 'result already has rewards')
                require(args[-1] == ['set7', 0], 'unexpected result end')
                args.insert(-1, reward)
            rows[i] = tag, arguments(tag, args)
            edited.append(op)
    gate = spec['enable_draw']
    start, = (i for i, row in enumerate(rows) if row == ('program', api['pack']('h', gate['program'])))
    end = next((i for i in range(start + 1, len(rows)) if rows[i][0] == 'program'), len(rows))
    require(any(tag == 'AreaSettings' and ['flag', gate['exit_flag']] in values(tag, payload)
                for tag, payload in rows[start+1:end]), 'wrong post-result program')
    rows.insert(start + 1, ('Progress', arguments('Progress', [gate['progress_flag']])))
    require(sorted(edited) == ['award', 'remove'], 'expected exactly two event edits')
    replacement = encode(entry.kind, rows)
    require(len(replacement) == entry.size + 8 and tokens(entry.kind, replacement) == rows,
            'event program size or encoding changed unexpectedly')
    # Append the enlarged program so linked members and aligned resources keep their offsets.
    patched = bytearray(data)
    patched.extend(bytes(-len(patched) % 16))
    api['directory_record'](patched, entry.ordinal, len(patched), len(replacement))
    patched.extend(replacement)
    require(api['sha'](patched) == spec['result_sha256'], 'unexpected result digest')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'data'
        dest = stage / path
        dest.parent.mkdir(parents=True)
        dest.write_bytes(patched)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', type=Path, required=True)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
