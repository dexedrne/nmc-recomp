"""Replace a superseded F8 progress table in a private copy of the shared pack."""
import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F15-xigbar.json'


def build(pack, f8, output):
    spec = json.loads(RECIPE.read_text())
    name = spec['member']
    replacement = (f8 / name).read_bytes()
    if hashlib.sha256(replacement).hexdigest() != spec['f8_release_sha256']:
        raise ValueError('F15: expected the final F8 distribution progress table')
    if hashlib.sha256((pack / name).read_bytes()).hexdigest() not in (
            spec['superseded_sha256'], spec['f8_release_sha256']):
        raise ValueError('F15: unknown progress table in the input pack')
    if output.exists() or output.is_symlink() or any(
            output.resolve().is_relative_to(p.resolve()) for p in (pack, f8)):
        raise ValueError('F15: choose a fresh output outside both inputs')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / 'pack'
        shutil.copytree(pack, stage)
        (stage / name).write_bytes(replacement)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('pack', 'f8', 'output'):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    build(args.pack, args.f8, args.output)
