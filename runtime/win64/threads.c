/* threads.c : guest threads (pthreads), TEB/TLS blocks, wait objects, critical sections, fibers (stackful coroutines with an
 * x86-64 and an arm64 context switch), kernel32 thread/sync/fiber imports and the msvcp140 _Mtx_* imports. */
#include "khnmc_rt.h"
#include <sys/mman.h>
#include <setjmp.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

void kh_set_cur_g(KCtx *g); void kh_install_sigaltstack(void); void kh_guest_bt(KCtx *g,FILE *f);
#if defined(__x86_64__)
#define KH_CPU_RELAX() __asm__ __volatile__("pause")
#else
#define KH_CPU_RELAX() __asm__ __volatile__("yield")
#endif

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
__asm__(".text\n.globl kh_ctx_switch\nkh_ctx_switch:\n"
        " sub sp, sp, #176\n stp x19,x20,[sp,#0]\n stp x21,x22,[sp,#16]\n stp x23,x24,[sp,#32]\n stp x25,x26,[sp,#48]\n stp x27,x28,[sp,#64]\n stp x29,x30,[sp,#80]\n"
        " stp d8,d9,[sp,#96]\n stp d10,d11,[sp,#112]\n stp d12,d13,[sp,#128]\n stp d14,d15,[sp,#144]\n"
        " mov x9, sp\n str x9,[x0]\n mov sp, x1\n"
        " ldp x19,x20,[sp,#0]\n ldp x21,x22,[sp,#16]\n ldp x23,x24,[sp,#32]\n ldp x25,x26,[sp,#48]\n ldp x27,x28,[sp,#64]\n ldp x29,x30,[sp,#80]\n"
        " ldp d8,d9,[sp,#96]\n ldp d10,d11,[sp,#112]\n ldp d12,d13,[sp,#128]\n ldp d14,d15,[sp,#144]\n add sp, sp, #176\n ret\n"
        ".globl kh_fiber_tramp\nkh_fiber_tramp:\n mov x0, x19\n bl kh_fiber_main\n brk #0\n");
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
static KThread *tlist[4096]; static u32 ntlist; static _Atomic u32 next_tid=1000;
u64 kh_tls_tmpl_lo, kh_tls_tmpl_hi, kh_tls_index_va, kh_tls_callbacks_va;

KCtx *kh_new_ctx(void){ KCtx *g=aligned_alloc(64,(sizeof(KCtx)+63)&~63ull); memset(g,0,sizeof *g); PUT(u64,176,1); g->bias=gs_bias; return g; }
u64 kh_new_guest_stack(u64 size,u64 *lo){ u64 p=ga_pages(size); if(!p){ fprintf(stderr,"kh: out of guest space for stacks\n"); abort(); } if(lo)*lo=p; return p+size-0x80; }
void kh_ctx_set_stack(KCtx *g,u64 top){ KH_RSP(g)=top&~0xFull; }

#define TEB_SIZE 0x1000
u64 kh_make_teb(KThread *t){
    KCtx *g=t->g; t->teb=ga_alloc(TEB_SIZE,16); memset(hp(g,t->teb),0,TEB_SIZE);
    t->tlsarr=ga_alloc(64*8,16); memset(hp(g,t->tlsarr),0,64*8);
    u64 sz=kh_tls_tmpl_hi-kh_tls_tmpl_lo; t->tlsblk=ga_alloc(sz+0x100,16); memset(hp(g,t->tlsblk),0,sz+0x100);
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
    pthread_mutex_init(&t->m,0); pthread_cond_init(&t->cv,0);
    pthread_mutex_lock(&tlist_m); if(ntlist<4096) tlist[ntlist++]=t; pthread_mutex_unlock(&tlist_m); return t;
}
static void thread_bind(KThread *t){ tl_thread=t; t->g->rt=t; kh_install_sigaltstack(); kh_set_cur_g(t->g); }
void kh_thread_init_main(void){
    KThread *t=thread_new(); t->is_main=1; u64 top=kh_new_guest_stack(8ull<<20,&t->stack_lo); t->stack_hi=t->stack_lo+(8ull<<20);
    kh_ctx_set_stack(t->g,top); kh_make_teb(t); thread_bind(t); snprintf(t->name,sizeof t->name,"main");
}

