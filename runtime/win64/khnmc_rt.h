/* khnmc_rt.h : internal interface of the KHNMC Win64 runtime v0 (hand-written, our own code).
 * Guest ABI is the lifter's (khrt.h): every runtime entry is `void f(KCtx *g)`, Win64 args in rcx,rdx,r8,r9 then the guest stack,
 * result in rax / xmm0, and the function pops its own return address (KH_RET) like a `ret`.  The IMP macros do that for you. */
#pragma once
#define _GNU_SOURCE
#include "khrt.h"
#include "khrt_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>

/* ---- guest space -------------------------------------------------------------------------------------------------- */
#define GS_BASE      KH_IMAGE_BASE                 /* guest address of the first byte of guest space (= image base) */
#define IMG_SIZE     0x2c2b000ull                  /* SizeOfImage */
#define GUEST_PTR(a) ((void*)XL(g,(a)))
extern uintptr_t gs_host_base;                     /* host address of guest address GS_BASE */
extern u64 gs_size;                                /* bytes of guest space (image + arena) */
extern uintptr_t gs_bias;                          /* host - guest (0 in identity mode) */
int  ga_ok(u64 a);                                 /* a is in the image or in a COMMITTED part of the guest arena (readable and writable) */
#define GA_OK(a) ga_ok((u64)(a))

/* Guest arena (core.c).  Every block carries the source that asked for it: the source decides the zone (see the layout comment in
 * core.c: the ID ZONE holds everything the game can turn into a 32-bit object id and is capped so that the game's 64-slot id table
 * cannot overflow; the FAR ZONE holds bulk data the game never encodes) and feeds the per-window report of idtable.c. */
enum { GA_SRC_RT=0,                                /* runtime-internal objects (COM objects, small strings, test blocks) */
       GA_SRC_CRT,                                 /* the game's malloc / calloc / realloc / _aligned_* (the UCRT heap on Windows) */
       GA_SRC_HEAP,                                /* HeapAlloc(GetProcessHeap), CoTaskMemAlloc, SysAllocString */
       GA_SRC_STACK,                               /* thread and fiber stacks */
       GA_SRC_TLS,                                 /* TEB, TLS array, TLS blocks */
       GA_SRC_FILE,                                /* file and HD-asset buffers the loose-tree layers hand to the game */
       GA_SRC_GFX,                                 /* null D3D12 backend: data of buffer resources (driver / GPU memory on Windows) */
       GA_SRC_AUDIO,                               /* software XAudio2 mixer */
       GA_SRC_MAP,                                 /* CreateFileMapping views */
       GA_SRC_POOL,                                /* the game's enlarged cache / data pools (moose.c) */
       GA_NSRC };
#define GA_WIN_SHIFT 25
#define GA_WIN       (1ull<<GA_WIN_SHIFT)          /* 32 MiB: one window of the game's object-id table */
#define GA_ID_WINDOWS 58                           /* [GS_BASE, GS_BASE + 58 windows) = image + id zone (1,856 MiB); 6 of the 64 table slots stay spare */
u64  ga_alloc_src(u64 size, u64 align, int src);   /* guest heap allocation (16-byte min alignment); zone exhaustion is a named fatal */
u64  ga_calloc_src(u64 n, u64 sz, int src);
u64  ga_pages_src(u64 size, int src);              /* page-granular zeroed run without a block header (stacks, mappings) */
void ga_pages_free_src(u64 p, u64 size, int src);
u64  ga_pages_aligned(u64 size, u64 align, int src); /* zeroed run whose start is a multiple of align (a power of two >= 4096) */
u64  ga_strdup_src(const char *s, int src);
#define ga_alloc(n,a)     ga_alloc_src((n),(a),GA_SRC_RT)
#define ga_calloc(n,s)    ga_calloc_src((n),(s),GA_SRC_RT)
#define ga_pages(n)       ga_pages_src((n),GA_SRC_RT)
#define ga_pages_free(p,n) ga_pages_free_src((p),(n),GA_SRC_RT)
#define ga_strdup(s)      ga_strdup_src((s),GA_SRC_RT)
void ga_free(u64 p);
u64  ga_realloc(u64 p, u64 size);                  /* a moved block keeps its source */
u64  ga_usable(u64 p);
void ga_stats(u64 *live_bytes, u64 *allocs);
extern int ga_deterministic;                       /* oracle mode: bump allocator, no reuse, legacy single-zone layout */
int  ga_describe(u64 addr, char *out, size_t cap); /* what an address is (image / stack / heap block and its source / not guest memory); 1 = inside guest space */
void ga_window_sources(u64 base, char *out, size_t cap);   /* live bytes per source inside the 32 MiB window at base */
void ga_report(FILE *f);                           /* zones: range, extent, committed, peak, live, free */
const char *ga_src_name(int src);

