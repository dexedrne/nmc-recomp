#include "savestate.h"
/* bgload.c : the engine's background file-load request list: serialisation (port hardening), stress knob, fault diagnostics, selftest.
 *
 * What the original does (kh2fm.exe, read from its disassembly; nothing here is taken from lifted C):
 *   request node (0x60 bytes, from the allocator object at [0x1409ba888], a forwarder in front of a kn::MemoryAllocator):
 *   +0 u32 id of the next node (32-bit object id, see 4ad240 / 4ad270), +4 state (0 pending, 1 being read, 2 read), +8 name,
 *   +0x30 kind, +0x38 buffer, +0x40 callback, +0x48 argument, +0x50 size read, +0x58 / +0x5e read parameters.
 *   List head [0x142a0d628], tail [0x142a0d630].
 *     main thread   3a0aa0 / 3a0b30 / 3a0bd0 : allocate a node, 3a0710 links it in as the new head, then the caller fills +0x38..+0x5e
 *     main thread   3a0790 (a per-frame task): for every node in state 2: callback, unlink, free
 *     loader thread 3a0c60 (thread 16c9b0 -> 15ed20, THREAD_PRIORITY_ABOVE_NORMAL): forever { walk the list from the head, take the
 *                   pending node with the smallest name, state 1, read the file (3f99e0 / 3f9850), state 2 }; idle = 15edd0
 *   There is NO lock around the list in the original: no critical section, no interlocked operation.  kn::MemoryAllocator::free
 *   (19c470) overwrites the freed block with 0xefaccafe.  A loader walk that still holds a node the main thread frees then reads
 *   0xefaccafe as the next id; 4ad270 maps it to table slot 55 (never registered, -1) and returns -1; the walk faults on [-1+4] =
 *   address 3 in 3a0c60 (the fault of Task 9 batch1: rax=-1, rcx=0x1accafe = 0xefaccafe & 0x1ffffff, rdx=0x142b0d720).  The second
 *   window is the publication of a node before its fields are set.  On Windows the loader's higher priority keeps a walk from being
 *   descheduled; this runtime cannot raise a thread's priority, and a port to other hardware must not depend on timing anyway.
 *
 * What this file does: one host lock L makes the three list operations atomic against each other, with wrapper hooks only (the
 * lifted bodies run unchanged through orig_f_*):
 *   loader loop 3a0c60        holds L while it executes its own body (walk, pick, state stores) ...
 *   15edd0, 3f99e0, 3f9850, 3a64f0   ... and releases it around these four calls made from that loop (idle wait, the file read,
 *                             the post-read processing): every blocking or long step runs unlocked
 *   3a0aa0, 3a0b30, 3a0bd0    enqueue (allocation, link, field setup) under L: a walk never sees a half-initialised node
 *   19c470                    kn::MemoryAllocator::free under L: a block is overwritten only when no walk is in progress, and the
 *                             main thread has already unlinked it, so no later walk can reach it
 * L is never held across a wait on another thread, so it cannot deadlock; a wait for L longer than 30 s is a named fatal.
 * KHNMC_BGLOAD_LOCK=0 turns the serialisation off (the original behaviour, for demonstrating the race).
 * KHNMC_BGLOAD_CHAOS=n pauses the loader's walk at every n-th step for KHNMC_BGLOAD_CHAOS_US microseconds (default 3000), exactly
 * where a preemption would park it (after 4ad270 returned the next node, before the node is read): the stress variant of the route. */
#include "khnmc_rt.h"
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define BG_HEAD        (GS_BASE+0x2a0d628ull)
#define BG_TAIL        (GS_BASE+0x2a0d630ull)
#define BG_ALLOC       (GS_BASE+0x9ba888ull)      /* -> the node allocator object */
#define BG_GATE_OBJ    (GS_BASE+0x79cf00ull)      /* 3a0790 only completes nodes while (s32)[[this]+0x1290] < 0 */
#define KN_ALLOC_VTBL  (GS_BASE+0x5b2bb0ull)      /* kn::MemoryAllocator vtable: [1] 19c2b0 alloc, [2] 19c470 free */
#define FWD_ALLOC_VTBL (GS_BASE+0x5b13e8ull)      /* the 0x48-byte object [0x1409ba888] points to in the game: [1] 14f740 and [2] 14fc30 forward to the allocator at +8 */
#define KN_ALLOC_LOCK  (GS_BASE+0xac0fd8ull)      /* its semaphore object (created on first use) */
#define PTAB           (GS_BASE+0x2b0d720ull)     /* 64 bases of the 32-bit object ids */
#define PTAB_GUARD     (GS_BASE+0x2b0d920ull)     /* thread-safe-static guard of the table */
#define FREE_FILL      0xefaccafeu
#define RA_SCAN_STEP   (GS_BASE+0x3a0c8bull)      /* return site of the walk's 4ad270 call in 3a0c60 */
#define RA_IDLE        (GS_BASE+0x3a0d8cull)
#define RA_READ_A      (GS_BASE+0x3a0cf8ull)      /* 3f99e0 */
#define RA_READ_B      (GS_BASE+0x3a0cffull)      /* 3f9850 */
#define RA_POST        (GS_BASE+0x3a0d7bull)      /* 3a64f0 */

