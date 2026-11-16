#!/usr/bin/env python3
"""Oracle check: an independent Unicorn emulation of the ORIGINAL kh2fm.exe bytes, driven by Python stubs that mirror the runtime's import
implementations (deterministic mode KH_DET=1), compared call by call with the import-call trace of the LIFTED run.

  oracle_unicorn.py --snap SNAP_PREFIX --lifted TRACE --out REPORT.json [--max N]

Start state: the lifted run's own snapshot (sparse arena pages, TEB/TLS/stack/rsp, steam objects, host-fn table), the exe image from the PE file with the
synthetic import addresses in the IAT, the fixed CPUID/XGETBV profile of khrt_cpu.c.  Both sides run the same entry (0x4393d0 after the TLS callbacks).
Compared per import call: name, rcx, rdx, r8, r9 (junk registers included) and the call-site return address.
Run with a python that has unicorn + pefile + capstone (scratch/S2-lifter/venv)."""
import sys, os, json, struct, argparse, re
sys.path.insert(0, '/home/user/projects/kh2-nmc-analysis/scratch/lift/tools')
from unicorn import *
from unicorn.x86_const import *
from khlift import imports as IM
import pefile

BASE = 0x140000000
IMG_SIZE = 0x2c2b000
IMPORT_BASE = 0x7ff7f0000000
HOST_BASE = 0x7ff7f8000000
SENT = 0x7ff7ffff0000
CONT = 0x7ff7ffff0100
EXE = '/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe'
M64 = (1 << 64) - 1
KNOWN_DLLS = ["kernel32.dll", "kernelbase.dll", "user32.dll", "ntdll.dll", "xaudio2_9.dll", "xaudio2_8.dll", "xinput1_4.dll", "xinput9_1_0.dll", "dinput8.dll", "d3d12.dll", "dxgi.dll", "d3d11.dll", "mfplat.dll", "mfreadwrite.dll", "advapi32.dll", "ole32.dll", "shell32.dll", "winmm.dll", "d3dcompiler_47.dll", "steam_api64.dll"]


def rup(v, a): return (v + a - 1) & ~(a - 1)


