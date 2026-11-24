"""Build the three-screen title layout (screens.2ld): ROXAS MAY CRY, XION ALWAYS CRIES, AXEL WON'T CRY.

One resident layout holds every picture and logo, so R3 on the title screen switches screens live (aspect.c f_1948c0, DLL logo.c
draw_layout rebind texture slots 3 / 4 / 5 / 6 / 7 and copy the chosen logo's geometry).  Texture slots:
   0-8   the game's title textures (3 / 5 / 6 = the boot-intro hooded-figure line-art sketches, 4 = the Roxas picture, 7 = the mod's
         own logo atlas)
   9-11  the controller sheets (aliases of 8, as f_2b4c60 expects)
   12    the Xion picture         13  the Axel picture
   14    the roxas logo atlas     15  the xion logo atlas     16  the axel logo atlas
   17-19 the Roxas boot sketches (-3 / -5 / -6), 20-22 the Xion sketches, 23-25 the Axel sketches (17 + 3*screen + k)
The HD pictures and sketches are NOT read from this file: the port serves slot 4 from remastered/menu/us/title.2ld/-4.png (the rmc
pack), 12 / 13 from each look pack's remastered/menu/us/title.2ld/-4.png, 14-16 from <logo dir>/{roxas,xion,axel}.png and 17-25 from
each screen's pack remastered/menu/us/title.2ld/-{3,5,6}.png; the DLL serves remastered/menu/us/rmcsc.2ld/-<slot>.png.  The SD copies
here (pictures downscaled to 512x512, each logo .2ld's own texture 7, the sketches the native slot 3 / 5 / 6 texture) only fill the
slots.  After the layout: b"T90G" + 4 x (16-byte sprite UV + 120-byte part rectangles) for the native, roxas, xion and
axel logos (taken from the logo .2ld files, pack_title_logo.py), at byte 79092 of the layout; the file ends in b"T90L"."""
import argparse
import hashlib
import struct
from pathlib import Path

import numpy as np
from PIL import Image

LAYOUT = Path("menu/us/title.2ld")
LOGOS = ("roxas", "xion", "axel")
SEQ = 0x7f4c
GEOMETRY_AT = 79092          # = the native layout's length; runtime/aspect.c TITLE_SCREENS_GEOMETRY, DLL logo.c


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def texture(blob, slot):
    imgz = u32(blob, 40)
    off, size = struct.unpack_from("<II", blob, imgz + 16 + 8 * slot)
    return blob[imgz + off:imgz + off + size]


def classic_picture(template, path):
    """Match the original 512x512 indexed background's 263,232-byte allocation."""
    result = bytearray(template)
    pixel_offset, size, palette_offset, length = struct.unpack_from('<4I', result, 8)
    assert (size, length) == (512 * 512, 1024) and not result[60] & 4
    with Image.open(path) as image:
        rgb = image.convert('RGB').resize((512, 512), Image.Resampling.LANCZOS)
    indexed = rgb.quantize(256, Image.Quantize.MEDIANCUT)
    result[pixel_offset:pixel_offset + size] = indexed.tobytes()
    palette = np.frombuffer(result, np.uint8, length, palette_offset).reshape(256, 4)
    index = np.arange(256)
    palette[(index&0xe7) | ((index&8)*2) | ((index&16)//2), :3] = np.array(indexed.getpalette(), np.uint8).reshape(256, 3)
    palette[:, 3] = 128
    return bytes(result)


def geometry(blob):
    assert blob[SEQ:SEQ+4] == b"SEQD"
    uv = SEQ + u32(blob, SEQ+12) + 2*44 + 4
    parts = SEQ + u32(blob, SEQ+20)
    return blob[uv:uv+16] + blob[parts:parts+120]


def build(data, logos, xion, axel, out, roxas=None):
    original = (data / LAYOUT).read_bytes()
    assert hashlib.sha256(original).hexdigest() == "45eeab550359e1f38dd42bb1702c06ac373b69121f0521e8bb00804852fb5ca1"
    lay = bytearray(original[48:48+u32(original, 28)])
    assert len(lay) == GEOMETRY_AT
    textures = [texture(original, i) for i in range(9)]
    if roxas:
        textures[4] = classic_picture(textures[4], roxas)
    textures.extend([textures[8]] * 3)
    for picture in (xion, axel):
        textures.append(classic_picture(textures[4], picture))
    blobs = [(logos / f"{name}.2ld").read_bytes() for name in LOGOS]
    for name, blob in zip(LOGOS, blobs):
        assert blob[-5:-1] == b"T80L", f"{name}.2ld is not a pack_title_logo.py layout"
        textures.append(texture(blob, 7))
    # Boot-intro sketches (17..25): the SD copy is the native sketch texture the slot stands in for (3 / 5 / 6) per screen; the HD
    # is served from each screen's pack remastered/menu/us/title.2ld/-{3,5,6}.png by runtime/hd_assets.c (never read here).
    for screen in range(3):
        for native in (3, 5, 6):
            textures.append(texture(original, native))
    lay += b"T90G" + geometry(original) + b"".join(geometry(blob) for blob in blobs)
    # Round 5: the runtime draws 12/13 in place of slot 4 and 14..16 in place of slot 7; anything but the native allocation (byte
    # count + IMGD header) makes the Load menu's save check fail with that texture on screen (runtime/file_layer.c
    # kh_title_screens_check, DLL logo.c title_screens_check refuse such a layout).
    for slot in range(12, 17):
        native = textures[4 if slot < 14 else 7]
        assert len(textures[slot]) == len(native) and textures[slot][8:64] == native[8:64], \
            f"texture {slot} is not in the allocation of native slot {4 if slot < 14 else 7}"
    imgz = bytearray(b"IMGZ" + struct.pack("<3I", 256, 16, len(textures)) + bytes(8*len(textures)))
    for i, tex in enumerate(textures):
        imgz.extend(bytes(-len(imgz) % 16))
        struct.pack_into("<2I", imgz, 16+8*i, len(imgz), len(tex))
        imgz += tex
    bar = bytearray(original[:48]) + lay
    bar.extend(bytes(-len(bar) % 16))
    struct.pack_into("<I", bar, 28, len(lay))
    struct.pack_into("<II", bar, 40, len(bar), len(imgz))
    out.mkdir(parents=True, exist_ok=True)
    (out / "screens.2ld").write_bytes(bar + imgz + b"T90L")
    print(f"{out/'screens.2ld'}: {len(textures)} textures, {len(bar)+len(imgz)+4} bytes")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("data", type=Path, help="the tree holding menu/us/title.2ld (data-full)")
    p.add_argument("logos", type=Path, help="folder with roxas.2ld, xion.2ld, axel.2ld (pack_title_logo.py)")
    p.add_argument("xion", type=Path, help="the Xion title picture (only its SD copy goes in)")
    p.add_argument("axel", type=Path, help="the Axel title picture (only its SD copy goes in)")
    p.add_argument("out", type=Path)
    p.add_argument("--roxas", type=Path, help="Roxas picture for the classic slot 4")
    a = p.parse_args()
    build(a.data, a.logos, a.xion, a.axel, a.out, a.roxas)
