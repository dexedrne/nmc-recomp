/* core.c : guest arena allocator, tracing, fatal/fault reporting, dispatch (guest address -> function), host-fn registry,
 * handle table, guest-to-host calls, time, Windows path mapping.  Replaces the lifter's khrt_dispatch.c (see README, ABI notes). */
#include "khnmc_rt.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <ctype.h>

#define XLG(a) ((uintptr_t)(a)+gs_bias)

uintptr_t gs_host_base, gs_bias; u64 gs_size;
int kh_verbose;
char kh_root[1024] = ".", kh_docs[1024] = "./docs";
_Atomic u64 kh_cnt_gfx_calls, kh_cnt_audio_calls, kh_cnt_input_calls, kh_cnt_mf_calls, kh_cnt_steam_calls, kh_cnt_window_calls;
_Atomic int kh_gx_reached;

/* ======================================================================================================================
 * guest arena allocator
 *
 * Guest space is ONE reservation that starts at the image base (identity mode: host address = guest address 0x140000000; bias
 * mode: anywhere, guest addresses unchanged).  It is reserved PROT_NONE and committed on demand:
 *
 *   GS_BASE                 image (committed by the loader): 0x2c2b000 bytes = id windows 0 and 1
 *   arena_lo                ID ZONE : everything the game itself can hold a pointer to: its CRT heap (malloc family, HeapAlloc,
 *                           CoTaskMemAlloc), thread and fiber stacks, TEB / TLS, file buffers, runtime objects, enlarged pools
 *   GS_BASE + 58 windows    FAR ZONE: bulk data the game reaches only through an API and never turns into an object id
 *                           (the null D3D12 backend's buffer resources: driver / GPU memory on Windows)
 *   GS_BASE + gs_size       end of the reservation
 *
 * Why: the exe keeps pointers in 32-bit fields as ids, id = 0x80000000 | slot<<25 | (ptr & 0x1ffffff) (0x4ad240 / 0x4ad270).  The
 * slot indexes a table of 64 bases of 32 MiB windows (image RVA 0x2b0d720) that 0x4ad390 fills on first use, never empties and
 * never checks for overflow (idtable.c).  A window costs a slot for the rest of the run once ONE pointer into it was encoded.  The
 * image and the id zone are contiguous and start on a window boundary, so every valid pointer of the game lies in at most
 * GA_ID_WINDOWS (58) windows however long it plays, and 6 slots stay spare for values that are not pointers (the game encodes small
 * integers too: window base 0).  A full id zone is a named fatal here instead of a silently aliased id there.  Nothing depends on
 * the numeric value of an address (the scheme is the same above 4 GiB and with a host bias): only on [GS_BASE, id zone end) being
 * one window-aligned range of GUEST addresses.
 *
 * Inside a zone: blocks up to 256 KiB come from size-class free lists (16-byte header in front of the payload), refilled from the
 * zone's bump pointer; larger blocks and header-less page runs (stacks) are page runs, address-ordered first fit with coalescing;
 * a released run goes back to the host (MADV_DONTNEED) and is reused lowest address first.
 *
 * Knobs: KHNMC_HEAP_LAYOUT=legacy (one zone over the whole reservation, as before task 17; KH_DET=1 implies it, so the oracle
 * snapshot keeps its meaning), KHNMC_GA_LOG_BIG=n (log every allocation of n bytes or more with its source and the call site of the
 * import that asked for it), and the stress layout KHNMC_HEAP_SPREAD=1: small blocks come from "heap segments" of
 * KHNMC_HEAP_SPREAD_SEG bytes (default 320 KiB, the smallest that holds the largest small block), and segment number k is placed
 * in window k mod N of a region of N windows cut off the top of the far zone (N about 200), so that every new segment of small
 * blocks lies in another 32 MiB window; a block of KHNMC_HEAP_SPREAD_MIN bytes or more (default 1016 KiB) gets fresh windows of
 * the far zone while there are any.  Nothing in Windows forbids such a layout for the original (its heap grows by segments and
 * hands large blocks to VirtualAlloc one by one, each wherever the kernel finds room), but it is far more hostile than Windows
 * is in practice: with it the game needs a window per segment and must run into the id-table guard's named fatal instead of
 * aliasing ids (with 1 MiB segments the field route still fits: 59 of 64 slots).
 * ==================================================================================================================== */
#define HDR 16
#define MAGIC_LIVE 0xA110C8EDu
#define MAGIC_FREE 0xF4EEF4EEu
#define MAGIC_ALN  0xA11A11A1u
#define CLS_BIG 0xffffu
typedef struct { u16 cls; u8 src, zone; u32 magic; u64 size; } BHdr;    /* size = payload bytes (MAGIC_ALN: address of the enclosing payload) */
_Static_assert(sizeof(BHdr)==HDR,"guest block header");
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
#define NCLS 120
typedef struct Run { u64 lo, size; struct Run *next; } Run;             /* free page runs, address ordered */
typedef struct Zone {
    const char *name; u64 lo, hi;            /* the reserved guest range */
    u64 bump;                                 /* first address never handed out (it only grows: also the zone's high-water mark) */
    u64 commit;                               /* [lo,commit) is readable and writable, [commit,hi) is still PROT_NONE */
    u64 freelist[NCLS];                       /* singly linked: payload[0] = next */
    Run *runs;
    u64 live, parked, runfree, nblocks;       /* payload bytes in use; bytes parked in the size-class lists; bytes in free runs; live blocks and runs */
} Zone;
enum { ZONE_ID=0, ZONE_FAR=1, ZONE_TEST=2, MAXZONE=3 };
static Zone zones[MAXZONE]; static int nzones;
static u8 src_zone[GA_NSRC];
static u64 arena_lo, arena_hi;
static u64 *win_live, nwin;                   /* [window][source]: payload bytes */
static _Atomic u64 alloc_count;
int ga_deterministic, ga_no_stress;
static int ga_legacy, ga_spread; static u64 ga_spread_min=1016ull<<10, ga_spread_seg=320ull<<10, ga_log_big;
static _Atomic u64 ga_spread_blocks;
static u64 sp_lo, sp_wins, sp_count, seg_cur, seg_hi; static int sp_zone;   /* spread: the segment region, segments handed out, the rest of the current segment, the zone whose free lists take its blocks */
#define BIG_LIMIT (256u*1024u)
#define COMMIT_STEP (2ull<<20)
#define COMMIT_SLACK 0x10000ull                  /* committed beyond the last block: code that reads a few bytes past a buffer's end (string and SIMD loops) must not meet PROT_NONE */
static const char *const src_names[GA_NSRC]={"runtime","crt-heap","win-heap","stack","tls","file","gfx","audio","mapping","pool"};
const char *ga_src_name(int s){ return (s>=0&&s<GA_NSRC)?src_names[s]:"?"; }
static u64 rup(u64 v,u64 a){ return (v+a-1)&~(a-1); }
static int cls_of(u64 n, u64 *csz){                                      /* size class for payload n <= BIG_LIMIT */
    if(n<=256){ int c=(int)((n+15)/16); if(c==0)c=1; *csz=(u64)c*16; return c; }          /* classes 1..16 : 16..256 */
    int e=63-__builtin_clzll(n-1);                                        /* 2^e < n <= 2^(e+1) */
    u64 step=(1ull<<e)/8; u64 c=rup(n,step); *csz=c; return 17+(e-8)*8+(int)((c-(1ull<<e))/step-1);
}
static u64 cls_size(int c){ if(c<=16) return (u64)c*16; int e=8+(c-17)/8, k=(c-17)%8; return (1ull<<e)+(u64)(k+1)*((1ull<<e)/8); }
static void win_acct(u64 p,u64 n,int src,int add){                           /* gm held: payload [p,p+n) of a source, per 32 MiB window */
    while(n){ u64 off=p-GS_BASE, w=off>>GA_WIN_SHIFT, room=GA_WIN-(off&(GA_WIN-1)), k=n<room?n:room;
        if(w<nwin){ if(add) win_live[w*GA_NSRC+(u64)src]+=k; else win_live[w*GA_NSRC+(u64)src]-=k; } p+=k; n-=k; }
}
/* the host's answer to "make this part of the reservation usable": the only place that touches protections */
static void zone_commit(Zone *z,u64 upto){                               /* gm held */
    if(upto<=z->commit) return;
    u64 to=rup(upto,COMMIT_STEP); if(to>z->hi) to=z->hi;
    if(mprotect((void*)XLG(z->commit),to-z->commit,PROT_READ|PROT_WRITE))
        kh_fatal(NULL,"guest arena: the host refused to commit %llu bytes of the %s at guest 0x%llx (%s)",(unsigned long long)(to-z->commit),z->name,(unsigned long long)z->commit,strerror(errno));
    z->commit=to;
}
static u64 zone_take(Zone *z,u64 p,u64 n){                               /* gm held: [p,p+n) at or above the bump pointer; 0 = the zone is full */
    if(p<z->bump||p+n<p||p+n>z->hi) return 0;
    zone_commit(z,p+n+COMMIT_SLACK); z->bump=p+n; return p;
}
static void run_free(Zone *z,u64 lo,u64 size);
static u64 run_first_fit(Zone *z,u64 size){
    for(Run **pp=&z->runs;*pp;pp=&(*pp)->next){ Run *r=*pp; if(r->size>=size){ u64 p=r->lo; r->lo+=size; r->size-=size; if(!r->size){ *pp=r->next; free(r);} z->runfree-=size; return p; } }
    return 0;
}
static u64 run_alloc(Zone *z,u64 size,int own_window){                   /* gm held; size multiple of 4096 */
    u64 p;
    if(own_window){                                                      /* spread: whole fresh windows while there are any, the unused rest stays reusable */
        u64 w=rup(size,GA_WIN); p=w>=size?zone_take(z,rup(z->bump,GA_WIN),w):0;
        if(p){ run_free(z,p+size,w-size); return p; }
        return run_first_fit(z,size); }
    p=run_first_fit(z,size); return p?p:zone_take(z,rup(z->bump,4096),size);
}
static void run_free(Zone *z,u64 lo,u64 size){                           /* gm held */
    if(ga_deterministic||!size) return;
    madvise((void*)XLG(lo),size,MADV_DONTNEED);
    Run **pp=&z->runs; while(*pp && (*pp)->lo<lo) pp=&(*pp)->next;
    Run *n=(Run*)malloc(sizeof(Run)); n->lo=lo; n->size=size; n->next=*pp; *pp=n; z->runfree+=size;
    if(n->next && n->lo+n->size==n->next->lo){ Run *m=n->next; n->size+=m->size; n->next=m->next; free(m); }
    Run *prev=NULL; for(Run *r=z->runs;r&&r!=n;r=r->next) prev=r;
    if(prev && prev->lo+prev->size==n->lo){ prev->size+=n->size; prev->next=n->next; free(n); }
}
/* spread: small blocks are cut from heap segments; segment k lies in window k mod sp_wins of the segment region, at offset
 * (k div sp_wins) * segment size: consecutive segments never share a window, and the address needs no bookkeeping */