void orig_f_3a0c60(KCtx *g); void orig_f_15edd0(KCtx *g); void orig_f_3f99e0(KCtx *g); void orig_f_3f9850(KCtx *g); void orig_f_3a64f0(KCtx *g);
void orig_f_3a0aa0(KCtx *g); void orig_f_3a0b30(KCtx *g); void orig_f_3a0bd0(KCtx *g); void orig_f_19c470(KCtx *g); void orig_f_4ad270(KCtx *g);
void kh_register_fault_diag(void (*fn)(FILE*));

/* ---- the lock ---------------------------------------------------------------------------------------------------------------- */
typedef struct { int depth, is_loader; } BgTls;
static __thread BgTls bg_tl;
static __attribute__((noinline)) BgTls *bg_tls(void){ BgTls *t; __asm__ __volatile__("" ::: "memory"); t=&bg_tl; return t; }
static kh_tmutex bg_m=KH_TMUTEX_INITIALIZER;         /* a mutex with a timed lock (kh_compat.h): a blocked holder becomes a named fatal, not a hang */
static int bg_lock_on=1;
static _Atomic u32 bg_owner_tid;
static _Atomic u64 c_enq, c_free, c_released, c_contended, c_wait_ns_max, c_loaders, c_chaos, c_scan_steps;
#define BG_LOCK_WAIT_MAX_S 30
static void bg_lock(KCtx *g){
    if(!bg_lock_on) return;
    BgTls *t=bg_tls();
    if(t->depth++) return;
    if(kh_tmutex_trylock(&bg_m)!=0){
        atomic_fetch_add(&c_contended,1); u64 t0=kh_host_ns();
        int rc=kh_tmutex_lock_stuck(&bg_m,BG_LOCK_WAIT_MAX_S);                 /* in slices (kh_compat.h): a stopped process loses one slice, not the budget */
        if(rc) kh_fatal(g,"bgload: waited %d slices (about that many seconds of this process running) for the load-list lock (held by thread %u, pthread rc %d): a holder is blocked, which the design excludes",BG_LOCK_WAIT_MAX_S,atomic_load(&bg_owner_tid),rc);
        u64 w=kh_host_ns()-t0, m=atomic_load(&c_wait_ns_max); while(w>m && !atomic_compare_exchange_weak(&c_wait_ns_max,&m,w)){}
    }
    KThread *th=cur_thread(); atomic_store(&bg_owner_tid,th?th->tid:1);
}
static void bg_unlock(KCtx *g){
    if(!bg_lock_on) return;
    BgTls *t=bg_tls();
    if(t->depth<=0) kh_fatal(g,"bgload: unlock without lock");
    if(--t->depth) return;
    atomic_store(&bg_owner_tid,0); kh_tmutex_unlock(&bg_m);
}

/* ---- redirectable originals (the selftest replaces the loader's blocking calls) ------------------------------------------------- */
static KhFn o_idle=orig_f_15edd0, o_read_a=orig_f_3f99e0, o_read_b=orig_f_3f9850, o_post=orig_f_3a64f0;
static void (*scan_step_fn)(KCtx *g,u64 node);        /* called after every walk step of the loader with the node it moved to */
static int scan_hook_on;                               /* 4ad270 takes its slow path only when this is set */

