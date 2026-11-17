#!/usr/bin/env python3
"""msgdump.py - the messages of a KH2 message file (msg/<lang>/*.bar) as plain text, one per line: id <TAB> text.

usage: msgdump.py FILE.bar [--grep REGEX] [--ids]

A message file is a BAR container whose entries of type 2 are message tables: entry count at +4, then 8-byte records
(message id word at +0, offset of the text from the table's start at +4).  A text is a byte string: 0 ends it, 1 a space,
2 a new line, 3..0x1f control codes with fixed argument counts, the rest characters (the table below is the one of
runtime/win64/route_objective.c, which was checked against the running game; it covers the English texts).
A new line is printed as ' / ', a colour change as '<<' ... '>>', a button icon as (icon 0xNN), an unknown byte as '?'.
Output is text of the owner's own game / mod data: for analysis only, never commit dumps.
"""
import argparse, re, struct, sys

CTL_ARGS = [0, 0, 0, 0, 1, 6, 1, 4, 3, 1, 1, 1, 1, 0, 1, 5, 0, 4, 2, 4, 2, 2, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1]
ROMAN = ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII', 'VIII', 'IX', 'X', 'XIII']
ONE = {0x2b: '+', 0x2c: '-', 0x54: '-', 0x55: '-', 0x66: '-', 0x67: '-', 0xcd: '-', 0x2d: 'x', 0x48: '!', 0x49: '?', 0x4a: '%',
       0x4b: '/', 0x4c: '*', 0x4d: ',', 0x50: ',', 0x4e: '.', 0x4f: '.', 0x51: ';', 0x52: ':', 0x53: '...', 0x56: '~', 0x57: "'",
       0xe9: "'", 0xea: "'", 0xec: "'", 0xee: "'", 0x5a: '(', 0x5b: ')', 0x5c: '"', 0x5d: '"', 0x5e: '"', 0x5f: '"', 0x60: '"',
       0x61: '"', 0xed: '"', 0x62: '[', 0x63: ']', 0x64: '<', 0x65: '>', 0x84: 'XI', 0x85: 'XII', 0x86: '&', 0x87: '#', 0xeb: '`'}


def decode(b, p):
    out, colour = [], False
    while p < len(b):
        c = b[p]; p += 1
        if c == 0:
            break
        if c in (1, 0x73):
            out.append(' ')
        elif c == 2:
            out.append(' / ')
        elif c < 0x20:
            n = CTL_ARGS[c]; a0 = b[p] if n and p < len(b) else 0; p += n
            if c == 7:
                out.append('<<'); colour = True
            elif c == 3:
                if colour: out.append('>>')
                colour = False
            elif c == 9:
                out.append('(icon 0x%02x)' % a0)
            elif c == 0x10:
                out.append(' // ')
        elif 0x21 <= c <= 0x2a:
            out.append(chr(ord('0') + c - 0x21))
        elif 0x2e <= c <= 0x47:
            out.append(chr(ord('A') + c - 0x2e))
        elif 0x90 <= c <= 0x99:
            out.append(chr(ord('0') + c - 0x90))
        elif 0x9a <= c <= 0xb3:
            out.append(chr(ord('a') + c - 0x9a))
        elif 0x74 <= c <= 0x7e:
            out.append(ROMAN[c - 0x74])
        else:
            out.append(ONE.get(c, '?'))
    if colour:
        out.append('>>')
    return re.sub(r' +', ' ', ''.join(out)).strip()


def tables(b):
    if b[:4] != b'BAR\x01':
        sys.exit('msgdump: not a BAR file')
    n = struct.unpack_from('<I', b, 4)[0]
    for i in range(n):
        t, dup, nm, o, s = struct.unpack_from('<HH4sII', b, 16 + 16 * i)
        if t == 2 and o + s <= len(b):
            yield nm.rstrip(b'\0').decode('latin1'), b[o:o + s]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file'); ap.add_argument('--grep'); ap.add_argument('--ids', action='store_true')
    a = ap.parse_args()
    rx = re.compile(a.grep, re.I) if a.grep else None
    b = open(a.file, 'rb').read()
    total = 0
    for name, t in tables(b):
        cnt = struct.unpack_from('<I', t, 4)[0]
        if cnt > 200000 or 8 + 8 * cnt > len(t):
            sys.exit('msgdump: table %s: bad entry count %d' % (name, cnt))
        for i in range(cnt):
            mid, off = struct.unpack_from('<II', t, 8 + 8 * i)
            mid &= 0xffff
            if off >= len(t):
                continue
            txt = decode(t, off)
            if rx and not rx.search(txt):
                continue
            total += 1
            print('0x%04x\t%s' % (mid, txt) if not a.ids else '0x%04x' % mid)
    sys.stderr.write('msgdump: %d message(s)\n' % total)


if __name__ == '__main__':
    main()
