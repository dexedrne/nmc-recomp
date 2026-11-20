"""Rebuild the Land of Dragons R02/R04 files over a read-only campaign install."""
import argparse
import json
import runpy
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F2-zexion.json'


def build(pin, campaign, output, rmc):
    api = runpy.run_path(str(rmc / 'tools/rmc-build.py'))
    require = api['require']
    require(not output.is_symlink() and not output.exists(), 'choose a fresh output directory')
    require(all(source != output and source not in output.parents for source in (pin, campaign, rmc)),
            'output must be outside input directories')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / 'installed'
        for edit in json.loads(RECIPE.read_text()):
            path = edit['path']
            dest = stage / path
            if edit['op'] == 'copy':
                require(not (pin / path).exists(), f'unexpected pinned destination: {path}')
                data = api['checked'](pin, edit['source'], edit['sha256'])
            else:
                require(edit['op'] == 'scene-props', f'unknown operation for {path}')
                data = api['archive'](api['checked'](pin, path, edit['sha256']), edit)
                require(api['sha'](data) == edit['result_sha256'], f'unexpected patched scene: {path}')
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        directories = [stage, *(p for p in stage.rglob('*') if p.is_dir())]
        for directory in directories:
            fallback = campaign / directory.relative_to(stage)
            if fallback.is_dir():
                for child in fallback.iterdir():
                    dest = directory / child.name
                    if not dest.exists() and not dest.is_symlink():
                        dest.symlink_to(child)
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('pin', 'campaign', 'output', 'rmc'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    build(args.pin.resolve(), args.campaign.resolve(), args.output.parent.resolve() / args.output.name, args.rmc.resolve())
