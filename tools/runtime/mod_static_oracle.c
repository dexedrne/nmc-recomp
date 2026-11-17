/* mod_static_oracle.c : Task 12 differential-test oracle (x86-64 Linux dev host only, never shipped).
 *
 * = the lifter's native oracle (tools/khlift/native_oracle.c, #included unmodified: original EXE bytes run on the CPU, callee entries
 *   are INT3 and get the deterministic stub effect) plus two additions:
 *
 *   1. entry thunk: clears CF/PF/AF/ZF/SF/OF before jumping to the entry, so a test may ENTER IN THE MIDDLE of a function (at a
 *      lifted block boundary) with the same flags as the lifted side's initial state (cc thunk all zero = no flag set);
 *   2. inline detour sites: in the oracle image the first byte of each site is INT3.  The trap runs the SAME C handler the
 *      re-lifted function calls (runtime/mod_static_sites.c) on the interrupted registers, stores its register and flag results
 *      and resumes at the handler's ok / skip address.  No DLL bytes are executed here, natively or emulated: the handler logic
 *      itself is checked by the runtime selftest against the DLL listing, this oracle checks everything around it;
 *   3. block stops for single-block tests (see kn_stop_add).
 */
#define _GNU_SOURCE
#define on_trap kn_base_trap
#define native_run kn_base_run
#include "native_oracle.c"
#undef on_trap
#undef native_run

__attribute__((visibility("hidden"))) u64 kn_real_entry, kn_scratch2;
extern char kn_entry_thunk[];
__asm__(
".text\n.globl kn_entry_thunk\n.type kn_entry_thunk,@function\nkn_entry_thunk:\n"
"  mov %rax, kn_scratch2(%rip)\n"
"  mov $1, %eax\n"
"  add $1, %eax\n"                      /* 1+1 = 2: CF=0 PF=0 AF=0 ZF=0 SF=0 OF=0 */
"  mov kn_scratch2(%rip), %rax\n"
"  jmp *kn_real_entry(%rip)\n"
);

static struct { u64 site, ok, skip; int (*fn)(KCtx *); } ksite[4];
static int nksite; u64 kn_site_hits;
void kn_site_clear(void){ nksite=0; }
int kn_site_add(u64 site,u64 ok,u64 skip,int (*fn)(KCtx *)){ if(nksite>=4) return -1; ksite[nksite].site=site; ksite[nksite].ok=ok; ksite[nksite].skip=skip; ksite[nksite].fn=fn; return nksite++; }
static const int greg_of[16]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};

/* single-block tests: INT3 on the start of every other block of the function; reaching one ends the run there (state as of the
 * block boundary is captured by the normal return path) and reports which block was reached. */
static u64 kstop[1024]; static int nkstop; u64 kn_stop_pc;
void kn_stop_clear(void){ nkstop=0; kn_stop_pc=0; }
int kn_stop_add(u64 va){ if(nkstop>=1024) return -1; kstop[nkstop++]=va; return 0; }

static void on_trap2(int s, siginfo_t *si, void *vu){
    ucontext_t *uc=(ucontext_t*)vu; greg_t *r=uc->uc_mcontext.gregs;
    if(s==SIGTRAP){
        u64 pc=(u64)r[REG_RIP]-1;
        for(int i=0;i<nkstop;i++) if(pc==kstop[i]){ kn_stop_pc=pc; r[REG_RIP]=(greg_t)(uintptr_t)kh_tramp_ret; return; }
        for(int i=0;i<nksite;i++) if(pc==ksite[i].site){
            KCtx k; memset(&k,0,sizeof k);
            for(int j=0;j<16;j++) KH_R64(&k,KO_RAX+8*j)=(u64)r[greg_of[j]];
            int ok=ksite[i].fn(&k);
            for(int j=0;j<16;j++) r[greg_of[j]]=(greg_t)KH_R64(&k,KO_RAX+8*j);
            u64 fl=rflags_all(KH_R64(&k,KO_CC_OP),KH_R64(&k,KO_CC_DEP1),KH_R64(&k,KO_CC_DEP2),KH_R64(&k,KO_CC_NDEP));
            r[REG_EFL]=(greg_t)(((u64)r[REG_EFL]&~0x8d5ull)|fl);
            r[REG_RIP]=(greg_t)(ok?ksite[i].ok:ksite[i].skip); kn_site_hits++; return;
        }
    }
    kn_base_trap(s,si,vu);
}
/* same contract as the lifter's native_run: 0 ok, else the signal number; fault address in *addr.  The time limit is CPU time
 * (ITIMER_VIRTUAL -> SIGVTALRM), so a loaded host does not cut short runs, and it is small: a run that loops over random data for
 * long is not a useful test and the lifted side (no limit) would take many times longer on it. */
int native_run(NState *in, NState *out, u64 entry, unsigned long *addr, int timeout_ms){
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_trap2; sa.sa_flags=SA_SIGINFO|SA_NODEFER|SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGTRAP,&sa,0); sigaction(SIGVTALRM,&sa,0);
    struct itimerval it={{0,0},{timeout_ms/1000,(timeout_ms%1000)*1000}}; setitimer(ITIMER_VIRTUAL,&it,0);
    syscall(SYS_arch_prctl,0x1001 /*ARCH_SET_GS*/,in->gs); kn_nlazy=0;
    if(sigsetjmp(nb,1)){ struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_VIRTUAL,&z,0); *addr=naddr; return nsig; }
    kn_real_entry=entry;
    kh_tramp(in,out,(u64)(uintptr_t)kn_entry_thunk);
    struct itimerval z={{0,0},{0,0}}; setitimer(ITIMER_VIRTUAL,&z,0);
    return 0;
}
