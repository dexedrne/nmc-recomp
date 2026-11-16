/* native_oracle.c : run the ORIGINAL x86-64 bytes of a function natively (real silicon) for differential tests.
 * Used for functions Unicorn cannot execute (256-bit AVX, rcpps/rsqrtps).  Callees are replaced by the same deterministic stubs
 * as the lifted-C side: their entry bytes are patched to INT3 by the harness and the SIGTRAP handler applies kh_test_stub() to
 * the interrupted register state, pops the return address and resumes.  x86-64 Linux only (dev oracle, never shipped). */
#define _GNU_SOURCE
#include "khrt.h"
#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct { u64 gpr[16]; u8 ymm[16][32]; u64 gs; } NState;     /* gpr: rax rcx rdx rbx rsp rbp rsi rdi r8..r15 ; ymm at +128 */
void kh_test_stub(KCtx *g, u64 va);
__attribute__((visibility("hidden"))) NState *kn_out; __attribute__((visibility("hidden"))) u64 kn_saved_rsp, kn_scratch, kn_entry;
static sigjmp_buf nb; static volatile int nsig; static volatile unsigned long naddr;
static const u64 *nset; static size_t ncnt;
u64 kn_lazy[48]; int kn_nlazy; u64 kn_pc;
#include <sys/mman.h>
extern void kh_tramp(NState *in, NState *out, u64 entry);
extern char kh_tramp_ret[];
u64 native_sentinel(void){ return (u64)(uintptr_t)kh_tramp_ret; }

#define GST(i,reg) "  mov %%" #reg ", " #i "(%%rax)\n"
__asm__(
".text\n.globl kh_tramp\n.type kh_tramp,@function\nkh_tramp:\n"
"  push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
"  mov %rsp, kn_saved_rsp(%rip)\n"
"  mov %rsi, kn_out(%rip)\n"
"  mov %rdx, kn_entry(%rip)\n"
"  vmovups 128(%rdi), %ymm0\n vmovups 160(%rdi), %ymm1\n vmovups 192(%rdi), %ymm2\n vmovups 224(%rdi), %ymm3\n"
"  vmovups 256(%rdi), %ymm4\n vmovups 288(%rdi), %ymm5\n vmovups 320(%rdi), %ymm6\n vmovups 352(%rdi), %ymm7\n"
"  vmovups 384(%rdi), %ymm8\n vmovups 416(%rdi), %ymm9\n vmovups 448(%rdi), %ymm10\n vmovups 480(%rdi), %ymm11\n"
"  vmovups 512(%rdi), %ymm12\n vmovups 544(%rdi), %ymm13\n vmovups 576(%rdi), %ymm14\n vmovups 608(%rdi), %ymm15\n"
"  mov 8(%rdi), %rcx\n mov 16(%rdi), %rdx\n mov 24(%rdi), %rbx\n mov 40(%rdi), %rbp\n mov 48(%rdi), %rsi\n"
"  mov 64(%rdi), %r8\n mov 72(%rdi), %r9\n mov 80(%rdi), %r10\n mov 88(%rdi), %r11\n"
"  mov 96(%rdi), %r12\n mov 104(%rdi), %r13\n mov 112(%rdi), %r14\n mov 120(%rdi), %r15\n"
"  mov 0(%rdi), %rax\n"
"  mov 32(%rdi), %rsp\n"
"  mov 56(%rdi), %rdi\n"
"  jmp *kn_entry(%rip)\n"
".globl kh_tramp_ret\nkh_tramp_ret:\n"
"  mov %rax, kn_scratch(%rip)\n"
"  mov kn_out(%rip), %rax\n"
"  mov %rcx, 8(%rax)\n mov %rdx, 16(%rax)\n mov %rbx, 24(%rax)\n mov %rsp, 32(%rax)\n mov %rbp, 40(%rax)\n mov %rsi, 48(%rax)\n mov %rdi, 56(%rax)\n"
"  mov %r8, 64(%rax)\n mov %r9, 72(%rax)\n mov %r10, 80(%rax)\n mov %r11, 88(%rax)\n mov %r12, 96(%rax)\n mov %r13, 104(%rax)\n mov %r14, 112(%rax)\n mov %r15, 120(%rax)\n"
"  vmovups %ymm0, 128(%rax)\n vmovups %ymm1, 160(%rax)\n vmovups %ymm2, 192(%rax)\n vmovups %ymm3, 224(%rax)\n"
"  vmovups %ymm4, 256(%rax)\n vmovups %ymm5, 288(%rax)\n vmovups %ymm6, 320(%rax)\n vmovups %ymm7, 352(%rax)\n"
"  vmovups %ymm8, 384(%rax)\n vmovups %ymm9, 416(%rax)\n vmovups %ymm10, 448(%rax)\n vmovups %ymm11, 480(%rax)\n"
"  vmovups %ymm12, 512(%rax)\n vmovups %ymm13, 544(%rax)\n vmovups %ymm14, 576(%rax)\n vmovups %ymm15, 608(%rax)\n"
"  mov kn_scratch(%rip), %rcx\n mov %rcx, 0(%rax)\n"
"  vzeroupper\n"
"  mov kn_saved_rsp(%rip), %rsp\n"
"  pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
"  ret\n"
);