/* ---- wait objects ---------------------------------------------------------------------------------------------------- */
static KWaitObj *wobj_new(int kind){ KWaitObj *w=calloc(1,sizeof *w); w->kind=kind; pthread_mutex_init(&w->m,0); pthread_cond_init(&w->cv,0); return w; }
static void abs_deadline(struct timespec *ts,u32 ms){ clock_gettime(CLOCK_REALTIME,ts); ts->tv_sec+=ms/1000; ts->tv_nsec+=(long)(ms%1000)*1000000L; if(ts->tv_nsec>=1000000000L){ ts->tv_sec++; ts->tv_nsec-=1000000000L; } }
u32 kh_wait_timeout_ms_max;
u32 kh_wait(KCtx *g,u64 h,u32 ms){
    KThread *me=cur_thread(); int k=h_kind(h);
    if(h==PSEUDO_THREAD||h==PSEUDO_PROCESS) return 0x102;
    KWaitObj *w=h_get(h,0); if(!w){ kh_set_last_error(6); return 0xFFFFFFFFu; }
    pthread_mutex_lock(&w->m);
    struct timespec dl; if(ms!=0xFFFFFFFFu) abs_deadline(&dl,ms);
    u32 res=0; u64 waited=0;
    for(;;){
        int got=0;
        if(k==HK_EVENT){ if(w->signaled){ if(!w->manual) w->signaled=0; got=1; } }
        else if(k==HK_SEM){ if(w->count>0){ w->count--; got=1; } }
        else if(k==HK_MUTEX){ if(w->recurse==0||w->owner==me->tid){ w->owner=me->tid; w->recurse++; got=1; } }
        else if(k==HK_THREAD){ if(w->thread->done) got=1; }
        else if(k==HK_PROCESS){ got=0; }
        else { pthread_mutex_unlock(&w->m); return 0xFFFFFFFFu; }
        if(got) break;
        if(ms==0){ res=0x102; break; }
        int rc;
        if(ms==0xFFFFFFFFu){ struct timespec d2; abs_deadline(&d2,10000); rc=pthread_cond_timedwait(&w->cv,&w->m,&d2);
            if(rc==ETIMEDOUT){ waited+=10; if(kh_verbose||(waited%60==0)) fprintf(stderr,"kh: thread %u ('%s') waiting %llus on handle %llx kind %d (site rva %llx)\n",me->tid,me->name,(unsigned long long)waited,(unsigned long long)h,k,(unsigned long long)(rd64(g,KH_RSP(g))-GS_BASE)); } }
        else { rc=pthread_cond_timedwait(&w->cv,&w->m,&dl); if(rc==ETIMEDOUT){ res=0x102; /* re-test once */ int g2=0; if(k==HK_EVENT&&w->signaled){ if(!w->manual) w->signaled=0; g2=1; } else if(k==HK_SEM&&w->count>0){ w->count--; g2=1; } else if(k==HK_MUTEX&&(w->recurse==0||w->owner==me->tid)){ w->owner=me->tid; w->recurse++; g2=1; } else if(k==HK_THREAD&&w->thread->done) g2=1; if(g2) res=0; break; } }
    }
    pthread_mutex_unlock(&w->m); return res;
}
/* thread-object wait uses the thread's own cv (done flag changes there) */
static void thread_done_broadcast(KThread *t){ pthread_mutex_lock(&tlist_m); /* wake all waiters: they poll with timed waits on their own wobj cv; broadcast through handles */ pthread_mutex_unlock(&tlist_m); }

