/* ordered_mem.c : KH_ORDERED_MEM (task 39; lifter-rt/khrt.h, Makefile ORDERED_MEM=1): the report line, the selftest that shows the
 * option is really on in THIS binary, and the microbenchmark (KHNMC_SELFTEST_ONLY=membench) for the Mac mini and the phone.
 *
 *  - kh_ordered_mem_report: `kh: memory ordering: acquire/release for aligned guest accesses (<instructions>)` and, in an ordered build,
 *    how many translation units of this binary were compiled with it.  Every unit compiled with KH_ORDERED_MEM=1 leaves a tag
 *    "kh-om1:<file>" in the section kh_om (khrt.h); the count of shard_*.c tags must equal the number of lifted shards (KH_OM_SHARDS,
 *    from the Makefile), so a binary that linked plain shards (e.g. the prebuilt identity archive) fails the check.  The opposite mix
 *    (ordered shards, plain runtime) does not link: every ordered unit references kh_ordered_mem_runtime_v1, defined only here and only
 *    in an ordered build.  EVERY build prints its line at start-up (kh_ordered_mem_startup, called by loader.c right after
 *    kh_host_env_init: on iOS that is after the conf's `log` moved stderr into the log file, so a phone log says which variant ran;
 *    review Y39 n=3: a constructor printed it before main(), to the stderr the log never sees, and plain builds printed nothing).
 *  - kh_ordered_mem_selftest (every --selftest): round trips of LD / ST and rd* / wr* for 1/2/4/8 bytes at every offset 0..7, float,
 *    double, v128 and v256 (the neighbours untouched); the tags; the bias alignment the guest-address test relies on; a short
 *    message-passing litmus of the ordered accessors.
 *  - kh_membench: N guest loads / stores three ways in one binary: plain memcpy, acquire/release (as KH_ORDERED_MEM=1 compiles them)
 *    and this build's own LD / ST, on addresses from an LCG over a region of guest memory (no vectorisation, no address chain); the
 *    three interleaved, best of 5 runs, ns per step.  Then the litmus for longer (on arm64 the plain accessors may show reorderings; the
 *    ordered ones must not).
 *    KHNMC_MEMBENCH_N (accesses per run, default 33554432), KHNMC_MEMBENCH_KB (region, default 256), KHNMC_MEMBENCH_LITMUS (rounds,
 *    default 4000000). */
#define _GNU_SOURCE
#include "khnmc_rt.h"
#include <time.h>
#include <string.h>

#if defined(KH_ORDERED_MEM) && KH_ORDERED_MEM
#define OM_ON 1
const char kh_ordered_mem_runtime_v1=1;
#if defined(__APPLE__)
extern const char om_sec_beg[] __asm("section$start$__DATA$__kh_om");
extern const char om_sec_end[] __asm("section$end$__DATA$__kh_om");
#else
extern const char __start_kh_om[], __stop_kh_om[];
#define om_sec_beg __start_kh_om
#define om_sec_end __stop_kh_om
#endif
typedef struct { int shards, relifted, runtime, lifter_cpu, total; } OmCount;
static OmCount om_count(void){
    OmCount c; memset(&c,0,sizeof c); const char *p=om_sec_beg, *e=om_sec_end;
    while(p<e){ if(!*p){ p++; continue; }
        size_t n=strnlen(p,(size_t)(e-p));
        if(n>7&&!memcmp(p,"kh-om1:",7)){ const char *nm=p+7, *sl=strrchr(nm,'/'); if(sl) nm=sl+1; c.total++;
            if(!strncmp(nm,"shard_",6)) c.shards++; else if(!strcmp(nm,"mod_static_lifted.c") || !strcmp(nm,"tweak_static.c")) c.relifted++;
            else { c.runtime++; if(!strcmp(nm,"khrt_cpu.c")) c.lifter_cpu=1; } }
        p+=n; }
    return c; }
