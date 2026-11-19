#include "savestate.h"
/* threads.c : guest threads (pthreads), TEB/TLS blocks, wait objects, critical sections, fibers (stackful coroutines with an
 * x86-64 and an arm64 context switch), kernel32 thread/sync/fiber imports and the msvcp140 _Mtx_* imports. */
#include "khnmc_rt.h"
#include <sys/mman.h>
#include <setjmp.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

void kh_set_cur_g(KCtx *g); void kh_install_sigaltstack(void); void kh_release_sigaltstack(void); void kh_guest_bt(KCtx *g,FILE *f);
#if defined(__x86_64__)
#define KH_CPU_RELAX() __asm__ __volatile__("pause")
#else
#define KH_CPU_RELAX() __asm__ __volatile__("yield")
#endif

/* ---- stress knob ----------------------------------------------------------------------------------------------------- */
/* KH_SYNC_CHAOS=n : at the entry of every sync primitive (critical section, wait, event, semaphore, fiber switch), with probability
 * 1/n, yield or sleep up to 200 us.  Any delay there is a schedule a preemptive OS may produce, so a run that only fails with the
 * knob has found a real ordering bug, not an artefact. */
static u32 sync_chaos; static _Atomic u64 sync_chaos_hits;
static __attribute__((noinline)) void sync_chaos_slow(void){
    static __thread u32 rs; u32 x=rs; if(!x) x=0x9e3779b9u^(u32)(uintptr_t)&rs^(u32)kh_now_ns(); if(!x) x=1;
    x^=x<<13; x^=x>>17; x^=x<<5; rs=x;
    if(x%sync_chaos) return; atomic_fetch_add(&sync_chaos_hits,1);
    if(x&0x10000u) sched_yield(); else { struct timespec ts={0,(long)((x>>8)%200u)*1000L}; nanosleep(&ts,0); }
}
#define SYNC_CHAOS() do{ if(__builtin_expect(sync_chaos,0)) sync_chaos_slow(); }while(0)
static _Atomic u64 cnt_self_suspend, cnt_foreign_suspend, cnt_fiber_create, cnt_fiber_switch, cnt_fiber_below;

/* ---- host context switch --------------------------------------------------------------------------------------------- */
void kh_ctx_switch(void **save_sp, void *new_sp);
void kh_fiber_tramp(void);
#if defined(__x86_64__)
__asm__(".text\n.globl kh_ctx_switch\n.type kh_ctx_switch,@function\nkh_ctx_switch:\n"
        " pushq %rbp\n pushq %rbx\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
        " movq %rsp,(%rdi)\n movq %rsi,%rsp\n"
        " popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbx\n popq %rbp\n ret\n"
        ".size kh_ctx_switch,.-kh_ctx_switch\n"
        ".globl kh_fiber_tramp\n.type kh_fiber_tramp,@function\nkh_fiber_tramp:\n"
        " movq %r12,%rdi\n call kh_fiber_main\n ud2\n.size kh_fiber_tramp,.-kh_fiber_tramp\n");
#define INIT_FRAME_WORDS 7
static void *make_frame(void *top,void *arg){
    u64 *sp=(u64*)(((uintptr_t)top)&~0xFull); sp-=2;      /* keep 16-byte alignment at the tramp's call */
    *--sp=(u64)kh_fiber_tramp;                             /* return address of ctx_switch */
    *--sp=0;            /* rbp */ *--sp=0; /* rbx */ *--sp=(u64)arg; /* r12 */ *--sp=0; *--sp=0; *--sp=0; /* r13 r14 r15 */
    return sp;
}
#elif defined(__aarch64__)
/* C symbols carry the object format's prefix in assembly: none on ELF, an underscore on Mach-O (macOS / iOS) */
#define KH_ASM_STR_(x) #x
#define KH_ASM_STR(x) KH_ASM_STR_(x)
#define KH_ASM_SYM(name) KH_ASM_STR(__USER_LABEL_PREFIX__) #name
__asm__(".text\n.p2align 2\n.globl " KH_ASM_SYM(kh_ctx_switch) "\n" KH_ASM_SYM(kh_ctx_switch) ":\n"
        " sub sp, sp, #176\n stp x19,x20,[sp,#0]\n stp x21,x22,[sp,#16]\n stp x23,x24,[sp,#32]\n stp x25,x26,[sp,#48]\n stp x27,x28,[sp,#64]\n stp x29,x30,[sp,#80]\n"
        " stp d8,d9,[sp,#96]\n stp d10,d11,[sp,#112]\n stp d12,d13,[sp,#128]\n stp d14,d15,[sp,#144]\n"
        " mov x9, sp\n str x9,[x0]\n mov sp, x1\n"
        " ldp x19,x20,[sp,#0]\n ldp x21,x22,[sp,#16]\n ldp x23,x24,[sp,#32]\n ldp x25,x26,[sp,#48]\n ldp x27,x28,[sp,#64]\n ldp x29,x30,[sp,#80]\n"
        " ldp d8,d9,[sp,#96]\n ldp d10,d11,[sp,#112]\n ldp d12,d13,[sp,#128]\n ldp d14,d15,[sp,#144]\n add sp, sp, #176\n ret\n"
        ".p2align 2\n.globl " KH_ASM_SYM(kh_fiber_tramp) "\n" KH_ASM_SYM(kh_fiber_tramp) ":\n mov x0, x19\n bl " KH_ASM_SYM(kh_fiber_main) "\n brk #0\n");
static void *make_frame(void *top,void *arg){
    u64 *sp=(u64*)(((uintptr_t)top)&~0xFull); sp-=22;
    memset(sp,0,176); sp[0]=(u64)arg; sp[11]=(u64)kh_fiber_tramp;       /* x19 = arg ; x30 = tramp */
    return sp;
}
#else
#error unsupported host architecture
#endif

/* ---- per-thread access ----------------------------------------------------------------------------------------------- */
static __thread KThread *tl_thread;
__attribute__((noinline)) KThread *cur_thread(void){ KThread *t; __asm__ __volatile__("" ::: "memory"); t=tl_thread; return t; }
static pthread_mutex_t tlist_m=PTHREAD_MUTEX_INITIALIZER;
/* the registry of LIVE guest threads (OpenThread, names, stack reports, the quiescence test of KHNMC_CLOCK_SYNC=all).  Review U35 n=33:
 * it used to keep every thread ever created and silently stopped registering at 4096 (the engine starts a worker every 4-5 s of play:
 * about 5 hours).  A thread now leaves it when it ends (its KThread stays allocated for its handles); a full registry is the named fatal
 * thread-registry-full.  tlist_created / tlist_ended count over the process's life (map reports, the sync report). */
#define TLIST_MAX 4096
static KThread *tlist[TLIST_MAX]; static u32 ntlist, tlist_cap=TLIST_MAX; static u64 tlist_created, tlist_ended; static _Atomic u32 next_tid=1000;
int kh_cut_threads(KThread **parked,unsigned count,int report){
    int missing=0;
    pthread_mutex_lock(&tlist_m);
    for(u32 i=0;i<ntlist;i++){
        KThread *t=tlist[i];
        if(t==cur_thread()) continue;
        unsigned k=0;
        while(k<count&&parked[k]!=t) k++;
        if(k<count) continue;
        missing++;
        if(report) fprintf(stderr,"kh: savestate-missing tid=%u start=%llx clock-state=%d\n",t->tid,(unsigned long long)t->start,atomic_load(&t->vc_state));
    }
    pthread_mutex_unlock(&tlist_m);
    return missing;
}
static int tlist_add_locked(KThread *t){ if(ntlist>=tlist_cap) return 0; tlist[ntlist++]=t; tlist_created++; return 1; }      /* caller holds tlist_m */
static void tlist_remove(KThread *t){
    pthread_mutex_lock(&tlist_m);
    for(u32 i=0;i<ntlist;i++) if(tlist[i]==t){ tlist[i]=tlist[--ntlist]; tlist[ntlist]=NULL; tlist_ended++; break; }
    pthread_mutex_unlock(&tlist_m);
}
static KThread *tlist_find(u32 tid){ KThread *f=NULL; pthread_mutex_lock(&tlist_m); for(u32 i=0;i<ntlist;i++) if(tlist[i]->tid==tid) f=tlist[i]; pthread_mutex_unlock(&tlist_m); return f; }
u64 kh_tls_tmpl_lo, kh_tls_tmpl_hi, kh_tls_index_va, kh_tls_callbacks_va;

KCtx *kh_new_ctx(void){ KCtx *g=aligned_alloc(64,(sizeof(KCtx)+63)&~63ull); memset(g,0,sizeof *g); PUT(u64,176,1); g->bias=gs_bias; return g; }
u64 kh_new_guest_stack(u64 size,u64 *lo){ u64 p=ga_pages_src(size,GA_SRC_STACK); if(!p){ fprintf(stderr,"kh: out of guest space for stacks\n"); abort(); } if(lo)*lo=p; return p+size-0x80; }
void kh_ctx_set_stack(KCtx *g,u64 top){ KH_RSP(g)=top&~0xFull; }

#define TEB_SIZE 0x1000
u64 kh_make_teb(KThread *t){
    KCtx *g=t->g; t->teb=ga_alloc_src(TEB_SIZE,16,GA_SRC_TLS); memset(hp(g,t->teb),0,TEB_SIZE);
    t->tlsarr=ga_alloc_src(64*8,16,GA_SRC_TLS); memset(hp(g,t->tlsarr),0,64*8);
    u64 sz=kh_tls_tmpl_hi-kh_tls_tmpl_lo; t->tlsblk=ga_alloc_src(sz+0x100,16,GA_SRC_TLS); memset(hp(g,t->tlsblk),0,sz+0x100);
    if(sz) memcpy(hp(g,t->tlsblk),hp(g,kh_tls_tmpl_lo),sz);
    u32 idx=kh_tls_index_va?rd32(g,kh_tls_index_va):0; if(idx>=64) idx=0;
    wr64(g,t->tlsarr+8ull*idx,t->tlsblk);
    wr64(g,t->teb+0x30,t->teb); wr64(g,t->teb+0x58,t->tlsarr); wr64(g,t->teb+0x08,t->stack_hi); wr64(g,t->teb+0x10,t->stack_lo);
    wr64(g,t->teb+0x40,1); wr64(g,t->teb+0x48,t->tid);
    KH_GS(g)=t->teb; return t->teb;
}
void kh_run_tls_callbacks(KCtx *g,u32 reason){
    if(!kh_tls_callbacks_va) return;
    for(u64 p=kh_tls_callbacks_va;;p+=8){ u64 cb=rd64(g,p); if(!cb) break; kh_call(g,cb,GS_BASE,reason,0,0); }
}
static KThread *thread_new(void){
    KThread *t=calloc(1,sizeof *t); t->g=kh_new_ctx(); t->tid=atomic_fetch_add(&next_tid,4); t->prio=0;
    pthread_mutex_init(&t->m,0); kh_cond_init(&t->cv); pthread_mutex_init(&t->vc_m,0);
    pthread_mutex_lock(&tlist_m); int ok=tlist_add_locked(t); u32 n=ntlist; pthread_mutex_unlock(&tlist_m);
    if(!ok) kh_fatal(NULL,"thread-registry-full: %u guest threads are alive at once, the registry holds %u (an ended thread leaves it); thread %u is not created",n,tlist_cap,t->tid);
    return t;
}
static void thread_bind(KThread *t){ tl_thread=t; t->g->rt=t; kh_install_sigaltstack(); kh_set_cur_g(t->g); }
void kh_thread_init_main(void){
    { const char *e=getenv("KH_SYNC_CHAOS"); if(e&&*e){ sync_chaos=(u32)strtoul(e,0,0); if(sync_chaos) fprintf(stderr,"kh: sync chaos: a random yield or sleep at 1 of %u sync primitive calls\n",sync_chaos); } }
    KThread *t=thread_new(); t->is_main=1; u64 top=kh_new_guest_stack(8ull<<20,&t->stack_lo); t->stack_hi=t->stack_lo+(8ull<<20);
    kh_ctx_set_stack(t->g,top); kh_make_teb(t); thread_bind(t); snprintf(t->name,sizeof t->name,"main");
}

/* ---- wait objects ---------------------------------------------------------------------------------------------------- */
/* Win32 behaviour reproduced here (each line has a check in kh_sync_selftest):
 *  - manual-reset event: SetEvent releases EVERY thread waiting at that moment, even when ResetEvent follows at once (the CRT's
 *    thread-safe-static fallback does exactly SetEvent + ResetEvent); the event stays set until reset.
 *  - auto-reset event: SetEvent releases one waiter and resets, or stays set until one wait consumes it.  The release is granted to a
 *    thread that is blocked at that moment (a token, review U35 n=4): two SetEvents before either of two blocked waiters has run release
 *    both, as on Windows (the shared bit used to merge them into one); a thread that begins to wait after the SetEvent cannot take it.
 *  - PulseEvent (only reached through timeSetEvent TIME_CALLBACK_EVENT_PULSE): manual = all current waiters, auto = one current
 *    waiter; the event is left reset, a pulse with nobody waiting is lost.
 *  - a thread handle is signalled when the thread ends; the wait returns then, not at the next poll.
 *  - an object lives while a thread waits on it, also when its last handle is closed meanwhile.
 *  - timeouts are relative and immune to wall-clock steps (CLOCK_MONOTONIC). */
static KWaitObj *wobj_new(int kind){ KWaitObj *w=calloc(1,sizeof *w); w->kind=kind; w->refs=1; pthread_mutex_init(&w->m,0);
    kh_cond_init(&w->cv); return w; }
static void abs_deadline(struct timespec *ts,u32 ms){ clock_gettime(CLOCK_MONOTONIC,ts); ts->tv_sec+=ms/1000; ts->tv_nsec+=(long)(ms%1000)*1000000L; if(ts->tv_nsec>=1000000000L){ ts->tv_sec++; ts->tv_nsec-=1000000000L; } }
u32 kh_wait_timeout_ms_max;
/* one test of the object for the calling thread; takes what a satisfied wait takes.  blocked = the thread has been waiting already
 * (only such a thread can be released by a token of an auto-reset event: SetEvent or a pulse while it was blocked).  gen0 = w->gen when
 * the wait began: an auto-reset event bumps gen with every token it grants, so a token is only for a thread that was waiting before it.
 * A blocked thread takes its token before the set state: a SetEvent after the one that released it leaves the event set for the next. */
static int wobj_try(KWaitObj *w,int k,KThread *me,u64 gen0,int blocked){
    if(k==HK_EVENT){
        if(w->manual) return w->signaled||w->gen!=gen0;
        if(blocked && w->pulse_tokens>0 && w->gen!=gen0){ w->pulse_tokens--; return 1; }
        if(w->signaled){ w->signaled=0; return 1; }
        return 0; }
    if(k==HK_SEM){ if(w->count>0){ w->count--; return 1; } return 0; }
    if(k==HK_MUTEX){ if(w->recurse==0||w->owner==me->tid){ w->owner=me->tid; w->recurse++; return 1; } return 0; }
    return 0;
}
/* TASK28, KHNMC_CLOCK_SYNC=all: a time-out is on the virtual clock (a clock client at the deadline: the clock stops there until this
 * thread has woken and is running again), and while the thread blocks it is idle for the quiescence test unless its object is ready */
