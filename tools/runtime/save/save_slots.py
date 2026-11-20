#!/usr/bin/env python3
"""List the 99 Load-menu slots in a KH2FM WW card. The input is read-only."""
import argparse
import hashlib
import re
import struct
import sys
import zlib
from pathlib import Path

SIZE = 0x10FC0
ROW = 0x158
COUNT = 100  # The first physical entry is the system file.
DATA = 16 + COUNT * ROW
WORLDS = 'zz es tt di hb bb he al mu po lk lm dc wi nm wm ca tr eh'.split()
DIFFICULTY = ('Beginner', 'Standard', 'Critical', 'Less Than Nothing')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def card_chunks(png):
    require(png.startswith(b'\x89PNG\r\n\x1a\n'), 'invalid PNG signature')
    pos = 8
    saved = {}
    previous = None
    while pos + 12 <= len(png):
        size, tag = struct.unpack_from('>I4s', png, pos)
        end = pos + 8 + size
        require(end + 4 <= len(png), 'truncated PNG chunk')
        crc, = struct.unpack_from('>I', png, end)
        require(zlib.crc32(png[pos+4:end]) == crc, f'invalid {tag!r} CRC')
        if tag in (b'sqEX', b'khXb'):
            require(tag not in saved, f'duplicate {tag!r}')
            saved[tag] = png[pos+8:end]
        if tag == b'IEND':
            require(size == 0 and end + 4 == len(png), 'invalid PNG ending')
            require(b'sqEX' in saved, 'missing sqEX')
            return saved[b'sqEX'], saved.get(b'khXb') if previous == b'khXb' else None
        previous, pos = tag, end + 4
    raise ValueError('missing PNG ending')


def markers(chunk):
    if chunk is None:
        return {}
    require((len(chunk), chunk[:1]) in ((9601, b'\x01'), (9701, b'\x02')), 'unknown khXb layout')
    found = {}
    for i in range(COUNT):
        offset = 1 + i * 96
        name, sep, _ = chunk[offset:offset+64].partition(b'\0')
        mode = chunk[9601+i] if chunk[0] == 2 else int(bool(name))
        require(sep and mode <= 3 and bool(mode) == bool(name), 'invalid khXb record')
        if name:
            found[name, chunk[offset+64:offset+96]] = mode
    return found


def crc_table():
    table = []
    for value in range(256):
        c = value << 24
        for _ in range(255):  # runtime/autosave.c kh_save_crc
            c = ((c << 1) & 0xffffffff) ^ (0x04c11db7 if c >> 31 else 0)
        table.append(c)
    return table


def slots(png):
    card, chunk = card_chunks(png)
    tags = markers(chunk)
    require(len(card) == (DATA + COUNT * SIZE + 1023) // 1024 * 1024, 'unexpected card size')
    require(card[DATA:DATA+6] == b'KH2SJP', 'unrecognized encrypted first directory entry')
    table = crc_table()
    rows = {}
    for physical in range(1, COUNT):
        entry = 16 + physical * ROW
        name = card[entry:entry+64].split(b'\0', 1)[0]
        if not name:
            continue
        match = re.fullmatch(rb'BISLPM-66675FM-(\d\d)', name)
        require(match, f'unrecognized directory entry {physical}')
        number = 1 + int(match[1])
        require(number < COUNT and number not in rows, 'invalid or duplicate slot number')
        start = DATA + physical * SIZE
        payload = card[start:start+SIZE]
        require(struct.unpack_from('<I', card, entry+80)[0] == SIZE and
                payload[:8] == b'KH2J\x3a\0\0\0', f'invalid slot {number} header')
        crc = 0xffffffff
        for b in payload[:8] + payload[12:]:
            crc = ((crc << 8) & 0xffffffff) ^ table[(crc >> 24) ^ b]
        require((crc ^ 0xffffffff) == struct.unpack_from('<I', payload, 8)[0], f'invalid slot {number} CRC')
        world, room = payload[12:14]
        munny, ticks = struct.unpack_from('<II', payload, 0x2440)
        difficulty, = struct.unpack_from('<I', payload, 0x2498)
        require(world < len(WORLDS) and difficulty < 4, f'invalid slot {number} summary')
        seconds = ticks // 60
        time = f'{seconds//3600}:{seconds//60%60:02}:{seconds%60:02}'
        tag = tags.get((name, hashlib.sha256(payload).digest()), 0)
        mode = f'XKB {tag}' if tag and tag == difficulty else 'ordinary'
        rows[number] = (name.decode(), payload[0x24ff], f'{WORLDS[world]}{room:02}', time,
                        munny, f'{DIFFICULTY[difficulty]} ({difficulty})', mode)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    args = parser.parse_args()
    try:
        rows = slots(args.file.read_bytes())
    except (ValueError, OSError) as exc:
        sys.exit(f'save-slots: {exc}')
    print('Slot\tLogical name\tLevel\tRoom\tPlay time\tMunny\tDifficulty\tMode')
    for n in range(1, COUNT):
        print(n, *rows.get(n, ('empty',)), sep='\t')


if __name__ == '__main__':
    main()