/* a call made from the loader loop that blocks or runs long: without the lock */
static __thread int loader_idle;
int kh_cut_loader_safe(void){ return !bg_tls()->depth && (!bg_tls()->is_loader || loader_idle); }
void kh_cut_loader_relock(KCtx *g){
    if(!loader_idle || bg_tls()->depth) kh_fatal(g,"savestate-loader-relock: expected unlocked idle call");
    loader_idle=0;
    bg_lock(g);
}
static void released(KCtx *g,KhFn orig,u64 site){
    BgTls *t=bg_tls();
    if(bg_lock_on && t->is_loader && t->depth==1 && rd64(g,KH_RSP(g))==site){
        atomic_fetch_add(&c_released,1); bg_unlock(g); loader_idle=(site==RA_IDLE); orig(g); loader_idle=0; bg_lock(g);
    } else orig(g);
}
static void locked(KCtx *g,KhFn orig){ bg_lock(g); orig(g); bg_unlock(g); }
/* The free of a request node must end in kn::MemoryAllocator::free (19c470), the function hooked above.  In the game [0x1409ba888] is
 * a forwarding object (vtable 0x1405b13e8: alloc 14f740 = jmp [[this+8]]->[1], free 14fc30 = jmp [[this+8]]->[2]) in front of a
 * kn::MemoryAllocator.  Anything else is a named fatal: the serialisation would silently not cover the free. */
static _Atomic u64 c_alloc_checked;
static void check_node_allocator(KCtx *g){
    if(!bg_lock_on) return;
    u64 a=rd64(g,BG_ALLOC), first=a; int hops=0;
    while(a && GA_OK(a) && rd64(g,a)==FWD_ALLOC_VTBL && hops<4){ a=rd64(g,a+8); hops++; }
    if(!a || !GA_OK(a) || rd64(g,a)!=KN_ALLOC_VTBL)
        kh_fatal(g,"bgload: the request-node allocator [0x1409ba888]=%llx resolves (after %d forwarding objects) to %llx with vtable %llx, not kn::MemoryAllocator (0x1405b2bb0): its free is not covered by the 19c470 hook",
                 (unsigned long long)first,hops,(unsigned long long)a,(unsigned long long)((a&&GA_OK(a))?rd64(g,a):0));
    atomic_fetch_add(&c_alloc_checked,1);
}

/* ---- hooks ------------------------------------------------------------------------------------------------------------------- */
void f_3a0c60(KCtx *g){                                 /* loader loop; never returns */
    bg_tls()->is_loader=1; atomic_fetch_add(&c_loaders,1);
    bg_lock(g); orig_f_3a0c60(g);
    kh_fatal(g,"bgload: the loader loop 0x3a0c60 returned");
}
void f_15edd0(KCtx *g){ released(g,o_idle,RA_IDLE); }
void f_3f99e0(KCtx *g){ released(g,o_read_a,RA_READ_A); }
void f_3f9850(KCtx *g){ released(g,o_read_b,RA_READ_B); }
void f_3a64f0(KCtx *g){ released(g,o_post,RA_POST); }
void f_3a0aa0(KCtx *g){ check_node_allocator(g); atomic_fetch_add(&c_enq,1); locked(g,orig_f_3a0aa0); }
void f_3a0b30(KCtx *g){ check_node_allocator(g); atomic_fetch_add(&c_enq,1); locked(g,orig_f_3a0b30); }
void f_3a0bd0(KCtx *g){ check_node_allocator(g); atomic_fetch_add(&c_enq,1); locked(g,orig_f_3a0bd0); }
void f_19c470(KCtx *g){ atomic_fetch_add(&c_free,1); locked(g,orig_f_19c470); }   /* kn::MemoryAllocator::free (any instance, any thread) */

/* 4ad270 (object id -> pointer) is called from 2,460 sites; only the loader's walk step is of interest, and only when a stress
 * knob or the selftest asks for it.  The normal path is one load, one predicted branch and the original. */
static u32 chaos_n, chaos_us=3000;
static void chaos_step(KCtx *g,u64 node){
    (void)g; (void)node;
    u64 n=atomic_fetch_add(&c_scan_steps,1)+1;
    if(chaos_n && n%chaos_n==0){ atomic_fetch_add(&c_chaos,1); struct timespec ts={chaos_us/1000000u,(long)(chaos_us%1000000u)*1000L}; nanosleep(&ts,0); }
}
extern int kh_aspect_dk; void kh_aspect_dk_next(KCtx *g,u64 ra,u64 obj);
void f_4ad270(KCtx *g){
    if(__builtin_expect(kh_aspect_dk,0)){                                   /* task 40 / 42, KHNMC_ASPECT=fill or KHNMC_ASPECT_LOG=2: the dk draw loop's next object (runtime/aspect.c: H3b, M) */
        u64 ra=rd64(g,KH_RSP(g));
        if(__builtin_expect(scan_hook_on,0)){ orig_f_4ad270(g); if(ra==RA_SCAN_STEP && scan_step_fn) scan_step_fn(g,KH_RAX(g)); }
        else orig_f_4ad270(g);
        kh_aspect_dk_next(g,ra,KH_RAX(g)); return;
    }
    if(__builtin_expect(scan_hook_on,0)){
        u64 ra=rd64(g,KH_RSP(g)); orig_f_4ad270(g);
        if(ra==RA_SCAN_STEP && scan_step_fn) scan_step_fn(g,KH_RAX(g));
        return;
    }
    orig_f_4ad270(g);
}