static int wobj_ready(KWaitObj *w,int k,KThread *t,u64 gen0,int blocked){
    if(k==HK_THREAD) return w->thread&&w->thread->done;
    if(k==HK_EVENT){ if(w->signaled) return 1; if(w->manual) return w->gen!=gen0; return blocked&&w->pulse_tokens>0&&w->gen!=gen0; }
    if(k==HK_SEM) return w->count>0;
    if(k==HK_MUTEX) return w->recurse==0||w->owner==t->tid;
    return 1;
}
static _Atomic u64 vc_vwaits;
/* Order input and sound initialization against subsequent parent allocations.
 * Sound handlers can allocate a timer thread and stack before their first wait. */
static void cut_boot_ready(KCtx *g){
    KThread *t=cur_thread();
    if(!t||!atomic_load(&t->cut_boot_wait)) return;
    u64 site=rd64(g,KH_RSP(g))-GS_BASE;
    if(site!=0x44f710&&site!=0x44d960&&site!=0x97d4f&&site!=0xfb0e0) return;
    pthread_mutex_lock(&t->m);
    atomic_store(&t->cut_boot_wait,0);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
}
u32 kh_wait(KCtx *g,u64 h,u32 ms){
    KThread *me=cur_thread(); SYNC_CHAOS();
    if(kh_cut_enabled) cut_boot_ready(g);
    if(h==PSEUDO_THREAD||h==PSEUDO_PROCESS) return 0x102;
    int k=0; KWaitObj *w=h_get_ref(h,&k); if(!w){ kh_set_last_error(6); return 0xFFFFFFFFu; }
    int vsync=kh_vclock_sync&&me, vcl=-1; u64 vdl=0;                                  /* the idle mark from the start; virtual time-outs once the clock steps */
    if(vsync&&kh_vclock_stepped()&&ms!=0xFFFFFFFFu&&ms){ vdl=kh_vclock_now_ns()+(u64)ms*1000000ull; vcl=kh_vclock_deadline_add(vdl); vc_vwaits++; }
    struct timespec dl; if(ms!=0xFFFFFFFFu) abs_deadline(&dl,ms);
    pthread_mutex_t *m=&w->m; pthread_cond_t *cv=&w->cv; KThread *th=NULL;
    if(k==HK_THREAD){ th=w->thread; m=&th->m; cv=&th->cv; }                 /* signalled by the thread's own exit notification */
    pthread_mutex_lock(m);
    u32 res=0; u64 waited=0, gen0=w->gen; int blocked=0;
    for(;;){
        if(atomic_load(&kh_cut_requested)) kh_cut_poll("handle-wait",m);
        if(th?th->done:wobj_try(w,k,me,gen0,blocked)) break;
        if(ms==0){ res=0x102; break; }
        if(!blocked){ blocked=1; if(!th){ w->waiters++; if(kh_vclock_on) pthread_cond_broadcast(cv); }     /* frames clock: a timer client may wait for a consumer to be back (kh_wobj_settle) */
            if(vsync){ atomic_store(&me->vc_k,k); atomic_store(&me->vc_gen0,gen0); atomic_store(&me->vc_blocked,1); atomic_store(&me->vc_w,(void*)w); atomic_store(&me->vc_state,1); kh_vclock_idle(2); } }
        int rc;
        if(vcl>=0){                                                                      /* virtual time-out: poll the clock, the object wakes us as usual */
            /* the due test runs the quiescence test, which takes tlist_m, other threads' vc_m and THEIR objects' locks: never with m held
             * (review of task 28: a second thread blocked on this object made the test lock m again, and two timed waiters testing at
             * once took each other's object locks in opposite orders).  m is retaken and the object tested again before a time-out. */
            if(kh_vclock_now_ns()>=vdl){ pthread_mutex_unlock(m); int due=kh_vclock_deadline_due(vcl,vdl); pthread_mutex_lock(m);
                if(due){ if(!(th?th->done:wobj_try(w,k,me,gen0,blocked))) res=0x102; break; } }
            struct timespec d2; abs_deadline(&d2,1); kh_cond_wait_until(cv,m,&d2); continue; }
        if(ms==0xFFFFFFFFu&&kh_cut_enabled){ kh_cut_wait("handle-wait",cv,m); continue; }
        if(ms==0xFFFFFFFFu){ struct timespec d2; abs_deadline(&d2,10000); rc=kh_cond_wait_until(cv,m,&d2);
            if(rc==ETIMEDOUT){ waited+=10; if(kh_verbose||(waited%60==0)) fprintf(stderr,"kh: thread %u ('%s') waiting %llus on handle %llx kind %d (site rva %llx)\n",me->tid,me->name,(unsigned long long)waited,(unsigned long long)h,k,(unsigned long long)(rd64(g,KH_RSP(g))-GS_BASE)); } }
        else { rc=kh_cond_wait_until(cv,m,&dl); if(rc==ETIMEDOUT){ if(!(th?th->done:wobj_try(w,k,me,gen0,blocked))) res=0x102; break; } }
    }
    if(blocked&&!th){ w->waiters--; if(w->pulse_tokens>w->waiters) w->pulse_tokens=w->waiters; }
    if(!th&&res==0){ w->satisfied++; w->last_taker=me; if(kh_vclock_on) pthread_cond_broadcast(cv); }
    if(vsync&&blocked) atomic_store(&me->vc_state,0);                                    /* running again: set while the object's lock is held (the quiescence test takes it) */
    pthread_mutex_unlock(m);
    if(vsync&&blocked){ pthread_mutex_lock(&me->vc_m); atomic_store(&me->vc_w,NULL); pthread_mutex_unlock(&me->vc_m); }
    if(vcl>=0) kh_vclock_deadline_drop(vcl);                                             /* after "running": the clock held at the deadline may go on */
    kh_wobj_unref(w); return res;
}
int kh_wobj_waitstate(u64 h,int *waiters,u64 *satisfied){
    int k; KWaitObj *w=h_get_ref(h,&k); if(!w) return -1; if(k==HK_THREAD){ kh_wobj_unref(w); return -1; }
    pthread_mutex_lock(&w->m); *waiters=w->waiters; *satisfied=w->satisfied; pthread_mutex_unlock(&w->m); kh_wobj_unref(w); return 0;
}
int kh_wobj_settle(u64 h,u64 sat0,int waiters0,u32 timeout_ms,volatile int *stop){
    int k; KWaitObj *w=h_get_ref(h,&k); if(!w) return 0; if(k==HK_THREAD){ kh_wobj_unref(w); return 0; }
    struct timespec dl; abs_deadline(&dl,timeout_ms); int rc=0;
    pthread_mutex_lock(&w->m);
    while(!(w->satisfied>sat0&&w->waiters>=waiters0)){
        if(stop&&*stop){ rc=2; break; }
        struct timespec d2; abs_deadline(&d2,20); if(kh_cond_wait_until(&w->cv,&w->m,&d2)==ETIMEDOUT){ struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); if(now.tv_sec>dl.tv_sec||(now.tv_sec==dl.tv_sec&&now.tv_nsec>=dl.tv_nsec)){ rc=1; break; } }
    }
    pthread_mutex_unlock(&w->m); kh_wobj_unref(w); return rc;
}
static __thread void (*tl_set_hook)(void*,u64); static __thread void *tl_set_ctx;
/* the quiescence test of KHNMC_CLOCK_SYNC=all: every live guest thread idle (a clock wait, parked) or blocked on an object that would not
 * satisfy it now.  Lock order: tlist_m -> the thread's vc_m -> the object's m.  So no caller may hold any object's m (a waiter never takes
 * vc_m or calls this while it holds m: kh_wait releases m around its due test). */
static KThread *const *quiet_only; static u32 quiet_nonly;            /* selftest only: the test looks at these threads (other suites' threads live on) */
static void (*quiet_hook)(KThread *t);                                 /* selftest only: called after a thread was judged idle, inside the test's pass */
/* not running for the quiescence test and the wake hand-off: idle (2), or waiting for a critical section that is owned by ANOTHER thread
 * (3; review of task 28, second fixer: owned by itself = its compare-exchange just took it and it runs, before it marks itself running);
 * a thread blocked on an object (1) is judged by its object */
static int vc_idle_now(KThread *t,int st){ if(st==2) return 1;
    if(st==3){ unsigned long long *o=atomic_load(&t->vc_csown); unsigned long long ow=o?__atomic_load_n(o,__ATOMIC_ACQUIRE):0; return ow!=0&&ow!=(unsigned long long)t->tid; }
    return 0; }
/* review of task 28, second fixer: the test is one pass over the threads, not an atomic snapshot.  A release that makes an idle thread
 * runnable WITHOUT a wake hand-off (LeaveCriticalSection, ResumeThread, ReleaseMutex, a thread's end) between the pass's look at the
 * released thread (still idle then) and its look at the releasing thread (idle again by then, or ended and skipped) made the pass
 * report "quiet" while a thread was runnable.  Every such release bumps vc_rel_seq; a pass during which it moved is not quiet (the
 * caller tests again, as for any running thread).  SetEvent / ReleaseSemaphore do not need it: their waker hands off (vc_after_wake). */
static _Atomic u64 vc_rel_seq, vc_rel_retests;
static inline void vc_released(void){ if(kh_vclock_sync) atomic_fetch_add(&vc_rel_seq,1); }
int kh_threads_quiet(char *who,size_t cap){
    int quiet=1; KThread *self=cur_thread(); u64 seq0=atomic_load(&vc_rel_seq); pthread_mutex_lock(&tlist_m);
    for(u32 i=0;i<ntlist&&quiet;i++){ KThread *t=tlist[i]; if(t->done||t==self) continue;
        if(quiet_only){ int in=0; for(u32 j=0;j<quiet_nonly;j++) in|=quiet_only[j]==t; if(!in) continue; }
        int st=atomic_load(&t->vc_state);
        if(vc_idle_now(t,st)){ if(quiet_hook) quiet_hook(t); continue; }
        if(st==1){ pthread_mutex_lock(&t->vc_m); KWaitObj *w=(KWaitObj*)atomic_load(&t->vc_w);
            if(w){ pthread_mutex_t *wm=(atomic_load(&t->vc_k)==HK_THREAD&&w->thread)?&w->thread->m:&w->m; pthread_mutex_lock(wm);
                int idle=atomic_load(&t->vc_state)==1&&!wobj_ready(w,atomic_load(&t->vc_k),t,atomic_load(&t->vc_gen0),atomic_load(&t->vc_blocked));
                pthread_mutex_unlock(wm); pthread_mutex_unlock(&t->vc_m); if(idle){ if(quiet_hook) quiet_hook(t); continue; } }
            else pthread_mutex_unlock(&t->vc_m); }
        quiet=0; if(who) snprintf(who,cap,"%u '%s'%s",t->tid,t->name,st==1?" (woken)":st==3?" (critical section free or just taken)":"");
    }
    pthread_mutex_unlock(&tlist_m);
    if(quiet&&atomic_load(&vc_rel_seq)!=seq0){ quiet=0; vc_rel_retests++; if(who) snprintf(who,cap,"(a thread was released during the test)"); }
    return quiet;
}
u64 kh_threads_vwaits(void){ return vc_vwaits; }
u64 kh_threads_rel_retests(void){ return vc_rel_retests; }
void kh_set_event_hook(void (*fn)(void*,u64),void *ctx){ tl_set_hook=fn; tl_set_ctx=ctx; }

/* ---- critical sections (state lives in the guest CRITICAL_SECTION: +0xC recursion, +0x10 owner) ---------------------------- */
static inline u64 cs_owner(KCtx *g,u64 cs){ return __atomic_load_n((u64*)XL(g,cs+0x10),__ATOMIC_ACQUIRE); }
/* KHNMC_CLOCK_SYNC=all: a waiter past 2,000 spins sleeps 50 us at a time; it used to be marked idle (2) for the quiescence test, which
 * stayed true after the owner LEFT the section: the clock could step before the waiter took it, at a host-chosen instant.  On ONE CPU
 * that decided which 16 ms handler tick created the title music's voice (review of task 28: pinned title starts gave 5 hashes in 20).
 * Now it is state 3: idle only while the section's owner field is non-zero (kh_threads_quiet reads it). */