/* ---- critical sections (state lives in the guest CRITICAL_SECTION: +0xC recursion, +0x10 owner) ---------------------------- */
static inline u64 cs_owner(KCtx *g,u64 cs){ return __atomic_load_n((u64*)XL(g,cs+0x10),__ATOMIC_ACQUIRE); }
static void cs_enter(KCtx *g,u64 cs){
    u64 me=cur_thread()->tid; u64 *own=(u64*)XL(g,cs+0x10);
    if(__atomic_load_n(own,__ATOMIC_RELAXED)==me){ wr32(g,cs+0xC,rd32(g,cs+0xC)+1); return; }
    int spins=0;
    for(;;){ u64 z=0; if(__atomic_compare_exchange_n(own,&z,me,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        if(++spins<200) KH_CPU_RELAX(); else if(spins<2000) sched_yield(); else { struct timespec ts={0,50000}; nanosleep(&ts,0); } }
    wr32(g,cs+0xC,1);
}
static void cs_leave(KCtx *g,u64 cs){
    u64 *own=(u64*)XL(g,cs+0x10); u32 r=rd32(g,cs+0xC);
    if(__atomic_load_n(own,__ATOMIC_RELAXED)!=cur_thread()->tid){ kh_fatal(g,"LeaveCriticalSection(%llx) by non-owner (owner=%llx me=%u)",(unsigned long long)cs,(unsigned long long)*own,cur_thread()->tid); }
    if(r>1){ wr32(g,cs+0xC,r-1); return; }
    wr32(g,cs+0xC,0); __atomic_store_n(own,0,__ATOMIC_RELEASE);
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
    struct timespec now; clock_gettime(CLOCK_REALTIME,&now); s64 d=((s64)ts.tv_sec-now.tv_sec)*1000000000ll+((s64)ts.tv_nsec-now.tv_nsec);
    if(d>0){ if(d>60ll*1000000000ll){ kh_warn("_Thrd_sleep: absolute target %llds in the future, clamping to 60 s",(long long)(d/1000000000ll)); ts.tv_sec=now.tv_sec+60; ts.tv_nsec=now.tv_nsec; } { struct timespec rel={(time_t)(d/1000000000ll),(long)(d%1000000000ll)}; if(d>60ll*1000000000ll){ rel.tv_sec=60; rel.tv_nsec=0; } nanosleep(&rel,0); } } }
IMP(MSVCP,_Xtime_get_ticks){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); SET_RAX(g,(u64)ts.tv_sec*10000000ull+(u64)ts.tv_nsec/100); }

/* ---- events / semaphores / mutexes ------------------------------------------------------------------------------------ */
static u64 mk_event(int manual,int initial){ KWaitObj *w=wobj_new(HK_EVENT); w->manual=manual; w->signaled=initial; return h_new(HK_EVENT,w); }
IMP(K32,CreateEventA){ SET_RAX(g,mk_event((int)(A1(g)&0xff),(int)(A2(g)&0xff))); }
IMP(K32,CreateEventW){ SET_RAX(g,mk_event((int)(A1(g)&0xff),(int)(A2(g)&0xff))); }
IMP(K32,CreateEventExA){ u32 fl=(u32)A2(g); SET_RAX(g,mk_event((fl&1)?1:0,(fl&2)?1:0)); }   /* CREATE_EVENT_MANUAL_RESET=1 INITIAL_SET=2 */
IMP(K32,SetEvent){ KWaitObj *w=h_get(A0(g),HK_EVENT); if(!w){ SET_RAX(g,0); return; } pthread_mutex_lock(&w->m); w->signaled=1; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m); SET_RAX(g,1); }
IMP(K32,ResetEvent){ KWaitObj *w=h_get(A0(g),HK_EVENT); if(!w){ SET_RAX(g,0); return; } pthread_mutex_lock(&w->m); w->signaled=0; pthread_mutex_unlock(&w->m); SET_RAX(g,1); }
static u64 mk_sem(s32 init,s32 max){ KWaitObj *w=wobj_new(HK_SEM); w->count=init; w->maxcount=max; return h_new(HK_SEM,w); }
IMP(K32,CreateSemaphoreA){ SET_RAX(g,mk_sem((s32)A1(g),(s32)A2(g))); }
IMP(K32,CreateSemaphoreW){ SET_RAX(g,mk_sem((s32)A1(g),(s32)A2(g))); }
IMP(K32,ReleaseSemaphore){ KWaitObj *w=h_get(A0(g),HK_SEM); if(!w){ SET_RAX(g,0); return; } pthread_mutex_lock(&w->m);
    if(A2(g)) wr32(g,A2(g),(u32)w->count); long n=(long)(s32)A1(g); if(w->count+n>w->maxcount){ pthread_mutex_unlock(&w->m); kh_set_last_error(298); SET_RAX(g,0); return; }
    w->count+=n; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m); SET_RAX(g,1); }
