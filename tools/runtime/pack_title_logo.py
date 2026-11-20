"""Build a local NMC title layout and HD atlas from one spelling-checked PNG."""
import argparse
import hashlib
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

LAYOUT = Path("menu/us/title.2ld")
NAMES = ("roxas", "xion", "xkb")


def pack(root, name, image, output):
    layout = bytearray((root / LAYOUT).read_bytes())
    assert hashlib.sha256(layout).hexdigest() == "45eeab550359e1f38dd42bb1702c06ac373b69121f0521e8bb00804852fb5ca1"
    hd = Image.open(root / "remastered" / LAYOUT / "-7.png").convert("RGBA")
    credit = (316, 0, 512, 33)
    assert hd.size == (1024, 1024)
    logo = Image.open(image).convert("RGBA")
    assert logo.getchannel("A").getextrema() == (0, 255)
    logo = logo.crop(logo.getbbox())
    width, height = ImageOps.contain(logo, (350, 188)).size

    # Sequence 6, part 5: keep the wordmark's animated centre and draw the whole emblem.
    seq = 0x7f4c
    assert layout[seq:seq+4] == b'SEQD'
    sprites, _, parts = struct.unpack_from('<3I', layout, seq+12)
    uv = (4, 68, 508, 508)
    struct.pack_into('<4i', layout, seq+sprites+44*2+4, *uv)
    struct.pack_into('<4i', layout, seq+parts+20*5, -width//2, -height//2+13,
                     width-width//2, height-height//2+13)
    for i in range(5):
        struct.pack_into('<4i', layout, seq+parts+20*i, 0, 0, 0, 0)

    atlas = Image.new("RGBA", (2048, 2048))
    credit_hd = hd.crop(tuple(2*v for v in credit))
    atlas.paste(credit_hd.resize((credit_hd.width*2, credit_hd.height*2), Image.Resampling.NEAREST),
                (4*credit[0], 0))
    atlas.paste(logo.resize(((uv[2]-uv[0])*4, (uv[3]-uv[1])*4), Image.Resampling.LANCZOS),
                (4*uv[0], 4*uv[1]))
    output.mkdir(parents=True, exist_ok=True)
    atlas.save(output / (name+".png"))

    # IMGD 7 is an 8-bit swizzled 512x512 texture. Keep all CLUT entries used by the credit.
    off = 0x128bd0
    assert layout[off:off+4] == b'IMGD'
    pix, size, clut, length = struct.unpack_from('<4I', layout, off+8)
    assert (size, length) == (512*512, 1024) and layout[off+60] & 4
    y, x = np.indices((512, 512))
    address = ((y&~15)*512 + (x&~15)*2 + (((y&~3)//2 + (y&1))%8)*1024
               + ((x + (((y+2)//4)&1)*4)&7)*4 + ((y//2)&1) + ((x//4)&2))
    assert np.unique(address).size == size
    pixels = np.frombuffer(layout, np.uint8, size, off+pix)
    palette = np.frombuffer(layout, np.uint8, length, off+clut).reshape(256, 4)
    keep = np.unique(pixels[address[:credit[3], credit[0]:]])
    free = np.setdiff1d(np.arange(256), keep)
    sd = atlas.resize((512,512), Image.Resampling.LANCZOS).quantize(len(free), Image.Quantize.FASTOCTREE)
    colors = np.array(sd.getpalette("RGBA"), np.uint8).reshape(-1,4)
    colors[:,3] = (colors[:,3].astype('u2')+1)//2
    ix = free[:len(colors)]
    palette[(ix&0xe7) | ((ix&8)*2) | ((ix&16)//2)] = colors
    replacement = free[np.asarray(sd)]
    replacement[:credit[3], credit[0]:] = pixels[address[:credit[3], credit[0]:]]
    pixels[address] = replacement
    idx = 1 + NAMES.index(name)
    # Distinct sizes are part of the native HD table cache and savestate key.
    (output / (name+".2ld")).write_bytes(layout + bytes(idx) + b'T80L' + bytes([idx]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", choices=NAMES)
    parser.add_argument("image", type=Path)
    parser.add_argument("--data", type=Path, default=Path("data-full"))
    parser.add_argument("--out", type=Path, default=Path("private/logos"))
    args = parser.parse_args()
    pack(args.data, args.name, args.image, args.out)
