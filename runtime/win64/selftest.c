/* selftest.c : runtime self-tests that need no game data beyond the mapped image: allocator, qsort/bsearch, printf engine, wait objects,
 * critical sections across threads, fibers (switch, resume on another thread), the two yield hooks.  `khnmc --selftest`. */
#include "khnmc_rt.h"
#include <unistd.h>

void rt_hook_fiber_yield_150060(KCtx *g); void rt_hook_fiber_yield_150080(KCtx *g);
extern void rt_imp_api_ms_win_crt_utility_l1_1_0__qsort(KCtx *g); extern void rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch(KCtx *g);
extern void rt_imp_api_ms_win_crt_stdio_l1_1_0____stdio_common_vsprintf(KCtx *g);
extern void rt_imp_kernel32__EnterCriticalSection(KCtx *g); extern void rt_imp_kernel32__LeaveCriticalSection(KCtx *g); extern void rt_imp_kernel32__InitializeCriticalSection(KCtx *g);
extern void rt_imp_kernel32__SwitchToFiber(KCtx *g);
static int fails, checks;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)

/* a guest-callable int comparator implemented on the host: compares two u32 */
static void cmp_u32(KCtx *g){ u32 a=rd32(g,KH_RCX(g)),b=rd32(g,KH_RDX(g)); SET_RAX(g,(u32)(a<b?-1:a>b?1:0)); KH_RET(g); }
static void do_call(KCtx *g,void (*imp)(KCtx*),u64 a0,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5){
    u64 save=KH_RSP(g); u64 sp=(save-0x200)&~0xFull; wr64(g,sp+0x20,a4); wr64(g,sp+0x28,a5); sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3; imp(g); KH_RSP(g)=save; }

/* ---- fibers ---- */
static u64 main_fiber, fibA, fibB; static _Atomic int step; static int order[16]; static int nord;
static void fiberA(KCtx *g){ u64 p=KH_RCX(g); order[nord++]=100+(int)p; kh_fiber_switch(g,main_fiber); order[nord++]=101; kh_fiber_switch(g,main_fiber); order[nord++]=102; kh_fiber_switch(g,main_fiber); for(;;) kh_fiber_switch(g,main_fiber); }
static void fiberB(KCtx *g){ (void)g; order[nord++]=200; kh_fiber_switch(g,fibA); for(;;) kh_fiber_switch(g,main_fiber); }
static _Atomic int mig_done;
static void migrate_proc(KCtx *g){          /* runs on a second thread: converts itself and resumes fiber A there */
    u64 mf=kh_fiber_convert(g,0); (void)mf; main_fiber=mf; /* this thread's own "main fiber" for the yield target */
    order[nord++]=300; kh_fiber_switch(g,fibA); order[nord++]=301; mig_done=1; kh_fiber_convert_to_thread(g); KH_RET(g);
}
static void sleep_ms(int ms){ usleep((useconds_t)ms*1000); }

