/* kh_compat_test.c : selftest of the host primitives of kh_compat.h.  The same checks run on Linux and on Darwin; none of them
 * depends on how fast the machine is (lower bounds come from the monotonic clock itself, upper bounds are seconds wide, and the
 * ordering checks use flags, not times). */
#include "khnmc_rt.h"
#include <signal.h>
#include <sys/wait.h>
#include <stdatomic.h>

static int checks,fails;
#define T(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"compat selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static long long mono_ms(void){ struct timespec ts; kh_mono_now(&ts); return (long long)ts.tv_sec*1000ll+ts.tv_nsec/1000000ll; }
static void ms_sleep(int ms){ struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); }

typedef struct { pthread_mutex_t m; pthread_cond_t cv; int flag, delay_ms; } CvT;
static void *cv_signaller(void *p){ CvT *c=p; ms_sleep(c->delay_ms); pthread_mutex_lock(&c->m); c->flag=1; pthread_cond_signal(&c->cv); pthread_mutex_unlock(&c->m); return 0; }

typedef struct { kh_tmutex *tm; _Atomic int holding, release, released, rc; } MxT;
static void *mx_holder(void *p){ MxT *x=p; atomic_store(&x->rc,kh_tmutex_lock_timeout_ms(x->tm,5000)); atomic_store(&x->holding,1);
    for(int i=0;i<20000&&!atomic_load(&x->release);i++) ms_sleep(1);
    atomic_store(&x->released,1); kh_tmutex_unlock(x->tm); return 0; }

/* ---- "a peer is stuck" waits against a STOPPED process (kh_compat.h: kh_tmutex_lock_stuck, kh_cond_wait_stuck, kh_cond_wait_stuck_rt) ----
 * Each case is a forked child: a holder thread keeps a lock (or keeps a condition false) until the parent says so through a pipe,
 * the child's main thread waits with a budget.  The parent stops the children (SIGSTOP) for LONGER than the budget, continues them,
 * and only then lets the holders go.  Sliced budget: the wait survives (exit 0).  The same budget as ONE deadline (1 slice): it has
 * expired during the stop and the child reports the time-out (exit 3): that is the defect, kept here as the control that proves the
 * scenario really kills an unsliced wait.  A holder that never lets go still ends in the time-out (exit 3): the budget is enforced. */
typedef struct { int mode, go_fd, rdy_fd; kh_tmutex tm; pthread_mutex_t m; pthread_cond_t cv; int flag; _Atomic int holding; } StuckT;
static void *stuck_holder(void *p){ StuckT *x=p; char c;
    if(x->mode==0) kh_tmutex_lock_timeout_ms(&x->tm,5000);
    atomic_store(&x->holding,1); if(write(x->rdy_fd,"r",1)!=1) _exit(90);
    if(read(x->go_fd,&c,1)!=1) return 0;                                  /* the parent closed the pipe: never let go */
    if(x->mode==0) kh_tmutex_unlock(&x->tm);
    else { pthread_mutex_lock(&x->m); x->flag=1; pthread_cond_broadcast(&x->cv); pthread_mutex_unlock(&x->m); }
    return 0; }
static void stuck_child(int mode,unsigned slice_ms,unsigned slices,int go_fd,int rdy_fd){
    static StuckT x; x.mode=mode; x.go_fd=go_fd; x.rdy_fd=rdy_fd; kh_stuck_slice_ms=slice_ms;
    if(mode==0){ kh_tmutex t0=KH_TMUTEX_INITIALIZER; x.tm=t0; }
    pthread_mutex_init(&x.m,0); if(mode==1) kh_cond_init(&x.cv); else pthread_cond_init(&x.cv,0);
    pthread_t th; if(pthread_create(&th,0,stuck_holder,&x)) _exit(91);
    while(!atomic_load(&x.holding)) ms_sleep(1);
    int rc=0;
    if(mode==0) rc=kh_tmutex_lock_stuck(&x.tm,slices);
    else { unsigned ex=0; pthread_mutex_lock(&x.m);
        while(!x.flag){ rc=mode==1?kh_cond_wait_stuck(&x.cv,&x.m,&ex,slices):kh_cond_wait_stuck_rt(&x.cv,&x.m,&ex,slices); if(rc) break; }
        pthread_mutex_unlock(&x.m); }
    _exit(rc==0?0:rc==ETIMEDOUT?3:92); }