static void spread_setup(void){                                          /* gm held (or start-up): cut the region off the top of the last zone, once */
    if(sp_wins||ga_deterministic) return;
    Zone *z=&zones[nzones-1]; u64 freew=(z->hi-rup(z->bump+COMMIT_SLACK,GA_WIN))>>GA_WIN_SHIFT, w=freew*5/8; if(w>256) w=256; if(w<8) return;
    z->hi-=w<<GA_WIN_SHIFT; sp_lo=z->hi; sp_wins=w; sp_zone=nzones-1;
}
static u64 seg_take(u64 n){                                              /* gm held */
    if(seg_hi-seg_cur<n){
        if(!sp_wins||n>ga_spread_seg||sp_count>=sp_wins*(GA_WIN/ga_spread_seg)) return 0;
        u64 a=sp_lo+(sp_count%sp_wins)*GA_WIN+(sp_count/sp_wins)*ga_spread_seg;
        if(mprotect((void*)XLG(a),ga_spread_seg,PROT_READ|PROT_WRITE)) kh_fatal(NULL,"guest arena: the host refused to commit a heap segment at guest 0x%llx (%s)",(unsigned long long)a,strerror(errno));
        sp_count++; seg_cur=a; seg_hi=a+ga_spread_seg; atomic_fetch_add(&ga_spread_blocks,1);
    }
    u64 r=seg_cur; seg_cur+=n; return r;
}
static int spread_ok(u64 a){                                              /* a lies in a segment that was handed out */
    if(!sp_wins||a<sp_lo||a>=sp_lo+(sp_wins<<GA_WIN_SHIFT)) return 0;
    u64 off=a-sp_lo; return ((off&(GA_WIN-1))/ga_spread_seg)*sp_wins+(off>>GA_WIN_SHIFT)<sp_count;
}
static void zone_set(Zone *z,const char *name,u64 lo,u64 hi){ memset(z,0,sizeof *z); z->name=name; z->lo=z->bump=z->commit=lo; z->hi=hi; }
/* lo = first guest address after the image (the loader has committed [GS_BASE, lo)), hi = end of the reservation */
void ga_init(u64 lo, u64 hi){
    const char *e;
    arena_lo=lo; arena_hi=hi;
    if((e=getenv("KHNMC_HEAP_LAYOUT"))&&!strcmp(e,"legacy")) ga_legacy=1;
    if(ga_deterministic) ga_legacy=1;
    if((e=getenv("KHNMC_HEAP_SPREAD"))&&*e&&*e!='0'){ if(ga_no_stress) fprintf(stderr,"kh: guest arena: KHNMC_HEAP_SPREAD is ignored in --selftest (the selftest drives the stress placement itself)\n"); else ga_spread=1; }
    if((e=getenv("KHNMC_HEAP_SPREAD_MIN"))&&*e) ga_spread_min=strtoull(e,0,0);
    if((e=getenv("KHNMC_HEAP_SPREAD_SEG"))&&*e) ga_spread_seg=strtoull(e,0,0);
    ga_spread_seg=rup(ga_spread_seg<65536?65536:ga_spread_seg>GA_WIN?GA_WIN:ga_spread_seg,65536);
    if(ga_spread_min<=BIG_LIMIT) ga_spread_min=BIG_LIMIT+1;                 /* only page-run blocks can have a window of their own */
    if((e=getenv("KHNMC_GA_LOG_BIG"))&&*e) ga_log_big=strtoull(e,0,0);
    u64 id_hi=GS_BASE+(u64)GA_ID_WINDOWS*GA_WIN;
    memset(src_zone,ZONE_ID,sizeof src_zone);
    if(ga_legacy||id_hi+GA_WIN>hi){ zone_set(&zones[ZONE_ID],"arena (legacy: one zone)",lo,hi); nzones=1; ga_legacy=1; }
    else { zone_set(&zones[ZONE_ID],"id zone",lo,id_hi); zone_set(&zones[ZONE_FAR],"far zone",id_hi,hi); nzones=2; src_zone[GA_SRC_GFX]=ZONE_FAR; }
    nwin=(hi-GS_BASE+GA_WIN-1)>>GA_WIN_SHIFT; win_live=(u64*)calloc(nwin*GA_NSRC,sizeof(u64));
    if(ga_spread) spread_setup();
    if(ga_legacy||ga_spread||ga_log_big)
        fprintf(stderr,"kh: guest arena: layout=%s spread=%s (a window per heap segment of %llu bytes and per block >= %llu bytes) log-big=%llu\n",ga_legacy?"legacy (one zone)":"id zone + far zone",ga_spread?"ON":"off",(unsigned long long)ga_spread_seg,(unsigned long long)ga_spread_min,(unsigned long long)ga_log_big);
}
int ga_ok(u64 a){
    if(a<GS_BASE) return 0; if(a<arena_lo) return 1;
    if(zones[ZONE_TEST].name&&a>=zones[ZONE_TEST].lo&&a<zones[ZONE_TEST].hi) return a<zones[ZONE_TEST].commit;
    if(sp_wins&&a>=sp_lo) return spread_ok(a);
    for(int i=0;i<nzones;i++) if(a>=zones[i].lo&&a<zones[i].hi) return a<zones[i].commit;
    return 0;
}
static Zone *zone_of_addr(u64 a){ for(int i=0;i<nzones;i++) if(a>=zones[i].lo&&a<zones[i].hi) return &zones[i]; return NULL; }
static int spread_block(u64 n){ return ga_spread && n>=ga_spread_min; }
static int zone_for(int src,u64 n){ if(src<0||src>=GA_NSRC) src=GA_SRC_RT; if(ga_spread&&sp_wins&&(n>=ga_spread_min||n<=BIG_LIMIT)) return sp_zone; return src_zone[src]; }   /* spread: segments and large blocks live where there are windows to burn */
/* A full zone.  The default is the named fatal; the selftest installs a recorder that returns (the allocation then yields 0). */
static void exhausted_fatal(Zone *z,u64 need,int src){
    kh_fatal(NULL,"guest arena: the %s (0x%llx..0x%llx, %llu MiB) is exhausted: a %s request of %llu bytes does not fit (in use %llu MiB in %llu blocks, "
                  "parked in size classes %llu MiB, free page runs %llu MiB, never used %llu MiB).%s",
             z->name,(unsigned long long)z->lo,(unsigned long long)z->hi,(unsigned long long)((z->hi-z->lo)>>20),ga_src_name(src),(unsigned long long)need,
             (unsigned long long)(z->live>>20),(unsigned long long)z->nblocks,(unsigned long long)(z->parked>>20),(unsigned long long)(z->runfree>>20),(unsigned long long)((z->hi-z->bump)>>20),
             z==&zones[ZONE_ID]&&!ga_legacy?"  The id zone is capped on purpose (image + id zone = 58 windows of 32 MiB): beyond it the game's 64-slot object-id table would alias (see idtable.c).":"");
}
static void (*exhausted_fn)(Zone *z,u64 need,int src)=exhausted_fatal;
static void set_hdr(u64 b,int cls,int src,int zi,u64 n){ BHdr *h=(BHdr*)XLG(b); h->cls=(u16)cls; h->src=(u8)src; h->zone=(u8)zi; h->magic=MAGIC_LIVE; h->size=n; }
static u64 alloc_locked(int zi,u64 n,int src){
    Zone *z=&zones[zi]; u64 p=0;
    if(n==0) n=1;
    if(ga_deterministic){ u64 b=zone_take(z,z->bump,HDR+rup(n,16)); if(b){ set_hdr(b,0,src,zi,n); p=b+HDR; } }
    else if(n<=BIG_LIMIT){
        u64 csz; int c=cls_of(n,&csz);
        p=z->freelist[c];
        if(p){ z->freelist[c]=*(u64*)XLG(p); z->parked-=csz; set_hdr(p-HDR,c,src,zi,n); }
        else { u64 b=(ga_spread&&sp_wins&&zi==sp_zone)?seg_take(HDR+csz):zone_take(z,z->bump,HDR+csz); if(b){ set_hdr(b,c,src,zi,n); p=b+HDR; } }
    } else {
        u64 b=run_alloc(z,rup(HDR+n,4096),spread_block(n));                 /* the payload is 16-aligned (HDR); the run size is derivable from n */
        if(b){ set_hdr(b,CLS_BIG,src,zi,n); p=b+HDR; if(spread_block(n)) atomic_fetch_add(&ga_spread_blocks,1); }
    }
    if(p){ win_acct(p,n,src,1); z->live+=n; z->nblocks++; }
    return p;
}
static __thread const char *tl_imp_name; static __thread u64 tl_imp_ra;   /* the import the calling thread is in (kh_trace_import) */
u64 ga_alloc_src(u64 n, u64 align, int src){
    u64 need=align<=16?n:n+align+HDR; int zi=zone_for(src,need); u64 r=0;
    if(need<n) need=~0ull;                                                  /* overflow: cannot fit */
    pthread_mutex_lock(&gm);
    if(need!=~0ull){
        u64 raw=alloc_locked(zi,need,src);
        if(raw && align>16){ u64 p=rup(raw+HDR,align); BHdr *h=(BHdr*)XLG(p-HDR); h->cls=0; h->src=(u8)src; h->zone=(u8)zi; h->magic=MAGIC_ALN; h->size=raw; r=p; }   /* size field = base payload address */
        else r=raw;
    }
    pthread_mutex_unlock(&gm);
    if(!r){ exhausted_fn(&zones[zi],need,src); return 0; }
    atomic_fetch_add(&alloc_count,1);
    if(ga_log_big && n>=ga_log_big)
        fprintf(stderr,"kh: ga: %s block of %llu bytes (align %llu) at 0x%llx in the %s, asked by %s from rva 0x%llx\n",ga_src_name(src),(unsigned long long)n,(unsigned long long)align,(unsigned long long)r,
                zones[zi].name,tl_imp_name?tl_imp_name:"the runtime",(unsigned long long)(tl_imp_ra-GS_BASE));
    return r;
}
static void free_locked(u64 p){
    BHdr *h=(BHdr*)XLG(p-HDR);
    if(h->magic==MAGIC_ALN){ u64 base=h->size; h->magic=0; free_locked(base); return; }
    if(h->magic!=MAGIC_LIVE||h->zone>=(zones[ZONE_TEST].name?MAXZONE:nzones)){ fprintf(stderr,"kh: ga_free of bad/double-freed block %llx (magic %x)\n",(unsigned long long)p,h->magic); abort(); }
    Zone *z=&zones[h->zone]; win_acct(p,h->size,h->src,0); z->live-=h->size; z->nblocks--;
    if(ga_deterministic){ h->magic=MAGIC_FREE; return; }
    if(h->cls==CLS_BIG){ u64 tot=rup(HDR+h->size,4096); h->magic=MAGIC_FREE; run_free(z,p-HDR,tot); return; }
    h->magic=MAGIC_FREE; *(u64*)XLG(p)=z->freelist[h->cls]; z->freelist[h->cls]=p; z->parked+=cls_size(h->cls);
}
void ga_free(u64 p){ if(!p) return; pthread_mutex_lock(&gm); free_locked(p); pthread_mutex_unlock(&gm); }
u64 ga_usable(u64 p){ BHdr *h=(BHdr*)XLG(p-HDR); if(h->magic==MAGIC_ALN){ BHdr *b=(BHdr*)XLG(h->size-HDR); return b->size-(p-h->size); } return h->size; }
u64 ga_realloc(u64 p,u64 n){
    if(!p) return ga_alloc_src(n,16,GA_SRC_CRT);
    if(n==0){ ga_free(p); return 0; }
    BHdr *h=(BHdr*)XLG(p-HDR); int src=h->src;
    u64 old=ga_usable(p);
    if(n<=old){
        if(h->magic==MAGIC_LIVE){ pthread_mutex_lock(&gm); Zone *z=&zones[h->zone]; win_acct(p,h->size,src,0); z->live-=h->size; h->size=n; win_acct(p,n,src,1); z->live+=n; pthread_mutex_unlock(&gm); }   /* keep the block, shrink its logical size */
        return p; }
    u64 q=ga_alloc_src(n,16,src); if(!q) return 0; memcpy((void*)XLG(q),(void*)XLG(p),old<n?old:n); ga_free(p); return q;
}
u64 ga_calloc_src(u64 n,u64 s,int src){ u64 t; if(__builtin_mul_overflow(n,s,&t)) return 0; u64 p=ga_alloc_src(t,16,src); if(p) memset((void*)XLG(p),0,t); return p; }
static void zero_run(u64 p,u64 size){
#ifdef __linux__
    if(p&&ga_deterministic) memset((void*)XLG(p),0,size);      /* Linux: fresh and madvise(DONTNEED)-released pages read as zero */
#else
    if(p) memset((void*)XLG(p),0,size);                        /* other hosts: zero explicitly */
#endif
}
u64 ga_pages_src(u64 size,int src){
    size=rup(size,4096); int zi=zone_for(src,size); Zone *z=&zones[zi];
    pthread_mutex_lock(&gm); u64 p=run_alloc(z,size,spread_block(size)); if(p){ win_acct(p,size,src,1); z->live+=size; z->nblocks++; if(spread_block(size)) atomic_fetch_add(&ga_spread_blocks,1); } pthread_mutex_unlock(&gm);
    if(!p){ exhausted_fn(z,size,src); return 0; }
    zero_run(p,size); return p;
}
/* a run whose START is a multiple of align: the lowest free run that contains such a piece, else the next aligned address above
 * the bump pointer; what lies in front of and behind the piece stays (or becomes) a free run */
