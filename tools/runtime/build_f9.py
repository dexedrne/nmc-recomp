"""Build the Traverse Town talk-table override from the installed PC data."""
import argparse
import json
import runpy
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F9-traverse.json'


def build(base, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    require = api['require']
    spec = json.loads(RECIPE.read_text())
    require(not output.is_symlink() and not output.exists(), 'choose a fresh overlay directory')
    require(not output.resolve().is_relative_to(base.resolve()), 'output inside read-only base')
    data = bytearray(api['checked'](base, spec['path'], spec['sha256']))
    entry = api['selected'](api['bar'](data), spec['ordinal'], spec['kind'], spec['name'], spec['link'])
    require(entry.size == spec['old_size'] and entry.offset + spec['size'] == len(data),
            'unexpected talk-table extent')
    api['directory_record'](data, entry.ordinal, entry.offset, spec['size'])
    require(api['sha'](data) == spec['result_sha256'], 'unexpected repaired talk-table digest')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'data'
        stage.mkdir()
        (stage / spec['path']).write_bytes(data)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', type=Path, required=True)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
