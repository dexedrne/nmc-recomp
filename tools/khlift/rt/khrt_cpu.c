/* khrt_cpu.c : CPU-identity and misc instruction services for lifted code (portable C, no host asm required).
 * CPUID profile = fixed "KHNMC x86-64 AVX2 profile" (POLICY.md section 6): GenuineIntel family 6 model 0x9E, SSE..SSE4.2,
 * POPCNT, AES, PCLMUL, FMA, F16C, MOVBE, XSAVE/OSXSAVE, AVX, AVX2, BMI1, BMI2, ERMS, LZCNT; NO AVX-512, NO RDRAND/RDSEED.
 * XGETBV returns XCR0 = 7 (x87 | SSE | AVX).  The vendor string is Intel so the CRT's __isa_available_init takes its normal
 * path (AVX2 => __isa_available = 5). */
#include "khrt.h"
#include <time.h>

#define KH_CPUID_MAXLEAF 0x16u
static const u32 ECX1 = (1u<<0)|(1u<<1)|(1u<<9)|(1u<<12)|(1u<<13)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<23)|(1u<<25)|(1u<<26)|(1u<<27)|(1u<<28)|(1u<<29);
static const u32 EDX1 = 0xBFEBFBFFu;
static const u32 EBX7 = (1u<<3)|(1u<<5)|(1u<<8)|(1u<<9);
static const char KH_BRAND[48] = "KHNMC lifted x86-64 AVX2 profile";

void kh_cpuid(KCtx *g){
    u32 leaf = (u32)KH_RAX(g), sub = (u32)KH_RCX(g), a=0,b=0,c=0,d=0;
    (void)sub;
    switch(leaf){
    case 0: a=KH_CPUID_MAXLEAF; b=0x756e6547u; d=0x49656e69u; c=0x6c65746eu; break;       /* "GenuineIntel" (ebx,edx,ecx) */
    case 1: a=0x000906EAu; b=0x00100800u; c=ECX1; d=EDX1; break;
    case 7: if(sub==0){ a=0; b=EBX7; c=0; d=0; } break;
    case 0x80000000u: a=0x80000008u; break;
    case 0x80000001u: c=(1u<<0)|(1u<<5)|(1u<<8); d=(1u<<11)|(1u<<20)|(1u<<27)|(1u<<29); break;
    case 0x80000002u: case 0x80000003u: case 0x80000004u: {
        const u32 *p=(const u32*)(KH_BRAND+16*(leaf-0x80000002u)); a=p[0]; b=p[1]; c=p[2]; d=p[3]; break; }
    default: break;
    }
    KH_R64(g,KO_RAX)=a; KH_R64(g,KO_RBX)=b; KH_R64(g,KO_RCX)=c; KH_R64(g,KO_RDX)=d;
}
void kh_xgetbv(KCtx *g){ KH_R64(g,KO_RAX)=7; KH_R64(g,KO_RDX)=0; }
void kh_rdtsc(KCtx *g){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    u64 t=(u64)ts.tv_sec*1000000000ull+(u64)ts.tv_nsec; KH_R64(g,KO_RAX)=(u32)t; KH_R64(g,KO_RDX)=t>>32;
}
void kh_pause(KCtx *g){ (void)g; __asm__ __volatile__("" ::: "memory"); }
void kh_emwarn(KCtx *g){ (void)g; }
u64 kh_check_ldmxcsr(u64 mxcsr){
    u64 rm=(mxcsr>>13)&3, ew=((mxcsr&0x1f80u)!=0x1f80u || (mxcsr&0x8040u)) ? 2 : 0;
    return rm | (ew<<32);
}
u64 kh_create_mxcsr(u64 sseround){ return 0x1f80u | ((sseround&3)<<13); }
