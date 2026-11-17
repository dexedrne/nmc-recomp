#!/usr/bin/env python3
"""Static resolution of KH2Tweaks_Refactor.dll's signature-scanned pointer slots to exe RVAs (Task 23).

  mod_dll_slots.py --dll KH2Tweaks_Refactor.dll --exe kh2fm.exe [--expect FILE] [--json OUT] [--show] [SLOT ...]

The DLL reaches the engine through global pointer slots (DLL RVA 0x81xxx / 0x82xxx) that its CRT initialisers fill by scanning the
exe image for a byte pattern.  This tool reads those scanner functions from the DLL FILE (objdump listing + raw bytes) and scans the
exe FILE image; no DLL or exe code is executed, natively or emulated.  For each slot:

  1. the scanner = the function that stores a register into the slot (`mov QWORD PTR [rip+..],reg  # slot`);
  2. the pattern: either the inline form (`cmp BYTE PTR [cursor+k],imm8`, missing k = wildcard) or the table form (mask bytes
     with '?' = wildcard and pattern bytes in the DLL's .rdata, both named by the listing); its length is cross-checked against the
     scanner's own loop bound (`add end,-N`);
  3. the scan range is checked to be the whole exe image: the scanner's start pointer must come from GetModuleHandleW(NULL) and
     its end pointer from start + SizeOfImage (K32GetModuleInformation); the exe is then scanned with the DLL's rule (lowest
     address first, first match wins); the number of matches is reported;
  4. the tail (what is stored once the pattern matched) is recognised as one of a few NAMED templates and the stored value is
     computed by this tool's own formula for that template:
        direct(+K)            slot = match + K
        call(+K)              slot = target of the rel32 call/jmp at match+K          (movsxd [p+1]; +5; +p)
        call_checked(+K)      the same, the DLL first requires the opcode byte at match+K to be e8 or e9 (add al,0x18; cmp al,1)
     Anything else is reported as UNRESOLVED with the listing of the tail, for a human to read.
  5. derived data pointers (DERIVED and OFFSET below): a second DLL function computes `slot2 = [slot1] + K + 4 + rel32 at [slot1]+K`
     (the address a rip-relative instruction of the exe names).  Each entry names the DLL instruction that does it; the tool checks
     the four instructions at that address before using the formula.

--expect FILE: lines `slot exe_rva [hits=N] [# comment]` (hex): every listed slot must resolve to that RVA with exactly one pattern
match (or N: the DLL takes the lowest address, so a second match is legal but must be known), and no match window may contain a byte
that KH2Moose.dll rewrites before this DLL scans (AVOID); else the tool exits 1.  Output: one line per slot; --json writes the same as a list.
"""
import sys, re, json, struct, subprocess, argparse

DLL_BASE = 0x180000000

# slot2: (DLL address of the `mov REG,[slot1]`, slot1, K, REG) -- the instruction group is
#   mov REG,QWORD PTR [slot1] ; movsxd rax,DWORD PTR [REG+K] ; add REG,K+4 ; add REG,rax ; mov QWORD PTR [slot2],REG
# (other instructions may sit between them as long as they leave REG, and rax between the load and the add, alone)
DERIVED = {
    0x817a0: (0x18002cd69, 0x81a18, 0x10, 'rcx'),      # exe 0x717008  current world / room bytes
    0x81960: (0x18002cf9b, 0x82278, 0x160, 'rcx'),     # exe 0x749804  flag dword tested by the command sub menu
    0x817d0: (0x18002cf6a, 0x81aa0, 0xe, 'rcx'),       # exe 0x9babb0  name of the loaded command-menu layout file
    0x818b8: (0x18002cfed, 0x82a00, 0x15, 'rcx'),      # exe 0x2a0f688 primary common script data
    0x819a0: (0x180033958, 0x81e98, 0xba, 'rcx'),      # menu cursor (pointer to a pointer)
    0x819a8: (0x180033977, 0x81ea0, 0x1c9, 'rcx'),     # menu item-id list (pointer to a pointer)
    0x81a08: (0x180033e42, 0x829e8, 0x35, 'rdx'),      # exe 0x753490  script syscall table directory
    0x81898: (0x18002ce8f, 0x822b0, 0x1a2, 'rcx'),     # exe 0x5c9030  form growth table (GetGrowthTable)
    0x819f8: (0x180033dc1, 0x826d8, 0x2d, 'rcx'),      # exe 0x2ae5768 the global the DLL's worker thread waits for
    0x81880: (0x180034340, 0x81c60, 0xe, 'rcx>rax'),   # exe 0x718cb0  (worker thread 0x342a0; the sum is formed in rax)
}
# slot2 = [slot1] + delta, formed right behind slot1's store: (DLL address of the `add rax,imm`, slot1, delta)
OFFSET = {
    0x81870: (0x18003438e, 0x81880, -8),               # exe 0x718ca8  camera mode (SetCameraMode), 8 bytes in front of 0x81880
}

