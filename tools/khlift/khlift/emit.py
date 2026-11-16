"""Per-function VEX-IR -> C lifter (guest-ABI model).

Guest ABI (see rt/khrt.h, POLICY.md):
  * every lifted function is `void f_<rva hex>(KCtx *g)`; all architectural state lives in *g (VEX AMD64 layout);
  * a call is `f_x(g)` after the IR has pushed the return address on the guest stack; `ret` is `return` after the IR has
    popped it, so the guest stack is exact and callee-saved/caller-saved register behaviour is the original's;
  * indirect call / jump / tail jump = `kh_dispatch(g, target)` (address -> function table built from functions.tsv, plus the
    synthetic import and hook addresses owned by the runtime);
  * `call [IAT slot]` / `jmp [IAT slot]` of a known import = a direct call of the named runtime function `rt_imp_<dll>__<name>`;
  * MSVC jump tables = a `switch` over the resolved target addresses of the universe's jump-table record for that site.
"""
import pyvex, archinfo
from . import BASE
from .ops import (op_c, ctype, const_c, bits_of, Unsupported)

ARCH = archinfo.ArchAMD64()
GS_CONST_OFF = ARCH.registers['gs_const'][0]
CPUID_PREFIX = 'amd64g_dirtyhelper_CPUID'
KNOWN_SYS_JK = ('Ijk_SigTRAP', 'Ijk_Sys_int', 'Ijk_SigILL', 'Ijk_NoDecode', 'Ijk_SigSEGV', 'Ijk_Sys_syscall',
                'Ijk_SigFPE_IntDiv', 'Ijk_SigFPE', 'Ijk_SigBUS', 'Ijk_Sys_sysenter')

class LiftEnv:
    """read-only tables shared by every function lift."""
    def __init__(self, image, starts, hooks=None, iat=None, nep=None):
        self.image = image                # bytes, offset = RVA
        self.starts = starts              # va -> Fn   (universe function starts; liftable ones)
        self.hooks = hooks or {}          # va -> C symbol of a hand-written runtime hook replacing the lifted function
        self.iat = iat or {}              # va of IAT slot -> C symbol of the runtime import function
        self.nep = nep or set()           # va of .nep stub units (not lifted; generated as stubs)

    def callee_sym(self, va):
        """C symbol of the function at va, or None if va is not a universe function start."""
        if va in self.hooks: return self.hooks[va]
        if va in self.starts or va in self.nep: return 'f_%x' % (va - BASE)
        return None

class LiftResult:
    __slots__ = ('text', 'insns', 'blocks', 'callees', 'imports', 'n_dispatch_call', 'n_dispatch_jmp', 'n_jt', 'unsup',
                 'v256', 'tails', 'cpuid', 'dup_blocks', 'err')
    def __init__(self):
        self.text = ''; self.insns = 0; self.blocks = 0; self.callees = set(); self.imports = set()
        self.n_dispatch_call = 0; self.n_dispatch_jmp = 0; self.n_jt = 0; self.unsup = []; self.v256 = False
        self.tails = 0; self.cpuid = 0; self.dup_blocks = 0; self.err = None

def last_insn_addr(irsb):
    last = 0
    for s in irsb.statements:
        if s.tag == 'Ist_IMark': last = s.addr + s.delta
    return last

def last_insn_end(irsb):
    last = 0
    for s in irsb.statements:
        if s.tag == 'Ist_IMark': last = s.addr + s.len
    return last

