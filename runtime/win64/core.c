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
 * ==================================================================================================================== */
#define HDR 16
#define MAGIC_LIVE 0xA110C8EDu
#define MAGIC_FREE 0xF4EEF4EEu
#define MAGIC_ALN  0xA11A11A1u
typedef struct { u32 cls; u32 magic; u64 size; } BHdr;                  /* size = payload bytes */
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static u64 arena_lo, arena_hi, bump;
#define NCLS 120
static u64 freelist[NCLS];                                              /* singly linked: payload[0] = next */
typedef struct Run { u64 lo, size; struct Run *next; } Run;             /* free page runs, address ordered */
static Run *runs;
static _Atomic u64 live_bytes, alloc_count;
int ga_deterministic;
#define BIG_LIMIT (256u*1024u)
static u64 rup(u64 v,u64 a){ return (v+a-1)&~(a-1); }
static int cls_of(u64 n, u64 *csz){                                      /* size class for payload n <= BIG_LIMIT */
    if(n<=256){ int c=(int)((n+15)/16); if(c==0)c=1; *csz=(u64)c*16; return c; }          /* classes 1..16 : 16..256 */
    int e=63-__builtin_clzll(n-1);                                        /* 2^e < n <= 2^(e+1) */
    u64 step=(1ull<<e)/8; u64 c=rup(n,step); *csz=c; return 17+(e-8)*8+(int)((c-(1ull<<e))/step-1);
}
static u64 bump_take(u64 n){ if(bump+n>arena_hi) return 0; u64 r=bump; bump+=n; return r; }
static u64 page_run_alloc(u64 size){                                    /* caller holds gm; size multiple of 4096 */
    Run **pp=&runs;
    while(*pp){ Run *r=*pp; if(r->size>=size){ u64 p=r->lo; r->lo+=size; r->size-=size; if(!r->size){ *pp=r->next; free(r);} return p; } pp=&r->next; }
    u64 p=rup(bump,4096); if(p+size>arena_hi) return 0; bump=p+size; return p;
}
static void page_run_free(u64 lo,u64 size){
    if(ga_deterministic) return;
    madvise((void*)XLG(lo),size,MADV_DONTNEED);
    Run **pp=&runs; while(*pp && (*pp)->lo<lo) pp=&(*pp)->next;
    Run *n=(Run*)malloc(sizeof(Run)); n->lo=lo; n->size=size; n->next=*pp; *pp=n;
    if(n->next && n->lo+n->size==n->next->lo){ Run *m=n->next; n->size+=m->size; n->next=m->next; free(m); }
    Run *prev=NULL; for(Run *r=runs;r&&r!=n;r=r->next) prev=r;
    if(prev && prev->lo+prev->size==n->lo){ prev->size+=n->size; prev->next=n->next; free(n); }
}
void ga_init(u64 lo, u64 hi){ arena_lo=bump=rup(lo,65536); arena_hi=hi; }
static u64 alloc_locked(u64 n){
    if(n==0) n=1;
    if(ga_deterministic){ u64 p=bump_take(HDR+rup(n,16)); if(!p) return 0; BHdr *h=(BHdr*)XLG(p); h->cls=0; h->magic=MAGIC_LIVE; h->size=n; return p+HDR; }
    if(n<=BIG_LIMIT){
        u64 csz; int c=cls_of(n,&csz);
        u64 p=freelist[c];
        if(p){ freelist[c]=*(u64*)XLG(p); BHdr *h=(BHdr*)XLG(p-HDR); h->magic=MAGIC_LIVE; h->size=n; return p; }
        u64 b=bump_take(HDR+csz); if(!b) return 0; BHdr *h=(BHdr*)XLG(b); h->cls=(u32)c; h->magic=MAGIC_LIVE; h->size=n; return b+HDR;
    }
    u64 tot=rup(HDR+n,4096); u64 b=page_run_alloc(tot); if(!b) return 0;
    BHdr *h=(BHdr*)XLG(b); h->cls=0xffffffffu; h->magic=MAGIC_LIVE; h->size=n;
    /* payload is 16-aligned (HDR); run size derivable from n */
    return b+HDR;
}
u64 ga_alloc(u64 n, u64 align){
    pthread_mutex_lock(&gm);
    u64 r;
    if(align<=16){ r=alloc_locked(n); }
    else {
        u64 raw=alloc_locked(n+align+HDR);
        if(!raw) r=0; else {
            u64 p=rup(raw+HDR,align); BHdr *h=(BHdr*)XLG(p-HDR); h->cls=0; h->magic=MAGIC_ALN; h->size=raw;   /* size field = base payload address */
            r=p;
        }
    }
    pthread_mutex_unlock(&gm);
    if(r){ atomic_fetch_add(&live_bytes,n); atomic_fetch_add(&alloc_count,1); }
    return r;
}
static void free_locked(u64 p){
    BHdr *h=(BHdr*)XLG(p-HDR);
    if(h->magic==MAGIC_ALN){ u64 base=h->size; h->magic=0; free_locked(base); return; }
    if(h->magic!=MAGIC_LIVE){ fprintf(stderr,"kh: ga_free of bad/double-freed block %llx (magic %x)\n",(unsigned long long)p,h->magic); abort(); }
    if(ga_deterministic){ h->magic=MAGIC_FREE; return; }
    if(h->cls==0xffffffffu){ u64 tot=rup(HDR+h->size,4096); h->magic=MAGIC_FREE; page_run_free(p-HDR,tot); return; }
    h->magic=MAGIC_FREE; *(u64*)XLG(p)=freelist[h->cls]; freelist[h->cls]=p;
}
void ga_free(u64 p){ if(!p) return; pthread_mutex_lock(&gm); BHdr *h=(BHdr*)XLG(p-HDR); u64 n=(h->magic==MAGIC_LIVE)?h->size:0; free_locked(p); pthread_mutex_unlock(&gm); atomic_fetch_sub(&live_bytes,n); }
u64 ga_usable(u64 p){ BHdr *h=(BHdr*)XLG(p-HDR); if(h->magic==MAGIC_ALN){ BHdr *b=(BHdr*)XLG(h->size-HDR); return b->size-(p-h->size); } return h->size; }
u64 ga_realloc(u64 p,u64 n){
    if(!p) return ga_alloc(n,16);
    if(n==0){ ga_free(p); return 0; }
    u64 old=ga_usable(p); if(n<=old){ BHdr *h=(BHdr*)XLG(p-HDR); if(h->magic==MAGIC_LIVE){ /* keep block, shrink logical size */ atomic_fetch_sub(&live_bytes,h->size-n); h->size=n; } return p; }
    u64 q=ga_alloc(n,16); if(!q) return 0; memcpy((void*)XLG(q),(void*)XLG(p),old<n?old:n); ga_free(p); return q;
}
u64 ga_calloc(u64 n,u64 s){ u64 t; if(__builtin_mul_overflow(n,s,&t)) return 0; u64 p=ga_alloc(t,16); if(p) memset((void*)XLG(p),0,t); return p; }
u64 ga_pages(u64 size){ size=rup(size,4096); pthread_mutex_lock(&gm); u64 p=page_run_alloc(size); pthread_mutex_unlock(&gm);
#ifdef __linux__
    if(p&&ga_deterministic) memset((void*)XLG(p),0,size);      /* Linux: fresh and madvise(DONTNEED)-released pages read as zero */
#else
    if(p) memset((void*)XLG(p),0,size);                        /* other hosts: zero explicitly */
#endif
    return p; }
void ga_pages_free(u64 p,u64 size){ pthread_mutex_lock(&gm); page_run_free(p,rup(size,4096)); pthread_mutex_unlock(&gm); }
u64 ga_strdup(const char *s){ size_t n=strlen(s)+1; u64 p=ga_alloc(n,16); if(p) memcpy((void*)XLG(p),s,n); return p; }
void ga_stats(u64 *lb,u64 *ac){ *lb=live_bytes; *ac=alloc_count; }

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
u64 ga_bump_ptr(void){ return bump; }
u64 ga_arena_lo(void){ return arena_lo; }

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
