#!/usr/bin/env python3
"""Build the optional Port Royal II Doom overlay from a pinned PC actor."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F5-doom.json'


def patched(data, spec):
    if hashlib.sha256(data).hexdigest() != spec['sha256']:
        raise ValueError('F5 input actor digest mismatch')
    entry = 16 + 16 * spec['ordinal']
    kind, link, name, offset, size = struct.unpack_from('<HH4sII', data, entry)
    if (kind, link, name.decode()) != (spec['kind'], spec['link'], spec['name']):
        raise ValueError('F5 actor script member mismatch')
    script = bytearray(data[offset:offset + size])
    pc = spec['store_pc']
    site = 16 + 2 * pc
    store = (0x81, spec['counter_offset'])
    if struct.unpack_from('<4H', script, site - 4) != (0x4a, 361, *store):
        raise ValueError('F5 expected sheet level read and Doom store')
    start = (size - 16) // 2
    # Preserve the level on the stack while testing level, world, room and selector.
    code = [0x149]  # dup
    exits = []
    for query, value in [((), spec['level']), ((0x4a, 23), spec['world']),
                         ((0x4a, 24), spec['area']), ((0x4a, 26), spec['battle'])]:
        code.extend((*query, 0, value, 0, 0x46, 0x87, 0))  # push, sub, bnez
        exits.append(len(code) - 1)
    code.extend((0xc9, 0, spec['doom'], 0))  # drop level and push compensation
    for branch in exits:
        code[branch] = len(code) - branch - 1
    code.extend((*store, 7, (pc + 2 - (start + len(code) + 4)) & 0xffff))
    struct.pack_into('<Hh', script, site, 7, start - (pc + 2))
    script.extend(struct.pack(f'<{len(code)}H', *code))
    # Keep graphics members and their physical offsets intact.
    result = bytearray(data)
    result.extend(bytes(-len(result) % 16))
    struct.pack_into('<II', result, entry + 8, len(result), len(script))
    result.extend(script)
    return result


def build(base, out):
    spec = json.loads(RECIPE.read_text())
    if out.is_symlink() or out.exists() or base.resolve() in out.resolve().parents:
        raise ValueError('F5 requires a fresh overlay outside the base')
    result = patched((base / spec['path']).read_bytes(), spec)
    if hashlib.sha256(result).hexdigest() != spec['result_sha256']:
        raise ValueError('F5 output actor digest mismatch')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent) as temp:
        stage = Path(temp) / 'pack'
        dest = stage / spec['path']
        dest.parent.mkdir(parents=True)
        dest.write_bytes(result)
        stage.rename(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.base, args.output)