/* host-side access to guest memory. g may be NULL in identity mode only; always pass it. */
static inline void *hp(KCtx *g, u64 a){ return (void*)XL(g,a); }
static inline u8  rd8 (KCtx *g,u64 a){ return *(u8*)XL(g,a); }
static inline u16 rd16(KCtx *g,u64 a){ u16 v; memcpy(&v,(void*)XL(g,a),2); return v; }
static inline u32 rd32(KCtx *g,u64 a){ u32 v; memcpy(&v,(void*)XL(g,a),4); return v; }
static inline u64 rd64(KCtx *g,u64 a){ u64 v; memcpy(&v,(void*)XL(g,a),8); return v; }
static inline void wr8 (KCtx *g,u64 a,u8 v){ *(u8*)XL(g,a)=v; }
static inline void wr16(KCtx *g,u64 a,u16 v){ memcpy((void*)XL(g,a),&v,2); }
static inline void wr32(KCtx *g,u64 a,u32 v){ memcpy((void*)XL(g,a),&v,4); }
static inline void wr64(KCtx *g,u64 a,u64 v){ memcpy((void*)XL(g,a),&v,8); }
const char *gstr(KCtx *g,u64 a);                   /* NUL-terminated guest C string as host pointer (identity: same bytes) */
size_t gw_len(KCtx *g,u64 a);                      /* UTF-16 length */
void gw_to_utf8(KCtx *g,u64 a,char *out,size_t outsz);
void utf8_to_gw(KCtx *g,u64 dst,size_t dstchars,const char *s);

/* ---- argument access ---------------------------------------------------------------------------------------------- */
#define A0(g) KH_RCX(g)
#define A1(g) KH_RDX(g)
#define A2(g) KH_R8(g)
#define A3(g) KH_R9(g)
static inline u64 A_(KCtx *g,int i){ switch(i){case 0:return KH_RCX(g);case 1:return KH_RDX(g);case 2:return KH_R8(g);case 3:return KH_R9(g);} return rd64(g,KH_RSP(g)+8+8*(u64)i); }
#define ARG(g,i) A_(g,i)
#define A4(g) A_(g,4)
#define A5(g) A_(g,5)
#define SET_RAX(g,v) (KH_RAX(g)=(u64)(v))
static inline double argd(KCtx *g,int i){ double d; if(i<4) memcpy(&d,&KH_XMM(g,i),8); else { u64 v=rd64(g,KH_RSP(g)+8+8*(u64)i); memcpy(&d,&v,8);} return d; }
static inline void ret_d(KCtx *g,double d){ v128 o=VZ; memcpy(&o,&d,8); KH_XMM(g,0)=o; }
static inline void ret_f(KCtx *g,float d){ v128 o=VZ; memcpy(&o,&d,4); KH_XMM(g,0)=o; }
u64 guest_return_addr(KCtx *g);                    /* the call site's return address (rsp at import entry) */

