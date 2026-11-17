/* gfx_vkd3d_int.h : what the two vkd3d-backed graphics bridges share (TASK21).
 *
 *   gfx_vkd3d.c   the common part (Vulkan driver selection, vkd3d instance and device, swap chain on real resources, frame dumps and video
 *                 stream, bundle recorder, default root descriptor tables, census, report, the device selftest) AND the TRAMPOLINE bridge
 *                 KHNMC_GFX=vkd3d: host vkd3d objects handed to the lifted code, calls forwarded by native_call.c.  That bridge needs
 *                 x86-64 and the identity address model and is compiled only there (KH_GFX_TRAMP).
 *   gfx_wrap.c    the WRAPPED bridge KHNMC_GFX=vkd3dw: every D3D12 object the game sees is a guest-side COM object that holds the real
 *                 interface pointer; every method decodes the guest arguments, translates guest pointers and calls vkd3d's C API.
 *                 Correct in every address mode (identity, bias); no assembly and no ABI of its own (the compiler calls vkd3d with the ABI
 *                 vkd3d's headers declare).  Runs on Linux x86-64 and, since TASK21b, on arm64 macOS (static vkd3d over MoltenVK).  The shared start-up depends on the OS, not the CPU: the CPU
 *                 driver policy (lavapipe manifest) exists on Linux only, elsewhere KHNMC_GFX_DRIVER=system must be given; the Vulkan
 *                 library is chosen per OS (macOS: libvulkan.1.dylib, else libMoltenVK.dylib) and handed to vkd3d.
 *
 * Everything below works on REAL (host) interface pointers.  Include after the vkd3d headers; only with KH_GFX_VKD3D. */
#pragma once
#include "khnmc_rt.h"

#if defined(__x86_64__) && KH_XLATE_MODE==0
#define KH_GFX_TRAMP 1                     /* the Windows-ABI trampoline bridge exists in this build */
#else
#define KH_GFX_TRAMP 0
#endif

extern int kh_gfx_vkd3d_on;                /* 1 = a vkd3d device is behind the game's D3D12 calls (either bridge) */
extern int kh_gfx_wrap_on;                 /* 1 = ... through the wrapped bridge (the game holds guest-side objects, never a host pointer) */
extern int kh_native_trace;                /* KHNMC_GFX_TRACE=1: one line per bridged call (both bridges) */
extern _Atomic u64 kh_native_calls;        /* calls that went through the Windows-ABI trampoline (must stay 0 on the wrapped bridge) */
extern _Atomic u64 gfx_draws, gfx_clears, gfx_barriers, gfx_execs, gfx_presents, gfx_copies, gfx_psos, gfx_resources, gfx_bundles, gfx_pso_binds;

#define GFX_P(x) ((unsigned long long)(x))
#define GFX_U(x) ((u64)(uintptr_t)(x))

/* ---- method names by vtable slot (trace, per-method call counts, named fatals) ---------------------------------------------- */
extern const char *const kh_gfxc_dev_names[], *const kh_gfxc_list_names[], *const kh_gfxc_queue_names[], *const kh_gfxc_alloc_names[], *const kh_gfxc_res_names[],
                  *const kh_gfxc_dheap_names[], *const kh_gfxc_fence_names[];
extern const int kh_gfxc_dev_nnames, kh_gfxc_list_nnames, kh_gfxc_queue_nnames, kh_gfxc_alloc_nnames, kh_gfxc_res_nnames, kh_gfxc_dheap_nnames, kh_gfxc_fence_nnames;

/* ---- common start-up (driver selection, dump / video configuration, bundle classes) and device creation ----------------------- */
void kh_gfxc_init_common(const char *label);
/* instance (created once) + a new vkd3d device with at least `level`; S_OK and *out, or the failing HRESULT (already reported) */
HRESULT kh_gfxc_device_new(KCtx *g,u32 level,u64 adapter_shown,ID3D12Device **out);
const char *kh_gfxc_device_name(void);                                               /* the Vulkan device behind the vkd3d instance, for reports */

