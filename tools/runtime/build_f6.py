#!/usr/bin/env python3
"""Build the sparse Agrabah story overlay from the PC data and extracted V0.20 hb43.map."""
import argparse
import json
import runpy
import struct
import tempfile
from pathlib import Path

RECIPE = Path(__file__).resolve().parents[1] / 'patches/rmc/F6-lexaeus.json'
NODE = struct.Struct('<14h2H')
MESH = struct.Struct('<6h2HBBH')
FACE = struct.Struct('<8H')
WIDTHS = (80, NODE.size, MESH.size, FACE.size, 16, 16, 12, 4)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sections(data):
    require(data[:8] == b'COCT\x01\0\0\0', 'expected COCT version 1')
    parts = []
    for (offset, size), width in zip(struct.iter_unpack('<II', data[16:80]), WIDTHS, strict=True):
        require(offset + size <= len(data) and size % width == 0, 'invalid collision section')
        parts.append(bytearray(data[offset:offset+size]))
    require(len(parts[0]) == 80, 'invalid collision header')
    return parts


def append_group(original, donor, group):
    dst, src = sections(original), sections(donor)
    count = [len(part)//width for part, width in zip(dst, WIDTHS, strict=True)]
    require(len(src[1]) == NODE.size, 'expected a one-node reference arena')
    child = list(NODE.unpack(src[1]))
    root = list(NODE.unpack_from(dst[1]))
    require(child[:8] == [-1]*8 and -1 in root[:8], 'expected a leaf and a free root branch')
    require(all(row[9] != group for row in MESH.iter_unpack(dst[2])), 'collision group already in use')
    # Attach the reference leaf to a vacant child slot so existing tree indices stay valid.
    root[root[:8].index(-1)] = count[1]
    root[8:11] = map(min, root[8:11], child[8:11])
    root[11:14] = map(max, root[11:14], child[11:14])
    NODE.pack_into(dst[1], 0, *root)
    child[14:] = [i+count[2] for i in child[14:]]
    dst[1].extend(NODE.pack(*child))
    for record in MESH.iter_unpack(src[2]):
        mesh = list(record)
        mesh[6:8] = [i+count[3] for i in mesh[6:8]]
        mesh[9] = group
        dst[2].extend(MESH.pack(*mesh))
    for record in FACE.iter_unpack(src[3]):
        face = list(record)
        # Four vertices, then plane, bounds and surface attributes. 0xffff means absent.
        for field, table in enumerate((4,4,4,4,5,6,7), 1):
            if face[field] != 0xffff:
                face[field] += count[table]
        dst[3].extend(FACE.pack(*face))
    for i in range(4, 8):
        dst[i].extend(src[i])

    result = bytearray(dst[0])
    for i in range(1, 8):
        result.extend(bytes(-len(result) % 16))
        struct.pack_into('<II', result, 16+8*i, len(result), len(dst[i]))
        result.extend(dst[i])
    return result


def visibility(payload, spec):
    result = bytearray(payload)
    offset = 0
    while offset + 4 <= len(result):
        program, length = struct.unpack_from('<HH', result, offset)
        if program == 0xffff:
            break
        require(4 <= length <= len(result)-offset, 'invalid map program length')
        if program == spec['program']:
            require(length == 16 and struct.unpack_from('<HHII', result, offset+4) ==
                    (1, 2, *spec['visibility']), 'unexpected story MapVisibility command')
            word = spec['group']//32
            struct.pack_into('<I', result, offset+8+4*word,
                             spec['visibility'][word] | (1 << (spec['group']%32)))
            return result
        offset += length
    raise ValueError('story map selector missing')


def build(base, output, rmc, reference):
    api = runpy.run_path(str(rmc/'tools/rmc-build.py'))
    specs = json.loads(RECIPE.read_text())
    require(not output.exists() and not output.is_symlink(), 'choose a fresh output directory')
    require(not output.resolve().is_relative_to(base.resolve()), 'output inside read-only base')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp)/'overlay'
        for s in specs:
            data = api['checked'](base, s['path'], s['sha256'])
            entry = api['selected'](api['bar'](data), s['ordinal'], s['kind'], s['name'], s['link'])
            result = bytearray(data)
            if s['op'] == 'enable-collision-group':
                result[entry.offset:entry.offset+entry.size] = visibility(entry.data(data), s)
            elif s['op'] == 'append-collision-group':
                donor = reference.read_bytes()
                require(api['sha'](donor) == s['source_sha256'], 'wrong V0.20 reference map')
                member = api['selected'](api['bar'](donor), s['source_ordinal'], 6, s['source_name'], s['source_link'])
                collision = member.data(donor)
                require(api['sha'](collision) == s['member_sha256'], 'wrong reference collision')
                payload = append_group(entry.data(data), collision, s['group'])
                result.extend(bytes(-len(result) % 16))
                api['directory_record'](result, entry.ordinal, len(result), len(payload))
                result.extend(payload)
            else:
                raise ValueError('unknown F6 operation')
            require(api['sha'](result) == s['result_sha256'], 'wrong patched digest for '+s['path'])
            dest = stage/s['path']
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(result)
        stage.rename(output)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('base', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--rmc', required=True, type=Path)
    p.add_argument('--ps2-map', required=True, type=Path)
    args = p.parse_args()
    build(args.base, args.output, args.rmc, args.ps2_map)