static void cs_enter(KCtx *g,u64 cs){
    SYNC_CHAOS();
    KThread *ct=cur_thread(); u64 me=ct->tid; u64 *own=(u64*)XL(g,cs+0x10);
    if(__atomic_load_n(own,__ATOMIC_RELAXED)==me){ wr32(g,cs+0xC,rd32(g,cs+0xC)+1); return; }
    int spins=0;
    for(;;){ u64 z=0; if(__atomic_compare_exchange_n(own,&z,me,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        if(++spins<200) KH_CPU_RELAX(); else if(spins<2000) sched_yield(); else { if(spins==2000&&kh_vclock_sync){ atomic_store(&ct->vc_csown,(unsigned long long*)own); atomic_store(&ct->vc_state,3); kh_vclock_idle(2); } struct timespec ts={0,50000}; nanosleep(&ts,0); } }
    if(spins>=2000&&kh_vclock_sync) kh_vclock_idle(0);
    wr32(g,cs+0xC,1);
}
static void cs_leave(KCtx *g,u64 cs){
    u64 *own=(u64*)XL(g,cs+0x10); u32 r=rd32(g,cs+0xC);
    if(__atomic_load_n(own,__ATOMIC_RELAXED)!=cur_thread()->tid){ kh_fatal(g,"LeaveCriticalSection(%llx) by non-owner (owner=%llx me=%u)",(unsigned long long)cs,(unsigned long long)*own,cur_thread()->tid); }
    if(r>1){ wr32(g,cs+0xC,r-1); return; }
    wr32(g,cs+0xC,0); __atomic_store_n(own,0,__ATOMIC_RELEASE); vc_released();
    SYNC_CHAOS();
}
IMP(K32,InitializeCriticalSection){ u64 cs=A0(g); memset(hp(g,cs),0,0x28); SET_RAX(g,0); }
IMP(K32,InitializeCriticalSectionAndSpinCount){ u64 cs=A0(g); memset(hp(g,cs),0,0x28); SET_RAX(g,1); }
IMP(K32,InitializeCriticalSectionEx){ u64 cs=A0(g); memset(hp(g,cs),0,0x28); SET_RAX(g,1); }
IMP(K32,DeleteCriticalSection){ memset(hp(g,A0(g)),0,0x28); }
IMP(K32,EnterCriticalSection){ cs_enter(g,A0(g)); }
IMP(K32,LeaveCriticalSection){ cs_leave(g,A0(g)); }
/* msvcp140 _Mtx_*: the in-situ mutex storage is used like a CRITICAL_SECTION (recursive, so also valid for std::mutex) */
IMP(MSVCP,_Mtx_init_in_situ){ memset(hp(g,A0(g)),0,0x28); SET_RAX(g,0); }
IMP(MSVCP,_Mtx_destroy_in_situ){ memset(hp(g,A0(g)),0,0x28); }
IMP(MSVCP,_Mtx_lock){ cs_enter(g,A0(g)); SET_RAX(g,0); }
IMP(MSVCP,_Mtx_unlock){ cs_leave(g,A0(g)); SET_RAX(g,0); }
/* _Thrd_sleep(const xtime*): xtime { __time64_t sec; long nsec } is an ABSOLUTE wall-clock time (std::this_thread::sleep_until builds it
 * from _Xtime_get_ticks); sleep until then. */
IMP(MSVCP,_Thrd_sleep){ u64 xt=A0(g); struct timespec ts; ts.tv_sec=(time_t)(s64)rd64(g,xt); ts.tv_nsec=(long)(s32)rd32(g,xt+8); if(ts.tv_nsec<0) ts.tv_nsec=0; if(ts.tv_nsec>=1000000000L) ts.tv_nsec=999999999L;
    struct timespec now;
    if(kh_cut_enabled){ u64 ns=kh_cut_unix_ns(); now.tv_sec=ns/1000000000; now.tv_nsec=ns%1000000000; }
    else clock_gettime(CLOCK_REALTIME,&now);
    s64 d=((s64)ts.tv_sec-now.tv_sec)*1000000000ll+((s64)ts.tv_nsec-now.tv_nsec);
    if(kh_cut_enabled&&d>0){
        s64 bounded=d>60000000000ll?60000000000ll:d;
        if(kh_vclock_vsleep((u32)((bounded+999999)/1000000))) return;
    }
    if(d>0){ if(d>60ll*1000000000ll){ kh_warn("_Thrd_sleep: absolute target %llds in the future, clamping to 60 s",(long long)(d/1000000000ll)); ts.tv_sec=now.tv_sec+60; ts.tv_nsec=now.tv_nsec; } { struct timespec rel={(time_t)(d/1000000000ll),(long)(d%1000000000ll)}; if(d>60ll*1000000000ll){ rel.tv_sec=60; rel.tv_nsec=0; } nanosleep(&rel,0); } } }
IMP(MSVCP,_Xtime_get_ticks){ if(kh_cut_enabled){ SET_RAX(g,kh_cut_unix_ns()/100); return; } struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); SET_RAX(g,(u64)ts.tv_sec*10000000ull+(u64)ts.tv_nsec/100); }

/* ---- events / semaphores / mutexes ------------------------------------------------------------------------------------ */
static u64 mk_event(int manual,int initial){ KWaitObj *w=wobj_new(HK_EVENT); w->manual=manual; w->signaled=initial; return h_new(HK_EVENT,w); }
/* caller holds w->m.  Auto-reset: a blocked thread without a token gets one (gen++: only threads waiting now may take it), else the
 * event is set (review U35 n=4: it used to set the bit in both cases, so two SetEvents before a woken waiter ran released one thread). */
static void event_signal_locked(KWaitObj *w,int pulse){
    if(w->manual){ w->gen++; w->signaled=!pulse; }                                        /* every thread waiting now is released, whatever the state becomes next */
    else if(pulse){ if(w->pulse_tokens<w->waiters){ w->pulse_tokens++; w->gen++; } w->signaled=0; }   /* one waiting thread, if there is one */
    else if(w->pulse_tokens<w->waiters){ w->pulse_tokens++; w->gen++; }                 /* one waiting thread is released, the event stays reset */
    else w->signaled=1;                                                                   /* nobody left to release: it stays set until a wait takes it */
    pthread_cond_broadcast(&w->cv);
}
static int event_signal(u64 h,int pulse){
    int k; KWaitObj *w=h_get_ref(h,&k); if(!w) return 0; if(k!=HK_EVENT){ kh_wobj_unref(w); return 0; }
    SYNC_CHAOS();
    pthread_mutex_lock(&w->m); event_signal_locked(w,pulse); pthread_mutex_unlock(&w->m); kh_wobj_unref(w); return 1;
}
/* TASK28, KHNMC_CLOCK_SYNC=all: a guest thread that wakes another one with SetEvent or ReleaseSemaphore (not ReleaseMutex) waits
 * until the thread it released (one that was blocked on the object) has taken it and is idle again, i.e. the woken thread ran to
 * its next wait before the waker goes on, so what the waker reads of the woken thread's work afterwards does not depend on the host.
 * Also on the mixer thread (a voice callback that wakes the stream handler) and the timer threads; never on host threads. */
static _Atomic u64 vc_wake_waits, vc_wake_guard;
/* the hand-off: only when the signal released a thread that was blocked on the object, and only until THAT thread is idle again */
static void vc_after_wake(u64 h,int had_waiters,u64 sat0){
    KThread *me=cur_thread(); if(!kh_vclock_sync||!me||!had_waiters||!kh_vclock_stepped()) return;
    int k; KWaitObj *w=h_get_ref(h,&k); if(!w) return;
    u64 t0=0; struct timespec ts={0,20000}; KThread *tk=NULL;
    for(;;){
        if(atomic_load(&kh_cut_requested)){
            u64 pause=kh_host_ns();
            kh_cut_poll("wake-handoff",NULL);
            if(t0) t0+=kh_host_ns()-pause;
        }
        pthread_mutex_lock(&w->m); u64 sat=w->satisfied; tk=w->last_taker; int left=w->waiters; pthread_mutex_unlock(&w->m);
        if(sat>sat0&&tk&&tk!=me&&(tk->done||atomic_load(&tk->vc_state)==1||vc_idle_now(tk,atomic_load(&tk->vc_state)))) break;     /* taken, and the taker waits again (or ended) */
        if(sat==sat0&&left==0) break;                                                  /* nobody blocked any more and nobody took it */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); u64 hn=(u64)now.tv_sec*1000000000ull+(u64)now.tv_nsec;
        if(!t0){ t0=hn; vc_wake_waits++; } if(hn-t0>2000000000ull){ vc_wake_guard++; break; }
        nanosleep(&ts,0);
    }
    kh_wobj_unref(w);
}
static int vc_wake_state(u64 h,u64 *sat){ int wt=0; if(!kh_vclock_sync) return 0; if(kh_wobj_waitstate(h,&wt,sat)) return 0; return wt; }
u64 kh_threads_wake_waits(u64 *guard){ if(guard) *guard=vc_wake_guard; return vc_wake_waits; }
int kh_event_set(u64 h){ return event_signal(h,0); }
int kh_event_pulse(u64 h){ return event_signal(h,1); }
static int event_reset(u64 h){ int k; KWaitObj *w=h_get_ref(h,&k); if(!w) return 0; if(k!=HK_EVENT){ kh_wobj_unref(w); return 0; } pthread_mutex_lock(&w->m); w->signaled=0; pthread_mutex_unlock(&w->m); kh_wobj_unref(w); return 1; }
IMP(K32,CreateEventA){ SET_RAX(g,mk_event((int)(A1(g)&0xff),(int)(A2(g)&0xff))); }
IMP(K32,CreateEventW){ SET_RAX(g,mk_event((int)(A1(g)&0xff),(int)(A2(g)&0xff))); }
IMP(K32,CreateEventExA){ u32 fl=(u32)A2(g); SET_RAX(g,mk_event((fl&1)?1:0,(fl&2)?1:0)); }   /* CREATE_EVENT_MANUAL_RESET=1 INITIAL_SET=2 */
IMP(K32,SetEvent){ if(tl_set_hook) tl_set_hook(tl_set_ctx,A0(g)); u64 s0=0; int wt=vc_wake_state(A0(g),&s0); int ok=kh_event_set(A0(g)); if(!ok) kh_set_last_error(6); SET_RAX(g,ok); if(ok) vc_after_wake(A0(g),wt,s0); }
IMP(K32,ResetEvent){ int ok=event_reset(A0(g)); if(!ok) kh_set_last_error(6); SET_RAX(g,ok); }
/* CreateSemaphore rejects initial<0, maximum<=0, initial>maximum (ERROR_INVALID_PARAMETER).  ReleaseSemaphore fails without changing
 * the count when the release count is <=0 (87) or would exceed the maximum (ERROR_TOO_MANY_POSTS 298); the previous count is
 * written only on success. */
static u64 mk_sem(s32 init,s32 max){ if(init<0||max<=0||init>max){ kh_set_last_error(87); return 0; } KWaitObj *w=wobj_new(HK_SEM); w->count=init; w->maxcount=max; return h_new(HK_SEM,w); }
static int sem_release(u64 h,long n,u32 *prev,u32 *err){
    int k; KWaitObj *w=h_get_ref(h,&k); if(!w){ *err=6; return 0; } if(k!=HK_SEM){ kh_wobj_unref(w); *err=6; return 0; }
    SYNC_CHAOS();
    pthread_mutex_lock(&w->m); int ok=0;
    if(n<=0) *err=87; else if(w->count+n>w->maxcount) *err=298; else { *prev=(u32)w->count; w->count+=n; pthread_cond_broadcast(&w->cv); ok=1; }
    pthread_mutex_unlock(&w->m); kh_wobj_unref(w); return ok;
}
IMP(K32,CreateSemaphoreA){ SET_RAX(g,mk_sem((s32)A1(g),(s32)A2(g))); }
IMP(K32,CreateSemaphoreW){ SET_RAX(g,mk_sem((s32)A1(g),(s32)A2(g))); }
IMP(K32,ReleaseSemaphore){ if(tl_set_hook) tl_set_hook(tl_set_ctx,A0(g)); u64 s0=0; int wt=vc_wake_state(A0(g),&s0); u32 prev=0,err=0; int ok=sem_release(A0(g),(long)(s32)A1(g),&prev,&err); if(ok&&kh_vclock_on) kh_vclock_note_release(A0(g)); if(ok){ if(A2(g)) wr32(g,A2(g),prev); } else kh_set_last_error(err); SET_RAX(g,ok); if(ok) vc_after_wake(A0(g),wt,s0); }
IMP(K32,CreateMutexA){ KWaitObj *w=wobj_new(HK_MUTEX); u64 h=h_new(HK_MUTEX,w); if(A1(g)&0xff){ w->owner=cur_thread()->tid; w->recurse=1; } SET_RAX(g,h); }
IMP(K32,ReleaseMutex){ int k; KWaitObj *w=h_get_ref(A0(g),&k); if(!w||k!=HK_MUTEX){ if(w) kh_wobj_unref(w); kh_set_last_error(6); SET_RAX(g,0); return; } pthread_mutex_lock(&w->m);
    if(w->recurse==0||w->owner!=cur_thread()->tid){ pthread_mutex_unlock(&w->m); kh_wobj_unref(w); kh_set_last_error(288); SET_RAX(g,0); return; }
    if(--w->recurse==0){ w->owner=0; pthread_cond_broadcast(&w->cv); vc_released(); } pthread_mutex_unlock(&w->m); kh_wobj_unref(w); SET_RAX(g,1); }
/* KHNMC_CLOCK=frames: the virtual display must know when the game thread is parked in its frame wait and when it has taken a release */
IMP(K32,WaitForSingleObject){ u64 h=A0(g); int fw=kh_vclock_on&&kh_vclock_frame_wait_enter(h); u32 r=kh_wait(g,h,(u32)A1(g)); if(fw) kh_vclock_frame_wait_leave(r==0); SET_RAX(g,r); }
IMP(K32,WaitForSingleObjectEx){ SET_RAX(g,kh_wait(g,A0(g),(u32)A1(g))); }
IMP(K32,CloseHandle){ u64 h=A0(g); if(h==PSEUDO_THREAD||h==PSEUDO_PROCESS){ SET_RAX(g,1); return; } int ok=h_close(h); if(!ok) kh_set_last_error(6); SET_RAX(g,ok); }
static void host_sleep_ms(u32 ms){ struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; if(kh_vclock_sync) kh_vclock_idle(1); nanosleep(&ts,0); if(kh_vclock_sync) kh_vclock_idle(0); }
IMP(K32,Sleep){ if(kh_cut_enabled) cut_boot_ready(g); u32 ms=(u32)A0(g); if(ms==0) sched_yield(); else if(!kh_vclock_vsleep(ms)) host_sleep_ms(ms); }
IMP(K32,SleepEx){ u32 ms=(u32)A0(g); if(ms==0) sched_yield(); else if(!kh_vclock_vsleep(ms)) host_sleep_ms(ms); SET_RAX(g,0); }
/* task 44: with KHNMC_CLOCK=frames and its default KHNMC_CLOCK_SYNC=all the virtual clock moves only while every guest thread waits, and a
 * thread that spins on SwitchToThread never does: the movie player's sound thread (exe 0x13ec00) yields in a loop until the game thread
 * sets a flag one game frame later, and that frame needs the clock (seen: every clock step then waited out the 60 s quiescence guard).
 * So a yield is 1 ms on the virtual clock, i.e. a wait like Sleep(1), counted and reported with the clock, but ONLY while an empty
 * movie's reader exists (kh_movie_live, runtime/null_mf.c: the default mode, from MFCreateSourceReaderFromURL to the reader's last
 * Release, which covers both loops: the sound thread's and the teardown's at 0x13cfc3).  Everywhere else it is the plain yield it
 * always was: with KHNMC_MOVIES=fail (the old path calls SwitchToThread once per skipped movie, from the teardown of the player
 * that never opened), in a run without a movie (a caller through the thunk 0x471e80 is not ruled out), and with the host clock
 * (every real run), where the virtual sleep says no.  The exe has two direct call sites, both in the movie player (0x13cfc3, 0x13ece9). */
static _Atomic u64 n_yield_vsleeps;
u64 kh_yield_vsleeps(void){ return n_yield_vsleeps; }
int kh_movie_live(void);
/* the decision alone (the movies selftest calls it with a stand-in for the clock): 1 = the yield was a virtual 1 ms wait */
int kh_yield(int movie_live,int (*vsleep)(u32 ms)){ if(movie_live&&vsleep(1)){ n_yield_vsleeps++; return 1; } sched_yield(); return 0; }
IMP(K32,SwitchToThread){ kh_yield(kh_movie_live(),kh_vclock_vsleep); SET_RAX(g,1); }

/* ---- threads --------------------------------------------------------------------------------------------------------- */
static void thread_exit_notify(KThread *t){ pthread_mutex_lock(&t->m); t->done=1; pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m); vc_released(); }
/* Windows releases a thread's stack and TEB when the thread ends.  So must we: the engine starts a short-lived worker (0xfddd0)
 * every few seconds of play, and a 2 MiB stack kept for each would fill the id zone (core.c) within the hour.  After this the
 * thread's KThread stays in the list (handles, exit code), with no stack. */
