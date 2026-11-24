"""Build the resident title art from the installed Task 72T painting and Task 80 logos."""
import argparse
import hashlib
import shutil
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

LAYOUT = Path("menu/us/title.2ld")
LOGOS = ("roxas", "xion", "xkb")


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def texture(blob, slot):
    imgz = u32(blob, 40)
    off, size = struct.unpack_from("<II", blob, imgz + 16 + 8 * slot)
    return blob[imgz + off:imgz + off + size]


def rgba(template, image):
    # Slot 8 is linear RGBA32. Native GS alpha uses 0..128.
    head = bytearray(template[:64])
    w, h = image.size
    size = 4 * w * h
    struct.pack_into("<III", head, 12, size, 64 + size, 0)
    struct.pack_into("<4H", head, 28, w, h, w.bit_length()-1, h.bit_length()-1)
    struct.pack_into("<H", head, 36, w//64)
    image = image.convert("RGBA")
    image.putalpha(image.getchannel("A").point(lambda a: (a + 1)//2))
    return head + image.tobytes()


def build(data, logos, painting, out, font):
    original = (data / LAYOUT).read_bytes()
    assert hashlib.sha256(original).hexdigest() == "45eeab550359e1f38dd42bb1702c06ac373b69121f0521e8bb00804852fb5ca1"
    out.mkdir(parents=True, exist_ok=True)
    lay = bytearray(original[48:48+u32(original,28)])
    textures = [texture(original, i) for i in range(9)]
    if painting.resolve() != (out/"painting.png").resolve():
        shutil.copyfile(painting, out/"painting.png")
    if out.resolve() != logos.resolve():
        for name in LOGOS:
            for ext in ("png", "2ld"):
                shutil.copyfile(logos/f"{name}.{ext}", out/f"{name}.{ext}")
    # f_2b4c60 aliases controller sheets 9..11 to sheet 8 by name.
    textures.extend([textures[8]] * 3)
    pic = Image.open(painting)
    textures.append(rgba(textures[8], pic.resize((512,512), Image.Resampling.LANCZOS)))
    # Append immutable source rectangles for the native wordmark and the three replacements.
    geometry = bytearray(b"T89G")
    for name in (None, *LOGOS):
        blob = (logos / (name+".2ld")).read_bytes() if name else original
        if name:
            textures.append(texture(blob,7))
        seq = 0x7f4c
        uv = seq + u32(blob,seq+12) + 2*44 + 4
        parts = seq + u32(blob,seq+20)
        geometry += blob[uv:uv+16] + blob[parts:parts+120]
    # Each additional property inherits GAME SETTINGS' native animation and alpha.
    props = bytearray()
    groups = bytearray(lay[2552:2952])
    for i in range(20):
        start,count = struct.unpack_from("<HH",groups,20*i)
        first = len(props)//24
        for j in range(start,start+count):
            prop = lay[32+24*j:56+24*j]
            props += prop
            if u32(prop,4)==7 and 23 <= u32(prop,8) <= 25:
                struct.pack_into("<I",prop,0,16)
                struct.pack_into("<i",prop,16,88)
                props += prop
        struct.pack_into("<HH",groups,20*i,first,len(props)//24-first)
    delta = len(props)-105*24
    lay[32:2952] = props + groups
    struct.pack_into("<I",lay,8,len(props)//24)
    for offset in (20,28):
        struct.pack_into("<I",lay,offset,u32(lay,offset)+delta)
    for i in range(8):
        off = 2952+delta+4*i
        struct.pack_into("<I",lay,off,u32(lay,off)+delta)
    lay += geometry
    for key in ("LB","L1","L"):
        atlas = Image.new("RGBA",(512,512))
        draw = ImageDraw.Draw(atlas)
        draw.rounded_rectangle((89,469,137,503),radius=7,fill="#414148",outline="#bcbac0",width=2)
        draw.text((113,486),key,anchor="mm",font=ImageFont.truetype(font,21),fill="white")
        draw.text((148,486),"TITLE: ROXAS / XION",anchor="lm",font=ImageFont.truetype(font,28),fill="#302f34",stroke_width=1,stroke_fill="#eceaf0")
        atlas.save(out/("prompt-"+key+".png"))
        textures.append(rgba(textures[8],atlas.resize((256,256),Image.Resampling.LANCZOS)))
    imgz = bytearray(b"IMGZ"+struct.pack("<3I",256,16,len(textures))+bytes(8*len(textures)))
    for i,tex in enumerate(textures):
        imgz.extend(bytes(-len(imgz)%16))
        struct.pack_into("<2I",imgz,16+8*i,len(imgz),len(tex))
        imgz += tex
    bar = bytearray(original[:48])+lay
    bar.extend(bytes(-len(bar)%16))
    struct.pack_into("<I",bar,28,len(lay))
    struct.pack_into("<II",bar,40,len(bar),len(imgz))
    (out/"styles.2ld").write_bytes(bar+imgz+b"T89L")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("data","logos","painting","out"):
        p.add_argument(arg,type=Path)
    p.add_argument("--font",default="NotoSans-Bold.ttf")
    a = p.parse_args()
    build(a.data,a.logos,a.painting,a.out,a.font)
