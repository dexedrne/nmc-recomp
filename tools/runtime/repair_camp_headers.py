"""Remove displaced title draws from the NMC 1.2 camp panel animations."""
import argparse
import hashlib
import struct
from pathlib import Path

# These frame groups were repurposed for the seven page titles in NMC.
TITLES = {73, 78, 90, 93, 96, 99, 102}


def repair(source):
    if hashlib.sha256(source[48:205408]).hexdigest() != 'a2e1ee6538b9f982da45ba3d89e194b28d2785b7691f4a1d8e4af524801e7680':
        raise ValueError('camp-layout-version: expected NMC 1.2 camp layout')
    result = bytearray(source)
    seq = 2180
    if source[seq:seq + 4] != b'SEQD':
        raise ValueError('camp-sequence: missing camp animation table')
    count, offset = struct.unpack_from('<II', source, seq + 32)
    # Group 8 is the layout's existing empty (zero-area) frame.
    changed = kept = 0
    for i in range(count):
        anim = seq + offset + 144 * i
        group, _, _, x0, x1, y0, y1 = struct.unpack_from('<7i', source, anim + 4)
        if group in TITLES:
            if any((x0, x1, y0, y1)):
                struct.pack_into('<I', result, anim + 4, 8)
                changed += 1
            else:
                kept += 1
    if (changed, kept) != (69, 42):
        raise ValueError(f'camp-header-references: {changed} panel and {kept} title draws')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    output = repair(args.source.read_bytes())
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(output)
