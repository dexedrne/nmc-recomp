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
int  kh_gfxc_selftest_pure(KCtx *g);                                                 /* every --selftest: bundle reference counts, fence events, video report (no device) */
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

/* ---- TASK36: the Vulkan side (IOS-GRAPHICS-PLAN steps 1, 6, 7) --------------------------------------------------------------- */
#include "gfx_present.h"
/* step 1: the presentation extensions vkd3d ENABLED (requested as optional; seen by the gipa wrapper at vkCreateInstance and at
 * vkCreateDevice), the REAL vkGetInstanceProcAddr, the frame-dump directory (check-mode pictures go next to the dumps) */
typedef struct { int surface, headless, metal, swapchain, display_timing, metal_objects, layer_settings, seen_instance, seen_device; } KhPresExt;
extern KhPresExt kh_gfxc_pres_ext;
PFN_vkGetInstanceProcAddr kh_gfxc_vk_gipa_real(void);
const char *kh_gfxc_dump_dir(void);
/* Vulkan functions of one vkd3d device (gfx_present.c): core ones always, the WSI ones when the extensions are enabled */
typedef struct KhVk KhVk;
const KhVk *kh_vk_for(ID3D12Device *d);
int kh_vk_memtype(const KhVk *v,u32 type_bits,VkMemoryPropertyFlags want);          /* the first matching memory type, -1 = none */
/* the calls the own back buffers make (gfx_vkd3d.c), through that table */
VkResult kh_vk_create_image(const KhVk *v,const VkImageCreateInfo *ci,VkImage *out);
void     kh_vk_image_memreq(const KhVk *v,VkImage im,VkMemoryRequirements *mr);
VkResult kh_vk_alloc(const KhVk *v,const VkMemoryAllocateInfo *ai,VkDeviceMemory *out);
VkResult kh_vk_bind_image(const KhVk *v,VkImage im,VkDeviceMemory m);
void     kh_vk_destroy_image(const KhVk *v,VkImage im);
void     kh_vk_free(const KhVk *v,VkDeviceMemory m);
/* every Vulkan object this runtime makes is counted (made / destroyed): "nothing is left after teardown" is a number */
enum { VKO_SURFACE, VKO_SWAPCHAIN, VKO_SEM, VKO_FENCE, VKO_POOL, VKO_BUF, VKO_MEM, VKO_IMAGE, VKO_N };
extern _Atomic long long kh_vko_live[VKO_N];
void kh_vko_note(int kind,int made);
long long kh_vko_live_total(void);
/* step 6/7: the presenter of one swap chain.  The back buffers it reads are the swap chain's own VkImages (as integers: VkImage is a
 * non-dispatchable handle); `general` = KHNMC_GFX_BACKBUFFERS=general (D3D12 PRESENT = Vulkan GENERAL, not TRANSFER_SRC) */
typedef struct KhPresenter KhPresenter;
typedef struct { u32 w, h, n; u64 image[3]; int general; } KhPresentBuffers;
int  kh_present_mode(void);                                                          /* 0 off, 1 headless, 2 layer (KHNMC_GFX_PRESENT) */
KhPresenter *kh_present_attach(ID3D12Device *dev,ID3D12CommandQueue *q,const KhPresentBuffers *bb);   /* NULL when presentation is off */
int  kh_present_frame(KCtx *g,KhPresenter *p,u32 idx,u64 present_no);               /* after the readback, before the index advances; 1 = a check
                                                                                       * present: the caller hands a D3D12 readback of the same
                                                                                       * back buffer, queued after it, to kh_present_check */
void kh_present_check(KhPresenter *p,u64 present_no,const u8 *ref,u32 ref_row_pitch); /* check mode against that independent reference (review V36 n=1) */
void kh_present_resize(KhPresenter *p,const KhPresentBuffers *bb);                   /* new back buffers (ResizeBuffers) */
void kh_present_quiesce(KhPresenter *p);                                             /* every submission of the presenter made so far is complete; any
                                                                                       * thread, also while a Present waits in its marker (d) (review Y39 n=1) */
