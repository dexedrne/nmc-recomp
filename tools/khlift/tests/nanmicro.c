/* nanmicro.c : NaN / denormal / min-max / conversion micro-test for POLICY.md.
 * For every operation and every pair of special values it prints
 *     <op> <a> <b> <host_raw> <policy> [<x86_ref>]
 *  host_raw : the plain C operator of the host compiler/CPU (what an unpoliced lift would do)
 *  policy   : the khrt kh_ and vx_ helpers that emitted code uses (KH_NANFIX=1)
 *  x86_ref  : (x86-64 build only) the real SSE instruction via inline asm, destination = first operand
 * Expected: policy == x86_ref on x86-64, and the policy column is IDENTICAL on every architecture (diff the outputs).
 * build:  cc -O1 -fno-strict-aliasing -ffp-contract=off -fno-math-errno -I ../rt nanmicro.c -o nanmicro */
#include "khrt.h"
#include <stdio.h>
static const u32 F[] = {0x00000000,0x80000000,0x3f800000,0xbf800000,0x40600000,0x7f7fffff,0x00800000,0x00000001,0x007fffff,0x00400000,
  0x80000001,0x7f800000,0xff800000,0x7fc00000,0x7fc12345,0xffc00000,0x7f800001,0x7fa00000,0xff800001,0x3f800001,0x4b000000,0xcf000000,0x4f000000,0x5f000000,0x7f000000,0x3fc00000};
static const u64 D[] = {0x0000000000000000ull,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,0x400c000000000000ull,0x7fefffffffffffffull,
  0x0010000000000000ull,0x0000000000000001ull,0x000fffffffffffffull,0x0008000000000000ull,0x8000000000000001ull,0x7ff0000000000000ull,0xfff0000000000000ull,
  0x7ff8000000000000ull,0x7ff8123456789abcull,0xfff8000000000000ull,0x7ff0000000000001ull,0x7ff4000000000000ull,0xfff0000000000001ull,0x3ff0000000000001ull,
  0x4330000000000000ull,0xc1e0000000000000ull,0x41e0000000000000ull,0x43e0000000000000ull,0x7fe0000000000000ull,0x3ff8000000000000ull};
#define NF (sizeof F/sizeof F[0])
#define ND (sizeof D/sizeof D[0])
static inline float f_(u32 u){ return kh_bf32(u); }
static inline double d_(u64 u){ return kh_bf64(u); }
#ifdef __x86_64__
#define REF2(inst,T,a,b) ({ T _a=(a); __asm__("" : "+x"(_a)); T _b=(b); __asm__ volatile(inst " %1, %0" : "+x"(_a) : "x"(_b)); _a; })
#define REF1(inst,T,a) ({ T _r; __asm__ volatile(inst " %1, %0" : "=x"(_r) : "x"((T)(a))); _r; })
#endif
static unsigned long nline, nmism, nraw;
static void P32(const char *op,u32 a,u32 b,u32 raw,u32 pol,int hasref,u32 ref){
    printf("%s %08x %08x %08x %08x",op,a,b,raw,pol); if(hasref) printf(" %08x%s",ref,ref==pol?"":" REFMISMATCH"); printf("\n");
    nline++; if(hasref&&ref!=pol) nmism++; if(raw!=pol) nraw++; }
static void P64(const char *op,u64 a,u64 b,u64 raw,u64 pol,int hasref,u64 ref){
    printf("%s %016llx %016llx %016llx %016llx",op,(unsigned long long)a,(unsigned long long)b,(unsigned long long)raw,(unsigned long long)pol);
    if(hasref) printf(" %016llx%s",(unsigned long long)ref,ref==pol?"":" REFMISMATCH"); printf("\n");
    nline++; if(hasref&&ref!=pol) nmism++; if(raw!=pol) nraw++; }
#ifdef __x86_64__
#define X86(...) __VA_ARGS__
#define HAS 1
#else
#define X86(...)
#define HAS 0
#define REF2(i,T,a,b) (a)
#define REF1(i,T,a) (a)
#endif
#define BIN32(NAME,RAWEXPR,POL,INST) for(size_t i=0;i<NF;i++) for(size_t j=0;j<NF;j++){ float a=f_(F[i]),b=f_(F[j]); volatile float ra=a,rb=b; float raw=(RAWEXPR); \
    float pol=(POL); u32 ref=0; X86(ref=kh_fb32(REF2(INST,float,a,b));) P32(NAME,F[i],F[j],kh_fb32(raw),kh_fb32(pol),HAS,ref); }
#define BIN64(NAME,RAWEXPR,POL,INST) for(size_t i=0;i<ND;i++) for(size_t j=0;j<ND;j++){ double a=d_(D[i]),b=d_(D[j]); volatile double ra=a,rb=b; double raw=(RAWEXPR); \
    double pol=(POL); u64 ref=0; X86(ref=kh_fb64(REF2(INST,double,a,b));) P64(NAME,D[i],D[j],kh_fb64(raw),kh_fb64(pol),HAS,ref); }