IMP(K32,CreateMutexA){ KWaitObj *w=wobj_new(HK_MUTEX); u64 h=h_new(HK_MUTEX,w); if(A1(g)&0xff){ w->owner=cur_thread()->tid; w->recurse=1; } SET_RAX(g,h); }
IMP(K32,ReleaseMutex){ KWaitObj *w=h_get(A0(g),HK_MUTEX); if(!w){ SET_RAX(g,0); return; } pthread_mutex_lock(&w->m);
    if(w->recurse==0||w->owner!=cur_thread()->tid){ pthread_mutex_unlock(&w->m); kh_set_last_error(288); SET_RAX(g,0); return; }
    if(--w->recurse==0){ w->owner=0; pthread_cond_broadcast(&w->cv); } pthread_mutex_unlock(&w->m); SET_RAX(g,1); }
IMP(K32,WaitForSingleObject){ SET_RAX(g,kh_wait(g,A0(g),(u32)A1(g))); }
IMP(K32,WaitForSingleObjectEx){ SET_RAX(g,kh_wait(g,A0(g),(u32)A1(g))); }
IMP(K32,CloseHandle){ u64 h=A0(g); if(h==PSEUDO_THREAD||h==PSEUDO_PROCESS){ SET_RAX(g,1); return; } int ok=h_close(h); if(!ok) kh_set_last_error(6); SET_RAX(g,ok); }
IMP(K32,Sleep){ u32 ms=(u32)A0(g); if(ms==0) sched_yield(); else { struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); } }
IMP(K32,SleepEx){ u32 ms=(u32)A0(g); if(ms==0) sched_yield(); else { struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); } SET_RAX(g,0); }
IMP(K32,SwitchToThread){ sched_yield(); SET_RAX(g,1); }