static _Atomic u64 cnt_thread_stacks_released;
static void thread_release_memory(KThread *t){
    KCtx *g=t->g; u64 err=t->teb?rd64(g,t->teb+0x700):0;                 /* the thread's errno cell (crt.c) */
    pthread_mutex_lock(&tlist_m); u64 lo=t->stack_lo, hi=t->stack_hi, teb=t->teb, ta=t->tlsarr, tb=t->tlsblk; t->stack_lo=t->stack_hi=0; t->teb=t->tlsarr=t->tlsblk=0; pthread_mutex_unlock(&tlist_m);
    if(err) ga_free(err); if(tb) ga_free(tb); if(ta) ga_free(ta); if(teb) ga_free(teb);
    if(hi){ ga_pages_free_src(lo,hi-lo,GA_SRC_STACK); atomic_fetch_add(&cnt_thread_stacks_released,1); }
}
static __thread jmp_buf *tl_exit_jb;
static void *thread_main(void *arg){
    KThread *t=arg; pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);    /* never cancelled (review U35 n=2): the runtime's waits have no cancellation cleanup */
    thread_bind(t); KCtx *g=t->g;
    jmp_buf jb; tl_exit_jb=&jb;
    if(setjmp(jb)==0){
        kh_run_tls_callbacks(g,2);
        pthread_mutex_lock(&t->m); if(t->suspended) atomic_store(&t->vc_state,2); while(t->suspended) kh_cut_wait("thread-start",&t->cv,&t->m); atomic_store(&t->vc_state,0); pthread_mutex_unlock(&t->m);
        t->exit_code=(u32)kh_call(g,t->start,t->param,0,0,0);
    }
    kh_run_tls_callbacks(g,3);
    thread_exit_notify(t);
    thread_release_memory(t);
    tlist_remove(t);                   /* out of the registry of live threads (review U35 n=33); the KThread stays for its handles */
    kh_release_sigaltstack();          /* last: the 256 KiB signal stack of this host thread */
    return NULL;
}
static u64 thread_create(KCtx *g,u64 stack_size,u64 start,u64 param,int suspended,u32 *tid_out){
    KThread *t=thread_new(); t->start=start; t->param=param; t->suspended=suspended;
    u64 ss=stack_size<(2ull<<20)?(2ull<<20):stack_size; u64 top=kh_new_guest_stack(ss,&t->stack_lo); t->stack_hi=t->stack_lo+ss; kh_ctx_set_stack(t->g,top); kh_make_teb(t);
    snprintf(t->name,sizeof t->name,"thr%u",t->tid);
    KWaitObj *w=wobj_new(HK_THREAD); w->thread=t; t->handle=(u32)h_new(HK_THREAD,w);
    /* Detached: Windows frees a thread's stack and kernel object when it ends, and nothing here ever joins a guest thread.  A joinable
     * pthread that is never joined keeps its 48 MiB host stack mapping for good (review 2026-11-16: address space and VMA count grew with
     * every worker the engine ended).  t->pt is therefore only valid while !t->done (nothing uses it after creation). */
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,48ull<<20); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    int ordered_boot=kh_cut_enabled&&!suspended&&(start==GS_BASE+0x44ea80||start==GS_BASE+0x44d850||start==GS_BASE+0xfab10);
    if(ordered_boot) atomic_store(&t->cut_boot_wait,1);
    int rc=pthread_create(&t->pt,&at,thread_main,t); pthread_attr_destroy(&at);
    if(rc){ kh_warn("pthread_create failed: %d",rc); h_close(t->handle); tlist_remove(t); return 0; }
    if(ordered_boot){
        struct timespec deadline;
        kh_mono_deadline_ms(&deadline,3000);
        pthread_mutex_lock(&t->m);
        while(atomic_load(&t->cut_boot_wait)){
            if(kh_cond_wait_until(&t->cv,&t->m,&deadline)==ETIMEDOUT&&atomic_load(&t->cut_boot_wait)){
                pthread_mutex_unlock(&t->m);
                kh_fatal(g,"savestate-worker-start: worker %llx did not reach its first idle import",(unsigned long long)start);
            }
        }
        pthread_mutex_unlock(&t->m);
    }
    t->started=1; if(tid_out) *tid_out=t->tid; (void)g;
    return t->handle;
}
IMP(K32,CreateThread){ u32 tid=0; u64 h=thread_create(g,A1(g),A2(g),A3(g),(rd32(g,KH_RSP(g)+8+0x20)&4)?1:0,&tid); u64 ptid=rd64(g,KH_RSP(g)+8+0x28); if(h&&ptid) wr32(g,ptid,tid); SET_RAX(g,h); }
IMP(CRTR,_beginthreadex){ u32 tid=0; u64 h=thread_create(g,A1(g),A2(g),A3(g),(rd32(g,KH_RSP(g)+8+0x20)&4)?1:0,&tid); u64 ptid=rd64(g,KH_RSP(g)+8+0x28); if(h&&ptid) wr32(g,ptid,tid); SET_RAX(g,h); }
IMP(CRTR,_beginthread){ u64 h=thread_create(g,A1(g),A0(g),A2(g),0,NULL); SET_RAX(g,h?h:(u64)-1); }   /* _beginthread(start, stack, arg) */
/* Suspend counts as on Windows: SuspendThread +1, ResumeThread -1, both return the previous count, the thread runs at 0.
 * A thread suspending ITSELF (the only use in the game: the service thread 0xfe930 parks itself at 0xfead9 when its queue is empty
 * and is resumed at 0xfef04 / 0xfec40) blocks here until it is resumed.  Suspending another running thread cannot be done with
 * pthreads; that call fails loudly (-1, ERROR_ACCESS_DENIED) instead of pretending. */
static u32 thread_resume(u64 h){ int k; KWaitObj *w=h_get_ref(h,&k); if(!w||k!=HK_THREAD){ if(w) kh_wobj_unref(w); kh_set_last_error(6); return (u32)-1; }
    KThread *t=w->thread; pthread_mutex_lock(&t->m); int was=t->suspended; if(was>0) t->suspended=was-1;
    if(was==1){ atomic_store(&t->vc_state,0); vc_released(); }   /* runnable from here on: running for the quiescence test until it blocks, not idle until it has run (review of task 28) */
    pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m); kh_wobj_unref(w); return (u32)was; }
static u32 thread_suspend(KCtx *g,u64 h){
    KThread *me=cur_thread(), *t=me; KWaitObj *w=NULL; (void)g;
    if(h!=PSEUDO_THREAD){ int k; w=h_get_ref(h,&k); if(!w||k!=HK_THREAD){ if(w) kh_wobj_unref(w); kh_set_last_error(6); return (u32)-1; } t=w->thread; kh_wobj_unref(w); }
    if(t!=me){ atomic_fetch_add(&cnt_foreign_suspend,1); kh_warn("SuspendThread of ANOTHER thread (target %u, caller %u, handle %llx) is not implemented: the target keeps running, the call returns failure",t->tid,me->tid,(unsigned long long)h); kh_set_last_error(5); return (u32)-1; }
    atomic_fetch_add(&cnt_self_suspend,1);
    pthread_mutex_lock(&t->m); int was=t->suspended; t->suspended=was+1; atomic_store(&t->vc_state,2); kh_vclock_idle(2); while(t->suspended>0) kh_cut_wait("self-suspend",&t->cv,&t->m); atomic_store(&t->vc_state,0); pthread_mutex_unlock(&t->m);
    return (u32)was;
}
IMP(K32,ResumeThread){ SET_RAX(g,thread_resume(A0(g))); }
IMP(K32,SuspendThread){ SET_RAX(g,thread_suspend(g,A0(g))); }
IMP(K32,GetCurrentThreadId){ SET_RAX(g,cur_thread()->tid); }
IMP(K32,GetCurrentThread){ SET_RAX(g,PSEUDO_THREAD); }
IMP(K32,GetCurrentProcess){ SET_RAX(g,PSEUDO_PROCESS); }
IMP(K32,GetCurrentProcessId){ SET_RAX(g,(ga_deterministic||kh_cut_enabled)?4242u:(u32)getpid()); }
/* the MSVC SetThreadName convention (RaiseException 0x406D1388, k32.c): the guest's own thread names ("CDevSd MSECHandler", "Stream
 * Thread", ...) replace "thrN" in reports and logs (TASK28: the audio log names the thread of every call).  tid -1 = the caller. */
void kh_thread_set_name(u32 tid,const char *name){
    KThread *f=NULL;
    if(tid==0xffffffffu) f=cur_thread();
    else f=tlist_find(tid);
    if(f&&name) snprintf(f->name,sizeof f->name,"%s",name);
}
IMP(K32,OpenThread){ u32 tid=(u32)A2(g); KThread *f=tlist_find(tid);
    if(!f){ kh_set_last_error(87); SET_RAX(g,0); return; } KWaitObj *w=wobj_new(HK_THREAD); w->thread=f; SET_RAX(g,h_new(HK_THREAD,w)); }
IMP(K32,GetExitCodeThread){ u64 h=A0(g); KThread *t; if(h==PSEUDO_THREAD) t=cur_thread(); else { KWaitObj *w=h_get(h,HK_THREAD); if(!w){ SET_RAX(g,0); return; } t=w->thread; }
    wr32(g,A1(g),t->done?(u32)t->exit_code:259u); SET_RAX(g,1); }
IMP(K32,GetThreadPriority){ SET_RAX(g,0); }
IMP(K32,SetThreadPriority){ SET_RAX(g,1); }
IMP(K32,SetThreadIdealProcessor){ SET_RAX(g,0); }
IMP(K32,GetProcessAffinityMask){ long n=sysconf(_SC_NPROCESSORS_ONLN); u64 m=n>=64?~0ull:((1ull<<n)-1); if(A1(g)) wr64(g,A1(g),m); if(A2(g)) wr64(g,A2(g),m); SET_RAX(g,1); }
/* TerminateThread of another thread cannot be done safely with pthreads (review U35 n=2): pthread_cancel stopped the target inside
 * whatever runtime wait it was in, holding that wait's mutex (its own t->m in a suspend, an object's in kh_wait), with no cleanup, so
 * the exit notification, every later wait on it and its memory release could hang or never happen.  As SuspendThread of another
 * thread, the call is refused loudly: the target keeps running, the call fails with ERROR_ACCESS_DENIED, the sync report counts it.
 * The game's only caller (0x44c8f0, the input-device thread stop after a 5 s grace) ignores the result and goes on.  A thread that has
 * ended already: success, its exit code stays. */
static _Atomic u64 cnt_foreign_terminate;
static u32 thread_terminate(KCtx *g,u64 h,u32 code){
    KWaitObj *w=h_get(h,HK_THREAD); if(!w||w->thread==cur_thread()) kh_fatal(g,"TerminateThread on self or bad handle");
    KThread *t=w->thread; pthread_mutex_lock(&t->m); int live=!t->done; pthread_mutex_unlock(&t->m);
    if(!live) return 1;
    atomic_fetch_add(&cnt_foreign_terminate,1);
    kh_warn("TerminateThread of ANOTHER thread (target %u '%s' start 0x%llx, caller %u, handle %llx, exit code %u) is not implemented: the target keeps running, the call returns failure",
            t->tid,t->name,(unsigned long long)(t->start>=GS_BASE&&t->start<GS_BASE+IMG_SIZE?t->start-GS_BASE:t->start),cur_thread()->tid,(unsigned long long)h,code);
    kh_set_last_error(5); return 0;
}
IMP(K32,TerminateThread){ SET_RAX(g,thread_terminate(g,A0(g),(u32)A1(g))); }
static void thread_end(KCtx *g,u32 code){ KThread *t=cur_thread(); t->exit_code=code; if(!tl_exit_jb) kh_fatal(g,"thread exit with no exit frame"); longjmp(*tl_exit_jb,1); }
IMPN(CRTR,_endthreadex){ thread_end(g,(u32)A0(g)); }
IMPN(CRTR,_endthread){ thread_end(g,0); }
IMPN(K32,ExitProcess){ extern void kh_exit_process(int); kh_exit_process((int)A0(g)); }

/* ---- fibers ---------------------------------------------------------------------------------------------------------- */
u64 kh_fiber_host_stack=2ull<<20;            /* host stack of a fiber (lazily committed); the vkd3d backend raises it because native graphics code runs on it */
struct KFiber { KFiber *cut_next; KCtx *g; void *hstack; size_t hsize; void *sp; u64 start, param, handle, gstack_lo, gstack_sz; KThread *owner; int is_thread_fiber, finished; KhFn resume_task; };
#define FIBER_MAGIC 0x46494245ull
static int fiber_teb_swap=1;                          /* 0 only inside the selftest's demonstration of the hazard */
/* guest stacks by address, for reports (ga_describe): threads from the thread list, fibers from this small table */
#define MAXFSTK 1024
static struct { u64 lo, size, start; } fstk[MAXFSTK]; static pthread_mutex_t fstk_m=PTHREAD_MUTEX_INITIALIZER;
static void fiber_stack_note(u64 lo,u64 size,u64 start){
    pthread_mutex_lock(&fstk_m);
    for(int i=0;i<MAXFSTK;i++) if(size?!fstk[i].size:(fstk[i].size&&fstk[i].lo==lo)){ fstk[i].lo=size?lo:0; fstk[i].size=size; fstk[i].start=start; break; }
    pthread_mutex_unlock(&fstk_m);
}
int kh_describe_stack(u64 a,char *out,size_t cap){
    int hit=0;
    if(pthread_mutex_trylock(&tlist_m)==0){
        for(u32 i=0;i<ntlist&&!hit;i++){ KThread *t=tlist[i]; if(t->stack_hi&&a>=t->stack_lo&&a<t->stack_hi){ snprintf(out,cap,"stack of thread %u ('%s', start rva 0x%llx) +0x%llx of 0x%llx",t->tid,t->name,(unsigned long long)(t->start?t->start-GS_BASE:0),(unsigned long long)(a-t->stack_lo),(unsigned long long)(t->stack_hi-t->stack_lo)); hit=1; } }
        pthread_mutex_unlock(&tlist_m); }
    if(!hit&&pthread_mutex_trylock(&fstk_m)==0){
        for(int i=0;i<MAXFSTK&&!hit;i++) if(fstk[i].size&&a>=fstk[i].lo&&a<fstk[i].lo+fstk[i].size){ snprintf(out,cap,"stack of a fiber (start rva 0x%llx) +0x%llx of 0x%llx",(unsigned long long)(fstk[i].start-GS_BASE),(unsigned long long)(a-fstk[i].lo),(unsigned long long)fstk[i].size); hit=1; }
        pthread_mutex_unlock(&fstk_m); }
    return hit;
}
/* The opaque guest handle doubles as its stable key. Native bindings stay in this
 * process and inactive fibers remain enumerable after their owner switches away. */
