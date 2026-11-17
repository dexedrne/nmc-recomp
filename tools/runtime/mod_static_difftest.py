#!/usr/bin/env python3
"""Differential test of the re-lifted mod functions against the PATCHED bytes, on the lifter's native oracle (Task 12).

  mod_static_difftest.py --khlift DIR --edits edit_list.json --exe kh2fm.exe --funcs "RVA ..." --chained "RVA ..."
        --vanilla-shards lifted/shards --lifted gen/mod_static_lifted.c --sites runtime/mod_static_sites.c
        --work DIR --out result.json [--tests 48] [--mid-tests 12] [--blk-tests 16] [--native-ms 20] [--jobs 4] [--seed 1234]
        [--only RVA,RVA] [--mutate-vanilla]      (debug: MSDT_DEBUG=<seconds>|trace, MSDT_ONE=<block hex>:<seed>)

Method (the lifter's tools/difftest.py --oracle native, reused: test generation, state compare, image diff, INT3 callee stubs):
  oracle  the patched image is mapped at its own address and the function's ORIGINAL (patched) x86 bytes run on this CPU;
          every other function start is INT3 and gets the deterministic stub effect;
  lifted  the exact text of gen/mod_static_lifted.c (checked byte for byte against a fresh lift), compiled with the shard flags;
  compare all 16 GPRs, xmm/ymm, the arena, the whole stack, every changed image byte (incl. the mod buffer), lazily mapped pages.
Additions needed for these functions (they sit deep in game logic; a random function-entry test rarely survives to the edit):
  * stubs return arena pointers most of the time and their effect depends on the argument registers they are called with
    (tools/mod_static_teststub.c), both sides use the same stub function;
  * a quarter of the test registers and some stub results are constants the function itself compares against (+-1);
  * MID-FUNCTION entries: every lifted block is also entered directly (native: jump to the block address with the stack pointer
    lowered by the block's static frame depth; lifted: an instrumented copy of the same text with an entry switch), flags cleared
    on both sides;
  * SINGLE-BLOCK tests: the same entry, but both sides stop at the first block boundary (oracle: INT3 on every other block
    start of the function; lifted: the instrumented copy returns there).  Compared: the full state at the boundary AND which
    block was reached, so an edited branch condition is checked on both outcomes even when no complete path through it survives;
  * globals the function names by constant address that are zero in the image (BSS pointers, tables) are seeded per test, the
    same on both sides;
  * the VEX `bt reg,reg` scratch store (see implementation notes): the lifted code writes the operand at [rsp-288], the original does
    not; exactly those bytes are excused in the stack compare and the tests that needed it are counted;
  * block traces of the instrumented copy give coverage per edit: how many VALID, MATCHING tests executed the block that holds
    each edited instruction, and which successors were taken.
  * the two inline detour sites: the oracle traps at the site and runs the same C handler as the lifted code
    (tools/mod_static_oracle.c).  No DLL code is executed.
Not counted as valid tests (each is a named skip in the result file): the oracle faulted or ran past its CPU-time limit; a
single-block test entered a jump-table dispatch block behind the function's bounds check with an index outside the table (the
lifted switch faults by policy, the oracle jumps wild); a run that changed more than 64 image pages.  The arena and the stack are
mapped without execute permission, so a wild jump into test data faults instead of running random bytes.
--mutate-vanilla is the sensitivity check: the VANILLA lifted text of the same functions against the patched oracle; every edit
whose block then shows a single-block mismatch is an edit this test can see.
Exit status: 0 all functions match and every edit is covered; 1 mismatch / crash / stale generated file; 3 match but an edit or
site was never executed by a valid test (reported, not hidden).  With --mutate-vanilla: 0 when every function mismatches, else 4.
"""
import sys, os, re, json, struct, ctypes, mmap, subprocess, argparse, collections, signal, hashlib, time, bisect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mod_static_lift as MSL

SLOT_PAGE = 0x30010000           # the dual-wield slot block of the site handler (outside the arena, never overwritten by a test)

def die(msg):
    sys.stderr.write('mod_static_difftest: ERROR: %s\n' % msg); sys.exit(1)

def gen_texts(path):
    """function texts of the generated C file -> {symbol: text}"""
    body = open(path).read()
    m = list(re.finditer(r'^void ((?:mod_)?f_[0-9a-f]+)\(KCtx \*g\)\{$', body, re.M))
    out = {}
    tail = body.index('\n/* {rva, replacement')
    for i, x in enumerate(m):
        e = m[i + 1].start() if i + 1 < len(m) else tail
        out[x.group(1)] = body[x.start():e].rstrip('\n') + '\n'
    return out

