/* native_call.c : calling host-native Windows-x64-ABI code from lifted code (docs/GFX-VKD3D-PLAN.md, TASK15).
 *
 * kh_dispatch falls back to kh_native_try() for a target that is neither a universe function nor an import / KH_HOSTFN address.  If the
 * target lies inside a REGISTERED native range (the executable segments of the vkd3d libraries, registered by gfx_vkd3d.c) it is called
 * natively through ONE generic trampoline: rcx, rdx, r8, r9 and xmm0..xmm3 come from the KCtx, 16 qwords of stack arguments are copied
 * from the guest stack (the guest `call` already pushed the return address, so they start at guest rsp+0x28), the callee gets 32 bytes of
 * shadow space on a 16-byte aligned host stack, rax and xmm0 go back into the KCtx and the guest return address is popped (KH_RET).
 * MXCSR and the x87 control word are restored after the call (callee-saved in the Windows ABI; the lifted code's float policy needs them).
 * Anything outside a registered range is NOT handled here: kh_dispatch then faults loudly as before.
 * Two side tables keyed by the exact native entry address: WATCHES count calls (the null recorder's draw / clear / ... statistics), and
 * OVERRIDES let the runtime take a call itself before it goes native (a handler returns 1 when it handled the call and popped the return
 * address, 0 to let the native code run): gfx_vkd3d.c uses them for D3D12 bundles, which vkd3d does not implement.  POST hooks see the
 * arguments and the result after the native call returned (state tracking without touching the call).
 * x86-64 identity address model only (the callee dereferences guest pointers as host pointers); other hosts get a named fatal. */
#include "khnmc_rt.h"
#include <unistd.h>
#include <sys/wait.h>

#define MAXNAT 16
#define MAXWATCH 24
typedef struct { u64 lo, hi; char name[48]; } NatRange;
static NatRange nat[MAXNAT]; static _Atomic int nnat;
typedef struct { u64 addr; _Atomic u64 *ctr; int by_rdx; } NatWatch;
static NatWatch watch[MAXWATCH]; static _Atomic int nwatch;
typedef struct { u64 addr; int (*h)(KCtx *); } NatOverride;
#define MAXOVR 16
static NatOverride ovr[MAXOVR]; static _Atomic int novr;
typedef void (*KhNatPost)(KCtx *g,const u64 *ireg,const u64 *stk,u64 rax);       /* ireg = rcx,rdx,r8,r9 ; stk = the stack arguments */
typedef struct { u64 addr; KhNatPost h; } NatPost;
static NatPost post[MAXOVR]; static _Atomic int npost;
#define MAXNAMES 512
static struct { u64 addr; char name[56]; } nname[MAXNAMES]; static _Atomic int nnames;     /* trace only: entry address -> "Interface::Method" */
static pthread_mutex_t natm=PTHREAD_MUTEX_INITIALIZER;
_Atomic u64 kh_native_calls;
int kh_native_trace;

