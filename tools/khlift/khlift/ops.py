"""Table-driven VEX operation -> C expression rules.

Every rule is (regex over the op name without the `Iop_` prefix, handler).  A handler gets the regex match `m`, the list of C
argument expressions `a` (already emitted), their VEX types `ts`, and the result type `rty`, and returns a C expression string.
Rules are tried in table order; the first match wins; no match raises Unsupported(op).  The emitter is deterministic: the
table order is the only thing that decides which rule fires.

Vector/float rules do NOT inline loops.  They call helpers named `vx_<Op>` (and `vy_<Op>` for 256-bit) that are defined in
rt/khrt_simd.h; `tools/selftest_ops.py` checks that every helper the table can name exists in that header.  This keeps the
emitted C compact and keeps the one place that implements the NaN / denormal / min-max / rounding policy (POLICY.md) in C.

Scalar integer ops are small enough to be inlined.
"""
import re, struct

class Unsupported(Exception):
    pass

CT = {'Ity_I1': 'u8', 'Ity_I8': 'u8', 'Ity_I16': 'u16', 'Ity_I32': 'u32', 'Ity_I64': 'u64', 'Ity_I128': 'u128',
      'Ity_F32': 'float', 'Ity_F64': 'double', 'Ity_V128': 'v128', 'Ity_V256': 'v256',
      'Ity_F16': 'u16', 'Ity_D32': 'u32', 'Ity_D64': 'u64'}
BITS = {'Ity_I1': 1, 'Ity_I8': 8, 'Ity_I16': 16, 'Ity_I32': 32, 'Ity_I64': 64, 'Ity_I128': 128, 'Ity_V128': 128,
        'Ity_V256': 256, 'Ity_F32': 32, 'Ity_F64': 64}

def ctype(t):
    if t not in CT:
        raise Unsupported('type ' + t)
    return CT[t]

def bits_of(t):
    return BITS[t]

U = {8: 'u8', 16: 'u16', 32: 'u32', 64: 'u64', 128: 'u128'}
S = {8: 's8', 16: 's16', 32: 's32', 64: 's64', 128: 's128'}

def const_c(con, ty):
    v = con.value
    if ty == 'Ity_F32':
        return 'LDV(float,&(u32){0x%x})' % (struct.unpack('<I', struct.pack('<f', v))[0] if isinstance(v, float) else v)
    if ty == 'Ity_F64':
        return 'LDV(double,&(u64){0x%xull})' % (struct.unpack('<Q', struct.pack('<d', v))[0] if isinstance(v, float) else v)
    if ty == 'Ity_V128':
        b = int(v).to_bytes(16, 'little')
        if not any(b): return 'VZ'
        return '((v128){%s})' % ','.join(str(x) for x in b)
    if ty == 'Ity_V256':
        b = int(v).to_bytes(32, 'little')
        if not any(b): return '((v256){VZ,VZ})'
        return '((v256){((v128){%s}),((v128){%s})})' % (','.join(str(x) for x in b[:16]), ','.join(str(x) for x in b[16:]))
    if ty == 'Ity_I128': return '((u128)0x%xull)' % v
    return '((%s)0x%xull)' % (ctype(ty), int(v) & ((1 << bits_of(ty)) - 1))

RULES = []
def rule(pat):
    rx = re.compile(pat)
    def deco(fn):
        RULES.append((rx, fn)); return fn
    return deco

def helper_names():
    """every vx_/vy_ helper this table may emit (for selftest): filled by `emit_names` as rules fire in tests."""
    return sorted(_NAMES)
_NAMES = set()
def call(name, *args):
    _NAMES.add(name)
    return '%s(%s)' % (name, ','.join(args))

# ------------------------------------------------------------ scalar integer
@rule(r'(Add|Sub|Mul|And|Or|Xor)(8|16|32|64)')
def _arith(m, a, ts, rty):
    T = U[int(m.group(2))]; sym = {'Add': '+', 'Sub': '-', 'Mul': '*', 'And': '&', 'Or': '|', 'Xor': '^'}[m.group(1)]
    return '(%s)((%s)%s %s (%s)%s)' % (T, T, a[0], sym, T, a[1])

@rule(r'Not(1|8|16|32|64)')
def _not(m, a, ts, rty):
    if m.group(1) == '1': return '(u8)(!%s)' % a[0]
    return '(%s)(~%s)' % (U[int(m.group(1))], a[0])

@rule(r'Neg(8|16|32|64)')
def _neg(m, a, ts, rty): return '(%s)(-%s)' % (U[int(m.group(1))], a[0])