static int in_set(u64 t){ size_t lo=0,hi=ncnt; while(lo<hi){ size_t m=(lo+hi)/2; if(nset[m]<t) lo=m+1; else hi=m; } return lo<ncnt && nset[lo]==t; }
static void on_trap(int s, siginfo_t *si, void *vu){
    ucontext_t *uc=(ucontext_t*)vu; greg_t *r=uc->uc_mcontext.gregs; u64 pc=(u64)r[REG_RIP]-1;
    if(s==SIGTRAP && in_set(pc)){
        KCtx k; memset(&k,0,sizeof k);
        KH_R64(&k,KO_RAX)=r[REG_RAX]; KH_R64(&k,KO_RCX)=r[REG_RCX]; KH_R64(&k,KO_RDX)=r[REG_RDX]; KH_R64(&k,KO_RSP)=r[REG_RSP];
        KH_R64(&k,KO_R8)=r[REG_R8]; KH_R64(&k,KO_R9)=r[REG_R9]; KH_R64(&k,KO_R10)=r[REG_R10]; KH_R64(&k,KO_R11)=r[REG_R11];
        for(int i=0;i<6;i++) memcpy(&KH_XMM(&k,i),&uc->uc_mcontext.fpregs->_xmm[i],16);
        u64 ret; memcpy(&ret,(void*)(uintptr_t)r[REG_RSP],8);
        kh_test_stub(&k,pc);
        r[REG_RAX]=KH_RAX(&k); r[REG_RCX]=KH_RCX(&k); r[REG_RDX]=KH_RDX(&k); r[REG_RSP]=KH_RSP(&k);
        r[REG_R8]=KH_R8(&k); r[REG_R9]=KH_R9(&k); r[REG_R10]=KH_R64(&k,KO_R10); r[REG_R11]=KH_R64(&k,KO_R11);
        for(int i=0;i<6;i++) memcpy(&uc->uc_mcontext.fpregs->_xmm[i],&KH_XMM(&k,i),16);
        r[REG_RIP]=(greg_t)ret; return;
    }
    if(s==SIGSEGV && si->si_addr){ u64 a=(u64)(uintptr_t)si->si_addr, pg=a&~0xfffull;       /* lazily map low pages like the Unicorn oracle */
        if(pg>=0x100000 && pg<0x7000000000000ull/16 && kn_nlazy<48 && mmap((void*)(uintptr_t)pg,0x1000,7,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)(uintptr_t)pg){ kn_lazy[kn_nlazy++]=pg; return; } }
    kn_pc=pc; nsig=s; naddr=(s==SIGSEGV||s==SIGBUS)?(unsigned long)si->si_addr:(unsigned long)pc; siglongjmp(nb,1);
}
static char altstk[1<<16];
void native_config(const u64 *set, size_t n){ nset=set; ncnt=n; stack_t ss; ss.ss_sp=altstk; ss.ss_size=sizeof altstk; ss.ss_flags=0; sigaltstack(&ss,0); }
/* returns 0 ok; else signal number (SIGALRM = timeout) ; addr of fault in *addr */
int native_run(NState *in, NState *out, u64 entry, unsigned long *addr, int timeout_ms){
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_trap; sa.sa_flags=SA_SIGINFO|SA_NODEFER|SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGTRAP,&sa,0); sigaction(SIGALRM,&sa,0);
    struct itimerval it={{0,0},{timeout_ms/1000,(timeout_ms%1000)*1000}}; setitimer(ITIMER_REAL,&it,0);
    syscall(SYS_arch_prctl,0x1001 /*ARCH_SET_GS*/,in->gs); kn_nlazy=0;
    if(sigsetjmp(nb,1)){ struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0); *addr=naddr; return nsig; }
    kh_tramp(in,out,entry);
    struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_REAL,&z,0);
    return 0;
}
