#!/usr/bin/env python3
"""Build the private Necromancer motion override on top of an existing RMC pack."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F24-necromancer.json'


def checked(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f'unexpected SHA-256 for {path}')
    return data


def build(base, pack, stock, out):
    spec = json.loads(RECIPE.read_text())
    out = out.parent.resolve() / out.name
    if os.path.lexists(out) or any(out.is_relative_to(root) for root in (base, pack, stock)):
        raise ValueError('choose a fresh output directory outside the inputs')

    def active(rel):
        return pack / rel if (pack / rel).exists() else base / rel

    table = spec['table']
    shared = Path('obj') / spec['shared_motion']
    private = Path('obj') / spec['private_motion']
    checked(active(shared), spec['shared_sha256'])
    if active(private).exists():
        raise ValueError(f'private motion name already in use: {private}')
    original = active(table).read_bytes()
    motion = checked(stock / shared, spec['stock_sha256'])
    version, count = struct.unpack_from('<II', original)
    if version != 3 or len(original) != 8 + 96 * count:
        raise ValueError('unexpected object table layout')
    matches = [i for i in range(8, len(original), 96)
               if struct.unpack_from('<I', original, i)[0] == spec['actor_id']]
    if len(matches) != 1:
        raise ValueError('expected one Necromancer record')
    at = matches[0]
    if hashlib.sha256(original[at:at+96]).hexdigest() != spec['record_sha256']:
        raise ValueError('unexpected Necromancer object record')
    if (original[at+8:at+40].rstrip(b'\0') != spec['model'].encode()
            or original[at+40:at+72].rstrip(b'\0') != shared.name.encode()):
        raise ValueError('unexpected Necromancer model or shared motion name')
    result = bytearray(original)
    result[at+40:at+72] = private.name.encode().ljust(32, b'\0')
    if hashlib.sha256(result[at:at+96]).hexdigest() != spec['result_record_sha256']:
        raise ValueError('unexpected patched object record')

    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as tmp:
        stage = Path(tmp) / 'pack'
        (stage / 'obj').mkdir(parents=True)
        (stage / table).write_bytes(result)
        (stage / private).write_bytes(motion)
        for directory in (Path('.'), Path('obj')):
            if (pack / directory).is_dir():
                for source in (pack / directory).iterdir():
                    target = stage / directory / source.name
                    if not target.exists():
                        target.symlink_to(source)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('base', 'pack', 'stock', 'output'):
        parser.add_argument(arg, type=Path)
    args = parser.parse_args()
    build(args.base.resolve(), args.pack.resolve(), args.stock.resolve(), args.output.absolute())