/* register [lo,hi) as native Windows-ABI code (entries are published by the count, so readers need no lock) */
void kh_native_register(const char *name,u64 lo,u64 hi){
    pthread_mutex_lock(&natm);
    int n=nnat; for(int i=0;i<n;i++) if(nat[i].lo==lo&&nat[i].hi==hi){ pthread_mutex_unlock(&natm); return; }
    if(n>=MAXNAT||hi<=lo){ pthread_mutex_unlock(&natm); kh_fatal(NULL,"native-range-table: cannot register %s [%llx,%llx) (table of %d full or empty range)",name,(unsigned long long)lo,(unsigned long long)hi,MAXNAT); }
    nat[n].lo=lo; nat[n].hi=hi; snprintf(nat[n].name,sizeof nat[n].name,"%s",name); nnat=n+1;
    pthread_mutex_unlock(&natm);
}
void kh_native_unregister(u64 lo,u64 hi){              /* selftest only: the last registered range */
    pthread_mutex_lock(&natm); int n=nnat; if(n&&nat[n-1].lo==lo&&nat[n-1].hi==hi) nnat=n-1; pthread_mutex_unlock(&natm);
}
/* count calls of one native entry point into *ctr (by_rdx: add the 32-bit second argument instead of 1) */
void kh_native_watch(u64 addr,_Atomic u64 *ctr,int by_rdx){
    pthread_mutex_lock(&natm);
    int n=nwatch; for(int i=0;i<n;i++) if(watch[i].addr==addr&&watch[i].ctr==ctr){ pthread_mutex_unlock(&natm); return; }
    if(n>=MAXWATCH){ pthread_mutex_unlock(&natm); kh_fatal(NULL,"native-watch-table: more than %d watched native entry points",MAXWATCH); }
    watch[n].addr=addr; watch[n].ctr=ctr; watch[n].by_rdx=by_rdx; nwatch=n+1;
    pthread_mutex_unlock(&natm);
}
/* let h(g) see every guest call of the native entry point `addr` first */
void kh_native_override(u64 addr,int (*h)(KCtx *)){
    pthread_mutex_lock(&natm);
    int n=novr; for(int i=0;i<n;i++) if(ovr[i].addr==addr){ int same=ovr[i].h==h; pthread_mutex_unlock(&natm); if(same) return; kh_fatal(NULL,"native-override-table: two different handlers for native address 0x%llx",(unsigned long long)addr); }
    if(n>=MAXOVR){ pthread_mutex_unlock(&natm); kh_fatal(NULL,"native-override-table: more than %d overridden native entry points",MAXOVR); }
    ovr[n].addr=addr; ovr[n].h=h; novr=n+1;
    pthread_mutex_unlock(&natm);
}
/* optional label of one native entry point for KHNMC_GFX_TRACE=1 (first label of an address wins) */
void kh_native_name(u64 addr,const char *iface,const char *method){
    pthread_mutex_lock(&natm);
    int n=nnames; for(int i=0;i<n;i++) if(nname[i].addr==addr){ pthread_mutex_unlock(&natm); return; }
    if(n<MAXNAMES){ nname[n].addr=addr; snprintf(nname[n].name,sizeof nname[n].name,"%s::%s",iface,method); nnames=n+1; }
    pthread_mutex_unlock(&natm);
}
const char *kh_native_name_of(u64 addr){ int n=nnames; for(int i=0;i<n;i++) if(nname[i].addr==addr) return nname[i].name; return NULL; }
/* call h after every guest call of the native entry point `addr` */
void kh_native_post(u64 addr,KhNatPost h){
    pthread_mutex_lock(&natm);
    int n=npost; for(int i=0;i<n;i++) if(post[i].addr==addr&&post[i].h==h){ pthread_mutex_unlock(&natm); return; }
    if(n>=MAXOVR){ pthread_mutex_unlock(&natm); kh_fatal(NULL,"native-post-table: more than %d native entry points with a post hook",MAXOVR); }
    post[n].addr=addr; post[n].h=h; npost=n+1;
    pthread_mutex_unlock(&natm);
}
const char *kh_native_range_name(u64 target,u64 *off){
    int n=nnat; for(int i=0;i<n;i++) if(target-nat[i].lo<nat[i].hi-nat[i].lo){ if(off) *off=target-nat[i].lo; return nat[i].name; }
    return NULL;
}
int kh_native_range_count(void){ return nnat; }

#if defined(__x86_64__) && KH_XLATE_MODE==0
/* u64 kh_ms_tramp(const u64 ireg[4], const u8 *xmm, const u64 *stk, u64 target, u8 *xmm0_out)   (System V side)
 *   rdi = {rcx,rdx,r8,r9}   rsi = xmm0 image, xmm1..3 follow at a 64-byte stride (the KCtx layout)   rdx = 16 qwords of stack arguments
 *   rcx = target            r8  = where to store the 16 bytes of xmm0 after the call             returns the callee's rax
 * frame: 0x00 shadow space (32) | 0x20 16 stack-argument qwords | 0xa0 saved MXCSR | 0xa4 saved x87 CW ; rsp is 16-byte aligned at the call */