class Oracle:
    def __init__(self, snap, maxcalls):
        self.meta = json.load(open(snap + '.json'))
        self.bin = open(snap + '.bin', 'rb').read()
        self.max = maxcalls
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.trace = []
        self.err = None
        self.frames = []
        self.pending = None
        self.done = False
        self.bump = self.meta['bump']
        self.handles = {}
        self.next_h = 1
        self.tick = 0
        self.p_commode = 0
        self.cmdline = 0
        self.imps = IM.load_imports()
        self.names = []
        for im in self.imps:
            s = im.sym[len('rt_imp_'):]
            dll, name = s.split('__', 1)
            self.names.append('%s!%s' % (dll, name))
        self.build()

    # ---- memory helpers
    def r(self, a, n): return bytes(self.uc.mem_read(a, n))
    def w(self, a, b): self.uc.mem_write(a, bytes(b))
    def r64(self, a): return struct.unpack('<Q', self.r(a, 8))[0]
    def r32(self, a): return struct.unpack('<I', self.r(a, 4))[0]
    def r16(self, a): return struct.unpack('<H', self.r(a, 2))[0]
    def w64(self, a, v): self.w(a, struct.pack('<Q', v & M64))
    def w32(self, a, v): self.w(a, struct.pack('<I', v & 0xffffffff))
    def w16(self, a, v): self.w(a, struct.pack('<H', v & 0xffff))
    def cstr(self, a):
        out = b''
        while True:
            c = self.r(a + len(out), 1)
            if c == b'\0': return out
            out += c
    def wstr(self, a):
        out = []
        while True:
            c = self.r16(a + 2 * len(out))
            if not c: break
            out.append(c)
        return ''.join(chr(c) for c in out)
    def reg(self, R): return self.uc.reg_read(R)
    def setreg(self, R, v): self.uc.reg_write(R, v & M64)
    def arg(self, i):
        if i < 4: return self.reg([UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9][i])
        return self.r64(self.reg(UC_X86_REG_RSP) + 8 + 8 * i)

    def build(self):
        uc = self.uc
        pe = pefile.PE(EXE)
        img = bytes(pe.get_memory_mapped_image())
        uc.mem_map(BASE, IMG_SIZE, UC_PROT_ALL)
        uc.mem_write(BASE, img[:IMG_SIZE])
        arena_lo = rup(BASE + IMG_SIZE, 0x1000)
        arena_hi = rup(self.meta['bump'] + 0x4000000, 0x1000)
        uc.mem_map(arena_lo, arena_hi - arena_lo, UC_PROT_ALL)
        for a, off in self.meta['pages']:
            uc.mem_write(a, self.bin[off:off + 4096])
        # synthetic import / host-function / sentinel ranges: `ret` bytes, hooks do the work
        uc.mem_map(IMPORT_BASE, 0x10000, UC_PROT_ALL); uc.mem_write(IMPORT_BASE, b'\xc3' * 0x10000)
        uc.mem_map(HOST_BASE, 0x100000, UC_PROT_ALL); uc.mem_write(HOST_BASE, b'\xc3' * 0x100000)
        uc.mem_map(SENT & ~0xfff, 0x1000, UC_PROT_ALL); uc.mem_write(SENT & ~0xfff, b'\xc3' * 0x1000)
        # IAT
        for im in self.imps:
            self.w64(im.slot, IMPORT_BASE + 16 * im.idx)
        self.setreg(UC_X86_REG_GS_BASE, self.meta['teb'])
        # CPU-identity sites of the exe (cpuid / xgetbv): replaced by the fixed profile, like the lifter does
        self.cpu_sites = {BASE + 4433123: ('cpuid', 2), BASE + 4433160: ('cpuid', 2), BASE + 4433290: ('cpuid', 2), BASE + 4433369: ('xgetbv', 3), BASE + 4489561: ('cpuid', 2)}
        uc.hook_add(UC_HOOK_CODE, self.on_code_import, None, IMPORT_BASE, IMPORT_BASE + 0x10000 - 1)
        uc.hook_add(UC_HOOK_CODE, self.on_code_host, None, HOST_BASE, HOST_BASE + 0x100000 - 1)
        uc.hook_add(UC_HOOK_CODE, self.on_code_cont, None, CONT, CONT)
        for a in self.cpu_sites:
            uc.hook_add(UC_HOOK_CODE, self.on_cpu, None, a, a)
        self.state = None
        uc.hook_add(UC_HOOK_CODE, self.on_manage_run, None, BASE + 0x579bf0, BASE + 0x579bf0)

    # ---- allocator (mirrors core.c deterministic mode)
    def ga_alloc(self, n):
        n = n or 1
        b = self.bump; self.bump += 16 + rup(n, 16)
        self.w32(b, 0); self.w32(b + 4, 0xA110C8ED); self.w64(b + 8, n)        # block header like core.c (cls, magic, size)
        return b + 16
    def strdup(self, s):
        p = self.ga_alloc(len(s) + 1); self.w(p, s + b'\0'); return p
    def errno_ptr(self):
        teb = self.meta['teb']; e = self.r64(teb + 0x700)
        if not e:
            e = self.ga_alloc(16); self.w64(e, 0); self.w64(teb + 0x700, e)
        return e
    def new_handle(self, kind):
        h = 0x100 + 4 * self.next_h; self.next_h += 1; self.handles[h] = kind; return h

    def page_crcs(self, stack):
        import zlib
        out = {}
        arena_lo = self.meta['arena_lo'] & ~0xfff
        for lo, hi in ((BASE + 0x713000, BASE + 0x2b83000), (BASE + 0x2bd4000, BASE + 0x2bd5000), (arena_lo, (self.bump + 0xfff) & ~0xfff)):
            for a in range(lo, hi, 4096):
                if lo == arena_lo and stack[0] <= a < stack[1]: continue
                out[a] = zlib.crc32(self.r(a, 4096)) & 0xffffffff
        return out

    def on_manage_run(self, uc, addr, size, ud):
        # the original exe reaches the ManageRun .nep unit here (the lifted run reaches its native-shell replacement at the same point)
        stack_hi = (self.meta['rsp'] + 0x80 + 0xfff) & ~0xfff
        self.state = dict(calls=len(self.trace), crcs=self.page_crcs((stack_hi - 0x800000, stack_hi)))
        self.state['pages'] = {a: self.r(a, 4096) for a in range((self.meta['arena_lo'] & ~0xfff), (self.bump + 0xfff) & ~0xfff, 4096) if not ((stack_hi - 0x800000) <= a < stack_hi)}
        self.done = True; uc.emu_stop()

    # ---- cpuid / xgetbv
    def on_cpu(self, uc, addr, size, ud):
        kind, ln = self.cpu_sites[addr]
        if kind == 'xgetbv':
            self.setreg(UC_X86_REG_RAX, 7); self.setreg(UC_X86_REG_RDX, 0)
        else:
            leaf = self.reg(UC_X86_REG_RAX) & 0xffffffff; sub = self.reg(UC_X86_REG_RCX) & 0xffffffff
            ECX1 = sum(1 << b for b in (0, 1, 9, 12, 13, 19, 20, 22, 23, 25, 26, 27, 28, 29)); EDX1 = 0xBFEBFBFF; EBX7 = (1 << 3) | (1 << 5) | (1 << 8) | (1 << 9)
            a = b = c = d = 0
            if leaf == 0: a = 0x16; b = 0x756e6547; d = 0x49656e69; c = 0x6c65746e
            elif leaf == 1: a = 0x906EA; b = 0x00100800; c = ECX1; d = EDX1
            elif leaf == 7 and sub == 0: b = EBX7
            elif leaf == 0x80000000: a = 0x80000008
            elif leaf == 0x80000001: c = (1 << 0) | (1 << 5) | (1 << 8); d = (1 << 11) | (1 << 20) | (1 << 27) | (1 << 29)
            elif 0x80000002 <= leaf <= 0x80000004:
                brand = b'KHNMC lifted x86-64 AVX2 profile'.ljust(48, b'\0'); o = 16 * (leaf - 0x80000002)
                a, b, c, d = struct.unpack('<IIII', brand[o:o + 16])
            self.setreg(UC_X86_REG_RAX, a); self.setreg(UC_X86_REG_RBX, b); self.setreg(UC_X86_REG_RCX, c); self.setreg(UC_X86_REG_RDX, d)
        self.setreg(UC_X86_REG_RIP, addr + ln)

    # ---- callbacks into guest code (state machine frames)
    def start_callee(self, fn, a0=0):
        """push a call to guest fn that returns to CONT"""
        f = self.frames[-1]
        sp = (f['sp0'] - 0x100) & ~0xF
        self.w64(sp + 0x20, 0)
        sp -= 8; self.w64(sp, CONT)
        self.setreg(UC_X86_REG_RSP, sp)
        self.setreg(UC_X86_REG_RCX, a0); self.setreg(UC_X86_REG_RDX, 0); self.setreg(UC_X86_REG_R8, 0); self.setreg(UC_X86_REG_R9, 0)
        self.pending = fn; self.uc.emu_stop()

    def finish_frame(self):
        f = self.frames.pop()
        self.setreg(UC_X86_REG_RSP, f['sp0'])
        for R, v in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), f['args']): self.setreg(R, v)
        if 'rax' in f: self.setreg(UC_X86_REG_RAX, f['rax'])
        if 'post' in f: f['post']()
        self.pending = f['stub'] + 8; self.uc.emu_stop()      # the `ret` at the import address pops the original return address

    def advance(self):
        f = self.frames[-1]
        if f['kind'] in ('initterm', 'initterm_e'):
            if f['kind'] == 'initterm_e' and f.get('started'):
                rv = self.reg(UC_X86_REG_RAX) & 0xffffffff
                f['rv'] = rv
                if rv: f['p'] = f['end']
            while f['p'] < f['end']:
                fn = self.r64(f['p']); f['p'] += 8
                if fn:
                    f['started'] = True; self.start_callee(fn); return
            if f['kind'] == 'initterm_e': f['rax'] = f.get('rv', 0)
            self.finish_frame(); return
        if f['kind'] == 'ctxinit':
            if not f.get('called'):
                f['called'] = True; self.start_callee(f['fn'], f['blk'] + 16); return
            self.w64(f['blk'] + 8, 1); f['rax'] = f['blk'] + 16
            self.finish_frame(); return
        raise RuntimeError('bad frame')

    def on_code_cont(self, uc, addr, size, ud):
        self.advance()

    # ---- import stubs
    def on_code_import(self, uc, addr, size, ud):
        if self.err: return
        idx = (addr - IMPORT_BASE) >> 4
        if addr & 15 == 8: return          # continuation `ret` (second slot of an import): pops the original return address
        if addr & 15 or idx >= len(self.names):
            self.err = 'jump into the middle of the import range: %x' % addr; uc.emu_stop(); return
        name = self.names[idx]
        rsp = self.reg(UC_X86_REG_RSP)
        ret = self.r64(rsp)
        args = [self.reg(R) for R in (UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9)]
        if len(self.trace) >= self.max:
            self.done = True; uc.emu_stop(); return
        self.trace.append((name, args, (ret - BASE) if BASE <= ret < BASE + IMG_SIZE else ret))
        h = STUBS.get(name)
        if h is None:
            self.err = 'no oracle stub for import %s (call #%d)' % (name, len(self.trace) - 1); uc.emu_stop(); return
        h(self, addr, args)

    def on_code_host(self, uc, addr, size, ud):
        idx = (addr - HOST_BASE) >> 4
        nm = self.meta['hostfn'][idx] if idx < len(self.meta['hostfn']) else None
        args = [self.reg(R) for R in (UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9)]
        if nm is None or not nm.startswith('com:steam:'):
            self.err = 'unexpected host-function call %s' % nm; uc.emu_stop(); return
        cls, slot = nm[len('com:steam:'):].rsplit('#', 1); slot = int(slot)
        rax = None
        if cls == 'ISteamUser023' and slot == 2: self.w64(args[1], 0x0110000100000001); rax = args[1]
        elif cls == 'ISteamInput006' and slot == 0: rax = 1
        elif cls == 'ISteamApps008' and slot == 4: rax = self.meta['steam'][8]
        elif cls == 'ISteamUserStats012' and slot == 0: rax = 1
        elif cls == 'ISteamFriends017' and slot == 43: rax = 1
        else: rax = 0
        self.trace.append(('host:' + nm, args, 0))
        self.setreg(UC_X86_REG_RAX, rax)

    def run(self):
        uc = self.uc
        rsp = self.meta['rsp']
        # TLS callbacks (DLL_PROCESS_ATTACH) like loader.c, then the CRT entry
        pe = pefile.PE(EXE, fast_load=True)
        tls = pe.OPTIONAL_HEADER.DATA_DIRECTORY[9].VirtualAddress
        cbs = struct.unpack('<Q', bytes(uc.mem_read(BASE + tls + 24, 8)))[0]
        self.setreg(UC_X86_REG_RSP, rsp)
        calls = []
        p = cbs
        while True:
            cb = self.r64(p)
            if not cb: break
            calls.append((cb, GS_REASON)); p += 8
        for cb, reason in calls:
            self.guest_call(cb, BASE, reason, 0, 0)
        self.guest_call_start(BASE + 0x4393d0)
        rip = self.reg(UC_X86_REG_RIP)
        while True:
            try:
                uc.emu_start(rip, SENT)
            except UcError as e:
                self.err = self.err or ('unicorn error %s at rip=%x rsp=%x' % (e, self.reg(UC_X86_REG_RIP), self.reg(UC_X86_REG_RSP)))
                break
            if self.pending is not None:
                rip = self.pending; self.pending = None; continue
            break
        return self.trace, self.err

    def guest_call_start(self, fn):
        sp = (self.reg(UC_X86_REG_RSP) - 0x100) & ~0xF
        self.w64(sp + 0x20, 0); sp -= 8; self.w64(sp, SENT)
        self.setreg(UC_X86_REG_RSP, sp)
        for R in (UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9): self.setreg(R, 0)
        self.setreg(UC_X86_REG_RIP, fn)

    def guest_call(self, fn, a0, a1, a2, a3):
        save = [self.reg(R) for R in (UC_X86_REG_RSP, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9)]
        sp = (save[0] - 0x100) & ~0xF
        self.w64(sp + 0x20, 0); sp -= 8; self.w64(sp, SENT)
        self.setreg(UC_X86_REG_RSP, sp)
        for R, v in zip((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), (a0, a1, a2, a3)): self.setreg(R, v)
        self.uc.emu_start(fn, SENT)
        for R, v in zip((UC_X86_REG_RSP, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9), save): self.setreg(R, v)