/* ---- fault diagnostics: the list and the id table, read without trusting any pointer ----------------------------------------------- */
static int ok_range(u64 a,u64 n){ return GA_OK(a) && GA_OK(a+n-1); }
static u64 dq(u64 a){ u64 v; memcpy(&v,(void*)((uintptr_t)a+gs_bias),8); return v; }
static u32 dd(u64 a){ u32 v; memcpy(&v,(void*)((uintptr_t)a+gs_bias),4); return v; }
static u64 id_to_ptr_nofault(u32 id){ if(!id) return 0; u32 v=id&0x7fffffffu; return dq(PTAB+8ull*(v>>25))|(u64)(v&0x1ffffffu); }
static void bg_fault_diag(FILE *f){
    u64 head=dq(BG_HEAD), tail=dq(BG_TAIL);
    fprintf(f,"---- bgload: request list head=%llx tail=%llx lock=%s owner-thread=%u ----\n",(unsigned long long)head,(unsigned long long)tail,bg_lock_on?"on":"OFF",atomic_load(&bg_owner_tid));
    u64 p=head; int n=0;
    while(p && n<64){
        if(!ok_range(p,0x60)){ fprintf(f,"  node %llx: NOT a guest address (walk stops)\n",(unsigned long long)p); break; }
        u32 nx=dd(p), st=dd(p+4); char nm[41]; memcpy(nm,(void*)((uintptr_t)(p+8)+gs_bias),40); nm[40]=0; for(char *c=nm;*c;c++) if(*c<0x20||*c>0x7e) *c='?';
        fprintf(f,"  node %llx next-id=%08x state=%d kind=%d name='%s' buf=%llx cb=%llx arg=%llx size=%llx%s\n",(unsigned long long)p,nx,(int)st,(int)dd(p+0x30),nm,
                (unsigned long long)dq(p+0x38),(unsigned long long)dq(p+0x40),(unsigned long long)dq(p+0x48),(unsigned long long)dq(p+0x50),
                (nx==FREE_FILL||st==FREE_FILL)?"  <== FREED BLOCK (kn::MemoryAllocator free fill 0xefaccafe)":"");
        p=id_to_ptr_nofault(nx); n++;
    }
    if(!head) fprintf(f,"  (empty)\n");
    fprintf(f,"  id table (guard=%d):",(int)dd(PTAB_GUARD)); int used=0;
    for(int i=0;i<64;i++){ u64 b=dq(PTAB+8ull*i); if(b!=~0ull && b){ fprintf(f," [%d]=%llx",i,(unsigned long long)b); used++; } }
    fprintf(f,"%s; id 0xefaccafe decodes through slot %u = %llx\n",used?"":" no slot registered",(FREE_FILL&0x7fffffffu)>>25,(unsigned long long)dq(PTAB+8ull*((FREE_FILL&0x7fffffffu)>>25)));
}