u64 ga_pages_aligned(u64 size,u64 align,int src){
    size=rup(size,4096); if(align<4096) align=4096; int zi=src_zone[src]; Zone *z=&zones[zi]; u64 a=0;   /* an aligned pool is where its source belongs, also under the stress layout */
    pthread_mutex_lock(&gm);
    if(!ga_deterministic) for(Run **pp=&z->runs;*pp;pp=&(*pp)->next){ Run *r=*pp; u64 c=rup(r->lo,align);
        if(c>=r->lo && c+size>=c && c+size<=r->lo+r->size){ u64 lo=r->lo, sz=r->size; *pp=r->next; free(r); z->runfree-=sz; a=c; run_free(z,lo,a-lo); run_free(z,a+size,lo+sz-(a+size)); break; } }
    if(!a){ u64 p=rup(z->bump,4096), c=rup(p,align); if(c>=p && c+size>=c && zone_take(z,p,c+size-p)){ a=c; run_free(z,p,a-p); } }
    if(a){ win_acct(a,size,src,1); z->live+=size; z->nblocks++; }
    pthread_mutex_unlock(&gm);
    if(!a){ exhausted_fn(z,size+align,src); return 0; }
    zero_run(a,size); return a;
}
void ga_pages_free_src(u64 p,u64 size,int src){
    size=rup(size,4096); Zone *z=zone_of_addr(p); if(!z&&zones[ZONE_TEST].name&&p>=zones[ZONE_TEST].lo&&p<zones[ZONE_TEST].hi) z=&zones[ZONE_TEST];
    if(!z){ fprintf(stderr,"kh: ga_pages_free of %llx, which is not in the guest arena\n",(unsigned long long)p); abort(); }
    pthread_mutex_lock(&gm); win_acct(p,size,src,0); z->live-=size; z->nblocks--; run_free(z,p,size); pthread_mutex_unlock(&gm);
}
u64 ga_strdup_src(const char *s,int src){ size_t n=strlen(s)+1; u64 p=ga_alloc_src(n,16,src); if(p) memcpy((void*)XLG(p),s,n); return p; }
void ga_stats(u64 *lb,u64 *ac){ u64 l=0; pthread_mutex_lock(&gm); for(int i=0;i<nzones;i++) l+=zones[i].live; pthread_mutex_unlock(&gm); *lb=l; *ac=alloc_count; }
u64 ga_bump_ptr(void){ return zones[ZONE_ID].bump; }
u64 ga_arena_lo(void){ return arena_lo; }
int ga_zone_get(int zi,u64 *lo,u64 *hi,u64 *bump,u64 *commit,u64 *live){
    if(zi<0||zi>=nzones) return 0; pthread_mutex_lock(&gm); Zone *z=&zones[zi]; if(lo)*lo=z->lo; if(hi)*hi=z->hi; if(bump)*bump=z->bump; if(commit)*commit=z->commit; if(live)*live=z->live; pthread_mutex_unlock(&gm); return 1;
}
int ga_spread_on(void){ return ga_spread; }
int kh_ga_spread_test(int on){ pthread_mutex_lock(&gm); int was=ga_spread; if(on) spread_setup(); ga_spread=on&&sp_wins; pthread_mutex_unlock(&gm); return was; }      /* selftest of the stress placement */
u64 kh_ga_spread_seg(void){ return ga_spread_seg; }
/* reports also run from the fatal and signal paths, possibly with the allocator lock held by the faulting thread: never wait for it */
static int diag_lock(void){ return pthread_mutex_trylock(&gm)==0; }
static void diag_unlock(int got){ if(got) pthread_mutex_unlock(&gm); }
void ga_report(FILE *f){
    int got=diag_lock();
    for(int i=0;i<nzones;i++){ Zone *z=&zones[i];
        u64 w0=(z->lo-GS_BASE)>>GA_WIN_SHIFT, w1=z->bump>z->lo?((z->bump-1-GS_BASE)>>GA_WIN_SHIFT):w0;
        fprintf(f,"kh: guest arena: %-8s 0x%llx..0x%llx (%llu MiB): high-water 0x%llx = %llu MiB used of address space (windows %llu..%llu), committed %llu MiB, in use %llu MiB in %llu blocks, "
                  "parked in size classes %llu MiB, free page runs %llu MiB\n",z->name,(unsigned long long)z->lo,(unsigned long long)z->hi,(unsigned long long)((z->hi-z->lo)>>20),(unsigned long long)z->bump,
                (unsigned long long)((z->bump-z->lo)>>20),(unsigned long long)w0,(unsigned long long)w1,(unsigned long long)((z->commit-z->lo)>>20),(unsigned long long)(z->live>>20),(unsigned long long)z->nblocks,
                (unsigned long long)(z->parked>>20),(unsigned long long)(z->runfree>>20)); }
    diag_unlock(got);
    if(ga_spread) fprintf(f,"kh: guest arena: KHNMC_HEAP_SPREAD gave %llu heap segments and large blocks a window of their own\n",(unsigned long long)ga_spread_blocks);
}
void ga_window_sources(u64 base,char *out,size_t cap){
    size_t o=0; if(cap) out[0]=0; u64 w=(base-GS_BASE)>>GA_WIN_SHIFT; if(base<GS_BASE||w>=nwin) return;
    int got=diag_lock();
    for(int s=0;s<GA_NSRC&&o+32<cap;s++){ u64 b=win_live[w*GA_NSRC+(u64)s]; if(!b) continue;
        if(b>=(1ull<<20)) o+=(size_t)snprintf(out+o,cap-o,"%s%s %.1fM",o?", ":"",src_names[s],(double)b/1048576.0); else o+=(size_t)snprintf(out+o,cap-o,"%s%s %lluK",o?", ":"",src_names[s],(unsigned long long)((b+1023)>>10)); }
    diag_unlock(got);
}
/* What is at a guest address?  For reports and fatals only: a heap block is found by scanning back for its header (a big block's
 * header sits on a page boundary, a small block's at most 256 KiB below), so block contents that imitate a header can mislead it. */
