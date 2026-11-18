/* khrt.h : guest-ABI runtime interface for lifted kh2fm.exe code (VEX-IR -> C, route B).
 *
 * One uniform calling convention for every lifted function, runtime hook and import wrapper:
 *     void f(KCtx *g);
 * g holds ALL guest architectural state in the VEX AMD64 guest-state layout (offsets below are fixed and checked by
 * tools/selftest_layout.py against archinfo).  A guest `call` has already pushed its return address on the guest stack when
 * the callee runs; the callee's `ret` pops it.  Hand-written runtime functions follow the same rule (use KH_RET(g)).
 *
 * Guest memory model: flat 64-bit guest addresses that preserve the image's original addresses (image base 0x140000000).
 * host_address = XL(g, guest_address).  Three compile-time modes (KH_XLATE_MODE):
 *   0 identity (default)   guest memory is mapped at the same host addresses (x86-64 Linux oracle, macOS arm64 fixed map)
 *   1 constant bias        host = guest + KH_BIAS
 *   2 dynamic bias         host = guest + g->bias           (iOS or any host where the fixed range cannot be mapped)
 * Policy for NaN / denormal / min / max / rounding / libm: see ../POLICY.md.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   s8;  typedef int16_t  s16; typedef int32_t  s32; typedef int64_t  s64;
typedef unsigned __int128 u128; typedef __int128 s128;
typedef u8  v128 __attribute__((vector_size(16)));
typedef s8  v16i8 __attribute__((vector_size(16)));  typedef u8  v16u8 __attribute__((vector_size(16)));
typedef s16 v8i16 __attribute__((vector_size(16)));  typedef u16 v8u16 __attribute__((vector_size(16)));
typedef s32 v4i32 __attribute__((vector_size(16)));  typedef u32 v4u32 __attribute__((vector_size(16)));
typedef s64 v2i64 __attribute__((vector_size(16)));  typedef u64 v2u64 __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));  typedef double v2d __attribute__((vector_size(16)));
typedef struct { v128 lo, hi; } v256;          /* AVX ymm = two xmm halves (lo = bits 0..127) */
#define VZ ((v128){0})

#define KH_IMAGE_BASE 0x140000000ull

/* ---- guest context ------------------------------------------------------------------------------------------------ */
#define KH_ST_SIZE 2560
typedef struct KCtx {
    u8 st[KH_ST_SIZE] __attribute__((aligned(64)));   /* VEX AMD64 guest state */
    uintptr_t bias;      /* guest->host delta for KH_XLATE_MODE 2 */
    void *rt;            /* runtime private (thread / fiber record) */
    u64 aux[6];          /* runtime scratch (hooks) */
} KCtx;
/* VEX guest-state offsets (archinfo.ArchAMD64) */
enum { KO_RAX=16, KO_RCX=24, KO_RDX=32, KO_RBX=40, KO_RSP=48, KO_RBP=56, KO_RSI=64, KO_RDI=72,
       KO_R8=80, KO_R9=88, KO_R10=96, KO_R11=104, KO_R12=112, KO_R13=120, KO_R14=128, KO_R15=136,
       KO_CC_OP=144, KO_CC_DEP1=152, KO_CC_DEP2=160, KO_CC_NDEP=168, KO_DFLAG=176, KO_RIP=184,
       KO_SSEROUND=216, KO_YMM0=224 /* stride 64 */, KO_FS_CONST=208, KO_GS_CONST=2536 };
#define KH_R64(g,off) (*(u64*)((g)->st+(off)))
#define KH_RAX(g) KH_R64(g,KO_RAX)
#define KH_RCX(g) KH_R64(g,KO_RCX)
#define KH_RDX(g) KH_R64(g,KO_RDX)
#define KH_RBX(g) KH_R64(g,KO_RBX)
#define KH_RSP(g) KH_R64(g,KO_RSP)
#define KH_RBP(g) KH_R64(g,KO_RBP)
#define KH_RSI(g) KH_R64(g,KO_RSI)
#define KH_RDI(g) KH_R64(g,KO_RDI)
#define KH_R8(g)  KH_R64(g,KO_R8)
#define KH_R9(g)  KH_R64(g,KO_R9)
#define KH_GS(g)  KH_R64(g,KO_GS_CONST)
#define KH_XMM(g,i) (*(v128*)((g)->st+KO_YMM0+64*(i)))
/* a hand-written guest function returns to its guest caller by popping the return address (like `ret`) */
#define KH_RET(g) do{ KH_RSP(g)+=8; }while(0)
/* Win64 argument helpers: arg0..3 in rcx,rdx,r8,r9 ; return value in rax / xmm0 */
#define KH_ARG0(g) KH_RCX(g)
#define KH_ARG1(g) KH_RDX(g)
#define KH_ARG2(g) KH_R8(g)
#define KH_ARG3(g) KH_R9(g)