void kh_present_resume(KhPresenter *p);                                              /* the swapchain is rebuilt at the next present; any thread, any time */
void kh_present_teardown(KhPresenter *p);
void kh_present_report(FILE *f);
u64  kh_gfx_gate_last_clear_ns(void);
/* selftest hooks of the presenter (gfx_present.c) and of the swap chain (gfx_vkd3d.c) */
void kh_present_test_force_check(int on);
void kh_present_test_after_acquire(void (*fn)(KhPresenter *));
void kh_present_test_override(int mode,int fit,u32 w,u32 h);
void kh_present_test_counters(u64 *frames,u64 *submits,u64 *presents,u64 *rebuilds,u64 *check_ok,u64 *check_fail,u64 *parked,u64 *remade);
void kh_present_test_extent(KhPresenter *p,u32 *w,u32 *h,u32 *nimg);
void kh_present_test_cost(u64 *n,u64 *sum,u64 *max);
double kh_present_test_dmax(void);
void kh_gfx_gate_test_reset_queue_waits(void);
typedef struct { ID3D12Device *dev; ID3D12CommandQueue *queue; ID3D12Resource *buf[3]; u64 image[3]; int bbmode; u32 w, h; KhPresenter *pres; } KhSwapInfo;
int  kh_vkd3d_swap_test_info(KCtx *g,u64 sc,KhSwapInfo *out);                       /* 0 = not a vkd3d swap chain */
void kh_vkd3d_swap_test_release(KCtx *g,u64 sc);                                     /* presenter teardown, back buffers released and destroyed */
void kh_vkd3d_test_bbmode(int mode);                                                 /* -1 = KHNMC_GFX_BACKBUFFERS; 0 own, 1 general, 2 committed */
int  kh_present_selftest(KCtx *g,u64 m,u64 dev,u64 q,u64 fct);                      /* gfx_present.c, called by the vkd3d selftest */
int  kh_present_test_pattern(KCtx *g,u64 gm,u64 fct,u64 q,u8 *out,double *dmax);    /* review C43 n=1: the pattern frame on any device, read back (4*96*64 BGRA) */
/* task 39 selftest helper (gfx_present.c): a thread that (when after_change is set) first waits until *after_change differs from
 * `from`, then sets the gate after set_ms, holds it hold_ms, clears it, waits 10 ms and then signals `fence` to 1 from the CPU (a queue
 * that Waits on it is blocked until then: the bounded waits must not count the held time) */
typedef struct { ID3D12Fence *fence; unsigned set_ms, hold_ms; const volatile u64 *after_change; u64 from; u64 set_ns, cleared_ns; int changed; } KhGateHoldSig;
void *kh_gfx_test_hold_then_signal(void *arg);
void kh_gfx_submit_counts(u64 *total,u64 *set_admitted,u64 *set_outside);   /* TASK41: vkQueueSubmit calls by vkd3d (the gipa wrapper) */

/* ---- selftest helpers: call a guest-visible function pointer like a lifted `call` (n arguments, 4 in registers) ----------------- */
extern int kh_gfxc_st_fails;
u64 kh_gfxc_st_call(KCtx *g,u64 target,void (*imp)(KCtx*),int n,const u64 *a);
#define GC(target,...) ({ u64 a_[]={__VA_ARGS__}; kh_gfxc_st_call(g,(target),NULL,(int)(sizeof a_/sizeof*a_),a_); })
#define GI(imp,...)    ({ u64 a_[]={__VA_ARGS__}; kh_gfxc_st_call(g,0,(imp),(int)(sizeof a_/sizeof*a_),a_); })
#define VT(o,slot) rd64(g,rd64(g,(u64)(o))+8ull*(slot))
/* the guest address at which the lifted code sees a HOST address (host - bias: exact, XL is an addition) */
#define GP(p) ((u64)(uintptr_t)(p)-(u64)XL(g,0))
