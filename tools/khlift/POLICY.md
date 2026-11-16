# khlift policy: what lifted code promises, and how

One policy for floating point, memory, calls and CPU identity. Everything here is implemented in `rt/khrt.h`,
`rt/khrt_simd.h`, `rt/khrt_cpu.c`, `rt/rt_libm.c` and the emitter (`khlift/emit.py`, `khlift/ops.py`), and every claim
marked (tested) has a test in `tools/tests/` or `tools/difftest.py`.

## 1. Guest ABI

* Every lifted function, runtime hook and import wrapper is `void f(KCtx *g)`.
* `KCtx` holds all guest architectural state in the VEX AMD64 guest-state layout (`st[2560]`; offsets are fixed:
  rax=16, rcx=24, rdx=32, rbx=40, rsp=48, rbp=56, rsi=64, rdi=72, r8..r15=80..136, cc_op=144.., dflag=176, xmm/ymm i = 224+64*i,
  gs_const=2536) followed by `bias`, `rt` (runtime pointer) and `aux[6]`.
* A guest `call` is the original instruction sequence: the IR pushes the return address on the guest stack, then the C code calls
  `f_x(g)`; the callee's `ret` pops it (`return;`). Hand-written functions pop with `KH_RET(g)`. So rsp, callee-saved registers,
  stack contents and the Win64 argument/return registers (rcx,rdx,r8,r9, xmm0-3 / rax, xmm0) are exactly the original's.
* Symbols: `f_<rva hex>` for universe functions; `rt_imp_<dll>__<name>` for the 368 PE imports; `rt_hook_*` for the hand-written
  replacements listed in `tools/hooks.tsv` (currently the two fiber yield leaves 0x150060 and 0x150080, which are not lifted).
* Indirect call / jump / tail jump through a register or non-import memory operand: `kh_dispatch(g, target)`.
  `call [IAT slot]` / `jmp [IAT slot]` of a known import is a direct call of `rt_imp_...`. Targets of `kh_dispatch` are the
  38,455 universe function starts, plus synthetic import addresses `KH_IMPORT_ADDR_BASE + 16*i` (the loader stores these in the
  IAT slots). Anything else faults (`kh_fault`).
* MSVC jump tables: a `switch` over the target addresses the universe resolved for that `jmp reg` site (522 tables).
  The universe bounds a table by the nearest `cmp`/`and`; two tables (f_472110 site 0x4721a3, f_374b00 site 0x374c53) were
  too short and are extended over the adjacent `unreached-data` chunk (`universe.extend_jump_tables`). The `default:` case is a fault.
* Not lifted: `.nep` CLR native-entry units (211; generated as `kh_nep_stub` aborts), CLR IL method bodies and data-in-code
  (never decoded), code that is not reachable by control flow from a function entry (SEH `__except` / catch-continuation
  labels: the EH policy is terminate-on-throw, see G1-GATE.md). `Ijk_SigTRAP` (int3), `int 0x29`, `ud2`, SIGFPE-on-div0 exits and
  the alignment checks VEX adds to `movaps` become `KH_FAULT`.

## 2. Address model

Flat 64-bit guest addresses that keep the image's addresses (image base `0x140000000`); `host = XL(g, guest)`.
`KH_XLATE_MODE` (compile-time): 0 identity (default), 1 constant `KH_BIAS`, 2 dynamic `g->bias`.
TLS: `gs:[0x58]` lowers to `GET(gs_const) + 0x58`; the runtime sets `st[2536]` per thread (165 sites in the exe, all counted).
Every access is `memcpy` (no alignment or strict-aliasing assumptions; build with `-fno-strict-aliasing`).

## 3. NaN, min, max, compare

x86 SSE semantics are reproduced bit-for-bit on every host (tested: `tests/nanmicro.c`, 10,998 operand pairs).
* A NaN-producing add/sub/mul/div/sqrt returns the first operand if it is a NaN (quieted), else the second if it is a NaN
  (quieted), else the default QNaN `0xFFC00000` / `0xFFF8000000000000` (sign set).
* This is an explicit fix-up (`KH_NANFIX=1`, the default, on x86-64 too): the host's own NaN choice is never used because
  arm64 differs from x86 (positive default NaN, signaling-NaN preference) and because gcc/clang may commute the operands of a
  commutative add/mul even on x86-64.
* `min`/`max`: the literal ternary `a<b?a:b` / `a>b?a:b` (second operand returned on NaN or equality, x86 `minps/maxps`),
  never `fmin/fmax`.
* Compares: ordered/quiet C comparisons; `UN` = `isnan(a)||isnan(b)`; `cmpf` for ucomiss/comiss flags.
* `cvtss2sd` / `cvtsd2ss` on NaN: sign and payload preserved, quiet bit set.