int kh_describe_stack(u64 a,char *out,size_t cap);     /* threads.c */
int ga_describe(u64 a,char *out,size_t cap){
    if(a<0x10000){ snprintf(out,cap,"not a pointer (small integer %llu)",(unsigned long long)a); return 0; }
    if(a<GS_BASE||a>=arena_hi){ const char *hn=kh_host_fn_name(a); if(hn) snprintf(out,cap,"runtime host function '%s' (synthetic address)",hn); else snprintf(out,cap,"outside guest space"); return 0; }
    if(a<arena_lo){ u64 r=a-GS_BASE;
        if(r>=0xbf33c0&&r<0x29f33c0) snprintf(out,cap,"image .bss: the game's static cache pool (30 MiB at rva 0xbf33c0) +0x%llx",(unsigned long long)(r-0xbf33c0));
        else if(r>=0x2a254c0&&r<0x2ae54c0) snprintf(out,cap,"image .bss: the game's static data pool (768 KiB at rva 0x2a254c0) +0x%llx",(unsigned long long)(r-0x2a254c0));
        else snprintf(out,cap,"image %s rva 0x%llx",r<0x1000?"header":r<0x579000?".text":r<0x57b000?".nep":r<0x713000?".rdata":r<0x79d000?".data":r<0x2b83000?".bss":r<IMG_SIZE?"(.pdata and later)":"(gap after the image)",(unsigned long long)r);
        return 1; }
    if(kh_describe_stack(a,out,cap)) return 1;
    if(sp_wins&&a>=sp_lo&&a<sp_lo+(sp_wins<<GA_WIN_SHIFT)){                                         /* a heap segment of the stress layout: small blocks only */
        if(!spread_ok(a)){ snprintf(out,cap,"stress-layout segment region, not handed out"); return 1; }
        u64 base=sp_lo+((a-sp_lo)&~(GA_WIN-1))+(((a-sp_lo)&(GA_WIN-1))/ga_spread_seg)*ga_spread_seg;
        for(u64 q=a&~15ull;q>=base+HDR;q-=16){ BHdr *h=(BHdr*)XLG(q-HDR); if(h->src>=GA_NSRC||h->cls<1||h->cls>=NCLS||a>=q+cls_size(h->cls)) continue;
            if(h->magic==MAGIC_LIVE||h->magic==MAGIC_FREE){ snprintf(out,cap,"stress-layout heap segment 0x%llx: %s%s block 0x%llx (%llu bytes) +0x%llx",(unsigned long long)base,h->magic==MAGIC_FREE?"FREED ":"",src_names[h->src],(unsigned long long)q,(unsigned long long)h->size,(unsigned long long)(a-q)); return 1; } }
        snprintf(out,cap,"stress-layout heap segment 0x%llx, no block header found",(unsigned long long)base); return 1; }
    Zone *z=zone_of_addr(a); if(!z){ snprintf(out,cap,"guest space, no zone"); return 1; }
    if(a>=z->bump){ snprintf(out,cap,"%s, %s",z->name,a>=z->commit?"not committed (never allocated)":"above the high-water mark (never allocated)"); return 1; }
    int got=diag_lock(), found=0;
    for(u64 q=a&~0xfffull, n=0; q>=z->lo && n<(1ull<<18) && !found; q-=4096, n++){              /* big blocks: up to 1 GiB back */
        BHdr *h=(BHdr*)XLG(q); if(h->magic==MAGIC_LIVE&&h->cls==CLS_BIG&&h->src<GA_NSRC&&a<q+HDR+h->size){
            snprintf(out,cap,"%s: %s block 0x%llx (%llu bytes) +0x%llx",z->name,src_names[h->src],(unsigned long long)(q+HDR),(unsigned long long)h->size,(unsigned long long)(a-q-HDR)); found=1; }
        if(q<z->lo+4096) break; }
    for(u64 q=(a&~15ull), n=0; q>=z->lo+HDR && n<=(BIG_LIMIT+2*HDR)/16 && !found; q-=16, n++){   /* small blocks */
        BHdr *h=(BHdr*)XLG(q-HDR); if(h->src>=GA_NSRC) continue;
        u64 span=(h->cls>=1&&h->cls<NCLS)?cls_size(h->cls):(h->cls==0?rup(h->size,16):0); if(!span||a>=q+span) continue;
        if(h->magic==MAGIC_LIVE){ snprintf(out,cap,"%s: %s block 0x%llx (%llu bytes) +0x%llx",z->name,src_names[h->src],(unsigned long long)q,(unsigned long long)h->size,(unsigned long long)(a-q)); found=1; }
        else if(h->magic==MAGIC_FREE){ snprintf(out,cap,"%s: FREED %s block 0x%llx (class of %llu bytes) +0x%llx",z->name,src_names[h->src],(unsigned long long)q,(unsigned long long)span,(unsigned long long)(a-q)); found=1; } }
    diag_unlock(got);
    if(!found) snprintf(out,cap,"%s: no block header found below it (a free page run, a header-less page run, or released memory)",z->name);
    return 1;
}