/* ---- import wrappers ---------------------------------------------------------------------------------------------- */
void kh_trace_import(KCtx *g, const char *name);
#define IMP_SYM(dll,name) rt_imp_##dll##__##name
#define IMP_(dll,name,shown) static void i_##dll##__##name(KCtx*g); \
    void IMP_SYM(dll,name)(KCtx *g){ kh_trace_import(g,shown); i_##dll##__##name(g); KH_RET(g); } \
    static void i_##dll##__##name(KCtx *g)
#define XS_(x) #x
#define XS(x) XS_(x)
#define IMP(dll,name) IMP_(dll,name,XS(dll) "!" XS(name))
/* variant that does NOT pop the return address (the body does it, or never returns) */
#define IMPN_(dll,name,shown) static void i_##dll##__##name(KCtx*g); \
    void IMP_SYM(dll,name)(KCtx *g){ kh_trace_import(g,shown); i_##dll##__##name(g); } \
    static void i_##dll##__##name(KCtx *g)
#define IMPN(dll,name) IMPN_(dll,name,XS(dll) "!" XS(name))
/* shorthand DLL tokens for the sanitised symbol prefixes */
#define K32 kernel32
#define U32 user32
#define CRTR api_ms_win_crt_runtime_l1_1_0
#define CRTS api_ms_win_crt_stdio_l1_1_0
#define CRTT api_ms_win_crt_string_l1_1_0
#define CRTH api_ms_win_crt_heap_l1_1_0
#define CRTM api_ms_win_crt_math_l1_1_0
#define CRTC api_ms_win_crt_convert_l1_1_0
#define CRTF api_ms_win_crt_filesystem_l1_1_0
#define CRTL api_ms_win_crt_locale_l1_1_0
#define CRTI api_ms_win_crt_time_l1_1_0
#define CRTU api_ms_win_crt_utility_l1_1_0
#define VCR vcruntime140
#define MSVCP msvcp140

/* ---- fatal / logging ---------------------------------------------------------------------------------------------- */
void kh_fatal(KCtx *g, const char *fmt, ...) __attribute__((noreturn,format(printf,2,3)));   /* named fatal: prints the last 30 trace lines */
void kh_warn(const char *fmt, ...) __attribute__((format(printf,1,2)));
void kh_log(const char *fmt, ...) __attribute__((format(printf,1,2)));
extern int kh_verbose;
#define KH_UNIMPL(g,what) kh_fatal((g),"unimplemented: %s",(what))

/* ---- host function registry: synthetic guest addresses that dispatch to host code ---------------------------------- */
#define KH_HOSTFN_BASE 0x7ff7f8000000ull
u64  kh_host_fn(const char *name, KhFn fn);        /* returns a synthetic guest address (stable per name) */
u64  kh_host_fn_lookup(const char *name);          /* 0 if none */
const char *kh_host_fn_name(u64 addr);
extern u64 kh_dispatch_target;                     /* set by kh_dispatch to the target for host functions that need it (per thread) */

/* ---- guest calls from the runtime --------------------------------------------------------------------------------- */
u64  kh_call(KCtx *g, u64 target, u64 a0, u64 a1, u64 a2, u64 a3);     /* call a guest (or host-fn) address, returns rax */
u64  kh_call5(KCtx *g, u64 target, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);

/* ---- threads / fibers ---------------------------------------------------------------------------------------------- */
typedef struct KFiber KFiber;
typedef struct KThread {
    KCtx *g;                  /* the thread's own context (its guest stack) */
    u64   teb, tlsblk, tlsarr;
    u32   tid;
    pthread_t pt;
    int   is_main, started, done, suspended, detached;
    u64   exit_code;
    u64   start, param;
    u64   stack_lo, stack_hi;
    pthread_mutex_t m; pthread_cond_t cv;
    KFiber *thread_fiber;     /* result of ConvertThreadToFiber, or NULL */
    KFiber *cur_fiber;        /* running fiber on this thread; NULL = the thread itself */
    u32   handle;             /* handle-table id of the thread object (0 if none) */
    char  name[24];
    int   prio;
    u64   last_error;
    int   lasterr;
} KThread;
KThread *cur_thread(void);
void kh_thread_init_main(void);
u64  kh_make_teb(KThread *t);                        /* sets t->teb, tlsarr, tlsblk (seeded from the TLS template) */
extern u64 kh_tls_tmpl_lo, kh_tls_tmpl_hi, kh_tls_index_va, kh_tls_callbacks_va;
void kh_run_tls_callbacks(KCtx *g, u32 reason);
u64  kh_new_guest_stack(u64 size, u64 *lo);          /* returns the initial rsp (top, aligned) ; *lo = base */
KCtx *kh_new_ctx(void);
void kh_ctx_set_stack(KCtx *g, u64 top);
extern const u32 kh_import_count_ext;

/* fibers */
u64  kh_fiber_create(KCtx *g, u64 stack_size, u64 start, u64 param);
u64  kh_fiber_convert(KCtx *g, u64 param);
void kh_fiber_switch(KCtx *g, u64 target);
void kh_fiber_delete(KCtx *g, u64 f);
int  kh_fiber_convert_to_thread(KCtx *g);

/* handles (HANDLE <-> host object) */
enum { HK_NONE=0, HK_EVENT, HK_SEM, HK_MUTEX, HK_THREAD, HK_PROCESS, HK_FILE, HK_MAPPING, HK_FIND, HK_HEAP, HK_SNAPSHOT, HK_WAITABLE };
u64  h_new(int kind, void *obj);                    /* returns a guest-visible HANDLE */
void *h_get(u64 h, int kind);                       /* NULL if invalid or wrong kind */
int  h_close(u64 h);                                /* 1 if it existed */
int  h_kind(u64 h);
#define PSEUDO_PROCESS ((u64)-1)
#define PSEUDO_THREAD  ((u64)-2)

/* sync objects (waitable) */
typedef struct KWaitObj {
    int kind; pthread_mutex_t m; pthread_cond_t cv;   /* cv uses CLOCK_MONOTONIC */
    int signaled, manual; long count, maxcount;       /* event: signaled/manual; semaphore: count/maxcount; mutex: owner/recursion */
    u64 owner; int recurse; KThread *thread;         /* mutex / thread */
    _Atomic int refs;                                 /* the handle + every thread currently inside a wait/signal on the object */
    u64 gen;                                          /* manual-reset event: bumped by SetEvent / pulse, releases every thread waiting at that moment */
    int waiters, pulse_tokens;                        /* threads blocked on the object; auto-reset pulses granted to them */
} KWaitObj;
u32 kh_wait(KCtx *g, u64 handle, u32 ms);
void *h_get_ref(u64 h, int *kind_out);               /* wait objects only (event, semaphore, mutex, thread): +1 reference, NULL otherwise */
void kh_wobj_unref(KWaitObj *w);                     /* frees the object with its last reference */
int  kh_event_set(u64 h);                            /* SetEvent / PulseEvent semantics for runtime-side signallers (timers, fences); 0 = bad handle */
int  kh_event_pulse(u64 h);
extern u32 kh_wait_timeout_ms_max;                   /* debug: clamp infinite waits to detect hangs (0 = off) */

/* last error */
u64  kh_errno_ptr(KCtx *g);                          /* guest-visible per-thread errno variable (allocated on first use) */
void kh_set_errno(KCtx *g,int v);
void kh_set_last_error(u32 e);
u32  kh_get_last_error(void);

/* ---- time --------------------------------------------------------------------------------------------------------- */
u64 kh_qpc(void);                                    /* 10 MHz ticks (QueryPerformanceFrequency = 10,000,000) */
u64 kh_now_ns(void);

/* ---- paths -------------------------------------------------------------------------------------------------------- */
extern char kh_root[1024];                           /* host data root: the "game directory" */
extern char kh_docs[1024];                           /* host dir standing in for Documents */
int  kh_winpath(const char *win, char *out, size_t outsz);       /* Windows path -> host path (case-insensitive resolution under kh_root), 1 on mapped */
int  kh_winpath_w(KCtx *g, u64 wstr, char *out, size_t outsz);

/* ---- printf engine over a Win64 va_list in guest memory ------------------------------------------------------------ */
int  kh_vformat(KCtx *g, char *dst, size_t cap, const char *fmt, u64 valist, int wide_fmt, int *needed);
int  kh_vformat_w(KCtx *g, u16 *dst, size_t cap, const u16 *fmt, u64 valist, int *needed);

/* ---- platform seams (each in its own file) ------------------------------------------------------------------------- */
void kh_seam_steam_init(void);                       /* platform_seam_store.c */
void kh_seam_gfx_init(void);                         /* null_d3d12.c */
void kh_seam_audio_init(void);                       /* null_audio.c */
int kh_audio_cocreate(KCtx *g);
void kh_seam_input_init(void);
void kh_seam_window_init(void);
void kh_seam_mf_init(void);
void kh_report_seams(FILE *f);
/* counters for the report */
extern _Atomic u64 kh_cnt_gfx_calls, kh_cnt_audio_calls, kh_cnt_input_calls, kh_cnt_mf_calls, kh_cnt_steam_calls, kh_cnt_window_calls;
extern _Atomic int kh_gx_reached;                    /* set when an import is called from inside the GX extent */

/* generic COM helper: a class = one shared vtable of host-fn thunks; an object = guest block {vtbl, class id, refcount, extra...}.
 * Every vtable slot dispatches through kh_dispatch -> com_thunk -> handler(g, this, slot, class name).  handler returns 1 if it handled
 * the call (it set rax), 0 for "not specially handled": strict classes then fail loudly, lenient classes return 0 (S_OK / false / null). */
typedef int (*KhComHandler)(KCtx *g,u64 obj,int slot,const char *cls);
#define COM_STRICT 1      /* unknown slot = named fatal (else returns 0) */
#define COM_NOUNK  2      /* slots 0..2 are NOT IUnknown (XAudio2 voices) */
#define COM_FREE   4      /* the default Release frees the object when its count reaches 0 (objects the game creates and drops every frame) */
int  kh_com_class(const char *name,int nslots,int flags,KhComHandler h);   /* returns class id */
void kh_com_dump(FILE *f);
u64  kh_com_obj(int cls,u64 extra_bytes);                                      /* returns guest address of a new object (refcount 1) */
int  kh_com_cls_of(KCtx *g,u64 obj);
u64  kh_com_vtbl(int cls);
#define COM_REFS(g,o) rd64(g,(o)+16)
void kh_register_proc(const char *dll,const char *name,u64 addr);
u64  kh_spawn_hostfn_thread(KCtx *g,const char *name,KhFn fn,u64 param);

int kh_resolve_ci(const char *base,const char *rel,char *out,size_t cap);
int kh_file_selftest(KCtx *g);

void kh_phase_file(KCtx *g,const char *path);
void kh_phase_present(KCtx *g,u64 present,u64 draws,u64 clears,u64 psos);
void kh_input_attach(KCtx *g);

void kh_mod_syscalls_init(KCtx *g);
int kh_mod_syscalls_selftest(KCtx *g);