@rule(r'Sh(l|r)(8|16|32|64)')
def _shift(m, a, ts, rty):
    n = int(m.group(2)); T = U[n]
    return ('SHL(%s,%d,%s,%s)' if m.group(1) == 'l' else 'SHR(%s,%d,%s,%s)') % (T, n, a[0], a[1])

@rule(r'Sar(8|16|32|64)')
def _sar(m, a, ts, rty):
    n = int(m.group(1)); return '(%s)SAR(%s,%d,%s,%s)' % (U[n], S[n], n, a[0], a[1])

@rule(r'(CmpEQ|CmpNE|CasCmpEQ|CasCmpNE|ExpCmpNE)(8|16|32|64)')
def _cmpeq(m, a, ts, rty):
    T = U[int(m.group(2))]; s = '==' if m.group(1) in ('CmpEQ', 'CasCmpEQ') else '!='
    return '(u8)((%s)%s %s (%s)%s)' % (T, a[0], s, T, a[1])

@rule(r'Cmp(LT|LE)(8|16|32|64)(S|U)')
def _cmplt(m, a, ts, rty):
    n = int(m.group(2)); T = (S if m.group(3) == 'S' else U)[n]; s = '<' if m.group(1) == 'LT' else '<='
    return '(u8)((%s)%s %s (%s)%s)' % (T, a[0], s, T, a[1])

@rule(r'CmpORD(32|64)(S|U)')
def _cmpord(m, a, ts, rty):
    n = int(m.group(1)); T = (S if m.group(2) == 'S' else U)[n]
    return '(%s)(((%s)%s<(%s)%s)?8:(((%s)%s>(%s)%s)?4:2))' % (U[n], T, a[0], T, a[1], T, a[0], T, a[1])

@rule(r'CmpNEZ(8|16|32|64)')
def _cmpnez(m, a, ts, rty): return '(u8)(%s!=0)' % a[0]

@rule(r'(\d+)(U|S)to(\d+)')
def _ext(m, a, ts, rty):
    f, sg, t = int(m.group(1)), m.group(2), int(m.group(3))
    if f == 1:
        if sg == 'U': return '(%s)(%s&1)' % (U[t], a[0])
        return '(%s)(-(s64)(%s&1))' % (U[t], a[0])
    if sg == 'U': return '(%s)(%s)' % (U[t], a[0])
    return '(%s)(%s)(%s)%s' % (U[t], S[t], S[f], a[0])

@rule(r'(8|16|32|64)to(1|8|16|32)')
def _narrow(m, a, ts, rty):
    t = int(m.group(2))
    if t == 1: return '(u8)(%s&1)' % a[0]
    return '(%s)%s' % (U[t], a[0])

@rule(r'(16|32|64)HIto(8|16|32)')
def _hi(m, a, ts, rty):
    t = int(m.group(2)); return '(%s)(%s>>%d)' % (U[t], a[0], t)

@rule(r'(8|16|32)HLto(16|32|64)')
def _hl(m, a, ts, rty):
    f = int(m.group(1)); t = int(m.group(2)); return '(%s)(((%s)%s<<%d)|(%s)%s)' % (U[t], U[t], a[0], f, U[f], a[1])

@rule(r'Mull(S|U)(8|16|32|64)')
def _mull(m, a, ts, rty):
    n = int(m.group(2)); sg = m.group(1)
    if n == 64:
        return ('(u128)((s128)(s64)%s*(s128)(s64)%s)' if sg == 'S' else '((u128)%s*(u128)%s)') % (a[0], a[1])
    T = (S if sg == 'S' else U)[n]; R = (S if sg == 'S' else U)[2 * n]
    return '(%s)((%s)(%s)%s*(%s)(%s)%s)' % (U[2 * n], R, T, a[0], R, T, a[1])

@rule(r'DivMod(U|S)(64to32|128to64|32to16)')
def _divmod(m, a, ts, rty):
    sg, k = m.group(1), m.group(2)
    if k == '64to32':
        T = ('s64', 's32') if sg == 'S' else ('u64', 'u32')
        return '({ %s _n=(%s)%s; %s _d=(%s)%s; (u64)(((u64)(u32)(%s)(_n%%_d)<<32)|(u32)(%s)(_n/_d)); })' % (T[0], T[0], a[0], T[1], T[1], a[1], T[1], T[1])
    if k == '128to64':
        T = ('s128', 's64') if sg == 'S' else ('u128', 'u64')
        return '({ %s _n=(%s)%s; %s _d=(%s)%s; (u128)(((u128)(u64)(%s)(_n%%_d)<<64)|(u64)(%s)(_n/_d)); })' % (T[0], T[0], a[0], T[1], T[1], a[1], T[1], T[1])
    raise Unsupported('divmod ' + k)