/* ---- address translation ------------------------------------------------------------------------------------------ */
#ifndef KH_XLATE_MODE
#define KH_XLATE_MODE 0
#endif
#if KH_XLATE_MODE==0
#define XL(g,a) ((uintptr_t)(a))
#elif KH_XLATE_MODE==1
#ifndef KH_BIAS
#error KH_BIAS required for KH_XLATE_MODE 1
#endif
#define XL(g,a) ((uintptr_t)(a)+(uintptr_t)(KH_BIAS))
#else
#define XL(g,a) ((uintptr_t)(a)+(g)->bias)
#endif

#ifdef TRACE_ON
#include <stdio.h>
#define TRACE(a) fprintf(stderr,"B %lx\n",(unsigned long)(a))
#else
#define TRACE(a) ((void)0)
#endif

/* ---- state access ------------------------------------------------------------------------------------------------- */
#define LD(T,a)  ({ T _v; memcpy(&_v,(const void*)XL(g,a),sizeof(T)); _v; })
#define ST(T,a,v) do{ T _v=(v); memcpy((void*)XL(g,a),&_v,sizeof(T)); }while(0)
#define LDV(T,p) ({ T _v; memcpy(&_v,(const void*)(p),sizeof(T)); _v; })     /* reinterpret a host value, no translation */
#define GET(T,off)   ({ T _v; memcpy(&_v,g->st+(off),sizeof(T)); _v; })
#define PUT(T,off,v) do{ T _v=(v); memcpy(g->st+(off),&_v,sizeof(T)); }while(0)
#define GETI(T,base,n,ix,bias) ({ T _v; u32 _i=((u32)(ix)+(u32)(bias))%(u32)(n); memcpy(&_v,g->st+(base)+_i*sizeof(T),sizeof(T)); _v; })
#define PUTI(T,base,n,ix,bias,v) do{ T _v=(v); u32 _i=((u32)(ix)+(u32)(bias))%(u32)(n); memcpy(g->st+(base)+_i*sizeof(T),&_v,sizeof(T)); }while(0)
#define KH_CAS(T,a,e,n) __sync_val_compare_and_swap((T*)XL(g,a),(T)(e),(T)(n))
#define KH_FENCE() __sync_synchronize()
#define SHL(T,N,x,a) ((a)>=N ? (T)0 : (T)((T)(x)<<(a)))
#define SHR(T,N,x,a) ((a)>=N ? (T)0 : (T)((T)(x)>>(a)))
#define SAR(S,N,x,a) ((S)(x) >> ((a)>=N ? N-1 : (a)))

/* ---- runtime services (implemented in khrt_core.c / the real runtime) -------------------------------------------- */
void kh_dispatch(KCtx *g, u64 target);                 /* indirect call / jump: guest address -> function; ABI as f(g) */
void kh_fault(KCtx *g, u64 va, const char *kind) __attribute__((noreturn));
void kh_cpuid(KCtx *g);                                /* reads EAX/ECX, writes EAX/EBX/ECX/EDX: fixed profile, POLICY.md section 6 */
void kh_xgetbv(KCtx *g);                               /* XCR0 = 7 (x87|SSE|AVX); writes EAX/EDX */
void kh_rdtsc(KCtx *g);
void kh_pause(KCtx *g);
void kh_emwarn(KCtx *g);
u64  kh_check_ldmxcsr(u64 mxcsr);
u64  kh_create_mxcsr(u64 sseround);
#define KH_FAULT(g,va,kind) kh_fault((g),(va),(kind))
#define KH_NORETURN_FALL(g,va) kh_fault((g),(va),"fall-out-of-function")