def stack_depths(K, image, fn, site):
    """-> (depth, bt sites, None) or (None, None, why).  depth: static frame depth (bytes pushed/reserved since function entry) at
    every reachable instruction; bt sites: [(va, depth, operand bytes)] of bt/bts/btr/btc reg,reg."""
    import capstone
    from capstone import x86_const as X
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); cs.detail = True
    BASE = K.BASE; depth = {}; work = [(fn.lo, 0)]; bts = []
    def indata(a): return any(lo <= a < hi for lo, hi, k in fn.data)
    while work:
        a, d = work.pop(); alias = {}                     # register -> depth rsp would have if loaded from it (lea rX,[rsp+N])
        while True:
            if not (fn.lo <= a < fn.hi) or indata(a): break
            if a in depth:
                if depth[a] != d: return None, None, 'two depths at %x (%d, %d)' % (a, depth[a], d)
                break
            depth[a] = d
            if site and a == BASE + site['site']:
                work.append((BASE + site['ok'], d)); work.append((BASE + site['skip'], d)); break
            ins = next(cs.disasm(image[a - BASE:a - BASE + 16], a, 1), None)
            if ins is None: return None, None, 'decode at %x' % a
            m = ins.mnemonic; ops = ins.operands
            rsp0 = bool(ops) and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_RSP
            if m == 'leave': return None, None, 'leave at %x' % a
            if m in ('bt', 'bts', 'btr', 'btc') and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG:
                bts.append((a, d, ops[0].size))           # VEX lifts these through a scratch store at [rsp-288]
            if m == 'lea' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_MEM and ops[1].mem.base == X.X86_REG_RSP and ops[1].mem.index == 0:
                alias[ops[0].reg] = d - ops[1].mem.disp
            if m == 'push': d += 8
            elif m == 'pop': d -= 8
            elif rsp0 and m in ('sub', 'add') and ops[1].type == X.X86_OP_IMM: d += ops[1].imm if m == 'sub' else -ops[1].imm
            elif rsp0 and m == 'mov' and ops[1].type == X.X86_OP_REG and ops[1].reg in alias: d = alias[ops[1].reg]
            elif rsp0 and m not in ('cmp', 'test'): return None, None, 'rsp written by %s at %x' % (m, a)
            if m in ('ret', 'int3', 'ud2', 'hlt') or m.startswith('int'): break
            if m == 'jmp':
                if ops[0].type == X.X86_OP_IMM: work.append((ops[0].imm, d))
                else:
                    for t in fn.jtabs.get(a, ()): work.append((t, d))
                break
            if m[0] == 'j' or m.startswith('loop'): work.append((ops[0].imm, d))
            a += ins.size
    return depth, sorted(set(bts)), None

def compare_constants(K, G, rva, cap=192):
    """immediates of the function's non-branch instructions, patched AND vanilla bytes (u32): test registers and stub results are
    biased towards them (+-1), otherwise an edit like `cmp eax,7 -> cmp eax,0x4d` is executed but never decided differently."""
    import capstone
    from capstone import x86_const as X
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); cs.detail = True
    BASE = K.BASE; vals = set()
    for image, ext in ((G.pat, G.extents[rva]), (G.van, G.vextents[rva])):
        for lo, hi in ext.items():
            if hi <= lo: continue
            for ins in cs.disasm(image[lo - BASE:hi - BASE], lo):
                m = ins.mnemonic
                if m[0] == 'j' or m.startswith('call') or m.startswith('loop') or m in ('push', 'ret', 'lea'): continue
                for op in ins.operands:
                    if op.type == X.X86_OP_IMM and -0x80000000 <= op.imm <= 0xffffffff: vals.add(op.imm & 0xffffffff)
    vals = sorted(vals)
    if len(vals) > cap:
        import random
        vals = sorted(random.Random(rva).sample(vals, cap))
    return vals

def direct_360a5a(DT, t, rr):
    """Site 0x360a5a continues at its "ok" address only when the list word [rbx] is below di (signed) AND (the category byte
    [rax+6] equals dil OR the table word [rax+0xc] is 1): random states almost never satisfy that.  Half of the tests entered at
    the site get such a state (both alternatives), so the spliced block's ok exit is exercised at function level too."""
    A = DT.ARENA + rr.randrange(0x1000, 0x1f000, 16); B = DT.ARENA + rr.randrange(0x1000, 0x1f000, 16)
    di = rr.choice((1, 5, 0x100, 0x7fff)); w = rr.choice((0, 0x8000, 0xffff, di - 1))
    t['rdi'] = (t['rdi'] & ~0xffff) | di; t['rax'] = A; t['rbx'] = B
    a = t['arena']; struct.pack_into('<H', a, B - DT.ARENA, w)
    if rr.random() < 0.5: a[A - DT.ARENA + 6] = di & 0xff                                              # category equals dil
    else: a[A - DT.ARENA + 6] = (di + 1) & 0xff; struct.pack_into('<H', a, A - DT.ARENA + 0xc, 1)      # the mod's extension
SITE_DIRECTED = {0x360a5a: direct_360a5a}

