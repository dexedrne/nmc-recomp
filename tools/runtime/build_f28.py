"""Build the three Mushroom battle selectors over the current RMC pack into a fresh directory."""
import hashlib
import json
import shutil
import struct
import sys
import tempfile
from pathlib import Path


def build(base, pack, output, stock):
    spec = json.loads((Path(__file__).resolve().parents[1] / 'patches/rmc/F28-tt25.json').read_text())
    rel = spec['path']
    source = pack / rel if (pack / rel).exists() else base / rel
    data = bytearray(source.read_bytes())
    offset = spec['offset']
    if hashlib.sha256(data).hexdigest() != spec['sha256'] or data[offset:offset+4] != spec['old'].encode():
        raise ValueError('unexpected TT25 battle selector or archive digest')
    data[offset:offset+4] = spec['new'].encode()
    if hashlib.sha256(data).hexdigest() != spec['result_sha256']:
        raise ValueError('wrong TT25 result digest')
    if output.exists() or any(root.resolve() in output.resolve().parents for root in (base, pack)):
        raise ValueError('choose a fresh output outside the source directories')
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp:
        stage = Path(tmp) / 'pack'
        shutil.copytree(pack, stage)
        (stage / rel).write_bytes(data)
        recipe = Path(__file__).resolve().parents[1] / 'patches/rmc/F28-postgame.json'
        for room in json.loads(recipe.read_text()):
            rel = room['path']
            target = stage / rel
            archive = bytearray((target if target.exists() else base / rel).read_bytes())
            donor = (stock / rel).read_bytes()
            if (hashlib.sha256(archive).hexdigest() != room['sha256'] or
                    hashlib.sha256(donor).hexdigest() != room['source_sha256']):
                raise ValueError(f'unexpected postgame archive for {rel}')
            entry = room['directory']
            _, name, start, length = struct.unpack_from('<I4sII', archive, entry)
            if name != b'btl\0':
                raise ValueError(f'missing battle script for {rel}')
            cut, src = room['offset'], room['source_offset']
            member = (archive[start:cut] + donor[src:src+room['size']] +
                      archive[cut+room['old_size']:start+length])
            archive.extend(bytes(-len(archive) % 16))
            struct.pack_into('<II', archive, entry+8, len(archive), len(member))
            archive.extend(member)
            if hashlib.sha256(archive).hexdigest() != room['result_sha256']:
                raise ValueError(f'wrong patched digest for {rel}')
            target.write_bytes(archive)
        stage.rename(output)


if __name__ == '__main__':
    build(*(Path(arg) for arg in sys.argv[1:]))
