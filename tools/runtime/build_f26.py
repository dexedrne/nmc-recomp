#!/usr/bin/env python3
"""Extend F5's level-one Doom compensation to the authored Data Marluxia selector."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F26-doom.json'


def patched(data, spec):
    if hashlib.sha256(data).hexdigest() != spec['sha256']:
        raise ValueError('F26 requires the pinned F5 actor from the cumulative RMC pack')
    entry = 16 + 16 * spec['ordinal']
    kind, link, name, offset, size = struct.unpack_from('<HH4sII', data, entry)
    if (kind, link, name, offset + size) != (3, 0, b'n_cm', len(data)):
        raise ValueError('F26 expected the F5 script at the end of the actor')
    code = offset + 16
    end = (size - 16) // 2
    store = spec['store_pc']
    branch, relative = struct.unpack_from('<Hh', data, code + 2 * store)
    start = store + 2 + relative
    result = bytearray(data)
    # F5 already filtered level 1. Extend only the world-mismatch path.
    site = start + 13
    if branch != 7 or struct.unpack_from('<2H', data, code + 2 * site) != (0x87, 20):
        raise ValueError('F26 expected the F5 world-mismatch branch')
    keep = site + 2 + 20
    compensate = keep - 4
    if struct.unpack_from('<6H', data, code + 2 * compensate) != (0xc9, 0, 13, 0, 0x81, 152):
        raise ValueError('F26 expected the existing Doom counter assignment')
    struct.pack_into('<h', result, code + 2 * (site + 1), end - (site + 2))
    words = []
    for query, expected in [(23, spec['world']), (24, spec['area']), (26, spec['battle'])]:
        words.extend([0x4a, query, 0, expected, 0, 0x46, 0x87])
        words.append((keep - (end + len(words) + 1)) & 0xffff)
    words.extend([7, (compensate - (end + len(words) + 2)) & 0xffff])
    result.extend(struct.pack(f'<{len(words)}H', *words))
    struct.pack_into('<I', result, entry + 12, size + 2 * len(words))
    return result


def build(pack, output):
    spec = json.loads(RECIPE.read_text())
    if output.is_symlink() or output.exists() or output.resolve().is_relative_to(pack.resolve()):
        raise ValueError('choose a fresh output outside the source pack')
    data = patched((pack / spec['path']).read_bytes(), spec)
    if hashlib.sha256(data).hexdigest() != spec['result_sha256']:
        raise ValueError('F26 output actor digest mismatch')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = Path(temp) / 'pack'
        dest = stage / spec['path']
        dest.parent.mkdir(parents=True)
        dest.write_bytes(data)
        for directory in (Path('.'), Path('obj')):
            for child in (pack / directory).iterdir():
                target = stage / directory / child.name
                if not target.exists():
                    target.symlink_to(child.resolve())
        stage.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.pack, args.output)
