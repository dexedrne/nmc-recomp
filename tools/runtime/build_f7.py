"""Install the PC Larxene objective for the US story mission in a sparse overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory


def build(base, output, rmc):
    spec = json.loads((Path(__file__).resolve().parents[1] /
                       'patches/rmc/F7-larxene.json').read_text())
    sys.path.insert(0, str(rmc / 'upstream/tools'))
    from formats import bar, directory_record, require, selected
    from message_pool import pack_messages, read_messages

    def source(key):
        path = spec[key]
        data = (base / path).read_bytes()
        require(hashlib.sha256(data).hexdigest() == spec['sha256'][path],
                f'F7 input digest mismatch for {path}')
        return data

    mission = bytearray(source('mission'))
    header = selected(bar(mission), 0, 2, 'HB33')
    objective = header.offset + 10
    key = spec['larxene_objective']
    require(struct.unpack_from('<H', mission, objective)[0] == spec['old_objective'],
            'F7 mission objective mismatch')
    struct.pack_into('<H', mission, objective, key)

    world = bytearray(source('world_messages'))
    donor = source('donor_messages')
    member = selected(bar(world), 0, 2, 'nm')
    pool = read_messages(bytes(member.data(world)))
    require(key not in pool, 'F7 objective ID already used in nm')
    pool[key] = read_messages(selected(bar(donor), 0, 2, 'hb').data(donor))[key]
    payload = pack_messages(pool, True)
    world.extend(bytes(-len(world) % 16))
    directory_record(world, member.ordinal, len(world), len(payload))
    world.extend(payload)
    require(read_messages(bar(world)[0].data(world)) == pool, 'F7 message verification failed')

    require(not output.exists(), f'F7 output already exists at {output}')
    with TemporaryDirectory(dir=output.parent) as stage:
        pack = Path(stage) / 'pack'
        for path, data in [(spec['mission'], mission), (spec['world_messages'], world)]:
            require(hashlib.sha256(data).hexdigest() == spec['result_sha256'][path],
                    f'F7 output digest mismatch for {path}')
            leaf = pack / path
            leaf.parent.mkdir(parents=True)
            leaf.write_bytes(data)
        pack.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rmc', required=True, type=Path)
    args = parser.parse_args()
    build(args.base, args.output, args.rmc)