static void stuck_selftest(void){
    enum { N=7 };                                    /* mode (0 timed lock, 1 monotonic condition, 2 default condition), slice ms, slices, stop it?, exit expected */
    static const struct { int mode; unsigned slice_ms, slices; int stop, want; } C[N]={
        {0,100,20,1,0},{1,100,20,1,0},{2,100,20,1,0},          /* 20 slices of 100 ms, stopped for 2.6 s: survives */
        {0,2000,1,1,3},{1,2000,1,1,3},{2,2000,1,1,3},          /* the same 2 s as one deadline: dead on resume (the control) */
        {0,50,6,0,3} };                                        /* never released: the budget still ends the wait */
    if(!kh_fork_ok()){ kh_selftest_not_run("compat","stuck-peer waits against a stopped process",N); return; }
    pid_t pid[N]; int go[N]; long long t0=mono_ms(); int started=0;
    for(int i=0;i<N;i++){ int g[2], r[2]; pid[i]=-1; go[i]=-1; if(pipe(g)||pipe(r)) break;
        fflush(stderr); pid[i]=kh_fork();
        if(!pid[i]){ close(g[1]); close(r[0]); stuck_child(C[i].mode,C[i].slice_ms,C[i].slices,g[0],r[1]); }
        close(g[0]); close(r[1]); go[i]=g[1]; char c; if(pid[i]>0&&read(r[0],&c,1)==1) started++; close(r[0]); }
    T(started==N);
    ms_sleep(50);                                                              /* every waiter is inside its wait */
    for(int i=0;i<N;i++) if(pid[i]>0&&C[i].stop) kill(pid[i],SIGSTOP);
    ms_sleep(2600);                                                            /* longer than the 2 s budgets */
    for(int i=0;i<N;i++) if(pid[i]>0&&C[i].stop) kill(pid[i],SIGCONT);
    ms_sleep(200);                                                             /* the holders have NOT let go yet when the children resume */
    { struct sigaction ign, old; memset(&ign,0,sizeof ign); ign.sa_handler=SIG_IGN; sigaction(SIGPIPE,&ign,&old);   /* a control child has ended by now: its pipe has no reader */
      for(int i=0;i<N;i++) if(go[i]>=0&&C[i].stop){ if(write(go[i],"u",1)!=1){} }
      sigaction(SIGPIPE,&old,0); }
    for(int i=0;i<N;i++){ int st=-1; if(pid[i]>0) waitpid(pid[i],&st,0); if(go[i]>=0) close(go[i]);
        int got=(pid[i]>0&&WIFEXITED(st))?WEXITSTATUS(st):-1;
        checks++; if(got!=C[i].want){ fails++; fprintf(stderr,"compat selftest FAIL: stuck-wait case %d (mode %d, %u ms x %u, %s): exit %d, expected %d\n",i,C[i].mode,C[i].slice_ms,C[i].slices,C[i].stop?"stopped 2.6 s":"never released",got,C[i].want); } }
    T(mono_ms()-t0>=2800 && kh_stuck_slice_ms==1000);                           /* the parent's own slice length is untouched */
}
int kh_compat_selftest(void){
    checks=fails=0;
    /* ---- condition with monotonic deadlines ---- */
    CvT c; pthread_mutex_init(&c.m,0); T(kh_cond_init(&c.cv)==0); c.flag=0;
    { /* nobody signals: the wait ends with ETIMEDOUT, and not before the monotonic deadline (a host that read the deadline as
         wall-clock time would return at once: CLOCK_MONOTONIC values lie decades in the past of CLOCK_REALTIME) */
      struct timespec dl; long long t0=mono_ms(); kh_mono_deadline_ms(&dl,120); int rc=0;
      pthread_mutex_lock(&c.m); while(!c.flag){ rc=kh_cond_wait_until(&c.cv,&c.m,&dl); if(rc==ETIMEDOUT) break; }
      long long left=kh_mono_left_ns(&dl), dt=mono_ms()-t0; T(pthread_mutex_unlock(&c.m)==0);          /* the mutex is held again on return */
      T(rc==ETIMEDOUT); T(left<=0); T(dt>=119 && dt<8000); }
    { /* signalled before the deadline: returns 0 with the predicate true, long before the 8 s deadline */
      pthread_t th; c.flag=0; c.delay_ms=40; struct timespec dl; long long t0=mono_ms(); kh_mono_deadline_ms(&dl,8000); int rc=0;
      pthread_mutex_lock(&c.m); T(pthread_create(&th,0,cv_signaller,&c)==0);
      while(!c.flag){ rc=kh_cond_wait_until(&c.cv,&c.m,&dl); if(rc==ETIMEDOUT) break; }
      long long dt=mono_ms()-t0; int flag=c.flag; pthread_mutex_unlock(&c.m); pthread_join(th,0);
      T(rc==0 && flag==1); T(dt>=39 && dt<7000); }
    { /* a deadline that has passed: ETIMEDOUT at once, mutex held */
      struct timespec dl; kh_mono_now(&dl); dl.tv_sec-=5; long long t0=mono_ms();
      pthread_mutex_lock(&c.m); int rc=kh_cond_wait_until(&c.cv,&c.m,&dl); T(pthread_mutex_unlock(&c.m)==0);
      T(rc==ETIMEDOUT); T(mono_ms()-t0<2000); }
    pthread_cond_destroy(&c.cv); pthread_mutex_destroy(&c.m);
    /* ---- sleep to an absolute monotonic time ---- */
    { struct timespec dl; long long t0=mono_ms(); kh_mono_deadline_ms(&dl,80); kh_sleep_until(&dl);
      T(kh_mono_left_ns(&dl)<=0); long long dt=mono_ms()-t0; T(dt>=79 && dt<8000);
      t0=mono_ms(); kh_sleep_until(&dl); T(mono_ms()-t0<2000);                                         /* passed: returns at once */
      struct timespec a,b; kh_mono_deadline_ms(&a,0); kh_mono_deadline_ms(&b,2500); long long d=((long long)b.tv_sec-a.tv_sec)*1000000000ll+(b.tv_nsec-a.tv_nsec);
      T(d>=2500000000ll && d<2600000000ll && b.tv_nsec>=0 && b.tv_nsec<1000000000L); }                /* deadline arithmetic carries into seconds */
    /* ---- mutex with a timed lock ---- */
    { static kh_tmutex tm=KH_TMUTEX_INITIALIZER; MxT x; memset(&x,0,sizeof x); x.tm=&tm;
      T(kh_tmutex_trylock(&tm)==0); T(kh_tmutex_trylock(&tm)==EBUSY); T(kh_tmutex_unlock(&tm)==0);
      T(kh_tmutex_lock_timeout_ms(&tm,1000)==0); T(kh_tmutex_unlock(&tm)==0);                          /* free: taken at once */
      pthread_t th; T(pthread_create(&th,0,mx_holder,&x)==0);
      for(int i=0;i<20000&&!atomic_load(&x.holding);i++) ms_sleep(1);
      T(atomic_load(&x.holding)==1 && atomic_load(&x.rc)==0); T(kh_tmutex_trylock(&tm)==EBUSY);
      long long t0=mono_ms(); int rc=kh_tmutex_lock_timeout_ms(&tm,150); long long dt=mono_ms()-t0;   /* held by the other thread for the whole wait */
      T(rc==ETIMEDOUT); T(dt>=145 && dt<8000); T(atomic_load(&x.released)==0);
      atomic_store(&x.release,1);                                                                      /* now the holder lets go: the waiter gets the lock, and only after the unlock */
      rc=kh_tmutex_lock_timeout_ms(&tm,8000); T(rc==0); T(atomic_load(&x.released)==1);
      T(kh_tmutex_trylock(&tm)==EBUSY); T(kh_tmutex_unlock(&tm)==0); pthread_join(th,0);
      T(kh_tmutex_trylock(&tm)==0); kh_tmutex_unlock(&tm); }
    /* ---- pages ---- */
    { unsigned long pg=kh_host_page(); T(pg>=4096 && !(pg&(pg-1)) && pg==(unsigned long)sysconf(_SC_PAGESIZE));
      /* the reservation pattern of the loader and the arena: PROT_NONE, then commit a part, on this host's page size */
      size_t n=1u<<20; u8 *p=mmap(0,n,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|KH_MAP_NORESERVE,-1,0); T(p!=MAP_FAILED);
      if(p!=MAP_FAILED){ T(mprotect(p+pg,pg,PROT_READ|PROT_WRITE)==0); p[pg]=0x5a; p[2*pg-1]=0xa5; T(p[pg]==0x5a && p[pg+1]==0 && p[2*pg-1]==0xa5);
          /* replacing committed pages by fresh ones inside the mapping (the arena's release path off Linux): zero again */
          T(mmap(p+pg,pg,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void*)(p+pg)); T(p[pg]==0 && p[2*pg-1]==0);
          T(munmap(p,n)==0); } }
    stuck_selftest();
    fprintf(stderr,"kh: compat selftest: %d checks, %d failures (host page %lu bytes, %s timing primitives)\n",checks,fails,kh_host_page(),KH_DARWIN?"Darwin":"POSIX clock");
    return fails;
}