GS_REASON = 1

# ======================================================================================================= stubs (mirror runtime/*.c, KH_DET=1)
def rax(o, v): o.setreg(UC_X86_REG_RAX, v)
def s_zero(o, a, args): rax(o, 0)
def s_one(o, a, args): rax(o, 1)
def s_none(o, a, args): pass
def s_systime(o, a, args): o.tick += 1; o.w64(args[0], 0x01DBA00000000000 + o.tick * 10000)
def s_qpc(o, a, args): o.tick += 1; o.w64(args[0], (1000000000 + o.tick * 100000) // 100); rax(o, 1)
def s_qpf(o, a, args): o.w64(args[0], 10000000); rax(o, 1)
def s_tid(o, a, args): rax(o, 1000)
def s_pid(o, a, args): rax(o, 4242)
def s_initterm(kind):
    def h(o, a, args):
        sp0 = o.reg(UC_X86_REG_RSP)
        o.frames.append(dict(kind=kind, p=args[0], end=args[1], sp0=sp0, args=args, stub=a))
        o.advance()
    return h
def s_commode(o, a, args):
    if not o.p_commode: o.p_commode = o.ga_alloc(16); o.w32(o.p_commode, 0)
    rax(o, o.p_commode)
def s_slist(o, a, args): o.w(args[0], b'\0' * 16)
def s_cs_init(r):
    def h(o, a, args):
        o.w(args[0], b'\0' * 0x28); rax(o, r) if r is not None else None
    return h
def s_cs_enter(o, a, args):
    cs = args[0]; owner = o.r64(cs + 0x10)
    if owner == 1000: o.w32(cs + 0xC, o.r32(cs + 0xC) + 1)
    else: o.w64(cs + 0x10, 1000); o.w32(cs + 0xC, 1)
def s_cs_leave(o, a, args):
    cs = args[0]; r = o.r32(cs + 0xC)
    if r > 1: o.w32(cs + 0xC, r - 1)
    else: o.w32(cs + 0xC, 0); o.w64(cs + 0x10, 0)
def dll_index(name):
    b = name.lower().split('\\')[-1]
    return KNOWN_DLLS.index(b) + 1 if b in KNOWN_DLLS else 0
def s_getmodulehandle(o, a, args):
    if not args[0]: rax(o, BASE); return
    i = dll_index(o.wstr(args[0])); rax(o, 0x7ffa00000000 + 0x10000 * i if i else 0)
def s_getprocaddress(o, a, args): rax(o, 0)     # no runtime implementation for the probed kernel32 names (condition variables)
def s_free(o, a, args):
    if args[0]: o.w32(args[0] - 12, 0xF4EEF4EE)          # deterministic ga_free marks the block header
def s_malloc(o, a, args): rax(o, o.ga_alloc(args[0]))
def s_memset(o, a, args): o.w(args[0], bytes([args[1] & 0xff]) * args[2]) if args[2] else None; rax(o, args[0])
def s_memcpy(o, a, args):
    if args[2]: o.w(args[0], o.r(args[1], args[2]))
    rax(o, args[0])
def s_memcmp(o, a, args):
    x = o.r(args[0], args[2]) if args[2] else b''; y = o.r(args[1], args[2]) if args[2] else b''
    rax(o, 0 if x == y else (0xffffffff if x < y else 1))
def s_strstr(o, a, args):
    h = o.cstr(args[0]); n = o.cstr(args[1]); i = h.find(n); rax(o, args[0] + i if i >= 0 else 0)
def s_strrchr(o, a, args):
    s = o.cstr(args[0]); c = args[1] & 0xff
    if c >= 128: c = c  # (int)(s8) compare is on char value; ASCII only in the prefix
    if c == 0: rax(o, args[0] + len(s)); return
    i = s.rfind(bytes([c])); rax(o, args[0] + i if i >= 0 else 0)
def s_strncpy(o, a, args):
    d, s, n = args[0], o.cstr(args[1]), args[2]
    b = (s[:n]).ljust(n, b'\0')
    if n: o.w(d, b)
    rax(o, d)
def s_strncpy_s(o, a, args):
    d, dsz, s, cnt = args[0], args[1], o.cstr(args[2]), args[3]
    if not d or not dsz: rax(o, 22); return
    L = len(s) if cnt == M64 else min(len(s), cnt)
    if L >= dsz:
        if cnt == M64: o.w(d, b'\0'); rax(o, 34); return
        L = dsz - 1
    o.w(d, s[:L] + b'\0'); rax(o, 0)
def s_getmodfile(o, a, args):
    p = b'C:\\KH2\\KINGDOM HEARTS II FINAL MIX.exe'; buf, n = args[1], args[2]
    if len(p) + 1 > n:
        if n: o.w(buf, p[:n - 1] + b'\0')
        rax(o, n); return
    o.w(buf, p + b'\0'); rax(o, len(p))
def s_cmdline(o, a, args):
    if not o.cmdline: o.cmdline = o.strdup(b'"C:\\KH2\\KINGDOM HEARTS II FINAL MIX.exe" ')
    rax(o, o.cmdline)
def s_getstartup(o, a, args): o.w(args[0], b'\0' * 0x68); o.w32(args[0], 0x68)
def fmt_simple(o, fmt, va):
    out = b''; i = 0; slot = 0
    while i < len(fmt):
        c = fmt[i:i + 1]
        if c != b'%': out += c; i += 1; continue
        if fmt[i:i + 2] == b'%s':
            p = o.r64(va + 8 * slot); slot += 1; out += o.cstr(p) if p else b'(null)'; i += 2
        elif fmt[i:i + 2] == b'%%': out += b'%'; i += 2
        else: raise RuntimeError('oracle printf: unsupported format %r' % fmt)
    return out
def s_vsprintf(o, a, args):
    buf, cnt, fmt = args[1], args[2], o.cstr(args[3]); va = o.arg(5)
    try: s = fmt_simple(o, fmt, va)
    except RuntimeError as e: o.err = str(e); o.uc.emu_stop(); return
    need = len(s); n = need if need < cnt else (cnt - 1 if cnt else 0)
    if cnt: o.w(buf, s[:n] + b'\0')
    opts = args[0]
    ret = need if (opts & 2) else (0xffffffff if (cnt and need >= cnt) else need)
    rax(o, ret)
def s_sopen(o, a, args):
    en = 2; e = o.errno_ptr(); o.w32(e, en); sec = o.arg(5) & 0xffffffff; pfh = o.arg(4)
    if sec:
        if pfh: o.w32(pfh, 0xffffffff)
        rax(o, en)
    else: rax(o, 0xffffffff)
def s_get_errno(o, a, args):
    e = o.errno_ptr()
    if args[0]: o.w32(args[0], o.r32(e))
    rax(o, 0)
def s_mkdir(o, a, args): rax(o, 0)
def s_handle(kind):
    def h(o, a, args): rax(o, o.new_handle(kind))
    return h
def s_setevent(o, a, args): rax(o, 1 if o.handles.get(args[0]) == 'event' else 0)
def s_steam_ctx(o, a, args):
    sp0 = o.reg(UC_X86_REG_RSP); blk = args[0]
    if o.r64(blk + 8) != 1:
        fn = o.r64(blk)
        o.frames.append(dict(kind='ctxinit', blk=blk, fn=fn, sp0=sp0, args=args, stub=a)); o.advance()
    else: rax(o, blk + 16)
def s_steam_find(o, a, args):
    v = o.cstr(args[1]); st = o.meta['steam']
    for pre, i in ((b'SteamUser', 0), (b'SteamFriends', 1), (b'SteamUtils', 2), (b'STEAMREMOTESTORAGE', 3), (b'STEAMUSERSTATS', 4), (b'SteamInput', 5), (b'STEAMAPPS', 6)):
        if v.startswith(pre): rax(o, st[i]); return
    rax(o, st[7])

import math
def xmm_f32(o, i): return struct.unpack('<f', struct.pack('<I', o.reg(UC_X86_REG_XMM0 + i) & 0xffffffff))[0]
def xmm_f64(o, i): return struct.unpack('<d', struct.pack('<Q', o.reg(UC_X86_REG_XMM0 + i) & M64))[0]
def put_xmm(o, bits, nbytes): o.uc.reg_write(UC_X86_REG_XMM0, bits & ((1 << (8 * nbytes)) - 1))        # upper bits zero, like `v128 o=VZ; memcpy(&o,...)`
def f32bits(x):
    try: return struct.unpack('<I', struct.pack('<f', x))[0]
    except OverflowError: return 0x7f800000 if x > 0 else 0xff800000
def f64bits(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
def safe(fn, *a):
    try: return fn(*a)
    except (ValueError, OverflowError, ZeroDivisionError): return float('nan')
def mk_f1(fn):
    def h(o, a, args): put_xmm(o, f32bits(safe(fn, xmm_f32(o, 0))), 4)
    return h
def mk_f2(fn):
    def h(o, a, args): put_xmm(o, f32bits(safe(fn, xmm_f32(o, 0), xmm_f32(o, 1))), 4)
    return h
def mk_d1(fn):
    def h(o, a, args): put_xmm(o, f64bits(safe(fn, xmm_f64(o, 0))), 8)
    return h
def mk_d2(fn):
    def h(o, a, args): put_xmm(o, f64bits(safe(fn, xmm_f64(o, 0), xmm_f64(o, 1))), 8)
    return h
M = 'api_ms_win_crt_math_l1_1_0!'
MATH = {
    M + 'sinf': mk_f1(math.sin), M + 'cosf': mk_f1(math.cos), M + 'tanf': mk_f1(math.tan), M + 'asinf': mk_f1(math.asin), M + 'acosf': mk_f1(math.acos), M + 'atanf': mk_f1(math.atan),
    M + 'expf': mk_f1(math.exp), M + 'logf': mk_f1(math.log), M + 'log10f': mk_f1(math.log10), M + 'floorf': mk_f1(math.floor), M + 'ceilf': mk_f1(math.ceil), M + 'sqrtf': mk_f1(math.sqrt),
    M + 'roundf': mk_f1(lambda x: math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5)),
    M + 'atan2f': mk_f2(math.atan2), M + 'powf': mk_f2(math.pow), M + 'fmodf': mk_f2(math.fmod),
    M + 'sin': mk_d1(math.sin), M + 'cos': mk_d1(math.cos), M + 'atan': mk_d1(math.atan), M + 'acos': mk_d1(math.acos), M + 'exp': mk_d1(math.exp), M + 'log': mk_d1(math.log),
    M + 'floor': mk_d1(math.floor), M + 'ceil': mk_d1(math.ceil), M + 'sqrt': mk_d1(math.sqrt), M + 'pow': mk_d2(math.pow),
}
STUBS = {
    'kernel32!GetSystemTimeAsFileTime': s_systime, 'kernel32!GetCurrentThreadId': s_tid, 'kernel32!GetCurrentProcessId': s_pid,
    'kernel32!QueryPerformanceCounter': s_qpc, 'kernel32!QueryPerformanceFrequency': s_qpf,
    'api_ms_win_crt_runtime_l1_1_0!_initterm': s_initterm('initterm'), 'api_ms_win_crt_runtime_l1_1_0!_initterm_e': s_initterm('initterm_e'),
    'api_ms_win_crt_runtime_l1_1_0!_set_app_type': s_zero, 'api_ms_win_crt_stdio_l1_1_0!_set_fmode': s_zero,
    'api_ms_win_crt_stdio_l1_1_0!__p__commode': s_commode, 'api_ms_win_crt_runtime_l1_1_0!_crt_atexit': s_zero,
    'api_ms_win_crt_runtime_l1_1_0!_configure_narrow_argv': s_zero, 'api_ms_win_crt_runtime_l1_1_0!_initialize_narrow_environment': s_zero,
    'api_ms_win_crt_locale_l1_1_0!_configthreadlocale': s_zero, 'api_ms_win_crt_heap_l1_1_0!_set_new_mode': s_zero,
    'kernel32!InitializeSListHead': s_slist, 'kernel32!InitializeCriticalSection': s_cs_init(0), 'kernel32!InitializeCriticalSectionAndSpinCount': s_cs_init(1),
    'kernel32!InitializeCriticalSectionEx': s_cs_init(1), 'kernel32!EnterCriticalSection': s_cs_enter, 'kernel32!LeaveCriticalSection': s_cs_leave,
    'kernel32!GetModuleHandleW': s_getmodulehandle, 'kernel32!GetProcAddress': s_getprocaddress, 'kernel32!SetUnhandledExceptionFilter': s_zero,
    'api_ms_win_crt_heap_l1_1_0!malloc': s_malloc, 'api_ms_win_crt_heap_l1_1_0!free': s_free,
    'vcruntime140!memset': s_memset, 'vcruntime140!memcpy': s_memcpy, 'vcruntime140!memmove': s_memcpy, 'vcruntime140!memcmp': s_memcmp,
    'vcruntime140!strstr': s_strstr, 'vcruntime140!strrchr': s_strrchr, 'api_ms_win_crt_string_l1_1_0!strncpy': s_strncpy, 'api_ms_win_crt_string_l1_1_0!strncpy_s': s_strncpy_s,
    'kernel32!GetModuleFileNameA': s_getmodfile, 'api_ms_win_crt_runtime_l1_1_0!_get_narrow_winmain_command_line': s_cmdline, 'kernel32!GetStartupInfoW': s_getstartup,
    'api_ms_win_crt_stdio_l1_1_0!__stdio_common_vsprintf': s_vsprintf, 'api_ms_win_crt_stdio_l1_1_0!_sopen_dispatch': s_sopen, 'api_ms_win_crt_runtime_l1_1_0!_get_errno': s_get_errno,
    'api_ms_win_crt_filesystem_l1_1_0!_mkdir': s_mkdir,
    'kernel32!CreateSemaphoreA': s_handle('sem'), 'kernel32!CreateMutexA': s_handle('mutex'), 'kernel32!CreateEventW': s_handle('event'),
    'kernel32!SetEvent': s_setevent, 'kernel32!ResetEvent': s_setevent,
    'steam_api64!SteamAPI_RestartAppIfNecessary': s_zero, 'steam_api64!SteamAPI_Init': s_one, 'steam_api64!SteamAPI_GetHSteamUser': s_one,
    'steam_api64!SteamInternal_ContextInit': s_steam_ctx, 'steam_api64!SteamInternal_FindOrCreateUserInterface': s_steam_find,
    'ole32!CoInitialize': s_zero, 'mfplat!MFStartup': s_zero,
}
STUBS.update(MATH)


def parse_lifted(path):
    rows = []
    rx = re.compile(r'^(\d+) t(\d+) (\S+?)\(([0-9a-f]*),([0-9a-f]*),([0-9a-f]*),([0-9a-f]*)\) <- ([0-9a-f]+)')
    for ln in open(path):
        m = rx.match(ln)
        if not m: continue
        rows.append((int(m.group(2)), m.group(3), [int(m.group(i), 16) for i in (4, 5, 6, 7)], int(m.group(8), 16)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snap', required=True); ap.add_argument('--state', help='lifted run KH_STATE_DUMP file to compare memory with'); ap.add_argument('--lifted', required=True); ap.add_argument('--out', required=True); ap.add_argument('--max', type=int, default=100000)
    a = ap.parse_args()
    lifted = [r for r in parse_lifted(a.lifted) if r[0] == 1000]
    # only the main thread before the first thread is created
    cut = len(lifted)
    for i, r in enumerate(lifted):
        if 'beginthread' in r[1] or 'CreateThread' in r[1]: cut = i + 1; break
    lifted = lifted[:cut]
    o = Oracle(a.snap, min(a.max, len(lifted)))
    trace, err = o.run()
    n = len(trace); first = None
    # host-function (COM thunk) calls are not in the lifted import trace: drop them for the comparison
    imp_trace = [t for t in trace if not t[0].startswith('host:')]
    n = min(len(imp_trace), len(lifted)); first = None
    for i in range(n):
        ln, la, lr = lifted[i][1], lifted[i][2], lifted[i][3]
        on, oa, orr = imp_trace[i]
        nrm = lambda v: 'HOST' if v in (SENT, CONT) else v      # return into the host caller: the runtime's sentinel vs the oracle's continuation slot
        if ln != on or la != oa or nrm(lr) != nrm(orr):
            first = dict(index=i, lifted=dict(name=ln, args=[hex(x) for x in la], ret=hex(lr)), oracle=dict(name=on, args=[hex(x) for x in oa], ret=hex(orr)),
                         context=[dict(i=j, lifted=lifted[j][1], oracle=imp_trace[j][0]) for j in range(max(0, i - 3), min(n, i + 2))])
            break
    rep = dict(lifted_calls_available=len(lifted), oracle_calls_emulated=len(imp_trace), compared=n if first is None else first['index'],
               identical_prefix=(first is None), first_divergence=first, oracle_stopped_because=err,
               imports_in_compared_prefix=sorted({lifted[i][1] for i in range(n if first is None else first['index'])}))
    if a.state and o.state:
        L = {}
        st = None
        for ln in open(a.state):
            p = ln.split()
            if p[0] == 'stack': st = (int(p[1], 16), int(p[2], 16)); continue
            L[int(p[0], 16)] = int(p[1], 16)
        if st: pass
        O = o.state['crcs']
        diff = sorted(a_ for a_ in set(L) | set(O) if L.get(a_) != O.get(a_))
        diffbytes = []
        hb = a.state + '.heap'
        if os.path.exists(hb):
            raw = open(hb, 'rb').read(); i = 0
            while i < len(raw):
                pa = struct.unpack('<Q', raw[i:i + 8])[0]; pg = raw[i + 8:i + 8 + 4096]; i += 8 + 4096
                if pa in diff:
                    og = o.state['pages'].get(pa)
                    if og is not None:
                        d = [j for j in range(4096) if pg[j] != og[j]]
                        diffbytes.append(dict(page=hex(pa), nbytes=len(d), first=[(hex(pa + j), pg[j], og[j]) for j in d[:12]]))
        rep['memory_compare'] = dict(diff_bytes=diffbytes, oracle_calls_at_manage_run=o.state['calls'], pages_compared=len(set(L) & set(O)), pages_only_lifted=len(set(L) - set(O)), pages_only_oracle=len(set(O) - set(L)),
                                     pages_different=len([x for x in diff if x in L and x in O]), first_different_pages=[hex(x) for x in diff[:20]])
    json.dump(rep, open(a.out, 'w'), indent=1)
    print(json.dumps({k: v for k, v in rep.items() if k != 'imports_in_compared_prefix'}, indent=1))
    print('distinct imports compared: %d' % len(rep['imports_in_compared_prefix']))

if __name__ == '__main__':
    main()