@rule(r'(Clz|Ctz)(Nat)?(32|64)')
def _clz(m, a, ts, rty):
    n = int(m.group(3)); fn = 'clz' if m.group(1) == 'Clz' else 'ctz'
    return '(%s)(%s==0?%d:__builtin_%s%s(%s))' % (U[n], a[0], n, fn, 'll' if n == 64 else '', a[0])

@rule(r'Left(8|16|32|64)')
def _left(m, a, ts, rty):
    n = int(m.group(1)); return '(%s)(%s|(%s)(-(%s)%s))' % (U[n], a[0], U[n], S[n], a[0])

@rule(r'Max(8|16|32|64)U')
def _maxu(m, a, ts, rty):
    n = int(m.group(1)); return '(%s)(%s>%s?%s:%s)' % (U[n], a[0], a[1], a[0], a[1])

@rule(r'128HIto64')
def _128hi(m, a, ts, rty): return '((u64)(%s>>64))' % a[0]
@rule(r'128to64')
def _128lo(m, a, ts, rty): return '((u64)%s)' % a[0]
@rule(r'64HLto128')
def _64hl128(m, a, ts, rty): return '(((u128)%s<<64)|(u128)%s)' % (a[0], a[1])

# ------------------------------------------------------------ scalar float
@rule(r'ReinterpI32asF32')
def _r1(m, a, ts, rty): return 'LDV(float,&(u32){%s})' % a[0]
@rule(r'ReinterpF32asI32')
def _r2(m, a, ts, rty): return 'LDV(u32,&(float){%s})' % a[0]
@rule(r'ReinterpI64asF64')
def _r3(m, a, ts, rty): return 'LDV(double,&(u64){%s})' % a[0]
@rule(r'ReinterpF64asI64')
def _r4(m, a, ts, rty): return 'LDV(u64,&(double){%s})' % a[0]
@rule(r'F32toF64')
def _f32f64(m, a, ts, rty): return call('kh_f32tof64', a[0])
@rule(r'F64toF32')
def _f64f32(m, a, ts, rty): return call('kh_f64tof32', a[1])       # rounding mode argument ignored: RN (POLICY.md section 4)
@rule(r'(Neg|Abs)F(32|64)')
def _negabs(m, a, ts, rty):
    return call('kh_%sf%s' % (m.group(1).lower(), m.group(2)), a[0])
@rule(r'(Add|Sub|Mul|Div)F(32|64)')
def _fbin(m, a, ts, rty):
    return call('kh_%sf%s' % (m.group(1).lower(), m.group(2)), a[1], a[2])
@rule(r'SqrtF(32|64)')
def _fsqrt(m, a, ts, rty): return call('kh_sqrtf%s' % m.group(1), a[1])
@rule(r'F(32|64)toI(16|32|64)S')
def _f2i(m, a, ts, rty):
    return '(%s)f2i%s((double)%s,(int)%s)' % (U[int(m.group(2))], m.group(2), a[1], a[0])
@rule(r'I(32|64)StoF(32|64)')
def _i2f(m, a, ts, rty):
    T = 'float' if m.group(2) == '32' else 'double'; return '((%s)(%s)%s)' % (T, S[int(m.group(1))], a[-1])
@rule(r'I(32|64)UtoF(32|64)')
def _u2f(m, a, ts, rty):
    T = 'float' if m.group(2) == '32' else 'double'; return '((%s)(%s)%s)' % (T, U[int(m.group(1))], a[-1])
@rule(r'CmpF(32|64)')
def _cmpf(m, a, ts, rty): return '(u32)cmpf((double)%s,(double)%s)' % (a[0], a[1])
@rule(r'RoundF64toInt')
def _rnd(m, a, ts, rty): return '((double)rnd(%s,(int)%s))' % (a[1], a[0])

# ------------------------------------------------------------ V128 whole-vector / lane moves
@rule(r'(And|Or|Xor)V128')
def _vlogic(m, a, ts, rty):
    return '(%s %s %s)' % (a[0], {'And': '&', 'Or': '|', 'Xor': '^'}[m.group(1)], a[1])