static KFiber *cut_fibers;
static pthread_mutex_t cut_fiber_m=PTHREAD_MUTEX_INITIALIZER;
static KFiber *fiber_of(KCtx *g,u64 h){
    if(!GA_OK(h)||rd64(g,h+8)!=FIBER_MAGIC) kh_fatal(g,"bad fiber handle %llx",(unsigned long long)h);
    if(!kh_cut_enabled) return (KFiber*)rd64(g,h);
    pthread_mutex_lock(&cut_fiber_m);
    KFiber *f=cut_fibers;
    while(f&&f->handle!=h) f=f->cut_next;
    pthread_mutex_unlock(&cut_fiber_m);
    if(!f||rd64(g,h)!=h) kh_fatal(g,"savestate-fiber-binding: unknown handle %llx",(unsigned long long)h);
    return f;
}
static u64 fiber_handle(KCtx *g,KFiber *f){
    u64 h=ga_alloc(64,16);
    f->handle=h;
    if(kh_cut_enabled){
        pthread_mutex_lock(&cut_fiber_m);
        f->cut_next=cut_fibers;
        cut_fibers=f;
        pthread_mutex_unlock(&cut_fiber_m);
    }
    wr64(g,h,kh_cut_enabled?h:(u64)(uintptr_t)f);
    wr64(g,h+8,FIBER_MAGIC);
    return h;
}
static void cut_forget_fiber(KFiber *f){
    if(!kh_cut_enabled) return;
    pthread_mutex_lock(&cut_fiber_m);
    KFiber **p=&cut_fibers;
    while(*p&&*p!=f) p=&(*p)->cut_next;
    if(!*p) kh_fatal(NULL,"savestate-fiber-registry: missing live fiber");
    *p=f->cut_next;
    pthread_mutex_unlock(&cut_fiber_m);
}
/* The guest stack still holds each lifted return address. At a quiescent
 * cut, replace only a complete, recognised chain on an inactive fiber. */
void kh_resume_13552f(KCtx *),kh_resume_14f65b(KCtx *),kh_resume_13522e(KCtx *);
void kh_resume_157334(KCtx *),kh_resume_2cbc2a(KCtx *),kh_resume_2cc31a(KCtx *);
void kh_resume_1503c8(KCtx *),kh_resume_1e0351(KCtx *),kh_resume_154d8b(KCtx *);

static int cut_rebuild_fiber(KFiber *f){
    static const struct { u32 ret, frame_size; KhFn resume; } tasks[]={
        {0x157334,0x28,kh_resume_157334},{0x2cbc2a,0x78,kh_resume_2cbc2a},
        {0x2cc31a,0x38,kh_resume_2cc31a},{0x1503c8,0x28,kh_resume_1503c8},
        {0x1e0351,0x38,kh_resume_1e0351},{0x154d8b,0x98,kh_resume_154d8b},
    };
    if(f->is_thread_fiber||f->owner||f->start!=GS_BASE+0x135220) return 0;
    KCtx *g=f->g;
    u64 sp=KH_RSP(g),end=f->gstack_lo+f->gstack_sz;
    if(sp<f->gstack_lo||sp>end||end-sp<0x38||rd64(g,sp)!=GS_BASE+0x13552f) return 0;
    u64 caller=rd64(g,sp+0x30);
    for(size_t i=0;i<sizeof tasks/sizeof tasks[0];i++){
        if(caller!=GS_BASE+tasks[i].ret) continue;
        u64 outer=sp+0x38+tasks[i].frame_size;
        if(outer>end||end-outer<0x68||rd64(g,outer)!=GS_BASE+0x14f65b||
           rd64(g,outer+0x30)!=GS_BASE+0x13522e||rd64(g,outer+0x60)!=0x7ff7ffff0000ull) return 0;
        f->resume_task=tasks[i].resume;
        f->sp=make_frame((char*)f->hstack+f->hsize,f);
        return 1;
    }
    return 0;
}
void kh_cut_rebuild_fibers(void){
    unsigned rebuilt=0,total=0;
    pthread_mutex_lock(&cut_fiber_m);
    for(KFiber *f=cut_fibers;f;f=f->cut_next){
        total++;
        rebuilt+=cut_rebuild_fiber(f);
        u64 sp=KH_RSP(f->g);
        fprintf(stderr,"kh: savestate-fiber handle=%llx owner=%u start=%llx rsp=%llx return=%llx\n",
                (unsigned long long)f->handle,f->owner?f->owner->tid:0,(unsigned long long)f->start,
                (unsigned long long)sp,(unsigned long long)(GA_OK(sp)?rd64(f->g,sp):0));
    }
    fprintf(stderr,"kh: savestate-fiber-rebuild replaced=%u registered=%u\n",rebuilt,total);
    pthread_mutex_unlock(&cut_fiber_m);
}
void kh_fiber_main(KFiber *f){
    KCtx *g=f->g;
    if(f->resume_task){
        KhFn task=f->resume_task;
        f->resume_task=NULL;
        KH_RET(g);             /* complete the suspended SwitchToFiber import */
        kh_resume_13552f(g);
        task(g);
        kh_resume_14f65b(g);
        kh_resume_13522e(g);
    }else (void)kh_call(g,f->start,f->param,0,0,0);
    kh_fatal(g,"fiber start routine 0x%llx returned (Windows would ExitThread)",(unsigned long long)f->start);
}
u64 kh_fiber_create(KCtx *g,u64 stack_size,u64 start,u64 param){
    KFiber *f=calloc(1,sizeof *f); f->g=kh_new_ctx(); f->start=start; f->param=param;
    u64 gs=stack_size<(1ull<<20)?(1ull<<20):stack_size; f->gstack_sz=gs; u64 top=kh_new_guest_stack(gs,&f->gstack_lo); kh_ctx_set_stack(f->g,top); fiber_stack_note(f->gstack_lo,gs,start);
    f->hsize=kh_fiber_host_stack; f->hstack=mmap(0,f->hsize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); if(f->hstack==MAP_FAILED) kh_fatal(g,"fiber host stack mmap failed");
    atomic_fetch_add(&cnt_fiber_create,1);
    f->sp=make_frame((char*)f->hstack+f->hsize,f); return fiber_handle(g,f);
}
u64 kh_fiber_convert(KCtx *g,u64 param){
    KThread *t=cur_thread(); (void)param;
    if(t->thread_fiber){ kh_warn("ConvertThreadToFiber on a thread that is already a fiber"); return t->thread_fiber->handle; }
    KFiber *f=calloc(1,sizeof *f); f->g=t->g; f->is_thread_fiber=1; f->owner=t; t->thread_fiber=f; t->cur_fiber=f; return fiber_handle(g,f);
}
__attribute__((noinline)) void kh_fiber_switch(KCtx *g,u64 target){
    KThread *t=cur_thread(); KFiber *to=fiber_of(g,target); KFiber *from=t->cur_fiber;
    if(!from) kh_fatal(g,"SwitchToFiber from a thread that was not converted to a fiber");
    if(to==from) return;
    SYNC_CHAOS();
    u64 gs=KH_GS(from->g); (void)gs; KH_GS(to->g)=t->teb; to->g->rt=t; to->owner=t; t->cur_fiber=to; from->owner=NULL;
    /* Windows SwitchToFiber swaps the TEB's StackBase / StackLimit to the fiber's stack.  The exe's __chkstk (0x43a460) probes from
     * gs:[0x10] downwards one page at a time (a zero byte per page) whenever the new stack pointer is below that limit: with the
     * THREAD's limit left in the TEB, a fiber whose stack lies below the thread's stack would have every page in between written. */
    atomic_fetch_add(&cnt_fiber_switch,1);
    if(fiber_teb_swap){
        u64 lo=to->is_thread_fiber?t->stack_lo:to->gstack_lo, hi=to->is_thread_fiber?t->stack_hi:to->gstack_lo+to->gstack_sz;
        if(!to->is_thread_fiber && hi<=t->stack_lo) atomic_fetch_add(&cnt_fiber_below,1);
        wr64(g,t->teb+0x08,hi); wr64(g,t->teb+0x10,lo);
    }
    kh_set_cur_g(to->g);
    kh_ctx_switch(&from->sp,to->sp);
    /* resumed here, possibly on another host thread: whoever switched to us already set cur_fiber and gs */
    kh_set_cur_g(from->g);
}
void kh_fiber_delete(KCtx *g,u64 h){
    KFiber *f=fiber_of(g,h); KThread *t=cur_thread();
    if(f==t->cur_fiber && !f->is_thread_fiber) kh_fatal(g,"DeleteFiber of the running fiber (ExitThread semantics) not supported");
    if(f->is_thread_fiber){ kh_warn("DeleteFiber on a thread fiber"); return; }
    cut_forget_fiber(f);
    if(f->hstack) munmap(f->hstack,f->hsize);
    fiber_stack_note(f->gstack_lo,0,0); ga_pages_free_src(f->gstack_lo,f->gstack_sz,GA_SRC_STACK); wr64(g,h+8,0); ga_free(h); free(f->g); free(f);
}
int kh_fiber_convert_to_thread(KCtx *g){
    KThread *t=cur_thread(); KFiber *f=t->thread_fiber; if(!f) return 0; if(t->cur_fiber!=f){ kh_fatal(g,"ConvertFiberToThread while a non-thread fiber is running"); }
    wr64(g,t->teb+0x08,t->stack_hi); wr64(g,t->teb+0x10,t->stack_lo);
    cut_forget_fiber(f);
    wr64(g,f->handle+8,0); ga_free(f->handle); free(f); t->thread_fiber=NULL; t->cur_fiber=NULL; return 1;
}
IMP(K32,ConvertThreadToFiber){ SET_RAX(g,kh_fiber_convert(g,A0(g))); }
IMP(K32,ConvertFiberToThread){ SET_RAX(g,kh_fiber_convert_to_thread(g)); }
IMP(K32,CreateFiber){ SET_RAX(g,kh_fiber_create(g,A0(g),A1(g),A2(g))); }
IMP(K32,DeleteFiber){ kh_fiber_delete(g,A0(g)); }
IMPN(K32,SwitchToFiber){ kh_fiber_switch(g,A0(g)); KH_RET(g); }

/* hand-written replacements of the two fiber yield leaves (hooks.tsv): exact copies of the original instruction sequences,
 * then a tail call into the lifted 0x135470 (switch helper) with its guest ABI. */
void f_135470(KCtx *g);
void rt_hook_fiber_yield_150060(KCtx *g){
    u64 rcx=KH_RCX(g); u64 rax=rd64(g,rcx+0x70); wr32(g,rax+0x1c,(u32)KH_RDX(g)); KH_RAX(g)=rax;
    u64 rdx=rd64(g,rcx+0x58); KH_RCX(g)=rd64(g,rcx+0x70); KH_RDX(g)=rd64(g,rdx+0x28); KH_RCX(g)=rd64(g,KH_RCX(g));
    f_135470(g);
}
void rt_hook_fiber_yield_150080(KCtx *g){
    u64 rcx=KH_RCX(g); u64 rax=rd64(g,rcx+0x70); KH_RAX(g)=rax;
    float c; u32 cu=rd32(g,GS_BASE+0x717480); memcpy(&c,&cu,4); float x; memcpy(&x,&KH_XMM(g,1),4); x=x-c; memcpy(&KH_XMM(g,1),&x,4);
    wr32(g,rax+0x20,*(u32*)&x);
    u64 rdx=rd64(g,rcx+0x58); KH_RCX(g)=rd64(g,rcx+0x70); KH_RDX(g)=rd64(g,rdx+0x28); KH_RCX(g)=rd64(g,KH_RCX(g));
    f_135470(g);
}

/* a detached guest thread whose body is a host function (used for winmm timers and the like) */
u64 kh_spawn_hostfn_thread(KCtx *g,const char *name,KhFn fn,u64 param){ u64 a=kh_host_fn(name,fn); u32 tid=0; u64 h=thread_create(g,0,a,param,1,&tid); if(h){ kh_thread_set_name(tid,name); thread_resume(h); h_close(h); } return h; }

u64 kh_mk_event_h(int manual,int initial){ return mk_event(manual,initial); }

void kh_dump_threads(FILE *f){
    /* also called from the fault paths: never block there.  Live threads only (an ended thread leaves the list); a KThread is never
     * freed, so a walk without the lock reads valid threads, at worst one twice or one missed while a thread ends. */
    int locked=(pthread_mutex_trylock(&tlist_m)==0);
    for(u32 i=0;i<ntlist;i++){ KThread *t=tlist[i]; fprintf(f,"kh: thread %u '%s' done=%d cur_fiber=%p start=%llx\n",t->tid,t->name,t->done,(void*)t->cur_fiber,(unsigned long long)(t->start>=GS_BASE&&t->start<GS_BASE+IMG_SIZE?t->start-GS_BASE:t->start));
        if(t->done&&!t->stack_hi) continue;                                 /* ended: its stack went back to the arena */
        kh_guest_bt(t->cur_fiber&&!t->cur_fiber->is_thread_fiber?t->cur_fiber->g:t->g,f); }
    if(locked) pthread_mutex_unlock(&tlist_m);
}

/* task 29: threads created / ended so far (the map reports of runtime/route_state.c); never blocks */
void kh_threads_counts(u64 *created,u64 *ended,u64 *released){
    int locked=(pthread_mutex_trylock(&tlist_m)==0);
    *created=tlist_created; *ended=tlist_ended;
    if(locked) pthread_mutex_unlock(&tlist_m);
    *released=atomic_load(&cnt_thread_stacks_released);
}
void kh_sync_report(FILE *f){
    int locked=(pthread_mutex_trylock(&tlist_m)==0); u64 cr=tlist_created, en=tlist_ended; u32 live=ntlist; if(locked) pthread_mutex_unlock(&tlist_m);
    fprintf(f,"kh: threads: %llu created, stacks of %llu ended threads released; %llu ended, %u alive\n",(unsigned long long)cr,(unsigned long long)cnt_thread_stacks_released,(unsigned long long)en,live);
    fprintf(f,"kh: sync report: self-suspends=%llu refused-suspends-of-other-threads=%llu refused-terminates-of-other-threads=%llu fibers-created=%llu fiber-switches=%llu switches-to-a-fiber-below-the-thread-stack=%llu sync-chaos=%u hits=%llu\n",
            (unsigned long long)cnt_self_suspend,(unsigned long long)cnt_foreign_suspend,(unsigned long long)cnt_foreign_terminate,(unsigned long long)cnt_fiber_create,(unsigned long long)cnt_fiber_switch,(unsigned long long)cnt_fiber_below,
            sync_chaos,(unsigned long long)sync_chaos_hits);
}

/* ==============================================================================================================================
 * selftest of the sync primitives against the Win32 behaviour listed above the wait objects
 * ============================================================================================================================ */