def instrument(text, sym, rva, BASE, ids):
    """c_<rva>: same blocks with a trace call at each block start (which also ends a single-block test) and an entry switch
    (kh_test_entry = block va, 0 = function entry)."""
    head, body = text.split('\n', 1)
    if head != 'void %s(KCtx *g){' % sym: die('unexpected function head: %s' % head)
    labels = [int(x, 16) for x in re.findall(r'^L([0-9a-f]+):\{', body, re.M)]
    sw = 'switch(kh_test_entry){ %s default: break; }' % ' '.join('case 0x%xull: goto L%x;' % (BASE + a, a) for a in sorted(labels))
    body = re.sub(r'^L([0-9a-f]+):\{', lambda m: 'L%s:{ if(kh_cov(%d)) return;' % (m.group(1), ids[(rva, int(m.group(1), 16))]), body, flags=re.M)
    return 'void c_%x(KCtx *g){\n%s\n%s' % (rva, sw, body), labels

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--khlift', required=True); ap.add_argument('--edits', required=True); ap.add_argument('--exe', required=True)
    ap.add_argument('--funcs', required=True); ap.add_argument('--chained', default=''); ap.add_argument('--vanilla-shards', required=True)
    ap.add_argument('--lifted', required=True); ap.add_argument('--sites', required=True); ap.add_argument('--check-image')
    ap.add_argument('--work', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--tests', type=int, default=48); ap.add_argument('--mid-tests', type=int, default=12); ap.add_argument('--blk-tests', type=int, default=16)
    ap.add_argument('--jobs', type=int, default=4); ap.add_argument('--seed', type=int, default=1234); ap.add_argument('--only')
    ap.add_argument('--native-ms', type=int, default=20, help='CPU-time limit of one oracle run (a useful run takes microseconds; long loops over random data are cut here)')
    ap.add_argument('--mutate-vanilla', action='store_true', help='sensitivity check: test the VANILLA lifted text against the patched oracle; mismatches are expected')
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    K = MSL.setup(a.khlift, a.exe); BASE = K.BASE
    G = MSL.generate(K, a)
    # the generated file that is linked into khnmc must be exactly this lift
    shipped = gen_texts(a.lifted)
    for rva in G.want:
        name, r = G.out[rva]
        if shipped.get(name) != r.text: die('%s: %s differs from a fresh lift (stale generated file: run make)' % (a.lifted, name))
    if len(shipped) != len(G.want): die('%s has %d functions, expected %d' % (a.lifted, len(shipped), len(G.want)))
    # the text under test: the shipped re-lift, or (sensitivity check) the vanilla lift of the same functions
    if a.mutate_vanilla: TXT = {rva: ('f_%x' % rva, G.vres[rva]) for rva in G.want}; EXT = G.vextents
    else: TXT = {rva: G.out[rva] for rva in G.want}; EXT = G.extents
    import difftest as DT
    U = K.U; U._img = G.pat; U.image_size = lambda: len(G.pat)
    env = DT.Env(a.work, 'native')
    if env.IMGSZ != len(G.pat) or env.IMGSZ & 0xfff: die('image size')
    env.secs.append((G.nmc_rva, 0x1000, 0xC0000040)); env.wr_secs.append((G.nmc_rva, 0x1000))       # the mod buffer: RW data
    want = G.want if not a.only else [int(x, 16) for x in a.only.split(',')]
    # ---- test library: pristine t_<rva> (the shipped text, renamed) + instrumented c_<rva> + callee stubs
    ids = {}; blocks = {}
    for rva in G.want:
        for lab in re.findall(r'^L([0-9a-f]+):\{', TXT[rva][1].text, re.M): ids[(rva, int(lab, 16))] = len(ids)
    if len(ids) >= 65535: die('too many blocks for the trace buffer')
    id2 = {v: k for k, v in ids.items()}
    src = ['#include "khrt.h"', 'void kh_test_stub(KCtx *g, u64 va);', 'int kh_cov(u32 id); extern u64 kh_test_entry;']
    for frva, s in sorted(MSL.SITES.items()): src.append('int %s(KCtx *g);' % s['handler'])
    callees = set(); imports = set(); body = []
    for rva in G.want:
        name, r = TXT[rva]
        if (BASE + rva) in r.callees: die('0x%x is self-recursive: not supported by the stub model' % rva)
        callees |= r.callees; imports |= r.imports
        body.append(r.text.replace('void %s(KCtx *g){' % name, 'void t_%x(KCtx *g){' % rva, 1))
        ctext, labels = instrument(r.text, name, rva, BASE, ids); body.append(ctext); blocks[rva] = sorted(labels)
    sym2addr = {im.sym: im.addr for im in env.imps}
    csym = lambda va: G.env_p.callee_sym(va)
    for va in sorted(callees): src.append('void %s(KCtx *g);' % csym(va))
    for s in sorted(imports): src.append('void %s(KCtx *g);' % s)
    stubs = ['void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (csym(va), va) for va in sorted(callees)]
    stubs += ['void %s(KCtx *g){ kh_test_stub(g,0x%xull); }' % (s, sym2addr[s]) for s in sorted(imports)]
    code = '\n'.join(src + body + stubs) + '\n'
    h = hashlib.sha1(code.encode()).hexdigest()[:10]
    cpath = os.path.join(a.work, 'msdt_%s.c' % h); so = os.path.join(a.work, 'msdt_%s.so' % h)
    open(cpath, 'w').write(code)
    rt = os.path.join(a.khlift, 'rt')
    cmd = ['gcc', '-std=gnu11', '-O1', '-w', '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks', '-fno-math-errno', '-ffp-contract=off',
           '-shared', '-fPIC', '-I', rt, '-I', a.khlift, '-o', so, cpath, os.path.join(rt, 'khtest.c'), os.path.join(HERE, 'mod_static_teststub.c'),
           os.path.join(rt, 'khrt_cpu.c'), os.path.join(rt, 'khrt_rcp_tab.c'), os.path.join(HERE, 'mod_static_oracle.c'), a.sites]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: die('compile failed: ' + r.stderr[:3000])
    lib = ctypes.CDLL(os.path.abspath(so))
    lib.khtest_call.restype = ctypes.c_int
    lib.khtest_stub_total.restype = ctypes.c_uint64; lib.khtest_disp_total.restype = ctypes.c_uint64
    lib.khtest_config.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64, ctypes.c_uint64]
    lib.native_sentinel.restype = ctypes.c_uint64
    lib.native_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
    lib.native_config.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.kn_site_add.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p]
    DT.host_map(env)
    # test data is never code: a wild jump into the arena or the stack (random bytes) must fault at once instead of executing them
    libc0 = DT.libc; libc0.mprotect(DT.ARENA, DT.ARENA_SZ, 3); libc0.mprotect(DT.STACK, DT.STACK_SZ, 3)
    DT.hmap(SLOT_PAGE, 0x1000, struct.pack('<6I', *(ctypes.c_uint32 * 6).in_dll(lib, 'kh_mod_dual_slots_image')))
    ctypes.c_uint64.in_dll(lib, 'kh_mod_dual_slots_va').value = SLOT_PAGE
    ss = (ctypes.c_uint64 * len(env.stubset))(*env.stubset)
    lib.khtest_config(ctypes.cast(ss, ctypes.c_void_p), len(env.stubset), DT.ARENA, DT.ARENA + DT.ARENA_SZ)
    lib.native_config(ctypes.cast(ss, ctypes.c_void_p), len(env.stubset))
    sent = lib.native_sentinel()
    cov_buf = (ctypes.c_uint16 * 16384).in_dll(lib, 'kh_cov_buf'); cov_n = ctypes.c_uint32.in_dll(lib, 'kh_cov_n')
    entry_sel = ctypes.c_uint64.in_dll(lib, 'kh_test_entry')
    single = ctypes.c_int.in_dll(lib, 'kh_single'); single_id = ctypes.c_uint32.in_dll(lib, 'kh_single_entry_id')
    stop_id = ctypes.c_uint32.in_dll(lib, 'kh_stop_id'); stopped = ctypes.c_int.in_dll(lib, 'kh_stopped'); stop_pc = ctypes.c_uint64.in_dll(lib, 'kn_stop_pc')
    lib.kn_stop_add.argtypes = [ctypes.c_uint64]
    lib.khtest_values.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64]
    R64 = DT.R64; RSP0 = DT.RSP0; libc = DT.libc
    # image diff: DT.host_imgdiff copies and compares every writable section (38 MB of .data/.bss) per call; do one memcmp per
    # section against a pristine copy first and only fall back to the byte-exact scan when something changed
    ref_img = ctypes.create_string_buffer(env.IMG, len(env.IMG)); ref_addr = ctypes.addressof(ref_img)
    libc.memcmp.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]; libc.memcmp.restype = ctypes.c_int
    def imgdiff():
        """changed image bytes vs pristine, writable sections only (= DT.host_imgdiff, but memcmp-driven).  None when more than
        64 pages changed: a run that scribbled over the image (wild jump); the byte map would take minutes to build."""
        out = {}; npg = 0
        for rva, sz in env.wr_secs:
            if not libc.memcmp(BASE + rva, ref_addr + rva, sz): continue
            for off in range(0, sz, 0x10000):
                n = min(0x10000, sz - off)
                if not libc.memcmp(BASE + rva + off, ref_addr + rva + off, n): continue
                cur = ctypes.string_at(BASE + rva + off, n); ref = env.IMG[rva + off:rva + off + n]
                for pg in range(0, n, 4096):
                    if cur[pg:pg + 4096] != ref[pg:pg + 4096]:
                        npg += 1
                        if npg > 64: return None
                        for k in range(pg, min(n, pg + 4096)):
                            if cur[k] != ref[k]: out[BASE + rva + off + k] = cur[k]
        return out
    def restore(d):
        if d is None:
            for rva, sz in env.wr_secs: ctypes.memmove(BASE + rva, ref_addr + rva, sz)
        else: DT.host_restore_img(env, d)
    def wr_ok(va, n): return any(BASE + r <= va and va + n <= BASE + r + z for r, z in env.wr_secs)
    def seed_plan(text):
        """zero-initialised globals the function names by constant address: [(va, kind)], kind = byte size of a direct load, or
        'tab' for an address used as a table base.  Without seeding, every path through a global pointer faults in the oracle."""
        plan = {}
        for m in re.finditer(r'LD\((u8|u16|u32|u64|float|double|v128),\(\(u64\)0x(14[0-9a-f]{7})ull\)\)', text):
            n = {'u8': 1, 'u16': 2, 'u32': 4, 'float': 4, 'u64': 8, 'double': 8, 'v128': 16}[m.group(1)]; va = int(m.group(2), 16)
            if wr_ok(va, n) and env.IMG[va - BASE:va - BASE + n] == bytes(n): plan[va] = n
        direct = set(int(x, 16) for x in re.findall(r'(?:LD|ST)\(\w+,\(\(u64\)0x(14[0-9a-f]{7})ull\)', text))
        for m in re.finditer(r'0x(14[0-9a-f]{7})ull', text):
            va = int(m.group(1), 16)
            if va in direct or va in plan: continue
            if wr_ok(va, 512) and env.IMG[va - BASE:va - BASE + 512] == bytes(512): plan[va] = 'tab'
        return sorted(plan.items(), key=lambda x: x[0])
    import random as _random
    def seed_globals(plan, seed):
        r = _random.Random(seed ^ 0x5eed5eed)
        ptr = lambda: DT.ARENA + r.randrange(0x1000, 0x20000, 16)
        for va, kind in plan:
            if kind == 'tab':
                b = b''.join(struct.pack('<Q', ptr() if r.random() < 0.6 else 0) for i in range(64))
            elif kind == 8:
                k = r.random(); b = struct.pack('<Q', ptr() if k < 0.75 else 0)
            elif kind == 16: b = struct.pack('<4f', *(r.uniform(-4, 4) for i in range(4)))
            else:
                k = r.random(); v = 0 if k < 0.4 else r.randint(1, 8) if k < 0.8 else r.getrandbits(8 * kind)
                b = v.to_bytes(kind, 'little')
            ctypes.memmove(va, b, len(b))

    KLUDGE = re.compile(r'=\(u64\)\(\(u64\)t\d+ - \(u64\)\(\(u64\)0x120ull\)\);\nPUT\(u64,48,t\d+\);\n')
    def run_function(rva):
        """runs in a forked child; returns the result dict"""
        f = G.by_rva[rva]; site = MSL.SITES.get(rva); text = TXT[rva][1].text
        if os.environ.get('MSDT_DEBUG'):                                  # where is a slow child? (python stack every N seconds)
            import faulthandler; faulthandler.dump_traceback_later(int(os.environ['MSDT_DEBUG']) if os.environ['MSDT_DEBUG'].isdigit() else 600, repeat=True, file=sys.stderr)
        depth, bts, why = stack_depths(K, G.pat, f, site)
        res = dict(rva='%x' % rva, depth_analysis=why, entry=dict(run=0, valid=0, mismatch=0), mid=dict(run=0, valid=0, mismatch=0),
                   blk=dict(run=0, valid=0, mismatch=0), skips=collections.Counter(),
                   examples=[], site_hits=0, bt_sites=[], bt_scratch_tests=0)
        # VEX lifts `bt/bts/btr/btc reg,reg` through a scratch store of the operand at [rsp-288] (rsp is lowered and restored inside
        # the instruction).  The original writes nothing.  Those bytes, and only those, are excused in the stack compare and counted.
        nk = len(KLUDGE.findall(text)); exempt = []
        if depth is not None and nk == len(bts):
            exempt = [(DT.RSP0 - d - 288 - DT.STACK, sz) for va, d, sz in bts]; res['bt_sites'] = ['%x' % (va - BASE) for va, d, sz in bts]
        elif nk: res['bt_note'] = '%d scratch-store sites in the lifted text, %s bt reg,reg instructions found: no stack exemption applied' % (nk, len(bts) if bts is not None else 'no')
        plan = seed_plan(text); res['seeded_globals'] = len(plan)
        ivals = compare_constants(K, G, rva); res['compare_constants'] = len(ivals)
        ivals_c = (ctypes.c_uint64 * max(1, len(ivals)))(*ivals)
        lib.khtest_values(ctypes.cast(ivals_c, ctypes.c_void_p), len(ivals), DT.STACK, DT.STACK + DT.STACK_SZ)
        # jump tables of the function as the image has them: site va -> the n table entries (targets)
        jt_entries = {}
        for jsite, (tab, n, jbase) in f.jt_ext.items():
            if jbase: continue
            jt_entries[jsite] = set(BASE + struct.unpack_from('<i', G.pat, tab - BASE + 4 * i)[0] for i in range(n))
        hits = collections.Counter(); succ = collections.defaultdict(set); entered = collections.Counter()
        bhits = collections.Counter(); bsucc = collections.defaultdict(set); bmis = collections.Counter()
        lib.khtest_init(); DT.patch_int3(env, rva)
        if site:
            pg = (BASE + site['site']) & ~0xfff
            libc.mprotect(pg, 0x2000, 7); ctypes.memmove(BASE + site['site'], b'\xcc', 1); libc.mprotect(pg, 0x2000, 5)
            lib.kn_site_clear()
            lib.kn_site_add(BASE + site['site'], BASE + site['ok'], BASE + site['skip'], ctypes.cast(getattr(lib, site['handler']), ctypes.c_void_p))
        t_fn = getattr(lib, 't_%x' % rva); c_fn = getattr(lib, 'c_%x' % rva)
        def setup(t, seed): DT.host_set(t); seed_globals(plan, seed)
        def lifted(fn, t, seed, sel, lazy, one_block=False):
            setup(t, seed); g = DT.mkstate(t); fs = ctypes.c_int(0); fa = ctypes.c_ulong(0)
            for pg, data in lazy: libc.mmap(pg, 0x1000, 7, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | DT.MAP_FIXED_NOREPLACE, -1, 0)
            lib.khtest_reset(); cov_n.value = 0; entry_sel.value = sel; stopped.value = 0; stop_id.value = 0
            single.value = 1 if one_block else 0
            if one_block: single_id.value = ids[(rva, sel - BASE)]
            rc = lib.khtest_call(ctypes.cast(fn, ctypes.c_void_p), ctypes.cast(g, ctypes.c_void_p), ctypes.byref(fa), ctypes.byref(fs))
            imgw = imgdiff()
            return rc, fa.value, fs.value, g, imgw
        def excuse_bt(ostack):
            """-> (oracle stack with the bt scratch bytes taken from the lifted side, number of excused bytes); (ostack, 0) if any other byte differs"""
            hs = ctypes.string_at(DT.STACK, DT.STACK_SZ)
            if hs == ostack or not exempt: return ostack, 0
            diffs = [k for pg in range(0, DT.STACK_SZ, 4096) if hs[pg:pg + 4096] != ostack[pg:pg + 4096] for k in range(pg, pg + 4096) if hs[k] != ostack[k]]
            if not all(any(lo <= k < lo + sz for lo, sz in exempt) for k in diffs): return ostack, 0
            b = bytearray(ostack)
            for k in diffs: b[k] = hs[k]
            return bytes(b), len(diffs)
        def one(seed, entry_va, d, one_block=False):
            mid = entry_va != 0; grp = res['blk' if one_block else 'mid' if mid else 'entry']; grp['run'] += 1
            t = DT.make_test(seed, env.fptrs, sent, True)
            if ivals:                                                     # a quarter of the registers: a compare constant of the function, +-1
                rr = _random.Random(seed ^ 0xb1a5ed)
                for nme in R64:
                    if nme == 'rsp' or rr.random() >= 0.25: continue
                    v = (rr.choice(ivals) + rr.choice((0, 0, 0, 1, -1))) & 0xffffffff; k = rr.random()
                    t[nme] = v if k < 0.5 else (v | (0xffffffff00000000 if v & 0x80000000 else 0)) if k < 0.75 else (v | (rr.getrandbits(32) << 32))
            if mid and (entry_va - BASE) in SITE_DIRECTED and (seed & 1): SITE_DIRECTED[entry_va - BASE](DT, t, _random.Random(seed ^ 0x51fe))
            if mid: t['rsp'] = RSP0 - d                                   # the return address slot stays at RSP0
            setup(t, seed)
            inn = DT.NState(); outn = DT.NState()
            for i, n in enumerate(R64): inn.gpr[i] = t[n]
            for i in range(16):
                v = t['xmm%d' % i] | (t['yh%d' % i] << 128)
                ctypes.memmove(ctypes.addressof(inn) + 128 + 32 * i, v.to_bytes(32, 'little'), 32)
            inn.gs = DT.GSB
            lib.khtest_reset(); fa = ctypes.c_ulong(0); stop_pc.value = 0
            _t0 = time.time()
            if os.environ.get('MSDT_DEBUG') == 'trace': sys.stderr.write('N f_%x entry %x seed %d single %s\n' % (rva, entry_va, seed, one_block)); sys.stderr.flush()
            sig = lib.native_run(ctypes.byref(inn), ctypes.byref(outn), entry_va or BASE + rva, ctypes.byref(fa), a.native_ms)
            nl = ctypes.c_int.in_dll(lib, 'kn_nlazy').value
            if os.environ.get('MSDT_DEBUG') and time.time() - _t0 > 1.0:
                sys.stderr.write('SLOW native_run %.1f s: f_%x entry %x seed %d single %s -> sig %d nlazy %d pc %x\n' % (time.time() - _t0, rva, entry_va, seed, one_block, sig, nl, ctypes.c_uint64.in_dll(lib, 'kn_pc').value)); sys.stderr.flush()
            lazy = [(ctypes.c_uint64 * 48).in_dll(lib, 'kn_lazy')[i] for i in range(nl)]
            if sig:
                res['skips']['native-sig%d' % sig] += 1
                for pg in lazy: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                restore(imgdiff()); return
            o = {n: outn.gpr[i] for i, n in enumerate(R64)}
            for i in range(16):
                v = int.from_bytes(ctypes.string_at(ctypes.addressof(outn) + 128 + 32 * i, 32), 'little'); o['xmm%d' % i] = v & ((1 << 128) - 1); o['yh%d' % i] = v >> 128
            o['arena'] = ctypes.string_at(DT.ARENA, DT.ARENA_SZ); o['stack'] = ctypes.string_at(DT.STACK, DT.STACK_SZ)
            o['imgw'] = imgdiff(); restore(o['imgw'])
            o['lazy'] = [(pg, ctypes.string_at(pg, 0x1000)) for pg in lazy]
            for pg in lazy: libc.munmap(ctypes.c_void_p(pg), 0x1000)
            if o['imgw'] is None: res['skips']['native-wrote-more-than-64-image-pages'] += 1; return
            nstop = stop_pc.value                                         # single-block: the block start the oracle reached (0 = it returned)
            if o['rsp'] != RSP0 + 8 and not (one_block and nstop): res['skips']['native-rsp'] += 1; return
            ok = True; noncanon = False; excused = 0; jt_out = False
            for kind, fn, sel in ([('pristine', t_fn, 0)] if not mid else []) + [('traced', c_fn, entry_va)]:
                _t1 = time.time()
                if os.environ.get('MSDT_DEBUG') == 'trace': sys.stderr.write('L %s\n' % kind); sys.stderr.flush()
                rc, fav, fsv, g, imgw = lifted(fn, t, seed, sel, o['lazy'], one_block)
                if os.environ.get('MSDT_DEBUG') and time.time() - _t1 > 1.0:
                    sys.stderr.write('SLOW lifted %.1f s: f_%x entry %x seed %d single %s kind %s rc %d cov_n %d\n' % (time.time() - _t1, rva, entry_va, seed, one_block, kind, rc, cov_n.value)); sys.stderr.flush()
                if rc or imgw is None:
                    for pg, data in o['lazy']: libc.munmap(ctypes.c_void_p(pg), 0x1000)
                    restore(imgw)
                    if not rc:                                            # the oracle changed at most 64 image pages, the lifted side more
                        ok = False
                        if len(res['examples']) < 4: res['examples'].append(dict(seed=seed, entry='%x' % (entry_va - BASE if entry_va else rva), kind=kind, diff='lifted side wrote more than 64 image pages'))
                        continue
                    if rc == 1 and fav in (0, 0xffffffffffffffff) and fsv == 11: noncanon = True; break     # non-canonical address #GP: no fault address to compare
                    if rc == 2 and one_block and fav in jt_entries and nstop not in jt_entries[fav]:
                        # The test entered a jump-table dispatch block directly, behind the function's own bounds check, with an index
                        # outside the table: the oracle jumped to a non-entry that happened to be a block start, the lifted switch took its
                        # `default` (policy fault "jumptable").  Not a valid test of the code.  (An in-table target would be a mismatch.)
                        jt_out = True; break
                    ok = False
                    if len(res['examples']) < 4: res['examples'].append(dict(seed=seed, entry='%x' % (entry_va - BASE if entry_va else rva), kind=kind, diff='FAULT rc%d sig%d addr %x' % (rc, fsv, fav)))
                    continue
                lib.khtest_acc()
                o2 = dict(o); o2['stack'], nex = excuse_bt(o['stack']); excused = max(excused, nex)
                dd = DT.compare(o2, DT.state_out(g, True), True, imgw, True)
                if one_block:
                    lstop = BASE + id2[stop_id.value][1] if stopped.value else 0
                    if lstop != nstop: dd.append(('next-block', '%x' % nstop, '%x' % lstop))
                for pg, data in o['lazy']:
                    if ctypes.string_at(pg, 0x1000) != data: dd.append(('lazy@%x' % pg, '', ''))
                    libc.munmap(ctypes.c_void_p(pg), 0x1000)
                restore(imgw)
                if dd:
                    ok = False
                    if len(res['examples']) < 4: res['examples'].append(dict(seed=seed, entry='%x' % (entry_va - BASE if entry_va else rva), kind=kind, diff=[list(map(str, x)) for x in dd[:4]]))
                if kind == 'traced': trace = [cov_buf[i] for i in range(min(cov_n.value, 16384))]
            if noncanon: res['skips']['noncanonical-GP'] += 1; return
            if jt_out: res['skips']['jumptable-index-outside-table'] += 1; return
            grp['valid'] += 1
            if not ok:
                grp['mismatch'] += 1
                if one_block: bmis[entry_va - BASE] += 1
                return
            if excused: res['bt_scratch_tests'] += 1
            if one_block:
                bhits[entry_va - BASE] += 1; bsucc[entry_va - BASE].add('%x' % (nstop - BASE) if nstop else 'ret'); return
            seen = set()
            for i, b in enumerate(trace):
                blk = id2[b][1]
                if blk not in seen: seen.add(blk); hits[blk] += 1
                if i + 1 < len(trace): succ[blk].add(id2[trace[i + 1]][1])
            if mid: entered[entry_va - BASE] += 1
        only = os.environ.get('MSDT_ONE')                                 # debug: "blockhex:seed" = run that one single-block test
        if only: only = (int(only.split(':')[0], 16), int(only.split(':')[1]))
        for k in range(0 if only else a.tests): one(a.seed + k * 7919 + rva, 0, 0)
        if depth is not None and not only:
            for blk in blocks[rva]:
                if blk == rva: continue
                d = depth.get(BASE + blk)
                if d is None: res['skips']['block-not-reached-by-depth-analysis'] += 1; continue
                for k in range(a.mid_tests): one(a.seed + 104729 * (k + 1) + blk * 31, BASE + blk, d)
        # single-block tests: INT3 on every block start; the block under test is restored while its tests run
        dz = depth or {}
        tpg = sorted(set((BASE + b) & ~0xfff for b in blocks[rva]))
        def poke(va, byte):
            pg = va & ~0xfff; libc.mprotect(pg, 0x1000, 7); ctypes.memmove(va, byte, 1); libc.mprotect(pg, 0x1000, 5)
        orig = {b: G.pat[b:b + 1] for b in blocks[rva]}
        for b in blocks[rva]:
            if not (site and b == site['site']): poke(BASE + b, b'\xcc')
        for blk in blocks[rva]:
            is_site = bool(site and blk == site['site'])
            if not is_site: poke(BASE + blk, orig[blk])
            lib.kn_stop_clear()
            for b in blocks[rva]:
                if b != blk: lib.kn_stop_add(BASE + b)
            for k in range(a.blk_tests):
                sd = a.seed + 15485864 * (k + 1) + blk * 131 + k
                if only and (blk, sd) != only: continue
                one(sd, BASE + blk, dz.get(BASE + blk, 0), True)
            if not is_site: poke(BASE + blk, b'\xcc')
        lib.kn_stop_clear(); single.value = 0
        res['skips'] = dict(res['skips']); res['hits'] = {('%x' % k): v for k, v in hits.items()}
        res['bhits'] = {('%x' % k): v for k, v in bhits.items()}; res['bsucc'] = {('%x' % k): sorted(v) for k, v in bsucc.items()}
        res['bmis'] = {('%x' % k): v for k, v in bmis.items()}
        res['succ'] = {('%x' % k): sorted('%x' % x for x in v) for k, v in succ.items()}
        res['mid_entered'] = {('%x' % k): v for k, v in entered.items()}
        res['site_hits'] = ctypes.c_uint64.in_dll(lib, 'kn_site_hits').value
        res['stub_calls'] = lib.khtest_stub_total(); res['blocks'] = len(blocks[rva])
        return res

    # ---- run: one forked child per function, a few at a time
    pending = list(want); running = {}; results = {}
    t0 = time.time()
    while pending or running:
        while pending and len(running) < a.jobs:
            rva = pending.pop(0); rp = os.path.join(a.work, 'res_%x.json' % rva)
            if os.path.exists(rp): os.unlink(rp)
            pid = os.fork()
            if pid == 0:
                try:
                    out = run_function(rva)
                except BaseException as e:
                    import traceback
                    out = dict(rva='%x' % rva, error=repr(e), tb=traceback.format_exc()[-1500:])
                open(rp, 'w').write(json.dumps(out)); os._exit(0)
            running[pid] = (rva, rp, time.time())
        pid, st = os.waitpid(-1, os.WNOHANG)
        if pid == 0:
            for p, (rva, rp, ts) in list(running.items()):
                if time.time() - ts > 1500: os.kill(p, signal.SIGKILL)
            time.sleep(0.05); continue
        rva, rp, ts = running.pop(pid)
        sys.stderr.write('mod_static_difftest: f_%x done in %.0f s (%d left)\n' % (rva, time.time() - ts, len(pending) + len(running))); sys.stderr.flush()
        if os.path.exists(rp): results[rva] = json.load(open(rp)); os.unlink(rp)
        else: results[rva] = dict(rva='%x' % rva, error='child died (signal %s)' % (os.WTERMSIG(st) if os.WIFSIGNALED(st) else '?'))
    # ---- per-edit coverage
    edits = []
    for rva in want:
        R = results[rva]; ext = EXT[rva]
        items = [('edit', e) for e in G.changed.get(rva, [])]
        if rva in MSL.SITES: items.append(('site', MSL.SITES[rva]['site']))
        for kind, e in items:
            blk = next((b - BASE for b, end in ext.items() if b - BASE <= e < end - BASE), None)
            key = ('%x' % blk) if blk is not None else None
            edits.append(dict(func='%x' % rva, kind=kind, rva='%x' % e, block=key,
                              path_hits=R.get('hits', {}).get(key, 0), path_successors=R.get('succ', {}).get(key, []),
                              block_hits=R.get('bhits', {}).get(key, 0), block_successors=R.get('bsucc', {}).get(key, []),
                              block_mismatches=R.get('bmis', {}).get(key, 0)))
    bad = [r for r in results.values() if r.get('error') or r['entry']['mismatch'] or r['mid']['mismatch'] or r['blk']['mismatch']]
    novalid = [r for r in results.values() if not r.get('error') and r['entry']['valid'] + r['mid']['valid'] + r['blk']['valid'] == 0]
    uncovered = [e for e in edits if e['path_hits'] + e['block_hits'] == 0]
    summ = dict(functions=len(want), entry_tests=sum(r.get('entry', {}).get('run', 0) for r in results.values()),
                entry_valid=sum(r.get('entry', {}).get('valid', 0) for r in results.values()),
                mid_tests=sum(r.get('mid', {}).get('run', 0) for r in results.values()), mid_valid=sum(r.get('mid', {}).get('valid', 0) for r in results.values()),
                block_tests=sum(r.get('blk', {}).get('run', 0) for r in results.values()), block_valid=sum(r.get('blk', {}).get('valid', 0) for r in results.values()),
                blocks=sum(r.get('blocks', 0) for r in results.values()), blocks_with_valid_block_test=sum(len(r.get('bhits', {})) for r in results.values()),
                mismatching_functions=len(bad), functions_without_valid_test=len(novalid), edits=len([e for e in edits if e['kind'] == 'edit']),
                sites=len([e for e in edits if e['kind'] == 'site']), uncovered=len(uncovered), seconds=round(time.time() - t0, 1),
                bt_scratch_sites=sum(len(r.get('bt_sites', [])) for r in results.values()), bt_scratch_tests=sum(r.get('bt_scratch_tests', 0) for r in results.values()),
                tests_per_function=a.tests, mid_tests_per_block=a.mid_tests, block_tests_per_block=a.blk_tests, seed=a.seed, lifted_sha256=hashlib.sha256(open(a.lifted, 'rb').read()).hexdigest())
    if a.mutate_vanilla:
        summ['mutation'] = 'VANILLA lifted text against the PATCHED oracle (sensitivity check; mismatches expected)'
        summ['edits_detected_by_a_single_block_mismatch'] = len([e for e in edits if e['block_mismatches']])
        summ['functions_detected'] = len(bad)
    json.dump(dict(summary=summ, functions=[results[r] for r in want], edits=edits), open(a.out, 'w'), indent=1)
    if a.mutate_vanilla:
        for rva in want:
            R = results[rva]
            if R.get('error'): print('%-7x ERROR %s' % (rva, R['error'])); continue
            print('%-7x vanilla text vs patched bytes: entry %d/%d mismatch, mid-entry %d/%d, single-block %d/%d' % (rva, R['entry']['mismatch'], R['entry']['valid'],
                  R['mid']['mismatch'], R['mid']['valid'], R['blk']['mismatch'], R['blk']['valid']))
        for e in edits: print('%s %s in f_%s block %s: %d of %d valid single-block tests mismatch%s' % (e['kind'], e['rva'], e['func'], e['block'], e['block_mismatches'],
                  e['block_mismatches'] + e['block_hits'], '' if e['block_mismatches'] else '   <<< edit NOT detected'))
        print(json.dumps(summ)); sys.exit(0 if len(bad) == len(want) else 4)
    for rva in want:
        R = results[rva]
        if R.get('error'): print('%-7x ERROR %s' % (rva, R['error'])); continue
        nb = R['blocks']; hb = len(R['hits'])
        print('%-7x entry %3d/%3d valid, %d mismatch | mid-entry %4d/%4d valid, %d mismatch | single-block %4d/%4d valid, %d mismatch | blocks: %3d/%3d on a valid path, %3d/%3d single | stubs %d%s%s' % (
            rva, R['entry']['valid'], R['entry']['run'], R['entry']['mismatch'], R['mid']['valid'], R['mid']['run'], R['mid']['mismatch'],
            R['blk']['valid'], R['blk']['run'], R['blk']['mismatch'], hb, nb, len(R['bhits']), nb, R['stub_calls'],
            (' | site traps %d' % R['site_hits']) if rva in MSL.SITES else '', (' | depth analysis: %s' % R['depth_analysis']) if R['depth_analysis'] else '')
            + ((' | bt scratch excused in %d tests (sites %s)' % (R['bt_scratch_tests'], ','.join(R['bt_sites']))) if R.get('bt_sites') else '') + ((' | ' + R['bt_note']) if R.get('bt_note') else ''))
        for ex in R['examples'][:2]: print('          example:', json.dumps(ex)[:400])
    for e in edits: print('%s %s in f_%s block %s: single-block %d valid matching tests -> %s | on %d valid matching paths -> %s%s' % (
        e['kind'], e['rva'], e['func'], e['block'], e['block_hits'], e['block_successors'], e['path_hits'], e['path_successors'], '' if e['path_hits'] + e['block_hits'] else '   <<< NOT COVERED'))
    print(json.dumps(summ))
    sys.exit(1 if bad else 3 if (uncovered or novalid) else 0)

if __name__ == '__main__':
    main()