/* ---- threads --------------------------------------------------------------------------------------------------------- */
static void thread_exit_notify(KThread *t){ pthread_mutex_lock(&t->m); t->done=1; pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m); }
static __thread jmp_buf *tl_exit_jb;
static void *thread_main(void *arg){
    KThread *t=arg; thread_bind(t); KCtx *g=t->g;
    jmp_buf jb; tl_exit_jb=&jb;
    if(setjmp(jb)==0){
        kh_run_tls_callbacks(g,2);
        pthread_mutex_lock(&t->m); while(t->suspended) pthread_cond_wait(&t->cv,&t->m); pthread_mutex_unlock(&t->m);
        t->exit_code=(u32)kh_call(g,t->start,t->param,0,0,0);
    }
    kh_run_tls_callbacks(g,3);
    thread_exit_notify(t);
    return NULL;
}
static u64 thread_create(KCtx *g,u64 stack_size,u64 start,u64 param,int suspended,u32 *tid_out){
    KThread *t=thread_new(); t->start=start; t->param=param; t->suspended=suspended;
    u64 ss=stack_size<(2ull<<20)?(2ull<<20):stack_size; u64 top=kh_new_guest_stack(ss,&t->stack_lo); t->stack_hi=t->stack_lo+ss; kh_ctx_set_stack(t->g,top); kh_make_teb(t);
    snprintf(t->name,sizeof t->name,"thr%u",t->tid);
    KWaitObj *w=wobj_new(HK_THREAD); w->thread=t; t->handle=(u32)h_new(HK_THREAD,w);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,48ull<<20);
    int rc=pthread_create(&t->pt,&at,thread_main,t); pthread_attr_destroy(&at);
    if(rc){ kh_warn("pthread_create failed: %d",rc); h_close(t->handle); return 0; }
    t->started=1; if(tid_out) *tid_out=t->tid; (void)g;
    return t->handle;
}
IMP(K32,CreateThread){ u32 tid=0; u64 h=thread_create(g,A1(g),A2(g),A3(g),(rd32(g,KH_RSP(g)+8+0x20)&4)?1:0,&tid); u64 ptid=rd64(g,KH_RSP(g)+8+0x28); if(h&&ptid) wr32(g,ptid,tid); SET_RAX(g,h); }
IMP(CRTR,_beginthreadex){ u32 tid=0; u64 h=thread_create(g,A1(g),A2(g),A3(g),(rd32(g,KH_RSP(g)+8+0x20)&4)?1:0,&tid); u64 ptid=rd64(g,KH_RSP(g)+8+0x28); if(h&&ptid) wr32(g,ptid,tid); SET_RAX(g,h); }
IMP(CRTR,_beginthread){ u64 h=thread_create(g,A1(g),A0(g),A2(g),0,NULL); SET_RAX(g,h?h:(u64)-1); }   /* _beginthread(start, stack, arg) */
IMP(K32,ResumeThread){ KWaitObj *w=h_get(A0(g),HK_THREAD); if(!w){ SET_RAX(g,(u64)-1); return; } KThread *t=w->thread; pthread_mutex_lock(&t->m); int was=t->suspended; t->suspended=0; pthread_cond_broadcast(&t->cv); pthread_mutex_unlock(&t->m); SET_RAX(g,was); }
IMP(K32,SuspendThread){ kh_warn("SuspendThread ignored (handle %llx)",(unsigned long long)A0(g)); SET_RAX(g,0); }
IMP(K32,GetCurrentThreadId){ SET_RAX(g,cur_thread()->tid); }
IMP(K32,GetCurrentThread){ SET_RAX(g,PSEUDO_THREAD); }
IMP(K32,GetCurrentProcess){ SET_RAX(g,PSEUDO_PROCESS); }
IMP(K32,GetCurrentProcessId){ SET_RAX(g,ga_deterministic?4242u:(u32)getpid()); }
IMP(K32,OpenThread){ u32 tid=(u32)A2(g); pthread_mutex_lock(&tlist_m); KThread *f=NULL; for(u32 i=0;i<ntlist;i++) if(tlist[i]->tid==tid) f=tlist[i]; pthread_mutex_unlock(&tlist_m);
    if(!f){ kh_set_last_error(87); SET_RAX(g,0); return; } KWaitObj *w=wobj_new(HK_THREAD); w->thread=f; SET_RAX(g,h_new(HK_THREAD,w)); }
IMP(K32,GetExitCodeThread){ u64 h=A0(g); KThread *t; if(h==PSEUDO_THREAD) t=cur_thread(); else { KWaitObj *w=h_get(h,HK_THREAD); if(!w){ SET_RAX(g,0); return; } t=w->thread; }
    wr32(g,A1(g),t->done?(u32)t->exit_code:259u); SET_RAX(g,1); }
IMP(K32,GetThreadPriority){ SET_RAX(g,0); }
IMP(K32,SetThreadPriority){ SET_RAX(g,1); }
IMP(K32,SetThreadIdealProcessor){ SET_RAX(g,0); }
IMP(K32,GetProcessAffinityMask){ long n=sysconf(_SC_NPROCESSORS_ONLN); u64 m=n>=64?~0ull:((1ull<<n)-1); if(A1(g)) wr64(g,A1(g),m); if(A2(g)) wr64(g,A2(g),m); SET_RAX(g,1); }
IMP(K32,TerminateThread){ KWaitObj *w=h_get(A0(g),HK_THREAD); if(w&&w->thread!=cur_thread()){ KThread *t=w->thread; pthread_cancel(t->pt); t->exit_code=(u32)A1(g); thread_exit_notify(t); kh_warn("TerminateThread: thread %u cancelled",t->tid); SET_RAX(g,1); return; } kh_fatal(g,"TerminateThread on self or bad handle"); }
static void thread_end(KCtx *g,u32 code){ KThread *t=cur_thread(); t->exit_code=code; if(!tl_exit_jb) kh_fatal(g,"thread exit with no exit frame"); longjmp(*tl_exit_jb,1); }
IMPN(CRTR,_endthreadex){ thread_end(g,(u32)A0(g)); }
IMPN(CRTR,_endthread){ thread_end(g,0); }
IMPN(K32,ExitProcess){ extern void kh_exit_process(int); kh_exit_process((int)A0(g)); }