#define SCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"sync selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
typedef struct { u64 h; u32 ms; _Atomic u32 res; _Atomic int done; _Atomic u64 dt_ms; } StW;
static void st_msleep(int ms){ struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); }
static void st_waiter(KCtx *g){ StW *a=(StW*)(uintptr_t)A0(g); u64 t0=kh_now_ns(); u32 r=kh_wait(g,a->h,a->ms); atomic_store(&a->dt_ms,(kh_now_ns()-t0)/1000000); atomic_store(&a->res,r); atomic_store(&a->done,1); KH_RET(g); }
static void st_spawn_waiters(KCtx *g,StW *a,int n,u64 h,u32 ms){ for(int i=0;i<n;i++){ a[i].h=h; a[i].ms=ms; atomic_store(&a[i].done,0); atomic_store(&a[i].res,0xdeadu); kh_spawn_hostfn_thread(g,"sync_selftest_waiter",st_waiter,(u64)(uintptr_t)&a[i]); } }
static int st_blocked(u64 h,int n,int ms){ KWaitObj *w=h_get(h,0); for(int i=0;i<ms*5;i++){ pthread_mutex_lock(&w->m); int c=w->waiters; pthread_mutex_unlock(&w->m); if(c>=n) return 1; struct timespec ts={0,200000}; nanosleep(&ts,0); } return 0; }
static int st_all_done(StW *a,int n,int ms){ for(int i=0;i<ms*5;i++){ int d=0; for(int k=0;k<n;k++) d+=atomic_load(&a[k].done); if(d==n) return 1; struct timespec ts={0,200000}; nanosleep(&ts,0); } return 0; }
static int st_count(StW *a,int n,u32 res){ int c=0; for(int k=0;k<n;k++) c+=(atomic_load(&a[k].res)==res); return c; }
static void st_sleeper(KCtx *g){ st_msleep((int)A0(g)); SET_RAX(g,7); KH_RET(g); }
static u32 st_terminate(KCtx *g,u64 h,u32 code){ u64 c=KH_RCX(g),d=KH_RDX(g); KH_RCX(g)=h; KH_RDX(g)=code; i_kernel32__TerminateThread(g); KH_RCX(g)=c; KH_RDX(g)=d; return (u32)KH_RAX(g); }   /* the import's body */
static struct { _Atomic int stage; _Atomic u32 prev; } st_susp;
static void st_suspender(KCtx *g){ atomic_store(&st_susp.stage,1); u32 p=thread_suspend(g,PSEUDO_THREAD); atomic_store(&st_susp.prev,p); atomic_store(&st_susp.stage,2); KH_RET(g); }
static struct { _Atomic int n; _Atomic u64 id, user; } st_tm;
static void st_timer_cb(KCtx *g){ atomic_store(&st_tm.id,A0(g)); atomic_store(&st_tm.user,A2(g)); atomic_fetch_add(&st_tm.n,1); KH_RET(g); }
static u64 st_import(const char *name){ extern u64 kh_import_addr_base; for(u32 i=0;i<kh_import_count;i++) if(!strcmp(kh_import_table[i].name,name)) return kh_import_addr_base+16ull*i; return 0; }
/* fiber part: the lifted __chkstk (0x43a460; frame size in rax) run inside a fiber, once with the TEB as the switch left it and once
 * with the limit a thread stack above the fiber would have left there (what the runtime did before) */
