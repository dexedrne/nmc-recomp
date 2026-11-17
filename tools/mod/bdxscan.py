#!/usr/bin/env python3
"""Scan KH2 BDX (AI / script) bytecode for `syscall` instructions, following ALL code (Task 23).

  bdxscan.py [--json OUT.json] [--mod-syscalls runtime/mod_syscalls.c] [--table N] [--detail] PATH [PATH...]   (files or directories)

Successor of tools/mod/bdxscan.py in the repo (same instruction table, same container walk).  That scanner walked the code from the
trigger entry points over jumps and gosubs only.  What it missed, and why: these scripts pass FUNCTION ADDRESSES to syscalls as plain
32-bit immediates.  The compiler emits `push 0 ; push <absolute word address> ; push 0 ; syscall 1,3` (and similar) for a callback /
coroutine start; the function behind the address is entered by the engine, never by a branch of the script itself, so a control-flow
walk from the triggers never sees it.  In Roxas' controller script (obj/P_EX020.mdlx, `dona`) 22,426 of 48,165 code words were
unreached that way, among them the calls of the mod's syscalls SetCameraMode (55) and PartyJump (93).

This scanner works in three passes and labels every syscall with how its code was found:

  entry     reached from a trigger entry point over jumps / gosubs (what the old scanner did);
  callback  reached from an address immediate: a `push imm32` in reached code whose value is the word address of the first unreached
            word behind reached code or behind a `ret` (a function start), decoding cleanly from there.  Repeated to a fixed point,
            so callbacks started by callbacks are found too;
  sweep     everything still unreached, decoded linearly from each gap start, re-synchronising behind every `ret` and skipping what
            does not decode (the string / data area at the end of a script is not code): code nothing in the script refers to.  A
            `sweep` syscall is in the file, but the scanner found no way for it to run (dead code, or a reference of a kind this
            scanner does not know); it is reported so that nothing is missed, with that caveat.
  raw       not a class of use: words that no pass decoded (the string / data area of a script) are searched for the bare encoding of
            a table-3 syscall (`00ca`, trap < 114); such hits are listed apart, so that a syscall cannot hide behind a decode failure.

Output: per (table, trap) the number of uses by class and the scripts that use it; with --mod-syscalls the names and implemented /
named-fatal state of table 3 are read from the runtime source (`case N:` labels of the handler switch).  Reads files only.
"""
import struct, sys, os, json, re, argparse, collections, fnmatch
sys.dont_write_bytecode = True

# (code, mask, size, kind) kind: j = unconditional jump, jc = conditional jump, g = gosub16, g32 = gosub32, s = syscall,
#                                r = ret / exit, p = push imm32, n = other.  Table as in tools/mod/bdxscan.py (re-derived there from the
#                                OpenKH Bdxio model and cross-checked against the exe VM 0x41b400: opcode = code & 0xF, table = code >> 6).
DESC = []
def add(code, mask, size, kind='n'): DESC.append((code, mask, size, kind))
add(0x0000, 0x003F, 3, 'p'); add(0x0010, 0x003F, 3)
for c in (0x20, 0x60, 0xA0, 0xE0, 0x30, 0x70, 0xB0, 0xF0): add(c, 0xFFFF, 2)
for c in (0x01, 0x41, 0x81, 0xC1): add(c, 0xFFCF, 2)
for c in (0x02, 0x42, 0x82, 0xC2): add(c, 0xFFCF, 3)
add(0x0003, 0x000F, 2); add(0x0004, 0x000F, 1)
for c in (0x5, 0x85, 0xC5, 0x105, 0x145, 0x185, 0x1C5, 0x205, 0x245, 0x285, 0x2C5, 0x55, 0x95, 0x155, 0x195, 0x1D5, 0x215, 0x255, 0x295, 0x2D5,
          0x6, 0x46, 0x86, 0xC6, 0x106, 0x146, 0x186, 0x1C6, 0x206, 0x246, 0x286, 0x2C6, 0x16, 0x56, 0x96, 0xD6, 0x116): add(c, 0xFFFF, 1)
add(0x0007, 0xFFCF, 2, 'j'); add(0x0047, 0xFFCF, 2, 'jc'); add(0x0087, 0xFFCF, 2, 'jc')
add(0x0008, 0x000F, 2, 'g')
add(0x0009, 0xFFCF, 1); add(0x0049, 0xFFCF, 1, 'r'); add(0x0089, 0xFFCF, 1, 'r')
for c in (0xC9, 0x149, 0x189, 0x1C9, 0x209, 0x249): add(c, 0xFFCF, 1)
add(0x000A, 0x000F, 2, 's'); add(0x000B, 0x000F, 3, 'g32')