/* ---- init / report ----------------------------------------------------------------------------------------------------------- */
void kh_bgload_init(void){
    const char *e=getenv("KHNMC_BGLOAD_LOCK"); if(e&&*e=='0') bg_lock_on=0;
    e=getenv("KHNMC_BGLOAD_CHAOS"); if(e&&*e) chaos_n=(u32)strtoul(e,0,0);
    e=getenv("KHNMC_BGLOAD_CHAOS_US"); if(e&&*e) chaos_us=(u32)strtoul(e,0,0);
    if(chaos_n){ scan_step_fn=chaos_step; scan_hook_on=1; }
    if(!bg_lock_on||chaos_n) fprintf(stderr,"kh: bgload: lock=%s chaos=every %u walk steps, %u us\n",bg_lock_on?"on":"OFF (original unsynchronised behaviour)",chaos_n,chaos_us);
    kh_register_fault_diag(bg_fault_diag);
}
void kh_bgload_report(FILE *f){
    fprintf(f,"kh: bgload report: lock=%s loader-threads=%llu enqueues=%llu (allocator verified %llu times) allocator-frees=%llu loader-unlocked-calls=%llu lock-contended=%llu max-lock-wait-us=%llu chaos-pauses=%llu\n",
            bg_lock_on?"on":"OFF",(unsigned long long)c_loaders,(unsigned long long)c_enq,(unsigned long long)c_alloc_checked,(unsigned long long)c_free,(unsigned long long)c_released,(unsigned long long)c_contended,
            (unsigned long long)(c_wait_ns_max/1000),(unsigned long long)c_chaos);
    /* the 32-bit object ids (4ad240 / 4ad270) index 64 bases of 32 MiB windows; 4ad390 has no overflow check: a 65th window would
     * alias slot 0 and every id of it would decode to a wrong address.  Report how close a run came. */
    int used=0; u64 lo=~0ull, hi=0; if(dd(PTAB_GUARD)){ for(int i=0;i<64;i++){ u64 b=dq(PTAB+8ull*i); if(b!=~0ull){ used++; if(b<lo) lo=b; if(b>hi) hi=b; } } }
    fprintf(f,"kh: object-id table: %d of 64 slots (32 MiB windows) in use%s",used,used>=64?"  <== FULL: a further window aliases slot 0 (wrong pointers)":used>=48?"  <== more than three quarters used":"");
    if(used) fprintf(f,", bases %llx..%llx",(unsigned long long)lo,(unsigned long long)hi); fputc('\n',f);
}

/* ==============================================================================================================================
 * selftest: the REAL lifted 3a0c60 (loader), 3a0b30 (enqueue), 3a0790 (completion), 19c2b0 / 19c470 (allocator) on a synthetic
 * list.  Only the loader's blocking calls (file read, idle wait) are test doubles.  The walk is parked on a node the main thread is
 * about to free: with the lock the free must wait and the node must still be intact when the walk resumes; with the lock off (the
 * original behaviour) the walk must follow the freed block's 0xefaccafe to a tripwire node the test registered under id-table slot 55
 * (unregistered in the game, where the same step is the fault at address 3).  The lock-off run is the proof that the test detects it.
 * ============================================================================================================================ */
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"bgload selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static struct {
    _Atomic int seq;                                   /* global event order */
    _Atomic u64 park_node; _Atomic int park_state2_only, park_ms;
    _Atomic int parked, park_begin_seq, park_end_seq, park_ended_by_go, go, node_intact_at_resume;
    _Atomic int trips, nloads, idle_calls, in_idle, wake, stop, read_gate, gate_from, in_read, read_gate_timeout;
    char loads[16][24];
    _Atomic int ncb; u64 cb_buf[16], cb_size[16], cb_arg[16];
} T;
static void msleep(int ms){ struct timespec ts={ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&ts,0); }
static int wait_for(_Atomic int *v,int want,int ms){ for(int i=0;i<ms*5;i++){ if(atomic_load(v)>=want) return 1; struct timespec ts={0,200000}; nanosleep(&ts,0); } return atomic_load(v)>=want; }
static void st_scan_step(KCtx *g,u64 node){
    if(!node || node!=atomic_load(&T.park_node)) return;
    if(atomic_load(&T.park_state2_only) && rd32(g,node+4)!=2) return;
    atomic_store(&T.park_node,0);
    atomic_store(&T.park_begin_seq,atomic_fetch_add(&T.seq,1)+1); atomic_store(&T.parked,1);
    int by_go=0; for(int i=0;i<atomic_load(&T.park_ms)*5;i++){ if(atomic_load(&T.go)){ by_go=1; break; } struct timespec ts={0,200000}; nanosleep(&ts,0); }
    atomic_store(&T.park_ended_by_go,by_go);
    atomic_store(&T.node_intact_at_resume,rd32(g,node)!=FREE_FILL && rd32(g,node+4)==2);
    atomic_store(&T.park_end_seq,atomic_fetch_add(&T.seq,1)+1);
}
static void st_read(KCtx *g){                          /* stands in for 3f99e0 / 3f9850: rcx = name, rdx = buffer; returns the size */
    const char *nm=gstr(g,A0(g)); int i=atomic_fetch_add(&T.nloads,1); if(i<16) snprintf(T.loads[i],sizeof T.loads[i],"%s",nm);
    if(!strcmp(nm,"TRIPWIRE")) atomic_fetch_add(&T.trips,1);
    if(atomic_load(&T.read_gate) && i>=atomic_load(&T.gate_from)){ atomic_store(&T.in_read,1); int ok=0; for(int k=0;k<25000;k++){ if(!atomic_load(&T.read_gate)){ ok=1; break; } struct timespec ts={0,200000}; nanosleep(&ts,0); } if(!ok) atomic_store(&T.read_gate_timeout,1); atomic_store(&T.in_read,0); }
    SET_RAX(g,0x10); KH_RET(g);
}
static void st_idle(KCtx *g){                          /* stands in for 15edd0: sleep until the test's "frame" wakes the loader */
    int mine=atomic_fetch_add(&T.idle_calls,1)+1; atomic_store(&T.in_idle,1);
    for(;;){ int stop=atomic_load(&T.stop); if(!stop && atomic_load(&T.wake)>=mine) break; struct timespec ts={0,stop?20000000:200000}; nanosleep(&ts,0); }   /* stop: parked for good */
    atomic_store(&T.in_idle,0); KH_RET(g);
}
static void st_cb(KCtx *g){ int i=atomic_fetch_add(&T.ncb,1); if(i<16){ T.cb_buf[i]=A0(g); T.cb_size[i]=A1(g); T.cb_arg[i]=A2(g); } KH_RET(g); }
static void st_loader_main(KCtx *g){ kh_call(g,GS_BASE+0x3a0c60,0,0,0,0); KH_RET(g); }
static u64 st_enqueue(KCtx *g,u64 strs,const char *name,u64 buf,u64 cb,u64 arg){
    u64 s=strs+0x40ull*(u64)(name[0]&0x1f); strcpy((char*)hp(g,s),name);
    return kh_call(g,GS_BASE+0x3a0b30,s,buf,cb,arg);
}
static int list_len(KCtx *g){ int n=0; for(u64 p=rd64(g,BG_HEAD);p&&n<100;n++) p=id_to_ptr_nofault(rd32(g,p)); return n; }

