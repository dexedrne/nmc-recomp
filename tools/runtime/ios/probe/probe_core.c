/* tools/ios/probe/probe_core.c : what does this device give a process like khnmc?  (task 26; the measuring part, plain C)
 *
 * The questions the Mac mini cannot answer for an iPhone (implementation notes section 5, implementation notes "unproven"):
 *   1. address space: does one contiguous PROT_NONE reservation of 12 GiB succeed (runtime/loader.c: guest_space_size)?  If not:
 *      the largest one that does, whether the two zones could be had as two ranges (1,856 MiB + 10,432 MiB), and how much
 *      address space can be reserved in total;
 *   2. memory: how much can be committed and touched before the system objects (the field route touches about 912 MiB): the
 *      footprint and what the system says is left, step by step.  THIS PART CAN GET THE APP KILLED (jetsam); every step is
 *      written and synced to the output file BEFORE it is taken, so the last line of the file is the answer;
 *   3. threads: one 128 MiB stack and 58 stacks of 48 MiB alive at the same time (what a route creates);
 *   4. the small facts: page size, mincore, fork, where temporary files can go, how long a 16.67 ms sleep really takes.
 * Output: lines `probe: <key>: <value>` in OUT (and on stderr).  Nothing is read except the clock and the process's own state;
 * nothing is written except OUT and one scratch file in TMPDIR, which is removed.
 *
 *   int khnmc_probe(const char *out_path, long touch_mib);     touch_mib: how far part 2 goes (0 = skip it)
 *
 * This file has RUN on Linux x86-64 and on arm64 macOS (tools/ios/probe/probe_host.c: `probe_host OUT TOUCH_MIB`), so its logic
 * is tested; the iOS-only calls (os_proc_available_memory) are compiled only for iOS and are UNRUN, like the app around it
 * (tools/ios/probe/probe_main.m). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define MIB (1024ull*1024ull)
#define GUEST_SPACE (0x300000000ull)                 /* 12 GiB: runtime/loader.c guest_space_size */
#define ZONE_IMAGE_ID (1856ull*MIB)                  /* image + id zone: must stay contiguous (task 17) */
#define ZONE_FAR (10432ull*MIB)

static int out_fd=-1;
static void say(const char *fmt,...){
    char b[512]; va_list ap; va_start(ap,fmt); int n=vsnprintf(b,sizeof b-1,fmt,ap); va_end(ap);
    if(n<0) return;
    if(n>(int)sizeof b-2) n=(int)sizeof b-2;
    b[n++]='\n';
    if(out_fd>=0){ if(write(out_fd,b,(size_t)n)<0){} fsync(out_fd); }
    if(write(2,b,(size_t)n)<0){}
}
/* KHNMC_PROBE_PRETEND_LIMIT_MIB=n (desk machines only: to run the "12 GiB refused" branch where 12 GiB is granted): a single
 * reservation above n MiB is refused as the system would refuse it */
static unsigned long long pretend_limit;
static void *reserve(unsigned long long size){
    if(pretend_limit&&size>pretend_limit){ errno=ENOMEM; return NULL; }
    void *p=mmap(0,(size_t)size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0); return p==MAP_FAILED?NULL:p; }
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (double)ts.tv_sec+(double)ts.tv_nsec/1e9; }

/* footprint of this process and what the system says is left, in MiB (-1: the host does not tell) */
static void mem_now(long long *footprint,long long *left){
    *footprint=-1; *left=-1;
#ifdef __APPLE__
    task_vm_info_data_t vi; mach_msg_type_number_t cnt=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vi,&cnt)==KERN_SUCCESS) *footprint=(long long)(vi.phys_footprint/MIB);
#if TARGET_OS_IPHONE
    *left=(long long)(os_proc_available_memory()/MIB);
#endif
#else
    FILE *f=fopen("/proc/self/statm","r"); long a=0,r=0; if(f){ if(fscanf(f,"%ld %ld",&a,&r)==2) *footprint=(long long)r*sysconf(_SC_PAGESIZE)/(long long)MIB; fclose(f); }
#endif
}

static pthread_mutex_t tm=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t tc=PTHREAD_COND_INITIALIZER; static int t_go, t_up;
static void *parked(void *a){ (void)a; pthread_mutex_lock(&tm); t_up++; while(!t_go) pthread_cond_wait(&tc,&tm); pthread_mutex_unlock(&tm); return NULL; }

