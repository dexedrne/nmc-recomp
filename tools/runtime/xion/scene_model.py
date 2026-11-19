"""Add the event-rigged H_EX500 to a local X5 look pack."""
import argparse
from pathlib import Path

from PIL import Image
from hud import patch_face
from mdlx import Archive, Model, OBJ, P, R, load, np


def build(pack):
    donor = Archive((pack / 'obj/P_EX100.mdlx').read_bytes())
    shell = load('H_EX500')
    field = Model(donor.members[4])
    scene = Model(shell.members[4])
    raw = bytearray(field.template)
    h = 144
    shift = len(scene.bones) - len(field.bones)
    next_model = R('<I', raw, h + 12)[0]
    if not next_model:
        raw.extend(bytes(-len(raw) % 16))
    shadow = h + next_model if next_model else len(raw)
    native_shadow = h + R('<I', scene.template, h + 12)[0]
    for group, (_, packets) in enumerate(field.groups):
        offsets = h + 32 + 32 * group + 16
        dma, palette = R('<II', raw, offsets)
        P('<II', raw, offsets, dma + shift, palette + shift)
        for packet in packets:
            # H_EX500 inserts 56 facial joints before the field rig's second arm.
            targets = [j if j <= 140 else j + 56 for j in packet.palette]
            assert np.allclose(field.rest[list(packet.palette)], scene.rest[targets], atol=1e-4)
            for joint, (tag, listing) in zip(targets, packet.edits):
                P('<I', raw, tag, joint)
                P('<I', raw, listing, joint)
            addr = packet.first + 4
            P('<I', raw, addr, R('<I', raw, addr)[0] + shift)
    assert field.bo - field.aux == scene.bo - scene.aux
    raw[shadow:] = scene.template[native_shadow:]
    raw[h + field.bo:h + field.bo + len(field.bones)] = scene.bones
    P('<H', raw, h + 16, scene.count)
    P('<I', raw, h + 12, shadow + shift - h)
    shell.members.update({4: bytes(raw), 7: donor.members[7]})
    encoded = shell.write()
    result = Model(Archive(encoded).members[4])
    assert result.bones == scene.bones
    for old, new in zip(field.mesh(field.rest), result.mesh(result.rest)):
        assert np.allclose(old, new, atol=1e-4)
    (pack / 'obj/H_EX500.mdlx').write_bytes(encoded)
    print('H_EX500', len(encoded), scene.count, 'bones, rest mesh checked')


def shop_faces(pack):
    # Use the first, healthy face from the X5 gauge atlas.
    with Image.open(pack / 'remastered/obj/P_EX100.a.us/-36.dds') as atlas:
        face = atlas.convert('RGBA').crop((0, 0, 270, 320))
        face = face.crop(face.getbbox())
    for stem in ('p_ex100', 'p_ex101', 'p_ex100_wi'):
        key = Path('menu/shopface') / (stem + '.bin')
        with Image.open(OBJ.parent / 'remastered' / key / '-0.dds') as original:
            image = Image.new('RGBA', original.size)
            box = original.convert('RGBA').getbbox()
        resized = face.copy()
        resized.thumbnail((box[2]-box[0], box[3]-box[1]), Image.Resampling.LANCZOS)
        image.alpha_composite(resized, ((box[0]+box[2]-resized.width)//2,box[3]-resized.height))
        target = pack / 'remastered' / key / '-0.dds'
        target.parent.mkdir(parents=True, exist_ok=True)
        image.save(target)
        with Image.open(target) as saved:
            assert np.array_equal(np.asarray(saved.convert('RGBA')), np.asarray(image))
        (pack / key).parent.mkdir(parents=True, exist_ok=True)
        (pack / key).write_bytes(patch_face((OBJ.parent / key).read_bytes(), image, b'p_ex'))
        print(key, image.size, 'embedded and HD')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('look_pack', type=Path)
    pack = parser.parse_args().look_pack
    build(pack)
    shop_faces(pack)
