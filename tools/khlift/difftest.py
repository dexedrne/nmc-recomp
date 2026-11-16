#!/usr/bin/env python3
"""Differential test: ORIGINAL bytes of a function (oracle) vs the lifted C, with callee stubs consistent on both sides.

  difftest.py --oracle unicorn|native --rvas 1234,5678 | --rvas-file F  [--tests N] [--out FILE] [--work DIR] [--seed S]

oracle unicorn  : Unicorn emulates the original bytes (no 256-bit AVX, no rcpps).  Callee entries are patched to INT3 and a
                  UC_HOOK_INTR applies the stub effect.  Unmapped accesses below 4 GiB are lazily mapped (and mirrored on the host).
oracle native   : the original bytes run natively on this CPU through tools/native_oracle.c (x86-64 Linux dev host only).
Lifted side     : the emitter output for the function (named t_<rva>), every callee replaced by `kh_test_stub` (khtest.c),
                  compiled with gcc -O1 -fno-strict-aliasing, run on the same initial state in a forked child.
Stub effect (identical in C and here): out-params at [rcx],[rdx] if inside the arena, rax,rdx,rcx,r8-r11 and xmm0-5 become
hash(callee address, call number), the return address is popped.  Calls to non-function addresses fault and the test is skipped.
Compared state: all 16 GPRs, xmm0-15 (ymm0-15 for AVX tests), the arena, the whole stack, every image byte that changed, every
lazily mapped page.  A test is VALID when the oracle returns normally to the sentinel.
"""
import sys, os, json, struct, random, ctypes, mmap, subprocess, time, argparse, collections, signal, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from khlift import BASE, universe as U, imports as IM
from khlift.emit import FnLifter
import lift_all as LA

HERE = os.path.dirname(os.path.abspath(__file__))
M64 = (1 << 64) - 1
ARENA, ARENA_SZ = 0x10000000, 0x40000
STACK, STACK_SZ = 0x20000000, 0x20000
RSP0 = STACK + 0x10008
SENT_UC = 0x30000000
GSB = ARENA + 0x20000
R64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
OFF = {n: 16 + 8 * i for i, n in enumerate(R64)}
OFF_DFLAG = 176; OFF_GS = 2536
YMM = lambda i: 224 + 64 * i
KSZ = 4096
MAP_FIXED_NOREPLACE = 0x100000
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

