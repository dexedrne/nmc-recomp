#!/usr/bin/env python3
"""Build a private F20 overlay from the pinned PC data and the RMC toolchain."""
import argparse
import json
import runpy
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F20-dragon.json'


def build(base, out, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    checked, bar, selected = (api[key] for key in ('checked', 'bar', 'selected'))
    require = api['require']
    require(not out.is_symlink(), 'choose a fresh output directory')
    out = out.resolve()
    require(out != base and base not in out.parents, 'output inside read-only base')
    require(not out.exists(), 'choose a fresh output directory')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        stage = Path(tmp) / 'data'
        for spec in json.loads(RECIPE.read_text()):
            rel = Path(spec['path'])
            data = checked(base, rel, spec['sha256'])
            entries = bar(data)
            entry = selected(entries, spec['ordinal'], spec['kind'], spec['name'], spec['link'])
            if spec['op'] == 'copy-member':
                donor = checked(base, spec['source'], spec['source_sha256'])
                src = selected(bar(donor), spec['source_ordinal'], entry.kind,
                               spec['source_name'], spec['source_link'])
                payload = src.data(donor)
                require(api['sha'](payload) == spec['member_sha256'], 'wrong donor payload')
                require(api['sha'](entry.data(data)) == spec['old_member_sha256'], 'wrong original collision')
                # Keep every other physical offset, including aligned graphics resources.
                result = bytearray(data)
                result.extend(bytes(-len(result) % 16))
                api['directory_record'](result, entry.ordinal, len(result), len(payload))
                result.extend(payload)
            else:
                result = api['archive'](data, spec)
            require(api['sha'](result) == spec['result_sha256'], f'wrong patched digest for {rel}')
            for old, new in zip(entries, bar(result), strict=True):
                if old != entry:
                    require(old.data(data) == new.data(result), f'unexpected change to {rel} member {old.ordinal}')
            dest = stage / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(result)
        # Populate only the branches leading to the two overrides. Fallbacks are read-only inputs.
        for directory in [stage, *[p for p in stage.rglob('*') if p.is_dir()]]:
            for child in (base / directory.relative_to(stage)).iterdir():
                if not (directory / child.name).exists():
                    (directory / child.name).symlink_to(child)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', type=Path, required=True, help='Roxas May Cry checkout')
    args = parser.parse_args()
    build(args.base.resolve(), args.output.absolute(), args.rmc.resolve())
