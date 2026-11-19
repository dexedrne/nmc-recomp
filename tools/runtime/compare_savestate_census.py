"""Compare two private CPU census files. Report only guest addresses and counts."""
from pathlib import Path
import struct
import sys


def records(path):
    with Path(path).open('rb') as f:
        if f.read(8) != b'KHCPU01\n':
            raise ValueError(f'{path}: unknown CPU census format')
        while True:
            key = f.read(8)
            if key == b'KHCPUEND' and not f.read(1):
                return
            fields = key + f.read(8)
            if len(fields) != 16:
                raise ValueError(f'{path}: incomplete record')
            address, size, stored = struct.unpack('<QII', fields)
            if not 0 < size <= 65536 or stored not in (0, size):
                raise ValueError(f'{path}: invalid length at {address:#x}')
            data = f.read(stored) if stored else bytes(size)
            if len(data) != size:
                raise ValueError(f'{path}: short payload at {address:#x}')
            yield address, data


def compare(left, right):
    from itertools import zip_longest
    total = changed = 0
    for a, b in zip_longest(records(left), records(right)):
        if not a or not b or a[0] != b[0] or len(a[1]) != len(b[1]):
            raise ValueError('census layout mismatch')
        address, data = a
        total += len(data)
        if data != b[1]:
            if not changed:
                first = next(i for i, (x, y) in enumerate(zip(data, b[1])) if x != y)
                print(f'first differing guest byte {address + first:#x}')
            changed += 1
    print(f'compared {total} guest bytes; {changed} differing records')
    return bool(changed)


if __name__ == '__main__':
    sys.exit(compare(*sys.argv[1:]))
