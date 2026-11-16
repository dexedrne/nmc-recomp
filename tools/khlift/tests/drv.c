/* drv.c : cross-architecture determinism driver for lifted functions (the SAME C compiled for x86-64 and arm64).
 *
 * For every function in drv_list.h it runs NT seeded tests against a sandboxed guest address space and prints one line:
 *     F <rva> ok=<n> fault=<n> timeout=<n> sum=<hash of all ok results>
 * Guest memory: one PROT_NONE reservation, guest address g lives at host base+g (KH_XLATE_MODE 2: g->bias = base).
 * Regions: IMAGE 0x140000000 (zero-filled, no game data needed) / ARENA 0x10000000 / STACK 0x20000000.  Everything else is
 * PROT_NONE so wild pointers fault identically on every host.  Callees are the deterministic test stubs of khtest_portable.c,
 * so a lifted function's result depends only on its own code and on the host's FP/NaN behaviour.
 * Diff the output of an x86-64 build against an arm64 build: any differing line is a cross-architecture mismatch. */
#define _GNU_SOURCE
#include "khrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include "drv_list.h"
#define ARENA 0x10000000ull
#define ARENA_SZ 0x40000ull
#define STACK 0x20000000ull
#define STACK_SZ 0x20000ull
#define IMAGE KH_IMAGE_BASE
#define IMAGE_SZ 0x2c2b000ull
#define RSP0 (STACK+0x10008ull)
#define SENT 0x30000000ull
#define GSB_ADDR (ARENA+0x20000ull)
#define NT 6
void kh_test_stub(KCtx *g, u64 va);
void khtest_config(const u64 *set, size_t n, u64 alo, u64 ahi);
void khtest_reset(void);
static u64 base;
static u64 rs=0x243F6A8885A308D3ull;
static u64 prng(void){ rs+=0x9E3779B97F4A7C15ull; u64 x=rs; x=(x^(x>>30))*0xBF58476D1CE4E5B9ull; x=(x^(x>>27))*0x94D049BB133111EBull; return x^(x>>31); }
static u32 fl(void){ u64 k=prng()%100; u32 r=(u32)prng();
    if(k<70){ int v=(int)(prng()%2001)-1000; float f=(float)v/16.0f; return kh_fb32(f); }
    if(k<80) return kh_fb32((float)((int)(prng()%17)-8));
    if(k<85){ static const u32 sp[]={0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00000,0xffc00000,0x7f800001,0x7fa00000,0x00000001,0x007fffff,0x00800000,0x7f7fffff,0x4f000000,0xcf000000,0x7fc12345}; return sp[prng()%17]; }
    if(k<92) return r&0x807fffff;                 /* denormals */
    return r; }
static u64 gpr(void){ u64 k=prng()%100; if(k<60) return ARENA+0x1000+16*(prng()%0x1c00); if(k<76) return prng()%65; if(k<88) return (u32)prng();
    static const u64 sp[]={0,1,0xffffffffull,0x80000000ull,0x7fffffffull,~0ull}; return sp[prng()%6]; }
static void fill(u8 *p, size_t n){ for(size_t i=0;i+4<=n;i+=4){ u32 w; u64 k=prng()%100; if(k<40) w=fl(); else if(k<60) w=(u32)(prng()%65); else if(k<75) w=(u32)prng(); else w=fl(); memcpy(p+i,&w,4); }
    for(size_t i=0;i+8<=n;i+=8){ u64 k=prng()%100; if(k<25){ u64 v=ARENA+0x1000+16*(prng()%0x1c00); memcpy(p+i,&v,8); } } }