/* ---- default root descriptor tables: state per real command list (see the file comment of gfx_vkd3d.c) ------------------------ */
void kh_gfxc_track_reset(KCtx *g,u64 list);                                          /* after a successful Reset */
void kh_gfxc_track_drop(u64 list);                                                   /* the list is gone */
void kh_gfxc_track_rootsig(KCtx *g,u64 list,u64 rs);                                 /* after SetGraphicsRootSignature */
void kh_gfxc_track_heaps(KCtx *g,u64 list,u32 n,const u64 *heaps);                   /* after SetDescriptorHeaps (real heap pointers) */
void kh_gfxc_track_table(KCtx *g,u64 list,u32 idx);                                  /* after SetGraphicsRootDescriptorTable */
void kh_gfxc_rootsig_learn(KCtx *g,u64 rs,const void *blob,size_t size);             /* after a successful CreateRootSignature */

/* ---- census ------------------------------------------------------------------------------------------------------------------ */
void kh_gfxc_census_resource(const D3D12_RESOURCE_DESC *d);                          /* a resource was created */
void kh_gfxc_census_barriers(const D3D12_RESOURCE_BARRIER *b,u32 n);                 /* barriers with REAL resource pointers */
void kh_gfxc_census_resolve(void);
void kh_gfxc_census_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d);                /* a pipeline state was created (host copy of the desc) */
void kh_gfxc_census_dsv(ID3D12Resource *res,u32 view_format);                        /* a depth-stencil view was created (0 = the resource's format) */

/* ---- bundles: our recorder objects (guest COM objects in both bridges), replayed on a real direct command list ----------------- */
u64  kh_gfxc_bundle_allocator_new(KCtx *g);                                          /* CreateCommandAllocator(BUNDLE) */
u64  kh_gfxc_bundle_new(KCtx *g,u64 allocator,u64 pso);                              /* CreateCommandList(BUNDLE): allocator and pso as the game passed them */
int  kh_gfxc_is_bundle(KCtx *g,u64 o);
void kh_gfxc_bundle_replay(KCtx *g,ID3D12GraphicsCommandList *l,u64 bundle);         /* ExecuteBundle */

/* ---- the wrapped bridge (gfx_wrap.c) -------------------------------------------------------------------------------------------- */
enum { WK_DEVICE=0, WK_QUEUE, WK_ALLOCATOR, WK_LIST, WK_RESOURCE, WK_DHEAP, WK_FENCE, WK_PSO, WK_ROOTSIG, WK_HEAP, WK_QHEAP, WK_BLOB, WK_N };
void  kh_wrap_classes(void);                                                         /* register the wrapper classes (idempotent) */
u64   kh_wrap_obj(KCtx *g,void *real,int kind);                                      /* THE guest object of a real object (made on first use; one per real object) */
void *kh_wrap_real(KCtx *g,u64 obj,int kind,const char *what);                       /* real pointer behind a guest object (0 -> NULL; anything else that is not such a wrapper: named fatal) */
u32   kh_wrap_release_real(KCtx *g,void *real);                                      /* the runtime drops ITS reference to a real object that may have a wrapper */
void  kh_wrap_serialize_rootsig(KCtx *g);                                            /* D3D12SerializeRootSignature import */
void  kh_wrap_report(FILE *f);
int   kh_wrap_selftest_device(KCtx *g);                                              /* KHNMC_GFX=vkd3dw --selftest: checks that need a real device */
int   kh_wrap_selftest_pure(KCtx *g);                                                /* every --selftest: structure translation under a synthetic bias, the table, named fatals */

/* ---- selftest helpers: call a guest-visible function pointer like a lifted `call` (n arguments, 4 in registers) ----------------- */
extern int kh_gfxc_st_fails;
u64 kh_gfxc_st_call(KCtx *g,u64 target,void (*imp)(KCtx*),int n,const u64 *a);
#define GC(target,...) ({ u64 a_[]={__VA_ARGS__}; kh_gfxc_st_call(g,(target),NULL,(int)(sizeof a_/sizeof*a_),a_); })
#define GI(imp,...)    ({ u64 a_[]={__VA_ARGS__}; kh_gfxc_st_call(g,0,(imp),(int)(sizeof a_/sizeof*a_),a_); })
#define VT(o,slot) rd64(g,rd64(g,(u64)(o))+8ull*(slot))
/* the guest address at which the lifted code sees a HOST address (host - bias: exact, XL is an addition) */
#define GP(p) ((u64)(uintptr_t)(p)-(u64)XL(g,0))