void f_43a460(KCtx *g);
static struct { u64 main_h, canary, canary_sz; u64 seen_lo, seen_hi; int probed_fixed, probed_legacy, pages_between; } st_fb;
static int st_count_probed(KCtx *g){ int c=0; for(u64 a=st_fb.canary;a<st_fb.canary+st_fb.canary_sz;a+=4096) c+=(rd8(g,a)!=0xAA); return c; }
static void st_chkstk(KCtx *g,u64 size){ u64 save=KH_RSP(g), sv_rax=KH_RAX(g); u64 sp=((save-0x100)&~0xFull)-8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RAX(g)=size; f_43a460(g); KH_RSP(g)=save; KH_RAX(g)=sv_rax; }
static void st_fiber_body(KCtx *g){
    KThread *t=cur_thread(); u64 teb=t->teb;
    st_fb.seen_hi=rd64(g,teb+0x08); st_fb.seen_lo=rd64(g,teb+0x10);
    st_chkstk(g,0x3000); st_fb.probed_fixed=st_count_probed(g);
    /* the hazard, contained: limit = top of the canary, as if the running thread's stack began right above it.  Every page start
     * between this fiber's stack pointer and that limit is saved and restored. */
    u64 lim=st_fb.canary+st_fb.canary_sz, lowpg=(KH_RSP(g)-0x4000)&~0xfffull; int n=(int)((lim-lowpg)/4096); u8 *sv=malloc((size_t)n);
    for(int i=0;i<n;i++) sv[i]=rd8(g,lowpg+4096ull*(u64)i);
    wr64(g,teb+0x10,lim); st_chkstk(g,0x3000); wr64(g,teb+0x10,st_fb.seen_lo);
    st_fb.probed_legacy=st_count_probed(g); st_fb.pages_between=n;
    for(int i=0;i<n;i++) wr8(g,lowpg+4096ull*(u64)i,sv[i]); free(sv);
    kh_fiber_switch(g,st_fb.main_h);
    kh_fatal(g,"sync selftest: fiber resumed after its last switch");
}
int kh_sync_selftest(KCtx *g){
    int checks=0,fails=0; StW a[3];
    /* 1. manual-reset event: SetEvent immediately followed by ResetEvent releases every thread that was waiting */
    u64 ev=mk_event(1,0); st_spawn_waiters(g,a,3,ev,5000); SCHECK(st_blocked(ev,3,3000));
    kh_event_set(ev); event_reset(ev);
    SCHECK(st_all_done(a,3,3000)); SCHECK(st_count(a,3,0)==3); SCHECK(kh_wait(g,ev,30)==0x102);
    kh_event_set(ev); SCHECK(kh_wait(g,ev,0)==0 && kh_wait(g,ev,0)==0);                       /* stays set */
    event_reset(ev);
    /* 2. auto-reset event: one waiter per SetEvent; with nobody waiting it stays set for exactly one wait */
    u64 ae=mk_event(0,0); st_spawn_waiters(g,a,3,ae,400); SCHECK(st_blocked(ae,3,3000));
    kh_event_set(ae); SCHECK(st_all_done(a,3,3000)); SCHECK(st_count(a,3,0)==1 && st_count(a,3,0x102)==2);
    kh_event_set(ae); SCHECK(kh_wait(g,ae,0)==0); SCHECK(kh_wait(g,ae,0)==0x102);
    /* 2b. review U35 n=4: two SetEvents before either of two blocked waiters has run release BOTH (one blocked waiter per SetEvent, as
     *     on Windows; the shared bit merged them into one release); a third finds nobody left to release and leaves the event set.
     *     Forced: the signals are made under the event's lock, so no woken waiter can run in between. */
    st_spawn_waiters(g,a,2,ae,600); SCHECK(st_blocked(ae,2,3000));
    { KWaitObj *w=h_get(ae,HK_EVENT); pthread_mutex_lock(&w->m); event_signal_locked(w,0); event_signal_locked(w,0); pthread_mutex_unlock(&w->m); }
    SCHECK(st_all_done(a,2,3000)); SCHECK(st_count(a,2,0)==2); SCHECK(kh_wait(g,ae,0)==0x102);
    st_spawn_waiters(g,a,2,ae,600); SCHECK(st_blocked(ae,2,3000));
    { KWaitObj *w=h_get(ae,HK_EVENT); pthread_mutex_lock(&w->m); for(int i=0;i<3;i++) event_signal_locked(w,0); pthread_mutex_unlock(&w->m); }
    SCHECK(st_all_done(a,2,3000)); SCHECK(st_count(a,2,0)==2); SCHECK(kh_wait(g,ae,0)==0); SCHECK(kh_wait(g,ae,0)==0x102);
    /*     the release belongs to a thread blocked when it was granted: one that begins to wait after the SetEvent cannot take it */
    { KWaitObj *w=h_get(ae,HK_EVENT); KThread *me=cur_thread(); pthread_mutex_lock(&w->m);
      u64 gA=w->gen; w->waiters++; event_signal_locked(w,0); u64 gN=w->gen; w->waiters++;                  /* A blocked, SetEvent, then N blocks */
      SCHECK(w->signaled==0 && w->pulse_tokens==1); SCHECK(!wobj_try(w,HK_EVENT,me,gN,1)); SCHECK(wobj_try(w,HK_EVENT,me,gA,1) && w->pulse_tokens==0 && w->signaled==0);
      w->waiters-=2; pthread_mutex_unlock(&w->m); }
    /* 3. pulses: auto = one current waiter, manual = all current waiters, nobody waiting = lost */
    st_spawn_waiters(g,a,2,ae,400); SCHECK(st_blocked(ae,2,3000)); kh_event_pulse(ae);
    SCHECK(st_all_done(a,2,3000)); SCHECK(st_count(a,2,0)==1 && st_count(a,2,0x102)==1);
    kh_event_pulse(ae); SCHECK(kh_wait(g,ae,20)==0x102);
    st_spawn_waiters(g,a,3,ev,5000); SCHECK(st_blocked(ev,3,3000)); kh_event_pulse(ev);
    SCHECK(st_all_done(a,3,3000)); SCHECK(st_count(a,3,0)==3); SCHECK(kh_wait(g,ev,0)==0x102);
    /* 4. the GX frame timer: timeSetEvent(period, TIME_PERIODIC|TIME_CALLBACK_EVENT_PULSE) on an auto-reset event, waited with a 1 s timeout */
    u64 tse=st_import("timeSetEvent"), tke=st_import("timeKillEvent"); SCHECK(tse && tke);
    if(tse&&tke){
        u64 te=mk_event(0,0); u32 id=(u32)kh_call5(g,tse,10,1,te,0,0x21); SCHECK(id!=0);
        u64 t0=kh_now_ns(); int ok=0; for(int i=0;i<20;i++) ok+=(kh_wait(g,te,1000)==0); u64 dt=(kh_now_ns()-t0)/1000000;
        SCHECK(ok==20); SCHECK(dt>=150 && dt<1500);                                            /* 20 periods of 10 ms */
        SCHECK((u32)kh_call(g,tke,id,0,0,0)==0); h_close(te);
        atomic_store(&st_tm.n,0); u32 id2=(u32)kh_call5(g,tse,5,1,kh_host_fn("sync_selftest_timer_cb",st_timer_cb),0x1234,1);
        st_msleep(120); SCHECK((u32)kh_call(g,tke,id2,0,0,0)==0); st_msleep(20); int n1=atomic_load(&st_tm.n); st_msleep(40);
        SCHECK(n1>=10 && n1<=27 && atomic_load(&st_tm.n)==n1 && atomic_load(&st_tm.id)==id2 && atomic_load(&st_tm.user)==0x1234);
    }
    /* 5. a thread handle is signalled when the thread ends (infinite and finite waits return then) */
    u64 sl=kh_host_fn("sync_selftest_sleeper",st_sleeper);
    /* the stopwatch starts BEFORE the thread exists: the thread's 120 ms sleep begins after t0, so dt>=120 holds even when this thread
     * is descheduled right after the create (first run on the iPhone, 2026-11-18: the app started while the phone was unlocking and
     * one of these two checks failed; the values are printed now when they do) */
    u64 t0=kh_now_ns(); u64 th=thread_create(g,0,sl,120,0,NULL); u32 r=kh_wait(g,th,0xFFFFFFFFu); u64 dt=(kh_now_ns()-t0)/1000000;
    if(!(th && r==0 && dt>=100 && dt<3000)) fprintf(stderr,"kh: sync selftest: thread-handle wait (infinite): handle %llx result %#x after %llu ms (expected 0 after 100..3000 ms)\n",(unsigned long long)th,r,(unsigned long long)dt);
    SCHECK(th && r==0 && dt>=100 && dt<3000); { KWaitObj *w=h_get(th,HK_THREAD); SCHECK(w && w->thread->done && w->thread->exit_code==7); } SCHECK(kh_wait(g,th,0)==0); h_close(th);
    t0=kh_now_ns(); th=thread_create(g,0,sl,120,0,NULL); r=kh_wait(g,th,8000); dt=(kh_now_ns()-t0)/1000000;
    if(!(r==0 && dt>=100 && dt<3000)) fprintf(stderr,"kh: sync selftest: thread-handle wait (8000 ms): handle %llx result %#x after %llu ms (expected 0 after 100..3000 ms)\n",(unsigned long long)th,r,(unsigned long long)dt);
    SCHECK(r==0 && dt>=100 && dt<3000);
    SCHECK(kh_wait(g,th,0)==0); h_close(th); SCHECK(kh_wait(g,th,0)==0xFFFFFFFFu);
    th=thread_create(g,0,sl,300,0,NULL); SCHECK(kh_wait(g,th,20)==0x102);                     /* still running */
    /* 6. suspending another thread is refused loudly; a self-suspend blocks until ResumeThread, counts as on Windows */
    SCHECK(thread_suspend(g,th)==(u32)-1 && kh_get_last_error()==5); SCHECK(kh_wait(g,th,5000)==0); h_close(th);
    atomic_store(&st_susp.stage,0); u64 sh=thread_create(g,0,kh_host_fn("sync_selftest_suspender",st_suspender),0,0,NULL); KWaitObj *sw=h_get(sh,HK_THREAD); int parked=0;
    for(int i=0;i<15000&&!parked;i++){ pthread_mutex_lock(&sw->thread->m); parked=sw->thread->suspended>0; pthread_mutex_unlock(&sw->thread->m); if(!parked){ struct timespec ts={0,200000}; nanosleep(&ts,0); } }
    SCHECK(parked); st_msleep(60); SCHECK(atomic_load(&st_susp.stage)==1);                   /* it really is blocked */
    SCHECK(thread_resume(sh)==1); SCHECK(kh_wait(g,sh,5000)==0); SCHECK(atomic_load(&st_susp.stage)==2 && atomic_load(&st_susp.prev)==0);
    SCHECK(thread_resume(sh)==0); h_close(sh);
    /* a thread created suspended starts at the first resume */
    th=thread_create(g,0,sl,10,1,NULL); SCHECK(kh_wait(g,th,80)==0x102); SCHECK(thread_resume(th)==1); SCHECK(kh_wait(g,th,5000)==0); h_close(th);
    /* 6b. review U35 n=2: TerminateThread of another live thread is refused (ERROR_ACCESS_DENIED, counted).  It used to pthread_cancel the
     *     target, which then ended inside whatever wait it was in, holding that wait's mutex, without its own exit code and memory
     *     release.  The target runs on and ends by itself; on an ended thread the call succeeds and changes nothing. */
    { u64 c0=atomic_load(&cnt_foreign_terminate); th=thread_create(g,0,sl,150,0,NULL); SCHECK(th && kh_wait(g,th,20)==0x102);
      kh_set_last_error(0); int refused=st_terminate(g,th,0x55)==0 && kh_get_last_error()==5; SCHECK(refused && atomic_load(&cnt_foreign_terminate)==c0+1);
      SCHECK(kh_wait(g,th,5000)==0); KWaitObj *tw=h_get(th,HK_THREAD); SCHECK(tw && tw->thread->exit_code==7);              /* its own end, its own exit code */
      SCHECK(st_terminate(g,th,0x66)==1 && tw && tw->thread->exit_code==7); h_close(th);
      if(refused){                                                         /* a target blocked in a runtime wait: refused, and that wait completes normally */
          u64 te=mk_event(0,0); a[0].h=te; a[0].ms=5000; atomic_store(&a[0].done,0); atomic_store(&a[0].res,0xdeadu);
          th=thread_create(g,0,kh_host_fn("sync_selftest_waiter",st_waiter),(u64)(uintptr_t)&a[0],0,NULL); SCHECK(th && st_blocked(te,1,3000));
          kh_set_last_error(0); SCHECK(st_terminate(g,th,0x55)==0 && kh_get_last_error()==5);
          kh_event_set(te); SCHECK(st_all_done(a,1,3000) && atomic_load(&a[0].res)==0); SCHECK(kh_wait(g,th,5000)==0); h_close(th); h_close(te); } }
    /* 6c. review U35 n=33: an ended thread leaves the registry of live threads (it kept every thread ever created and silently stopped
     *     registering at 4096: OpenThread and the quiescence test then missed new threads); a live one is found by its id; a full
     *     registry refuses instead of skipping (thread_new turns that into the named fatal thread-registry-full) */
    { pthread_mutex_lock(&tlist_m); u64 c0=tlist_created, e0=tlist_ended; pthread_mutex_unlock(&tlist_m);
      KThread *mine[8]; int nm=0;
      for(int i=0;i<6;i++){ u64 h=thread_create(g,0,sl,1,0,NULL); KWaitObj *w=h?h_get(h,HK_THREAD):NULL; SCHECK(w!=NULL); if(w) mine[nm++]=w->thread; SCHECK(h && kh_wait(g,h,5000)==0); if(h) h_close(h); }
      u32 tid=0; u64 h=thread_create(g,0,sl,200,0,&tid); KWaitObj *w=h?h_get(h,HK_THREAD):NULL; SCHECK(w && tid); if(w) mine[nm++]=w->thread;
      KThread *lt=tid?tlist_find(tid):NULL; SCHECK(lt && w && lt==w->thread && !lt->done);                              /* alive: found by its id */
      SCHECK(h && kh_wait(g,h,5000)==0); if(h) h_close(h);
      int gone=0; for(int i=0;i<15000&&!gone;i++){ gone=1; pthread_mutex_lock(&tlist_m); for(int j=0;j<nm;j++) for(u32 k=0;k<ntlist;k++) if(tlist[k]==mine[j]) gone=0; pthread_mutex_unlock(&tlist_m);
          if(!gone){ struct timespec ts={0,200000}; nanosleep(&ts,0); } }
      SCHECK(gone && nm==7); SCHECK(tid && tlist_find(tid)==NULL);                                                      /* ended: out of the registry */
      u64 c1=0,e1=0,r1=0; kh_threads_counts(&c1,&e1,&r1); SCHECK(c1>=c0+7 && e1>=e0+7);                                 /* lifetime counts go on */
      static KThread dummy; pthread_mutex_lock(&tlist_m); u32 cap0=tlist_cap, nb=ntlist; tlist_cap=ntlist; int added=tlist_add_locked(&dummy); tlist_cap=cap0; int same=ntlist==nb; pthread_mutex_unlock(&tlist_m);
      SCHECK(!added && same); }
    /* 7. "SetEvent then CloseHandle" while a thread waits: the wait is satisfied and the object is freed only after the waiter left */
    { extern long kh_wobj_frees(void); u64 ce=mk_event(1,0); st_spawn_waiters(g,a,1,ce,5000); SCHECK(st_blocked(ce,1,3000)); long f0=kh_wobj_frees();
      kh_event_set(ce); SCHECK(h_close(ce)==1); SCHECK(st_all_done(a,1,3000)); SCHECK(atomic_load(&a[0].res)==0);
      int freed=0; for(int i=0;i<5000&&!freed;i++){ freed=(kh_wobj_frees()>f0); if(!freed) st_msleep(1); } SCHECK(freed); SCHECK(kh_wait(g,ce,0)==0xFFFFFFFFu && !kh_event_set(ce)); }
    /* 8. semaphore counts */
    { u64 sm=mk_sem(0,1); u32 prev=9,err=0; SCHECK(sm && sem_release(sm,1,&prev,&err)==1 && prev==0);
      prev=9; SCHECK(sem_release(sm,1,&prev,&err)==0 && err==298 && prev==9); SCHECK(sem_release(sm,0,&prev,&err)==0 && err==87); SCHECK(sem_release(sm,-1,&prev,&err)==0 && err==87);
      SCHECK(kh_wait(g,sm,0)==0); SCHECK(kh_wait(g,sm,0)==0x102); SCHECK(sem_release(ev,1,&prev,&err)==0 && err==6); h_close(sm);
      SCHECK(mk_sem(2,1)==0 && kh_get_last_error()==87); SCHECK(mk_sem(-1,1)==0); SCHECK(mk_sem(0,0)==0);
      u64 s3=mk_sem(1,3); prev=9; SCHECK(sem_release(s3,2,&prev,&err)==1 && prev==1); SCHECK(kh_wait(g,s3,0)==0 && kh_wait(g,s3,0)==0 && kh_wait(g,s3,0)==0 && kh_wait(g,s3,0)==0x102); h_close(s3); }
    /* 9. a wait on something that is not a waitable object fails instead of touching it */
    SCHECK(kh_wait(g,0x100+4ull*60000,0)==0xFFFFFFFFu && kh_get_last_error()==6);
    /* 10. fibers: the TEB stack bounds follow the running fiber, and the lifted __chkstk leaves memory outside the fiber's stack alone */
    { u64 ga_bump_ptr(void); KThread *t=cur_thread(); static u64 drained[4096]; int nd=0;
      for(;nd<4096;nd++){ u64 b0=ga_bump_ptr(); drained[nd]=ga_pages(1ull<<20); if(ga_bump_ptr()!=b0){ nd++; break; } }     /* use up every reusable run of 1 MiB or more: the next 1 MiB runs are consecutive */
      u64 fib=kh_fiber_create(g,1ull<<20,kh_host_fn("sync_selftest_fiber",st_fiber_body),0); KFiber *F=fiber_of(g,fib);
      st_fb.canary_sz=1ull<<20; st_fb.canary=ga_pages(st_fb.canary_sz); memset(hp(g,st_fb.canary),0xAA,st_fb.canary_sz);
      u64 ftop=F->gstack_lo+F->gstack_sz; SCHECK(st_fb.canary>=ftop && st_fb.canary-ftop<=0x10000);                           /* layout the test needs */
      if(st_fb.canary>=ftop && st_fb.canary-ftop<=0x10000){
          SCHECK(rd64(g,t->teb+0x10)==t->stack_lo && rd64(g,t->teb+0x08)==t->stack_hi);
          st_fb.main_h=kh_fiber_convert(g,0); st_fb.probed_fixed=st_fb.probed_legacy=-1; kh_fiber_switch(g,fib);
          SCHECK(st_fb.seen_lo==F->gstack_lo && st_fb.seen_hi==ftop);                           /* inside the fiber: the fiber's bounds */
          SCHECK(st_fb.probed_fixed==0);                                                         /* nothing outside the fiber's stack was written */
          SCHECK(st_fb.probed_legacy==256);                                                      /* with the thread's limit every canary page got the probe byte: the check has teeth */
          SCHECK(st_count_probed(g)==0 && st_fb.pages_between>=256 && st_fb.pages_between<300);  /* and the demonstration cleaned up after itself */
          SCHECK(rd64(g,t->teb+0x10)==t->stack_lo && rd64(g,t->teb+0x08)==t->stack_hi);         /* back on the thread: the thread's bounds */
          kh_fiber_delete(g,fib); SCHECK(kh_fiber_convert_to_thread(g)==1);
      }
      ga_pages_free(st_fb.canary,st_fb.canary_sz); for(int i=0;i<nd;i++) if(drained[i]) ga_pages_free(drained[i],1ull<<20); }
    h_close(ev); h_close(ae);
    fprintf(stderr,"kh: sync selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

/* ==============================================================================================================================
 * review of task 28: the virtual time-outs of KHNMC_CLOCK_SYNC=all (kh_wait -> kh_vclock_deadline_due -> kh_threads_quiet).  The due
 * test used to run with the waited object's lock held, while the quiescence test takes the object lock of every other blocked thread:
 *   A. two threads blocked on ONE event, one with a 5 ms time-out: the timed waiter locked the event's (non-recursive) mutex a second
 *      time and hung with tlist_m held; the clock then died after 30 s with "vclock-client-stuck: clock client 'timed-wait'";
 *   B. the same on one THREAD handle (the lock is the thread's own m);
 *   C. two timed waiters with equal keys (same name, same start) on two events, due at the same instant: both run the quiescence test,
 *      each holding its own event's lock and taking the other's (opposite orders).  A race: repeated, not forced.
 * Run on a stepped test clock with sync=all; the quiescence test is limited to this test's threads (threads of earlier suites live on).
 * A watchdog turns a hang into the named fatal sync-vclock-selftest-deadlock after 5 s of host time.  Without the fix A hangs at once. */
typedef struct { u64 h; u32 ms; KThread *_Atomic t; _Atomic u32 res; _Atomic int done; _Atomic u64 at; } StVW;
static void st_vwaiter(KCtx *g){ StVW *a=(StVW*)(uintptr_t)A0(g); atomic_store(&a->t,cur_thread()); u32 r=kh_wait(g,a->h,a->ms); atomic_store(&a->at,kh_vclock_now_ns()); atomic_store(&a->res,r); atomic_store(&a->done,1); KH_RET(g); }
static void vst_start(StVW *a,u64 h,u32 ms){ a->h=h; a->ms=ms; atomic_store(&a->t,NULL); atomic_store(&a->res,0xdeadu); atomic_store(&a->done,0); atomic_store(&a->at,0); }
static int vst_blocked(StVW *a,int ms){ for(int i=0;i<ms*5;i++){ KThread *t=atomic_load(&a->t); if(t&&atomic_load(&t->vc_state)==1&&atomic_load(&t->vc_w)) return 1; struct timespec ts={0,200000}; nanosleep(&ts,0); } return 0; }
static int vst_done(StVW *a,int ms){ for(int i=0;i<ms*5&&!atomic_load(&a->done);i++){ struct timespec ts={0,200000}; nanosleep(&ts,0); } return atomic_load(&a->done); }
static KThread *vst_set[8];
static void vst_filter(int n){ pthread_mutex_lock(&tlist_m); quiet_nonly=(u32)n; quiet_only=n?vst_set:NULL; pthread_mutex_unlock(&tlist_m); }
static const char *_Atomic vst_what; static _Atomic int vst_stop;
static void *vst_watchdog(void *arg){
    (void)arg; const char *last=NULL; u64 t0=kh_host_ns();
    while(!atomic_load(&vst_stop)){
        const char *w=atomic_load(&vst_what); if(w!=last){ last=w; t0=kh_host_ns(); }
        if(w&&kh_host_ns()-t0>5000000000ull){ fprintf(stderr,"sync vclock selftest FAIL: %s did not finish within 5 s of host time\n",w);
            kh_fatal(NULL,"sync-vclock-selftest-deadlock: %s: a thread in the virtual time-out path of kh_wait never returned (lock-order deadlock between the object lock and the quiescence test)",w); }
        struct timespec ts={0,10000000}; nanosleep(&ts,0);
    }
    return NULL;
}
typedef struct { u64 cs; KThread *_Atomic t; _Atomic int got, go; } StCs;
static void st_cswaiter(KCtx *g){ StCs *c=(StCs*)(uintptr_t)A0(g); atomic_store(&c->t,cur_thread()); cs_enter(g,c->cs); atomic_store(&c->got,1);
    while(!atomic_load(&c->go)) sched_yield();                                                       /* holds the section, running */
    cs_leave(g,c->cs); atomic_store(&c->got,2); KH_RET(g); }
static void vst_vblank(KCtx *g){ kh_vclock_idle(1); kh_vclock_test_vblank(g); kh_vclock_idle(0); }    /* as kh_vclock_vblank: the stepping thread is idle */
static KThread *vst_hook_t, *vst_hook_caller; static KCtx *vst_hook_g; static u64 vst_hook_cs; static _Atomic int vst_hook_fired;
static void vst_release_hook(KThread *t){                                                           /* E: inside the pass, after the waiter was judged idle */
    if(t!=vst_hook_t||cur_thread()!=vst_hook_caller||atomic_load(&vst_hook_fired)) return;
    atomic_store(&vst_hook_fired,1); cs_leave(vst_hook_g,vst_hook_cs); }
/* F, G, H (review U35 n=20/29): a guest thread that runs (never idle) until a host time or until stopped; a clock client's thread */
typedef struct { _Atomic u64 until; _Atomic int stop, ended; KThread *_Atomic t; } StBusy;
static void st_busy(KCtx *g){ StBusy *b=(StBusy*)(uintptr_t)A0(g); atomic_store(&b->t,cur_thread());
    while(!atomic_load(&b->stop)&&(!atomic_load(&b->until)||kh_host_ns()<atomic_load(&b->until))) sched_yield();
    atomic_store(&b->ended,1); KH_RET(g); }
typedef struct { int id; u64 due; StBusy *busy; KThread *_Atomic t; _Atomic int ran, busy_ended_at_run, done; } StVCl;
static void st_vclient(KCtx *g){ StVCl *c=(StVCl*)(uintptr_t)A0(g); atomic_store(&c->t,cur_thread());
    kh_vclock_client_wait(c->id,c->due); atomic_store(&c->busy_ended_at_run,atomic_load(&c->busy->ended)); atomic_store(&c->ran,1);
    kh_vclock_client_drop(c->id); atomic_store(&c->done,1); KH_RET(g); }
/* H: two threads with one start routine and no names (equal keys): role 0 sleeps 5 ms on the virtual clock, then runs 400 ms; role 1
 * waits 5 ms on an event that is never set.  Same instant: the sleep (key sequence 0) goes before the time-out (1). */
typedef struct { int role; u64 ev; StBusy *busy; KThread *_Atomic t; _Atomic u32 res; _Atomic u64 at; _Atomic int done; } StH;
static void st_hthread(KCtx *g){ StH *h=(StH*)(uintptr_t)A0(g); atomic_store(&h->t,cur_thread());
    if(h->role==0){ kh_vclock_vsleep(5); atomic_store(&h->busy->until,kh_host_ns()+400000000ull); while(kh_host_ns()<atomic_load(&h->busy->until)) sched_yield(); atomic_store(&h->busy->ended,1); }
    else { u32 r=kh_wait(g,h->ev,5); atomic_store(&h->at,kh_vclock_now_ns()); atomic_store(&h->res,r); }
    atomic_store(&h->done,1); KH_RET(g); }
static int vst_flag(_Atomic int *f,int ms){ for(int i=0;i<ms*5&&!atomic_load(f);i++){ struct timespec ts={0,200000}; nanosleep(&ts,0); } return atomic_load(f); }
int kh_sync_vclock_selftest(KCtx *g){
    int checks=0,fails=0;
    if(kh_vclock_stepped()){ fprintf(stderr,"sync vclock selftest FAIL: the clock is already stepped (KHNMC_CLOCK=frames run?): not run\n"); return 1; }
    int sync0=kh_vclock_sync; const u64 T0=6000000000000ull;
    kh_vclock_test_begin(T0); kh_vclock_sync=1;
    atomic_store(&vst_stop,0); atomic_store(&vst_what,NULL); pthread_t wd; SCHECK(pthread_create(&wd,NULL,vst_watchdog,NULL)==0);
    static StVW va[2], vb[3], vc[2];
    /* A. one event, an infinite waiter and a 5 ms waiter */
    atomic_store(&vst_what,"A (one event: an infinite and a 5 ms waiter)");
    u64 ev=mk_event(0,0); vst_start(&va[0],ev,0xFFFFFFFFu); vst_start(&va[1],ev,5);
    kh_spawn_hostfn_thread(g,"sync_vclock_selftest_a",st_vwaiter,(u64)(uintptr_t)&va[0]); SCHECK(vst_blocked(&va[0],3000));
    kh_spawn_hostfn_thread(g,"sync_vclock_selftest_a",st_vwaiter,(u64)(uintptr_t)&va[1]); SCHECK(vst_blocked(&va[1],3000));
    { KWaitObj *w=h_get(ev,HK_EVENT); pthread_mutex_lock(&w->m); SCHECK(w->waiters==2); pthread_mutex_unlock(&w->m); }
    vst_set[0]=cur_thread(); vst_set[1]=atomic_load(&va[0].t); vst_set[2]=atomic_load(&va[1].t); vst_filter(3);
    u64 a0=kh_vclock_now_ns(); vst_vblank(g);
    SCHECK(atomic_load(&va[1].done)&&atomic_load(&va[1].res)==0x102); SCHECK(atomic_load(&va[1].at)==a0+5000000ull);   /* timed out at its virtual deadline */
    SCHECK(!atomic_load(&va[0].done)); SCHECK(kh_vclock_now_ns()==a0+16666666ull);
    kh_event_set(ev); SCHECK(vst_done(&va[0],3000)&&atomic_load(&va[0].res)==0);                                  /* the other waiter still takes the event */
    /* B. one thread handle (the lock is that thread's m): an infinite and a 5 ms waiter; the thread itself waits for an event */
    atomic_store(&vst_what,"B (one thread handle: an infinite and a 5 ms waiter)");
    u64 evx=mk_event(1,0); vst_start(&vb[0],evx,0xFFFFFFFFu);
    u64 hx=thread_create(g,0,kh_host_fn("sync_vclock_selftest_target",st_vwaiter),(u64)(uintptr_t)&vb[0],0,NULL); SCHECK(hx!=0); SCHECK(vst_blocked(&vb[0],3000));
    vst_start(&vb[1],hx,0xFFFFFFFFu); vst_start(&vb[2],hx,5);
    kh_spawn_hostfn_thread(g,"sync_vclock_selftest_b",st_vwaiter,(u64)(uintptr_t)&vb[1]); SCHECK(vst_blocked(&vb[1],3000));
    kh_spawn_hostfn_thread(g,"sync_vclock_selftest_b",st_vwaiter,(u64)(uintptr_t)&vb[2]); SCHECK(vst_blocked(&vb[2],3000));
    vst_set[1]=atomic_load(&vb[0].t); vst_set[2]=atomic_load(&vb[1].t); vst_set[3]=atomic_load(&vb[2].t); vst_filter(4);
    a0=kh_vclock_now_ns(); vst_vblank(g);
    SCHECK(atomic_load(&vb[2].done)&&atomic_load(&vb[2].res)==0x102); SCHECK(atomic_load(&vb[2].at)==a0+5000000ull); SCHECK(!atomic_load(&vb[1].done));
    kh_event_set(evx); SCHECK(vst_done(&vb[0],3000)&&atomic_load(&vb[0].res)==0); SCHECK(vst_done(&vb[1],3000)&&atomic_load(&vb[1].res)==0);   /* the thread ended: its handle is signalled */
    h_close(hx); h_close(evx);
    /* C. equal keys, two events, one instant; 20 rounds (a race, not forced) */
    atomic_store(&vst_what,"C (two timed waiters with equal keys due at one instant)");
    u64 e1=mk_event(0,0), e2=mk_event(0,0); int okc=0, keys=1;
    for(int r=0;r<20;r++){
        vst_start(&vc[0],e1,5); vst_start(&vc[1],e2,5);
        kh_spawn_hostfn_thread(g,"sync_vclock_selftest_c",st_vwaiter,(u64)(uintptr_t)&vc[0]); kh_spawn_hostfn_thread(g,"sync_vclock_selftest_c",st_vwaiter,(u64)(uintptr_t)&vc[1]);
        if(!vst_blocked(&vc[0],3000)||!vst_blocked(&vc[1],3000)) break;
        vst_set[1]=atomic_load(&vc[0].t); vst_set[2]=atomic_load(&vc[1].t); vst_filter(3);
        keys&=kh_vclock_thread_key(vst_set[1])==kh_vclock_thread_key(vst_set[2]);
        a0=kh_vclock_now_ns(); vst_vblank(g);
        okc+=atomic_load(&vc[0].res)==0x102&&atomic_load(&vc[1].res)==0x102&&atomic_load(&vc[0].at)==a0+5000000ull&&atomic_load(&vc[1].at)==a0+5000000ull;
        vst_done(&vc[0],3000); vst_done(&vc[1],3000);
    }
    SCHECK(keys); SCHECK(okc==20); h_close(e1); h_close(e2); h_close(ev);
    /* D. a critical-section waiter past 2,000 spins: idle while the section is owned by ANOTHER thread, NOT idle once it is free (it used
     *    to stay idle until it next polled, so the clock could step in between: the one-CPU divergence of the title music), and NOT idle
     *    while the owner field holds its own id (second fixer: its compare-exchange has taken the section and it runs, before it marks
     *    itself running; the old test called that idle).
     * E. (second fixer) a release in the MIDDLE of the test's pass: the pass looks at the waiter (the section owned by this thread: idle),
     *    then this thread leaves the section before the pass ends (as an owner that leaves and blocks between the pass's look at the
     *    waiter and its look at the owner).  The pass must not say quiet: the waiter is runnable.  Forced with a hook inside the pass. */
    atomic_store(&vst_what,"D (critical-section waiter)");
    { u64 cs=ga_calloc(1,0x28); u64 *own=(u64*)XL(g,cs+0x10); wr64(g,cs+0x10,0x7777777ull); static StCs sc; sc.cs=cs; atomic_store(&sc.t,NULL); atomic_store(&sc.got,0); atomic_store(&sc.go,0);
      kh_spawn_hostfn_thread(g,"sync_vclock_selftest_cs",st_cswaiter,(u64)(uintptr_t)&sc);
      int st3=0; for(int i=0;i<15000&&!st3;i++){ KThread *t=atomic_load(&sc.t); st3=t&&atomic_load(&t->vc_state)==3; if(!st3){ struct timespec ts={0,200000}; nanosleep(&ts,0); } }
      SCHECK(st3); KThread *wt=atomic_load(&sc.t), *me=cur_thread(); vst_set[0]=wt; vst_filter(1);
      SCHECK(kh_threads_quiet(NULL,0)==1);                                                          /* owned by someone else: idle */
      __atomic_store_n(own,(u64)wt->tid,__ATOMIC_RELEASE); SCHECK(kh_threads_quiet(NULL,0)==0);    /* its own id (it cannot take it: the field is not 0): running */
      atomic_store(&vst_what,"E (a critical section left during the test's pass)");
      wr32(g,cs+0xC,1); __atomic_store_n(own,(u64)me->tid,__ATOMIC_RELEASE);                         /* this thread owns it now */
      SCHECK(kh_threads_quiet(NULL,0)==1);                                                          /* owned by another thread: idle */
      u64 r0=vc_rel_retests; vst_hook_t=wt; vst_hook_caller=me; vst_hook_g=g; vst_hook_cs=cs; atomic_store(&vst_hook_fired,0);
      pthread_mutex_lock(&tlist_m); quiet_hook=vst_release_hook; pthread_mutex_unlock(&tlist_m);
      int q=kh_threads_quiet(NULL,0);
      pthread_mutex_lock(&tlist_m); quiet_hook=NULL; pthread_mutex_unlock(&tlist_m);
      SCHECK(atomic_load(&vst_hook_fired)==1&&rd64(g,cs+0x10)!=(u64)me->tid);                       /* left inside the pass */
      SCHECK(q==0&&vc_rel_retests==r0+1);                                                           /* not quiet: the waiter is runnable */
      SCHECK(kh_threads_quiet(NULL,0)==0);                                                          /* free, or just taken by the waiter: running */
      for(int i=0;i<15000&&!atomic_load(&sc.got);i++){ struct timespec ts={0,200000}; nanosleep(&ts,0); } SCHECK(atomic_load(&sc.got)==1);
      atomic_store(&sc.go,1); for(int i=0;i<15000&&atomic_load(&sc.got)!=2;i++){ struct timespec ts={0,200000}; nanosleep(&ts,0); }
      SCHECK(rd64(g,cs+0x10)==0); vst_filter(0); ga_free(cs); }
    /* F. (review U35 n=20/29) a clock client due NOW waits for a guest thread that runs 400 ms: longer than the client-stuck budget (30
     *    slices, 5 ms each here), shorter than the quiescence guard.  The clock used to charge that wait to the client (the named fatal
     *    vclock-client-stuck; counted by the recorder here); now it waits uncharged and the client runs once the thread is done.
     * G. the thread never stops: the client is let go by the quiescence guard (300 ms here; counted), it is not declared stuck.
     * H. the same for a wait's virtual time-out (a timed-wait client) due while a thread runs 400 ms. */
    { extern u64 kh_vclock_test_stuck_record(int), kh_vclock_test_quiet_guard(u64), kh_vclock_client_guard_count(void); extern int kh_vclock_test_client_quiet(int);
      unsigned sl0=kh_stuck_slice_ms; kh_stuck_slice_ms=5; u64 r0=kh_vclock_test_stuck_record(1), cg0=kh_vclock_client_guard_count();
      static StBusy bz; static StVCl cl;
      for(int part=0;part<2;part++){                                                                   /* 0 = F, 1 = G */
          atomic_store(&vst_what,part?"G (a due client let go by the quiescence guard)":"F (a due client waiting for quiescence)");
          u64 gd0=part?kh_vclock_test_quiet_guard(300000000ull):0;
          atomic_store(&bz.stop,0); atomic_store(&bz.ended,0); atomic_store(&bz.t,NULL); atomic_store(&bz.until,part?0:kh_host_ns()+400000000ull);
          kh_spawn_hostfn_thread(g,"sync_vclock_selftest_busy",st_busy,(u64)(uintptr_t)&bz);
          u64 a0=kh_vclock_now_ns(); cl.due=a0; cl.busy=&bz; cl.id=kh_vclock_client_add_prio("sync-vclock-selftest-client",a0,0);
          atomic_store(&cl.t,NULL); atomic_store(&cl.ran,0); atomic_store(&cl.busy_ended_at_run,0); atomic_store(&cl.done,0);
          kh_spawn_hostfn_thread(g,"sync_vclock_selftest_client",st_vclient,(u64)(uintptr_t)&cl);
          for(int i=0;i<15000&&!(atomic_load(&bz.t)&&atomic_load(&cl.t));i++){ struct timespec ts={0,200000}; nanosleep(&ts,0); }
          SCHECK(atomic_load(&bz.t)&&atomic_load(&cl.t));
          vst_set[0]=cur_thread(); vst_set[1]=atomic_load(&bz.t); vst_set[2]=atomic_load(&cl.t); vst_filter(3);
          int qm=0; for(int i=0;i<15000&&!qm;i++){ qm=kh_vclock_test_client_quiet(cl.id); if(!qm){ struct timespec ts={0,200000}; nanosleep(&ts,0); } }
          SCHECK(qm);                                                                                     /* at its instant, waiting for quiescence */
          u64 h0=kh_host_ns(); vst_vblank(g); u64 dt=kh_host_ns()-h0;
          SCHECK(kh_vclock_test_stuck_record(1)==r0);                                                     /* never charged as stuck */
          SCHECK(atomic_load(&cl.ran) && kh_vclock_now_ns()>a0 && kh_vclock_now_ns()==T0+kh_vclock_vblanks()*1000000000ull/60ull);   /* one whole period */
          if(!part) SCHECK(atomic_load(&cl.busy_ended_at_run) && kh_vclock_client_guard_count()==cg0);  /* F: ran after the thread was done: ordered */
          else SCHECK(!atomic_load(&cl.busy_ended_at_run) && kh_vclock_client_guard_count()==cg0+1 && dt>=300000000ull);   /* G: let go by the guard */
          atomic_store(&bz.stop,1); SCHECK(vst_flag(&bz.ended,3000) && vst_flag(&cl.done,3000));
          if(part) kh_vclock_test_quiet_guard(gd0);
          vst_filter(0); }
      atomic_store(&vst_what,"H (a wait's time-out due while a thread runs)");
      { static StH hs[2]; static StBusy hb; u64 he=mk_event(0,0); u64 fn=kh_host_fn("sync_vclock_selftest_h",st_hthread); u64 hh[2];
        atomic_store(&hb.stop,0); atomic_store(&hb.ended,0); atomic_store(&hb.until,~0ull);
        for(int i=0;i<2;i++){ hs[i].role=i; hs[i].ev=he; hs[i].busy=&hb; atomic_store(&hs[i].t,NULL); atomic_store(&hs[i].res,0xdeadu); atomic_store(&hs[i].at,0); atomic_store(&hs[i].done,0); }
        u64 a0=kh_vclock_now_ns();
        for(int i=0;i<2;i++) hh[i]=thread_create(g,0,fn,(u64)(uintptr_t)&hs[i],0,NULL);                 /* unnamed ("thrN"), one start routine: equal keys */
        int up=0; for(int i=0;i<15000&&!up;i++){ KThread *t0=atomic_load(&hs[0].t), *t1=atomic_load(&hs[1].t); up=t0&&t1&&atomic_load(&t0->vc_state)!=0&&atomic_load(&t1->vc_state)==1&&atomic_load(&t1->vc_w);
            if(!up){ struct timespec ts={0,200000}; nanosleep(&ts,0); } }
        SCHECK(up && hh[0] && hh[1]);                                                                  /* the sleeper idle in its clock wait, the waiter blocked */
        vst_set[0]=cur_thread(); vst_set[1]=atomic_load(&hs[0].t); vst_set[2]=atomic_load(&hs[1].t); vst_filter(3);
        SCHECK(kh_vclock_thread_key(vst_set[1])==kh_vclock_thread_key(vst_set[2]));
        vst_vblank(g);
        SCHECK(kh_vclock_test_stuck_record(1)==r0);                                                     /* never charged as stuck */
        SCHECK(vst_flag(&hs[1].done,3000) && atomic_load(&hs[1].res)==0x102 && atomic_load(&hs[1].at)==a0+5000000ull);   /* timed out at its instant */
        SCHECK(atomic_load(&hb.ended) && kh_vclock_client_guard_count()==cg0+1 && kh_vclock_now_ns()==T0+kh_vclock_vblanks()*1000000000ull/60ull);  /* after the thread: ordered */
        SCHECK(vst_flag(&hs[0].done,3000)); for(int i=0;i<2;i++) if(hh[i]){ SCHECK(kh_wait(g,hh[i],5000)==0); h_close(hh[i]); } h_close(he); vst_filter(0); }
      kh_vclock_test_stuck_record(0); kh_stuck_slice_ms=sl0; }
    vst_filter(0); atomic_store(&vst_what,NULL); atomic_store(&vst_stop,1); pthread_join(wd,NULL);
    kh_vclock_sync=sync0; kh_vclock_test_end();
    fprintf(stderr,"kh: sync vclock selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}