# exe bytes KH2Moose.dll rewrites (it is loaded before KH2Tweaks_Refactor.dll: Panacea loads dll\\*.dll in directory order), runtime/moose.c
AVOID = [(0x39d719, 7, 'moose cache lea'), (0x39dac1, 7, 'moose cache lea'), (0x3e5f90, 7, 'moose data lea'), (0x3e5f3d, 7, 'moose data lea'),
         (0x39e260, 14, 'moose block hook'), (0x39e270, 19, 'moose block hook'), (0x927e2, 5, 'moose scd_priority'), (0x92758, 16, 'moose scd_priority bit 2 site'),
         (0x2b46eb, 5, 'moose fast_title')]

RANGE_OK = {}; BY_TAG = {}; IMPORTS = {}

def die(msg):
    sys.stderr.write('mod_dll_slots: ERROR: %s\n' % msg); sys.exit(1)

class PE:
    def __init__(self, path):
        self.raw = open(path, 'rb').read(); r = self.raw
        pe = struct.unpack_from('<I', r, 0x3c)[0]
        if r[pe:pe + 4] != b'PE\0\0': die('%s is not a PE file' % path)
        nsec = struct.unpack_from('<H', r, pe + 6)[0]; optsz = struct.unpack_from('<H', r, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<Q', r, opt + 24)[0]; self.size_of_image = struct.unpack_from('<I', r, opt + 56)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = r[o:o + 8].rstrip(b'\0').decode('latin1'); vsz, va, rsz, rp = struct.unpack_from('<IIII', r, o + 8)
            self.secs.append((name, va, vsz, rp, rsz))
        img = bytearray(self.size_of_image)
        hdr = struct.unpack_from('<I', r, opt + 60)[0]; img[:hdr] = r[:hdr]
        for name, va, vsz, rp, rsz in self.secs:
            n = min(rsz, vsz) if vsz else rsz
            img[va:va + n] = r[rp:rp + n]
        self.img = bytes(img)
    def rd(self, rva, n): return self.img[rva:rva + n]
    def s32(self, rva): return struct.unpack_from('<i', self.img, rva)[0]

def listing(dll):
    out = subprocess.check_output(['objdump', '-d', '-Mintel', '--no-show-raw-insn', dll], text=True)
    ins = []
    for ln in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\t(.*)$', ln)
        if m: ins.append((int(m.group(1), 16), m.group(2).strip()))
    return ins

def find_function(ins, idx):
    """index range [lo, hi) of the function around instruction idx: bounded by int3 padding, or by a `ret` that is directly followed
    by a scanner preamble (two range loads and `add end,-N`: two scanners back to back without padding)"""
    ld = r'mov\s+r[a-z0-9]+,QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x'
    def edge(k):
        if ins[k - 1][1] == 'int3': return True
        return (ins[k - 1][1] == 'ret' and k + 2 < len(ins) and re.match(ld, ins[k][1]) and re.match(ld, ins[k + 1][1])
                and re.match(r'add\s+r[a-z0-9]+,0xffffffffffffff[0-9a-f]{2}$', ins[k + 2][1]) is not None)
    lo = idx
    while lo > 0 and not edge(lo): lo -= 1
    hi = idx + 1
    while hi < len(ins) and ins[hi][1] != 'int3' and not edge(hi): hi += 1
    return lo, hi

def imports(pe):
    """IAT rva -> imported name"""
    r = pe.raw; o = struct.unpack_from('<I', r, 0x3c)[0] + 24
    irva = struct.unpack_from('<I', r, o + 112 + 8)[0]; out = {}
    while True:
        oft, _, _, name, ft = struct.unpack_from('<IIIII', pe.img, irva)
        if not name: break
        k = 0
        while True:
            v = struct.unpack_from('<Q', pe.img, (oft or ft) + 8 * k)[0]
            if not v: break
            if not v >> 63: out[ft + 8 * k] = pe.img[v + 2:pe.img.index(b'\0', v + 2)].decode('latin1')
            k += 1
        irva += 20
    return out

def check_range(ins, by_tag, imps, start_slot):
    """the scan range of a scanner: start = GetModuleHandleW(NULL), end = start + SizeOfImage (K32GetModuleInformation); '' or an error text"""
    st = [i for i in by_tag.get(start_slot, []) if re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],rax\s', ins[i][1])]
    if len(st) != 1: return 'range start global 0x%x has %d stores' % (start_slot, len(st))
    i = st[0]; prev = [t for a, t in ins[max(0, i - 4):i]]
    m = None
    for t in prev:
        mm = re.match(r'call\s+QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)$', t)
        if mm: m = mm
    if not m or imps.get(int(m.group(1), 16) - DLL_BASE) != 'GetModuleHandleW' or not any(re.match(r'xor\s+ecx,ecx$', t) for t in prev):
        return 'range start global 0x%x is not GetModuleHandleW(NULL)' % start_slot
    nxt = ins[i:i + 16]; names = [imps.get(int(mm.group(1), 16) - DLL_BASE) for a, t in nxt for mm in [re.match(r'call\s+QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)$', t)] if mm]
    if 'K32GetModuleInformation' not in names or not any(re.match(r'add\s+rax,QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x%x$' % (DLL_BASE + start_slot), t) for a, t in nxt):
        return 'range end of 0x%x is not start + SizeOfImage from K32GetModuleInformation' % start_slot
    return ''

def scan(img, pat):
    """the DLL's rule: every candidate from the start, compare byte by byte; returns all matches (the DLL takes the first)"""
    n = len(pat); fixed = [(i, b) for i, b in enumerate(pat) if b is not None]
    if not fixed: die('pattern without a fixed byte')
    # anchor on the longest run of fixed bytes for speed, then verify
    best = (0, 0); i = 0
    while i < n:
        if pat[i] is None: i += 1; continue
        j = i
        while j < n and pat[j] is not None: j += 1
        if j - i > best[1]: best = (i, j - i)
        i = j
    a0, alen = best; anchor = bytes(pat[a0:a0 + alen]); hits = []; p = img.find(anchor)
    while p >= 0:
        s = p - a0
        if s >= 0 and s + n <= len(img) and all(img[s + k] == b for k, b in fixed): hits.append(s)
        p = img.find(anchor, p + 1)
    return hits

def resolve_scanner(ins, by_addr, dll, exe, slot):
    """-> dict(slot, scanner, pattern, hits, template, value)"""
    tag = '# 0x%x' % (DLL_BASE + slot)
    stores = [i for i, (a, t) in enumerate(ins) if t.endswith(tag) and re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],(r[a-z0-9]+)\s', t)]
    if not stores: return dict(slot=slot, error='no register store to the slot in the DLL')
    funcs = {find_function(ins, i) for i in stores}
    if len(funcs) != 1: return dict(slot=slot, error='%d functions store to the slot: %s' % (len(funcs), ['%x' % ins[lo][0] for lo, hi in funcs]))
    lo, hi = funcs.pop(); body = ins[lo:hi]; text = [t for a, t in body]
    R = dict(slot=slot, scanner=body[0][0] - DLL_BASE)
    loads = [int(m.group(1), 16) - DLL_BASE for t in text[:2] for m in [re.match(r'mov\s+r[a-z0-9]+,QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)$', t)] if m]
    if len(loads) != 2 or loads[0] != loads[1] + 8: return dict(R, error='scanner does not start with the two range loads (end, start)')
    key = ('range', loads[1])
    if key not in RANGE_OK: RANGE_OK[key] = check_range(ins, BY_TAG, IMPORTS, loads[1])
    if RANGE_OK[key]: return dict(R, error=RANGE_OK[key])
    R['range'] = '0x%x' % loads[1]
    # loop bound: add <end>,-N
    N = None
    for t in text[:6]:
        m = re.match(r'add\s+r[a-z0-9]+,0x(ffffffffffffff[0-9a-f]{2})$', t)
        if m: N = 0x10000000000000000 - int(m.group(1), 16)
    # pattern
    inline = {}
    cur = None
    for t in text:
        m = re.match(r'cmp\s+BYTE PTR \[(r[a-z0-9]+)(?:\+0x([0-9a-f]+))?\],0x([0-9a-f]+)$', t)
        if m:
            if cur is None: cur = m.group(1)
            if m.group(1) == cur: inline[int(m.group(2) or '0', 16)] = int(m.group(3), 16)
    table = [(int(m.group(1), 16)) for t in text for m in [re.match(r'cmp\s+BYTE PTR \[r[a-z0-9]+\+r[a-z0-9]+\*1\+0x([0-9a-f]+)\],0x3f$', t)] if m]
    ptab = [(int(m.group(1), 16)) for t in text for m in [re.match(r'cmp\s+BYTE PTR \[r[a-z0-9]+\+r[a-z0-9]+\*1\+0x([0-9a-f]+)\],[a-d]l$', t)] if m]
    if table and ptab:
        if N is None: return dict(R, error='table-form scanner without a loop bound')
        mrva, prva = min(table), min(ptab); mask = dll.rd(mrva, N); pb = dll.rd(prva, N)
        pat = [None if mask[i] == 0x3f else pb[i] for i in range(N)]
        R['form'] = 'table(mask 0x%x, bytes 0x%x)' % (mrva, prva)
        cursor = None
        for t in text[:4]:
            m = re.match(r'mov\s+(r[a-z0-9]+),QWORD PTR \[rip\+0x[0-9a-f]+\]', t)
            if m: cursor = m.group(1)          # the second of the two loads (start pointer) is the cursor
    elif inline:
        n = max(inline) + 1
        if N is not None and N != n: return dict(R, error='inline pattern length %d differs from the loop bound %d' % (n, N))
        pat = [inline.get(i) for i in range(n)]; R['form'] = 'inline'; cursor = cur
    else:
        return dict(R, error='pattern form not recognised')
    R['pattern'] = ' '.join('??' if b is None else '%02x' % b for b in pat)
    hits = scan(exe.img, pat); R['hits'] = len(hits)
    if not hits: return dict(R, error='pattern does not occur in the exe')
    M = hits[0]; R['match'] = M
    # tail: the instructions that produce the stored register.  Collect every `lea X,[cursor+K]` (pre-computed result pointers inside
    # the compare chain) and the instructions from the last conditional jump out of the loop to the store.
    st = [i for i, (a, t) in enumerate(body) if t.endswith(tag) and not re.search(r'\],0x0\s', t) and re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],(r[a-z0-9]+)\s', t)]
    cands = []
    for si in st:
        reg = re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],(r[a-z0-9]+)\s', body[si][1]).group(1)
        # walk back over the straight-line tail
        k = si; tail = []
        while k > 0:
            t = body[k - 1][1]
            if re.match(r'(ret|jmp|j[a-z]+)\b', t) and not re.match(r'(ja|jbe|je)\s', t): break
            if re.match(r'(inc|cmp\s+r[a-z0-9]+,r[a-z0-9]+)', t): break
            tail.insert(0, t); k -= 1
            if len(tail) > 12: break
        cands.append((reg, tail, body[si][0]))
    R['tail'] = [' ; '.join(tl) for reg, tl, a in cands]
    pre = {}
    for t in text:
        m = re.match(r'lea\s+(r[a-z0-9]+),\[%s\+0x([0-9a-f]+)\]$' % cursor, t)
        if m: pre[m.group(1)] = int(m.group(2), 16)
    val = None; tmpl = None
    for si in st:                                             # `ret ; inc CURSOR ; mov [slot],CURSOR`: the match address plus one
        reg = re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],(r[a-z0-9]+)\s', body[si][1]).group(1)
        if si >= 2 and reg == cursor and body[si - 1][1] == 'inc    %s' % cursor and body[si - 2][1] == 'ret':
            val = M + 1; tmpl = 'direct(+0x1)'; R['tail'] = ['inc %s' % cursor]
    for reg, tail, addr in cands:
        if val is not None: break
        j = ' ; '.join(tail)
        # zero store paths (xor eax,eax ; mov [slot],rax) are the failure path
        if re.search(r'xor\s+e(\w\w),e\1$', j) or j.endswith('xor    eax,eax'): continue
        # call_checked via a pointer register P = cursor+K
        m = re.search(r'movzx\s+eax,BYTE PTR \[(r[a-z0-9]+)(?:\+0x([0-9a-f]+))?\] ; add\s+al,0x18 ; cmp\s+al,0x1 ; (?:ja|jbe)\s+0x[0-9a-f]+', ' ; '.join(text))
        m2 = re.search(r'movsxd\s+rax,DWORD PTR \[(r[a-z0-9]+)\+0x([0-9a-f]+)\] ; (add|sub)\s+(r[a-z0-9]+),0x([0-9a-f]+) ; add\s+(r[a-z0-9]+),(r[a-z0-9]+)$', j)
        if m2:
            preg, d1, r1, c5, r2, r3 = m2.group(1), int(m2.group(2), 16), m2.group(4), int(m2.group(5), 16), m2.group(6), m2.group(7)
            if m2.group(3) == 'sub': c5 = 0x10000000000000000 - c5          # `sub rax,-0x80` is how the compiler writes +0x80
            if preg == cursor: K = d1 - 1; ok = (c5 == d1 + 4)
            elif preg in pre: K = pre[preg] + d1 - 1; ok = (c5 == 5 and d1 == 1)
            else: K = None; ok = False
            if ok and {r1, r2, r3} <= {'rax', preg} and reg in (r1, r2):
                op = exe.img[M + K]
                checked = bool(m)
                if checked and op not in (0xe8, 0xe9): return dict(R, error='call_checked(+0x%x): opcode byte %02x is not e8/e9 (the DLL would store 0)' % (K, op))
                val = M + K + 5 + exe.s32(M + K + 1); tmpl = '%s(+0x%x)' % ('call_checked' if checked else 'call', K)
                R['opcode'] = '%02x' % op
                break
        if j == '' or re.fullmatch(r'(lea\s+r[a-z0-9]+,\[r[a-z0-9]+\+0x[0-9a-f]+\] ; )?(je|test\s+\w+,\w+ ; je)?.*', j):
            if reg == cursor: val = M; tmpl = 'direct(+0)'; break
            if reg in pre and not m2: val = M + pre[reg]; tmpl = 'direct(+0x%x)' % pre[reg]; break
    if val is None: return dict(R, error='tail not recognised')
    R['template'] = tmpl; R['value'] = val
    return R

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', required=True); ap.add_argument('--exe', required=True); ap.add_argument('--expect'); ap.add_argument('--json'); ap.add_argument('--show', action='store_true')
    ap.add_argument('slots', nargs='*')
    a = ap.parse_args()
    dll = PE(a.dll); exe = PE(a.exe); ins = listing(a.dll); by_addr = {x[0]: i for i, x in enumerate(ins)}
    IMPORTS.update(imports(dll))
    for i, (ad, t) in enumerate(ins):
        m = re.search(r'# 0x([0-9a-f]+)$', t)
        if m: BY_TAG.setdefault(int(m.group(1), 16) - DLL_BASE, []).append(i)
    expect = {}; exp_hits = {}
    if a.expect:
        for ln in open(a.expect):
            ln = ln.split('#')[0].split()
            if len(ln) >= 2:
                expect[int(ln[0], 16)] = int(ln[1], 16)
                for w in ln[2:]:
                    if w.startswith('hits='): exp_hits[int(ln[0], 16)] = int(w[5:])
    slots = [int(s, 16) for s in a.slots] or sorted(expect)
    cache = {}
    def res(slot):
        if slot in cache: return cache[slot]
        if slot in DERIVED:
            addr, s1, K, rg = DERIVED[slot]; i = by_addr.get(addr)
            inrax = rg.endswith('>rax'); rg = rg.split('>')[0]; res_reg = 'rax' if inrax else rg
            want = [r'mov\s+%s,QWORD PTR \[rip\+0x[0-9a-f]+\]\s+# 0x%x$' % (rg, DLL_BASE + s1), r'movsxd\s+rax,DWORD PTR \[%s\+0x%x\]$' % (rg, K),
                    r'add\s+%s,0x%x$' % (rg, K + 4), (r'add\s+rax,%s$' % rg) if inrax else (r'add\s+%s,rax$' % rg),
                    r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],%s\s+# 0x%x$' % (res_reg, DLL_BASE + slot)]
            # the five instructions in this order within a short window; anything between them must not write the pointer register,
            # and nothing may write rax between the movsxd and the add
            ok = i is not None; k = i; step = 0
            while ok and step < 5:
                if k - i > 20: ok = False; break
                t = ins[k][1]
                if re.match(want[step], t): step += 1
                elif re.match(r'call\b', t): ok = False
                elif re.match(r'(mov|lea|add|sub|movsxd|movzx|xor|and|or)\s+%s\b' % rg, t) and not (inrax and step == 4): ok = False   # (rax form: the pointer register is free once the sum is in rax)
                elif step in ((2, 3, 4) if inrax else (2, 3)) and re.match(r'(mov|lea|add|sub|movsxd|movzx|xor)\s+[re]ax\b', t): ok = False
                k += 1
            if not ok:
                r = dict(slot=slot, error='DERIVED entry does not match the listing at 0x%x' % addr)
            else:
                b = res(s1)
                if 'value' not in b: r = dict(slot=slot, error='base slot 0x%x: %s' % (s1, b.get('error')))
                else:
                    p = b['value']; r = dict(slot=slot, scanner=addr - DLL_BASE, template='rip_operand(slot 0x%x = 0x%x, +0x%x)' % (s1, p, K), hits=b['hits'],
                                             value=p + K + 4 + exe.s32(p + K), pattern=b['pattern'], form='derived')
        elif slot in OFFSET:
            addr, s1, delta = OFFSET[slot]; i = by_addr.get(addr)
            imm = (delta + 0x10000000000000000) & 0xffffffffffffffff
            ok = (i is not None and re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],rax\s+# 0x%x$' % (DLL_BASE + s1), ins[i - 1][1]) and re.match(r'add\s+rax,0x%x$' % imm, ins[i][1])
                  and re.match(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],rax\s+# 0x%x$' % (DLL_BASE + slot), ins[i + 1][1]))
            b = res(s1)
            if not ok: r = dict(slot=slot, error='OFFSET entry does not match the listing at 0x%x' % addr)
            elif 'value' not in b: r = dict(slot=slot, error='base slot 0x%x: %s' % (s1, b.get('error')))
            else: r = dict(slot=slot, scanner=addr - DLL_BASE, template='offset(slot 0x%x = 0x%x, %+d)' % (s1, b['value'], delta), hits=b['hits'], value=b['value'] + delta, pattern=b['pattern'], form='derived')
        else: r = resolve_scanner(ins, by_addr, dll, exe, slot)
        cache[slot] = r; return r
    bad = 0; out = []
    for s in slots:
        r = res(s); out.append(r); e = expect.get(s)
        if 'value' in r:
            eh = exp_hits.get(s, 1)
            okx = '' if e is None else (' == expected' if e == r['value'] and r['hits'] == eh else ' != EXPECTED 0x%x with %d match(es)' % (e, eh))
            if e is not None and (e != r['value'] or r['hits'] != eh): bad += 1
            for lo, n, what in AVOID:                               # bytes KH2Moose.dll (loaded before this DLL) rewrites: no match window may contain one
                if 'match' in r and r['match'] < lo + n and lo < r['match'] + len(r['pattern'].split()):
                    bad += 1; okx += '  PATTERN WINDOW OVERLAPS %s' % what
            print('slot 0x%x -> exe 0x%x  %s  scanner 0x%x  %s  hits %d%s' % (s, r['value'], r['template'], r['scanner'], r.get('form', ''), r['hits'], okx))
        else:
            bad += 1
            print('slot 0x%x UNRESOLVED: %s%s' % (s, r.get('error'), '' if e is None else '  (expected 0x%x)' % e))
        if a.show or 'value' not in r:
            if 'pattern' in r: print('    pattern: %s' % r['pattern'])
            for t in r.get('tail', []): print('    tail: %s' % t)
    if a.json:
        json.dump([{k: (('0x%x' % v) if k in ('slot', 'scanner', 'value', 'match') else v) for k, v in r.items()} for r in out], open(a.json, 'w'), indent=1)
    if bad: die('%d of %d slots unresolved or different from --expect' % (bad, len(slots)))
    print('mod_dll_slots: %d slots resolved%s' % (len(slots), ', all equal to --expect with exactly one pattern match' if expect else ''))

if __name__ == '__main__':
    main()
