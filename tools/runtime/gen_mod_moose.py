#!/usr/bin/env python3
"""Task 23: decode KH2Moose.dll's two ini-switched CODE patches statically and generate what the port needs.

  gen_mod_moose.py --dll KH2Moose.dll --exe kh2fm.exe --universe DIR --edits edit_list.json
                   --out-json gen/mod_moose_edits.json --out-h gen/mod_moose_table.h

Nothing of the DLL is executed (not natively, not emulated): the file is read as bytes.  Standard library only.

What the DLL does (init 0x180001da0 calls cache, data, blocks, 0x180003050 `fixes`, then 0x180001df0 `hacks`):

  [kh2.fixes] scd_priority   0x180003050.  A missing key means 1; value 0 skips everything.  Bit 1 ("Voice"): the 13-byte pattern
                             at DLL 0x44038 (value/mask pairs) must match the exe exactly once (a second search from match+1 must
                             fail, else the DLL throws its "Pattern not found." error); the 5 bytes at the match (a call) are
                             re-encoded from the request {mnemonic 0x6eb, 2 operands, register 0x25, register 0x25}: XOR EAX,EAX,
                             the rest of the 5 bytes filled with NOPs.  Bit 2 ("Sound effects") is a second patch (DLL 0x44058,
                             mnemonic 0x31b): not set in the shipped ini, not generated here.
  [kh2.hacks] fast_title     0x180001fba.  A missing key means OFF; non-zero: pattern at DLL 0x440a0 (5 bytes), unique match, the
                             5 bytes re-encoded from {mnemonic 0x1b4, 2 operands, register 0x2c, immediate 0x11}: MOV EDI,0x11.

Both go through DLL 0x18000b640(address, length, request): VirtualProtect, the encoder (0x180020b80: writes the instruction at the
address, returns its length), then 0x180020fa0 fills the remaining bytes from the DLL's own NOP table (0x1800495a0, nine-byte rows:
90 / 66 90 / 0f 1f 00 / ...).

How the bytes are obtained.  Every instruction that sets a request field is matched against a template at its DLL address and the
immediates are READ FROM THE DLL (a different build fails here).  Register ids are the encoder library's (0x25 = EAX, 0x2c = EDI:
the 32-bit registers follow 20 8-bit and 16 16-bit ones); the same numbering yields RAX/RBX/RDI/R14 for the four pool patches that
runtime/moose.c already implements, with mnemonic 0x1b4 = MOV there too.  The encoder library's tables are NOT decoded, so two things
are derived, not read:
  * `mov edi,0x11` in 5 bytes is `bf 11 00 00 00` (the only 5-byte encoding of a 32-bit MOV of an immediate into EDI);
  * `xor eax,eax` is written as `31 c0` (it could be `33 c0`: the same instruction, the same length, the same lifted code).
The NOP bytes behind it ARE read from the DLL's table.

Outputs (mod-derived: gen/ only, never committed):
  --out-json  the byte edits for tools/mod_static_lift.py / mod_static_difftest.py (`--extra-edits`, table "moose")
  --out-h     the same for runtime/moose.c (ini section / key / bit / default, old and new bytes)
"""
import sys, os, json, struct, hashlib, argparse, bisect
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_mod_static import PE, load_universe

DLL_SHA256 = '6ef0785bf31ac2ff65242a866faa1dd8cd1573a1211446fd6fb9b5deaa66df69'

def die(msg):
    sys.stderr.write('gen_mod_moose: ERROR: %s\n' % msg); sys.exit(1)

def mapped(path):
    pe = PE(path); raw = pe.raw
    soh = struct.unpack_from('<I', raw, struct.unpack_from('<I', raw, 0x3c)[0] + 24 + 60)[0]
    m = bytearray(pe.size_of_image); m[:soh] = raw[:soh]
    for name, va, vsz, off, rsz, ch in pe.secs:
        n = min(vsz, rsz); m[va:va + n] = raw[off:off + n]
    return pe, bytes(m)