def hmap(addr, size, init=None):
    r = libc.mmap(addr, size, 7, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
    if r != addr: raise RuntimeError('mmap fail %x %x errno=%d' % (addr, r or 0, ctypes.get_errno()))
    if init: ctypes.memmove(addr, init, len(init))

def sm64(x):
    x = (x + 0x9E3779B97F4A7C15) & M64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M64
    return x ^ (x >> 31)

def vfl(h1, h2):
    hs = [h1, h2]; v = 0
    for i in range(4):
        h = hs[i >> 1] >> ((i & 1) * 32)
        w = 0x3f800000 | ((h & 0xffffffff) & 0x7fffff) | ((((h >> 23) & 1)) << 31)
        v |= w << (32 * i)
    return v

def stub_effect(va, n, rcx, rdx, arena_lo, arena_hi):
    """-> (regs dict, [(addr, bytes)] writes, xmm list) ; mirrors kh_test_stub"""
    h = sm64(((va * 0x100000001B3) & M64) ^ ((n * 0x9E3779B97F4A7C15) & M64))
    wr = []
    for i, p in enumerate((rcx, rdx)):
        if arena_lo <= p and p + 16 <= arena_hi:
            wr.append((p, struct.pack('<QQ', sm64((h + 20 + i) & M64), sm64((h + 30 + i) & M64))))
    regs = dict(rax=sm64((h + 1) & M64), rdx=sm64((h + 2) & M64), rcx=sm64((h + 3) & M64), r8=sm64((h + 4) & M64), r9=sm64((h + 5) & M64),
                r10=sm64((h + 6) & M64), r11=sm64((h + 7) & M64))
    xmm = [vfl(sm64((h + 40 + 2 * i) & M64), sm64((h + 41 + 2 * i) & M64)) for i in range(6)]
    return regs, wr, xmm

# ------------------------------------------------------------------ test generation
NONAN = False
class Gen:
    def __init__(self, seed, fptrs, avx=False): self.r = random.Random(seed); self.fptrs = fptrs; self.avx = avx
    def fl(self):
        v = self._fl()
        if NONAN and ((v >> 23) & 0xff) == 0xff: v = (v & 0x807fffff) | (0x80 << 23)       # no Inf/NaN (Unicorn's softfloat NaN rules differ from silicon)
        return v
    def _fl(self):
        r = self.r; k = r.random()
        if k < .80: return struct.unpack('<I', struct.pack('<f', r.uniform(-100, 100)))[0]
        if k < .88: return struct.unpack('<I', struct.pack('<f', float(r.randint(-8, 8))))[0]
        if k < .92: return r.choice([0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000])
        if k < .95: return r.getrandbits(32)
        if k < .98: return r.getrandbits(23)
        return 0x7fc00000
    def word(self):
        r = self.r; k = r.random()
        if k < .35: return self.fl()
        if k < .55: return r.randint(0, 64)
        if k < .70: return r.getrandbits(32)
        if k < .75: return r.getrandbits(8)
        return self.fl()
    def mem(self, n):
        b = bytearray()
        for i in range(n // 4): b += struct.pack('<I', self.word())
        for i in range(0, n - 8, 8):
            k = self.r.random()
            if k < 0.25: b[i:i + 8] = struct.pack('<Q', ARENA + self.r.randrange(0x1000, 0x20000, 16))
            elif k < (0.60 if PROFILE == 'fptr' else 0.33) and self.fptrs: b[i:i + 8] = struct.pack('<Q', self.r.choice(self.fptrs))
        return bytes(b)
    def gpr(self):
        r = self.r; k = r.random()
        if k < .62: return ARENA + r.randrange(0x1000, 0x20000, 16)
        if k < .78: return r.randint(0, 64)
        if k < .88: return r.getrandbits(32)
        if k < .92: return r.getrandbits(46)
        return r.choice([0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0xffffffffffffffff])
    def xmm(self): return sum(self.fl() << (32 * i) for i in range(4))

PROFILE = None
def make_test(seed, fptrs, sent, avx=False):
    g = Gen(seed, fptrs, avx); t = {}
    for n in R64:
        if n != 'rsp': t[n] = g.gpr()
    t['rsp'] = RSP0
    for i in range(16): t['xmm%d' % i] = g.xmm()
    for i in range(16): t['yh%d' % i] = g.xmm() if avx else 0
    t['arena'] = bytearray(g.mem(ARENA_SZ)); t['stack'] = bytearray(g.mem(STACK_SZ))
    t['stack'][RSP0 - STACK:RSP0 - STACK + 8] = struct.pack('<Q', sent)
    if PROFILE == 'dsp':
        # struct-of-pointers calling convention shared by the 9 AVX DSP kernels: rcx -> {src,dst,coef,count,stride@0x24,aux@0x2c,flag@0x3c,flag@0x40}
        r = g.r; base = ARENA + 0x2000; a = t['arena']
        for off in range(0x8000, ARENA_SZ - 0x1000, 4):
            v = r.uniform(-4, 4) if r.random() > 0.01 else r.choice([float('nan'), float('inf'), -0.0, 1e-39])
            a[off:off + 4] = struct.pack('<f', v)
        def ptr(): return ARENA + 0x8000 + 32 * r.randrange(0, 0x6000 // 32)
        st = struct.pack('<QQQQ', ptr(), ptr(), ptr(), r.choice([0, 0, 1, 2, 3, 4, 5, 8]))
        a[0x2000:0x2000 + 32] = st
        a[0x2000 + 0x24:0x2000 + 0x2c] = struct.pack('<Q', r.choice([1, 8, 16, 24, 32, 33, 40, 48, 64]))
        a[0x2000 + 0x2c:0x2000 + 0x34] = struct.pack('<Q', ptr())
        a[0x2000 + 0x3c:0x2000 + 0x44] = struct.pack('<II', r.choice([0, 1, 2, 3, 4, 8, 16]), r.choice([0, 1]))
        t['rcx'] = base
    return t

# ------------------------------------------------------------------ environment
class Env:
    def __init__(self, work, oracle):
        self.work = work; self.oracle = oracle
        self.fs = U.load(); self.by_rva = {f.rva: f for f in self.fs}
        self.imps = IM.load_imports()
        img = bytearray(U.image()); self.IMGSZ = U.image_size()
        for im in self.imps: img[im.slot - BASE:im.slot - BASE + 8] = struct.pack('<Q', im.addr)       # IAT -> synthetic addresses
        self.IMG = bytes(img)
        hooks = LA.load_hooks()
        self.stubset = sorted({f.va for f in self.fs} | set(hooks) | {im.addr for im in self.imps})
        self.starts = sorted({f.va for f in self.fs} | set(hooks))
        rnd = random.Random(5)
        self.fptrs = rnd.sample(self.starts, 300) + [im.addr for im in self.imps[:40]]
        import pefile
        pe = pefile.PE(U.EXE, fast_load=True)
        self.secs = [(s.VirtualAddress, (max(s.Misc_VirtualSize, s.SizeOfRawData) + 0xfff) & ~0xfff, s.Characteristics) for s in pe.sections]
        self.wr_secs = [(a, b) for a, b, ch in self.secs if ch & 0x80000000]
        self.text = [(a, b) for a, b, ch in self.secs if ch & 0x20000000]
        self.sent = SENT_UC

libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
def patch_int3(env, rva):
    for a, sz in env.text: libc.mprotect(BASE + a, sz, 7)
    for va in env.starts:
        if va != BASE + rva: ctypes.memmove(va, b'\xcc', 1)
    for a, sz in env.text: libc.mprotect(BASE + a, sz, 5)
def sec_perm(ch):
    p = 0
    if ch & 0x40000000: p |= 1
    if ch & 0x80000000: p |= 2
    if ch & 0x20000000: p |= 4
    return p or 1
def host_map(env):
    hmap(BASE, env.IMGSZ, env.IMG)
    libc.mprotect(BASE, 0x1000, 1)
    for rva, sz, ch in env.secs: libc.mprotect(BASE + rva, sz, sec_perm(ch))
    hmap(ARENA, ARENA_SZ); hmap(STACK, STACK_SZ)
    hmap(IM.KH_IMPORT_ADDR_BASE, (16 * len(env.imps) + 0xfff) & ~0xfff, (b'\xcc' + bytes(15)) * len(env.imps))

# ------------------------------------------------------------------ build lifted library
def build_lib(env, rvas, avx_names=False):
    """lift rvas as t_<rva>, add callee stubs, compile -> (lib path, ok rvas, failures)"""
    LA.G['fs'] = {f.rva: f for f in env.fs}
    fs, liftable, lenv, imps = LA.build_env()
    src = ['#include "khrt.h"', 'void kh_test_stub(KCtx *g, u64 va);']
    callees = set(); imports = set(); ok = []; fail = {}; info = {}
    for rva in rvas:
        f = env.by_rva[rva]
        L = FnLifter(lenv, f, name='t_%x' % rva); r = L.lift()
        if r.err: fail[rva] = r.err; continue
        if f.va in r.callees: fail[rva] = 'self-recursive (excluded)'; continue
        ok.append(rva); src.append(r.text); callees |= r.callees; imports |= r.imports
        info[rva] = dict(callees=len(r.callees), imports=len(r.imports), dcall=r.n_dispatch_call, djmp=r.n_dispatch_jmp, jt=r.n_jt, v256=r.v256, insns=r.insns)
    sym2addr = {im.sym: im.addr for im in env.imps}
    stubs = []
    for va in sorted(callees):
        sym = lenv.callee_sym(va); stubs.append('void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (sym, va))
    for s in sorted(imports): stubs.append('void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (s, sym2addr[s]))
    cdecl = []
    decls = ['void %s(KCtx *g);' % lenv.callee_sym(va) for va in sorted(callees)] + ['void %s(KCtx *g);' % s_ for s_ in sorted(imports)]
    body = '\n'.join(src[:2] + decls + src[2:] + stubs)
    h = hashlib.sha1((body + env.oracle).encode()).hexdigest()[:10]
    c = os.path.join(env.work, 'dt_%s.c' % h); so = os.path.join(env.work, 'dt_%s.so' % h)
    open(c, 'w').write(body)
    rt = os.path.join(HERE, 'rt')
    cmd = ['gcc', '-O1', '-w', '-fno-strict-aliasing', '-shared', '-fPIC', '-I', rt, '-o', so, c, os.path.join(rt, 'khtest.c'), os.path.join(rt, 'khtest_stub.c'), os.path.join(rt, 'khrt_cpu.c'), os.path.join(rt, 'khrt_rcp_tab.c')]
    if env.oracle == 'native': cmd.append(os.path.join(HERE, 'native_oracle.c'))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: raise RuntimeError('compile failed: ' + r.stderr[:2000])
    return so, ok, fail, info

# ------------------------------------------------------------------ Unicorn oracle
class UcOracle:
    def __init__(self, env):
        from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_INTR, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, UC_HOOK_MEM_WRITE
        import unicorn.x86_const as X
        self.X = X; self.env = env
        uc = Uc(UC_ARCH_X86, UC_MODE_64); self.uc = uc
        uc.mem_map(BASE, env.IMGSZ, 7); uc.mem_write(BASE, env.IMG)
        uc.mem_map(ARENA, ARENA_SZ); uc.mem_map(STACK, STACK_SZ); uc.mem_map(SENT_UC, 0x1000)
        nimp = len(env.imps)
        uc.mem_map(IM.KH_IMPORT_ADDR_BASE & ~0xfff, (16 * nimp + 0xfff) & ~0xfff)
        self.patched = {}
        for va in env.starts:
            uc.mem_write(va, b'\xcc')
        for im in env.imps: uc.mem_write(im.addr, b'\xcc')
        uc.mem_protect(BASE, 0x1000, 1)
        for rva, sz, ch in env.secs: uc.mem_protect(BASE + rva, sz, sec_perm(ch))
        self.n = 0; self.err = None
        self.UCR = {n: getattr(X, 'UC_X86_REG_' + n.upper()) for n in R64}
        self.UCX = [getattr(X, 'UC_X86_REG_XMM%d' % i) for i in range(16)]
        self.UCY = [getattr(X, 'UC_X86_REG_YMM%d' % i) for i in range(16)]
        self.stubset = set(env.stubset)
        uc.hook_add(UC_HOOK_INTR, self.on_intr)
        self.UC_HOOK_MEM_READ_UNMAPPED = UC_HOOK_MEM_READ_UNMAPPED; self.UC_HOOK_MEM_WRITE_UNMAPPED = UC_HOOK_MEM_WRITE_UNMAPPED; self.UC_HOOK_MEM_WRITE = UC_HOOK_MEM_WRITE
    def on_intr(self, uc, intno, ud):
        X = self.X
        rip = uc.reg_read(X.UC_X86_REG_RIP); va = rip - 1
        if intno != 3 or va not in self.stubset: self.err = 'int%d@%x' % (intno, va); uc.emu_stop(); return
        rsp = uc.reg_read(X.UC_X86_REG_RSP); ret = struct.unpack('<Q', bytes(uc.mem_read(rsp, 8)))[0]
        regs, wr, xmm = stub_effect(va, self.n, uc.reg_read(X.UC_X86_REG_RCX), uc.reg_read(X.UC_X86_REG_RDX), ARENA, ARENA + ARENA_SZ)
        self.n += 1
        for a, b in wr: uc.mem_write(a, b)
        for k, v in regs.items(): uc.reg_write(self.UCR[k], v)
        for i, v in enumerate(xmm): uc.reg_write(self.UCX[i], v)
        uc.reg_write(X.UC_X86_REG_RSP, rsp + 8); uc.reg_write(X.UC_X86_REG_RIP, ret)
    def run(self, rva, t, maxins=30000):
        from unicorn import UcError
        X = self.X; uc = self.uc; entry = BASE + rva; env = self.env
        uc.mem_write(entry, env.IMG[rva:rva + 1])      # un-patch the function under test
        uc.mem_write(ARENA, bytes(t['arena'])); uc.mem_write(STACK, bytes(t['stack']))
        for n in R64: uc.reg_write(self.UCR[n], t[n])
        for i in range(16):
            uc.reg_write(self.UCX[i], t['xmm%d' % i])
        uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        try: uc.reg_write(X.UC_X86_REG_MXCSR, 0x1f80)
        except Exception: pass
        try: uc.reg_write(X.UC_X86_REG_GS_BASE, GSB)
        except Exception: pass
        self.n = 0; self.err = None
        writes = []; lazy = []
        def hum(u, access, addr, size, value, ud):
            pg = addr & ~0xfff
            if pg < 0x100000 or pg >= 0x100000000 or len(lazy) >= 48: return False
            try: u.mem_map(pg, 0x1000); lazy.append(pg); return True
            except Exception: return False
        hu = uc.hook_add(self.UC_HOOK_MEM_READ_UNMAPPED | self.UC_HOOK_MEM_WRITE_UNMAPPED, hum)
        def hw(u, access, addr, size, value, ud):
            if BASE <= addr < BASE + env.IMGSZ: writes.append((addr, size))
        h = uc.hook_add(self.UC_HOOK_MEM_WRITE, hw)
        ok = False; err = None
        try:
            uc.emu_start(entry, SENT_UC, timeout=3000000, count=maxins)
            ok = uc.reg_read(X.UC_X86_REG_RIP) == SENT_UC and self.err is None
            err = None if ok else (self.err or 'noreturn')
        except UcError as e: err = str(e)
        uc.hook_del(h); uc.hook_del(hu)
        res = None
        if ok:
            res = {n: uc.reg_read(self.UCR[n]) for n in R64}
            for i in range(16): res['xmm%d' % i] = uc.reg_read(self.UCX[i])
            res['arena'] = bytes(uc.mem_read(ARENA, ARENA_SZ)); res['stack'] = bytes(uc.mem_read(STACK, STACK_SZ))
            imgw = {}
            for a, s in writes:
                fin = bytes(uc.mem_read(a, s))
                for k in range(s):
                    if fin[k] != env.IMG[a - BASE + k]: imgw[a + k] = fin[k]
            res['imgw'] = imgw
            res['lazy'] = [(pg, bytes(uc.mem_read(pg, 0x1000))) for pg in lazy]
        for pg in lazy: uc.mem_unmap(pg, 0x1000)
        for a, s in reversed(writes): uc.mem_write(a, env.IMG[a - BASE:a - BASE + s])
        uc.mem_write(entry, b'\xcc')
        return ok, err, res

# ------------------------------------------------------------------ host-side helpers
def host_set(t):
    ctypes.memmove(ARENA, bytes(t['arena']), ARENA_SZ); ctypes.memmove(STACK, bytes(t['stack']), STACK_SZ)

def mkstate(t):
    g = (ctypes.c_uint8 * KSZ)()
    for n in R64: struct.pack_into('<Q', g, OFF[n], t[n])
    struct.pack_into('<Q', g, OFF_DFLAG, 1); struct.pack_into('<Q', g, OFF_GS, GSB)
    for i in range(16):
        v = t['xmm%d' % i]; struct.pack_into('<QQ', g, YMM(i), v & M64, v >> 64)
        y = t['yh%d' % i]; struct.pack_into('<QQ', g, YMM(i) + 16, y & M64, y >> 64)
    return g

def state_out(g, avx):
    gb = bytes(g); d = {}
    for n in R64: d[n] = struct.unpack_from('<Q', gb, OFF[n])[0]
    for i in range(16):
        lo, hi = struct.unpack_from('<QQ', gb, YMM(i)); d['xmm%d' % i] = lo | (hi << 64)
        if avx:
            lo, hi = struct.unpack_from('<QQ', gb, YMM(i) + 16); d['yh%d' % i] = lo | (hi << 64)
    return d

def compare(res, d, avx, hostimgw, lazyok, stack_lo=0):
    diffs = []
    for n in R64:
        if res[n] != d[n]: diffs.append((n, hex(res[n]), hex(d[n])))
    for i in range(16):
        if res['xmm%d' % i] != d['xmm%d' % i]: diffs.append(('xmm%d' % i, '%032x' % res['xmm%d' % i], '%032x' % d['xmm%d' % i]))
        if avx and res.get('yh%d' % i, 0) != d.get('yh%d' % i, 0): diffs.append(('ymmhi%d' % i, '%032x' % res.get('yh%d' % i, 0), '%032x' % d.get('yh%d' % i, 0)))
    ha = ctypes.string_at(ARENA, ARENA_SZ); hs = ctypes.string_at(STACK, STACK_SZ)
    if ha != res['arena']:
        i = next(k for k in range(ARENA_SZ) if ha[k] != res['arena'][k]); diffs.append(('arena@%x' % i, res['arena'][i:i + 16].hex(), ha[i:i + 16].hex()))
    if hs[stack_lo:] != res['stack'][stack_lo:]:
        i = next(k for k in range(stack_lo, STACK_SZ) if hs[k] != res['stack'][k]); diffs.append(('stack@%x' % i, res['stack'][i:i + 16].hex(), hs[i:i + 16].hex()))
    if res['imgw'] != hostimgw:
        ks = sorted(set(res['imgw']) ^ set(hostimgw) | {k for k in res['imgw'] if k in hostimgw and res['imgw'][k] != hostimgw[k]})
        diffs.append(('img@%x' % ks[0], res['imgw'].get(ks[0]), hostimgw.get(ks[0])))
    return diffs

def host_imgdiff(env, imgsnap):
    """changed image bytes vs pristine (writable sections only) -> {addr: byte}"""
    out = {}
    for rva, sz in env.wr_secs:
        cur = ctypes.string_at(BASE + rva, sz); ref = env.IMG[rva:rva + sz]
        if cur != ref:
            for pg in range(0, sz, 4096):
                if cur[pg:pg + 4096] != ref[pg:pg + 4096]:
                    for k in range(pg, min(sz, pg + 4096)):
                        if cur[k] != ref[k]: out[BASE + rva + k] = cur[k]
    return out

def host_restore_img(env, imgw):
    for a in imgw: ctypes.memmove(a, env.IMG[a - BASE:a - BASE + 1], 1)

# ------------------------------------------------------------------ driver
def run_unicorn_function(env, uco, lib, rva, ntests, seed0, sent):
    fn = getattr(lib, 't_%x' % rva)
    tests = []; skips = collections.Counter()
    for k in range(ntests):
        t = make_test(seed0 + k * 7919 + rva, env.fptrs, SENT_UC, False)
        ok, err, res = uco.run(rva, t)
        if ok: tests.append((t, res))
        else: skips[(err or '')[:30]] += 1
    out = {'rva': hex(rva), 'valid': len(tests), 'skipped': dict(skips), 'mismatch': 0, 'crash': False, 'examples': []}
    if not tests: return out
    rfd, wfd = os.pipe(); pid = os.fork()
    if pid == 0:
        os.close(rfd); results = []; signal.alarm(60)
        try:
            lib.khtest_init()
            patch_int3(env, rva)                                          # same bytes as the oracle sees at callee entries
            for idx, (t, res) in enumerate(tests):
                host_set(t); g = mkstate(t); okmap = True
                for pg, data in res['lazy']:
                    if libc.mmap(pg, 0x1000, 7, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != pg: okmap = False
                    else: ctypes.memmove(pg, bytes(0x1000), 0x1000)
                if not okmap: results.append(None); continue
                fa = ctypes.c_ulong(0); fs = ctypes.c_int(0)
                lib.khtest_reset()
                rc = lib.khtest_call(ctypes.cast(fn, ctypes.c_void_p), ctypes.cast(g, ctypes.c_void_p), ctypes.byref(fa), ctypes.byref(fs))
                imgw = host_imgdiff(env, None)
                if rc:
                    for pg, data in res['lazy']: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                    host_restore_img(env, imgw)
                    results.append(None if (rc == 1 and fa.value in (0, 0xffffffffffffffff) and fs.value == 11) else ['FAULT rc%d sig%d addr %x' % (rc, fs.value, fa.value)]); continue
                lib.khtest_acc()
                d = compare(res, state_out(g, False), False, imgw, True)
                for pg, data in res['lazy']:
                    if ctypes.string_at(pg, 0x1000) != data: d.append(('lazy@%x' % pg, '', ''))
                    libc.munmap(ctypes.c_void_p(pg), 0x1000)
                host_restore_img(env, imgw)
                results.append(d)
            os.write(wfd, json.dumps(results).encode())
        except BaseException as e:
            os.write(wfd, json.dumps({'err': repr(e)}).encode())
        os._exit(0)
    os.close(wfd); data = b''
    while True:
        c = os.read(rfd, 1 << 20)
        if not c: break
        data += c
    os.close(rfd); _, st = os.waitpid(pid, 0)
    return finish(out, data, st)

def finish(out, data, st):
    if not data:
        out['crash'] = True; out['signal'] = os.WTERMSIG(st) if os.WIFSIGNALED(st) else None; return out
    r = json.loads(data)
    if isinstance(r, dict): out['crash'] = True; out['err'] = r['err']; return out
    nonc = sum(1 for d in r if d is None)
    fl = sum(1 for d in r if d and isinstance(d[0], str))
    out['valid'] -= nonc; out['skipped']['host-nomap'] = nonc
    # a host FAULT where the oracle returned normally is a real mismatch (lifted code faulted); count it
    mm = [(i, d) for i, d in enumerate(r) if d]
    out['mismatch'] = len(mm); out['host_fault'] = fl; out['examples'] = [(i, d[:3]) for i, d in mm[:2]]
    return out

def run_native_function(env, lib, no, rva, ntests, seed0):
    """oracle = native execution; both oracle and lifted run in ONE forked child"""
    fn = getattr(lib, 't_%x' % rva)
    sent = no.native_sentinel()
    out = {'rva': hex(rva), 'valid': 0, 'skipped': {}, 'mismatch': 0, 'crash': False, 'examples': []}
    rfd, wfd = os.pipe(); pid = os.fork()
    if pid == 0:
        os.close(rfd); results = []; skips = collections.Counter(); valid = 0
        try:
            signal.alarm(120)
            lib.khtest_init()
            # patch callee entries to int3 (native oracle only); host lifted code never reads code bytes
            patch_int3(env, rva)
            for k in range(ntests):
                t = make_test(seed0 + k * 7919 + rva, env.fptrs, sent, True)
                host_set(t)
                inn = NState(); outn = NState()
                for i, n in enumerate(R64): inn.gpr[i] = t[n]
                for i in range(16):
                    v = t['xmm%d' % i] | (t['yh%d' % i] << 128)
                    ctypes.memmove(ctypes.addressof(inn) + 128 + 32 * i, v.to_bytes(32, 'little'), 32)
                inn.gs = GSB
                lib.khtest_reset()
                fa = ctypes.c_ulong(0)
                sig = no.native_run(ctypes.byref(inn), ctypes.byref(outn), BASE + rva, ctypes.byref(fa), 2000)
                nl = ctypes.c_int.in_dll(no, 'kn_nlazy').value
                lazy = [(ctypes.c_uint64 * 48).in_dll(no, 'kn_lazy')[i] for i in range(nl)]
                if sig:
                    skips['native-sig%d@%x/%x' % (sig, ctypes.c_uint64.in_dll(no, 'kn_pc').value - BASE, fa.value >> 12 if fa.value else 0)] += 1
                    for pg in lazy: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                    host_set(t); imgw = host_imgdiff(env, None); host_restore_img(env, imgw); continue
                res = {n: outn.gpr[i] for i, n in enumerate(R64)}
                for i in range(16):
                    b = bytes(bytearray(ctypes.string_at(ctypes.addressof(outn) + 128 + 32 * i, 32)))
                    v = int.from_bytes(b, 'little'); res['xmm%d' % i] = v & ((1 << 128) - 1); res['yh%d' % i] = v >> 128
                res['arena'] = ctypes.string_at(ARENA, ARENA_SZ); res['stack'] = ctypes.string_at(STACK, STACK_SZ)
                res['imgw'] = host_imgdiff(env, None); host_restore_img(env, res['imgw'])
                res['lazy'] = [(pg, ctypes.string_at(pg, 0x1000)) for pg in lazy]
                for pg in lazy: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                if res['rsp'] != RSP0 + 8: skips['native-rsp'] += 1; continue
                valid += 1
                host_set(t); g = mkstate(t); fs = ctypes.c_int(0)
                for pg, data in res['lazy']:
                    libc.mmap(pg, 0x1000, 7, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
                lib.khtest_reset()
                rc = lib.khtest_call(ctypes.cast(fn, ctypes.c_void_p), ctypes.cast(g, ctypes.c_void_p), ctypes.byref(fa), ctypes.byref(fs))
                imgw = host_imgdiff(env, None)
                if rc:
                    for pg, data in res['lazy']: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                    host_restore_img(env, imgw); results.append(None if (rc == 1 and fa.value in (0, 0xffffffffffffffff) and fs.value == 11) else ['FAULT rc%d sig%d addr %x' % (rc, fs.value, fa.value)]); continue
                lib.khtest_acc()
                d = compare(res, state_out(g, True), True, imgw, True)
                for pg, data in res['lazy']:
                    if ctypes.string_at(pg, 0x1000) != data: d.append(('lazy@%x' % pg, '', ''))
                    libc.munmap(ctypes.c_void_p(pg), 0x1000)
                host_restore_img(env, imgw)
                results.append(d)
            os.write(wfd, json.dumps(dict(results=results, valid=valid, skips=dict(skips), stubs=lib.khtest_stub_total(), disp=lib.khtest_disp_total())).encode())
        except BaseException as e:
            os.write(wfd, json.dumps({'err': repr(e)}).encode())
        os._exit(0)
    os.close(wfd); data = b''
    while True:
        c = os.read(rfd, 1 << 20)
        if not c: break
        data += c
    os.close(rfd); _, st = os.waitpid(pid, 0)
    if not data:
        out['crash'] = True; out['signal'] = os.WTERMSIG(st) if os.WIFSIGNALED(st) else None; return out
    r = json.loads(data)
    if 'err' in r: out['crash'] = True; out['err'] = r['err']; return out
    out['stub_calls'] = r.get('stubs', 0); out['dispatch_calls'] = r.get('disp', 0)
    res = r['results']; out['valid'] = r['valid'] - sum(1 for d in res if d is None); out['skipped'] = r['skips']; out['skipped']['noncanonical-GP'] = sum(1 for d in res if d is None)
    mm = [(i, d) for i, d in enumerate(res) if d]
    out['mismatch'] = len(mm); out['host_fault'] = sum(1 for d in res if d and isinstance(d[0], str)); out['examples'] = [(i, d[:3]) for i, d in mm[:2]]
    return out

class NState(ctypes.Structure):
    _fields_ = [('gpr', ctypes.c_uint64 * 16), ('ymm', ctypes.c_uint8 * 512), ('gs', ctypes.c_uint64)]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--oracle', default='unicorn'); ap.add_argument('--rvas'); ap.add_argument('--rvas-file')
    ap.add_argument('--tests', type=int, default=24); ap.add_argument('--nonan', action='store_true'); ap.add_argument('--profile'); ap.add_argument('--out', default='diff_result.json')
    ap.add_argument('--work', default=os.path.join(HERE, '..', 'work', 'difftest')); ap.add_argument('--seed', type=int, default=1234)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    global NONAN, PROFILE; NONAN = a.nonan; PROFILE = a.profile
    rvas = [int(x, 16) for x in a.rvas.split(',')] if a.rvas else [int(x, 16) for x in open(a.rvas_file).read().split()]
    env = Env(a.work, a.oracle)
    so, ok, fail, info = build_lib(env, rvas)
    lib = ctypes.CDLL(os.path.abspath(so))
    for fn in ('khtest_call',): getattr(lib, fn).restype = ctypes.c_int
    lib.khtest_stub_total.restype = ctypes.c_uint64; lib.khtest_disp_total.restype = ctypes.c_uint64
    lib.khtest_config.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_uint64]
    host_map(env)
    ss = (ctypes.c_uint64 * len(env.stubset))(*env.stubset)
    lib.khtest_config(ctypes.cast(ss, ctypes.c_void_p), len(env.stubset), ARENA, ARENA + ARENA_SZ)
    rows = []
    if a.oracle == 'unicorn':
        uco = UcOracle(env)
        for rva in ok:
            o = run_unicorn_function(env, uco, lib, rva, a.tests, a.seed, SENT_UC); o['info'] = info[rva]; rows.append(o)
    else:
        no = lib
        no.native_sentinel.restype = ctypes.c_uint64
        no.native_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
        no.native_config.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        no.native_config(ctypes.cast(ss, ctypes.c_void_p), len(env.stubset))
        for rva in ok:
            o = run_native_function(env, lib, no, rva, a.tests, a.seed); o['info'] = info[rva]; rows.append(o)
    json.dump(dict(rows=rows, lift_fail={hex(k): v for k, v in fail.items()}), open(a.out, 'w'))
    tot = len(rows); nv = sum(1 for r in rows if r['valid'] > 0)
    okf = sum(1 for r in rows if r['valid'] > 0 and r['mismatch'] == 0 and not r['crash'])
    print('functions', tot, 'with valid tests', nv, 'all-match', okf, 'mismatching', sum(1 for r in rows if r['mismatch']), 'crash', sum(1 for r in rows if r['crash']),
          'no-valid', tot - nv, 'lift-fail/excluded', len(fail), 'valid tests', sum(r['valid'] for r in rows))

if __name__ == '__main__':
    main()