static int bg_scenario(KCtx *g,int lock,int *trips_out){
    int checks=0,fails=0;
    /* saved image state (the game is not initialised in --selftest: all of these are zero) */
    u64 sv_head=rd64(g,BG_HEAD), sv_tail=rd64(g,BG_TAIL), sv_alloc=rd64(g,BG_ALLOC), sv_gate=rd64(g,BG_GATE_OBJ), sv_alock=rd64(g,KN_ALLOC_LOCK);
    u32 sv_guard=rd32(g,PTAB_GUARD); u64 sv_tab[64]; memcpy(sv_tab,hp(g,PTAB),sizeof sv_tab);
    int sv_lock=bg_lock_on; KhFn sv_i=o_idle,sv_a=o_read_a,sv_b=o_read_b; void (*sv_step)(KCtx*,u64)=scan_step_fn; int sv_hook=scan_hook_on;
    u64 c0_cont=atomic_load(&c_contended), c0_rel=atomic_load(&c_released);
    memset(&T,0,sizeof T);
    /* id table as its initialiser leaves it; guard = INT_MIN so that every thread takes 4ad2c0's fast path */
    for(int i=0;i<64;i++) wr64(g,PTAB+8ull*i,~0ull); wr32(g,PTAB_GUARD,0x80000000u);
    /* tripwire: the node that id 0xefaccafe decodes to once slot 55 is registered */
    u64 region=ga_pages(0x4000000), tw=((region+0x1ffffffull)&~0x1ffffffull)+(FREE_FILL&0x1ffffffu);
    CHECK(region && tw+0x60<=region+0x4000000);
    wr64(g,PTAB+8ull*((FREE_FILL&0x7fffffffu)>>25),tw&~0x1ffffffull);
    memset(hp(g,tw),0,0x60); strcpy((char*)hp(g,tw+8),"TRIPWIRE");
    CHECK(id_to_ptr_nofault(FREE_FILL)==tw);
    /* a real kn::MemoryAllocator (19c3a0 builds one in a buffer) as the node allocator, the completion gate object, strings, buffers */
    u64 abuf=ga_alloc(0x8000,16), gate=ga_calloc(1,0x1300), strs=ga_calloc(1,0x800), bufs=ga_calloc(1,0x400);
    wr32(g,gate+0x1290,0xffffffffu); wr64(g,BG_GATE_OBJ,gate); wr64(g,tw+0x38,bufs+0x300);
    wr64(g,KN_ALLOC_LOCK,0);
    u64 alloc=kh_call(g,GS_BASE+0x19c3a0,abuf,0x8000,0,0);
    CHECK(alloc && rd64(g,alloc)==KN_ALLOC_VTBL && rd64(g,KN_ALLOC_LOCK)!=0);
    u64 fwd=ga_calloc(1,0x48); wr64(g,fwd,FWD_ALLOC_VTBL); wr64(g,fwd+8,alloc);     /* as in the game: 14f740 / 14fc30 forward to the allocator */
    wr64(g,BG_ALLOC,fwd); wr64(g,BG_HEAD,0); wr64(g,BG_TAIL,0);
    bg_lock_on=lock; o_idle=st_idle; o_read_a=st_read; o_read_b=st_read; scan_step_fn=st_scan_step; scan_hook_on=1;
    u64 cb=kh_host_fn("bgload_selftest_cb",st_cb);

    /* 1. two requests; the loader reads "a", walks again and is parked on the completed node A */
    u64 A=st_enqueue(g,strs,"a",bufs+0x00,cb,0xA11), B=st_enqueue(g,strs,"b",bufs+0x40,cb,0xB22);
    CHECK(A && B && rd64(g,BG_HEAD)==B && rd64(g,BG_TAIL)==A && id_to_ptr_nofault(rd32(g,B))==A && rd32(g,A)==0 && rd32(g,A+4)==0 && rd32(g,B+4)==0);
    CHECK(rd64(g,A+0x38)==bufs && rd64(g,A+0x40)==cb && rd64(g,A+0x48)==0xA11 && !strcmp(gstr(g,A+8),"a") && rd32(g,A+0x30)==0);
    atomic_store(&T.park_state2_only,1); atomic_store(&T.park_ms,lock?300:5000); atomic_store(&T.park_node,A);
    kh_spawn_hostfn_thread(g,"bgload_selftest_loader",st_loader_main,0);
    CHECK(wait_for(&T.parked,1,5000));
    CHECK(rd32(g,A+4)==2 && rd64(g,A+0x50)==0x10 && atomic_load(&T.nloads)==1 && !strcmp(T.loads[0],"a"));
    /* 2. the main thread's completion task: callback, unlink, FREE of A while the walk still holds A */
    kh_call(g,GS_BASE+0x3a0790,0,0,0,0);
    int free_seq=atomic_fetch_add(&T.seq,1)+1; atomic_store(&T.go,1);
    CHECK(atomic_load(&T.ncb)==1 && T.cb_buf[0]==bufs && T.cb_size[0]==0x10 && T.cb_arg[0]==0xA11);
    CHECK(rd32(g,A)==FREE_FILL && rd32(g,A+4)==FREE_FILL);                    /* the allocator's free fill is what the walk would read */
    CHECK(wait_for(&T.park_end_seq,1,6000));
    if(lock){
        CHECK(free_seq>atomic_load(&T.park_end_seq));                         /* the free waited for the walk */
        CHECK(!atomic_load(&T.park_ended_by_go) && atomic_load(&T.node_intact_at_resume)==1);
    } else {
        CHECK(free_seq<atomic_load(&T.park_end_seq) && atomic_load(&T.park_ended_by_go) && atomic_load(&T.node_intact_at_resume)==0);
    }
    /* 3. the loader finishes "b" (and, unlocked, the tripwire first), then idles; second completion pass empties the list */
    CHECK(wait_for(&T.idle_calls,1,5000));
    kh_call(g,GS_BASE+0x3a0790,0,0,0,0);
    int trips=atomic_load(&T.trips); *trips_out=trips;
    if(lock){
        CHECK(trips==0 && atomic_load(&T.nloads)==2 && !strcmp(T.loads[1],"b"));
        CHECK(atomic_load(&T.ncb)==2 && T.cb_buf[1]==bufs+0x40 && T.cb_size[1]==0x10 && T.cb_arg[1]==0xB22);
        CHECK(rd64(g,BG_HEAD)==0 && rd64(g,alloc+0x50)==0);                   /* list empty, every node returned to the allocator */
        CHECK(atomic_load(&c_contended)>c0_cont && atomic_load(&c_released)>c0_rel);

        /* 4. enqueue against a parked walk: it must wait as well (a walk never sees a half-built node) */
        u64 E=st_enqueue(g,strs,"e",bufs+0x80,cb,0xE55), F=st_enqueue(g,strs,"f",bufs+0xc0,cb,0xF66);
        CHECK(E && F && list_len(g)==2);
        atomic_store(&T.parked,0); atomic_store(&T.park_end_seq,0); atomic_store(&T.go,0); atomic_store(&T.park_state2_only,0); atomic_store(&T.park_ms,300); atomic_store(&T.park_node,E);
        atomic_store(&T.gate_from,3); atomic_store(&T.read_gate,1);          /* the read of "e" (load 2) passes, the next one ("f") is held open */
        atomic_fetch_add(&T.wake,1);                                          /* the frame's wake of the loader */
        CHECK(wait_for(&T.parked,1,5000));
        u64 G=st_enqueue(g,strs,"g",bufs+0x100,cb,0x677); int enq_seq=atomic_fetch_add(&T.seq,1)+1;
        CHECK(G && atomic_load(&T.park_end_seq) && enq_seq>atomic_load(&T.park_end_seq) && rd64(g,BG_HEAD)==G);
        /* 5. while the loader is inside a read (lock released) the main thread enqueues and completes (frees "e") without waiting for it */
        CHECK(wait_for(&T.in_read,1,10000));
        CHECK(rd32(g,E+4)==2 && rd32(g,F+4)==1 && rd32(g,G+4)==0);
        u64 t0=kh_host_ns(); u64 H=st_enqueue(g,strs,"h",bufs+0x140,cb,0x888); kh_call(g,GS_BASE+0x3a0790,0,0,0,0); u64 dt=kh_host_ns()-t0;
        CHECK(H && atomic_load(&T.in_read)==1 && dt<2000000000ull);           /* did not block behind the reading loader */
        CHECK(atomic_load(&T.ncb)==3 && T.cb_arg[2]==0xE55 && rd32(g,E)==FREE_FILL && list_len(g)==3);
        atomic_store(&T.read_gate,0);
        /* drain: frames until everything is read and completed */
        int done=0; for(int k=0;k<4000&&!done;k++){ atomic_fetch_add(&T.wake,1); msleep(1); kh_call(g,GS_BASE+0x3a0790,0,0,0,0); done=(rd64(g,BG_HEAD)==0); }
        CHECK(done && !atomic_load(&T.read_gate_timeout) && atomic_load(&T.trips)==0);
        CHECK(atomic_load(&T.nloads)==6 && atomic_load(&T.ncb)==6 && rd64(g,alloc+0x50)==0);
        int seen=0; for(int i=2;i<6;i++){ const char *l=T.loads[i]; if(l[0]>='e'&&l[0]<='h'&&!l[1]) seen|=1<<(l[0]-'e'); } CHECK(seen==15);
        CHECK(!strcmp(T.loads[2],"e") && !strcmp(T.loads[3],"f"));           /* smallest pending name first, as in the original */
        u64 args=0; for(int i=2;i<6;i++) args+=T.cb_arg[i]; CHECK(args==0xE55+0xF66+0x677+0x888);
    } else {
        CHECK(trips>=1);                                                      /* the original behaviour reads the freed node: the test has teeth */
        CHECK(atomic_load(&T.nloads)>=2 && !strcmp(T.loads[1],"TRIPWIRE"));
    }
    /* park the loader thread for good (it is inside the idle double or will reach it), restore the image state */
    atomic_store(&T.stop,1); CHECK(wait_for(&T.in_idle,1,5000)); msleep(2);
    bg_lock_on=sv_lock; o_idle=sv_i; o_read_a=sv_a; o_read_b=sv_b; scan_step_fn=sv_step; scan_hook_on=sv_hook;
    wr64(g,BG_HEAD,sv_head); wr64(g,BG_TAIL,sv_tail); wr64(g,BG_ALLOC,sv_alloc); wr64(g,BG_GATE_OBJ,sv_gate); wr64(g,KN_ALLOC_LOCK,sv_alock);
    memcpy(hp(g,PTAB),sv_tab,sizeof sv_tab); wr32(g,PTAB_GUARD,sv_guard);
    ga_pages_free(region,0x4000000);
    fprintf(stderr,"kh: bgload selftest scenario lock=%s: %d checks, %d failures, tripwire loads=%d\n",lock?"on":"off",checks,fails,trips);
    return fails<<16|checks;
}
int kh_bgload_selftest(KCtx *g){
    int t_on=0,t_off=0;
    int r1=bg_scenario(g,1,&t_on), r2=bg_scenario(g,0,&t_off);
    int checks=(r1&0xffff)+(r2&0xffff), fails=(r1>>16)+(r2>>16);
    fprintf(stderr,"kh: bgload selftest: %d checks, %d failures (freed-node reads: lock on %d, lock off %d)\n",checks,fails,t_on,t_off);
    return fails;
}
