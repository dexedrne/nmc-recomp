"""Build the six unhooded Xion HUD replacements from a 1.2 look directory."""
import argparse
from pathlib import Path
import struct

import numpy as np
from mdlx import directory


def neutralize(rgba, top):
    r, g, b, a = rgba.astype(np.int32).transpose(2, 0, 1)
    # The red lining starts below the chin in each of the three expression cells.
    mask = (r > 2*g) & (r > 2*b) & (b*3 >= g*2) & (a > 0)
    mask[:top] = False
    shade = (54*r + 183*g + 19*b + 128)//256
    out = rgba.copy()
    out[mask, :3] = shade[mask, None]
    assert mask.any() and np.array_equal(out[:, :, 3], rgba[:, :, 3])
    return out


def repair_dds(blob):
    height, width = struct.unpack_from('<II', blob, 12)
    assert blob[:4] == b'DDS ' and (height, width) == (512, 1024)
    assert struct.unpack_from('<8I', blob, 76) == (32, 65, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000)
    bgra = np.frombuffer(blob[128:], np.uint8).reshape(height, width, 4)
    order = [2, 1, 0, 3]
    return blob[:128] + neutralize(bgra[:, :, order], 220)[:, :, order].tobytes()


def repair_bar(blob):
    _, _, _, off, size = next(e for e in directory(blob) if e[0] == 24 and e[2] == b'face')
    buf, count, pal, length = struct.unpack_from('<4I', blob, off+8)
    assert blob[off:off+4] == b'IMGD' and struct.unpack_from('<HH', blob, off+28) == (256, 128)
    assert count == 32768 and length == 1024 and max(buf+count, pal+length) <= size
    pixels = np.frombuffer(blob, np.uint8, count, off+buf).reshape(128, 256).copy()
    y, x = np.indices(pixels.shape)
    address = y*256 + x
    if blob[off+60] & 4:
        address = ((y & ~15)*256 + (x & ~15)*2 + (((y & ~3)//2 + (y & 1)) % 8)*512
                   + ((x + (((y+2)//4) & 1)*4) & 7)*4 + ((y//2) & 1) + ((x//4) & 2))
    assert len(np.unique(address)) == count
    pixels = pixels.ravel()[address]
    permutation = np.arange(256)
    permutation = (permutation & 0xe7) | ((permutation & 8) << 1) | ((permutation & 16) >> 1)
    colors = np.frombuffer(blob, np.uint8, length, off+pal).reshape(256, 4)[permutation]
    original = colors[pixels]
    fixed = neutralize(original, 55)
    changed = np.any(original != fixed, axis=2)
    spare = iter(sorted(set(range(256)) - set(np.unique(pixels))))
    for old in np.unique(pixels[changed]):
        use = (pixels == old) & changed
        index = next(spare) if np.any((pixels == old) & ~changed) else old
        colors[index] = fixed[use][0]
        pixels[use] = index
    assert np.array_equal(colors[pixels], fixed)
    encoded = np.empty(count, np.uint8)
    encoded[address] = pixels
    result = bytearray(blob)
    result[off+buf:off+buf+count] = encoded.tobytes()
    result[off+pal:off+pal+length] = colors[permutation].tobytes()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    for stem, slot in [('P_EX101', 36), ('P_EX101_BTLF', 47), ('P_EX110_BTLF', 0)]:
        name = Path('obj') / (stem + '.a.us')
        for path, repair in [(name, repair_bar), (Path('remastered') / name / f'-{slot}.dds', repair_dds)]:
            dest = args.output / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            result = repair((args.source / path).read_bytes())
            with dest.open('xb') as output:
                output.write(result)


if __name__ == '__main__':
    main()