static u64 fnv(u64 h,const void *p,size_t n){ const u8 *b=p; for(size_t i=0;i<n;i++){ h^=b[i]; h*=1099511628211ull; } return h; }
static sigjmp_buf jb; static volatile int sigcode;
static void on_sig(int s){ sigcode=s; siglongjmp(jb,1); }
void kh_fault(KCtx *g,u64 va,const char *k){ (void)g;(void)va;(void)k; sigcode=-1; siglongjmp(jb,1); }
static u8 zero[4096];
int main(int argc,char **argv){
    int only = argc>1 ? (int)strtol(argv[1],0,16) : -1;
    size_t resv = 0x600000000ull + IMAGE + IMAGE_SZ + 0x100000ull;       /* 6 GiB PROT_NONE below guest 0: sign-extended 32-bit pointers fault on every host */     /* guard + guest [0, image end) */
    u8 *res = mmap(0,resv,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
    if(res==MAP_FAILED){ perror("reserve"); return 2; }
    base=(u64)(uintptr_t)res+0x600000000ull;
    #define MAPRW(g,sz) do{ if(mprotect((void*)(uintptr_t)(base+(g)),(sz),PROT_READ|PROT_WRITE)){ perror("mprotect"); return 2; } }while(0)
    MAPRW(ARENA,ARENA_SZ); MAPRW(STACK,STACK_SZ); MAPRW(IMAGE,IMAGE_SZ);
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_sig; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGTRAP,&sa,0); sigaction(SIGALRM,&sa,0);
    khtest_config(stub_set,stub_n,ARENA,ARENA+ARENA_SZ);
    static KCtx K; KCtx *g=&K; g->bias=(uintptr_t)base;
    u8 *arena=(u8*)(uintptr_t)(base+ARENA), *stack=(u8*)(uintptr_t)(base+STACK), *image=(u8*)(uintptr_t)(base+IMAGE);
    for(size_t fi=0; fi<DRV_NFUNCS; fi++){
        if(only>=0 && drv_funcs[fi].rva!=(u32)only) continue;
        int nok=0,nflt=0,nto=0; u64 sum=1469598103934665603ull;
        for(int t=0;t<NT;t++){
            rs=0x243F6A8885A308D3ull ^ ((u64)drv_funcs[fi].rva*0x100000001B3ull) ^ ((u64)t*0x9E3779B97F4A7C15ull);
            memset(g->st,0,sizeof g->st);
            for(int i=0;i<16;i++){ u64 v=(i==4)?RSP0:gpr(); memcpy(g->st+16+8*i,&v,8); }
            u64 one=1; memcpy(g->st+KO_DFLAG,&one,8);
            for(int i=0;i<16;i++){ u32 w[4]={fl(),fl(),fl(),fl()}; memcpy(g->st+KO_YMM0+64*i,w,16); }
            u64 gsb=GSB_ADDR; memcpy(g->st+KO_GS_CONST,&gsb,8);
            fill(arena,ARENA_SZ); fill(stack,STACK_SZ); u64 se=SENT; memcpy(stack+(RSP0-STACK),&se,8);
            khtest_reset(); sigcode=0;
            struct itimerval it={{0,0},{1,0}}; setitimer(ITIMER_REAL,&it,0);
            if(sigsetjmp(jb,1)==0){ drv_funcs[fi].fn(g); struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0); sigcode=0; }
            else { struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0); }
            if(sigcode==SIGALRM) nto++; else if(sigcode) nflt++; else {
                nok++; u64 h=1469598103934665603ull; h=fnv(h,g->st+16,128); for(int i=0;i<16;i++) h=fnv(h,g->st+KO_YMM0+64*i,16);
                h=fnv(h,arena,ARENA_SZ); h=fnv(h,stack,STACK_SZ);
                for(u64 o=0;o<IMAGE_SZ;o+=4096) if(memcmp(image+o,zero,4096)){ h=fnv(h,&o,8); h=fnv(h,image+o,4096); }
                sum=fnv(sum,&h,8); }
            for(u64 o=0;o<IMAGE_SZ;o+=4096) if(memcmp(image+o,zero,4096)) memset(image+o,0,4096);
        }
        printf("F %x ok=%d fault=%d timeout=%d sum=%016llx\n",drv_funcs[fi].rva,nok,nflt,nto,(unsigned long long)sum);
    }
    return 0; }