class FnLifter:
    def __init__(self, env, fn, name=None, trace=False):
        self.env = env; self.fn = fn; self.name = name or 'f_%x' % fn.rva
        self.trace = trace
        self.r = LiftResult()
        self.work = []
        self.data_starts = sorted(a for a, b, k in fn.data)
        self.leaders = None

    # ---- helpers
    def infunc(self, a): return self.fn.lo <= a < self.fn.hi
    def indata(self, a): return any(lo <= a < hi for lo, hi, k in self.fn.data)
    def label(self, a): return 'L%x' % (a - BASE)

    def lift_block(self, addr):
        end = self.fn.hi
        for d in self.data_starts:
            if d > addr: end = min(end, d); break
        if self.leaders:
            import bisect
            k = bisect.bisect_right(self.leaders, addr)
            if k < len(self.leaders): end = min(end, self.leaders[k])
        data = self.env.image[addr - BASE:end - BASE]
        return pyvex.lift(data, addr, ARCH, max_bytes=len(data), opt_level=1)

    def unsup(self, why):
        if why not in self.r.unsup: self.r.unsup.append(why)

    # ---- expressions
    def expr(self, irsb, e, defs):
        t = e.tag
        if t == 'Iex_RdTmp': return 't%d' % e.tmp
        ty = e.result_type(irsb.tyenv)
        if ty == 'Ity_V256': self.r.v256 = True
        if t == 'Iex_Const': return const_c(e.con, ty)
        if t == 'Iex_Get': return 'GET(%s,%d)' % (ctype(ty), e.offset)
        if t == 'Iex_GetI':
            d = e.descr; ix = self.expr(irsb, e.ix, defs)
            return 'GETI(%s,%d,%d,%s,%d)' % (ctype(d.elemTy), d.base, d.nElems, ix, e.bias)
        if t == 'Iex_Load': return 'LD(%s,%s)' % (ctype(ty), self.expr(irsb, e.addr, defs))
        if t == 'Iex_ITE':
            return '(%s ? %s : %s)' % (self.expr(irsb, e.cond, defs), self.expr(irsb, e.iftrue, defs), self.expr(irsb, e.iffalse, defs))
        if t == 'Iex_GSPTR': return '((void*)g)'
        if t == 'Iex_CCall':
            n = e.cee.name; args = [self.expr(irsb, x, defs) for x in e.args]
            if n == 'amd64g_calculate_condition': return '((%s)calc_cond(%s))' % (ctype(ty), ','.join('(u64)' + x for x in args[:5]))
            if n == 'amd64g_calculate_rflags_all': return 'rflags_all((u64)%s,(u64)%s,(u64)%s,(u64)%s)' % tuple(args[:4])
            if n == 'amd64g_calculate_rflags_c': return 'rflags_c((u64)%s,(u64)%s,(u64)%s,(u64)%s)' % tuple(args[:4])
            if n == 'amd64g_check_ldmxcsr': return 'kh_check_ldmxcsr((u64)%s)' % args[0]
            if n == 'amd64g_create_mxcsr': return 'kh_create_mxcsr((u64)%s)' % args[0]
            raise Unsupported('ccall:' + n)
        if t in ('Iex_Unop', 'Iex_Binop', 'Iex_Triop', 'Iex_Qop'):
            args = list(e.args)
            ats = [a.result_type(irsb.tyenv) for a in args]
            return op_c(e.op, [self.expr(irsb, a, defs) for a in args], ats, ty)
        raise Unsupported('expr:' + t)

    def call_target(self, irsb, nx, defs):
        """-> ('direct', va) | ('import', symbol) | ('dispatch', C expr)"""
        if nx.tag == 'Iex_Const': return ('direct', nx.con.value)
        if nx.tag == 'Iex_RdTmp':
            d = defs.get(nx.tmp)
            if d is not None and d.tag == 'Iex_Load' and d.addr.tag == 'Iex_Const':
                sym = self.env.iat.get(d.addr.con.value)
                if sym: return ('import', sym)
        return ('dispatch', self.expr(irsb, nx, defs))

    # ---- statements
    def stmt(self, irsb, s, out, defs):
        t = s.tag
        if t in ('Ist_IMark', 'Ist_AbiHint', 'Ist_NoOp'): return
        if t == 'Ist_Put':
            ty = s.data.result_type(irsb.tyenv)
            if ty == 'Ity_V256': self.r.v256 = True
            out.append('PUT(%s,%d,%s);' % (ctype(ty), s.offset, self.expr(irsb, s.data, defs))); return
        if t == 'Ist_PutI':
            d = s.descr
            out.append('PUTI(%s,%d,%d,%s,%d,%s);' % (ctype(d.elemTy), d.base, d.nElems, self.expr(irsb, s.ix, defs), s.bias, self.expr(irsb, s.data, defs))); return
        if t == 'Ist_WrTmp':
            out.append('t%d=%s;' % (s.tmp, self.expr(irsb, s.data, defs))); return
        if t == 'Ist_Store':
            ty = s.data.result_type(irsb.tyenv)
            if ty == 'Ity_V256': self.r.v256 = True
            out.append('ST(%s,%s,%s);' % (ctype(ty), self.expr(irsb, s.addr, defs), self.expr(irsb, s.data, defs))); return
        if t == 'Ist_Exit':
            dst = s.dst.value; g = self.expr(irsb, s.guard, defs)
            if s.jk == 'Ijk_Yield':
                out.append('if(%s) kh_pause(g);' % g); self.work.append(dst); out.append('if(%s) goto %s;' % (g, self.label(dst))); return
            if s.jk == 'Ijk_EmWarn':
                out.append('if(%s) kh_emwarn(g);' % g); return
            if s.jk != 'Ijk_Boring':
                out.append('if(%s) KH_FAULT(g,0x%x,"%s");' % (g, last_insn_addr(irsb), s.jk[4:])); return
            if not self.infunc(dst) or self.indata(dst):
                sym = self.env.callee_sym(dst)
                if sym is None:
                    out.append('if(%s){ kh_dispatch(g,0x%xull); return; }' % (g, dst)); self.r.n_dispatch_jmp += 1
                else:
                    self.r.callees.add(dst); self.r.tails += 1
                    out.append('if(%s){ %s(g); return; }' % (g, sym))
                return
            self.work.append(dst); out.append('if(%s) goto %s;' % (g, self.label(dst))); return
        if t == 'Ist_CAS':
            if s.dataHi is not None: raise Unsupported('cas-double')
            ty = s.dataLo.result_type(irsb.tyenv); T = ctype(ty)
            out.append('t%d=KH_CAS(%s,%s,%s,%s);' % (s.oldLo, T, self.expr(irsb, s.addr, defs), self.expr(irsb, s.expdLo, defs), self.expr(irsb, s.dataLo, defs))); return
        if t == 'Ist_MBE':
            out.append('KH_FENCE();'); return
        if t == 'Ist_Dirty':
            n = s.cee.name
            if n.startswith(CPUID_PREFIX):
                self.r.cpuid += 1; out.append('kh_cpuid(g);'); return
            if n == 'amd64g_dirtyhelper_RDTSC':
                out.append('kh_rdtsc(g);'); return
            raise Unsupported('dirty:' + n)
        raise Unsupported('stmt:' + t)

    # ---- blocks
    def block_c(self, a, irsb):
        r = self.r
        defs = {}
        for s in irsb.statements:
            if s.tag == 'Ist_WrTmp': defs[s.tmp] = s.data
        lines = []
        used = set()
        # IMark walk: XGETBV is replaced by the runtime profile (VEX bakes in XCR0=0xE7, i.e. AVX-512 state)
        skip = False
        for s in irsb.statements:
            if s.tag == 'Ist_IMark':
                r.insns += 1
                skip = False
                if self.env.image[s.addr - BASE:s.addr - BASE + s.len] == b'\x0f\x01\xd0':
                    lines.append('kh_xgetbv(g);'); skip = True
                continue
            if skip: continue
            try: self.stmt(irsb, s, lines, defs)
            except Unsupported as e: self.unsup(str(e)); lines.append('/*UNSUP %s*/' % e)
        jk = irsb.jumpkind
        nx = irsb.next
        term = []
        last = last_insn_addr(irsb); lend = last_insn_end(irsb)
        if jk == 'Ijk_Boring':
            if nx.tag == 'Iex_Const':
                d = nx.con.value
                if self.infunc(d) and not self.indata(d):
                    self.work.append(d); term.append('goto %s;' % self.label(d))
                else:
                    sym = self.env.callee_sym(d)
                    if sym is None:
                        term.append('kh_dispatch(g,0x%xull); return;' % d); r.n_dispatch_jmp += 1
                    else:
                        r.callees.add(d); r.tails += 1; term.append('%s(g); return;' % sym)
            else:
                tg = sorted(set(self.fn.jtabs.get(last, ())))
                if tg:
                    r.n_jt += 1
                    try: ex = self.expr(irsb, nx, defs)
                    except Unsupported as e: self.unsup(str(e)); ex = '0'
                    term.append('{ switch(%s){ %s default: KH_FAULT(g,0x%x,"jumptable"); } }' % (
                        ex, ''.join('case 0x%xull: goto %s; ' % (x, self.label(x)) for x in tg), last))
                    self.work.extend(tg)
                else:
                    kind, v = self.call_target(irsb, nx, defs)
                    if kind == 'import':
                        r.imports.add(v); term.append('%s(g); return;' % v)
                    else:
                        r.n_dispatch_jmp += 1
                        try: term.append('kh_dispatch(g,%s); return;' % (v if kind == 'dispatch' else '0x%xull' % v))
                        except Exception: pass
        elif jk == 'Ijk_Ret':
            term.append('return;')
        elif jk == 'Ijk_Call':
            kind, v = self.call_target(irsb, nx, defs)
            if kind == 'direct':
                sym = self.env.callee_sym(v)
                if sym is None:
                    r.n_dispatch_call += 1; term.append('kh_dispatch(g,0x%xull);' % v)
                else:
                    r.callees.add(v); term.append('%s(g);' % sym)
            elif kind == 'import':
                r.imports.add(v); term.append('%s(g);' % v)
            else:
                r.n_dispatch_call += 1; term.append('kh_dispatch(g,%s);' % v)
            if self.infunc(lend) and not self.indata(lend):
                self.work.append(lend); term.append('goto %s;' % self.label(lend))
            else:
                term.append('KH_NORETURN_FALL(g,0x%x);' % lend)
        elif jk == 'Ijk_Yield':
            term.append('kh_pause(g);')
            if self.infunc(lend): self.work.append(lend); term.append('goto %s;' % self.label(lend))
        elif jk in KNOWN_SYS_JK:
            term.append('KH_FAULT(g,0x%x,"%s");' % (last, jk[4:]))
        else:
            raise Unsupported('jumpkind:' + jk)
        # temporaries: declare those assigned in this block
        types = irsb.tyenv.types
        used = sorted(set(s.tmp for s in irsb.statements if s.tag == 'Ist_WrTmp') | set(s.oldLo for s in irsb.statements if s.tag == 'Ist_CAS'))
        byty = {}
        for tmp in used: byty.setdefault(ctype(types[tmp]), []).append('t%d' % tmp)
        decl = ' '.join('%s %s;' % (T, ','.join(v)) for T, v in sorted(byty.items()))
        body = '\n'.join(lines + term)
        trace = 'TRACE(0x%x); ' % (a - BASE) if self.trace else ''
        return '%s:{ %s%s\n%s\n}' % (self.label(a), trace, decl, body)

    def lift(self):
        # pass 1: discover every basic-block leader (jump / branch / call-fallthrough / jump-table targets);
        # pass 2: lift each block bounded by the next leader, so no instruction is emitted twice.
        self._discover()
        return self._emit()

    def _discover(self):
        fn = self.fn; self.work = [fn.lo]; done = set()
        saved = self.r; self.r = LiftResult()
        while self.work:
            a = self.work.pop()
            if a in done or not self.infunc(a) or self.indata(a): continue
            done.add(a)
            try:
                irsb = self.lift_block(a); self.block_c(a, irsb)
            except Exception:
                pass
        self.r = saved
        self.leaders = sorted(done)

    def _emit(self):
        r = self.r; fn = self.fn
        self.work = [fn.lo]; done = set(); blocks = []; covered = []
        while self.work:
            a = self.work.pop()
            if a in done: continue
            if not self.infunc(a) or self.indata(a): continue
            done.add(a)
            try:
                irsb = self.lift_block(a)
            except Exception as e:
                self.unsup('liftexc:%s' % type(e).__name__); continue
            try:
                blocks.append((a, self.block_c(a, irsb)))
            except Unsupported as e:
                self.unsup(str(e))
            except Exception as e:
                self.unsup('exc:%s:%s' % (type(e).__name__, str(e)[:60]))
            covered.append((a, last_insn_end(irsb)))
        # blocks that overlap another block (a jump into the middle of an already lifted run) were lifted twice; count them
        covered.sort()
        for (a, e), (b, f) in zip(covered, covered[1:]):
            if b < e: r.dup_blocks += 1
        r.blocks = len(blocks)
        if r.unsup:
            r.err = r.unsup[0]; return r
        blocks.sort()
        # deterministic order: entry block first, then ascending address
        blocks.sort(key=lambda x: (x[0] != fn.lo, x[0]))
        r.text = 'void %s(KCtx *g){\n%s\n}\n' % (self.name, '\n'.join(b for a, b in blocks))
        return r
