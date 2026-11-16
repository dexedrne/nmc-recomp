#!/usr/bin/env python3
"""Read-only check of our descriptor metadata against the shipped DLL constructor/exports.
Usage: python3 runtime/check_mod_syscalls.py /path/to/KH2Tweaks_Refactor.dll
Requires only Python's standard library and objdump; never loads or executes the DLL.
"""
import pathlib
import re
import struct
import subprocess
import sys


def check(dll):
    raw = pathlib.Path(dll).read_bytes()
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    nsec, optsize = struct.unpack_from('<H', raw, pe + 6)[0], struct.unpack_from('<H', raw, pe + 20)[0]
    sections = [struct.unpack_from('<IIII', raw, pe + 24 + optsize + 40 * i + 8) for i in range(nsec)]

    def off(rva):
        for size, va, rawsize, pos in sections:
            if va <= rva < va + rawsize:
                return pos + rva - va
        raise ValueError(f'RVA not file backed: {rva:x}')

    er = struct.unpack_from('<I', raw, pe + 24 + 112)[0]
    export = struct.unpack_from('<IIHHIIIIIII', raw, off(er))
    functions, names, ordinals = export[-3:]
    exported = {}
    for i in range(export[7]):
        nr = struct.unpack_from('<I', raw, off(names) + 4 * i)[0]
        ordinal = struct.unpack_from('<H', raw, off(ordinals) + 2 * i)[0]
        fn = struct.unpack_from('<I', raw, off(functions) + 4 * ordinal)[0]
        name = raw[off(nr):].split(b'\0', 1)[0].decode()
        exported.setdefault(fn, set()).add(name)

    dis = subprocess.check_output(['objdump', '-d', '--start-address=0x1800151d8',
                                   '--stop-address=0x180015ccd', dll], text=True)
    regs, memory = {}, {}
    for line in dis.splitlines():
        lea = re.search(r'lea\s+.*,%(rcx|r8)\s+# 0x([0-9a-f]+)', line)
        if lea:
            regs[lea[1]] = int(lea[2], 16) - 0x180000000
        if re.search(r'xor\s+%edx,%edx', line):
            regs['rdx'] = 0
        store = re.search(r'movq?\s+(\$0x[0-9a-f]+|%r(?:cx|dx|8)),(0x[0-9a-f]+)?\(%rax\)', line)
        if store and int(line.split(":", 1)[0], 16) >= 0x1800151ff:
            source, offset = store[1], int(store[2] or '0', 16)
            memory[offset] = int(source[1:], 16) if source.startswith('$') else regs[source[1:]]
    assert set(memory) == set(range(0, 114 * 16, 8)), 'constructor size/shape changed'
    source = pathlib.Path(__file__).with_name('mod_syscalls.c').read_text()
    entries = re.findall(r'\{"(\w+)", (0x[0-9a-f]+)u, (0x[0-9a-f]+)\}, /\* (\d+) \*/', source)
    assert len(entries) == 114
    for name, flags, handler, index in entries:
        i = int(index)
        assert memory[16 * i] == int(handler, 16), (i, 'handler mismatch')
        assert memory[16 * i + 8] == int(flags, 16), (i, 'flags mismatch')
        assert name in exported[int(handler, 16)], (i, 'export name mismatch')
    print('PASS: all 114 index/handler/flags/name records match static DLL constructor and exports')


if __name__ == '__main__':
    check(sys.argv[1])