/* ---- selftest: zones, commit on demand, alignment, growth, exhaustion, free / reuse, accounting ---------------------------------- */
static struct { int calls; Zone *z; u64 need; int src; } ex_rec;
static void exhausted_record(Zone *z,u64 need,int src){ ex_rec.calls++; ex_rec.z=z; ex_rec.need=need; ex_rec.src=src; }
static int readable(u64 a){ int fd[2]; if(pipe(fd)) return -1; ssize_t r=write(fd[1],(void*)XLG(a),1); int e=errno; close(fd[0]); close(fd[1]); return r==1?1:(e==EFAULT?0:-1); }   /* the kernel probes the page: no signal */
#define GCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"arena selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
int kh_ga_selftest(KCtx *g){
    int checks=0,fails=0; (void)g;
    Zone *idz=&zones[ZONE_ID];
    int sv_spread=ga_spread; ga_spread=0;                         /* the checks describe the normal placement */
    /* 1. layout: the image and the id zone are one window-aligned range of at most GA_ID_WINDOWS windows, the far zone follows */
    GCHECK((GS_BASE&(GA_WIN-1))==0 && arena_lo>=GS_BASE+IMG_SIZE && idz->lo==arena_lo);
    if(!ga_legacy){
        GCHECK(nzones==2 && idz->hi==GS_BASE+(u64)GA_ID_WINDOWS*GA_WIN && zones[ZONE_FAR].lo==idz->hi && zones[ZONE_FAR].hi==(sp_wins?sp_lo:arena_hi) && GA_ID_WINDOWS<64);
        /* 2. routing: what the game can encode is in the id zone, GPU buffer data in the far zone */
        static const int id_src[]={GA_SRC_RT,GA_SRC_CRT,GA_SRC_HEAP,GA_SRC_TLS,GA_SRC_FILE,GA_SRC_AUDIO,GA_SRC_POOL};
        for(unsigned i=0;i<sizeof id_src/sizeof*id_src;i++) for(int big=0;big<2;big++){ u64 n=big?(3u<<20):200;
            u64 p=ga_alloc_src(n,16,id_src[i]); GCHECK(p>=idz->lo && p+n<=idz->hi && ((p+n-1-GS_BASE)>>GA_WIN_SHIFT)<GA_ID_WINDOWS); ga_free(p); }
        { u64 s=ga_pages_src(1u<<20,GA_SRC_STACK), m=ga_pages_src(8192,GA_SRC_MAP); GCHECK(s>=idz->lo&&s<idz->hi&&m>=idz->lo&&m<idz->hi); ga_pages_free_src(s,1u<<20,GA_SRC_STACK); ga_pages_free_src(m,8192,GA_SRC_MAP); }
        { u64 p=ga_alloc_src(1000,256,GA_SRC_GFX), q=ga_alloc_src(5u<<20,256,GA_SRC_GFX); GCHECK(p>=zones[ZONE_FAR].lo&&p<zones[ZONE_FAR].hi&&q>=zones[ZONE_FAR].lo&&!(p&255)&&!(q&255)); ga_free(p); ga_free(q); }
    }
    /* 3. a private zone inside a block of the id zone: every later check runs there and cannot disturb the real zones.
     *    The block is first made PROT_NONE again so that commit on demand is really exercised. */
    const u64 TZ=64ull<<20; u64 blk=ga_pages_aligned(TZ,GA_WIN,GA_SRC_RT); GCHECK(blk && !(blk&(GA_WIN-1)));
    if(!blk||ga_deterministic){ ga_spread=sv_spread; fprintf(stderr,"kh: arena selftest: %d checks, %d failures%s\n",checks,fails,ga_deterministic?" (KH_DET: zone tests skipped)":""); return fails; }
    GCHECK(readable(blk)==1 && rd64(g,blk)==0 && rd64(g,blk+TZ-8)==0);
    GCHECK(mprotect((void*)XLG(blk),TZ,PROT_NONE)==0);
    pthread_mutex_lock(&gm); Zone *t=&zones[ZONE_TEST]; zone_set(t,"selftest zone",blk,blk+TZ); void (*sv_ex)(Zone*,u64,int)=exhausted_fn; exhausted_fn=exhausted_record; pthread_mutex_unlock(&gm);
    #define TALLOC(n) ({ pthread_mutex_lock(&gm); u64 _p=alloc_locked(ZONE_TEST,(n),GA_SRC_RT); pthread_mutex_unlock(&gm); _p; })
    #define TRUN(n)   ({ pthread_mutex_lock(&gm); u64 _p=run_alloc(t,(n),0); if(_p){ t->live+=(n); t->nblocks++; } pthread_mutex_unlock(&gm); _p; })
    #define TRUNFREE(p,n) do{ pthread_mutex_lock(&gm); t->live-=(n); t->nblocks--; run_free(t,(p),(n)); pthread_mutex_unlock(&gm); }while(0)
    /* growth: nothing is committed before the first block; the commit boundary moves in 2 MiB steps with the bump pointer */
    GCHECK(t->commit==blk && readable(blk)==0 && !ga_ok(blk));
    u64 a=TALLOC(100); GCHECK(a==blk+HDR && t->commit==blk+COMMIT_STEP && t->bump==blk+HDR+112 && readable(a)==1 && ga_ok(a));
    GCHECK(readable(blk+COMMIT_STEP)==0 && !ga_ok(blk+COMMIT_STEP));                              /* the rest is still PROT_NONE */
    wr64(g,a,0x1122334455667788ull); GCHECK(rd64(g,a)==0x1122334455667788ull);
    u64 b5=TALLOC(5u<<20); GCHECK(b5 && ((b5-HDR)&4095)==0 && t->commit>=b5+(5u<<20) && t->commit<=rup(b5+(5u<<20),COMMIT_STEP) && readable(b5+(5u<<20)-1)==1);
    GCHECK((t->commit&(COMMIT_STEP-1))==(blk&(COMMIT_STEP-1)) && readable(t->commit)==0);
    memset((void*)XLG(b5),0x5a,5u<<20);
    /* alignment: every payload is 16-aligned; size classes round up by at most 12.5 %; ga_alloc_src honours larger alignments */
    for(u64 n=1;n<=BIG_LIMIT;n=n*3+1){ u64 csz; int c=cls_of(n,&csz); GCHECK(c>=1&&c<NCLS&&csz>=n&&cls_size(c)==csz&&(n<=256?csz<n+16:csz*8<=n*9+8)); u64 p=TALLOC(n); GCHECK(p&&!(p&15)); pthread_mutex_lock(&gm); free_locked(p); pthread_mutex_unlock(&gm); }
    { static const u64 al[]={16,64,256,4096,1u<<16,1u<<20}; for(unsigned i=0;i<sizeof al/sizeof*al;i++) for(int big=0;big<2;big++){ u64 n=big?(600u<<10):777; u64 p=ga_alloc_src(n,al[i],GA_SRC_RT);
        GCHECK(p&&!(p&(al[i]-1))&&ga_usable(p)>=n); memset(hp(g,p),0xee,n); ga_free(p); } }
    { u64 lv0=idz->live; u64 p=ga_pages_aligned(40ull<<20,GA_WIN,GA_SRC_POOL); GCHECK(p&&!(p&(GA_WIN-1))&&idz->live==lv0+(40ull<<20)&&rd64(g,p)==0&&rd64(g,p+(40ull<<20)-8)==0);
      char d[160]; ga_window_sources(p,d,sizeof d); GCHECK(strstr(d,"pool 32.0M")!=NULL); ga_window_sources(p+GA_WIN,d,sizeof d); GCHECK(strstr(d,"pool 8.0M")!=NULL);
      ga_pages_free_src(p,40ull<<20,GA_SRC_POOL); GCHECK(idz->live==lv0); ga_window_sources(p,d,sizeof d); GCHECK(!strstr(d,"pool")); }
    /* free / reuse: a freed small block is the next one of its class; a freed run is reused lowest address first and neighbours merge */
    u64 s1=TALLOC(300), s2=TALLOC(300); GCHECK(s1&&s2&&s1!=s2); u64 bump0=t->bump;
    pthread_mutex_lock(&gm); free_locked(s1); pthread_mutex_unlock(&gm); GCHECK(t->parked>=320);
    u64 s3=TALLOC(290); GCHECK(s3==s1 && t->bump==bump0);                                           /* same class (289..320): same block, no growth */
    u64 r1=TRUN(1u<<20), r2=TRUN(1u<<20), r3=TRUN(1u<<20); GCHECK(r1&&r2==r1+(1u<<20)&&r3==r2+(1u<<20)); u64 bump1=t->bump;
    memset((void*)XLG(r1),0x77,3u<<20);
    TRUNFREE(r1,1u<<20); TRUNFREE(r3,1u<<20); GCHECK(t->runfree==(2u<<20));
    u64 r4=TRUN(1u<<20); GCHECK(r4==r1 && *(u8*)XLG(r4)==0 && t->bump==bump1);                      /* lowest first; a reused run reads as zero */
    TRUNFREE(r4,1u<<20); TRUNFREE(r2,1u<<20); GCHECK(t->runs && t->runs->lo==r1 && t->runs->size==(3u<<20) && !t->runs->next);   /* three neighbours merged */
    u64 r5=TRUN(3u<<20); GCHECK(r5==r1 && t->bump==bump1 && t->runfree==0); TRUNFREE(r5,3u<<20);
    { u64 hw=0; int ok=1; for(int i=0;i<2000;i++){ u64 n=4096ull*(u64)(1+(i%30)*10); u64 p=TRUN(n), q=TALLOC(1+(u64)(i%50)*97); if(!p||!q){ ok=0; break; } if(i==100) hw=t->bump; TRUNFREE(p,n); pthread_mutex_lock(&gm); free_locked(q); pthread_mutex_unlock(&gm); }
      GCHECK(ok && hw && t->bump==hw); }                                                           /* steady churn does not move the high-water mark */
    /* exhaustion: the zone is 64 MiB.  A request that cannot fit is reported (in production: a named fatal) and yields 0; nothing is damaged and the zone keeps working */
    u64 hole=TRUN(3u<<20); GCHECK(hole==r1 && !t->runs);                                             /* no free run is left: what follows comes from the top */
    ex_rec.calls=0; u64 left=t->hi-rup(t->bump,4096);
    u64 fill=TRUN(left-(1u<<20)); GCHECK(fill && ex_rec.calls==0);
    pthread_mutex_lock(&gm); u64 nofit=alloc_locked(ZONE_TEST,8u<<20,GA_SRC_CRT); pthread_mutex_unlock(&gm); GCHECK(nofit==0);
    { u64 lo0=idz->lo, hi0=idz->hi, bump_sv=idz->bump, commit_sv=idz->commit;                        /* the public entry on a zone made full for the moment */
      pthread_mutex_lock(&gm); idz->hi=idz->commit; u64 room=idz->hi-idz->bump; pthread_mutex_unlock(&gm);
      const u64 HUGE=1ull<<32; u64 big=ga_alloc_src(room+HUGE,16,GA_SRC_CRT);
      GCHECK(big==0 && ex_rec.calls==1 && ex_rec.z==idz && ex_rec.src==GA_SRC_CRT && ex_rec.need==room+HUGE && idz->bump==bump_sv && idz->commit==commit_sv);
      u64 pr=ga_pages_src(room+HUGE,GA_SRC_STACK); GCHECK(pr==0 && ex_rec.calls==2 && ex_rec.src==GA_SRC_STACK);
      u64 ov=ga_alloc_src(~0ull-8,4096,GA_SRC_CRT); GCHECK(ov==0 && ex_rec.calls==3);              /* size + alignment overflow */
      pthread_mutex_lock(&gm); idz->hi=hi0; pthread_mutex_unlock(&gm); GCHECK(idz->lo==lo0);
      u64 okp=ga_alloc_src(100,16,GA_SRC_CRT); GCHECK(okp!=0 && ex_rec.calls==3); ga_free(okp); }
    { u64 tail=TRUN(1u<<20); GCHECK(tail && t->bump==t->hi); u64 none=TRUN(4096); GCHECK(none==0);                 /* exactly full, then one page too many */
      TRUNFREE(tail,1u<<20); u64 again=TRUN(1u<<20); GCHECK(again==tail); TRUNFREE(again,1u<<20); }                  /* after a free it fits again */
    TRUNFREE(fill,left-(1u<<20)); TRUNFREE(hole,3u<<20);
    GCHECK(sv_ex==exhausted_fatal);                                                                 /* outside this test a full zone is the named fatal */
    /* accounting: everything of the private zone is released; hand the block back */
    pthread_mutex_lock(&gm); free_locked(a); free_locked(b5); free_locked(s2); free_locked(s3); pthread_mutex_unlock(&gm);
    GCHECK(t->live==0 && t->nblocks==0);
    pthread_mutex_lock(&gm); exhausted_fn=sv_ex; for(Run *r=t->runs;r;){ Run *n=r->next; free(r); r=n; } memset(t,0,sizeof *t); pthread_mutex_unlock(&gm);
    ga_spread=sv_spread;
    GCHECK(mprotect((void*)XLG(blk),TZ,PROT_READ|PROT_WRITE)==0); madvise((void*)XLG(blk),TZ,MADV_DONTNEED);
    ga_pages_free_src(blk,TZ,GA_SRC_RT);
    /* interior pointers are attributed to their block (after the private zone is gone: the scan reads the memory below an address) */
    { char d[200]; u64 p=ga_alloc_src(5000,16,GA_SRC_FILE), q=ga_alloc_src(700u<<10,16,GA_SRC_CRT), al=ga_alloc_src(3000,4096,GA_SRC_AUDIO);
      GCHECK(ga_describe(p+4999,d,sizeof d)==1&&strstr(d,"file block")&&strstr(d,"+0x1387")); GCHECK(ga_describe(q+(699u<<10),d,sizeof d)==1&&strstr(d,"crt-heap block")&&strstr(d,"(716800 bytes)"));
      GCHECK(ga_describe(al+10,d,sizeof d)==1&&strstr(d,"audio block")); ga_free(p); GCHECK(ga_describe(p+8,d,sizeof d)==1&&strstr(d,"FREED file block")); ga_free(q); ga_free(al);
      GCHECK(ga_describe(GS_BASE+0xbf33c0+0x40,d,sizeof d)==1&&strstr(d,"static cache pool")&&strstr(d,"+0x40")); GCHECK(ga_describe(GS_BASE+0x4ad390,d,sizeof d)==1&&strstr(d,".text"));
      GCHECK(ga_describe(7,d,sizeof d)==0&&strstr(d,"small integer")); GCHECK(ga_describe(0x7fff00000000ull,d,sizeof d)==0); GCHECK(ga_describe(KH_RSP(g),d,sizeof d)==1&&strstr(d,"stack")); }
    /* objects the game creates and drops every frame (command allocators and lists of the null D3D12 backend: COM_FREE) go back to
     * the arena at their last Release; classes without the flag keep the old behaviour (the object stays) */
    { extern u64 kh_com_freed(void); int cf=kh_com_class("selftest:freed-at-release",3,COM_FREE,NULL), ck=kh_com_class("selftest:kept",3,0,NULL);
      u64 o=kh_com_obj(cf,16), k=kh_com_obj(ck,16), vt=rd64(g,o), n0=kh_com_freed(), lv, lv2, na; ga_stats(&lv,&na);
      GCHECK(kh_call(g,rd64(g,vt+8),o,0,0,0)==2 && kh_call(g,rd64(g,vt+16),o,0,0,0)==1 && rd64(g,o)==vt && kh_com_freed()==n0);      /* AddRef, Release: still referenced */
      GCHECK(kh_call(g,rd64(g,vt+16),o,0,0,0)==0 && kh_com_freed()==n0+1 && rd64(g,o)!=vt); ga_stats(&lv2,&na); GCHECK(lv2==lv-48);   /* last Release: freed, vtable pointer gone */
      GCHECK(kh_com_obj(cf,16)==o);                                                                                                 /* the next object of that size reuses the block */
      kh_call(g,rd64(g,vt+16),o,0,0,0);
      u64 vk=rd64(g,k); GCHECK(kh_call(g,rd64(g,vk+16),k,0,0,0)==0 && rd64(g,k)==vk && kh_com_freed()==n0+2); ga_free(k); }
    fprintf(stderr,"kh: arena selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

const char *gstr(KCtx *g,u64 a){ return (const char*)XL(g,a); }
size_t gw_len(KCtx *g,u64 a){ size_t n=0; while(rd16(g,a+2*n)) n++; return n; }
void gw_to_utf8(KCtx *g,u64 a,char *out,size_t cap){
    size_t o=0; for(u64 p=a;;p+=2){ u32 c=rd16(g,p); if(!c) break;
        if(c>=0xD800&&c<0xDC00){ u32 d=rd16(g,p+2); if(d>=0xDC00&&d<0xE000){ c=0x10000+((c-0xD800)<<10)+(d-0xDC00); p+=2; } }
        if(c<0x80){ if(o+1>=cap) break; out[o++]=(char)c; }
        else if(c<0x800){ if(o+2>=cap) break; out[o++]=(char)(0xC0|(c>>6)); out[o++]=(char)(0x80|(c&63)); }
        else if(c<0x10000){ if(o+3>=cap) break; out[o++]=(char)(0xE0|(c>>12)); out[o++]=(char)(0x80|((c>>6)&63)); out[o++]=(char)(0x80|(c&63)); }
        else { if(o+4>=cap) break; out[o++]=(char)(0xF0|(c>>18)); out[o++]=(char)(0x80|((c>>12)&63)); out[o++]=(char)(0x80|((c>>6)&63)); out[o++]=(char)(0x80|(c&63)); } }
    if(cap) out[o<cap?o:cap-1]=0;
}
void utf8_to_gw(KCtx *g,u64 dst,size_t cap,const char *s){
    size_t o=0; const unsigned char *p=(const unsigned char*)s;
    while(*p && o+1<cap){ u32 c=*p++; if(c>=0xF0&&p[0]&&p[1]&&p[2]){ c=((c&7)<<18)|((p[0]&63)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; } else if(c>=0xE0&&p[0]&&p[1]){ c=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63); p+=2; } else if(c>=0xC0&&p[0]){ c=((c&31)<<6)|(p[0]&63); p++; }
        if(c>=0x10000){ if(o+2>=cap) break; c-=0x10000; wr16(g,dst+2*o++,(u16)(0xD800+(c>>10))); wr16(g,dst+2*o++,(u16)(0xDC00+(c&1023))); } else wr16(g,dst+2*o++,(u16)c); }
    if(cap) wr16(g,dst+2*o,0);
}

/* ======================================================================================================================
 * time
 * ==================================================================================================================== */
static _Atomic u64 det_tick;
/* oracle mode (KH_DET=1): every clock read returns the next value of a counter, so a run is reproducible and the Unicorn oracle can mirror it */
u64 kh_det_tick(void){ return atomic_fetch_add(&det_tick,1)+1; }
u64 kh_now_ns(void){ if(ga_deterministic) return 1000000000ull+kh_det_tick()*100000ull; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+(u64)ts.tv_nsec; }
u64 kh_qpc(void){ return kh_now_ns()/100; }

/* ======================================================================================================================
 * trace
 * ==================================================================================================================== */
typedef struct { u64 seq; u32 tid; const char *name; u64 a[4]; u64 ret; } TrEnt;
#define TRN 4096
static TrEnt tring[TRN]; static _Atomic u64 trseq;
static FILE *trf; static u64 trmax=~0ull; static int stop_at_gx; static pthread_mutex_t trm = PTHREAD_MUTEX_INITIALIZER;
static int trace_skip_sync;
u64 guest_return_addr(KCtx *g){ return rd64(g,KH_RSP(g)); }
static void tr_line(FILE *f,const TrEnt *e){
    u64 r=e->ret; fprintf(f,"%llu t%u %s(%llx,%llx,%llx,%llx) <- %llx\n",(unsigned long long)e->seq,e->tid,e->name,(unsigned long long)e->a[0],(unsigned long long)e->a[1],(unsigned long long)e->a[2],(unsigned long long)e->a[3],
                           (unsigned long long)((r>=GS_BASE&&r<GS_BASE+IMG_SIZE)?r-GS_BASE:r));
}
void kh_trace_init(void){
    const char *p=getenv("KH_TRACE"); if(p&&*p){ trf=fopen(p,"w"); }
    p=getenv("KH_TRACE_MAX"); if(p) trmax=strtoull(p,0,0);
    p=getenv("KH_STOP_AT_GX"); stop_at_gx=p&&*p=='1';
    p=getenv("KH_TRACE_NOSYNC"); trace_skip_sync=p&&*p=='1';
}
u64 kh_trace_count(void){ return trseq; }
void kh_trace_import(KCtx *g,const char *name){
    u64 s=atomic_fetch_add(&trseq,1);
    TrEnt *e=&tring[s%TRN]; e->seq=s; e->tid=cur_thread()?cur_thread()->tid:0; e->name=name; e->a[0]=KH_RCX(g); e->a[1]=KH_RDX(g); e->a[2]=KH_R8(g); e->a[3]=KH_R9(g); e->ret=rd64(g,KH_RSP(g));
    tl_imp_name=name; tl_imp_ra=e->ret;
    u64 rv=e->ret-GS_BASE;
    if(rv>=0x109000&&rv<0x12a000 && !atomic_exchange(&kh_gx_reached,1)){
        fprintf(stderr,"kh: FIRST GX CALL REACHED: import %s from call-site RVA 0x%llx (import call #%llu)\n",name,(unsigned long long)rv,(unsigned long long)s);
        if(stop_at_gx){ fflush(stderr); if(trf) fflush(trf); _exit(0); }
    }
    if(trf && s<trmax){
        if(trace_skip_sync && (strstr(name,"CriticalSection")||strstr(name,"Mtx_"))) return;
        pthread_mutex_lock(&trm); tr_line(trf,e); pthread_mutex_unlock(&trm);
    }
}
static void dump_last(FILE *f,int n){
    u64 s=trseq; u64 from=s>(u64)n?s-n:0; fprintf(f,"---- last %d import calls ----\n",n);
    for(u64 i=from;i<s;i++){ const TrEnt *e=&tring[i%TRN]; if(e->seq==i) tr_line(f,e); }
}

/* ======================================================================================================================
 * fatal / fault
 * ==================================================================================================================== */
void kh_guest_bt(KCtx *g,FILE *f);
static void guest_bt(KCtx *g,FILE *f){ kh_guest_bt(g,f); }
void kh_guest_bt(KCtx *g,FILE *f){
    if(!g) return; u64 rsp=KH_RSP(g); fprintf(f,"---- guest: rsp=%llx rip(state)=%llx rax=%llx rcx=%llx rdx=%llx ----\n",(unsigned long long)rsp,(unsigned long long)KH_R64(g,KO_RIP),(unsigned long long)KH_RAX(g),(unsigned long long)KH_RCX(g),(unsigned long long)KH_RDX(g));
    /* the remaining general registers (state as last stored by the lifted code: a value still held in a host register is not visible) */
    fprintf(f,"  rbx=%llx rbp=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
            (unsigned long long)KH_RBX(g),(unsigned long long)KH_RBP(g),(unsigned long long)KH_RSI(g),(unsigned long long)KH_RDI(g),(unsigned long long)KH_R8(g),(unsigned long long)KH_R9(g),
            (unsigned long long)KH_R64(g,KO_R8+16),(unsigned long long)KH_R64(g,KO_R8+24),(unsigned long long)KH_R64(g,KO_R8+32),(unsigned long long)KH_R64(g,KO_R8+40),(unsigned long long)KH_R64(g,KO_R8+48),(unsigned long long)KH_R64(g,KO_R8+56));
    if(!GA_OK(rsp)) return; int n=0;
    for(u64 p=rsp;p<rsp+0x4000&&GA_OK(p+8)&&n<24;p+=8){ u64 v=rd64(g,p); if(v<GS_BASE+0x1000||v>=GS_BASE+0x5784f3) continue;
        const u8 *c=(const u8*)XL(g,v); int ok=(c[-5]==0xE8)||(c[-6]==0xFF&&c[-5]==0x15)||(c[-2]==0xFF&&(c[-1]&0xF8)==0xD0)||(c[-3]==0xFF&&(c[-2]&0xF8)==0x50)||(c[-2]==0xFF&&(c[-1]&0xF8)==0x10);
        if(ok){ fprintf(f,"  [rsp+0x%llx] return-site rva 0x%llx\n",(unsigned long long)(p-rsp),(unsigned long long)(v-GS_BASE)); n++; } }
}
static __thread KCtx *tl_cur_g;
void kh_set_cur_g(KCtx *g){ tl_cur_g=g; }
/* fault-time diagnostics of other modules (bgload.c: the background-load list).  Called once from the fatal / signal / fault paths,
 * after the generic report; a callback only reads guest memory it has validated and prints. */
#define MAXDIAG 8
static void (*diag_fn[MAXDIAG])(FILE*); static int ndiag; static _Atomic int diag_ran;
void kh_register_fault_diag(void (*fn)(FILE*)){ for(int i=0;i<ndiag;i++) if(diag_fn[i]==fn) return; if(ndiag<MAXDIAG) diag_fn[ndiag++]=fn; }
void kh_dump_threads(FILE *f);
static void fault_extras(FILE *f){
    if(atomic_exchange(&diag_ran,1)) return;
    for(int i=0;i<ndiag;i++) diag_fn[i](f);
    fprintf(f,"---- all threads at the fault ----\n"); kh_dump_threads(f);
}
static void fault_who(FILE *f){ KThread *t=cur_thread(); if(t) fprintf(f,"kh: faulting thread %u ('%s')\n",t->tid,t->name); else fprintf(f,"kh: faulting thread is not a guest thread\n"); }
void kh_fatal(KCtx *g,const char *fmt,...){
    va_list ap; fprintf(stderr,"\nkh: FATAL: "); va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); fputc('\n',stderr);
    if(g){ u64 ra=0; if(GA_OK(KH_RSP(g))) ra=rd64(g,KH_RSP(g)); fprintf(stderr,"kh: call-site (return address at rsp) = 0x%llx (rva 0x%llx)\n",(unsigned long long)ra,(unsigned long long)(ra-GS_BASE)); }
    fault_who(stderr); dump_last(stderr,30); guest_bt(g?g:tl_cur_g,stderr);
    void *bt[40]; int n=backtrace(bt,40); fprintf(stderr,"---- host backtrace ----\n"); backtrace_symbols_fd(bt,n,2);
    fault_extras(stderr);
    fflush(stderr); if(trf) fflush(trf); _exit(3);
}
void kh_warn(const char *fmt,...){ va_list ap; fprintf(stderr,"kh: warn: "); va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); fputc('\n',stderr); }
void kh_log(const char *fmt,...){ if(!kh_verbose) return; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); fputc('\n',stderr); }
static void on_sig(int sig,siginfo_t *si,void *uc){
    (void)uc; fprintf(stderr,"\nkh: SIGNAL %d (%s) addr=%p\n",sig,strsignal(sig),si->si_addr);
    fault_who(stderr); dump_last(stderr,30); guest_bt(tl_cur_g,stderr);
    void *bt[48]; int n=backtrace(bt,48); fprintf(stderr,"---- host backtrace ----\n"); backtrace_symbols_fd(bt,n,2);
    fault_extras(stderr); fflush(stderr); if(trf) fflush(trf); _exit(4);
}
static __thread char *altstack_mem;
void kh_install_sigaltstack(void){
    if(altstack_mem) return; size_t sz=1<<18; altstack_mem=malloc(sz); stack_t ss={.ss_sp=altstack_mem,.ss_size=sz,.ss_flags=0}; sigaltstack(&ss,0);
}
void kh_install_signals(void){
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_sig; sa.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER; sigemptyset(&sa.sa_mask);
    int sigs[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT,SIGTRAP}; for(unsigned i=0;i<sizeof sigs/sizeof*sigs;i++) sigaction(sigs[i],&sa,0);
}