#ifdef KH_OM_SHARDS
#define OM_SHARDS_EXPECTED KH_OM_SHARDS
#else
#define OM_SHARDS_EXPECTED (-1)
#endif
static void om_line(FILE *f,int with_bias){
    OmCount c=om_count();
    fprintf(f,"kh: memory ordering: acquire/release for aligned guest accesses (%s); units built with it in this binary: %d lifted shards (expected %d), %d re-lifted (mod_static_lifted.c), %d runtime / lifter-rt",
            KH_OM_INSNS,c.shards,OM_SHARDS_EXPECTED,c.relifted,c.runtime);
    if(with_bias) fprintf(f,"; guest bias %#llx (a multiple of 4096: %s)",(unsigned long long)gs_bias,(gs_bias&4095u)?"NO":"yes");
    fputc('\n',f); }
#else
#define OM_ON 0
#endif

static void om_report(FILE *f,int with_bias){
#if OM_ON
    om_line(f,with_bias);
#else
    (void)with_bias; fprintf(f,"kh: memory ordering: plain memcpy for guest accesses (built without ORDERED_MEM=1: x86 TSO is not kept on a weakly ordered host)\n");
#endif
}
void kh_ordered_mem_report(FILE *f){ om_report(f,1); }
/* loader.c, right after kh_host_env_init (every build; review Y39 n=3); the selftest checks it ran once.  Without the bias: the guest
 * image is not mapped yet */
static int om_start_lines;
void kh_ordered_mem_startup(void){ om_report(stderr,0); om_start_lines++; }

/* ---- the accessors, both ways, whatever this build is ---- */
static inline u64 pl_ld64(uintptr_t h,u64 ga){ (void)ga; u64 v; memcpy(&v,(const void*)h,8); return v; }
static inline u32 pl_ld32(uintptr_t h,u64 ga){ (void)ga; u32 v; memcpy(&v,(const void*)h,4); return v; }
static inline void pl_st64(uintptr_t h,u64 ga,u64 v){ (void)ga; memcpy((void*)h,&v,8); }
static inline u64 or_ld64(uintptr_t h,u64 ga){ if(!(ga&7)) return __atomic_load_n((const u64*)h,__ATOMIC_ACQUIRE); u64 v; memcpy(&v,(const void*)h,8); return v; }
static inline u32 or_ld32(uintptr_t h,u64 ga){ if(!(ga&3)) return __atomic_load_n((const u32*)h,__ATOMIC_ACQUIRE); u32 v; memcpy(&v,(const void*)h,4); return v; }
static inline void or_st64(uintptr_t h,u64 ga,u64 v){ if(!(ga&7)){ __atomic_store_n((u64*)h,v,__ATOMIC_RELEASE); return; } memcpy((void*)h,&v,8); }
/* this build's own model: the lifted code's LD / ST (khrt.h), whichever it is */
#define bd_ld64(h,ga) LD(u64,ga)
#define bd_ld32(h,ga) LD(u32,ga)
#define bd_st64(h,ga,v) ST(u64,ga,v)

