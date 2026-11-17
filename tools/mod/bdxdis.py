#!/usr/bin/env python3
"""bdxdis.py - disassemble KH2 BDX (AI/script) bytecode found in a game file, with operands and syscall names.

usage: bdxdis.py FILE [--descs BdxInstructionDescs.cs] [--traps BdxTraps.cs] [--mod-syscalls mod_syscalls.c]
                      [--around NAME] [--context N] [--entry SUBSTR]

Instruction and trap tables are read at run time from OpenKH's Bdxio model sources (Apache-2.0; pass the two .cs files) so nothing
from OpenKH is copied here. Table 3 (the NMC mod's 114 added syscalls) is named from our runtime/win64/mod_syscalls.c.
--around NAME prints only windows of N instructions (default 30) before each call of that syscall and a few after.
Output is for analysis of the owner's own game/mod data; never commit dumps.
"""
import argparse, os, re, struct, sys


def load_descs(path):
    out = []
    for m in re.finditer(r'Code = (0x[0-9A-Fa-f]+), CodeMask = (0x[0-9A-Fa-f]+), Name = "([^"]+)",(.*?)CodeSize = (\d+)(.*)', open(path).read()):
        code, mask, name, size, rest = int(m.group(1), 16), int(m.group(2), 16), m.group(3), int(m.group(5)), m.group(6)
        args = re.findall(r'Type = ArgType\.(\w+)', rest)
        out.append((code, mask, name, size, args))
    return out


def load_traps(path):
    t = {}
    for m in re.finditer(r'TableIndex = (\d+), TrapIndex = (\d+), Name = "([^"]+)", Flags = (0x[0-9A-Fa-f]+)', open(path).read()):
        t[(int(m.group(1)), int(m.group(2)))] = (m.group(3), int(m.group(4), 16))
    return t


def load_mod(path):
    t = {}
    if not path or not os.path.exists(path):
        return t
    for m in re.finditer(r'\{"([A-Za-z_0-9]+)",\s*(0x[0-9a-fA-F]+)u?,\s*0x[0-9a-fA-F]+\},\s*/\*\s*(\d+)\s*\*/', open(path).read()):
        t[(3, int(m.group(3)))] = (m.group(1), int(m.group(2), 16))
    return t


def bar_entries(b):
    if b[:4] != b'BAR\x01' or len(b) < 16:
        return []
    n = struct.unpack_from('<I', b, 4)[0]
    if n > 5000 or 16 + 16 * n > len(b):
        return []
    out = []
    for i in range(n):
        t, dup, nm, o, s = struct.unpack_from('<HH4sII', b, 16 + 16 * i)
        if o + s > len(b) or o < 16:
            continue
        out.append((t, nm.rstrip(b'\0').decode('latin1'), b[o:o + s]))
    return out


def walk(path, b, depth=0):
    if depth > 6:
        return
    for t, nm, d in bar_entries(b):
        lab = '%s/%s' % (path, nm)
        if t == 3:
            yield lab, d
        elif d[:4] == b'BAR\x01':
            yield from walk(lab, d, depth + 1)


def disasm(d, descs, traps):
    """linear disassembly from each trigger entry (recursive over jumps); returns {pc: text} and the trigger list"""
    name = d[:16].split(b'\0')[0].decode('latin1')
    trig = []
    p = 28
    while p + 8 <= len(d):
        k, a = struct.unpack_from('<ii', d, p)
        p += 8
        if k == 0 and a == 0:
            break
        trig.append((k, a))
    n = (len(d) - 16) // 2
    w = lambda pc: struct.unpack_from('<H', d, 16 + 2 * pc)[0]
    seen, out, work = set(), {}, [a for k, a in trig if 0 <= a < n]
    while work:
        pc = work.pop()
        while 0 <= pc < n and pc not in seen:
            code = w(pc)
            desc = next((x for x in descs if code & x[1] == x[0]), None)
            if desc is None:
                out[pc] = '?? %04x' % code
                break
            _, mask, nm, size, args = desc
            if pc + size > n:
                break
            seen.add(pc)
            txt = nm
            sub = (code & ~mask) & 0xFFFF
            if nm == 'syscall':
                tbl, idx = (code >> 6) & 0x3ff, w(pc + 1)
                tn = traps.get((tbl, idx))
                txt = 'syscall %d,%d  %s' % (tbl, idx, (tn[0] + ' (argc %d%s)' % (tn[1] & 0xffff, ', ret' if tn[1] & 0x40000000 else '')) if tn else '<unnamed>')
            elif nm == 'jal32':
                v = struct.unpack_from('<i', d, 16 + 2 * (pc + 1))[0]
                tgt = pc + size + v
                txt = 'jal32 -> %d' % tgt
                work.append(tgt)
            elif size == 3 and args and args[0] in ('Imm32',):
                v = struct.unpack_from('<i', d, 16 + 2 * (pc + 1))[0]
                txt = '%s %d (0x%x)' % (nm, v, v & 0xffffffff)
            elif size == 3 and args and args[0] == 'Float32':
                txt = '%s %g' % (nm, struct.unpack_from('<f', d, 16 + 2 * (pc + 1))[0])
            elif size == 3:
                txt = '%s %d,%d' % (nm, w(pc + 1), w(pc + 2))
            elif size == 2:
                v = struct.unpack_from('<h', d, 16 + 2 * (pc + 1))[0]
                if nm in ('b', 'beqz', 'bnez', 'jal'):
                    tgt = pc + size + v
                    txt = '%s -> %d' % (nm, tgt)
                    work.append(tgt)
                else:
                    txt = '%s %d' % (nm, v)
            if sub:
                txt += '   ;sub=0x%x' % sub
            out[pc] = txt
            if nm in ('b', 'ret', 'exit'):
                break
            pc += size
    return name, trig, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--descs', required=True)
    ap.add_argument('--traps', required=True)
    ap.add_argument('--mod-syscalls')
    ap.add_argument('--around')
    ap.add_argument('--context', type=int, default=30)
    ap.add_argument('--entry')
    a = ap.parse_args()
    descs = load_descs(a.descs)
    traps = load_traps(a.traps)
    traps.update(load_mod(a.mod_syscalls))
    b = open(a.file, 'rb').read()
    scripts = [(os.path.basename(a.file), b)] if a.file.lower().endswith('.bdx') else list(walk(os.path.basename(a.file), b))
    for lab, d in scripts:
        if a.entry and a.entry not in lab:
            continue
        if len(d) < 0x20:
            continue
        name, trig, ins = disasm(d, descs, traps)
        pcs = sorted(ins)
        if a.around:
            hits = [i for i, pc in enumerate(pcs) if ('  ' + a.around + ' ') in ins[pc]]
            if not hits:
                continue
            print('==== %s  (script "%s", %d instructions, triggers %s)' % (lab, name, len(pcs), trig[:12]))
            shown = set()
            for h in hits:
                lo, hi = max(0, h - a.context), min(len(pcs), h + 6)
                if lo in shown:
                    continue
                print('  ---- window ending at pc %d' % pcs[h])
                for i in range(lo, hi):
                    if i in shown:
                        continue
                    shown.add(i)
                    print('  %6d: %s' % (pcs[i], ins[pcs[i]]))
        else:
            print('==== %s  (script "%s", %d instructions, triggers %s)' % (lab, name, len(pcs), trig[:12]))
            for pc in pcs:
                print('  %6d: %s' % (pc, ins[pc]))


if __name__ == '__main__':
    main()