## 4. Rounding, denormals, conversions

* Round-to-nearest-even, MXCSR architecturally `0x1F80` (all exceptions masked, DAZ=FTZ=0). `ldmxcsr` is lifted
  (`kh_check_ldmxcsr`/`kh_create_mxcsr`); changing the rounding mode affects float-to-int conversions (the VEX rounding-mode
  operand is honoured) but not arithmetic, which is always RN. The exe has one MXCSR setter, f_447d70 (the CRT's floating-point control routine, 2 `ldmxcsr`
  sites; the `ecx==-1` form ORs in `0x9fc0` = FTZ|DAZ|all masks). It has no direct caller in the universe. If it is ever reached,
  the FTZ/DAZ request is ignored (`kh_check_ldmxcsr` reports an emulation warning, `kh_emwarn` is a no-op) so the policy above holds.
* Full IEEE gradual underflow: no flush-to-zero, no denormals-are-zero. The runtime must keep the host FPU in that mode
  (arm64: `FPCR.FZ=0`, the default on macOS and iOS) (tested: denormal operands/results in `nanmicro`, 0 differences).
* Float to int: out of range / NaN gives the x86 "integer indefinite" (`0x80000000`, `0x8000000000000000`), not the host's
  saturation (arm64 saturates: tested, differs on 20 of the micro-test lines without the policy).
* Float arithmetic is compiled with `-ffp-contract=off` (no FMA fusing), `-fno-math-errno`, no `-ffast-math`.

## 5. rcpps / rsqrtps

26 instruction sites in 22 functions (`work/rcp_sites.json`), every one followed by a Newton-Raphson step except one. The
refined result still depends on the low bits of the estimate, so the estimate is modelled **bit-exactly**: on silicon the result
depends only on sign, exponent and the top 12 mantissa bits; `rt/khrt_rcp_tab.c` holds 3 x 4096 measured results
(rcp, rsqrt even exponent, rsqrt odd exponent) and `kh_rcp32` / `kh_rsqrt32` shift the exponent and handle zero/denormal/inf/NaN.
Verified against hardware for all 2^32 inputs (`tests/rcp_check_header.c`: 0 mismatches). The table is **AMD Zen 3 / Zen 4**
(hash of all 2^32 results identical on both); Intel's table differs (known; regenerate with `tests/gen_rcp_tab.c` on an Intel
host if the target should be Intel silicon: the model and the code do not change).

## 6. CPUID, XGETBV, RDTSC

`kh_cpuid` returns a fixed profile ("KHNMC x86-64 AVX2 profile"): vendor `GenuineIntel`, family 6 model 0x9E (eax 0x906EA, not one of
the Atom models the CRT special-cases), max basic leaf 0x16; leaf 1 ecx: SSE3 PCLMUL SSSE3 FMA CX16 SSE4.1 SSE4.2 MOVBE POPCNT AES
XSAVE OSXSAVE AVX F16C; edx `0xBFEBFBFF`; leaf 7 ebx: BMI1 AVX2 BMI2 ERMS; LZCNT in 0x80000001. **No AVX-512, no RDRAND/RDSEED.**
`kh_xgetbv` returns XCR0 = 7 (x87|SSE|AVX); pyvex bakes `0xE7` (AVX-512 state) into the IR, so the emitter replaces every
`xgetbv` instruction (3 sites in 3 functions) by the runtime call. The exe's own checks (`__isa_available_init` at 0x43a4bc) then set
`__isa_available = 5` (AVX2), so the AVX2 CRT paths and the 9 AVX functions are reachable and lifted (256-bit vectors are
`struct {v128 lo, hi}`, tested against the CPU: 9 functions, 0 mismatches). `kh_rdtsc` = monotonic nanoseconds.

## 7. libm

The CRT math imports (`api-ms-win-crt-math`: sinf cosf tanf asinf acosf atanf atan2f expf logf log10f powf fmodf floorf ceilf roundf
sqrtf and the double versions, modf frexp ldexp _isnan _finite) are named runtime functions in `rt/rt_libm.c`. Single-precision
functions are evaluated in double precision and rounded once to float, which makes the result independent of the host's own
`sinf` (glibc, Apple libm); this is correctly rounded except in the rare double-rounding case. It is **not** guaranteed to match
UCRT bit for bit; if a differential against the original shows a difference, swap the body of one wrapper, nothing else changes.
`sqrtf`/`sqrt` are exact IEEE (`kh_sqrtf32`).

## 8. Build flags

`-O1 -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -fno-math-errno -ffp-contract=off` (see `buildgen.py`).