u64 kh_ms_tramp(const u64 *ireg,const u8 *xmm,const u64 *stk,u64 target,u8 *xmm0_out);
__asm__(".text\n.globl kh_ms_tramp\n.type kh_ms_tramp,@function\nkh_ms_tramp:\n"
        " pushq %rbp\n movq %rsp,%rbp\n pushq %rbx\n pushq %r12\n subq $0xb0,%rsp\n"      /* r12 is pushed only to keep rsp 16-byte aligned */
        " movq %r8,%rbx\n movq %rcx,%rax\n"
        " stmxcsr 0xa0(%rsp)\n fnstcw 0xa4(%rsp)\n"
        " xorl %r11d,%r11d\n"
        "1: movq (%rdx,%r11,8),%r10\n movq %r10,0x20(%rsp,%r11,8)\n incq %r11\n cmpq $16,%r11\n jb 1b\n"
        " movups (%rsi),%xmm0\n movups 64(%rsi),%xmm1\n movups 128(%rsi),%xmm2\n movups 192(%rsi),%xmm3\n"
        " movq 8(%rdi),%rdx\n movq 16(%rdi),%r8\n movq 24(%rdi),%r9\n movq (%rdi),%rcx\n"
        " call *%rax\n"
        " movups %xmm0,(%rbx)\n"
        " fnclex\n fldcw 0xa4(%rsp)\n ldmxcsr 0xa0(%rsp)\n"
        " leaq -16(%rbp),%rsp\n popq %r12\n popq %rbx\n popq %rbp\n ret\n"
        ".size kh_ms_tramp,.-kh_ms_tramp\n");
#define KH_NATIVE_OK 1
#endif