int kh_selftest(KCtx *g){
    /* allocator */
    u64 p[64]; for(int i=0;i<64;i++){ p[i]=ga_alloc((u64)(i*37+1),(i%5==0)?64:16); CHECK(p[i]&&!(p[i]&15)); if(i%5==0) CHECK(!(p[i]&63)); memset(hp(g,p[i]),0xA5,(size_t)(i*37+1)); }
    for(int i=0;i<64;i+=2) ga_free(p[i]); u64 q=ga_calloc(10,10); CHECK(q&&rd64(g,q)==0&&rd64(g,q+88)==0); q=ga_realloc(q,5000); CHECK(q&&ga_usable(q)>=5000);
    u64 big=ga_alloc(3<<20,16); CHECK(big); memset(hp(g,big),1,3<<20); ga_free(big); u64 big2=ga_alloc(3<<20,4096); CHECK(big2&&!(big2&4095)); ga_free(big2);
    for(int i=1;i<64;i+=2) ga_free(p[i]); ga_free(q);
    /* qsort (Microsoft algorithm) + bsearch */
    u32 v[40]; for(int i=0;i<40;i++) v[i]=(u32)((i*2654435761u)>>7)%1000; u64 arr=ga_alloc(sizeof v,16); memcpy(hp(g,arr),v,sizeof v); u64 cmp=kh_host_fn("selftest_cmp",cmp_u32);
    do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__qsort,arr,40,4,cmp,0,0); u32 *o=hp(g,arr); int sorted=1; for(int i=1;i<40;i++) if(o[i-1]>o[i]) sorted=0; CHECK(sorted);
    u32 key=o[17]; u64 kp=ga_alloc(16,16); wr32(g,kp,key); do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch,kp,arr,40,4,cmp,0); CHECK(KH_RAX(g)!=0&&rd32(g,KH_RAX(g))==key);
    wr32(g,kp,100000); do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch,kp,arr,40,4,cmp,0); CHECK(KH_RAX(g)==0);
    /* printf engine: Win64 va_list = array of 8-byte slots */
    u64 fmt=ga_strdup("%d|%5.2f|%s|%llu|%x|%c|%-4d|%p|%ls"); u64 buf=ga_alloc(256,16); u64 va=ga_alloc(128,16); u64 s1=ga_strdup("hi"); u64 ws=ga_alloc(16,16); wr16(g,ws,'w'); wr16(g,ws+2,'z'); wr16(g,ws+4,0);
    double d=3.14159; u64 dv; memcpy(&dv,&d,8); wr64(g,va,(u64)(u32)-42); wr64(g,va+8,dv); wr64(g,va+16,s1); wr64(g,va+24,1234567890123ull); wr64(g,va+32,0xBEEF); wr64(g,va+40,'Q'); wr64(g,va+48,7); wr64(g,va+56,0x140001000ull); wr64(g,va+64,ws);
    do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0____stdio_common_vsprintf,0,buf,256,fmt,0,va);
    CHECK(!strcmp(gstr(g,buf),"-42| 3.14|hi|1234567890123|beef|Q|7   |0000000140001000|wz"));
    if(strcmp(gstr(g,buf),"-42| 3.14|hi|1234567890123|beef|Q|7   |0000000140001000|wz")) fprintf(stderr,"  got: %s\n",gstr(g,buf));
    /* critical section + events across threads */
    u64 cs=ga_alloc(64,16); do_call(g,rt_imp_kernel32__InitializeCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__EnterCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__EnterCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__LeaveCriticalSection,cs,0,0,0,0,0);
    CHECK(rd32(g,cs+0xC)==1); do_call(g,rt_imp_kernel32__LeaveCriticalSection,cs,0,0,0,0,0); CHECK(rd64(g,cs+0x10)==0);
    /* fibers: main converts, A yields back, B hands over to A, then A is resumed on ANOTHER thread */
    main_fiber=kh_fiber_convert(g,0); CHECK(main_fiber!=0);
    fibA=kh_fiber_create(g,0,kh_host_fn("selftest_fiberA",fiberA),7); fibB=kh_fiber_create(g,0,kh_host_fn("selftest_fiberB",fiberB),0);
    kh_fiber_switch(g,fibA); CHECK(nord==1&&order[0]==107);                       /* A started and yielded */
    kh_fiber_switch(g,fibB); CHECK(nord==3&&order[1]==200&&order[2]==101);          /* B ran, switched to A, A continued and yielded to main */
    u64 mf_save=main_fiber; (void)mf_save;
    /* A is now suspended at its second yield; resume it from a second host thread (migration) */
    u64 a=kh_host_fn("selftest_migrate",migrate_proc); kh_spawn_hostfn_thread(g,"selftest_migrate",migrate_proc,0); (void)a;
    for(int i=0;i<200&&!mig_done;i++) sleep_ms(10);
    CHECK(mig_done==1); CHECK(order[3]==300&&order[4]==102&&order[5]==301);
    main_fiber=mf_save;
    kh_fiber_delete(g,fibB); kh_fiber_delete(g,fibA); CHECK(kh_fiber_convert_to_thread(g)==1);
    /* yield hooks: from==to so 0x135470 returns immediately; checks the register plumbing and the stores */
    u64 task=ga_alloc(0x100,16),P=ga_alloc(0x40,16),Q=ga_alloc(0x40,16),X=ga_alloc(0x10,16); memset(hp(g,task),0,0x100); memset(hp(g,P),0,0x40); memset(hp(g,Q),0,0x40);
    wr64(g,task+0x70,P); wr64(g,P,X); wr64(g,task+0x58,Q); wr64(g,Q+0x28,X);
    do_call(g,rt_hook_fiber_yield_150060,task,0x1234,0,0,0,0); CHECK(rd32(g,P+0x1c)==0x1234);
    float c; u32 cu=rd32(g,GS_BASE+0x717480); memcpy(&c,&cu,4); float x1=2.5f; memcpy(&KH_XMM(g,1),&x1,4); do_call(g,rt_hook_fiber_yield_150080,task,0,0,0,0,0); float r; u32 ru=rd32(g,P+0x20); memcpy(&r,&ru,4); CHECK(r==2.5f-c);
    fprintf(stderr,"kh: selftest: %d checks, %d failures\n",checks,fails);
    if(getenv("KH_SELFTEST_FATAL")){ extern void rt_imp_gdi32__MoveToEx(KCtx*); do_call(g,rt_imp_gdi32__MoveToEx,1,2,3,4,0,0); }   /* demonstrates the loud failure of an unimplemented import */
    return fails;
}