/* ======================================================================================================================
 * dispatch: replaces khrt_dispatch.c (also covers runtime-registered host functions, used for COM vtables)
 * ==================================================================================================================== */
u64 kh_image_size = 0x2c2b000;
u64 kh_import_addr_base = KH_IMPORT_ADDR_BASE_DEFAULT;
static u32 *ht; static u32 hmask; static int ready;
static pthread_mutex_t hostfn_m = PTHREAD_MUTEX_INITIALIZER;
#define MAXHOST 65536
static struct { const char *name; KhFn fn; } hostfns[MAXHOST]; static u32 nhost;
u64 kh_host_fn(const char *name,KhFn fn){
    pthread_mutex_lock(&hostfn_m);
    for(u32 i=0;i<nhost;i++) if(!strcmp(hostfns[i].name,name)){ pthread_mutex_unlock(&hostfn_m); return KH_HOSTFN_BASE+16ull*i; }
    if(nhost>=MAXHOST){ pthread_mutex_unlock(&hostfn_m); fprintf(stderr,"kh: host fn table full\n"); abort(); }
    hostfns[nhost].name=strdup(name); hostfns[nhost].fn=fn; u64 a=KH_HOSTFN_BASE+16ull*nhost++; pthread_mutex_unlock(&hostfn_m); return a;
}
u64 kh_host_fn_lookup(const char *name){ for(u32 i=0;i<nhost;i++) if(!strcmp(hostfns[i].name,name)) return KH_HOSTFN_BASE+16ull*i; return 0; }
u32 kh_host_fn_count(void){ return nhost; }
const char *kh_host_fn_name(u64 a){ u64 d=a-KH_HOSTFN_BASE; if(d<16ull*nhost && !(d&15)) return hostfns[d>>4].name; return NULL; }
static inline u32 hh(u32 rva){ return (rva*2654435761u)>>8; }
static void build(void){
    u32 n=1; while(n<kh_fn_count*2) n<<=1;
    ht=(u32*)malloc(sizeof(u32)*n); memset(ht,0xff,sizeof(u32)*n); hmask=n-1;
    for(u32 i=0;i<kh_fn_count;i++){ u32 k=hh(kh_fn_table[i].rva)&hmask; while(ht[k]!=0xffffffffu) k=(k+1)&hmask; ht[k]=i; }
    ready=1;
}
KhFn kh_lookup(u64 target){
    if(!ready) build();
    u64 rva=target-KH_IMAGE_BASE;
    if(rva<kh_image_size){ u32 k=hh((u32)rva)&hmask; while(ht[k]!=0xffffffffu){ if(kh_fn_table[ht[k]].rva==(u32)rva) return kh_fn_table[ht[k]].fn; k=(k+1)&hmask; } return 0; }
    u64 d=target-kh_import_addr_base;
    if(d<16ull*kh_import_count && (d&15)==0) return kh_import_table[d>>4].fn;
    d=target-KH_HOSTFN_BASE; if(d<16ull*nhost && !(d&15)) return hostfns[d>>4].fn;
    return 0;
}
int kh_native_try(KCtx *g,u64 target);                 /* native_call.c: registered native Windows-ABI ranges (vkd3d), else 0 */
void kh_dispatch(KCtx *g,u64 target){
    KhFn f=kh_lookup(target);
    if(!f){ if(kh_native_try(g,target)) return; kh_fault(g,target,"dispatch: not a function start"); }
    g->aux[0]=target; f(g);
}
void kh_fault(KCtx *g,u64 va,const char *kind){
    fprintf(stderr,"\nkh_fault: %s at guest 0x%llx (rva 0x%llx)\n",kind,(unsigned long long)va,(unsigned long long)(va-GS_BASE));
    if(g&&GA_OK(KH_RSP(g))) fprintf(stderr,"kh: top of guest stack (return addr) = 0x%llx\n",(unsigned long long)rd64(g,KH_RSP(g)));
    fault_who(stderr); dump_last(stderr,30); guest_bt(g,stderr);
    void *bt[40]; int n=backtrace(bt,40); fprintf(stderr,"---- host backtrace ----\n"); backtrace_symbols_fd(bt,n,2);
    fault_extras(stderr); fflush(stderr); if(trf) fflush(trf); _exit(5);
}
void kh_unimplemented_import(KCtx *g,const char *dll,const char *name){
    u64 ra=GA_OK(KH_RSP(g))?rd64(g,KH_RSP(g)):0;
    kh_fatal(g,"unimplemented import %s!%s called (call-site RVA 0x%llx, args rcx=%llx rdx=%llx r8=%llx r9=%llx)",dll,name,(unsigned long long)(ra-GS_BASE),
             (unsigned long long)KH_RCX(g),(unsigned long long)KH_RDX(g),(unsigned long long)KH_R8(g),(unsigned long long)KH_R9(g));
}
void kh_nep_stub(KCtx *g,u32 rva){ kh_fatal(g,".nep CLR stub 0x%x called (no CLR in this port)",rva); }