/* 1 = target was native and has been called (guest return address popped); 0 = not a registered native address */
int kh_native_try(KCtx *g,u64 target){
    int n=nnat,i; for(i=0;i<n;i++) if(target-nat[i].lo<nat[i].hi-nat[i].lo) break;
    if(i==n) return 0;
#ifdef KH_NATIVE_OK
    int no=novr; for(int k=0;k<no;k++) if(ovr[k].addr==target){ if(ovr[k].h(g)) return 1; break; }
    u64 sp=KH_RSP(g); u64 ir[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    int nw=nwatch; for(int k=0;k<nw;k++) if(watch[k].addr==target) atomic_fetch_add(watch[k].ctr,watch[k].by_rdx?(u64)(u32)ir[1]:1ull);
    atomic_fetch_add(&kh_native_calls,1);
    u64 rax=kh_ms_tramp(ir,g->st+KO_YMM0,(const u64*)XL(g,sp+0x28),target,g->st+KO_YMM0);
    if(kh_native_trace){ const char *nm=kh_native_name_of(target); const u64 *st=(const u64*)XL(g,sp+0x28); char where[96]; if(!nm){ snprintf(where,sizeof where,"%s+0x%llx",nat[i].name,(unsigned long long)(target-nat[i].lo)); nm=where; }
        fprintf(stderr,"kh: native t%u %s(%llx, %llx, %llx, %llx | %llx, %llx, %llx, %llx) -> %llx\n",cur_thread()?cur_thread()->tid:0,nm,(unsigned long long)ir[0],(unsigned long long)ir[1],(unsigned long long)ir[2],(unsigned long long)ir[3],
                (unsigned long long)st[0],(unsigned long long)st[1],(unsigned long long)st[2],(unsigned long long)st[3],(unsigned long long)rax); }
    KH_RAX(g)=rax; KH_RSP(g)=sp+8;                                           /* KH_RET: pop the guest return address */
    int np=npost; for(int k=0;k<np;k++) if(post[k].addr==target) post[k].h(g,ir,(const u64*)XL(g,sp+0x28),rax);
    return 1;
#else
    kh_fatal(g,"native-call-unsupported-host: guest called native address 0x%llx (%s) but the Windows-ABI trampoline exists only for x86-64 with the identity address model",(unsigned long long)target,nat[i].name);
#endif
}

/* ---- selftest ------------------------------------------------------------------------------------------------------------ */
#ifdef KH_NATIVE_OK
#define MS __attribute__((ms_abi,noinline,noclone))
typedef struct { u64 a,b,c; } Big3;                                                  /* 24 bytes: returned through a hidden pointer in rcx */
static volatile u64 t_seen_rsp;
static MS u64 t_ints(u64 a,u64 b,u64 c,u64 d){ t_seen_rsp=(u64)(uintptr_t)__builtin_frame_address(0); return a*1000+b*100+c*10+d; }
static MS double t_fp4(double a,float b,double c,float d){ return a+2.0*b+3.0*c+4.0*d; }      /* xmm0..xmm3 */
static MS double t_mixed(u64 a,double b,u64 c,float d){ return (double)a*1000.0+b*100.0+(double)c*10.0+d; }   /* rcx, xmm1, r8, xmm3 */
static MS float t_retf(float x,u32 k){ return x*(float)k; }                              /* xmm0 in, edx, float result in xmm0 */
static MS u64 t_stack12(u64 a0,u64 a1,u64 a2,u64 a3,u64 s0,u64 s1,u64 s2,u64 s3,u64 s4,u64 s5,u64 s6,u64 s7,u64 s8,u64 s9,u64 s10,u64 s11){
    u64 v[16]={a0,a1,a2,a3,s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11}; u64 h=0xcbf29ce484222325ull; for(int i=0;i<16;i++) h=(h^v[i])*0x100000001b3ull; return h; }
static MS double t_stackfp(u64 a,u64 b,u64 c,u64 d,double e,float f,u8 small,u64 *out){ *out=a+b+c+d+small; return e*10.0+f; }   /* FP and narrow values in stack slots */
static MS Big3 t_struct(u64 x,u64 y){ Big3 r={x+y,x*y,x-y}; return r; }                  /* rcx = hidden result pointer, rdx = x, r8 = y; rax = the pointer */
static MS void t_void(u64 *p,u64 v){ *p=v; }
static MS u64 t_fpstate(void){ u32 m=0x9fc0; __asm__ __volatile__("ldmxcsr %0"::"m"(m)); return 7; }   /* leaves FTZ|DAZ set: the trampoline must undo it */

static _Atomic u64 post_sum;
static void t_post(KCtx *g,const u64 *ir,const u64 *stk,u64 rax){ (void)g; post_sum+=ir[0]+ir[3]+stk[0]+stk[11]+rax; }
static _Atomic int ov_seen;
static int t_override(KCtx *g){ ov_seen++; if(KH_RCX(g)!=77) return 0; KH_RAX(g)=KH_RDX(g)+5000; KH_RET(g); return 1; }   /* takes the call only when rcx == 77 */
static int checks, fails;
#define T(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"native-call selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static int rsp_ok;
/* call `target` like a lifted `call`: n arguments (4 in registers, the rest in stack slots), optional xmm0..3 images */
static u64 ncall(KCtx *g,u64 target,int n,const u64 *a,const double *fp,int fp_is_float_mask){
    u64 saved[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(saved[0]-0x300)&~15ull; memset(hp(g,sp),0xEE,0x100);
    for(int i=4;i<n;i++) wr64(g,sp+8*(u64)i,a[i]);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0;
    if(fp) for(int i=0;i<4;i++){ v128 x=VZ; if(fp_is_float_mask&(1<<i)){ float f=(float)fp[i]; memcpy(&x,&f,4); } else memcpy(&x,&fp[i],8); KH_XMM(g,i)=x; }
    kh_dispatch(g,target);
    rsp_ok=KH_RSP(g)==sp+8 && rd64(g,sp)==0x7ff7ffff0000ull;
    u64 r=KH_RAX(g); KH_RSP(g)=saved[0]; KH_RCX(g)=saved[1]; KH_RDX(g)=saved[2]; KH_R8(g)=saved[3]; KH_R9(g)=saved[4];
    return r;
}
static double xmm0_d(KCtx *g){ double d; memcpy(&d,&KH_XMM(g,0),8); return d; }
static float xmm0_f(KCtx *g){ float f; memcpy(&f,&KH_XMM(g,0),4); return f; }
/* a dispatch to `target` must end in kh_fault ("not a function start", exit code 5) */
static int fault_case(KCtx *g,u64 target){
    if(!kh_fork_ok()){ kh_selftest_not_run("native-call","a dispatch to an address that is not a function start (fault 5)",1); checks--; return 1; }   /* the caller's T() must not count it */
    FILE *f=kh_tmpfile(); if(!f) return 0;
    fflush(stderr); pid_t child=kh_fork();
    if(child==0){ dup2(fileno(f),2); u64 sp=(KH_RSP(g)-0x200)&~15ull; sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; kh_dispatch(g,target); _exit(99); }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char out[8192]; size_t n=fread(out,1,sizeof out-1,f); out[n]=0; fclose(f);
    return WIFEXITED(status) && WEXITSTATUS(status)==5 && strstr(out,"dispatch: not a function start")!=NULL;
}
#endif

int kh_native_selftest(KCtx *g){
#ifdef KH_NATIVE_OK
    checks=fails=0;
    static void *const fns[]={(void*)t_ints,(void*)t_fp4,(void*)t_mixed,(void*)t_retf,(void*)t_stack12,(void*)t_stackfp,(void*)t_struct,(void*)t_void,(void*)t_fpstate};
    u64 lo=~0ull,hi=0; for(unsigned i=0;i<sizeof fns/sizeof*fns;i++){ u64 a=(u64)(uintptr_t)fns[i]; if(a<lo) lo=a; if(a>hi) hi=a; } hi+=1;
    extern KhFn kh_lookup(u64);
    /* before registration the test functions are ordinary host addresses: not dispatchable */
    T(kh_lookup((u64)(uintptr_t)t_ints)==NULL); T(kh_native_range_name((u64)(uintptr_t)t_ints,NULL)==NULL);
    T(fault_case(g,(u64)(uintptr_t)t_ints));
    int before=kh_native_range_count(); u64 calls0=kh_native_calls;
    kh_native_register("selftest",lo,hi);
    T(kh_native_range_count()==before+1);
    { u64 off=99; const char *nm=kh_native_range_name(lo,&off); T(nm&&!strcmp(nm,"selftest")&&off==0); T(kh_native_range_name(hi,NULL)==NULL||strcmp(kh_native_range_name(hi,NULL),"selftest")); }
    /* integers in rcx, rdx, r8, r9; rax back; return address popped; host stack 16-byte aligned inside the callee */
    u64 a4[4]={1,2,3,4}; T(ncall(g,(u64)(uintptr_t)t_ints,4,a4,NULL,0)==1234); T(rsp_ok);
    T((t_seen_rsp&15)==0);
    u64 big[4]={0xfedcba9876543210ull,0,0,0x8000000000000001ull}; T(ncall(g,(u64)(uintptr_t)t_ints,4,big,NULL,0)==0xfedcba9876543210ull*1000+0x8000000000000001ull); T(rsp_ok);
    /* floats and doubles in slots 0..3 */
    double f4[4]={1.5,2.25,-3.0,0.125}; ncall(g,(u64)(uintptr_t)t_fp4,0,NULL,f4,0xA); T(xmm0_d(g)==1.5+2.0*2.25+3.0*-3.0+4.0*0.125); T(rsp_ok);
    /* positional mix: integer slots 0 and 2, FP slots 1 and 3 */
    u64 mi[4]={7,0xdead,9,0xbeef}; double mf[4]={-1.0,2.5,-1.0,0.5}; ncall(g,(u64)(uintptr_t)t_mixed,4,mi,mf,0x8); T(xmm0_d(g)==7000.0+250.0+90.0+0.5); T(rsp_ok);
    /* float result in xmm0 */
    u64 ri[2]={0,6}; double rf[4]={1.75,0,0,0}; ncall(g,(u64)(uintptr_t)t_retf,2,ri,rf,0x1); T(xmm0_f(g)==10.5f); T(rsp_ok);
    /* 12 stack arguments */
    u64 s[16]; u64 h=0xcbf29ce484222325ull; for(int i=0;i<16;i++){ s[i]=0x1111111111111111ull*(u64)(i+1)^((u64)i<<56); h=(h^s[i])*0x100000001b3ull; }
    T(ncall(g,(u64)(uintptr_t)t_stack12,16,s,NULL,0)==h); T(rsp_ok);
    /* FP and narrow values in stack slots, pointer result written through a stack argument */
    u64 out=ga_alloc(64,16); wr64(g,out,0);
    { double e=12.5; float f=0.75f; u64 eb,fb=0xAAAAAAAA00000000ull; memcpy(&eb,&e,8); memcpy(&fb,&f,4);
      u64 sa[8]={10,20,30,40,eb,fb,0xFFFFFFFFFFFFFF05ull,out}; ncall(g,(u64)(uintptr_t)t_stackfp,8,sa,NULL,0); T(xmm0_d(g)==125.75); T(rd64(g,out)==105); T(rsp_ok); }
    /* struct result through the hidden pointer: rcx = result, arguments shifted by one, rax = result pointer */
    memset(hp(g,out),0xA5,64);
    { u64 sa[3]={out,100,7}; T(ncall(g,(u64)(uintptr_t)t_struct,3,sa,NULL,0)==out); T(rd64(g,out)==107&&rd64(g,out+8)==700&&rd64(g,out+16)==93&&rd64(g,out+24)==0xa5a5a5a5a5a5a5a5ull); T(rsp_ok); }
    /* void function, callee-saved guest state untouched */
    { u64 rbx=KH_RBX(g),rsi=KH_RSI(g),rdi=KH_RDI(g),rbp=KH_RBP(g); KH_RBX(g)=0x1111; KH_RSI(g)=0x2222; KH_RDI(g)=0x3333; KH_RBP(g)=0x4444;
      u64 sa[2]={out,0x1234567890abcdefull}; ncall(g,(u64)(uintptr_t)t_void,2,sa,NULL,0); T(rd64(g,out)==0x1234567890abcdefull); T(rsp_ok);
      T(KH_RBX(g)==0x1111&&KH_RSI(g)==0x2222&&KH_RDI(g)==0x3333&&KH_RBP(g)==0x4444); KH_RBX(g)=rbx; KH_RSI(g)=rsi; KH_RDI(g)=rdi; KH_RBP(g)=rbp; }
    /* MXCSR is restored after a callee that changes it (the lifted float policy needs gradual underflow) */
    { u32 m0=0,m1=0; __asm__ __volatile__("stmxcsr %0":"=m"(m0)); T(ncall(g,(u64)(uintptr_t)t_fpstate,0,NULL,NULL,0)==7); __asm__ __volatile__("stmxcsr %0":"=m"(m1)); T(m0==m1); T(rsp_ok);
      volatile float tiny=1e-38f, quarter=0.25f; volatile float den=tiny*quarter; T(den>0.0f); }
    /* through kh_call5 (runtime -> guest call path): fifth argument in the first stack slot */
    { u64 e[16]={5,6,7,8,9,0,0,0,0,0,0,0,0,0,0,0}; u64 hh=0xcbf29ce484222325ull; for(int i=0;i<16;i++) hh=(hh^e[i])*0x100000001b3ull;
      u64 sp=KH_RSP(g); u64 base=((sp-0x100)&~0xFull); memset(hp(g,base+0x28),0,11*8);   /* kh_call5 writes only slot 4; zero the slots the callee also reads */
      T(kh_call5(g,(u64)(uintptr_t)t_stack12,5,6,7,8,9)==hh); T(KH_RSP(g)==sp); }
    /* call counter and watch counters */
    static _Atomic u64 w_calls, w_rdx;
    kh_native_watch((u64)(uintptr_t)t_ints,&w_calls,0); kh_native_watch((u64)(uintptr_t)t_ints,&w_rdx,1);
    { u64 sa[4]={0,17,0,0}; u64 c0=kh_native_calls; ncall(g,(u64)(uintptr_t)t_ints,4,sa,NULL,0); ncall(g,(u64)(uintptr_t)t_ints,4,sa,NULL,0); T(kh_native_calls==c0+2); T(w_calls==2); T(w_rdx==34); }
    T(kh_native_calls>calls0);
    /* post hook: sees the register arguments, the stack arguments and the result of the native call */
    kh_native_post((u64)(uintptr_t)t_stack12,t_post);
    { post_sum=0; u64 r=ncall(g,(u64)(uintptr_t)t_stack12,16,s,NULL,0); T(r==h); T(post_sum==s[0]+s[3]+s[4]+s[15]+h); T(rsp_ok); }
    /* override: the handler takes the call (no native call counted) or declines and the native code runs */
    kh_native_override((u64)(uintptr_t)t_ints,t_override);
    { u64 c0=kh_native_calls; u64 hit[4]={77,3,0,0}, miss[4]={1,2,3,4}; ov_seen=0;
      T(ncall(g,(u64)(uintptr_t)t_ints,4,hit,NULL,0)==5003); T(rsp_ok); T(kh_native_calls==c0);
      T(ncall(g,(u64)(uintptr_t)t_ints,4,miss,NULL,0)==1234); T(rsp_ok); T(kh_native_calls==c0+1); T(ov_seen==2); }
    /* outside every registered range: still the loud fault (a libc function, the byte after the range, a null pointer, an image address
     * that is not a function start) */
    T(fault_case(g,(u64)(uintptr_t)&getpid));
    T(fault_case(g,0)); T(fault_case(g,GS_BASE+1));
    kh_native_unregister(lo,hi); T(kh_native_range_count()==before);
    T(fault_case(g,(u64)(uintptr_t)t_ints));
    ga_free(out);
    fprintf(stderr,"kh: native-call selftest: %d checks, %d failures\n",checks,fails);
    return fails;
#else
    (void)g; fprintf(stderr,"kh: native-call selftest: SKIPPED (the Windows-ABI trampoline exists only on x86-64 with the identity address model)\n");
    return 0;
#endif
}