class Dll:
    def __init__(self, path):
        self.pe, self.mem = mapped(path); self.sha = hashlib.sha256(self.pe.raw).hexdigest()
    def match(self, rva, tmpl):
        """tmpl: hex bytes, `??` = captured byte.  -> the captured bytes; dies when a fixed byte differs."""
        got = bytearray()
        for i, t in enumerate(tmpl.split()):
            b = self.mem[rva + i]
            if t == '??': got.append(b)
            elif int(t, 16) != b: die('DLL 0x%x+%d: byte %02x, expected %s (template "%s"): not the analysed build' % (rva, i, b, t, tmpl))
        return bytes(got)
    def rip(self, rva, tmpl, ilen):
        """a rip-relative operand: template with four `??` for the disp32; -> the RVA it names"""
        d = struct.unpack('<i', self.match(rva, tmpl))[0]; return rva + ilen + d
    def wstr(self, rva):
        out = ''
        while True:
            c = struct.unpack_from('<H', self.mem, rva)[0]
            if not c: return out
            out += chr(c); rva += 2

def scan_pairs(img, pairs):
    """the DLL's pattern form: (value, mask) byte pairs, mask 0 = any byte; all matches"""
    pat = [(pairs[i], pairs[i + 1]) for i in range(0, len(pairs), 2)]
    if any(m not in (0, 0xff) for v, m in pat) or pat[0][1] != 0xff: die('pattern with a partial mask')
    hits = []; p = img.find(bytes([pat[0][0]]))
    while p >= 0:
        if p + len(pat) <= len(img) and all(m == 0 or img[p + k] == v for k, (v, m) in enumerate(pat)): hits.append(p)
        p = img.find(bytes([pat[0][0]]), p + 1)
    return hits, ' '.join('%02x' % v if m else '??' for v, m in pat)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', required=True); ap.add_argument('--exe', required=True); ap.add_argument('--universe', required=True); ap.add_argument('--edits', required=True)
    ap.add_argument('--out-json', required=True); ap.add_argument('--out-h', required=True)
    a = ap.parse_args()
    D = Dll(a.dll); m = D.match
    if D.sha != DLL_SHA256: die('KH2Moose.dll sha256 %s is not the analysed build %s' % (D.sha, DLL_SHA256))
    E = json.load(open(a.edits)); epe, exe = mapped(a.exe)
    if hashlib.sha256(epe.raw).hexdigest() != E['exe_sha256']: die('exe is not the pinned build')
    # ---- the driver: fixes (0x3050) before hacks (0x1df0, a tail jump)
    if 0x1dd1 + 5 + struct.unpack('<i', m(0x1dd1, 'e8 ?? ?? ?? ??'))[0] != 0x3050: die('driver 0x1da0 does not call 0x3050')
    if 0x1de6 + 5 + struct.unpack('<i', m(0x1de6, 'e9 ?? ?? ?? ??'))[0] != 0x1df0: die('driver 0x1da0 does not end in jmp 0x1df0')
    # ---- the NOP table of 0x20fa0 (lea r14,[rip+..]; rows of 9 bytes, row n-1 holds an n-byte NOP)
    nop_tab = D.rip(0x20fde, '4c 8d 35 ?? ?? ?? ??', 7)
    m(0x21000, '48 8d 53 ff 48 8d 14 d2 49 03 d6')                               # rdx = table + (n-1)*9
    nop3 = D.mem[nop_tab + 2 * 9:nop_tab + 2 * 9 + 3]
    if D.mem[nop_tab:nop_tab + 1] != b'\x90' or nop3 != bytes.fromhex('0f1f00'): die('NOP table at DLL 0x%x is not the expected one' % nop_tab)
    # ---- b640: encode at the address, then NOP-fill what is left
    if 0xb683 + 5 + struct.unpack('<i', m(0xb683, 'e8 ?? ?? ?? ??'))[0] != 0x20b80: die('0xb640 does not call the encoder 0x20b80')
    if 0xb69d + 5 + struct.unpack('<i', m(0xb69d, 'e8 ?? ?? ?? ??'))[0] != 0x20fa0: die('0xb640 does not call the NOP fill 0x20fa0')
    patches = []
    # ---- [kh2.fixes] scd_priority, bit 1
    key = D.wstr(D.rip(0x3086, '4c 8d 0d ?? ?? ?? ??', 7)); sec = D.wstr(D.rip(0x308f, '4c 8d 05 ?? ?? ?? ??', 7))
    if (key, sec) != ('scd_priority', 'fixes'): die('0x3050 reads %r / %r' % (sec, key))
    m(0x30be, '80 78 08 00 74 0e 48 8b 18 48 85 db 0f 84 ?? ?? ?? ?? eb 05 bb 01 00 00 00')     # found? value : 1 ; value 0 -> out
    m(0x3102, 'f6 c3 01 0f 84 ?? ?? ?? ??')                                                     # test bl,1
    p0 = D.rip(0x310b, '0f 10 05 ?? ?? ?? ??', 7); p1 = D.rip(0x312e, 'f2 0f 10 05 ?? ?? ?? ??', 8); p2 = D.rip(0x3112, '0f b7 05 ?? ?? ?? ??', 7)
    if (p1, p2) != (p0 + 16, p0 + 24): die('scd pattern is not one 26-byte block')
    hits, pat = scan_pairs(exe, D.mem[p0:p0 + 26])
    mn = struct.unpack('<I', m(0x31b4, 'c7 45 a8 ?? ?? ?? ??'))[0]; nops = m(0x31c7, 'c6 45 c8 ??')[0]
    t0 = struct.unpack('<I', m(0x31cb, 'c7 45 d0 ?? ?? ?? ??'))[0]; r0 = struct.unpack('<I', m(0x31d2, 'c7 45 d4 ?? ?? ?? ??'))[0]
    t1 = struct.unpack('<I', m(0x31f7, 'c7 45 10 ?? ?? ?? ??'))[0]; r1 = struct.unpack('<I', m(0x31fe, 'c7 45 14 ?? ?? ?? ??'))[0]
    ln = struct.unpack('<I', m(0x322f, 'ba ?? ?? ?? ??'))[0]
    if (mn, nops, t0, r0, t1, r1, ln) != (0x6eb, 2, 1, 0x25, 1, 0x25, 5): die('scd_priority request is not {0x6eb, reg 0x25, reg 0x25}, length 5')
    if len(hits) != 1: die('scd_priority pattern has %d matches in the exe (the DLL needs exactly one)' % len(hits))
    patches.append(dict(section='kh2.fixes', key='scd_priority', bit=1, default=1, rva=hits[0], pattern=pat, new=bytes.fromhex('31c0') + nop3,
                        what='call 0x1400ae1e0 (voice priority lookup) -> xor eax,eax + 3-byte NOP', request='mnemonic 0x6eb, registers 0x25, 0x25, length 5'))
    # ---- [kh2.hacks] fast_title
    key = D.wstr(D.rip(0x1fbf, '4c 8d 0d ?? ?? ?? ??', 7)); sec = D.wstr(D.rip(0x1fcb, '4c 8d 05 ?? ?? ?? ??', 7))
    if (key, sec) != ('fast_title', 'hacks'): die('0x1fba reads %r / %r' % (sec, key))
    m(0x1feb, '44 38 60 08 0f 84 ?? ?? ?? ?? 4c 39 20 0f 84 ?? ?? ?? ??')                       # not found -> skip ; value 0 -> skip (r12 = 0)
    q0 = D.rip(0x2012, 'f2 0f 10 05 ?? ?? ?? ??', 8); q1 = D.rip(0x1ffe, '0f b7 05 ?? ?? ?? ??', 7)
    if q1 != q0 + 8: die('fast_title pattern is not one 10-byte block')
    hits, pat = scan_pairs(exe, D.mem[q0:q0 + 10])
    mn = struct.unpack('<I', m(0x209b, 'c7 45 98 ?? ?? ?? ??'))[0]; nops = m(0x20ae, 'c6 45 b8 ??')[0]
    t0 = struct.unpack('<I', m(0x20b2, 'c7 45 c0 ?? ?? ?? ??'))[0]; r0 = struct.unpack('<I', m(0x20b9, 'c7 45 c4 ?? ?? ?? ??'))[0]
    t1 = struct.unpack('<I', m(0x20de, '48 c7 45 00 ?? ?? ?? ??'))[0]; imm = struct.unpack('<I', m(0x20fc, '48 c7 45 38 ?? ?? ?? ??'))[0]
    ln = struct.unpack('<I', m(0x2114, 'ba ?? ?? ?? ??'))[0]
    if (mn, nops, t0, r0, t1, ln) != (0x1b4, 2, 1, 0x2c, 4, 5): die('fast_title request is not {0x1b4, reg 0x2c, immediate}, length 5')
    if len(hits) != 1: die('fast_title pattern has %d matches in the exe (the DLL needs exactly one)' % len(hits))
    patches.append(dict(section='kh2.hacks', key='fast_title', bit=0, default=0, rva=hits[0], pattern=pat, new=b'\xbf' + struct.pack('<I', imm),
                        what='lea edi,[r12-0xc] -> mov edi,0x%x' % imm, request='mnemonic 0x1b4, register 0x2c, immediate 0x%x, length 5' % imm))
    # ---- owners, old bytes, sanity
    fs = load_universe(a.universe); los = [f[0] for f in fs]
    main_edits = [(int(e['rva'], 16), len(bytes.fromhex(e['old']))) for e in E['edits'] if e['cls'] != 'runtime_only_bss'] + \
                 [(int(r['rva'], 16), len(bytes.fromhex(r['old']))) for r in E.get('detour_hook_regions', [])]
    for p in patches:
        i = bisect.bisect_right(los, p['rva']) - 1; lo, hi, start = fs[i]
        if not (lo <= p['rva'] and p['rva'] + 5 <= hi): die('patch 0x%x is not inside one universe function' % p['rva'])
        p['func'] = start; p['old'] = exe[p['rva']:p['rva'] + 5]
        if len(p['new']) != 5: die('patch 0x%x: new bytes are not 5' % p['rva'])
        for r, n in main_edits:
            if r < hi and lo < r + n: die('patch 0x%x: function 0x%x is also changed by the main mod DLL (edit 0x%x)' % (p['rva'], start, r))
    if patches[0]['old'][:1] != b'\xe8' or patches[1]['old'] != bytes.fromhex('418d7c24f4'): die('exe bytes at the patch sites are not the expected instructions')
    os.makedirs(os.path.dirname(os.path.abspath(a.out_json)), exist_ok=True)
    J = dict(dll_sha256=D.sha, exe_sha256=E['exe_sha256'], image_base='140000000', table='moose', dll_name='KH2Moose.dll',
             note='Task 23, runtime/moose.c: two ini-switched code patches; the runtime hook of each function runs the re-lifted body only when the ini turns the patch on',
             nop_table_rva='%x' % nop_tab,
             edits=[dict(rva='%x' % p['rva'], old=p['old'].hex(), new=p['new'].hex(), func='%x' % p['func'], pad=0, section=p['section'], key=p['key'], bit=p['bit'],
                         default=p['default'], pattern=p['pattern'], request=p['request'], what=p['what']) for p in patches],
             caves=[], functions=['%x' % p['func'] for p in patches])
    json.dump(J, open(a.out_json, 'w'), indent=1)
    h = ['/* GENERATED by tools/gen_mod_moose.py from KH2Moose.dll (sha256 %s...): the two ini-switched code patches.  Mod-derived: never commit. */' % D.sha[:16],
         '#define KH_MOOSE_NPATCH %d' % len(patches),
         '/* section / key of KH2Moose.ini; bit != 0: the patch is on when (value & bit), else when value != 0; dflt = the value the DLL assumes for a missing key */',
         'static const struct kh_moose_patch { const char *section, *key; unsigned bit; long long dflt; unsigned rva, func; unsigned char old_[5], new_[5]; const char *what; } kh_moose_patches[KH_MOOSE_NPATCH] = {']
    for p in patches:
        h.append('  {"%s", "%s", %d, %d, 0x%x, 0x%x, {%s}, {%s}, "%s"},' % (p['section'], p['key'], p['bit'], p['default'], p['rva'], p['func'],
                 ','.join('0x%02x' % b for b in p['old']), ','.join('0x%02x' % b for b in p['new']), p['what']))
    h.append('};')
    open(a.out_h, 'w').write('\n'.join(h) + '\n')
    print('gen_mod_moose: %s' % '; '.join('[%s] %s%s: exe 0x%x in f_%x: %s -> %s (%s)' % (p['section'], p['key'], ' bit %d' % p['bit'] if p['bit'] else '', p['rva'], p['func'],
          p['old'].hex(' '), p['new'].hex(' '), p['what']) for p in patches))

if __name__ == '__main__':
    main()