/* ---- benchmark kernels: addresses from an LCG over the region (8-byte slots), so nothing is vectorised and no load feeds an address */
#define LCG(i) ((i)=(i)*6364136223846793005ull+1442695040888963407ull)
#define BENCH(P) \
static __attribute__((noinline)) u64 bn_##P##_load64(KCtx *g,u64 base,u64 mask,u64 n){ u64 s=0,i=0x9e3779b97f4a7c15ull; \
    for(u64 k=0;k<n;k++){ LCG(i); u64 ga=base+((i>>32)&mask)*8; s+=P##_ld64(XL(g,ga),ga); } return s; } \
static __attribute__((noinline)) u64 bn_##P##_load32(KCtx *g,u64 base,u64 mask,u64 n){ u64 s=0,i=0x9e3779b97f4a7c15ull; \
    for(u64 k=0;k<n;k++){ LCG(i); u64 ga=base+((i>>32)&(2*mask+1))*4; s+=P##_ld32(XL(g,ga),ga); } return s; } \
static __attribute__((noinline)) u64 bn_##P##_store64(KCtx *g,u64 base,u64 mask,u64 n){ u64 i=0x9e3779b97f4a7c15ull; \
    for(u64 k=0;k<n;k++){ LCG(i); u64 ga=base+((i>>32)&mask)*8; P##_st64(XL(g,ga),ga,k); } return i; } \
static __attribute__((noinline)) u64 bn_##P##_mixed64(KCtx *g,u64 base,u64 mask,u64 n){ u64 s=0,i=0x9e3779b97f4a7c15ull; \
    for(u64 k=0;k<n;k++){ LCG(i); u64 ga=base+((i>>32)&mask)*8, gb=base+((i>>16)&mask)*8; u64 v=P##_ld64(XL(g,ga),ga); P##_st64(XL(g,gb),gb,v+k); s+=v; } return s; }
BENCH(pl)
BENCH(or)
BENCH(bd)
typedef u64 (*BnFn)(KCtx*,u64,u64,u64);
static volatile u64 bn_sink;
/* the three variants of one kernel, INTERLEAVED (one warm-up pass, then BN_REPS rounds of plain / ordered / this build), best per variant
 * in ns per step: clock-frequency changes and other load on the host hit all three alike */
#define BN_REPS 5
static void bn_time3(BnFn fn[3],KCtx *g,u64 base,u64 mask,u64 n,double best[3]){
    for(int v=0;v<3;v++){ bn_sink+=fn[v](g,base,mask,n/8+1); best[v]=1e30; }
    for(int r=0;r<BN_REPS;r++) for(int v=0;v<3;v++){ u64 t0=kh_host_ns(); bn_sink+=fn[v](g,base,mask,n); double d=(double)(kh_host_ns()-t0)/(double)n; if(d<best[v]) best[v]=d; } }

/* ---- message-passing litmus: the writer stores data (x) then flag (y), both = i; the reader loads y then x.  x86 (TSO) and acquire /
 * release both guarantee x >= y as read; plain accessors on arm64 (and the compiler) do not.  The two words are 128 bytes apart. */
typedef struct { KCtx *g; u64 x, y, n; int mode; _Atomic int go, done; } Lit;
#define LIT_W(P) static void lit_w_##P(Lit *L){ KCtx *g=L->g; for(u64 i=1;i<=L->n;i++){ P##_st64(XL(g,L->x),L->x,i); P##_st64(XL(g,L->y),L->y,i); } }
/* the reader: *complete = it read the LAST flag value (the loop ended by f >= n, not by the deadline: review Y39 n=4, the flag used to
 * be read after the writer's join and was always 1); *during = reads that saw the writer between its first and last round */
#define LIT_R(P) static u64 lit_r_##P(Lit *L,u64 *reads,u64 *during,u64 deadline,int *complete){ KCtx *g=L->g; u64 v=0,r=0,f=0,w=0; \
    while(f<L->n){ f=P##_ld64(XL(g,L->y),L->y); u64 d=P##_ld64(XL(g,L->x),L->x); if(d<f) v++; r++; if(f&&f<L->n) w++; \
        __asm__ __volatile__("":::"memory"); if(!(r&4095)&&kh_host_ns()>deadline) break; } *reads=r; *during=w; *complete=f>=L->n; return v; }
LIT_W(pl) LIT_W(or) LIT_W(bd)
LIT_R(pl) LIT_R(or) LIT_R(bd)
static void *lit_writer(void *a){ Lit *L=(Lit*)a; while(!atomic_load(&L->go)){}
    if(L->mode==0) lit_w_pl(L); else if(L->mode==1) lit_w_or(L); else lit_w_bd(L); atomic_store(&L->done,1); return NULL; }
/* returns the violations (x < y as read); *reads = reader iterations, *during = those while the writer ran; *complete = the reader saw
 * the last value before its time budget (5 s; the selftest also gives it 0 to see a reader cut short) ran out */
#define LIT_BUDGET_NS 5000000000ull
static u64 litmus(KCtx *g,u64 mem,int mode,u64 rounds,u64 budget_ns,u64 *reads,u64 *during,int *complete){
    Lit L; memset(&L,0,sizeof L); L.g=g; L.x=mem; L.y=mem+128; L.n=rounds; L.mode=mode; wr64(g,L.x,0); wr64(g,L.y,0);
    pthread_t t; if(pthread_create(&t,NULL,lit_writer,&L)){ *reads=*during=0; *complete=0; return ~0ull; }
    u64 dl=kh_host_ns()+budget_ns; atomic_store(&L.go,1);
    u64 v=mode==0?lit_r_pl(&L,reads,during,dl,complete):mode==1?lit_r_or(&L,reads,during,dl,complete):lit_r_bd(&L,reads,during,dl,complete);
    pthread_join(t,NULL); return v; }
/* the reader's state as words in the result lines: cut short (it never read the last flag) and no read while the writer ran (nothing
 * was tested) are named, and counted by the callers */
static const char *lit_note(int done,u64 during){ return !done?" (reader cut short: it never read the last value)":!during?" (no read while the writer ran: nothing tested)":""; }

static const char *const mode_name[3]={"plain memcpy","acquire/release","this build's LD/ST"};
int kh_ordered_mem_selftest(KCtx *g){
    int n=0,f=0;
#define TO(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"memory ordering selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    kh_ordered_mem_report(stderr);
    u64 m=ga_alloc(4096,64); TO(m!=0&&!(m&63)); if(!m) goto out;
    /* 1. round trips of every size at every offset 0..7, through LD / ST (the lifted code's macros) and rd* / wr*, neighbours untouched */
    { int bad=0; static const u64 pat=0x8877665544332211ull;
      for(int sz=1;sz<=8;sz*=2) for(int off=0;off<8;off++){
          memset(hp(g,m),0xA5,64); u64 a=m+16+off;
          if(sz==1){ ST(u8,a,(u8)pat); if(LD(u8,a)!=(u8)pat||rd8(g,a)!=(u8)pat) bad++; wr8(g,a,0x5A); if(LD(u8,a)!=0x5A) bad++; }
          if(sz==2){ ST(u16,a,(u16)pat); if(LD(u16,a)!=(u16)pat||rd16(g,a)!=(u16)pat) bad++; wr16(g,a,0x5A5B); if(LD(u16,a)!=0x5A5B) bad++; }
          if(sz==4){ ST(u32,a,(u32)pat); if(LD(u32,a)!=(u32)pat||rd32(g,a)!=(u32)pat) bad++; wr32(g,a,0x5A5B5C5Du); if(LD(u32,a)!=0x5A5B5C5Du) bad++; }
          if(sz==8){ ST(u64,a,pat); if(LD(u64,a)!=pat||rd64(g,a)!=pat) bad++; wr64(g,a,~pat); if(LD(u64,a)!=~pat) bad++; }
          const u8 *b=(const u8*)hp(g,m); for(int k=0;k<64;k++) if((k<16+off||k>=16+off+sz)&&b[k]!=0xA5) bad++; }
      TO(bad==0); }
    /* 2. float, double (4 / 8 bytes: the integer access, the bits unchanged), v128, v256 (memcpy), aligned and not */
    { int bad=0; for(int off=0;off<8;off+=3){ u64 a=m+256+off; float fv=-1.25e-3f; double dv=6.0221e23; ST(float,a,fv); if(LD(float,a)!=fv) bad++;
          ST(double,a+8,dv); if(LD(double,a+8)!=dv) bad++;
          v128 x; for(int k=0;k<16;k++) ((u8*)&x)[k]=(u8)(k*7+off); ST(v128,a+32,x); v128 y=LD(v128,a+32); if(memcmp(&x,&y,16)) bad++;
          v256 z; memset(&z,0,sizeof z); for(int k=0;k<32;k++) ((u8*)&z)[k]=(u8)(k*5+off); ST(v256,a+64,z); v256 w=LD(v256,a+64); if(memcmp(&z,&w,32)) bad++; }
      TO(bad==0); }
    /* 3. the explicit ordered accessors (what the benchmark measures) agree with the plain ones */
    { u64 a=m+512; pl_st64(XL(g,a),a,0x0102030405060708ull); TO(or_ld64(XL(g,a),a)==0x0102030405060708ull&&or_ld32(XL(g,a+4),a+4)==0x01020304u);
      or_st64(XL(g,a+3),a+3,0x1122334455667788ull); TO(pl_ld64(XL(g,a+3),a+3)==0x1122334455667788ull); }
#if OM_ON
    /* 4. the option is ON in this binary: every lifted shard, the re-lifted mod functions and the runtime were compiled with it, and
     *    the guest bias keeps the alignment of guest addresses */
    { OmCount c=om_count(); TO(c.shards>0 && (OM_SHARDS_EXPECTED<0||c.shards==OM_SHARDS_EXPECTED)); TO(c.relifted==1); TO(c.runtime>=10 && c.lifter_cpu);
      TO((gs_bias&4095u)==0); }
#endif
    /* 5. message passing, short: the ordered accessors never show the flag ahead of the data; so does this build's LD / ST when it is
     *    an ordered build; the plain accessors are reported (they may reorder on arm64) */
    { u64 rounds=200000; for(int mode=0;mode<3;mode++){ u64 reads=0,during=0; int done=0; u64 v=litmus(g,m+1024,mode,rounds,LIT_BUDGET_NS,&reads,&during,&done);
          if(mode==1||(mode==2&&OM_ON)) TO(v==0&&done);
          fprintf(stderr,"kh: memory ordering selftest: litmus %s: %llu rounds, %llu reads (%llu while the writer ran), %llu with the flag ahead of the data%s\n",mode_name[mode],(unsigned long long)rounds,
                  (unsigned long long)reads,(unsigned long long)during,(unsigned long long)v,lit_note(done,during)); } }
    /* 6. (review Y39 n=4) a reader cut short is reported as such: with no time budget the reader stops at its first deadline check
     *    (4096 reads), long before the writer's 50,000,000 rounds; `complete` must say so (it was read after the writer's join, always 1) */
    { u64 reads=0,during=0; int done=1; litmus(g,m+1024,1,50000000ull,0,&reads,&during,&done); TO(!done && reads>0 && reads<=4096);
      fprintf(stderr,"kh: memory ordering selftest: litmus with no time budget: %llu reads, complete %d (must be 0)\n",(unsigned long long)reads,done); }
    /* 7. the start-up line was printed once, by loader.c after the host environment (review Y39 n=3: on iOS the log file has it) */
    TO(om_start_lines==1);
    /* 8. the CRT math out-parameters (lifter-rt/rt_libm.c; review Y39 n=6: guest stores through ST, ordered in an ordered build) land
     *    whole, aligned and not, with the neighbours untouched */
    { extern void rt_imp_api_ms_win_crt_math_l1_1_0__modf(KCtx*), rt_imp_api_ms_win_crt_math_l1_1_0__modff(KCtx*), rt_imp_api_ms_win_crt_math_l1_1_0__frexp(KCtx*);
      int bad=0; for(int off=0;off<8;off+=3){ u64 a=m+2048+16+off; double x, r; float xf, rf; const u8 *b=(const u8*)hp(g,m+2048);
          u64 sp=KH_RSP(g); memset(hp(g,m+2048),0xA5,64); KH_RDX(g)=a; x=2.75; memcpy(&KH_XMM(g,0),&x,8); rt_imp_api_ms_win_crt_math_l1_1_0__modf(g); KH_RSP(g)=sp;
          memcpy(&r,&KH_XMM(g,0),8); if(r!=0.75||LD(double,a)!=2.0) bad++; for(int k=0;k<64;k++) if((k<16+off||k>=16+off+8)&&b[k]!=0xA5) bad++;
          memset(hp(g,m+2048),0xA5,64); KH_RDX(g)=a; xf=-3.5f; memcpy(&KH_XMM(g,0),&xf,4); rt_imp_api_ms_win_crt_math_l1_1_0__modff(g); KH_RSP(g)=sp;
          memcpy(&rf,&KH_XMM(g,0),4); if(rf!=-0.5f||LD(float,a)!=-3.0f) bad++; for(int k=0;k<64;k++) if((k<16+off||k>=16+off+4)&&b[k]!=0xA5) bad++;
          memset(hp(g,m+2048),0xA5,64); KH_RDX(g)=a; x=48.0; memcpy(&KH_XMM(g,0),&x,8); rt_imp_api_ms_win_crt_math_l1_1_0__frexp(g); KH_RSP(g)=sp;
          memcpy(&r,&KH_XMM(g,0),8); if(r!=0.75||LD(s32,a)!=6) bad++; for(int k=0;k<64;k++) if((k<16+off||k>=16+off+4)&&b[k]!=0xA5) bad++; }
      TO(bad==0); }
    ga_free(m);
out:
    fprintf(stderr,"kh: memory ordering selftest: %d checks, %d failures (%s build; round trips of LD/ST and rd/wr at every offset, float/double/v128/v256, the accessors, %smessage-passing litmus)\n",
            n,f,OM_ON?"ORDERED_MEM=1":"plain",OM_ON?"the units built with the option and the bias, ":"");
#undef TO
    return f;
}

int kh_membench(KCtx *g){
    const char *e; u64 n=1ull<<25, kb=256, rounds=4000000;
    if((e=getenv("KHNMC_MEMBENCH_N"))&&*e){ char *end; unsigned long long v=strtoull(e,&end,10); if(*end||!v) kh_fatal(g,"membench-n: KHNMC_MEMBENCH_N=%s (a positive number of accesses per run)",e); n=v; }
    if((e=getenv("KHNMC_MEMBENCH_KB"))&&*e){ char *end; unsigned long long v=strtoull(e,&end,10); if(*end||v<4||v>(1u<<20)||(v&(v-1))) kh_fatal(g,"membench-kb: KHNMC_MEMBENCH_KB=%s (a power of two, 4..1048576)",e); kb=v; }
    if((e=getenv("KHNMC_MEMBENCH_LITMUS"))&&*e){ char *end; unsigned long long v=strtoull(e,&end,10); if(*end||!v) kh_fatal(g,"membench-litmus: KHNMC_MEMBENCH_LITMUS=%s (a positive number of rounds)",e); rounds=v; }
    kh_ordered_mem_report(stderr);
    u64 bytes=kb*1024, base=ga_alloc(bytes+4096,4096); if(!base) kh_fatal(g,"membench: cannot allocate %llu KiB of guest memory",(unsigned long long)kb);
    memset(hp(g,base),1,bytes); u64 mask=bytes/8-1;
    static const char *const kn[4]={"load64","load32","store64","load64+store64"};
    BnFn fns[3][4]={{bn_pl_load64,bn_pl_load32,bn_pl_store64,bn_pl_mixed64},{bn_or_load64,bn_or_load32,bn_or_store64,bn_or_mixed64},{bn_bd_load64,bn_bd_load32,bn_bd_store64,bn_bd_mixed64}};
    fprintf(stderr,"kh: membench: %llu steps per run (load64+store64: one load and one store per step), region %llu KiB of guest memory, LCG addresses, the three variants interleaved, best of %d runs; ns per step:\n",(unsigned long long)n,(unsigned long long)kb,BN_REPS);
    for(int k=0;k<4;k++){ double t[3]; BnFn f3[3]={fns[0][k],fns[1][k],fns[2][k]}; bn_time3(f3,g,base,mask,n,t);
        fprintf(stderr,"kh: membench: %-15s plain %.3f ns, acquire/release %.3f ns (%+.1f%%), this build (%s) %.3f ns\n",kn[k],t[0],t[1],t[0]>0?(t[1]/t[0]-1)*100:0.0,OM_ON?"ordered":"plain",t[2]); }
    int f=0;
    int untested=0;
    for(int mode=0;mode<3;mode++){ u64 reads=0,during=0; int done=0; u64 v=litmus(g,base,mode,rounds,LIT_BUDGET_NS,&reads,&during,&done);
        int must=mode==1||(mode==2&&OM_ON); if(must&&(v||!done)) f++; if(!during) untested++;
        fprintf(stderr,"kh: membench: litmus %s: %llu rounds, %llu reads (%llu while the writer ran), %llu with the flag ahead of the data%s%s\n",mode_name[mode],(unsigned long long)rounds,(unsigned long long)reads,
                (unsigned long long)during,(unsigned long long)v,lit_note(done,during),must?(v||!done?"  FAIL (must be 0 and complete)":"  (must be 0: ok)"):""); }
    if(untested) fprintf(stderr,"kh: membench: %d litmus way(s) had no read while the writer ran: their 0 tests nothing (run again, or raise KHNMC_MEMBENCH_LITMUS)\n",untested);
    ga_free(base);
    fprintf(stderr,"kh: membench: done, %d failure(s)\n",f);
    return f;
}