@rule(r'NotV128')
def _vnot(m, a, ts, rty): return '(~%s)' % a[0]
@rule(r'V128to64')
def _v2l(m, a, ts, rty): return call('vx_V128to64', a[0])
@rule(r'V128HIto64')
def _v2h(m, a, ts, rty): return call('vx_V128HIto64', a[0])
@rule(r'V128to32')
def _v2w(m, a, ts, rty): return call('vx_V128to32', a[0])
@rule(r'64HLtoV128')
def _hl64(m, a, ts, rty): return call('vx_64HLtoV128', a[0], a[1])
@rule(r'(64|32)UtoV128')
def _utov(m, a, ts, rty): return call('vx_%sUtoV128' % m.group(1), a[0])
@rule(r'ZeroHI(96|64|112|120)ofV128')
def _zhi(m, a, ts, rty): return call('vx_ZeroHI%sofV128' % m.group(1), a[0])
@rule(r'SetV128lo(32|64)')
def _setlo(m, a, ts, rty): return call('vx_SetV128lo%s' % m.group(1), a[0], a[1])
@rule(r'(ShlV128|ShrV128)')
def _vshift(m, a, ts, rty): return call('vx_' + m.group(1), a[0], a[1])
@rule(r'Dup(8|16|32)x(16|8|4)')
def _dup(m, a, ts, rty): return call('vx_Dup%sx%s' % (m.group(1), m.group(2)), a[0])

# ------------------------------------------------------------ V128 float lane ops
def _strip_rm(a):
    return a[1:] if len(a) == 3 else a

@rule(r'(Add|Sub|Mul|Div|Min|Max)(32|64)F(x4|x2|0x4|0x2)')
def _vfbin(m, a, ts, rty):
    return call('vx_' + m.group(0), *_strip_rm(a))
@rule(r'Sqrt(32|64)F(x4|x2|0x4|0x2)')
def _vfsqrt(m, a, ts, rty):
    return call('vx_' + m.group(0), a[-1])
@rule(r'Cmp(EQ|LT|LE|UN)(32|64)F(x4|x2|0x4|0x2)')
def _vfcmp(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'(Abs|Neg)(32|64)Fx(4|2)')
def _vfabs(m, a, ts, rty): return call('vx_' + m.group(0), a[0])
@rule(r'(RecipEst|RSqrtEst)32(F0x4|Fx4)')
def _vrcp(m, a, ts, rty): return call('vx_' + m.group(0), a[0])
@rule(r'I32StoF32x4')
def _vi2f(m, a, ts, rty): return call('vx_I32StoF32x4', a[-1])
@rule(r'F32toI32Sx4(_RZ)?')
def _vf2i(m, a, ts, rty):
    if m.group(1): return call('vx_F32toI32Sx4', a[-1], '3')
    return call('vx_F32toI32Sx4', a[-1], '(int)' + a[0])
@rule(r'F32toF64x2')
def _vf32f64(m, a, ts, rty): return call('vx_F32toF64x2', a[0])
@rule(r'F64toF32x2')
def _vf64f32(m, a, ts, rty): return call('vx_F64toF32x2', a[-1])

# ------------------------------------------------------------ V128 integer lane ops
@rule(r'(Add|Sub|Mul)(8|16|32|64)x(16|8|4|2)')
def _viarith(m, a, ts, rty):
    if m.group(1) == 'Mul' and m.group(2) == '8': raise Unsupported('Mul8x16')
    return call('vx_' + m.group(0), a[0], a[1])
@rule(r'Cmp(EQ|GT)(8|16|32|64)(S|U)?x(16|8|4|2)')
def _vicmp(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'(Min|Max)(8|16|32|64)(S|U)x(16|8|4|2)')
def _vminmax(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'(Shl|Shr|Sar)N(8|16|32|64)x(16|8|4|2)')
def _vshiftn(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'Interleave(LO|HI)(8|16|32|64)x(16|8|4|2)')
def _vinter(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'(QNarrowBin16Sto8U|QNarrowBin16Sto8S|QNarrowBin32Sto16S|QNarrowBin32Sto16U|NarrowBin16to8|NarrowBin32to16)x(16|8)')
def _vnarrow(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])
@rule(r'GetMSBs8x16')
def _msb(m, a, ts, rty): return call('vx_GetMSBs8x16', a[0])
@rule(r'(CatEvenLanes|CatOddLanes)(8|16|32)x(16|8|4)')
def _vcat(m, a, ts, rty): return call('vx_' + m.group(0), a[0], a[1])