def find(code):
    for c, m, sz, k in DESC:
        if code & m == c: return sz, k
    return None

class Script:
    def __init__(self, d):
        self.d = d; self.n = (len(d) - 16) // 2
        self.ins = {}            # pc -> (size, kind, code, class)
        self.cov = bytearray(self.n)
    def w(self, i): return struct.unpack_from('<H', self.d, 16 + 2 * i)[0]
    def s16(self, i): return struct.unpack_from('<h', self.d, 16 + 2 * i)[0]
    def s32(self, i): return struct.unpack_from('<i', self.d, 16 + 2 * i)[0]
    def walk(self, entries, cls):
        """recursive descent; returns the number of instructions added"""
        work = [a for a in entries if 0 <= a < self.n]; added = 0
        while work:
            pc = work.pop()
            while 0 <= pc < self.n and pc not in self.ins and not self.cov[pc]:
                code = self.w(pc); r = find(code)
                if r is None: break
                sz, kind = r
                if pc + sz > self.n or any(self.cov[pc + k] for k in range(sz)): break
                self.ins[pc] = (sz, kind, code, cls); added += 1
                for k in range(sz): self.cov[pc + k] = 1
                if kind in ('j', 'jc', 'g'): work.append(pc + sz + self.s16(pc + 1))
                elif kind == 'g32': work.append(pc + sz + self.s32(pc + 1))
                if kind in ('j', 'r'): break
                pc += sz
        return added
    def clean_function(self, pc):
        """does the code at pc decode, without touching reached words, up to a ret / unconditional jump?"""
        k = 0
        while 0 <= pc < self.n and k < 100000:
            if self.cov[pc]: return pc in self.ins          # ran into reached code at an instruction boundary: fine
            r = find(self.w(pc))
            if r is None: return False
            sz, kind = r
            if pc + sz > self.n: return False
            if kind in ('r', 'j'): return True
            pc += sz; k += 1
        return False
    def callbacks(self):
        """address immediates of reached pushes that name a function start in unreached code"""
        out = set()
        for pc, (sz, kind, code, cls) in self.ins.items():
            if kind != 'p': continue
            v = self.s32(pc + 1)
            if not (self.code0 <= v < self.n) or self.cov[v]: continue
            before = v - 1
            starts = v == self.code0 or self.cov[before] == 1                      # first unreached word behind reached code ...
            if not starts:                                                         # ... or behind a `ret` of unreached code
                r = find(self.w(before)); starts = r is not None and r[1] == 'r'
            if starts and self.clean_function(v): out.add(v)
        return out
    def sweep(self):
        """linear decode of every remaining gap, re-synchronising behind each ret; returns instructions added"""
        added = 0; pc = self.code0
        while pc < self.n:
            if self.cov[pc]: pc += 1; continue
            # a run of code: must decode from here to a ret / jump without hitting an unknown opcode or reached words
            q = pc; run = []; ok = False
            while q < self.n and not self.cov[q]:
                r = find(self.w(q))
                if r is None: break
                sz, kind = r
                if q + sz > self.n or any(self.cov[q + k] for k in range(sz)): break
                run.append((q, sz, kind, self.w(q))); q += sz
                if kind in ('r', 'j'): ok = True; break
            else:
                ok = bool(run) and q < self.n and q in self.ins           # ran into reached code at an instruction boundary
            if ok:
                for a, sz, kind, code in run:
                    self.ins[a] = (sz, kind, code, 'sweep'); added += 1
                    for k in range(sz): self.cov[a + k] = 1
                pc = q
            else: pc += 1
        return added