/* ======================================================================================================================
 * guest calls from the runtime
 * ==================================================================================================================== */
#define KH_SENTINEL_RET 0x7ff7ffff0000ull
u64 kh_call5(KCtx *g,u64 target,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){
    u64 save=KH_RSP(g), sv_rcx=KH_RCX(g), sv_rdx=KH_RDX(g), sv_r8=KH_R8(g), sv_r9=KH_R9(g);
    u64 sp=(save-0x100)&~0xFull;
    wr64(g,sp+0x20,a4);            /* 5th argument slot (after the 32-byte shadow space and return address) */
    sp-=8; wr64(g,sp,KH_SENTINEL_RET);
    KH_RSP(g)=sp; KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    kh_dispatch(g,target);
    u64 r=KH_RAX(g); KH_RSP(g)=save; KH_RCX(g)=sv_rcx; KH_RDX(g)=sv_rdx; KH_R8(g)=sv_r8; KH_R9(g)=sv_r9; return r;
}
u64 kh_call(KCtx *g,u64 t,u64 a0,u64 a1,u64 a2,u64 a3){ return kh_call5(g,t,a0,a1,a2,a3,0); }

/* ======================================================================================================================
 * handle table
 * ==================================================================================================================== */
typedef struct { int kind; void *obj; } HEnt;
#define MAXH 65536
static HEnt htab[MAXH]; static u32 hnext=1; static pthread_mutex_t hm=PTHREAD_MUTEX_INITIALIZER;
u64 h_new(int kind,void *obj){
    pthread_mutex_lock(&hm);
    for(u32 n=0;n<MAXH;n++){ u32 i=hnext; hnext=hnext+1>=MAXH?1:hnext+1; if(htab[i].kind==HK_NONE){ htab[i].kind=kind; htab[i].obj=obj; pthread_mutex_unlock(&hm); return 0x100+4ull*i; } }
    pthread_mutex_unlock(&hm); return 0;
}
void *h_get(u64 h,int kind){
    if(h<0x100||h>=0x100+4ull*MAXH||(h&3)) return NULL; u32 i=(u32)((h-0x100)>>2); pthread_mutex_lock(&hm); void *o=NULL;
    if(htab[i].kind!=HK_NONE && (kind==0||htab[i].kind==kind)) o=htab[i].obj; pthread_mutex_unlock(&hm); return o;
}
int h_kind(u64 h){ if(h<0x100||h>=0x100+4ull*MAXH||(h&3)) return 0; return htab[(h-0x100)>>2].kind; }
/* Windows keeps a kernel object alive while a thread waits on it, even when its last handle is closed meanwhile ("SetEvent then
 * CloseHandle" is a legal pattern).  A waiter or signaller therefore takes a reference under the table lock. */
