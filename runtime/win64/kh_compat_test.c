/* kh_compat_test.c : selftest of the host primitives of kh_compat.h.  The same checks run on Linux and on Darwin; none of them
 * depends on how fast the machine is (lower bounds come from the monotonic clock itself, upper bounds are seconds wide, and the
 * ordering checks use flags, not times). */
#include "khnmc_rt.h"

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
    fprintf(stderr,"kh: compat selftest: %d checks, %d failures (host page %lu bytes, %s timing primitives)\n",checks,fails,kh_host_page(),KH_DARWIN?"Darwin":"POSIX clock");
    return fails;
}
