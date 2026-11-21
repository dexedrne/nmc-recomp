"""Merge three chest IDs into a fresh copy of the shared RMC pack."""
from pathlib import Path
import shutil
import struct
import sys
import tempfile

work = Path('/home/user/work')
sys.path[:0] = [str(work/'nmc-decomp/tools'), str(work/'nmc-decomp/tools/rooms')]
from formats import bar, require
from records import tokens, unpack, SCHEMA

shared = work/'khnmc-packs/rmc'
stock = work/'khnmc-build/data-full'
repairs = {'po06': {1102: 1}, 'po07': {1002: 1, 1003: 2}}


def chest_member(archive):
    member, = [m for m in bar(archive) if (m.kind, m.name, m.link) == (12, b'm_70', 0)]
    return member


def repair(room):
    path = Path(f'ard/us/{room}.ard')
    base = (stock/path).read_bytes()
    current = (shared/path).read_bytes() if (shared/path).exists() else base
    original, merged = chest_member(base), chest_member(current)
    wanted = repairs[room]
    base_entities = {raw for kind, raw in tokens(12, original.data(base)) if kind == 'entity'}
    result = bytearray(current)
    offset = merged.offset
    seen = set()
    for kind, raw in tokens(12, merged.data(current)):
        if kind == 'entity':
            entity = unpack(SCHEMA[kind][0], raw)
            obj, argument, serial, arg1 = entity[0], *entity[8:11]
            if serial in wanted:
                require(obj == 321 and argument == wanted[serial] and arg1 == 0
                        and raw in base_entities and serial not in seen,
                        f'conflicting chest placement in {room} serial {serial}')
                struct.pack_into('<i', result, offset+32, wanted[serial])
                seen.add(serial)
        offset += len(raw)
    require(seen == set(wanted), f'missing chest placement in {room}')
    require(sum(a != b for a, b in zip(result, current)) == len(wanted),
            'unexpected binary delta')
    return path, result


def build(out):
    require(not out.exists(), 'output directory already exists')
    with tempfile.TemporaryDirectory(dir=out.parent) as temporary:
        stage = Path(temporary)/'pack'
        shutil.copytree(shared, stage)
        for room in repairs:
            path, data = repair(room)
            (stage/path).write_bytes(data)
        stage.rename(out)


if __name__ == '__main__':
    build(Path(sys.argv[1]))