int khnmc_probe(const char *out_path,long touch_mib){
    out_fd=open(out_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(out_fd<0){ fprintf(stderr,"probe: cannot write %s: %s\n",out_path,strerror(errno)); return 1; }
    long pg=sysconf(_SC_PAGESIZE);
    say("probe: version: 1 (khnmc task 26)");
    { const char *e=getenv("KHNMC_PROBE_PRETEND_LIMIT_MIB"); pretend_limit=e&&*e?strtoull(e,NULL,10)*MIB:0;
      if(pretend_limit) say("probe: PRETENDING that no single reservation above %llu MiB is granted (KHNMC_PROBE_PRETEND_LIMIT_MIB): the address-space lines below are a test of the probe, not facts",pretend_limit/MIB); }
#if defined(__APPLE__) && TARGET_OS_IPHONE
    say("probe: host: iOS%s",TARGET_OS_SIMULATOR?" SIMULATOR (a macOS process: its answers are the Mac's, not an iPhone's)":" device");
#elif defined(__APPLE__)
    say("probe: host: macOS (not an iPhone: a test of this program only)");
#else
    say("probe: host: not Darwin (a test of this program only)");
#endif
    say("probe: pointer bits: %d, page size: %ld",(int)(sizeof(void*)*8),pg);
#ifdef __APPLE__
    { uint64_t ms=0; size_t l=sizeof ms; if(!sysctlbyname("hw.memsize",&ms,&l,NULL,0)) say("probe: physical memory: %llu MiB",(unsigned long long)(ms/MIB)); }
#endif
    { long long fp,left; mem_now(&fp,&left); say("probe: at start: footprint %lld MiB, the system says %lld MiB are left for this process (-1: not told)",fp,left); }
    { int local=0; void *m=malloc(64); say("probe: layout: code %p, stack %p, small malloc %p",(void*)(uintptr_t)&khnmc_probe,(void*)&local,m); free(m); }

    /* ---- 1. address space ---- */
    void *g=reserve(GUEST_SPACE);
    if(g) say("probe: reserve 12 GiB contiguous (PROT_NONE): OK at %p",g); else say("probe: reserve 12 GiB contiguous (PROT_NONE): FAILED: %s",strerror(errno));
    unsigned long long largest=g?GUEST_SPACE:0;
    if(g) munmap(g,(size_t)GUEST_SPACE);
    else {                                             /* largest single reservation, to 64 MiB */
        unsigned long long lo=0, hi=GUEST_SPACE;
        while(hi-lo>64*MIB){ unsigned long long mid=(lo+hi)/2/(64*MIB)*(64*MIB); void *p=reserve(mid); if(p){ munmap(p,(size_t)mid); lo=mid; } else hi=mid; }
        largest=lo; say("probe: largest contiguous reservation: %llu MiB (the runtime needs %llu; image + id zone alone need %llu)",largest/MIB,GUEST_SPACE/MIB,ZONE_IMAGE_ID/MIB);
        void *a=reserve(ZONE_IMAGE_ID), *b=a?reserve(ZONE_FAR):NULL;
        say("probe: as two ranges: image + id zone %llu MiB: %s; then the far zone %llu MiB: %s",ZONE_IMAGE_ID/MIB,a?"OK":"FAILED",ZONE_FAR/MIB,b?"OK":"FAILED");
        if(b) munmap(b,(size_t)ZONE_FAR);
        if(a) munmap(a,(size_t)ZONE_IMAGE_ID);
    }
    { enum { MAXC=64 }; void *c[MAXC]; int n=0; while(n<MAXC&&(c[n]=reserve(1024*MIB))) n++;
      say("probe: total reservable in 1 GiB pieces: %d GiB%s",n,n==MAXC?" (stopped at the probe's own limit)":""); while(n) munmap(c[--n],(size_t)(1024*MIB)); }

    /* ---- 4a. small facts that need a mapping ---- */
    { size_t len=(size_t)(4*pg); unsigned char *p=mmap(0,len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
      if(p!=MAP_FAILED){ p[0]=1; unsigned char vec[4]={0,0,0,0}; int rc=mincore(p,len,(void*)vec);
          say("probe: mincore: %s; touched page %d, untouched page %d",rc?strerror(errno):"works",vec[0]&1,vec[1]&1);
          int rc2=mprotect(p+4096,4096,PROT_NONE); say("probe: mprotect of 4096 bytes at a 4096-byte offset: %s",rc2?strerror(errno):"accepted");
          munmap(p,len); } }

    /* ---- 3. threads: 1 x 128 MiB + 58 x 48 MiB stacks alive together ---- */
    { enum { NT=59 }; pthread_t th[NT]; int made=0; t_go=0; t_up=0;
      for(int i=0;i<NT;i++){ pthread_attr_t at; pthread_attr_init(&at); int rs=pthread_attr_setstacksize(&at,(size_t)((i?48:128)*MIB));
          if(rs||pthread_create(&th[i],&at,parked,NULL)){ say("probe: threads: thread %d (%d MiB stack) FAILED: %s",i,i?48:128,strerror(rs?rs:errno)); break; } made++; }
      for(int k=0;k<2000;k++){ pthread_mutex_lock(&tm); int up=t_up; pthread_mutex_unlock(&tm); if(up==made) break; struct timespec ts={0,1000000}; nanosleep(&ts,NULL); }
      say("probe: threads: %d of %d with large stacks alive together (1 x 128 MiB + 58 x 48 MiB = 2,912 MiB of stack address space)",made,NT);
      pthread_mutex_lock(&tm); t_go=1; pthread_cond_broadcast(&tc); pthread_mutex_unlock(&tm);
      for(int i=0;i<made;i++) pthread_join(th[i],NULL); }

    /* ---- 4b. fork, temporary files, sleep ---- */
    { errno=0; pid_t p=fork(); if(p==0) _exit(0);
      if(p>0){ int st=0; waitpid(p,&st,0); say("probe: fork: WORKS here (child %d ended with status %d)",(int)p,st); } else say("probe: fork: refused: %s",strerror(errno)); }
    { const char *t=getenv("TMPDIR"); say("probe: TMPDIR: %s",t?t:"(not set)");
      char p[1100]; snprintf(p,sizeof p,"%s/khnmc-probe-XXXXXX",t&&*t?t:"/tmp"); int fd=mkstemp(p);
      say("probe: mkstemp in %s: %s",t&&*t?"TMPDIR":"/tmp",fd>=0?"works":strerror(errno)); if(fd>=0){ close(fd); unlink(p); }
      FILE *f=tmpfile(); say("probe: tmpfile(): %s",f?"works":strerror(errno)); if(f) fclose(f);
      say("probe: /tmp writable: %s",access("/tmp",W_OK)?"no":"yes"); }
    { double t0=now_s(); for(int i=0;i<60;i++){ struct timespec ts={0,16666667}; nanosleep(&ts,NULL); } double d=(now_s()-t0)/60.0*1000.0;
      say("probe: 60 sleeps of 16.667 ms took %.3f ms each on average",d); }

    /* ---- 2. commit and touch, step by step (last: it can end the process) ---- */
    if(touch_mib>0){
        unsigned long long want=(unsigned long long)touch_mib*MIB; if(largest&&want>largest) want=largest;
        unsigned char *base=largest?reserve(largest):NULL;
        if(!base) say("probe: touch: no reservation to touch in");
        else {
            const unsigned long long step=64*MIB; unsigned long long done=0;
            say("probe: touch: plan %llu MiB in 64 MiB steps inside a %llu MiB reservation; if the file ends before `touch: done`, the last line is where the system stopped the process",want/MIB,largest/MIB);
            while(done<want){
                long long fp,left; mem_now(&fp,&left);
                say("probe: touch: about to commit and write MiB %llu..%llu (footprint %lld MiB, left %lld MiB)",done/MIB,(done+step)/MIB,fp,left);
                if(mprotect(base+done,(size_t)step,PROT_READ|PROT_WRITE)){ say("probe: touch: mprotect FAILED at %llu MiB: %s",done/MIB,strerror(errno)); break; }
                for(unsigned long long o=0;o<step;o+=(unsigned long long)pg) base[done+o]=(unsigned char)(o>>12|1);
                done+=step;
            }
            long long fp,left; mem_now(&fp,&left);
            say("probe: touch: done: %llu MiB written, footprint %lld MiB, left %lld MiB",done/MIB,fp,left);
            munmap(base,(size_t)largest);
        }
    } else say("probe: touch: skipped (touch_mib 0)");
    say("probe: end");
    close(out_fd); out_fd=-1;
    return 0;
}
