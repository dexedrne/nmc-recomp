/* kh_compat.h : the host primitives that differ between Linux (glibc) and Darwin (macOS / iOS), in one place.
 *
 * Timing.  Every timed wait of the runtime is against CLOCK_MONOTONIC (immune to wall-clock steps).  Linux has the calls for
 * that; Darwin has no pthread_condattr_setclock, no clock_nanosleep / TIMER_ABSTIME and no pthread_mutex_timedlock, and its
 * pthread_cond_timedwait always reads the deadline as REALTIME (a monotonic deadline there lies decades in the past: the wait
 * returns at once).  So:
 *   kh_cond_init(cv)                    a condition whose kh_cond_wait_until deadlines are CLOCK_MONOTONIC
 *   kh_cond_wait_until(cv,m,deadline)   pthread_cond_timedwait with an ABSOLUTE CLOCK_MONOTONIC deadline; 0 or ETIMEDOUT
 *                                       (Darwin: pthread_cond_timedwait_relative_np with the time left).  cv must come from kh_cond_init.
 *   kh_mono_deadline_ms(ts,ms)          now + ms on CLOCK_MONOTONIC
 *   kh_sleep_until(deadline)            sleep to an absolute CLOCK_MONOTONIC time (returns at once when it has passed)
 *   kh_tmutex                           a mutex with a timed lock: kh_tmutex_trylock / kh_tmutex_lock_timeout_ms(m,ms) / kh_tmutex_unlock.
 *                                       Linux: a pthread mutex and pthread_mutex_timedlock, the calls the runtime made before this
 *                                       header existed.  Darwin: a flag under a mutex + condition, so a waiter sleeps until the
 *                                       unlock wakes it (no polling) and the timeout is monotonic.  Not recursive.
 * Memory.  KH_MAP_NORESERVE (0 where the flag does not exist: Darwin never reserves swap for untouched anonymous pages) and
 * kh_host_page() (4 KiB on x86-64 Linux, 16 KiB on arm64 macOS / iOS: every mprotect / mmap / munmap / madvise range must be a
 * multiple of it).
 * Selftest: runtime/kh_compat_test.c (`kh: compat selftest`), the same checks on both systems.
 */
#pragma once
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#ifdef __APPLE__
#define KH_DARWIN 1
#else
#define KH_DARWIN 0
#endif

/* ---- file systems ---- */
/* 1 where the host's file system may fold case (macOS APFS by default: stat() succeeds for any spelling of an existing name), so a
 * case-insensitive lookup has to read the directory to learn the stored spelling.  0: stat() answers for the exact name only. */
#if KH_DARWIN
#define KH_FS_MAY_FOLD_CASE 1
#else
#define KH_FS_MAY_FOLD_CASE 0
#endif

/* ---- memory ---- */
#ifdef MAP_NORESERVE
#define KH_MAP_NORESERVE MAP_NORESERVE
#else
#define KH_MAP_NORESERVE 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
static inline unsigned long kh_host_page(void){ static unsigned long pg; if(!pg) pg=(unsigned long)sysconf(_SC_PAGESIZE); return pg; }

/* ---- monotonic time ---- */
static inline void kh_mono_now(struct timespec *ts){ clock_gettime(CLOCK_MONOTONIC,ts); }
static inline void kh_mono_deadline_ms(struct timespec *ts,unsigned long long ms){
    clock_gettime(CLOCK_MONOTONIC,ts); ts->tv_sec+=(time_t)(ms/1000); ts->tv_nsec+=(long)(ms%1000)*1000000L;
    if(ts->tv_nsec>=1000000000L){ ts->tv_sec++; ts->tv_nsec-=1000000000L; } }
/* nanoseconds from now to the deadline (negative: passed) */
static inline long long kh_mono_left_ns(const struct timespec *deadline){
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return ((long long)deadline->tv_sec-(long long)now.tv_sec)*1000000000ll+((long long)deadline->tv_nsec-(long long)now.tv_nsec); }

/* ---- condition with monotonic deadlines ---- */
static inline int kh_cond_init(pthread_cond_t *cv){
#if KH_DARWIN
    return pthread_cond_init(cv,NULL);
#else
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a,CLOCK_MONOTONIC); int rc=pthread_cond_init(cv,&a); pthread_condattr_destroy(&a); return rc;
#endif
}
static inline int kh_cond_wait_until(pthread_cond_t *cv,pthread_mutex_t *m,const struct timespec *deadline){
#if KH_DARWIN
    for(;;){ long long left=kh_mono_left_ns(deadline); if(left<=0) return ETIMEDOUT;      /* ETIMEDOUT only once CLOCK_MONOTONIC has reached the deadline */
        struct timespec rel={(time_t)(left/1000000000ll),(long)(left%1000000000ll)};
        int rc=pthread_cond_timedwait_relative_np(cv,m,&rel); if(rc!=ETIMEDOUT) return rc; }
#else
    return pthread_cond_timedwait(cv,m,deadline);
#endif
}

/* ---- sleep to an absolute monotonic time ---- */
static inline void kh_sleep_until(const struct timespec *deadline){
#if KH_DARWIN
    for(;;){ long long left=kh_mono_left_ns(deadline); if(left<=0) return;
        struct timespec rel={(time_t)(left/1000000000ll),(long)(left%1000000000ll)}; nanosleep(&rel,NULL); }   /* EINTR or early return: the loop looks again */
#else
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,deadline,NULL)==EINTR){}
#endif
}

/* ---- mutex with a timed lock ---- */
#if KH_DARWIN
typedef struct { pthread_mutex_t m; pthread_cond_t cv; int locked, waiters; } kh_tmutex;
#define KH_TMUTEX_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 }
static inline int kh_tmutex_trylock(kh_tmutex *t){ pthread_mutex_lock(&t->m); int rc=t->locked?EBUSY:(t->locked=1,0); pthread_mutex_unlock(&t->m); return rc; }
static inline int kh_tmutex_lock_timeout_ms(kh_tmutex *t,unsigned ms){
    struct timespec dl; kh_mono_deadline_ms(&dl,ms); int rc=0;
    pthread_mutex_lock(&t->m); t->waiters++;
    while(t->locked){ if(kh_cond_wait_until(&t->cv,&t->m,&dl)==ETIMEDOUT && t->locked){ rc=ETIMEDOUT; break; } }
    t->waiters--; if(!rc) t->locked=1;
    pthread_mutex_unlock(&t->m); return rc; }
static inline int kh_tmutex_unlock(kh_tmutex *t){ pthread_mutex_lock(&t->m); t->locked=0; int w=t->waiters; pthread_mutex_unlock(&t->m); if(w) pthread_cond_signal(&t->cv); return 0; }
#else
typedef pthread_mutex_t kh_tmutex;
#define KH_TMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline int kh_tmutex_trylock(kh_tmutex *t){ return pthread_mutex_trylock(t); }
static inline int kh_tmutex_lock_timeout_ms(kh_tmutex *t,unsigned ms){
    struct timespec dl; clock_gettime(CLOCK_REALTIME,&dl); dl.tv_sec+=(time_t)(ms/1000); dl.tv_nsec+=(long)(ms%1000)*1000000L;
    if(dl.tv_nsec>=1000000000L){ dl.tv_sec++; dl.tv_nsec-=1000000000L; }
    return pthread_mutex_timedlock(t,&dl); }
static inline int kh_tmutex_unlock(kh_tmutex *t){ return pthread_mutex_unlock(t); }
#endif