/* ---- float conversions (x86 "integer indefinite" for out-of-range) ------------------------------------------------ */
static inline double rnd(double x,int rm){ switch(rm){case 0: return nearbyint(x); case 1: return floor(x); case 2: return ceil(x); default: return trunc(x);} }
static inline s32 f2i32(double x,int rm){ double r=rnd(x,rm); if(!(r>=-2147483648.0 && r<2147483648.0)) return INT32_MIN; return (s32)r; }
static inline s64 f2i64(double x,int rm){ double r=rnd(x,rm); if(!(r>=-9223372036854775808.0 && r<9223372036854775808.0)) return INT64_MIN; return (s64)r; }
static inline s16 f2i16(double x,int rm){ double r=rnd(x,rm); if(!(r>=-32768.0 && r<32768.0)) return INT16_MIN; return (s16)r; }
static inline u32 cmpf(double a,double b){ if(isnan(a)||isnan(b)) return 0x45; if(a<b) return 0x01; if(a>b) return 0x00; return 0x40; }

/* ---- condition-code thunk (VEX amd64 lazy flags) ------------------------------------------------------------------ */
enum { CC_COPY=0, CC_ADD=1, CC_SUB=5, CC_ADC=9, CC_SBB=13, CC_LOGIC=17, CC_INC=21, CC_DEC=25, CC_SHL=29, CC_SHR=33, CC_ROL=37, CC_ROR=41, CC_UMUL=45, CC_SMUL=49 };
#define FCF 1u
#define FPF 4u
#define FAF 0x10u
#define FZF 0x40u
#define FSF 0x80u
#define FOF 0x800u
static inline u64 parity8(u64 v){ v&=0xff; v^=v>>4; v&=0xf; return ((0x6996>>v)&1)?0:FPF; }
static inline u64 rflags_all(u64 op,u64 d1,u64 d2,u64 nd){
  if(op==CC_COPY) return d1 & (FCF|FPF|FAF|FZF|FSF|FOF);
  int fam=(int)((op-1)/4)*4+1; int sz=(int)((op-1)%4); int bits=8<<sz; u64 mask= bits==64?~0ull:((1ull<<bits)-1); u64 msb=1ull<<(bits-1);
  u64 L=d1&mask,R=d2&mask,res=0,cf=0,of=0,af=0; u64 fl=0;
  switch(fam){
   case CC_ADD: res=(L+R)&mask; cf=res<L; of=(~(L^R)&(L^res)&msb)?1:0; af=(L^R^res)&0x10; break;
   case CC_SUB: res=(L-R)&mask; cf=L<R; of=((L^R)&(L^res)&msb)?1:0; af=(L^R^res)&0x10; break;
   case CC_ADC: { u64 oc=nd&1; u64 r=(R^oc)&mask; res=(L+r+oc)&mask; cf= oc? res<=L : res<L; of=(~(L^r)&(L^res)&msb)?1:0; af=(L^r^res)&0x10; break; }
   case CC_SBB: { u64 oc=nd&1; u64 r=(R^oc)&mask; res=(L-r-oc)&mask; cf= oc? L<=r : L<r; of=((L^r)&(L^res)&msb)?1:0; af=(L^r^res)&0x10; break; }
   case CC_LOGIC: res=L; break;
   case CC_INC: res=L; cf=nd&1; of=(res==msb); af=((res&0xf)==0); break;
   case CC_DEC: res=L; cf=nd&1; of=(res==((msb)-1)); af=((res&0xf)==0xf); break;
   case CC_SHL: res=L; cf=(R>>(bits-1))&1; of=((L^R)>>(bits-1))&1; break;
   case CC_SHR: res=L; cf=R&1; of=((L^R)>>(bits-1))&1; break;
   case CC_ROL: { res=L; u64 keep=nd&(FZF|FSF|FPF|FAF); fl=keep; cf=res&1; of=((res<<1)^res)>>(bits-1)&1; fl|=cf?FCF:0; fl|=of?FOF:0; return fl; }
   case CC_ROR: { res=L; u64 keep=nd&(FZF|FSF|FPF|FAF); fl=keep; cf=(res>>(bits-1))&1; of=((res<<1)^res)>>(bits-1)&1; fl|=cf?FCF:0; fl|=of?FOF:0; return fl; }
   case CC_UMUL: { u128 p=(u128)L*(u128)R; res=(u64)p&mask; u64 hi=(bits==64)?(u64)(p>>64):(u64)(p>>bits)&mask; cf=of=(hi!=0); break; }
   case CC_SMUL: { s64 a=(s64)(L<<(64-bits))>>(64-bits), b=(s64)(R<<(64-bits))>>(64-bits); s128 p=(s128)a*(s128)b; res=(u64)p&mask; s64 lo=(s64)(res<<(64-bits))>>(64-bits); cf=of=(p!=(s128)lo); break; }
   default: res=L; break;
  }
  fl = (cf?FCF:0)|(of?FOF:0)|(af?FAF:0)|(res==0?FZF:0)|((res&msb)?FSF:0)|parity8(res);
  return fl;
}
static inline u64 rflags_c(u64 op,u64 d1,u64 d2,u64 nd){ return rflags_all(op,d1,d2,nd)&FCF; }
static inline u64 calc_cond(u64 cond,u64 op,u64 d1,u64 d2,u64 nd){
  u64 f=rflags_all(op,d1,d2,nd); int cf=!!(f&FCF),zf=!!(f&FZF),sf=!!(f&FSF),of=!!(f&FOF),pf=!!(f&FPF); int r;
  switch(cond&~1ull){ case 0:r=of;break; case 2:r=cf;break; case 4:r=zf;break; case 6:r=cf|zf;break; case 8:r=sf;break; case 10:r=pf;break; case 12:r=sf^of;break; default:r=zf|(sf^of);break; }
  return (cond&1)? !r : r;
}
#include "khrt_simd.h"

