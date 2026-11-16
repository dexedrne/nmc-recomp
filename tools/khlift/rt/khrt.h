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