# ------------------------------------------------------------ 256-bit (V256 is a struct of two v128 halves)
@rule(r'V128HLtoV256')
def _v256hl(m, a, ts, rty): return '((v256){%s,%s})' % (a[1], a[0])      # (hi, lo)
@rule(r'V256toV128_0')
def _v256lo(m, a, ts, rty): return '(%s).lo' % a[0]
@rule(r'V256toV128_1')
def _v256hi(m, a, ts, rty): return '(%s).hi' % a[0]
@rule(r'64x4toV256')
def _64x4(m, a, ts, rty):
    # (w3, w2, w1, w0): most significant first
    return '((v256){vx_64HLtoV128(%s,%s),vx_64HLtoV128(%s,%s)})' % (a[2], a[3], a[0], a[1])
@rule(r'V256to64_(0|1|2|3)')
def _v256to64(m, a, ts, rty):
    k = int(m.group(1)); h = 'lo' if k < 2 else 'hi'
    return call('vx_V128HIto64' if k & 1 else 'vx_V128to64', '(%s).%s' % (a[0], h))
@rule(r'(And|Or|Xor)V256')
def _v256logic(m, a, ts, rty):
    sym = {'And': '&', 'Or': '|', 'Xor': '^'}[m.group(1)]
    return '({ v256 _x=%s,_y=%s; (v256){_x.lo %s _y.lo,_x.hi %s _y.hi}; })' % (a[0], a[1], sym, sym)
@rule(r'NotV256')
def _v256not(m, a, ts, rty): return '({ v256 _x=%s; (v256){~_x.lo,~_x.hi}; })' % a[0]
# lane-wise 256-bit ops = the 128-bit op applied to both halves (names: Add32Fx8 -> Add32Fx4, Sub64Fx4 -> Sub64Fx2, ...)
_W = {'Fx8': 'Fx4', 'Fx4': 'Fx2', 'x32': 'x16', 'x16': 'x8', 'x8': 'x4', 'x4': 'x2'}
@rule(r'(?P<op>(Add|Sub|Mul|Div|Min|Max)(32|64)F|Sqrt(32|64)F|Cmp(EQ|LT|LE|UN)(32|64)F)(?P<w>x8|x4)')
def _v256f(m, a, ts, rty):
    op = m.group('op'); w = m.group('w')
    if w == 'x8' and '64' in op: raise Unsupported('V256 lane shape')
    if w == 'x4' and '32' in op: raise Unsupported('V256 lane shape')
    narrow = 'x4' if w == 'x8' else 'x2'
    args = _strip_rm(a) if len(a) == 3 else a
    return _half2(op + narrow, args)
@rule(r'I32StoF32x8')
def _v256i2f(m, a, ts, rty):
    return '({ v256 _x=%s; (v256){vx_I32StoF32x4(_x.lo),vx_I32StoF32x4(_x.hi)}; })' % a[-1]
@rule(r'F32toI32Sx8(_RZ)?')
def _v256f2i(m, a, ts, rty):
    rm = '3' if m.group(1) else '(int)' + a[0]
    return '({ v256 _x=%s; (v256){vx_F32toI32Sx4(_x.lo,%s),vx_F32toI32Sx4(_x.hi,%s)}; })' % (a[-1], rm, rm)
def _half2(op, args):
    _NAMES.add('vx_' + op)
    if len(args) == 1:
        return '({ v256 _x=%s; (v256){vx_%s(_x.lo),vx_%s(_x.hi)}; })' % (args[0], op, op)
    return '({ v256 _x=%s,_y=%s; (v256){vx_%s(_x.lo,_y.lo),vx_%s(_x.hi,_y.hi)}; })' % (args[0], args[1], op, op)
@rule(r'(?P<op>(Add|Sub|Mul|Min|Max|Cmp(EQ|GT))(8|16|32|64)(S|U)?)x(?P<n>32|16|8|4)')
def _v256int(m, a, ts, rty):
    n = int(m.group('n')); w = int(m.group(3) if False else re.search(r'(8|16|32|64)', m.group('op')).group(1))
    if n * w != 256: raise Unsupported('V256 lane shape')
    return _half2(m.group(0).replace('x%d' % n, 'x%d' % (n // 2)), a)

# ------------------------------------------------------------ driver
def op_c(op, a, ts, rty):
    o = op[4:] if op.startswith('Iop_') else op
    for rx, fn in RULES:
        m = rx.fullmatch(o)
        if m:
            return fn(m, a, ts, rty)
    raise Unsupported(op)