def parse_bdx(d):
    """-> dict(name, triggers, counts, syscalls=[(table, trap, pc, class)]) or None if not a plausible BDX"""
    if len(d) < 0x20: return None
    name = d[:16]
    if not all(32 <= b < 127 or b == 0 for b in name) or name[0] == 0: return None
    work, stack, temp = struct.unpack_from('<iii', d, 16)
    if not (0 <= work < 0x100000 and 0 <= stack < 0x100000 and 0 <= temp < 0x100000): return None
    trig = []; p = 28
    while p + 8 <= len(d):
        k, a = struct.unpack_from('<ii', d, p); p += 8
        if k == 0 and a == 0: break
        trig.append((k, a))
        if len(trig) > 4096: return None
    else: return None
    S = Script(d); S.code0 = (p - 16) // 2
    n_entry = S.walk([a for k, a in trig], 'entry')
    n_cb = 0; rounds = 0; cbs = []
    while True:
        c = sorted(S.callbacks())
        if not c: break
        rounds += 1; cbs += c
        n_cb += S.walk(c, 'callback')
        if rounds > 10000: break
    words_before_sweep = sum(S.cov)
    n_sweep = S.sweep()
    sysc = [((code >> 6) & 0x3ff, S.w(pc + 1), pc, cls) for pc, (sz, kind, code, cls) in sorted(S.ins.items()) if kind == 's']
    # safety net: the raw encoding of a table-3 syscall (opcode word 0x00ca, trap < 114) in words no pass decoded (data, or code behind
    # something that does not decode): class `raw`, not counted as a use
    for pc in range(S.code0, S.n - 1):
        if not S.cov[pc] and S.w(pc) == 0x00ca and S.w(pc + 1) < 114: sysc.append((3, S.w(pc + 1), pc, 'raw'))
    return dict(name=name.split(b'\0')[0].decode('latin1'), ntrig=len(trig), triggers=trig, code_words=S.n - S.code0,
                words_entry_callback=words_before_sweep, words_sweep=sum(S.cov) - words_before_sweep, words_undecoded=S.n - S.code0 - sum(S.cov),
                ninstr_entry=n_entry, ninstr_callback=n_cb, ninstr_sweep=n_sweep, callback_functions=len(cbs), syscalls=sysc)

def bar_entries(b):
    if b[:4] != b'BAR\x01' or len(b) < 16: return []
    n = struct.unpack_from('<I', b, 4)[0]
    if n > 5000 or 16 + 16 * n > len(b): return []
    out = []
    for i in range(n):
        t, dup, nm, o, s = struct.unpack_from('<HH4sII', b, 16 + 16 * i)
        if o + s > len(b) or o < 16: continue
        out.append((t, nm.rstrip(b'\0').decode('latin1'), b[o:o + s]))
    return out

def walk(path, b, depth=0):
    """yield (label, bdx_result) for every BDX found"""
    if depth > 6: return
    for t, nm, d in bar_entries(b):
        lab = '%s/%s' % (path, nm)
        if t == 3:
            r = parse_bdx(d)
            if r: yield lab, r
        elif d[:4] == b'BAR\x01': yield from walk(lab, d, depth + 1)

def scan_file(p):
    try: b = open(p, 'rb').read()
    except Exception: return []
    out = []
    if p.lower().endswith('.bdx'):
        r = parse_bdx(b)
        if r: out.append((os.path.basename(p), r))
    out += list(walk(os.path.basename(p), b))
    return out