/* ---- KH_ORDERED_MEM (task 39; Makefile ORDERED_MEM=1): x86 memory order for guest accesses on a weakly ordered host --------------
 * LD / ST above are plain memcpy.  x86 keeps the order of a thread's loads among themselves and of its stores among themselves (TSO):
 * a guest thread may publish data with plain stores (payload, then flag) and another may read flag, then payload, with no fence.
 * On arm64 plain loads and stores can be reordered (by the compiler too), so the reader can see the new flag and the old payload.
 * With -DKH_ORDERED_MEM=1 every naturally aligned 1/2/4/8-byte guest access is an acquire load / release store:
 *     arm64: LDAPR (the rcpc feature; macOS arm64 targets have it, the iOS SDK build adds +rcpc: Makefile OM_RCPC) else LDAR; STLR
 *     x86-64: the same MOVs as before plus compiler ordering (TSO already gives acquire/release), so this checks correctness and
 *             compile cost on Linux, not the arm64 cost
 * Unaligned accesses, 16- and 32-byte vector accesses and every other size keep memcpy.  The alignment is tested on the GUEST
 * address: the bias is a multiple of the page size (the guest space is an mmap; runtime/ordered_mem.c checks it), so the host
 * address has the same alignment and a constant guest address needs no run-time test.  KH_CAS and KH_FENCE are unchanged.
 * Every unit compiled this way carries a tag in the section kh_om (runtime/ordered_mem.c counts them: the selftest shows the lifted
 * shards really are ordered) and references kh_ordered_mem_runtime_v1, which only a runtime built the same way defines: an ordered
 * object linked with a plain runtime does not link.  With KH_ORDERED_MEM unset nothing below exists and LD / ST are unchanged. */
#if defined(KH_ORDERED_MEM) && KH_ORDERED_MEM
#if KH_XLATE_MODE==1
_Static_assert((((uintptr_t)(KH_BIAS))&4095u)==0,"KH_ORDERED_MEM tests alignment on the guest address: KH_BIAS must be a multiple of 4096");
#endif
#define KH_OM_INL static inline __attribute__((always_inline))
KH_OM_INL u8  kh_om_ld8 (uintptr_t h,u64 ga){ (void)ga; return __atomic_load_n((const u8*)h,__ATOMIC_ACQUIRE); }
KH_OM_INL u16 kh_om_ld16(uintptr_t h,u64 ga){ u16 v; if(!(ga&1)) return __atomic_load_n((const u16*)h,__ATOMIC_ACQUIRE); memcpy(&v,(const void*)h,2); return v; }
KH_OM_INL u32 kh_om_ld32(uintptr_t h,u64 ga){ u32 v; if(!(ga&3)) return __atomic_load_n((const u32*)h,__ATOMIC_ACQUIRE); memcpy(&v,(const void*)h,4); return v; }
KH_OM_INL u64 kh_om_ld64(uintptr_t h,u64 ga){ u64 v; if(!(ga&7)) return __atomic_load_n((const u64*)h,__ATOMIC_ACQUIRE); memcpy(&v,(const void*)h,8); return v; }
KH_OM_INL void kh_om_st8 (uintptr_t h,u64 ga,u8 v){ (void)ga; __atomic_store_n((u8*)h,v,__ATOMIC_RELEASE); }
KH_OM_INL void kh_om_st16(uintptr_t h,u64 ga,u16 v){ if(!(ga&1)){ __atomic_store_n((u16*)h,v,__ATOMIC_RELEASE); return; } memcpy((void*)h,&v,2); }
KH_OM_INL void kh_om_st32(uintptr_t h,u64 ga,u32 v){ if(!(ga&3)){ __atomic_store_n((u32*)h,v,__ATOMIC_RELEASE); return; } memcpy((void*)h,&v,4); }
KH_OM_INL void kh_om_st64(uintptr_t h,u64 ga,u64 v){ if(!(ga&7)){ __atomic_store_n((u64*)h,v,__ATOMIC_RELEASE); return; } memcpy((void*)h,&v,8); }
/* the size branches are compile-time constants (sizeof); the byte count of the copies is clamped so that the dead branches of a
 * type of another size stay well-formed */