int main(void){
    /* scalar single/double arithmetic */
    BIN32("addss",ra+rb,kh_addf32(a,b),"addss") BIN32("subss",ra-rb,kh_subf32(a,b),"subss") BIN32("mulss",ra*rb,kh_mulf32(a,b),"mulss") BIN32("divss",ra/rb,kh_divf32(a,b),"divss")
    BIN32("minss",ra<rb?ra:rb,kh_min32(a,b),"minss") BIN32("maxss",ra>rb?ra:rb,kh_max32(a,b),"maxss")
    BIN64("addsd",ra+rb,kh_addf64(a,b),"addsd") BIN64("subsd",ra-rb,kh_subf64(a,b),"subsd") BIN64("mulsd",ra*rb,kh_mulf64(a,b),"mulsd") BIN64("divsd",ra/rb,kh_divf64(a,b),"divsd")
    BIN64("minsd",ra<rb?ra:rb,kh_min64(a,b),"minsd") BIN64("maxsd",ra>rb?ra:rb,kh_max64(a,b),"maxsd")
    for(size_t i=0;i<NF;i++){ float a=f_(F[i]); volatile float ra=a; float raw=sqrtf(ra); float pol=kh_sqrtf32(a); u32 ref=0; X86(ref=kh_fb32(REF1("sqrtss",float,a));) P32("sqrtss",F[i],0,kh_fb32(raw),kh_fb32(pol),HAS,ref); }
    for(size_t i=0;i<ND;i++){ double a=d_(D[i]); volatile double ra=a; double raw=sqrt(ra); double pol=kh_sqrtf64(a); u64 ref=0; X86(ref=kh_fb64(REF1("sqrtsd",double,a));) P64("sqrtsd",D[i],0,kh_fb64(raw),kh_fb64(pol),HAS,ref); }
    /* conversions */
    for(size_t i=0;i<NF;i++){ float a=f_(F[i]); volatile float ra=a; double raw=(double)ra; double pol=kh_f32tof64(a); u64 ref=0; X86(double r; __asm__ volatile("cvtss2sd %1,%0":"=x"(r):"x"(a)); ref=kh_fb64(r);) P64("cvtss2sd",F[i],0,kh_fb64(raw),kh_fb64(pol),HAS,ref); }
    for(size_t i=0;i<ND;i++){ double a=d_(D[i]); volatile double ra=a; float raw=(float)ra; float pol=kh_f64tof32(a); u32 ref=0; X86(float r; __asm__ volatile("cvtsd2ss %1,%0":"=x"(r):"x"(a)); ref=kh_fb32(r);) P32("cvtsd2ss",(u32)(D[i]>>32),(u32)D[i],kh_fb32(raw),kh_fb32(pol),HAS,ref); }
    for(size_t i=0;i<NF;i++){ float a=f_(F[i]); volatile float ra=a; s32 raw=(s32)ra; s32 pol=f2i32((double)a,3); u32 ref=0; X86(int r; __asm__ volatile("cvttss2si %1,%0":"=r"(r):"x"(a)); ref=(u32)r;) P32("cvttss2si",F[i],0,(u32)raw,(u32)pol,HAS,ref); }
    for(size_t i=0;i<NF;i++){ float a=f_(F[i]); volatile float ra=a; s32 raw=(s32)nearbyintf(ra); s32 pol=f2i32((double)a,0); u32 ref=0; X86(int r; __asm__ volatile("cvtss2si %1,%0":"=r"(r):"x"(a)); ref=(u32)r;) P32("cvtss2si",F[i],0,(u32)raw,(u32)pol,HAS,ref); }
    for(size_t i=0;i<ND;i++){ double a=d_(D[i]); volatile double ra=a; s64 raw=(s64)ra; s64 pol=f2i64(a,3); u64 ref=0; X86(long long r; __asm__ volatile("cvttsd2si %1,%0":"=r"(r):"x"(a)); ref=(u64)r;) P64("cvttsd2si64",D[i],0,(u64)raw,(u64)pol,HAS,ref); }
    /* packed ops through the exact helpers used by emitted code (lane 0 and lane 3 hold the pair) */
    for(size_t i=0;i<NF;i++) for(size_t j=0;j<NF;j++){
        v4u32 A={F[i],F[j],F[i],F[j]}, B={F[j],F[i],F[j],F[i]};
        v128 mn=vx_Min32Fx4((v128)A,(v128)B), mx=vx_Max32Fx4((v128)A,(v128)B), ad=vx_Add32Fx4((v128)A,(v128)B), ml=vx_Mul32Fx4((v128)A,(v128)B);
        v4f fa=(v4f)A, fb=(v4f)B; volatile v4f va=fa, vb=fb; v4f rmn=va[0]<vb[0]?va:vb, rmx=va[0]>vb[0]?va:vb, rad=va+vb, rml=va*vb;
        u32 ref=0; X86({ v4u32 r=A; __asm__ volatile("minps %1,%0":"+x"(r):"x"(B)); ref=r[0]; })
        P32("minps",F[i],F[j],((v4u32)rmn)[0],((v4u32)mn)[0],HAS,ref);
        X86({ v4u32 r=A; __asm__ volatile("maxps %1,%0":"+x"(r):"x"(B)); ref=r[0]; })
        P32("maxps",F[i],F[j],((v4u32)rmx)[0],((v4u32)mx)[0],HAS,ref);
        X86({ v4u32 r=A; __asm__ volatile("addps %1,%0":"+x"(r):"x"(B)); ref=r[0]; })
        P32("addps",F[i],F[j],((v4u32)rad)[0],((v4u32)ad)[0],HAS,ref);
        X86({ v4u32 r=A; __asm__ volatile("mulps %1,%0":"+x"(r):"x"(B)); ref=r[0]; })
        P32("mulps",F[i],F[j],((v4u32)rml)[0],((v4u32)ml)[0],HAS,ref);
    }
    fprintf(stderr,"nanmicro: %lu lines, %lu policy-vs-x86-ref mismatches, %lu lines where host_raw != policy\n",nline,nmism,nraw);
    return nmism?1:0;
}