static _Atomic long wobj_frees;
long kh_wobj_frees(void){ return wobj_frees; }
void *h_get_ref(u64 h,int *kind_out){
    if(kind_out) *kind_out=HK_NONE;
    if(h<0x100||h>=0x100+4ull*MAXH||(h&3)) return NULL; u32 i=(u32)((h-0x100)>>2); KWaitObj *w=NULL;
    pthread_mutex_lock(&hm); int k=htab[i].kind; if(kind_out) *kind_out=k;
    if(k==HK_EVENT||k==HK_SEM||k==HK_MUTEX||k==HK_THREAD){ w=htab[i].obj; atomic_fetch_add(&w->refs,1); }
    pthread_mutex_unlock(&hm); return w;
}
void kh_wobj_unref(KWaitObj *w){ if(atomic_fetch_sub(&w->refs,1)==1){ pthread_mutex_destroy(&w->m); pthread_cond_destroy(&w->cv); free(w); atomic_fetch_add(&wobj_frees,1); } }
int h_close(u64 h){
    if(h<0x100||h>=0x100+4ull*MAXH||(h&3)) return 0; u32 i=(u32)((h-0x100)>>2); pthread_mutex_lock(&hm); int k=htab[i].kind; void *o=htab[i].obj; htab[i].kind=HK_NONE; htab[i].obj=NULL; pthread_mutex_unlock(&hm);
    if(k==HK_NONE) return 0;
    if(k==HK_EVENT||k==HK_SEM||k==HK_MUTEX||k==HK_THREAD){ kh_wobj_unref((KWaitObj*)o); }
    else if(k==HK_FIND||k==HK_FILE||k==HK_MAPPING||k==HK_SNAPSHOT){ extern void kh_handle_free(int kind,void *o); kh_handle_free(k,o); }
    return 1;
}

/* ======================================================================================================================
 * Windows path mapping (case-insensitive under kh_root)
 * ==================================================================================================================== */
int kh_resolve_ci(const char *base,const char *rel,char *out,size_t cap){
    snprintf(out,cap,"%s",base); size_t len=strlen(out);
    const char *p=rel; int ok=1;
    while(*p){
        while(*p=='/') p++; if(!*p) break; const char *e=p; while(*e&&*e!='/') e++;
        char comp[512]; size_t cl=(size_t)(e-p); if(cl>=sizeof comp) cl=sizeof comp-1; memcpy(comp,p,cl); comp[cl]=0; p=e;
        if(!strcmp(comp,".")) continue; if(!strcmp(comp,"..")){ char *sl=strrchr(out,'/'); if(sl&&sl>out){ *sl=0; len=strlen(out);} continue; }
        char cand[2048]; snprintf(cand,sizeof cand,"%s/%s",out,comp); struct stat st;
        if(ok && stat(cand,&st)!=0){
            DIR *d=opendir(out); int found=0; if(d){ struct dirent *de; while((de=readdir(d))){ if(!strcasecmp(de->d_name,comp)){ snprintf(cand,sizeof cand,"%s/%s",out,de->d_name); found=1; break; } } closedir(d); }
            if(!found) ok=0;
        }
        snprintf(out+len,cap-len,"/%s",strrchr(cand,'/')+1); len=strlen(out);
    }
    return ok;
}
int kh_winpath(const char *win,char *out,size_t cap){
    char t[2048]; size_t n=0;
    for(const char *p=win;*p&&n<sizeof t-1;p++) t[n++]=(*p=='\\')?'/':*p; t[n]=0;
    char *s=t; if(!strncmp(s,"//?/",4)) s+=4; if(isalpha((unsigned char)s[0])&&s[1]==':') s+=2;
    const char *pkgroot=getenv("KHNMC_PKGDIR");
    const char *image=NULL;
    if(!strncasecmp(s,"/hd28/STEAM/Image/",18)) image=s+18;
    else if(!strncasecmp(s,"/KH2/Image/",11)) image=s+11;
    else if(!strncasecmp(s,"Image/",6)) image=s+6;
    if(pkgroot&&*pkgroot&&image){
        /* Only package basenames cross this mapping; never allow traversal. */
        size_t len=strlen(image); char base[2048];
        if(strchr(image,'/')||len<5||
           (strcasecmp(image+len-4,".hed")&&strcasecmp(image+len-4,".pkg"))||
           snprintf(base,sizeof base,"%s/Image",pkgroot)>=(int)sizeof base){
            snprintf(out,cap,"/invalid-package-path"); return 0;
        }
        return kh_resolve_ci(base,image,out,cap);
    }
    /* synthetic Windows layout: C:\KH2 = game dir (kh_root); C:\Users\khnmc\Documents = kh_docs */
    if(!strncasecmp(s,"/KH2",4)&&(s[4]=='/'||s[4]==0)) return kh_resolve_ci(kh_root,s+4,out,cap);
    if(!strncasecmp(s,"/Users/khnmc/Documents",22)) return kh_resolve_ci(kh_docs,s+22,out,cap);
    if(s[0]=='/') { snprintf(out,cap,"%s",s); return 0; }                 /* other absolute: pass through (will not exist) */
    return kh_resolve_ci(kh_root,s,out,cap);                                 /* relative: the game dir is the working directory */
}
int kh_winpath_w(KCtx *g,u64 w,char *out,size_t cap){ char u[2048]; gw_to_utf8(g,w,u,sizeof u); return kh_winpath(u,out,cap); }

/* last error */
void kh_set_last_error(u32 e){ KThread *t=cur_thread(); if(t) t->lasterr=(int)e; }
u32 kh_get_last_error(void){ KThread *t=cur_thread(); return t?(u32)t->lasterr:0; }