def mod_table(path):
    """names of table 3 and the implemented indices, from the runtime source"""
    src = open(path).read(); names = {}
    for m in re.finditer(r'\{"([A-Za-z_0-9]+)",\s*0x[0-9a-fA-F]+u?,\s*0x[0-9a-fA-F]+\},\s*/\*\s*(\d+)\s*\*/', src): names[int(m.group(2))] = m.group(1)
    sw = src[src.index('switch(idx){'):]; sw = sw[:sw.index('default:')]
    impl = set()
    for m in re.finditer(r'case\s+(\d+)\s*:', sw): impl.add(int(m.group(1)))
    return names, impl

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json'); ap.add_argument('--mod-syscalls'); ap.add_argument('--table', type=int, default=3); ap.add_argument('--detail', action='store_true')
    ap.add_argument('--skip-glob', action='append', default=[], help='relative path pattern (fnmatch) of files to leave out, e.g. "obj/G_*": the vanilla gummi-ship objects, '
                    'whose table 3 is the exe\'s own gummi table (0x14072f1a0, installed by exe 0x1f9aa0), not the mod\'s')
    ap.add_argument('paths', nargs='+')
    a = ap.parse_args()
    res = {}; nfiles = 0; nbdx = 0; nskip = 0; tot = collections.Counter()
    for root in a.paths:
        files = [root] if os.path.isfile(root) else sorted(os.path.join(dp, f) for dp, dn, fn in os.walk(root, followlinks=True) if '.git' not in dp.split(os.sep) for f in fn)
        for p in files:
            try:
                with open(p, 'rb') as fh: magic = fh.read(4)
            except Exception: continue
            if magic != b'BAR\x01' and not p.lower().endswith('.bdx'): continue
            rel = os.path.relpath(p, root if os.path.isdir(root) else os.path.dirname(root) or '.')
            if any(fnmatch.fnmatch(rel, g) for g in a.skip_glob): nskip += 1; continue
            nfiles += 1
            for lab, r in scan_file(p):
                nbdx += 1
                for k in ('code_words', 'words_entry_callback', 'words_sweep', 'words_undecoded', 'ninstr_entry', 'ninstr_callback', 'ninstr_sweep', 'callback_functions'): tot[k] += r[k]
                res[os.path.relpath(p, root if os.path.isdir(root) else os.path.dirname(root) or '.') + '::' + lab] = r
    names, impl = mod_table(a.mod_syscalls) if a.mod_syscalls else ({}, set())
    use = {}                                   # (table, trap) -> class -> {script: count}
    for k, r in res.items():
        for t, tr, pc, cls in r['syscalls']: use.setdefault((t, tr), {}).setdefault(cls, collections.Counter())[k] += 1
    by_table = collections.Counter(t for (t, tr), u in use.items() for c in u.values() for _ in range(sum(c.values())))
    if a.skip_glob: print('bdxscan: %d files left out by --skip-glob %s' % (nskip, ' '.join(a.skip_glob)))
    print('bdxscan: %d bar/bdx files, %d BDX scripts; code words %d: %d reached from entries and callbacks, %d found by the linear sweep, %d not decodable (data); '
          'instructions: %d entry, %d callback (%d callback functions), %d sweep' % (nfiles, nbdx, tot['code_words'], tot['words_entry_callback'], tot['words_sweep'], tot['words_undecoded'],
          tot['ninstr_entry'], tot['ninstr_callback'], tot['callback_functions'], tot['ninstr_sweep']))
    print('bdxscan: syscalls by table: %s' % ', '.join('%d: %d' % (t, by_table[t]) for t in sorted(by_table)))
    T = a.table
    print('bdxscan: table %d, by trap (uses in code reached from an entry / from a callback address / only by the linear sweep; scripts):' % T)
    for (t, tr) in sorted(use):
        if t != T: continue
        u = use[(t, tr)]; c = {cls: sum(u.get(cls, {}).values()) for cls in ('entry', 'callback', 'sweep', 'raw')}
        scripts = sorted({s for cc in u.values() for s in cc})
        st = ''
        if a.mod_syscalls and T == 3: st = '  %-28s %s' % (names.get(tr, '?'), 'implemented' if tr in impl else 'NAMED FATAL')
        print('  %3d: %5d / %5d / %5d   in %3d scripts%s%s' % (tr, c['entry'], c['callback'], c['sweep'], len(scripts), st,
              '   (+ %d raw encodings in undecoded words)' % c['raw'] if c['raw'] else ''))
        if a.detail:
            for s in scripts: print('         %s  (%s)' % (s, ', '.join('%s %d' % (cls, u[cls][s]) for cls in ('entry', 'callback', 'sweep', 'raw') if u.get(cls, {}).get(s))))
    if a.mod_syscalls and T == 3:
        fat = sorted(tr for (t, tr) in use if t == 3 and tr not in impl and any(use[(3, tr)].get(c) for c in ('entry', 'callback', 'sweep')))
        rawonly = sorted(tr for (t, tr) in use if t == 3 and not any(use[(3, tr)].get(c) for c in ('entry', 'callback', 'sweep')))
        if rawonly: print('bdxscan: table 3 traps seen ONLY as a raw encoding in undecoded words (data that looks like one; not counted): %s' % ', '.join('%d %s' % (tr, names.get(tr, '?')) for tr in rawonly))
        live = [tr for tr in fat if any(use[(3, tr)].get(c) for c in ('entry', 'callback'))]
        print('bdxscan: table 3 traps that are used and still a named fatal: %s; of those in code reached from an entry or a callback: %s'
              % (', '.join('%d %s' % (tr, names.get(tr, '?')) for tr in fat) or 'none', ', '.join('%d %s' % (tr, names.get(tr, '?')) for tr in live) or 'none'))
    if a.json:
        json.dump(dict(files=nfiles, bdx=nbdx, totals=dict(tot), by_table={str(k): v for k, v in by_table.items()},
                       uses={'%d,%d' % k: {cls: dict(c) for cls, c in u.items()} for k, u in use.items()},
                       scripts={k: dict(r, syscalls=[list(x) for x in r['syscalls']]) for k, r in res.items()}), open(a.json, 'w'))

if __name__ == '__main__':
    main()
