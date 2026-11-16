/* khtest.c : differential-test runtime.  Callees are replaced by deterministic stubs that the Unicorn side reproduces
 * (tools/difftest.py `stub_effect`); dispatch to anything that is not a known function start / import address faults. */
#define _GNU_SOURCE
#include "khrt.h"
#include <signal.h>
#include <setjmp.h>
static sigjmp_buf jb; static volatile unsigned long faddr; static volatile int fsig;
static void sh(int s, siginfo_t *si, void *u){ (void)u; faddr=(unsigned long)si->si_addr; fsig=s; siglongjmp(jb,1); }
/* returns 0 ok, 1 hardware fault (sig,addr), 2 kh_fault() */
static char kt_alt[1<<16];
void khtest_init(void){ stack_t ss; ss.ss_sp=kt_alt; ss.ss_size=sizeof kt_alt; ss.ss_flags=0; sigaltstack(&ss,0); }
int khtest_call(void (*fn)(KCtx*), KCtx *g, unsigned long *addr, int *sig){
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=sh; sa.sa_flags=SA_SIGINFO|SA_NODEFER|SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGFPE,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGTRAP,&sa,0);
    if(sigsetjmp(jb,1)){ *addr=faddr; *sig=fsig; return fsig==-1?2:1; }
    fn(g); return 0;
}
void kh_fault(KCtx *g, u64 va, const char *kind){ (void)g; faddr=va; fsig=-1; (void)kind; siglongjmp(jb,1); }