/* ---- fibers ---------------------------------------------------------------------------------------------------------- */
u64 kh_fiber_host_stack=2ull<<20;            /* host stack of a fiber (lazily committed); the vkd3d backend raises it because native graphics code runs on it */
struct KFiber { KCtx *g; void *hstack; size_t hsize; void *sp; u64 start, param, handle, gstack_lo, gstack_sz; KThread *owner; int is_thread_fiber, finished; };
#define FIBER_MAGIC 0x46494245ull
static KFiber *fiber_of(KCtx *g,u64 h){ if(!GA_OK(h)||rd64(g,h+8)!=FIBER_MAGIC){ kh_fatal(g,"bad fiber handle %llx",(unsigned long long)h); } return (KFiber*)rd64(g,h); }
static u64 fiber_handle(KCtx *g,KFiber *f){ u64 h=ga_alloc(64,16); wr64(g,h,(u64)(uintptr_t)f); wr64(g,h+8,FIBER_MAGIC); f->handle=h; return h; }
void kh_fiber_main(KFiber *f){
    KCtx *g=f->g; (void)kh_call(g,f->start,f->param,0,0,0);
    kh_fatal(g,"fiber start routine 0x%llx returned (Windows would ExitThread)",(unsigned long long)f->start);
}
u64 kh_fiber_create(KCtx *g,u64 stack_size,u64 start,u64 param){
    KFiber *f=calloc(1,sizeof *f); f->g=kh_new_ctx(); f->start=start; f->param=param;
    u64 gs=stack_size<(1ull<<20)?(1ull<<20):stack_size; f->gstack_sz=gs; u64 top=kh_new_guest_stack(gs,&f->gstack_lo); kh_ctx_set_stack(f->g,top);
    f->hsize=kh_fiber_host_stack; f->hstack=mmap(0,f->hsize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); if(f->hstack==MAP_FAILED) kh_fatal(g,"fiber host stack mmap failed");
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
    u64 gs=KH_GS(from->g); (void)gs; KH_GS(to->g)=t->teb; to->g->rt=t; to->owner=t; t->cur_fiber=to; from->owner=NULL;
    kh_set_cur_g(to->g);
    kh_ctx_switch(&from->sp,to->sp);
    /* resumed here, possibly on another host thread: whoever switched to us already set cur_fiber and gs */
    kh_set_cur_g(from->g);
}
void kh_fiber_delete(KCtx *g,u64 h){
    KFiber *f=fiber_of(g,h); KThread *t=cur_thread();
    if(f==t->cur_fiber && !f->is_thread_fiber) kh_fatal(g,"DeleteFiber of the running fiber (ExitThread semantics) not supported");
    if(f->is_thread_fiber){ kh_warn("DeleteFiber on a thread fiber"); return; }
    if(f->hstack) munmap(f->hstack,f->hsize);
    ga_pages_free(f->gstack_lo,f->gstack_sz); wr64(g,h+8,0); ga_free(h); free(f->g); free(f);
}
int kh_fiber_convert_to_thread(KCtx *g){
    KThread *t=cur_thread(); KFiber *f=t->thread_fiber; if(!f) return 0; if(t->cur_fiber!=f){ kh_fatal(g,"ConvertFiberToThread while a non-thread fiber is running"); }
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
u64 kh_spawn_hostfn_thread(KCtx *g,const char *name,KhFn fn,u64 param){ u64 a=kh_host_fn(name,fn); u64 h=thread_create(g,0,a,param,0,NULL); if(h) h_close(h); return h; }

u64 kh_mk_event_h(int manual,int initial){ return mk_event(manual,initial); }

void kh_dump_threads(FILE *f){
    pthread_mutex_lock(&tlist_m);
    for(u32 i=0;i<ntlist;i++){ KThread *t=tlist[i]; fprintf(f,"kh: thread %u '%s' done=%d cur_fiber=%p\n",t->tid,t->name,t->done,(void*)t->cur_fiber); kh_guest_bt(t->cur_fiber&&!t->cur_fiber->is_thread_fiber?t->cur_fiber->g:t->g,f); }
    pthread_mutex_unlock(&tlist_m);
}