#define KH_OM_N(T,I) (sizeof(T)<sizeof(I)?sizeof(T):sizeof(I))
#undef LD
#undef ST
#define LD(T,a) ({ T _v; u64 _ga=(u64)(a); uintptr_t _h=XL(g,_ga); \
    if(sizeof(T)==1){ u8 _x=kh_om_ld8(_h,_ga); memcpy(&_v,&_x,KH_OM_N(T,u8)); } \
    else if(sizeof(T)==2){ u16 _x=kh_om_ld16(_h,_ga); memcpy(&_v,&_x,KH_OM_N(T,u16)); } \
    else if(sizeof(T)==4){ u32 _x=kh_om_ld32(_h,_ga); memcpy(&_v,&_x,KH_OM_N(T,u32)); } \
    else if(sizeof(T)==8){ u64 _x=kh_om_ld64(_h,_ga); memcpy(&_v,&_x,KH_OM_N(T,u64)); } \
    else memcpy(&_v,(const void*)_h,sizeof(T)); _v; })
#define ST(T,a,v) do{ T _v=(v); u64 _ga=(u64)(a); uintptr_t _h=XL(g,_ga); \
    if(sizeof(T)==1){ u8 _x=0; memcpy(&_x,&_v,KH_OM_N(T,u8)); kh_om_st8(_h,_ga,_x); } \
    else if(sizeof(T)==2){ u16 _x=0; memcpy(&_x,&_v,KH_OM_N(T,u16)); kh_om_st16(_h,_ga,_x); } \
    else if(sizeof(T)==4){ u32 _x=0; memcpy(&_x,&_v,KH_OM_N(T,u32)); kh_om_st32(_h,_ga,_x); } \
    else if(sizeof(T)==8){ u64 _x=0; memcpy(&_x,&_v,KH_OM_N(T,u64)); kh_om_st64(_h,_ga,_x); } \
    else memcpy((void*)_h,&_v,sizeof(T)); }while(0)
/* which instructions this unit's accesses compile to (the report line of runtime/ordered_mem.c) */
#if defined(__aarch64__) && defined(__ARM_FEATURE_RCPC)
#define KH_OM_INSNS "arm64: LDAPR (rcpc) / STLR"
#elif defined(__aarch64__)
#define KH_OM_INSNS "arm64: LDAR / STLR (this target has no rcpc)"
#elif defined(__x86_64__)
#define KH_OM_INSNS "x86-64: MOV (TSO) + compiler ordering"
#else
#define KH_OM_INSNS "__atomic acquire / release"
#endif
#if defined(__APPLE__)
#define KH_OM_SECTION "__DATA,__kh_om"
#else
#define KH_OM_SECTION "kh_om"
#endif
#ifdef __BASE_FILE__
#define KH_OM_FILE __BASE_FILE__                   /* the translation unit (not this header); the counter takes its last path component */
#else
#define KH_OM_FILE __FILE__
#endif
static const char kh_om_tu_tag[] __attribute__((used,section(KH_OM_SECTION))) = "kh-om1:" KH_OM_FILE;
extern const char kh_ordered_mem_runtime_v1;
static const char *const kh_om_tu_need __attribute__((used)) = &kh_ordered_mem_runtime_v1;
#endif
