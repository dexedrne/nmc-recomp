/* gfx_vkd3d.c : KHNMC_GFX=vkd3d, real pixels on Linux (docs/GFX-VKD3D-PLAN.md, TASK15).
 *
 * The lifted game's D3D12 calls are handed to vkd3d (WineHQ's D3D12-on-Vulkan library, LGPL-2.1+, dynamically linked):
 *  - D3D12CreateDevice (d3d12.dll #101) creates a HOST vkd3d device and gives it to the lifted code as-is.  Its COM methods use the Windows
 *    x64 calling convention, so `call [vtable+n]` in lifted code reaches them through kh_dispatch -> kh_native_try (native_call.c): one
 *    generic trampoline, no per-method marshalling.  The executable segments of the three vkd3d libraries are the registered native ranges.
 *  - D3D12SerializeRootSignature -> vkd3d-utils.
 *  - DXGI stays ours (null_d3d12.c: factory / adapter / output / swap-chain objects).  In this mode the swap chain owns three real
 *    B8G8R8A8 render-target resources created through the game's device (found with ID3D12CommandQueue::GetDevice), GetBuffer returns
 *    them, ResizeBuffers recreates them, and Present can copy the just-presented buffer to a readback buffer and write an image:
 *        KHNMC_GFX_DUMP=<dir>           write <dir>/frame-NNNNNN.ppm (binary P6) and append a line to <dir>/index.tsv
 *        KHNMC_GFX_DUMP_EVERY=<n>       every n-th present (default 300)
 *        KHNMC_GFX_HASHLOG=<file>       one line per present with two 64-bit hashes of the presented picture (RGB only, and BGRA): two runs
 *                                       can then be compared present by present, and aligned by content where their timelines differ
 *  - Vulkan driver: the runtime selects the CPU driver itself.  Unless KHNMC_GFX_DRIVER=system is given, VK_DRIVER_FILES is forced to the
 *    lavapipe manifest before the first Vulkan use, implicit layers are disabled, LP_NUM_THREADS defaults to 4, and every physical device
 *    the Vulkan instance reports must be of type CPU or the process stops with a named fatal before a device is created.  That CPU policy
 *    is Linux's (the manifest path); in a build for another OS the default is the named fatal gfx-driver-no-cpu-driver and the system's
 *    Vulkan (macOS: MoltenVK) needs KHNMC_GFX_DRIVER=system.  The Vulkan library (libvulkan.so.1; macOS: libvulkan.1.dylib, else
 *    libMoltenVK.dylib, vkd3d's own order) is loaded by the runtime and handed to vkd3d, so vkd3d and the device check use the same one.
 *        KHNMC_GFX_VULKAN_LIB=<absolute path>   load exactly this library instead of searching by name (TASK21b: MoltenVK's dylib on
 *                                       macOS, where the system strips DYLD_LIBRARY_PATH from protected launchers such as nice).  A
 *                                       build may carry a default (-DKH_GFX_VULKAN_LIB="<absolute path>", Makefile MOLTENVK_LIB on
 *                                       Darwin); the variable wins.  A named library that cannot be loaded is a named fatal: there is
 *                                       no fall-back to the search by name.
 *  - The vkd3d instance is created by us (vkd3d_create_instance), not by vkd3d-utils' D3D12CreateDevice, for two reasons that are on our
 *    side of the boundary: fence completion events must signal OUR kernel32 event handles (pfn_signal_event), and the game's wide strings
 *    (SetName) are 16-bit (wchar_size 2; vkd3d-utils would say 4 on Linux).
 *  - Bundles are ours too.  vkd3d 2.1 does not implement them (CreateCommandAllocator / CreateCommandList of type BUNDLE: "Unhandled command
 *    list type 0x1"; ID3D12GraphicsCommandList::ExecuteBundle: "stub!"), and the game records most of its draws in one-shot bundles.  Three
 *    native entry points are overridden (native_call.c): the two creation calls return OUR recorder objects for type BUNDLE, and
 *    ExecuteBundle replays the recorded calls on the real direct command list at that point, which is exactly D3D12's inheritance rule
 *    (a bundle sees the caller's state, its own state changes stay).  The recorder accepts the 12 methods a bundle may use that this game
 *    uses; any other method on a bundle is a named fatal.
 *  - Default root descriptor tables.  The game draws its copy-blit pass (fn 0x10dbc0, DrawInstanced(3)) on a freshly Reset command list
 *    without ever setting the texture and sampler tables its pixel shader reads.  D3D12 leaves that undefined and Windows drivers tolerate
 *    it; through vkd3d the Vulkan driver reads an unwritten descriptor and crashes (seen on lavapipe: fault in the generated sampler
 *    code).  We define it: as soon as a direct command list has a root signature and descriptor heaps, every descriptor-table parameter
 *    the game has not set itself is bound to the first descriptor of the heap of its type.  State is tracked with post hooks on five
 *    command-list methods and CreateRootSignature (whose blob gives the table layout); well-defined draws are not affected.
 * Compiled out (no KH_GFX_VKD3D): KHNMC_GFX=vkd3d is a loud named fatal.
 *
 * TASK21: this file is also the COMMON part of the second bridge, KHNMC_GFX=vkd3dw (gfx_wrap.c: guest-side wrapper objects instead of host
 * objects, for address modes where a guest pointer is not a host pointer and for hosts without the Windows ABI).  Driver selection, the
 * vkd3d instance and device, the swap chain, frame dumps, the video stream, the bundle recorder, the default descriptor tables, the census
 * and the device selftest are shared (gfx_vkd3d_int.h); they work on REAL interface pointers and translate guest memory with hp().  The
 * trampoline bridge itself (host objects handed out, native ranges, watches / overrides / post hooks) is the part inside KH_GFX_TRAMP: it
 * needs x86-64 and the identity address model, and in any other build KHNMC_GFX=vkd3d is the named fatal gfx-vkd3d-needs-identity. */
#define _GNU_SOURCE
#ifdef KH_GFX_VKD3D
/* TASK38 (IOS-GRAPHICS-PLAN step 4): the memory census reads phys_footprint (task_info) on Apple and os_proc_available_memory on iOS.
 * System headers first, before vkd3d's Windows-style macros (interface, WINAPI, __stdcall) exist. */
#if defined(__APPLE__)
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
#include <os/proc.h>
#endif
#endif
#define COBJMACROS
/* The D3D12 interface GUIDs (IID_ID3D12Device ...).  vkd3d's SHARED libraries do not export them, so this file defines them (INITGUID)
 * for the whole program.  vkd3d's STATIC libraries (KH_GFX_VKD3D_STATIC: the Darwin build, Makefile) define every one of them in their
 * own objects, which are part of the link: there the definitions here would be duplicate symbols, so the libraries' are used.  An IID
 * that such a library lacked would be an undefined symbol at link time, never a zero GUID at run time. */
#ifndef KH_GFX_VKD3D_STATIC
#define INITGUID
#endif
#ifndef WIDL_C_INLINE_WRAPPERS
#define WIDL_C_INLINE_WRAPPERS
#endif
#include <vkd3d_windows.h>
#include <signal.h>
#include <vkd3d_d3d12.h>
#include <vkd3d.h>
#include <vkd3d_utils.h>
/* TASK38: MoltenVK's private API (gpuMemoryAllocated for the memory census), iPhone build only (KH_GFX_MVK_API, Makefile) */
#ifdef KH_GFX_MVK_API
#include <MoltenVK/mvk_private_api.h>
#endif
#endif
#include "khnmc_rt.h"
#include "rewind.h"
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

int kh_gfx_vkd3d_on;                       /* 1 = the D3D12 device is a vkd3d device (set once at start-up, before any guest thread) */
int kh_gfx_wrap_on;                        /* 1 = ... and the game sees it through guest-side wrapper objects (KHNMC_GFX=vkd3dw, gfx_wrap.c) */

#ifndef KH_GFX_VKD3D
/* ---- backend compiled out ------------------------------------------------------------------------------------------------ */
#define NOT_BUILT(what) kh_fatal(g,"gfx-vkd3d-not-built: %s reached but the vkd3d backend is not compiled into this binary",what)
void kh_gfx_vkd3d_init(void){
    kh_fatal(NULL,"gfx-vkd3d-not-built: KHNMC_GFX=vkd3d was requested but the vkd3d backend is not compiled into this binary. "
                  "Rebuild with GFX_VKD3D=1 (the Makefile enables it when $(VKD3D_PREFIX)/lib/libvkd3d.so exists; default prefix /home/user/work/khnmc-deps/vkd3d/usr)");
}
void kh_gfx_wrap_init(void){
    kh_fatal(NULL,"gfx-vkd3d-not-built: KHNMC_GFX=vkd3dw was requested but the vkd3d backend is not compiled into this binary. "
                  "Rebuild with GFX_VKD3D=1 (the Makefile enables it on Linux when $(VKD3D_PREFIX)/lib/libvkd3d.so exists; on macOS it is off unless "
                  "`make GFX_VKD3D=1` is given, which links vkd3d's static libraries from $(VKD3D_LIBDIR))");
}
void kh_vkd3d_create_device(KCtx *g){ NOT_BUILT("D3D12CreateDevice"); }
void kh_vkd3d_serialize_rootsig(KCtx *g){ NOT_BUILT("D3D12SerializeRootSignature"); }
void kh_vkd3d_swap_attach(KCtx *g,u64 sc,u64 queue,u64 desc){ (void)sc;(void)queue;(void)desc; NOT_BUILT("CreateSwapChainForHwnd"); }
void kh_vkd3d_swap_getbuffer(KCtx *g,u64 o){ (void)o; NOT_BUILT("IDXGISwapChain::GetBuffer"); }
void kh_vkd3d_swap_resize(KCtx *g,u64 o,u32 w,u32 h){ (void)o;(void)w;(void)h; NOT_BUILT("IDXGISwapChain::ResizeBuffers"); }
void kh_vkd3d_present(KCtx *g,u64 o,u64 n){ (void)o;(void)n; NOT_BUILT("IDXGISwapChain::Present"); }
void kh_gfx_vkd3d_report(FILE *f){ (void)f; }
void kh_gfx_vkd3d_finish(void){}
int kh_gfx_vkd3d_selftest(KCtx *g){ NOT_BUILT("the vkd3d selftest"); }
void kh_gfx_submit_counts(u64 *total,u64 *set_admitted,u64 *set_outside){ *total=*set_admitted=*set_outside=0; }   /* review A41: the KHNMC_TEST_GATE check (no backend: nothing submits) */
void kh_gfx_vkd3d_exit_report(FILE *f){ (void)f; }                    /* TASK43: no backend, no pipeline */
#else
/* ---- backend ------------------------------------------------------------------------------------------------------------- */
#include "gfx_vkd3d_int.h"
#include "gfx_pipecache.h"
#include <dlfcn.h>
#if KH_GFX_TRAMP
#include <link.h>
#endif

void kh_native_name(u64 addr,const char *iface,const char *method);
void kh_native_post(u64 addr,void (*h)(KCtx *g,const u64 *ireg,const u64 *stk,u64 rax));
void kh_native_register(const char *name,u64 lo,u64 hi); void kh_native_watch(u64 addr,_Atomic u64 *ctr,int by_rdx); void kh_native_override(u64 addr,int (*h)(KCtx *));
const char *kh_native_range_name(u64 target,u64 *off); int kh_native_range_count(void);
extern u64 kh_fiber_host_stack;
u64 kh_mk_event_h(int manual,int initial);
extern KhFn kh_lookup(u64 target);

#define LVP_ICD "/usr/share/vulkan/icd.d/lvp_icd.json"
#define DXGI_INVALID_CALL 0x887A0001u
static int drv_system;
/* TASK36 (plan step 6): KHNMC_GFX_BACKBUFFERS, see the swap-chain section */
enum { BB_OWN, BB_GENERAL, BB_COMMITTED };
static const char *const bb_names[]={"own","general","committed"};
static int bb_mode_env, bb_mode_test=-1;
static long long memlow_test=-1;   /* review X38 n=2: KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB (tests only), see the gfx-memory-low soft stop */
static _Atomic u64 n_bb_images_made, n_bb_images_freed, n_bb_drains;
static char dump_dir[1024]; static u64 dump_every=300; static _Atomic u64 dumps_written; static u64 dump_from, dump_to=~0ull;
static FILE *hash_f; static char hash_path[1024]; static _Atomic u64 hashes_written;
static FILE *band_f; static char band_path[1024]; static _Atomic u64 bands_written;   /* task 40: KHNMC_GFX_BANDLOG */
static char vk_dev_name[300]="(no Vulkan device created yet)";
static struct vkd3d_instance *vk_inst; static pthread_mutex_t vm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 n_devices, n_probe_devices, n_events_signalled, n_bundles_made, n_bundles_run, n_bundle_cmds, n_bundles_live, n_default_tables;
static int C_balloc, C_bundle;
static int h_balloc(KCtx *g,u64 o,int s,const char *c); static int h_bundle(KCtx *g,u64 o,int s,const char *c);
static void bundle_classes(void){ C_balloc=kh_com_class("vkd3d:BundleAllocator",9,COM_STRICT,h_balloc); C_bundle=kh_com_class("vkd3d:BundleCommandList",60,COM_STRICT,h_bundle); }   /* idempotent */
#define P(x) ((unsigned long long)(x))
#define U(x) ((u64)(uintptr_t)(x))

/* ---- start-up: driver selection, native ranges, dump configuration --------------------------------------------------------- */
static void driver_setup(void){
    const char *d=getenv("KHNMC_GFX_DRIVER");
#if KH_IOS_PATHS && !defined(__linux__)
    /* TASK36 (plan step 1): an Apple app build (KH_IOS_PATHS on Darwin) has exactly one Vulkan implementation, MoltenVK, linked into the
     * program; it is the default there.  On LINUX the CPU-only policy below stays exactly as it is, KH_IOS_PATHS or not (the plan
     * said "under KH_IOS_PATHS"; that would have made a Linux IOS_PATHS build pick the system driver = the real GPU). */
    if(!d||!*d){ drv_system=1; fprintf(stderr,"kh: gfx vkd3d: Apple app build: the system's Vulkan implementation (MoltenVK) is the default driver\n"); return; }
#endif
    if(d&&!strcmp(d,"system")){
        drv_system=1;
        fprintf(stderr,"kh: gfx vkd3d: KHNMC_GFX_DRIVER=system: the SYSTEM Vulkan drivers (GPU) are used; the Vulkan environment is left as it is\n");
        return;
    }
    if(d&&*d&&strcmp(d,"lavapipe")) kh_fatal(NULL,"gfx-driver-unknown: KHNMC_GFX_DRIVER=%s (use 'lavapipe' = CPU, the default, or 'system')",d);
#if !defined(__linux__)
    /* the CPU driver is selected through its Linux manifest; elsewhere the runtime does not pick a driver on its own (macOS: the Vulkan
     * implementation is MoltenVK, a GPU driver on Metal), so using the system's Vulkan is an explicit choice there, as it is on Linux */
    kh_fatal(NULL,"gfx-driver-no-cpu-driver: the CPU Vulkan driver (lavapipe) is selected through its Linux manifest %s and this is not a Linux build. "
                  "Run with KHNMC_GFX_DRIVER=system to use the system's Vulkan implementation (macOS: MoltenVK, a GPU driver)",LVP_ICD);
#endif
    FILE *f=fopen(LVP_ICD,"r");
    if(!f) kh_fatal(NULL,"gfx-lavapipe-missing: the lavapipe Vulkan driver manifest %s cannot be opened (%s); install the CPU Vulkan driver (vulkan-swrast)",LVP_ICD,strerror(errno));
    char buf[2048]; size_t n=fread(buf,1,sizeof buf-1,f); buf[n]=0; fclose(f);
    if(!strstr(buf,"libvulkan_lvp")) kh_fatal(NULL,"gfx-lavapipe-missing: %s does not name libvulkan_lvp (not the lavapipe manifest)",LVP_ICD);
    if(setenv("VK_DRIVER_FILES",LVP_ICD,1)||unsetenv("VK_ICD_FILENAMES")||unsetenv("VK_ADD_DRIVER_FILES")||setenv("VK_LOADER_LAYERS_DISABLE","~implicit~",1)||setenv("LP_NUM_THREADS","4",0))
        kh_fatal(NULL,"gfx-driver-env: cannot set the Vulkan driver environment (%s)",strerror(errno));
    fprintf(stderr,"kh: gfx vkd3d: CPU Vulkan driver selected by the runtime: VK_DRIVER_FILES=%s, implicit layers disabled, LP_NUM_THREADS=%s\n",getenv("VK_DRIVER_FILES"),getenv("LP_NUM_THREADS"));
}
#if KH_GFX_TRAMP
static int phdr_cb(struct dl_phdr_info *info,size_t size,void *data){
    (void)size; const char *nm=info->dlpi_name; if(!nm||!strstr(nm,"libvkd3d")) return 0;
    const char *base=strrchr(nm,'/'); base=base?base+1:nm;
    for(int i=0;i<info->dlpi_phnum;i++){ const ElfW(Phdr) *ph=&info->dlpi_phdr[i];
        if(ph->p_type==PT_LOAD&&(ph->p_flags&PF_X)){ u64 lo=(u64)info->dlpi_addr+ph->p_vaddr;
            kh_native_register(base,lo,lo+ph->p_memsz); (*(int*)data)++;
            fprintf(stderr,"kh: gfx vkd3d: native Windows-ABI range %s [0x%llx,0x%llx) from %s\n",base,P(lo),P(lo+ph->p_memsz),nm); } }
    return 0;
}
#endif
static void video_vblank(KCtx *g,u64 k,u64 vnow);
static const KhPcProvider gw_pc_prov;      /* TASK43: defined with the gipa wrapper */
static char video_path[1024]; static int video_scale, video_from_field, video_exit; static u64 video_from, video_lead, video_max;
static void video_setup(void){
    const char *e=getenv("KHNMC_GFX_VIDEO"); if(!e||!*e) return;
    if(!kh_vclock_on) kh_fatal(NULL,"gfx-video-needs-frames-clock: KHNMC_GFX_VIDEO writes one picture per virtual vblank and needs KHNMC_CLOCK=frames; on the host clock the pictures would have no fixed cadence");
    if(snprintf(video_path,sizeof video_path,"%s",e)>=(int)sizeof video_path-8) kh_fatal(NULL,"gfx-video: KHNMC_GFX_VIDEO is too long");
    e=getenv("KHNMC_GFX_VIDEO_FRAMES"); char *end=NULL; if(!e||!*e||(video_max=strtoull(e,&end,10),*end)||!video_max) kh_fatal(NULL,"gfx-video-frames: KHNMC_GFX_VIDEO_FRAMES=%s must be the number of pictures to write (a positive integer)",e?e:"(unset)");
    e=getenv("KHNMC_GFX_VIDEO_SCALE"); video_scale=1; if(e&&*e){ if(!strcmp(e,"2")) video_scale=2; else if(strcmp(e,"1")) kh_fatal(NULL,"gfx-video-scale: KHNMC_GFX_VIDEO_SCALE=%s (1 = full size, 2 = mean of 2x2 blocks)",e); }
    e=getenv("KHNMC_GFX_VIDEO_FROM");
    if(e&&!strncmp(e,"field",5)){ video_from_field=1; if(e[5]=='+'&&e[6]){ video_lead=strtoull(e+6,&end,10); if(*end) kh_fatal(NULL,"gfx-video-from: KHNMC_GFX_VIDEO_FROM=%s (field or field+<vblanks>)",e); } else if(e[5]) kh_fatal(NULL,"gfx-video-from: KHNMC_GFX_VIDEO_FROM=%s (field or field+<vblanks>)",e); }
    else if(e&&*e){ video_from=strtoull(e,&end,10); if(*end) kh_fatal(NULL,"gfx-video-from: KHNMC_GFX_VIDEO_FROM=%s (a virtual vblank number, field, or field+<vblanks>)",e); }
    e=getenv("KHNMC_GFX_VIDEO_EXIT"); video_exit=e&&*e=='1';
    /* The stream may be a FIFO.  A reader that goes away must end in the named fatal of video_vblank ("gfx-video-write: short
     * write"), with the exit reports, not in a silent death by SIGPIPE (review 2026-11-17).  Only for a run that writes a video
     * stream: every other run keeps the default, so `khnmc ... | head` still ends when the reader does. */
    { struct sigaction ign; memset(&ign,0,sizeof ign); ign.sa_handler=SIG_IGN; sigaction(SIGPIPE,&ign,0); }
    kh_vclock_on_vblank(video_vblank);
}
/* KHNMC_GFX=vkd3d: the trampoline bridge */
void kh_gfx_vkd3d_init(void){
#if KH_GFX_TRAMP
    driver_setup();
    int n=0; dl_iterate_phdr(phdr_cb,&n);
    if(n<3) kh_fatal(NULL,"gfx-vkd3d-libs: found %d executable segment(s) of libvkd3d*, expected the three libraries libvkd3d, libvkd3d-utils, libvkd3d-shader",n);
    kh_gfxc_init_common("vkd3d (D3D12 on Vulkan), trampoline bridge: host objects, Windows-ABI calls");
#else
    kh_fatal(NULL,"gfx-vkd3d-needs-identity: KHNMC_GFX=vkd3d hands host vkd3d objects to the lifted code and calls them through the Windows-ABI trampoline; "
                  "that exists only on x86-64 with the identity address model, and this binary was built with KH_XLATE_MODE=%d for %s. Use KHNMC_GFX=vkd3dw (the wrapped bridge)",KH_XLATE_MODE,
#if defined(__x86_64__)
                  "x86-64"
#elif defined(__aarch64__)
                  "arm64"
#else
                  "another architecture"
#endif
                  );
#endif
}
/* KHNMC_GFX=vkd3dw: the wrapped bridge (gfx_wrap.c) on the same vkd3d device, swap chain and frame output */
void kh_gfx_wrap_init(void){
    driver_setup();
    kh_wrap_classes();
    kh_gfx_wrap_on=1;
    kh_gfxc_init_common("vkd3d (D3D12 on Vulkan), WRAPPED bridge: guest-side COM objects, no Windows-ABI call");
}
/* what both bridges configure: frame dumps, the video stream, the trace switch, the fiber host stack, the bundle recorder classes */
void kh_gfxc_init_common(const char *label){
    const char *e=getenv("KHNMC_GFX_DUMP");
    if(e&&*e){ if(snprintf(dump_dir,sizeof dump_dir,"%s",e)>=(int)sizeof dump_dir) kh_fatal(NULL,"gfx-dump-dir: KHNMC_GFX_DUMP is too long");
        if(mkdir(dump_dir,0755)&&errno!=EEXIST) kh_fatal(NULL,"gfx-dump-dir: cannot create %s (%s)",dump_dir,strerror(errno)); }
    e=getenv("KHNMC_GFX_DUMP_EVERY");
    if(e&&*e){ char *end; unsigned long long v=strtoull(e,&end,10); if(*end||!v) kh_fatal(NULL,"gfx-dump-every: KHNMC_GFX_DUMP_EVERY=%s is not a positive integer",e); dump_every=v; }
    e=getenv("KHNMC_GFX_DUMP_FROM"); if(e&&*e){ char *end; dump_from=strtoull(e,&end,10); if(*end) kh_fatal(NULL,"gfx-dump-window: KHNMC_GFX_DUMP_FROM=%s is not a present number",e); }
    e=getenv("KHNMC_GFX_DUMP_TO"); if(e&&*e){ char *end; dump_to=strtoull(e,&end,10); if(*end||dump_to<dump_from) kh_fatal(NULL,"gfx-dump-window: KHNMC_GFX_DUMP_TO=%s is not a present number at or after KHNMC_GFX_DUMP_FROM=%llu",e,P(dump_from)); }
    e=getenv("KHNMC_GFX_BACKBUFFERS"); bb_mode_env=BB_OWN;                  /* TASK36: own VkImages (default), general, committed */
    if(e&&*e){ if(!strcmp(e,"own")) bb_mode_env=BB_OWN; else if(!strcmp(e,"general")) bb_mode_env=BB_GENERAL; else if(!strcmp(e,"committed")) bb_mode_env=BB_COMMITTED;
        else kh_fatal(NULL,"gfx-backbuffers-mode: KHNMC_GFX_BACKBUFFERS=%s (own = our VkImages, PRESENT = TRANSFER_SRC; general = our VkImages, PRESENT = GENERAL; committed = vkd3d's resources, the old path)",e); }
    e=getenv("KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB");                     /* review X38 n=2: TESTS ONLY, a reading for the gfx-memory-low soft stop */
    if(e&&*e){ char *end; unsigned long long v=strtoull(e,&end,10); if(*end||e[0]<'0'||e[0]>'9'||v>(1ull<<40)) kh_fatal(NULL,"gfx-memory-test: KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB=%s is not a number of MiB",e); memlow_test=(long long)v; }
    e=getenv("KHNMC_GFX_HASHLOG");
    if(e&&*e){ if(snprintf(hash_path,sizeof hash_path,"%s",e)>=(int)sizeof hash_path) kh_fatal(NULL,"gfx-hashlog: KHNMC_GFX_HASHLOG is too long");
        hash_f=fopen(hash_path,"w"); if(!hash_f) kh_fatal(NULL,"gfx-hashlog: cannot create %s (%s)",hash_path,strerror(errno));
        setvbuf(hash_f,NULL,_IOLBF,0); fprintf(hash_f,"present\tgame_frame\tvblank\trgb_hash\tbgra_hash\tdraws_total\twidth\theight\n"); }
    e=getenv("KHNMC_GFX_BANDLOG");                                          /* task 40 (the fill mode's GPU checks): per present, the 16:9 band and the strips */
    if(e&&*e){ if(snprintf(band_path,sizeof band_path,"%s",e)>=(int)sizeof band_path) kh_fatal(NULL,"gfx-bandlog: KHNMC_GFX_BANDLOG is too long");
        band_f=fopen(band_path,"w"); if(!band_f) kh_fatal(NULL,"gfx-bandlog: cannot create %s (%s)",band_path,strerror(errno));
        setvbuf(band_f,NULL,_IOLBF,0);
        fprintf(band_f,"present\tgame_frame\twidth\theight\tband_top_row\tband_rows\tband_hash\ttop_hash\tbottom_hash\tband_nonblack_pct\ttop_nonblack_pct\tbottom_nonblack_pct\tband_mean\ttop_mean\tbottom_mean\tedge_top\tedge_bottom\tband_step_p95\tedge_window_rows\tedge_top_max\tedge_bottom_max\n"); }
    video_setup();
    e=getenv("KHNMC_GFX_TRACE"); kh_native_trace=e&&*e=='1';
    kh_fiber_host_stack=16ull<<20;          /* vkd3d and the Vulkan driver run on the calling fiber's host stack */
    kh_pc_set_provider(&gw_pc_prov);        /* TASK43 (step 12): how the disk pipeline cache gets the driver's bytes (only at its two save points) */
    /* review C43 n=2: KHNMC_GFX_VKD3D_LOG_REPEATS=n: vkd3d's lines go through the runtime's de-duplicating filter (null_d3d12.c), so play
     * mode can keep vkd3d at warn, where its failures are logged.  As shared objects (Linux) each of the three libraries has its own
     * callback; libvkd3d-shader is not on this binary's link line, so its setter is looked up (it is loaded as libvkd3d's dependency).  In
     * a static link (the iPhone app) the three share ONE callback (libvkd3d-common's debug.c), and the first call sets it for all */
    { extern int kh_vklog_setup(void); extern void kh_vklog_callback(const char*,va_list);
      if(kh_vklog_setup()){ vkd3d_set_log_callback(kh_vklog_callback); vkd3d_utils_set_log_callback(kh_vklog_callback);
          void (*ssl)(PFN_vkd3d_log)=(void(*)(PFN_vkd3d_log))dlsym(RTLD_DEFAULT,"vkd3d_shader_set_log_callback"); if(ssl) ssl(kh_vklog_callback);
          fprintf(stderr,"kh: gfx vkd3d log: the filter is installed in libvkd3d and libvkd3d-utils%s\n",ssl?", and in libvkd3d-shader (its setter was found by name)":"; vkd3d_shader_set_log_callback was not found by name (a static link, where one callback serves all three; otherwise the shader library's lines are printed unfiltered)"); } }
    bundle_classes();
    kh_gfx_vkd3d_on=1;
    fprintf(stderr,"kh: gfx backend: %s; address model: %s; frame dump: %s%s every %llu presents",label,KH_XLATE_MODE==0?"identity":"bias",dump_dir[0]?"to ":"off",dump_dir,P(dump_every));
    if(dump_from||dump_to!=~0ull) fprintf(stderr,", presents %llu..%llu only",P(dump_from),P(dump_to));
    if(hash_f) fprintf(stderr,"; picture hash of every present: %s",hash_path);
    if(band_f) fprintf(stderr,"; 16:9 band and strips of every present: %s",band_path);
    fprintf(stderr,"; back buffers: %s",bb_names[bb_mode_env]);
    if(video_path[0]) fprintf(stderr,"; video stream: %s, %llu pictures from %s%s%llu, scale 1/%d%s",video_path,P(video_max),video_from_field?"the field":"virtual vblank ",video_from_field?" + ":"",P(video_from_field?video_lead:video_from),video_scale,video_exit?", then exit":"");
    fputc('\n',stderr);
}

/* ---- vkd3d instance --------------------------------------------------------------------------------------------------------- */
/* called by vkd3d's fence worker thread: SetEvent, as Windows does when the fence reaches the value.  kh_event_set holds a reference
 * to the event while it signals (a CloseHandle on another thread meanwhile is legal) and releases one blocked waiter of an auto-reset
 * event per completion (review U35 n=2: the bit was set here directly, so two completions before the woken waiter ran released one) */
static HRESULT sig_event(HANDLE h){
    if(!kh_event_set(U(h))) kh_fatal(NULL,"gfx-fence-event-handle: a fence completed and wants to signal handle %p, which is not an event of this runtime",h);
    atomic_fetch_add(&n_events_signalled,1); return S_OK;
}
static const char *pd_type(VkPhysicalDeviceType t){ switch(t){ case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU"; case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "DISCRETE_GPU"; case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "VIRTUAL_GPU"; default: return "OTHER"; } }
/* The Vulkan library: loaded by US and handed to vkd3d (pfn_vkGetInstanceProcAddr), so the device check below and vkd3d use the same
 * implementation on every OS.  Names in vkd3d's own search order (m4/check-vulkan.m4): the loader, else MoltenVK itself on macOS. */
#if defined(__APPLE__)
static const char *const vk_lib_names[]={"libvulkan.1.dylib","libMoltenVK.dylib"};
#else
static const char *const vk_lib_names[]={"libvulkan.so.1"};
#endif
static PFN_vkGetInstanceProcAddr vk_gipa; static const char *vk_lib_name, *vk_lib_from="";
PFN_vkGetInstanceProcAddr kh_gfxc_vk_gipa_real(void){ return vk_gipa; }
const char *kh_gfxc_dump_dir(void){ return dump_dir; }

/* ---- TASK36, plan step 1: the gipa wrapper seam and the presentation extensions ----------------------------------------------------
 * vkd3d gets kh_vk_gipa_wrap as its vkGetInstanceProcAddr.  vkd3d loads its global and instance functions through it, and every device
 * function through the vkGetDeviceProcAddr it gets there (vkd3d utils.c 814-864), so this is the one place where later steps can
 * intercept (step 9: VkLayerSettingsCreateInfoEXT chained on a COPY of vkd3d's VkInstanceCreateInfo, whose pNext vkd3d leaves NULL;
 * step 12: vkGetDeviceProcAddr for the pipeline cache; step 11: a vkQueueSubmit counter).  Today it passes everything through: the
 * two observers below call the real vkCreateInstance / vkCreateDevice with the caller's arguments unchanged and only record which
 * extensions vkd3d enabled, for the report line and the selftest.  A name it resolves for itself (vkGetInstanceProcAddr) is answered
 * with the wrapper, so nothing reaches the real one around it. */
KhPresExt kh_gfxc_pres_ext;
static _Atomic u64 gw_names, gw_inst_calls, gw_dev_calls;
static PFN_vkCreateInstance gw_real_ci; static PFN_vkCreateDevice gw_real_cd;
static VkInstance gw_instance; static void gw_dev_note(VkPhysicalDevice pd,VkDevice d);   /* TASK43 */
static int has_name(const char *const *v,u32 n,const char *s){ for(u32 i=0;i<n;i++) if(v[i]&&!strcmp(v[i],s)) return 1; return 0; }
static VkResult VKAPI_PTR gw_create_instance(const VkInstanceCreateInfo *ci,const VkAllocationCallbacks *a,VkInstance *out){
    atomic_fetch_add(&gw_inst_calls,1);
    VkResult r=gw_real_ci(ci,a,out);
    if(r==VK_SUCCESS&&out) gw_instance=*out;                              /* TASK43: the device identity of a pipeline cache is read through it */
    if(r==VK_SUCCESS&&ci){ const char *const *e=ci->ppEnabledExtensionNames; u32 n=ci->enabledExtensionCount;
        kh_gfxc_pres_ext.surface=has_name(e,n,"VK_KHR_surface"); kh_gfxc_pres_ext.headless=has_name(e,n,"VK_EXT_headless_surface");
        kh_gfxc_pres_ext.metal=has_name(e,n,"VK_EXT_metal_surface"); kh_gfxc_pres_ext.layer_settings=has_name(e,n,"VK_EXT_layer_settings"); kh_gfxc_pres_ext.seen_instance=1; }
    return r;
}
static VkResult VKAPI_PTR gw_create_device(VkPhysicalDevice pd,const VkDeviceCreateInfo *ci,const VkAllocationCallbacks *a,VkDevice *out){
    atomic_fetch_add(&gw_dev_calls,1);
    VkResult r=gw_real_cd(pd,ci,a,out);
    if(r==VK_SUCCESS&&out) gw_dev_note(pd,*out);                           /* TASK43: which physical device this VkDevice is on (the pipeline cache's identity) */
    if(r==VK_SUCCESS&&ci){ const char *const *e=ci->ppEnabledExtensionNames; u32 n=ci->enabledExtensionCount;
        kh_gfxc_pres_ext.swapchain=has_name(e,n,"VK_KHR_swapchain"); kh_gfxc_pres_ext.display_timing=has_name(e,n,"VK_GOOGLE_display_timing");
        kh_gfxc_pres_ext.metal_objects=has_name(e,n,"VK_EXT_metal_objects"); kh_gfxc_pres_ext.seen_device=1; }
    return r;
}
/* TASK41 (plan step 11, fact 11): the vkQueueSubmit counter.  vkd3d loads every device function through the vkGetDeviceProcAddr it
 * resolves here (vulkan_procs.h 41, utils.c 856-864), so the wrapper answers vkGetDeviceProcAddr with gw_gdpa, which answers
 * vkGetDeviceQueue (to learn which real vkQueueSubmit belongs to each VkQueue) and vkQueueSubmit with pass-throughs.  Every submission
 * vkd3d makes is counted; one made while the suspend gate is set is counted as admitted (a region that was let in, or the suspend drain)
 * or as OUTSIDE an admission, the case the gate exists to exclude (e.g. queued work flushed by a CPU fence signal that nobody held:
 * plan fact 11).  The presenter's own submissions do not come through here (it resolves its functions with the real loader). */
static PFN_vkGetDeviceProcAddr gw_real_gdpa;
typedef struct { VkQueue q; PFN_vkQueueSubmit submit; } GwQueue;
static GwQueue gw_q[32]; static int gw_nq; static pthread_mutex_t gw_qm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 gw_submits, gw_sub_set_admitted, gw_sub_set_outside, gw_queues_seen;
static void VKAPI_PTR gw_get_device_queue(VkDevice d,uint32_t fam,uint32_t idx,VkQueue *q){
    PFN_vkGetDeviceQueue real=(PFN_vkGetDeviceQueue)gw_real_gdpa(d,"vkGetDeviceQueue"); PFN_vkQueueSubmit sub=(PFN_vkQueueSubmit)gw_real_gdpa(d,"vkQueueSubmit");
    if(!real||!sub) kh_fatal(NULL,"gfx-gipa-wrapper: the Vulkan device has no vkGetDeviceQueue / vkQueueSubmit");
    real(d,fam,idx,q); atomic_fetch_add(&gw_queues_seen,1);
    pthread_mutex_lock(&gw_qm); int i; for(i=0;i<gw_nq;i++) if(gw_q[i].q==*q) break;
    if(i==gw_nq){ if(gw_nq>=32){ pthread_mutex_unlock(&gw_qm); kh_fatal(NULL,"gfx-gipa-wrapper: more than 32 VkQueues"); } gw_nq++; }
    gw_q[i].q=*q; gw_q[i].submit=sub; pthread_mutex_unlock(&gw_qm);
}
static VkResult VKAPI_PTR gw_queue_submit(VkQueue q,uint32_t n,const VkSubmitInfo *si,VkFence f){
    PFN_vkQueueSubmit real=NULL; pthread_mutex_lock(&gw_qm); for(int i=0;i<gw_nq;i++) if(gw_q[i].q==q){ real=gw_q[i].submit; break; } pthread_mutex_unlock(&gw_qm);
    if(!real) kh_fatal(NULL,"gfx-gipa-submit-queue: vkQueueSubmit on a VkQueue that vkGetDeviceQueue never returned through the gipa wrapper");
    atomic_fetch_add(&gw_submits,1);
    if(kh_gfx_gate_is_set()){
        if(kh_gfx_gate_thread_admitted()) atomic_fetch_add(&gw_sub_set_admitted,1);
        else if(atomic_fetch_add(&gw_sub_set_outside,1)<8) fprintf(stderr,"kh: gfx gate: vkQueueSubmit by vkd3d while the suspend gate is set, on a thread that is NOT admitted (counted; the gate does not hold this path)\n"); }
    return real(q,n,si,f);
}
void kh_gfx_submit_counts(u64 *total,u64 *set_admitted,u64 *set_outside){ *total=atomic_load(&gw_submits); *set_admitted=atomic_load(&gw_sub_set_admitted); *set_outside=atomic_load(&gw_sub_set_outside); }
/* ---- TASK43 (IOS-GRAPHICS-PLAN step 12): the disk pipeline cache and the pipeline-creation counters, in the gipa wrapper ------------
 * vkd3d makes ONE VkPipelineCache per device, with no initial data (device.c 2409-2430), passes it to every vkCreateGraphicsPipelines
 * (state.c 4124; pipelines are made at the first draw that needs them, d3d12_pipeline_state_get_or_create_pipeline), passes
 * VK_NULL_HANDLE to vkCreateComputePipelines (state.c 2509-2510), and never calls vkGetPipelineCacheData or vkMergePipelineCaches.
 * So gw_gdpa answers five more names:
 *   vkCreatePipelineCache      the real one, with pInitialData from the configured file when its own header says it was written by THIS
 *                              physical device (gfx_pipecache.c); not for a capability probe (D3D12CreateDevice with ppDevice NULL makes
 *                              and destroys a whole device: on MoltenVK its cache would compile every stored shader for nothing).  If
 *                              the driver refuses bytes that passed our checks, the real call is made again without them.
 *   vkDestroyPipelineCache, vkDestroyDevice   the table below forgets the object (never while a save reads it).
 *   vkCreateGraphicsPipelines, vkCreateComputePipelines   counted and timed (wall time, and the calling thread's CPU time: a driver
 *                              that compiles on other threads or in another process, as Metal does, shows in the wall time only).
 * The save (gfx_pipecache.c, only while the game is parked or at exit) fetches vkGetPipelineCacheData of the newest live cache of a
 * device that is not a probe, through pc_fetch below.  While it does, no pipeline is created: a creation call that arrives waits on
 * gw_dcv for the end of the fetch, and the fetch first waits, bounded, for the calls already inside the driver.  That makes the
 * two-call size / data sequence exact (MoltenVK's writeData sizes its buffer from a count taken at the size call: MVKPipeline.mm
 * writeDataImpl) and needs no promise from the driver about concurrent use. */
#define GW_NDEV 8
#define GW_SAVE_WAIT_SLICES 6u                 /* the fetch waits at most this many slices of GW_SAVE_SLICE_MS for creation calls in flight */
#define GW_SAVE_SLICE_MS 50u
typedef struct { VkDevice d; int probe, have_id; KhPcDev id; VkPipelineCache cache; u64 seq, creations, saved;
    PFN_vkCreatePipelineCache create_cache; PFN_vkDestroyPipelineCache destroy_cache; PFN_vkGetPipelineCacheData get_data;
    PFN_vkCreateGraphicsPipelines create_gp; PFN_vkCreateComputePipelines create_cp; PFN_vkDestroyDevice destroy_dev; } GwDev;
typedef struct { u64 calls, pipelines, failed, wall_ns, wall_max_ns, cpu_ns, over16, with_cache; } GwPipeStat;
static GwDev gw_dev[GW_NDEV]; static pthread_mutex_t gw_dm=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t gw_dcv; static pthread_once_t gw_donce=PTHREAD_ONCE_INIT;
static int gw_saving, gw_inflight, gw_test_refuse_next; static GwDev *gw_save_dev; static u64 gw_save_mark, gw_cache_seq;
static GwPipeStat gw_gp, gw_cp; static u64 gw_caches_made, gw_caches_destroyed, gw_cache_ns, gw_devs_noted, gw_pipe_waits, gw_destroy_waits, gw_fail_said;
/* selftest hooks (review C43 n=3 / n=8, n=2): the fetch stands between its size call and its data call (1 = armed, 2 = it stands there,
 * until 3); the next data call is made to look like VK_INCOMPLETE (1), 0 bytes (2), more bytes than announced (3); the next
 * vkCreateGraphicsPipelines fails without reaching the driver */
static _Atomic int gw_test_fetch_pause; static int gw_test_data_fault, gw_test_fail_gp_next; static unsigned gw_fetch_slices_last;
static u64 gw_init_bytes_last, gw_fetch_data_calls;      /* what the last real vkCreatePipelineCache was handed; data-phase calls of vkGetPipelineCacheData */
static __thread int gw_probe_tls;              /* this thread is creating a capability-probe device (kh_vkd3d_create_device) */
static void gw_dinit(void){ kh_cond_init(&gw_dcv); }
/* caller holds gw_dm: the entry of d (made when create is set and none exists) */
static GwDev *gw_dev_of(VkDevice d,int create){
    GwDev *freee=NULL; for(int i=0;i<GW_NDEV;i++){ if(gw_dev[i].d==d&&d) return &gw_dev[i]; if(!gw_dev[i].d&&!freee) freee=&gw_dev[i]; }
    if(!create) return NULL;
    if(!freee){ pthread_mutex_unlock(&gw_dm); kh_fatal(NULL,"gfx-gipa-wrapper: more than %d Vulkan devices alive at once",GW_NDEV); }
    memset(freee,0,sizeof *freee); freee->d=d; return freee;
}
static void gw_dev_note(VkPhysicalDevice pd,VkDevice d){
    pthread_once(&gw_donce,gw_dinit);
    PFN_vkGetPhysicalDeviceProperties gp=gw_instance?(PFN_vkGetPhysicalDeviceProperties)vk_gipa(gw_instance,"vkGetPhysicalDeviceProperties"):NULL;
    VkPhysicalDeviceProperties pp; memset(&pp,0,sizeof pp); if(gp) gp(pd,&pp);
    pthread_mutex_lock(&gw_dm); GwDev *e=gw_dev_of(d,1); e->probe=gw_probe_tls; e->have_id=gp!=NULL;
    memcpy(e->id.uuid,pp.pipelineCacheUUID,16); e->id.vendor=pp.vendorID; e->id.device=pp.deviceID; e->id.driver=pp.driverVersion; gw_devs_noted++;
    pthread_mutex_unlock(&gw_dm);
}
static u64 thread_cpu_ns(void){ struct timespec ts; if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)) return 0; return (u64)ts.tv_sec*1000000000ull+(u64)ts.tv_nsec; }
static VkResult VKAPI_PTR gw_create_pipeline_cache(VkDevice d,const VkPipelineCacheCreateInfo *ci,const VkAllocationCallbacks *a,VkPipelineCache *out){
    pthread_once(&gw_donce,gw_dinit);
    pthread_mutex_lock(&gw_dm); GwDev *e=gw_dev_of(d,1); PFN_vkCreatePipelineCache real=e->create_cache; KhPcDev id=e->id; int have_id=e->have_id, probe=e->probe; pthread_mutex_unlock(&gw_dm);
    if(!real) kh_fatal(NULL,"gfx-gipa-wrapper: vkCreatePipelineCache on a VkDevice whose function was never resolved through the gipa wrapper");
    VkPipelineCacheCreateInfo c=*ci; uint8_t *file=NULL; const uint8_t *pl=NULL; size_t n=0; int lr=KH_PC_MISSING, tried=0;
    if(kh_pc_active()&&!ci->initialDataSize){
        if(probe) kh_pc_note_probe_skipped();
        else if(!have_id) fprintf(stderr,"kh: gfx pipeline cache: the physical device of this VkDevice is unknown to the gipa wrapper (vkCreateDevice did not come through it): no cache data is handed over (an empty cache)\n");
        else { tried=1; lr=kh_pc_load(&id,&file,&pl,&n); } }
    if(tried&&lr==KH_PC_OK){ c.initialDataSize=n; c.pInitialData=pl; }
    gw_init_bytes_last=(u64)c.initialDataSize;
    u64 t0=kh_host_ns(); VkResult r=real(d,&c,a,out); u64 dt=kh_host_ns()-t0;
    if(tried&&lr==KH_PC_OK){
        if(r==VK_SUCCESS&&gw_test_refuse_next){ gw_test_refuse_next=0;           /* selftest: a driver that refuses the bytes */
            pthread_mutex_lock(&gw_dm); PFN_vkDestroyPipelineCache dc=e->destroy_cache; pthread_mutex_unlock(&gw_dm);
            if(dc){ dc(d,*out,a); *out=VK_NULL_HANDLE; r=VK_ERROR_INITIALIZATION_FAILED; } }
        if(r==VK_SUCCESS) kh_pc_note_loaded(n,kh_pc_fnv(pl,n),&id,dt);
        else { kh_pc_note_driver_refused(n,(int)r); t0=kh_host_ns(); r=real(d,ci,a,out); dt=kh_host_ns()-t0; } }   /* vkd3d's own call: an empty cache */
    free(file);
    pthread_mutex_lock(&gw_dm); gw_cache_ns+=dt;
    if(r==VK_SUCCESS){ e=gw_dev_of(d,1); e->cache=*out; e->seq=++gw_cache_seq; e->creations=e->saved=0; gw_caches_made++; }
    pthread_mutex_unlock(&gw_dm);
    return r;
}
/* caller holds gw_dm: a save that is reading the driver's cache finishes first (*waits counts the callers that found one) */
static void gw_wait_save(u64 *waits){ if(gw_saving){ (*waits)++; while(gw_saving) pthread_cond_wait(&gw_dcv,&gw_dm); } }
static void VKAPI_PTR gw_destroy_pipeline_cache(VkDevice d,VkPipelineCache pc,const VkAllocationCallbacks *a){
    pthread_once(&gw_donce,gw_dinit);
    pthread_mutex_lock(&gw_dm); gw_wait_save(&gw_destroy_waits); GwDev *e=gw_dev_of(d,0); PFN_vkDestroyPipelineCache real=e?e->destroy_cache:NULL;
    if(e&&pc&&e->cache==pc){ e->cache=VK_NULL_HANDLE; gw_caches_destroyed++; }
    pthread_mutex_unlock(&gw_dm);
    if(!real) kh_fatal(NULL,"gfx-gipa-wrapper: vkDestroyPipelineCache on a VkDevice the gipa wrapper does not know");
    real(d,pc,a);
}
static void VKAPI_PTR gw_destroy_device(VkDevice d,const VkAllocationCallbacks *a){
    pthread_once(&gw_donce,gw_dinit);
    pthread_mutex_lock(&gw_dm); gw_wait_save(&gw_destroy_waits); GwDev *e=gw_dev_of(d,0); PFN_vkDestroyDevice real=e?e->destroy_dev:NULL; if(e) memset(e,0,sizeof *e); pthread_mutex_unlock(&gw_dm);
    if(!real){ real=(PFN_vkDestroyDevice)gw_real_gdpa(d,"vkDestroyDevice"); if(!real) kh_fatal(NULL,"gfx-gipa-wrapper: the Vulkan device has no vkDestroyDevice"); }
    real(d,a);
}
/* a pipeline-creation call enters (waits while a save reads the cache) and leaves (counted, timed) */
static void gw_pipe_enter(void){ pthread_once(&gw_donce,gw_dinit); pthread_mutex_lock(&gw_dm); gw_wait_save(&gw_pipe_waits); gw_inflight++; pthread_mutex_unlock(&gw_dm); }
static void gw_pipe_leave(GwPipeStat *st,VkDevice d,VkPipelineCache pc,u32 n,VkResult r,u64 wall,u64 cpu){
    pthread_mutex_lock(&gw_dm); gw_inflight--; u64 failed=0; int say=0;
    if(st){ st->calls++; st->pipelines+=n; if(r<0){ failed=++st->failed; say=gw_fail_said<8; } st->wall_ns+=wall; if(wall>st->wall_max_ns) st->wall_max_ns=wall; st->cpu_ns+=cpu; if(wall>=16000000ull) st->over16++;
        GwDev *e=gw_dev_of(d,0); if(e&&pc&&e->cache==pc){ e->creations++; st->with_cache++; } }
    if(!gw_inflight) pthread_cond_broadcast(&gw_dcv);
    pthread_mutex_unlock(&gw_dm);
    /* review C43 n=2: a failed creation is said HERE, when it happens (vkd3d says it at class warn only, and the exit report is not reached
     * by a play session that the system ends): vkd3d drops every draw that needs this pipeline */
    if(say){ fprintf(stderr,"kh: gfx pipelines: pipeline-create-failed: %s returned VkResult %d for %u pipeline(s) (%.1f ms): vkd3d drops the draws or dispatches that need it (counted: %llu failed call(s) of this kind so far; the first 8 failures are printed)\n",
                     st==&gw_gp?"vkCreateGraphicsPipelines":"vkCreateComputePipelines",(int)r,n,wall/1e6,P(failed));
        pthread_mutex_lock(&gw_dm); gw_fail_said++; pthread_mutex_unlock(&gw_dm); }        /* gw_fail_said = the lines printed */
}
static VkResult VKAPI_PTR gw_create_gp(VkDevice d,VkPipelineCache pc,uint32_t n,const VkGraphicsPipelineCreateInfo *ci,const VkAllocationCallbacks *a,VkPipeline *out){
    pthread_mutex_lock(&gw_dm); GwDev *e=gw_dev_of(d,0); PFN_vkCreateGraphicsPipelines real=e?e->create_gp:NULL; pthread_mutex_unlock(&gw_dm);
    if(!real) kh_fatal(NULL,"gfx-gipa-wrapper: vkCreateGraphicsPipelines on a VkDevice the gipa wrapper does not know");
    gw_pipe_enter(); u64 w0=kh_host_ns(), c0=thread_cpu_ns(); VkResult r;
    if(__builtin_expect(gw_test_fail_gp_next,0)){ gw_test_fail_gp_next=0; for(u32 i=0;i<n;i++) out[i]=VK_NULL_HANDLE; r=VK_ERROR_OUT_OF_HOST_MEMORY; }   /* selftest: a driver that fails the creation */
    else r=real(d,pc,n,ci,a,out);
    u64 w1=kh_host_ns(), c1=thread_cpu_ns();
    gw_pipe_leave(&gw_gp,d,pc,n,r,w1-w0,c1-c0); return r;
}
static VkResult VKAPI_PTR gw_create_cp(VkDevice d,VkPipelineCache pc,uint32_t n,const VkComputePipelineCreateInfo *ci,const VkAllocationCallbacks *a,VkPipeline *out){
    pthread_mutex_lock(&gw_dm); GwDev *e=gw_dev_of(d,0); PFN_vkCreateComputePipelines real=e?e->create_cp:NULL; pthread_mutex_unlock(&gw_dm);
    if(!real) kh_fatal(NULL,"gfx-gipa-wrapper: vkCreateComputePipelines on a VkDevice the gipa wrapper does not know");
    gw_pipe_enter(); u64 w0=kh_host_ns(), c0=thread_cpu_ns(); VkResult r=real(d,pc,n,ci,a,out); u64 w1=kh_host_ns(), c1=thread_cpu_ns();
    gw_pipe_leave(&gw_cp,d,pc,n,r,w1-w0,c1-c0); return r;
}
/* caller holds gw_dm: the newest live cache of a device that is not a probe (the game has one device; a test may have more) */
static GwDev *gw_cache_newest(void){ GwDev *b=NULL; for(int i=0;i<GW_NDEV;i++){ GwDev *e=&gw_dev[i]; if(e->d&&e->cache&&!e->probe&&(!b||e->seq>b->seq)) b=e; } return b; }
static int pc_dirty(void){ pthread_once(&gw_donce,gw_dinit); pthread_mutex_lock(&gw_dm); GwDev *e=gw_cache_newest(); int r=e?(e->creations!=e->saved):-1; pthread_mutex_unlock(&gw_dm); return r; }
static void pc_end_locked(void){ gw_saving=0; gw_save_dev=NULL; pthread_cond_broadcast(&gw_dcv); }
/* review C43 n=11: the slices of GW_SAVE_SLICE_MS a fetch may wait for creation calls in flight: all of them with no deadline, else what
 * fits before the caller's deadline less the save's margin (0 = it does not wait at all) */
static unsigned gw_wait_slices(u64 now_ns,u64 deadline_ns){
    if(!deadline_ns) return GW_SAVE_WAIT_SLICES;
    u64 margin=(u64)KH_PC_SAVE_MARGIN_MS*1000000ull; if(now_ns+margin>=deadline_ns) return 0;
    u64 k=(deadline_ns-now_ns-margin)/((u64)GW_SAVE_SLICE_MS*1000000ull); return k<GW_SAVE_WAIT_SLICES?(unsigned)k:GW_SAVE_WAIT_SLICES;
}
static int pc_fetch(uint64_t cap,uint64_t deadline_ns,void **data,size_t *len,KhPcDev *dev,char *why,size_t whycap){
    pthread_once(&gw_donce,gw_dinit); *data=NULL; *len=0;
    pthread_mutex_lock(&gw_dm);
    if(gw_saving){ pthread_mutex_unlock(&gw_dm); snprintf(why,whycap,"another fetch of the cache data is running"); return KH_PCF_BUSY; }
    GwDev *e=gw_cache_newest();
    if(!e){ pthread_mutex_unlock(&gw_dm); snprintf(why,whycap,"no VkPipelineCache of a game device is alive"); return KH_PCF_NONE; }
    gw_saving=1;                                                  /* from now on a creation call that arrives waits */
    unsigned expired=0, allow=gw_wait_slices(kh_host_ns(),deadline_ns); gw_fetch_slices_last=allow;
    while(gw_inflight){
        if(expired>=allow){ int k=gw_inflight; pc_end_locked(); pthread_mutex_unlock(&gw_dm);
            snprintf(why,whycap,"%d pipeline-creation call(s) were still inside the driver after %u ms%s",k,expired*GW_SAVE_SLICE_MS,allow<GW_SAVE_WAIT_SLICES?" (the wait was cut to the caller's bound)":""); return KH_PCF_BUSY; }
        struct timespec dl; kh_mono_deadline_ms(&dl,GW_SAVE_SLICE_MS);
        if(kh_cond_wait_until(&gw_dcv,&gw_dm,&dl)==ETIMEDOUT&&gw_inflight) expired++; }
    VkDevice d=e->d; VkPipelineCache pc=e->cache; PFN_vkGetPipelineCacheData get=e->get_data; *dev=e->id; gw_save_dev=e; gw_save_mark=e->creations;
    pthread_mutex_unlock(&gw_dm);                                 /* e, d and pc stay valid: the destroy calls wait while gw_saving */
    int res=KH_PCF_FAILED; size_t n=0; void *b=NULL;
    if(!get){ get=(PFN_vkGetPipelineCacheData)gw_real_gdpa(d,"vkGetPipelineCacheData"); }
    if(!get) snprintf(why,whycap,"the Vulkan device has no vkGetPipelineCacheData");
    else { VkResult r=get(d,pc,&n,NULL);
        if(r!=VK_SUCCESS) snprintf(why,whycap,"vkGetPipelineCacheData (size) failed: VkResult %d",(int)r);
        else if(!n){ snprintf(why,whycap,"the driver's cache holds no data (0 bytes)"); res=KH_PCF_NONE; }
        else if((uint64_t)n>cap){ *len=n; res=KH_PCF_TOO_LARGE; }
        else if(!(b=malloc(n))) snprintf(why,whycap,"out of memory for %zu bytes of cache data",n);
        else { if(__builtin_expect(atomic_load(&gw_test_fetch_pause)==1,0)){ atomic_store(&gw_test_fetch_pause,2);   /* selftest: between the size call and the data call, the exclusion held */
                   while(atomic_load(&gw_test_fetch_pause)==2){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } }
            size_t m=n; gw_fetch_data_calls++; r=get(d,pc,&m,b);
            if(__builtin_expect(gw_test_data_fault,0)){ int k=gw_test_data_fault; gw_test_data_fault=0; if(k==1) r=VK_INCOMPLETE; else if(k==2) m=0; else m=n+1; }
            /* VK_SUCCESS with FEWER bytes than the size call announced is the driver's right (the size call gives a maximum; lavapipe
             * announces 32 and writes 20): those bytes are the data.  More than announced, none at all, or VK_INCOMPLETE is a failure */
            if(r!=VK_SUCCESS||!m||m>n){ snprintf(why,whycap,"vkGetPipelineCacheData wrote %zu bytes after announcing %zu (VkResult %d)",m,n,(int)r); free(b); b=NULL; }
            else { *data=b; *len=m; res=KH_PCF_OK; } } }
    if(res!=KH_PCF_OK){ pthread_mutex_lock(&gw_dm); pc_end_locked(); pthread_mutex_unlock(&gw_dm); }      /* only an OK fetch keeps the exclusion until done() */
    return res;
}
static void pc_done(int committed){ pthread_mutex_lock(&gw_dm); if(committed&&gw_save_dev) gw_save_dev->saved=gw_save_mark; pc_end_locked(); pthread_mutex_unlock(&gw_dm); }
static const KhPcProvider gw_pc_prov={pc_dirty,pc_fetch,pc_done};
/* the exit report's lines of this backend (null_d3d12.c kh_gfx_exit_report): pipeline creation, and on the iPhone MoltenVK's own
 * shader statistics (the same private API the memory census reads; they need MVK_CONFIG_PERFORMANCE_TRACKING=1) */
static ID3D12Device *exit_stat_dev(void);
void kh_gfx_vkd3d_exit_report(FILE *f){
    pthread_mutex_lock(&gw_dm); GwPipeStat g=gw_gp, c=gw_cp; u64 made=gw_caches_made, gone=gw_caches_destroyed, cns=gw_cache_ns, waits=gw_pipe_waits, dwaits=gw_destroy_waits; pthread_mutex_unlock(&gw_dm);
    fprintf(f,"kh: gfx pipelines (the gipa wrapper's count): vkCreateGraphicsPipelines %llu call(s), %llu pipeline(s), %llu failed, %llu through vkd3d's VkPipelineCache: %.1f ms of wall time in all, longest call %.1f ms, %llu call(s) of 16 ms or more, CPU time of the calling threads %.1f ms; "
              "vkCreateComputePipelines %llu call(s), %llu pipeline(s), %llu failed, %.1f ms (vkd3d gives those no cache); VkPipelineCache objects made %llu (%.1f ms inside vkCreatePipelineCache), destroyed %llu; creation calls that waited for a cache save %llu, destroy calls that waited for one %llu\n",
            P(g.calls),P(g.pipelines),P(g.failed),P(g.with_cache),g.wall_ns/1e6,g.wall_max_ns/1e6,P(g.over16),g.cpu_ns/1e6,P(c.calls),P(c.pipelines),P(c.failed),c.wall_ns/1e6,P(made),cns/1e6,P(gone),P(waits),P(dwaits));
#ifdef KH_GFX_MVK_API
    { ID3D12Device *d=exit_stat_dev();
      if(!d) fprintf(f,"kh: gfx MoltenVK shader statistics: absent (no live swap chain to read them from)\n");
      else { MVKPerformanceStatistics ps; memset(&ps,0,sizeof ps); size_t sz=sizeof ps; VkResult r=vkGetPerformanceStatisticsMVK(vkd3d_get_vk_device(d),&ps,&sz);
        if(r!=VK_SUCCESS) fprintf(f,"kh: gfx MoltenVK shader statistics: ERROR (vkGetPerformanceStatisticsMVK result %d, %zu of %zu bytes: MoltenVK's private API differs from the headers' version %d)\n",(int)r,sz,sizeof ps,MVK_PRIVATE_API_VERSION);
        else { const MVKShaderCompilationPerformance *s=&ps.shaderCompilation; const MVKPipelineCachePerformance *q=&ps.pipelineCache;
#define TK(t) (t).count,(t).average,(t).maximum
          fprintf(f,"kh: gfx MoltenVK shader statistics (count, mean ms, longest ms): spirvToMSL %u %.2f %.2f; mslCompile %u %.2f %.2f; mslLoad %u %.2f %.2f; shaderLibraryFromCache %u %.2f %.2f; functionRetrieval %u %.2f %.2f; functionSpecialization %u %.2f %.2f; pipelineCompile %u %.2f %.2f; "
                    "mslCompress %u %.2f %.2f; mslDecompress %u %.2f %.2f; pipeline cache: read %u %.2f %.2f, size %u %.2f %.2f, write %u %.2f %.2f%s\n",
                  TK(s->spirvToMSL),TK(s->mslCompile),TK(s->mslLoad),TK(s->shaderLibraryFromCache),TK(s->functionRetrieval),TK(s->functionSpecialization),TK(s->pipelineCompile),TK(s->mslCompress),TK(s->mslDecompress),
                  TK(q->readPipelineCache),TK(q->sizePipelineCache),TK(q->writePipelineCache),
                  (s->spirvToMSL.count|s->mslCompile.count|s->pipelineCompile.count|q->readPipelineCache.count)?"":" [every count is 0: MoltenVK's performance tracking is off: MVK_CONFIG_PERFORMANCE_TRACKING=1]");
#undef TK
        } } }
#elif defined(__APPLE__)
    fprintf(f,"kh: gfx MoltenVK shader statistics: absent (MoltenVK's statistics are read in the iPhone build only: KH_GFX_MVK_API; the pipeline counts above are the wrapper's own)\n");
#endif
}
static PFN_vkVoidFunction VKAPI_PTR gw_gdpa(VkDevice d,const char *name){
    PFN_vkVoidFunction f=gw_real_gdpa(d,name);
    if(!f||!name) return f;
    if(!strcmp(name,"vkGetDeviceQueue")) return (PFN_vkVoidFunction)gw_get_device_queue;
    if(!strcmp(name,"vkQueueSubmit")) return (PFN_vkVoidFunction)gw_queue_submit;
    /* TASK43 (step 12): the pipeline cache and the pipeline-creation counters; the device's own function is kept per VkDevice */
    int k=!strcmp(name,"vkCreatePipelineCache")?1:!strcmp(name,"vkDestroyPipelineCache")?2:!strcmp(name,"vkGetPipelineCacheData")?3:!strcmp(name,"vkCreateGraphicsPipelines")?4:
          !strcmp(name,"vkCreateComputePipelines")?5:!strcmp(name,"vkDestroyDevice")?6:0;
    if(k){ pthread_mutex_lock(&gw_dm); GwDev *e=gw_dev_of(d,1);
        switch(k){ case 1: e->create_cache=(PFN_vkCreatePipelineCache)f; f=(PFN_vkVoidFunction)gw_create_pipeline_cache; break;
                   case 2: e->destroy_cache=(PFN_vkDestroyPipelineCache)f; f=(PFN_vkVoidFunction)gw_destroy_pipeline_cache; break;
                   case 3: e->get_data=(PFN_vkGetPipelineCacheData)f; break;                       /* vkd3d never calls it; the save does */
                   case 4: e->create_gp=(PFN_vkCreateGraphicsPipelines)f; f=(PFN_vkVoidFunction)gw_create_gp; break;
                   case 5: e->create_cp=(PFN_vkCreateComputePipelines)f; f=(PFN_vkVoidFunction)gw_create_cp; break;
                   default: e->destroy_dev=(PFN_vkDestroyDevice)f; f=(PFN_vkVoidFunction)gw_destroy_device; break; }
        pthread_mutex_unlock(&gw_dm); }
    return f;
}
static PFN_vkVoidFunction VKAPI_PTR kh_vk_gipa_wrap(VkInstance inst,const char *name){
    atomic_fetch_add(&gw_names,1);
    if(name&&!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)kh_vk_gipa_wrap;
    PFN_vkVoidFunction f=vk_gipa(inst,name);
    if(!f||!name) return f;
    if(!strcmp(name,"vkCreateInstance")){ gw_real_ci=(PFN_vkCreateInstance)f; return (PFN_vkVoidFunction)gw_create_instance; }
    if(!strcmp(name,"vkCreateDevice")){ gw_real_cd=(PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)gw_create_device; }
    if(!strcmp(name,"vkGetDeviceProcAddr")){ gw_real_gdpa=(PFN_vkGetDeviceProcAddr)f; return (PFN_vkVoidFunction)gw_gdpa; }   /* TASK41 */
    return f;
}
/* requested as OPTIONAL: a missing one is skipped by vkd3d (device.c 355-367, 415-420); a missing REQUIRED one would only be warned
 * about and still be passed on, and instance or device creation would fail (device.c 347-351, 411-414) */
static const char *const opt_inst_ext[]={"VK_KHR_surface","VK_EXT_headless_surface","VK_EXT_metal_surface","VK_EXT_layer_settings"};
#if defined(__APPLE__)
static const char *const opt_dev_ext[]={"VK_KHR_swapchain","VK_GOOGLE_display_timing","VK_EXT_metal_objects"};
#else
static const char *const opt_dev_ext[]={"VK_KHR_swapchain","VK_GOOGLE_display_timing"};
#endif
static int pres_ext_printed;
static void pres_ext_report(void){
    if(pres_ext_printed) return; pres_ext_printed=1; const KhPresExt *x=&kh_gfxc_pres_ext;
    fprintf(stderr,"kh: gfx vkd3d: presentation extensions: surface=%d headless=%d metal=%d swapchain=%d display_timing=%d metal_objects=%d layer_settings=%d "
                   "(requested as optional; 1 = enabled, as seen at vkCreateInstance / vkCreateDevice through the gipa wrapper; %llu names resolved through it)\n",
            x->surface,x->headless,x->metal,x->swapchain,x->display_timing,x->metal_objects,x->layer_settings,P(gw_names));
}
/* caller holds vm */
static HRESULT instance_get(void){
    if(vk_inst) return S_OK;
#ifdef KH_GFX_VULKAN_STATIC
    /* TASK36 (plan step 1): the Vulkan implementation is LINKED into the program (Apple: MoltenVK's static library, no loader; a Linux
     * test build links the loader with -lvulkan).  Nothing is loaded at run time, so a named library is an error, not a silent no-op. */
    if(!vk_gipa){ const char *named=getenv("KHNMC_GFX_VULKAN_LIB");
        if(named&&*named) kh_fatal(NULL,"gfx-vulkan-lib-static: KHNMC_GFX_VULKAN_LIB=%s, but this build links its Vulkan implementation (KH_GFX_VULKAN_STATIC): there is no library to load",named);
        vk_gipa=vkGetInstanceProcAddr; vk_lib_name="the program's own vkGetInstanceProcAddr (KH_GFX_VULKAN_STATIC: linked, not loaded)"; vk_lib_from=""; }
#else
    if(!vk_gipa){ char why[512]; why[0]=0; void *lib=NULL;
        /* a library named outright (KHNMC_GFX_VULKAN_LIB, else the build's KH_GFX_VULKAN_LIB) is THE library: an absolute path, and no
         * search by name when it cannot be loaded */
        const char *named=getenv("KHNMC_GFX_VULKAN_LIB"), *from="from KHNMC_GFX_VULKAN_LIB; ";
        if(named&&!*named) named=NULL;
#ifdef KH_GFX_VULKAN_LIB
        if(!named){ named=KH_GFX_VULKAN_LIB; from="the build's KH_GFX_VULKAN_LIB; "; }
#endif
        if(named){
            if(named[0]!='/') kh_fatal(NULL,"gfx-vulkan-lib-path: the Vulkan library was named as '%s' (%s): it must be an absolute path (a bare name would be searched like any other library)",named,from);
            lib=dlopen(named,RTLD_NOW|RTLD_LOCAL);
            if(!lib){ const char *e=dlerror(); kh_fatal(NULL,"gfx-vulkan-loader: the Vulkan library %s (%s) cannot be loaded: %s",named,from,e?e:"no reason given"); }
            vk_lib_name=named; vk_lib_from=from; }
        for(size_t i=0;!lib&&i<sizeof vk_lib_names/sizeof*vk_lib_names;i++){ lib=dlopen(vk_lib_names[i],RTLD_NOW|RTLD_LOCAL);
            if(lib) vk_lib_name=vk_lib_names[i]; else { const char *e=dlerror(); size_t l=strlen(why); snprintf(why+l,sizeof why-l,"%s%s",l?"; ":"",e?e:vk_lib_names[i]); } }
        if(!lib) kh_fatal(NULL,"gfx-vulkan-loader: no Vulkan library of this OS can be loaded (%s). KHNMC_GFX_VULKAN_LIB=<absolute path> names one outright",why);
        vk_gipa=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
        if(!vk_gipa) kh_fatal(NULL,"gfx-vulkan-loader: %s has no vkGetInstanceProcAddr",vk_lib_name); }
#endif
    struct vkd3d_application_info app; memset(&app,0,sizeof app); app.type=VKD3D_STRUCTURE_TYPE_APPLICATION_INFO; app.application_name="khnmc"; app.api_version=VKD3D_API_VERSION_2_1;
    struct vkd3d_optional_instance_extensions_info oie; memset(&oie,0,sizeof oie); oie.type=VKD3D_STRUCTURE_TYPE_OPTIONAL_INSTANCE_EXTENSIONS_INFO; oie.next=&app;
    oie.extensions=opt_inst_ext; oie.extension_count=(uint32_t)(sizeof opt_inst_ext/sizeof*opt_inst_ext);
    struct vkd3d_instance_create_info ici; memset(&ici,0,sizeof ici); ici.type=VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.next=&oie; ici.pfn_signal_event=sig_event; ici.wchar_size=2;
    ici.pfn_vkGetInstanceProcAddr=kh_vk_gipa_wrap;
    HRESULT hr=vkd3d_create_instance(&ici,&vk_inst);
    if(FAILED(hr)){ vk_inst=NULL; fprintf(stderr,"kh: gfx vkd3d: vkd3d_create_instance FAILED hr=0x%08x (Vulkan library %s, VK_DRIVER_FILES=%s)\n",(unsigned)hr,vk_lib_name,getenv("VK_DRIVER_FILES")?getenv("VK_DRIVER_FILES"):"(unset)"); return hr; }
    /* every physical device the instance can see must be a CPU implementation unless the system driver was asked for; the two functions
     * come from the instance vkd3d created, through the library vkd3d uses */
    VkInstance vi=vkd3d_instance_get_vk_instance(vk_inst);
    PFN_vkEnumeratePhysicalDevices enum_pd=(PFN_vkEnumeratePhysicalDevices)vk_gipa(vi,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props=(PFN_vkGetPhysicalDeviceProperties)vk_gipa(vi,"vkGetPhysicalDeviceProperties");
    if(!enum_pd||!props) kh_fatal(NULL,"gfx-vulkan-loader: %s gives no vkEnumeratePhysicalDevices / vkGetPhysicalDeviceProperties for the instance",vk_lib_name);
    fprintf(stderr,"kh: gfx vkd3d: Vulkan library %s (%sits vkGetInstanceProcAddr is handed to vkd3d behind the gipa wrapper)\n",vk_lib_name,vk_lib_from);
    VkPhysicalDevice pd[16]; uint32_t n=16; VkResult vr=enum_pd(vi,&n,pd);
    if((vr!=VK_SUCCESS&&vr!=VK_INCOMPLETE)||!n) kh_fatal(NULL,"gfx-vulkan-devices: vkEnumeratePhysicalDevices result %d, %u device(s)",(int)vr,n);
    for(uint32_t i=0;i<n;i++){ VkPhysicalDeviceProperties p; memset(&p,0,sizeof p); props(pd[i],&p);
        fprintf(stderr,"kh: gfx vkd3d: Vulkan physical device %u/%u: '%s' type=%s vendor=0x%x device=0x%x api=%u.%u.%u\n",i+1,n,p.deviceName,pd_type(p.deviceType),p.vendorID,p.deviceID,
                VK_API_VERSION_MAJOR(p.apiVersion),VK_API_VERSION_MINOR(p.apiVersion),VK_API_VERSION_PATCH(p.apiVersion));
        if(!drv_system&&p.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)
            kh_fatal(NULL,"gfx-driver-not-cpu: the Vulkan instance reports '%s' (type %s), not a CPU driver, although lavapipe was selected. Refusing to create a device on it.",p.deviceName,pd_type(p.deviceType));
        if(i==0) snprintf(vk_dev_name,sizeof vk_dev_name,"%s (%s)",p.deviceName,pd_type(p.deviceType)); }
    return S_OK;
}

/* method names by vtable slot: KHNMC_GFX_TRACE=1, the wrapped bridge's per-method counts and its named fatals */
#define IUNK "QueryInterface","AddRef","Release"
#define OBJ IUNK,"GetPrivateData","SetPrivateData","SetPrivateDataInterface","SetName"
#define dev_names kh_gfxc_dev_names
#define list_names kh_gfxc_list_names
#define queue_names kh_gfxc_queue_names
#define alloc_names kh_gfxc_alloc_names
#define res_names kh_gfxc_res_names
#define dheap_names kh_gfxc_dheap_names
#define fence_names kh_gfxc_fence_names
const char *const dev_names[]={OBJ,"GetNodeCount","CreateCommandQueue","CreateCommandAllocator","CreateGraphicsPipelineState","CreateComputePipelineState","CreateCommandList","CheckFeatureSupport","CreateDescriptorHeap",
    "GetDescriptorHandleIncrementSize","CreateRootSignature","CreateConstantBufferView","CreateShaderResourceView","CreateUnorderedAccessView","CreateRenderTargetView","CreateDepthStencilView","CreateSampler","CopyDescriptors",
    "CopyDescriptorsSimple","GetResourceAllocationInfo","GetCustomHeapProperties","CreateCommittedResource","CreateHeap","CreatePlacedResource","CreateReservedResource","CreateSharedHandle","OpenSharedHandle","OpenSharedHandleByName",
    "MakeResident","Evict","CreateFence","GetDeviceRemovedReason","GetCopyableFootprints","CreateQueryHeap","SetStablePowerState","CreateCommandSignature","GetResourceTiling","GetAdapterLuid"};
const char *const list_names[]={OBJ,"GetDevice","GetType","Close","Reset","ClearState","DrawInstanced","DrawIndexedInstanced","Dispatch","CopyBufferRegion","CopyTextureRegion","CopyResource","CopyTiles","ResolveSubresource",
    "IASetPrimitiveTopology","RSSetViewports","RSSetScissorRects","OMSetBlendFactor","OMSetStencilRef","SetPipelineState","ResourceBarrier","ExecuteBundle","SetDescriptorHeaps","SetComputeRootSignature","SetGraphicsRootSignature",
    "SetComputeRootDescriptorTable","SetGraphicsRootDescriptorTable","SetComputeRoot32BitConstant","SetGraphicsRoot32BitConstant","SetComputeRoot32BitConstants","SetGraphicsRoot32BitConstants","SetComputeRootConstantBufferView",
    "SetGraphicsRootConstantBufferView","SetComputeRootShaderResourceView","SetGraphicsRootShaderResourceView","SetComputeRootUnorderedAccessView","SetGraphicsRootUnorderedAccessView","IASetIndexBuffer","IASetVertexBuffers","SOSetTargets",
    "OMSetRenderTargets","ClearDepthStencilView","ClearRenderTargetView","ClearUnorderedAccessViewUint","ClearUnorderedAccessViewFloat","DiscardResource","BeginQuery","EndQuery","ResolveQueryData","SetPredication","SetMarker","BeginEvent",
    "EndEvent","ExecuteIndirect"};
const char *const queue_names[]={OBJ,"GetDevice","UpdateTileMappings","CopyTileMappings","ExecuteCommandLists","SetMarker","BeginEvent","EndEvent","Signal","Wait","GetTimestampFrequency","GetClockCalibration","GetDesc"};
const char *const alloc_names[]={OBJ,"GetDevice","Reset"};
const char *const res_names[]={OBJ,"GetDevice","Map","Unmap","GetDesc","GetGPUVirtualAddress","WriteToSubresource","ReadFromSubresource","GetHeapProperties"};
const char *const dheap_names[]={OBJ,"GetDevice","GetDesc","GetCPUDescriptorHandleForHeapStart","GetGPUDescriptorHandleForHeapStart"};
const char *const fence_names[]={OBJ,"GetDevice","GetCompletedValue","SetEventOnCompletion","Signal"};
#define NN(t) (int)(sizeof t/sizeof*t)
const int kh_gfxc_dev_nnames=NN(dev_names), kh_gfxc_list_nnames=NN(list_names), kh_gfxc_queue_nnames=NN(queue_names), kh_gfxc_alloc_nnames=NN(alloc_names), kh_gfxc_res_nnames=NN(res_names),
          kh_gfxc_dheap_nnames=NN(dheap_names), kh_gfxc_fence_nnames=NN(fence_names);
#if KH_GFX_TRAMP
static void name_vtable(const void *obj,const char *iface,const char *const *names,size_t n){ void **vt=*(void***)obj; for(size_t i=0;i<n;i++) kh_native_name(U(vt[i]),iface,names[i]); }
#define NAME_VT(obj,iface,tab) name_vtable(obj,iface,tab,sizeof tab/sizeof*tab)
#endif
/* the two bridges differ in what the game holds: a host interface pointer (trampoline) or the guest object that wraps it */
static void *real_of(KCtx *g,u64 shown,int kind,const char *what){
    if(kh_gfx_wrap_on) return kh_wrap_real(g,shown,kind,what);
#if KH_GFX_TRAMP
    return (void*)(uintptr_t)shown;
#else
    kh_fatal(g,"gfx-vkd3d-needs-identity: %s: a host interface pointer was expected, which only the trampoline bridge (x86-64, identity addresses) hands out",what);
#endif
}
static u64 shown_of(KCtx *g,void *real,int kind){
    if(kh_gfx_wrap_on) return kh_wrap_obj(g,real,kind);
#if KH_GFX_TRAMP
    return U(real);
#else
    kh_fatal(g,"gfx-vkd3d-needs-identity: a host interface pointer cannot be handed to the lifted code in this build");
#endif
}
/* the runtime gives up ITS reference to a real object the game may also hold (wrapped bridge: the guest object goes with the last reference) */
static u32 drop_real(KCtx *g,void *real){ return kh_gfx_wrap_on?kh_wrap_release_real(g,real):(u32)IUnknown_Release((IUnknown*)real); }
/* ---- default root descriptor tables (see the file comment) ------------------------------------------------------------------- */
typedef struct { u64 rs; int n; u8 idx[16], samp[16]; } RsLayout;               /* descriptor-table parameters of one root signature */
typedef struct { u64 list, rs, heap[2]; u32 set_mask; } LState;                 /* heap[0] = CBV_SRV_UAV, heap[1] = SAMPLER ; set_mask = tables the game set */
#define MAXRS 32
#define MAXLISTS 4096
static RsLayout rsl[MAXRS]; static int nrsl; static LState lst[MAXLISTS]; static int nlst; static pthread_mutex_t lsm=PTHREAD_MUTEX_INITIALIZER;
static LState lstate_get(u64 l){ LState r; memset(&r,0,sizeof r); r.list=l; pthread_mutex_lock(&lsm); for(int i=0;i<nlst;i++) if(lst[i].list==l){ r=lst[i]; break; } pthread_mutex_unlock(&lsm); return r; }
static void lstate_put(KCtx *g,const LState *st){
    pthread_mutex_lock(&lsm); int i; for(i=0;i<nlst;i++) if(lst[i].list==st->list) break;
    if(i==nlst){ if(nlst>=MAXLISTS){ pthread_mutex_unlock(&lsm); kh_fatal(g,"gfx-list-table: more than %d live command lists are tracked",MAXLISTS); } nlst++; }
    lst[i]=*st; pthread_mutex_unlock(&lsm);
}
static void lstate_drop(u64 l){ pthread_mutex_lock(&lsm); for(int i=0;i<nlst;i++) if(lst[i].list==l){ lst[i]=lst[--nlst]; break; } pthread_mutex_unlock(&lsm); }
/* bind the first descriptor of the matching heap to every table the game has not set (needs root signature and heaps) */
static void defaults_apply(ID3D12GraphicsCommandList *l,const LState *st){
    if(!st->rs) return; RsLayout lay; int found=0;
    pthread_mutex_lock(&lsm); for(int i=0;i<nrsl;i++) if(rsl[i].rs==st->rs){ lay=rsl[i]; found=1; break; } pthread_mutex_unlock(&lsm);
    if(!found) return;
    for(int i=0;i<lay.n;i++){ u64 heap=st->heap[lay.samp[i]]; if(!heap||(st->set_mask&(1u<<lay.idx[i]))) continue;
        D3D12_GPU_DESCRIPTOR_HANDLE h=ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart((ID3D12DescriptorHeap*)(uintptr_t)heap);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l,lay.idx[i],h); atomic_fetch_add(&n_default_tables,1);
        if(kh_native_trace) fprintf(stderr,"kh: default table: list %p root parameter %u := first descriptor of %s heap %llx\n",(void*)l,lay.idx[i],lay.samp[i]?"sampler":"CBV/SRV/UAV",P(heap)); }
}
void kh_gfxc_track_reset(KCtx *g,u64 l){ LState st; memset(&st,0,sizeof st); st.list=l; lstate_put(g,&st); }
void kh_gfxc_track_rootsig(KCtx *g,u64 l,u64 rs){ LState st=lstate_get(l); if(st.rs==rs) return;               /* the same root signature keeps the bindings (D3D12 and vkd3d) */
    st.rs=rs; st.set_mask=0; lstate_put(g,&st); defaults_apply((ID3D12GraphicsCommandList*)(uintptr_t)l,&st); }
void kh_gfxc_track_heaps(KCtx *g,u64 l,u32 n,const u64 *heaps){ LState st=lstate_get(l); int changed=0;
    for(u32 i=0;i<n&&heaps;i++){ if(!heaps[i]) continue; D3D12_DESCRIPTOR_HEAP_DESC d=ID3D12DescriptorHeap_GetDesc((ID3D12DescriptorHeap*)(uintptr_t)heaps[i]);
        int t=d.Type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER?1:d.Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV?0:-1;
        if(t>=0&&st.heap[t]!=heaps[i]){ if(st.heap[t]) st.set_mask=0; st.heap[t]=heaps[i]; changed=1; } }                 /* a REPLACED heap makes the game's tables stale */
    if(!changed) return; lstate_put(g,&st); defaults_apply((ID3D12GraphicsCommandList*)(uintptr_t)l,&st); }
void kh_gfxc_track_table(KCtx *g,u64 l,u32 idx){ if(idx>=32) kh_fatal(g,"gfx-rootsig-layout: SetGraphicsRootDescriptorTable with root parameter %u (the tracker holds 32)",idx); LState st=lstate_get(l); if(st.set_mask&(1u<<idx)) return; st.set_mask|=1u<<idx; lstate_put(g,&st); }
void kh_gfxc_track_drop(u64 l){ lstate_drop(l); }
/* a root signature was created from `blob` (host address): remember which of its root parameters are descriptor tables.  rs = the REAL root signature */
void kh_gfxc_rootsig_learn(KCtx *g,u64 rs,const void *blob,size_t size){
    ID3D12RootSignatureDeserializer *d=NULL; HRESULT hr=vkd3d_create_root_signature_deserializer(blob,(SIZE_T)size,&IID_ID3D12RootSignatureDeserializer,(void**)&d);
    if(FAILED(hr)||!d) kh_fatal(g,"gfx-rootsig-layout: the blob of root signature 0x%llx cannot be deserialized (hr=0x%08x)",P(rs),(unsigned)hr);
    const D3D12_ROOT_SIGNATURE_DESC *desc=ID3D12RootSignatureDeserializer_GetRootSignatureDesc(d); RsLayout lay; memset(&lay,0,sizeof lay); lay.rs=rs;
    for(UINT i=0;desc&&i<desc->NumParameters;i++){ const D3D12_ROOT_PARAMETER *p=&desc->pParameters[i];
        if(p->ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE||!p->DescriptorTable.NumDescriptorRanges) continue;
        if(lay.n>=16||i>=32) kh_fatal(g,"gfx-rootsig-layout: root signature 0x%llx has more descriptor tables than the tracker holds",P(rs));
        lay.idx[lay.n]=(u8)i; lay.samp[lay.n]=p->DescriptorTable.pDescriptorRanges[0].RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; lay.n++; }
    unsigned nparams=desc?desc->NumParameters:0;
    ID3D12RootSignatureDeserializer_Release(d);
    pthread_mutex_lock(&lsm); int i; for(i=0;i<nrsl;i++) if(rsl[i].rs==rs) break;
    if(i==nrsl){ if(nrsl>=MAXRS){ pthread_mutex_unlock(&lsm); kh_fatal(g,"gfx-rootsig-layout: more than %d root signatures",MAXRS); } nrsl++; }
    rsl[i]=lay; pthread_mutex_unlock(&lsm);
    fprintf(stderr,"kh: gfx vkd3d: root signature 0x%llx: %u parameters, %d descriptor table(s) get a default binding\n",P(rs),nparams,lay.n);
}
#if KH_GFX_TRAMP
/* the trampoline bridge tracks the same state with post hooks on the native entry points */
static void post_list_reset(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk; if(!(u32)rax) kh_gfxc_track_reset(g,a[0]); }
static void post_list_release(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)g;(void)stk; if(!(u32)rax) lstate_drop(a[0]); }
static void post_list_rootsig(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk;(void)rax; kh_gfxc_track_rootsig(g,a[0],a[1]); }
static void post_list_heaps(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk;(void)rax; kh_gfxc_track_heaps(g,a[0],(u32)a[1],(const u64*)hp(g,a[2])); }
static void post_list_table(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk;(void)rax; kh_gfxc_track_table(g,a[0],(u32)a[1]); }
/* ID3D12Device::CreateRootSignature(this, node mask, blob, size, riid, **pp) */
static void post_create_rootsig(KCtx *g,const u64 *a,const u64 *stk,u64 rax){
    if((u32)rax||!stk[1]) return; u64 rs=rd64(g,stk[1]); if(!rs) return;
    kh_gfxc_rootsig_learn(g,rs,(const void*)hp(g,a[2]),(size_t)a[3]);
}
#endif

/* ---- bundles: recorded by us, replayed on the direct command list at ExecuteBundle --------------------------------------------- */
typedef struct { u8 slot; u32 blob, blen; u64 a[5]; } BCmd;                    /* a[] = rdx, r8, r9, first two stack arguments; blob = copied pointee */
typedef struct Bundle { u64 obj; int open,arena; BCmd *cmd; u32 n,cap; u8 *blob; u32 bn,bcap; } Bundle;
#define B_PTR 32                               /* guest bundle object: host Bundle* */
#define NOBLOB 0xffffffffu
static Bundle *bundle_of(KCtx *g,u64 o){ return (Bundle*)(uintptr_t)rd64(g,o+B_PTR); }
/* Bundle objects store a native pointer in guest RAM. Its allocation and both
 * growing arrays must rewind with that pointer when checkpoints are enabled. */
static void *bundle_resize(void *p,size_t size,int arena){
    if(!arena){
        if(!size){ free(p); return NULL; }
        return realloc(p,size);
    }
    u64 old=p?(uintptr_t)p-gs_bias:0;
    if(!size){ ga_free(old); return NULL; }
    u64 addr=ga_realloc(old,size);
    return addr?(void *)(uintptr_t)(gs_bias+addr):NULL;
}
static void bundle_rec(KCtx *g,Bundle *b,int slot,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4,u64 ptr,u32 len){
    if(!b->open) kh_fatal(g,"gfx-bundle-closed: ID3D12GraphicsCommandList method slot %d called on a bundle that is closed (no Reset since Close)",slot);
    if(b->n==b->cap){ b->cap=b->cap?b->cap*2:32; b->cmd=bundle_resize(b->cmd,sizeof(BCmd)*b->cap,b->arena); if(!b->cmd) kh_fatal(g,"gfx-bundle: out of memory"); }
    BCmd *c=&b->cmd[b->n++]; c->slot=(u8)slot; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3; c->a[4]=a4; c->blob=NOBLOB; c->blen=len;
    if(ptr&&len){ u32 need=(len+15)&~15u;
        if(b->bn+need>b->bcap){ b->bcap=(b->bn+need)*2+256; b->blob=bundle_resize(b->blob,b->bcap,b->arena); if(!b->blob) kh_fatal(g,"gfx-bundle: out of memory"); }
        memcpy(b->blob+b->bn,hp(g,ptr),len); c->blob=b->bn; b->bn+=need; }
}
/* what a bundle records for an interface argument: the REAL object (the wrapped bridge's game passes the guest object that wraps it) */
static u64 b_real(KCtx *g,u64 shown,int kind,const char *what){ return shown?U(real_of(g,shown,kind,what)):0; }
static int h_balloc(KCtx *g,u64 o,int s,const char *c){ (void)c;
    if(s>=3&&s<=6){ SET_RAX(g,0); return 1; }
    if(s==2){ int last=0; u64 r=kh_com_ref_drop(g,o,&last); SET_RAX(g,r); if(last){ wr64(g,o,0); ga_free(o); } return 1; }   /* Release: atomic, as AddRef (com.c; review U35 n=4) */
    if(s==8){ SET_RAX(g,0); return 1; }                                         /* Reset */
    return 0; }
static int h_bundle(KCtx *g,u64 o,int s,const char *c){ (void)c; Bundle *b=bundle_of(g,o);
    switch(s){
    case 2: { int last=0; u64 r=kh_com_ref_drop(g,o,&last); SET_RAX(g,r);                 /* atomic, as AddRef (com.c; review U35 n=4) */
              if(last){ bundle_resize(b->cmd,0,b->arena); bundle_resize(b->blob,0,b->arena); bundle_resize(b,0,b->arena); wr64(g,o,0); wr64(g,o+B_PTR,0); ga_free(o); atomic_fetch_sub(&n_bundles_live,1); } return 1; }   /* Release: the recording dies with the last reference */
    case 3: case 4: case 5: case 6: SET_RAX(g,0); return 1;                     /* private data, SetName */
    case 8: SET_RAX(g,D3D12_COMMAND_LIST_TYPE_BUNDLE); return 1;                /* GetType */
    case 9: if(!b->open){ fprintf(stderr,"kh: gfx vkd3d: bundle 0x%llx: Close on a closed bundle -> E_FAIL\n",P(o)); SET_RAX(g,(u32)E_FAIL); return 1; }
            b->open=0; SET_RAX(g,0); return 1;                                  /* Close */
    case 10: { u64 al=A1(g), pso=A2(g);                                          /* Reset(allocator, initial pipeline state) */
            if(b->open){ fprintf(stderr,"kh: gfx vkd3d: bundle 0x%llx: Reset while still recording -> E_FAIL\n",P(o)); SET_RAX(g,(u32)E_FAIL); return 1; }
            if(!GA_OK(al)||rd64(g,al)!=kh_com_vtbl(C_balloc)) kh_fatal(g,"gfx-bundle-allocator: bundle Reset with allocator 0x%llx, which is not a bundle allocator",P(al));
            b->n=0; b->bn=0; b->open=1; if(pso) bundle_rec(g,b,25,b_real(g,pso,WK_PSO,"bundle Reset: initial pipeline state"),0,0,0,0,0,0); SET_RAX(g,0); return 1; }
    case 12: bundle_rec(g,b,s,A1(g),A2(g),A3(g),ARG(g,4),0,0,0); return 1;       /* DrawInstanced(vertices, instances, start vertex, start instance) */
    case 13: bundle_rec(g,b,s,A1(g),A2(g),A3(g),ARG(g,4),ARG(g,5),0,0); return 1; /* DrawIndexedInstanced(indices, instances, start index, base vertex, start instance) */
    case 20: case 24: bundle_rec(g,b,s,A1(g),0,0,0,0,0,0); return 1;             /* IASetPrimitiveTopology, OMSetStencilRef */
    case 25: bundle_rec(g,b,s,b_real(g,A1(g),WK_PSO,"bundle SetPipelineState"),0,0,0,0,0,0); return 1;
    case 30: bundle_rec(g,b,s,b_real(g,A1(g),WK_ROOTSIG,"bundle SetGraphicsRootSignature"),0,0,0,0,0,0); return 1;
    case 23: bundle_rec(g,b,s,A1(g),0,0,0,0,A1(g),16); return 1;                 /* OMSetBlendFactor(const FLOAT[4] or NULL) */
    case 28: { u32 n=(u32)A1(g); if(n>8) kh_fatal(g,"gfx-bundle: SetDescriptorHeaps with %u heaps on a bundle (D3D12 allows one per heap type)",n);
               bundle_rec(g,b,s,n,0,0,0,0,A2(g),8*n);                              /* SetDescriptorHeaps(count, heaps): the copied array holds REAL heap pointers */
               BCmd *bc=&b->cmd[b->n-1]; if(bc->blob!=NOBLOB) for(u32 i=0;i<n;i++){ u64 v; memcpy(&v,b->blob+bc->blob+8*i,8); v=b_real(g,v,WK_DHEAP,"bundle SetDescriptorHeaps"); memcpy(b->blob+bc->blob+8*i,&v,8); }
               return 1; }
    case 32: case 38: bundle_rec(g,b,s,A1(g),A2(g),0,0,0,0,0); return 1;         /* SetGraphicsRootDescriptorTable(index, gpu handle), SetGraphicsRootConstantBufferView(index, gpu va) */
    case 43: bundle_rec(g,b,s,A1(g),0,0,0,0,A1(g),sizeof(D3D12_INDEX_BUFFER_VIEW)); return 1;             /* IASetIndexBuffer(view or NULL) */
    case 44: bundle_rec(g,b,s,A1(g),A2(g),A3(g),0,0,A3(g),(u32)sizeof(D3D12_VERTEX_BUFFER_VIEW)*(u32)A2(g)); return 1;   /* IASetVertexBuffers(start, count, views or NULL) */
    default: return 0; } }                                                        /* slots 0, 1: IUnknown defaults; anything else: named fatal (strict class) */
/* ID3D12Device::CreateCommandAllocator(BUNDLE): our allocator object */
u64 kh_gfxc_bundle_allocator_new(KCtx *g){ u64 o=kh_com_obj(C_balloc,0); if(!o) kh_fatal(g,"gfx-bundle: out of memory"); return o; }
/* ID3D12Device::CreateCommandList(BUNDLE, allocator, initial pipeline state): our recorder object; a bundle starts open, like every list */
static u64 bundle_new(KCtx *g,u64 al,u64 pso,int arena){
    if(!GA_OK(al)||rd64(g,al)!=kh_com_vtbl(C_balloc)) kh_fatal(g,"gfx-bundle-allocator: CreateCommandList(BUNDLE) with allocator 0x%llx, which is not a bundle allocator",P(al));
    u64 o=kh_com_obj(C_bundle,16); Bundle *b=bundle_resize(NULL,sizeof *b,arena); if(!o||!b) kh_fatal(g,"gfx-bundle: out of memory");
    *b=(Bundle){.obj=o,.open=1,.arena=arena}; wr64(g,o+B_PTR,U(b)); if(pso) bundle_rec(g,b,25,b_real(g,pso,WK_PSO,"CreateCommandList(BUNDLE): initial pipeline state"),0,0,0,0,0,0);
    atomic_fetch_add(&n_bundles_made,1); atomic_fetch_add(&n_bundles_live,1);
    return o;
}
u64 kh_gfxc_bundle_new(KCtx *g,u64 al,u64 pso){ return bundle_new(g,al,pso,kh_cut_enabled); }
int kh_gfxc_is_bundle(KCtx *g,u64 o){ return o&&GA_OK(o)&&rd64(g,o)==kh_com_vtbl(C_bundle); }
/* ID3D12GraphicsCommandList::ExecuteBundle(l = REAL direct list, bo = our recorder): replay the recording on the list */
void kh_gfxc_bundle_replay(KCtx *g,ID3D12GraphicsCommandList *l,u64 bo){
    if(!kh_gfxc_is_bundle(g,bo)) kh_fatal(g,"gfx-bundle-foreign: ExecuteBundle(0x%llx): the argument is not a bundle recorded by this runtime",P(bo));
    Bundle *b=bundle_of(g,bo);
    if(b->open) kh_fatal(g,"gfx-bundle-open: ExecuteBundle(0x%llx) on a bundle that was not closed",P(bo));
    for(u32 i=0;i<b->n;i++){ const BCmd *c=&b->cmd[i]; const void *p=c->blob==NOBLOB?NULL:b->blob+c->blob;
        if(kh_native_trace){ fprintf(stderr,"kh: bundle %llx replay on %p: %s(%llx, %llx, %llx, %llx, %llx)",P(bo),(void*)l,list_names[c->slot],P(c->a[0]),P(c->a[1]),P(c->a[2]),P(c->a[3]),P(c->a[4]));
            if(p){ fprintf(stderr," data:"); for(u32 k=0;k<c->blen&&k<64;k+=8){ u64 v=0; memcpy(&v,(const u8*)p+k,c->blen-k<8?c->blen-k:8); fprintf(stderr," %llx",P(v)); } } fputc('\n',stderr); }
        switch(c->slot){
        case 12: ID3D12GraphicsCommandList_DrawInstanced(l,(UINT)c->a[0],(UINT)c->a[1],(UINT)c->a[2],(UINT)c->a[3]); atomic_fetch_add(&gfx_draws,1); break;
        case 13: ID3D12GraphicsCommandList_DrawIndexedInstanced(l,(UINT)c->a[0],(UINT)c->a[1],(UINT)c->a[2],(INT)c->a[3],(UINT)c->a[4]); atomic_fetch_add(&gfx_draws,1); break;
        case 20: ID3D12GraphicsCommandList_IASetPrimitiveTopology(l,(D3D12_PRIMITIVE_TOPOLOGY)c->a[0]); break;
        case 23: ID3D12GraphicsCommandList_OMSetBlendFactor(l,(const FLOAT*)p); break;
        case 24: ID3D12GraphicsCommandList_OMSetStencilRef(l,(UINT)c->a[0]); break;
        case 25: ID3D12GraphicsCommandList_SetPipelineState(l,(ID3D12PipelineState*)(uintptr_t)c->a[0]); atomic_fetch_add(&gfx_pso_binds,1); break;
        case 28: ID3D12GraphicsCommandList_SetDescriptorHeaps(l,(UINT)c->a[0],(ID3D12DescriptorHeap*const*)p); kh_gfxc_track_heaps(g,U(l),(u32)c->a[0],(const u64*)p); break;
        case 30: ID3D12GraphicsCommandList_SetGraphicsRootSignature(l,(ID3D12RootSignature*)(uintptr_t)c->a[0]); kh_gfxc_track_rootsig(g,U(l),c->a[0]); break;
        case 32: { D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr=c->a[1]; ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l,(UINT)c->a[0],h); kh_gfxc_track_table(g,U(l),(u32)c->a[0]); break; }
        case 38: ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l,(UINT)c->a[0],(D3D12_GPU_VIRTUAL_ADDRESS)c->a[1]); break;
        case 43: ID3D12GraphicsCommandList_IASetIndexBuffer(l,(const D3D12_INDEX_BUFFER_VIEW*)p); break;
        case 44: ID3D12GraphicsCommandList_IASetVertexBuffers(l,(UINT)c->a[0],(UINT)c->a[1],(const D3D12_VERTEX_BUFFER_VIEW*)p); break;
        default: kh_fatal(g,"gfx-bundle-replay: recorded method slot %u has no replay",c->slot); } }
    atomic_fetch_add(&n_bundle_cmds,b->n); atomic_fetch_add(&n_bundles_run,1); atomic_fetch_add(&gfx_bundles,1);
}
#if KH_GFX_TRAMP
/* the trampoline bridge takes three native entry points itself (native_call.c overrides): vkd3d has no bundles */
/* ID3D12Device::CreateCommandAllocator(this, type, riid, **pp) */
static int ov_create_allocator(KCtx *g){
    if((u32)A1(g)!=D3D12_COMMAND_LIST_TYPE_BUNDLE) return 0;
    if(!A3(g)) kh_fatal(g,"gfx-bundle: CreateCommandAllocator(BUNDLE) without a result pointer");
    wr64(g,A3(g),kh_gfxc_bundle_allocator_new(g)); SET_RAX(g,0); KH_RET(g); return 1;
}
/* ID3D12Device::CreateCommandList(this, node mask, type, allocator, initial pipeline state, riid, **pp) */
static int ov_create_list(KCtx *g){
    if((u32)A2(g)!=D3D12_COMMAND_LIST_TYPE_BUNDLE) return 0;
    u64 pp=ARG(g,6); if(!pp) kh_fatal(g,"gfx-bundle: CreateCommandList(BUNDLE) without a result pointer");
    wr64(g,pp,kh_gfxc_bundle_new(g,A3(g),ARG(g,4))); SET_RAX(g,0); KH_RET(g); return 1;
}
/* ID3D12GraphicsCommandList::ExecuteBundle(this = real direct list, bundle = our recorder) */
static int ov_execute_bundle(KCtx *g){ kh_gfxc_bundle_replay(g,(ID3D12GraphicsCommandList*)(uintptr_t)A0(g),A1(g)); KH_RET(g); return 1; }
#endif

/* ---- census (TASK18): what the game asks for, for the report -------------------------------------------------------------------
 * Render-target / depth-stencil textures and every multisampled texture by (format, size, samples, flags); barriers by type; the
 * distinct pairs of ALIASING barriers (vkd3d 2.1 answers those with "Aliasing barriers not implemented yet": it does nothing, which
 * is correct as long as the game initialises the resource it switches to before reading it); ResolveSubresource calls. */
typedef struct { u32 dim, fmt, samples, flags, w, h, mips; u64 n; } ResKind;
typedef struct { u64 before, after, n; u32 bf, af, bw, bh, aw, ah, bfl, afl; } AliasPair;
static ResKind res_kinds[48]; static int n_res_kinds; static AliasPair alias_pairs[16]; static int n_alias_pairs; static pthread_mutex_t cm=PTHREAD_MUTEX_INITIALIZER;
/* TASK21b: the plain sampled textures by DXGI format (which block-compressed formats does the game upload? a device without BC support
 * would need them decoded on the CPU) */
static struct { u32 fmt; u64 n; } tex_fmts[24]; static int n_tex_fmts; static u64 c_tex_fmt_more;
static _Atomic u64 c_res_total, c_res_buffers, c_res_sampled, c_bar_transition, c_bar_aliasing, c_bar_uav, c_resolves, c_msaa_resources;
void kh_gfxc_census_resource(const D3D12_RESOURCE_DESC *d){
    if(!d) return; D3D12_RESOURCE_DESC c; memcpy(&c,d,sizeof c);                  /* the caller's pointer may be guest memory: no alignment assumed */
    u32 dim=c.Dimension, fmt=c.Format, samples=c.SampleDesc.Count, flags=c.Flags, w=(u32)c.Width, h=c.Height, mips=c.MipLevels;
    atomic_fetch_add(&c_res_total,1);
    if(dim==1){ atomic_fetch_add(&c_res_buffers,1); return; }
    if(samples>1) atomic_fetch_add(&c_msaa_resources,1);
    if(!(flags&3)&&samples<=1){ atomic_fetch_add(&c_res_sampled,1);
        pthread_mutex_lock(&cm); int j; for(j=0;j<n_tex_fmts;j++) if(tex_fmts[j].fmt==fmt) break;
        if(j==n_tex_fmts&&n_tex_fmts<24){ tex_fmts[j].fmt=fmt; tex_fmts[j].n=0; n_tex_fmts++; }
        if(j<24) tex_fmts[j].n++; else c_tex_fmt_more++; pthread_mutex_unlock(&cm); return; }
    pthread_mutex_lock(&cm); int i; for(i=0;i<n_res_kinds;i++){ ResKind *k=&res_kinds[i]; if(k->dim==dim&&k->fmt==fmt&&k->samples==samples&&k->flags==flags&&k->w==w&&k->h==h&&k->mips==mips) break; }
    if(i==n_res_kinds&&n_res_kinds<48){ res_kinds[i]=(ResKind){dim,fmt,samples,flags,w,h,mips,0}; n_res_kinds++; }
    if(i<48) res_kinds[i].n++; pthread_mutex_unlock(&cm);
}
void kh_gfxc_census_resolve(void){ atomic_fetch_add(&c_resolves,1); }
/* TASK21b: what the game's pipeline states and depth-stencil views ask of the rasterizer.  These are the questions a Vulkan-on-Metal
 * device raises (MoltenVK has no VK_EXT_depth_clip_enable, no geometry shaders, no stream output, and vkd3d maps D24_UNORM_S8_UINT to
 * D32_SFLOAT_S8_UINT where the 24-bit format is missing): is depth clipping ever turned OFF, which depth formats and sample counts are
 * asked for, which shader stages.  Counted by the wrapped bridge at CreateGraphicsPipelineState / CreateDepthStencilView. */
typedef struct { u32 dsv, rtv0, nrt, samples, depth, stencil, clip; u64 n; } PsoKind;
typedef struct { u32 view, res, samples; u64 n; } DsvKind;
static PsoKind pso_kinds[48]; static int n_pso_kinds; static DsvKind dsv_kinds[16]; static int n_dsv_kinds;
static _Atomic u64 c_pso, c_pso_noclip, c_pso_wire, c_pso_gs, c_pso_tess, c_pso_so, c_pso_cons, c_pso_bias, c_pso_a2c, c_pso_msen, c_pso_aaline, c_pso_forced, c_pso_more, c_dsv, c_dsv_more;
void kh_gfxc_census_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d){
    if(!d) return; const D3D12_RASTERIZER_DESC *r=&d->RasterizerState;
    atomic_fetch_add(&c_pso,1);
    if(!r->DepthClipEnable) atomic_fetch_add(&c_pso_noclip,1);
    if(r->FillMode==D3D12_FILL_MODE_WIREFRAME) atomic_fetch_add(&c_pso_wire,1);
    if(d->GS.pShaderBytecode&&d->GS.BytecodeLength) atomic_fetch_add(&c_pso_gs,1);
    if((d->HS.pShaderBytecode&&d->HS.BytecodeLength)||(d->DS.pShaderBytecode&&d->DS.BytecodeLength)) atomic_fetch_add(&c_pso_tess,1);
    if(d->StreamOutput.NumEntries) atomic_fetch_add(&c_pso_so,1);
    if(r->ConservativeRaster) atomic_fetch_add(&c_pso_cons,1);
    if(r->DepthBias||r->SlopeScaledDepthBias!=0.0f) atomic_fetch_add(&c_pso_bias,1);
    if(d->BlendState.AlphaToCoverageEnable) atomic_fetch_add(&c_pso_a2c,1);
    if(r->MultisampleEnable) atomic_fetch_add(&c_pso_msen,1);
    if(r->AntialiasedLineEnable) atomic_fetch_add(&c_pso_aaline,1);
    if(r->ForcedSampleCount) atomic_fetch_add(&c_pso_forced,1);
    PsoKind k={(u32)d->DSVFormat,d->NumRenderTargets?(u32)d->RTVFormats[0]:0,d->NumRenderTargets,d->SampleDesc.Count,d->DepthStencilState.DepthEnable!=0,d->DepthStencilState.StencilEnable!=0,r->DepthClipEnable!=0,0};
    pthread_mutex_lock(&cm); int i; for(i=0;i<n_pso_kinds;i++){ PsoKind *q=&pso_kinds[i]; if(q->dsv==k.dsv&&q->rtv0==k.rtv0&&q->nrt==k.nrt&&q->samples==k.samples&&q->depth==k.depth&&q->stencil==k.stencil&&q->clip==k.clip) break; }
    if(i==n_pso_kinds&&n_pso_kinds<48){ pso_kinds[i]=k; n_pso_kinds++; }
    if(i<48) pso_kinds[i].n++; else atomic_fetch_add(&c_pso_more,1); pthread_mutex_unlock(&cm);
}
/* view_format: the DSV desc's format, 0 = no desc or DXGI_FORMAT_UNKNOWN (the resource's own format is used) */
void kh_gfxc_census_dsv(ID3D12Resource *res,u32 view_format){
    atomic_fetch_add(&c_dsv,1); u32 rf=0, samples=0;
    if(res){ D3D12_RESOURCE_DESC d=ID3D12Resource_GetDesc(res); rf=d.Format; samples=d.SampleDesc.Count; }
    pthread_mutex_lock(&cm); int i; for(i=0;i<n_dsv_kinds;i++) if(dsv_kinds[i].view==view_format&&dsv_kinds[i].res==rf&&dsv_kinds[i].samples==samples) break;
    if(i==n_dsv_kinds&&n_dsv_kinds<16){ dsv_kinds[i]=(DsvKind){view_format,rf,samples,0}; n_dsv_kinds++; }
    if(i<16) dsv_kinds[i].n++; else atomic_fetch_add(&c_dsv_more,1); pthread_mutex_unlock(&cm);
}
/* barriers of one ResourceBarrier call, with REAL resource pointers (calls with more than 64 barriers are not counted: the game batches 8) */
void kh_gfxc_census_barriers(const D3D12_RESOURCE_BARRIER *b,u32 n){
    if(!b||n>64) return;
    for(u32 i=0;i<n;i++){
        if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) atomic_fetch_add(&c_bar_transition,1);
        else if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) atomic_fetch_add(&c_bar_uav,1);
        else if(b[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){ atomic_fetch_add(&c_bar_aliasing,1);
            u64 rb=U(b[i].Aliasing.pResourceBefore), ra=U(b[i].Aliasing.pResourceAfter);
            pthread_mutex_lock(&cm); int k; for(k=0;k<n_alias_pairs;k++) if(alias_pairs[k].before==rb&&alias_pairs[k].after==ra) break;
            if(k==n_alias_pairs&&n_alias_pairs<16){ AliasPair *p=&alias_pairs[k]; memset(p,0,sizeof *p); p->before=rb; p->after=ra; n_alias_pairs++;
                if(rb){ D3D12_RESOURCE_DESC d=ID3D12Resource_GetDesc((ID3D12Resource*)(uintptr_t)rb); p->bf=d.Format; p->bw=(u32)d.Width; p->bh=d.Height; p->bfl=d.Flags; }
                if(ra){ D3D12_RESOURCE_DESC d=ID3D12Resource_GetDesc((ID3D12Resource*)(uintptr_t)ra); p->af=d.Format; p->aw=(u32)d.Width; p->ah=d.Height; p->afl=d.Flags; } }
            if(k<16) alias_pairs[k].n++; pthread_mutex_unlock(&cm); } }
}
#if KH_GFX_TRAMP
/* CreateCommittedResource(this, heap props, heap flags, desc, ...) / CreatePlacedResource(this, heap, offset, desc, ...): desc is the 4th argument of both */
static void post_create_resource(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk; if(!(u32)rax&&a[3]) kh_gfxc_census_resource((const D3D12_RESOURCE_DESC*)hp(g,a[3])); }
static void post_resolve(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)g;(void)a;(void)stk;(void)rax; kh_gfxc_census_resolve(); }
/* ResourceBarrier(this, count, barriers) */
static void post_barrier(KCtx *g,const u64 *a,const u64 *stk,u64 rax){ (void)stk;(void)rax; if(a[2]) kh_gfxc_census_barriers((const D3D12_RESOURCE_BARRIER*)hp(g,a[2]),(u32)a[1]); }
#endif
static void census_report(FILE *f){
    fprintf(f,"kh: gfx vkd3d census: resources created=%llu (buffers=%llu, sampled single-sample textures=%llu, multisampled=%llu); barriers: transition=%llu aliasing=%llu uav=%llu; ResolveSubresource calls=%llu\n",
            P(c_res_total),P(c_res_buffers),P(c_res_sampled),P(c_msaa_resources),P(c_bar_transition),P(c_bar_aliasing),P(c_bar_uav),P(c_resolves));
    pthread_mutex_lock(&cm);
    if(n_tex_fmts){ fprintf(f,"kh: gfx vkd3d census: sampled single-sample textures by DXGI format (fmt71 / 74 / 77 = BC1 / BC2 / BC3, fmt28 / 87 = R8G8B8A8 / B8G8R8A8):");
        for(int i=0;i<n_tex_fmts;i++) fprintf(f," [fmt%u = %llu]",tex_fmts[i].fmt,P(tex_fmts[i].n)); if(c_tex_fmt_more) fprintf(f," [+%llu in formats beyond the table]",P(c_tex_fmt_more)); fputc('\n',f); }
    if(n_res_kinds){ fprintf(f,"kh: gfx vkd3d census: render-target / depth-stencil / multisampled textures (dimension format WxH mips samples flags = count):");
        for(int i=0;i<n_res_kinds;i++){ ResKind *k=&res_kinds[i]; fprintf(f," [%uD fmt%u %ux%u m%u s%u fl0x%x = %llu]",k->dim-1,k->fmt,k->w,k->h,k->mips,k->samples,k->flags,P(k->n)); } fputc('\n',f); }
    if(c_pso){ fprintf(f,"kh: gfx vkd3d census: pipeline states=%llu: DepthClipEnable=FALSE in %llu, wireframe %llu, geometry shader %llu, hull/domain shader %llu, stream output %llu, conservative raster %llu, depth bias %llu, alpha-to-coverage %llu, MultisampleEnable %llu, AntialiasedLineEnable %llu, ForcedSampleCount %llu\n",
                P(c_pso),P(c_pso_noclip),P(c_pso_wire),P(c_pso_gs),P(c_pso_tess),P(c_pso_so),P(c_pso_cons),P(c_pso_bias),P(c_pso_a2c),P(c_pso_msen),P(c_pso_aaline),P(c_pso_forced));
        fprintf(f,"kh: gfx vkd3d census: pipeline states by (DSV format, RTV0 format x targets, samples, depth test, stencil test, depth clip = count):");
        for(int i=0;i<n_pso_kinds;i++){ PsoKind *k=&pso_kinds[i]; fprintf(f," [dsv fmt%u rtv fmt%u x%u s%u depth %u stencil %u clip %u = %llu]",k->dsv,k->rtv0,k->nrt,k->samples,k->depth,k->stencil,k->clip,P(k->n)); }
        if(c_pso_more) fprintf(f," [+%llu in kinds beyond the table]",P(c_pso_more)); fputc('\n',f); }
    if(c_dsv){ fprintf(f,"kh: gfx vkd3d census: depth-stencil views=%llu (view format on resource format, samples = count; view fmt0 = the resource's):",P(c_dsv));
        for(int i=0;i<n_dsv_kinds;i++){ DsvKind *k=&dsv_kinds[i]; fprintf(f," [fmt%u on fmt%u s%u = %llu]",k->view,k->res,k->samples,P(k->n)); }
        if(c_dsv_more) fprintf(f," [+%llu beyond the table]",P(c_dsv_more)); fputc('\n',f); }
    for(int i=0;i<n_alias_pairs;i++){ AliasPair *p=&alias_pairs[i]; fprintf(f,"kh: gfx vkd3d census: aliasing barrier pair %d: before=%llx (fmt%u %ux%u fl0x%x) after=%llx (fmt%u %ux%u fl0x%x) x%llu\n",i,P(p->before),p->bf,p->bw,p->bh,p->bfl,P(p->after),p->af,p->aw,p->ah,p->afl,P(p->n)); }
    pthread_mutex_unlock(&cm);
}

/* ---- d3d12.dll imports ------------------------------------------------------------------------------------------------------ */
#if KH_GFX_TRAMP
static void need_native(KCtx *g,const void *obj,const char *what){
    u64 fn=U(*(void**)*(void***)obj);
    if(!kh_native_range_name(fn,NULL)) kh_fatal(g,"gfx-vtable-outside-native-range: %s %p has its first method at 0x%llx, outside the registered libvkd3d ranges",what,obj,P(fn));
}
/* once, with the first device: the native entry points to count (the null recorder's statistics) and to override (bundles).  vkd3d has one
 * implementation of each interface, so the addresses read from one device / one command list hold for all of them. */
static void learn_entry_points(KCtx *g,ID3D12Device *dev){
    static int done; pthread_mutex_lock(&vm); if(done){ pthread_mutex_unlock(&vm); return; }
    ID3D12CommandAllocator *al=NULL; ID3D12GraphicsCommandList *cl=NULL; ID3D12CommandQueue *q=NULL; D3D12_COMMAND_QUEUE_DESC qd; memset(&qd,0,sizeof qd);
    HRESULT hr=ID3D12Device_CreateCommandAllocator(dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&al);
    if(SUCCEEDED(hr)) hr=ID3D12Device_CreateCommandList(dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,al,NULL,&IID_ID3D12GraphicsCommandList,(void**)&cl);
    if(SUCCEEDED(hr)) hr=ID3D12Device_CreateCommandQueue(dev,&qd,&IID_ID3D12CommandQueue,(void**)&q);
    if(FAILED(hr)) kh_fatal(g,"gfx-vkd3d-call: cannot create the probe allocator / command list / queue on a new device, hr=0x%08x",(unsigned)hr);
    const ID3D12GraphicsCommandListVtbl *v=cl->lpVtbl;
    kh_native_watch(U(v->DrawInstanced),&gfx_draws,0); kh_native_watch(U(v->DrawIndexedInstanced),&gfx_draws,0);
    kh_native_watch(U(v->ClearDepthStencilView),&gfx_clears,0); kh_native_watch(U(v->ClearRenderTargetView),&gfx_clears,0);
    kh_native_watch(U(v->SetPipelineState),&gfx_pso_binds,0); kh_native_watch(U(v->ResourceBarrier),&gfx_barriers,1);
    kh_native_watch(U(v->CopyBufferRegion),&gfx_copies,0); kh_native_watch(U(v->CopyTextureRegion),&gfx_copies,0); kh_native_watch(U(v->CopyResource),&gfx_copies,0); kh_native_watch(U(v->ResolveSubresource),&gfx_copies,0);
    kh_native_watch(U(q->lpVtbl->ExecuteCommandLists),&gfx_execs,0);
    kh_native_watch(U(dev->lpVtbl->CreateGraphicsPipelineState),&gfx_psos,0);
    kh_native_watch(U(dev->lpVtbl->CreateCommittedResource),&gfx_resources,0); kh_native_watch(U(dev->lpVtbl->CreatePlacedResource),&gfx_resources,0);
    kh_native_override(U(dev->lpVtbl->CreateCommandAllocator),ov_create_allocator); kh_native_override(U(dev->lpVtbl->CreateCommandList),ov_create_list);
    kh_native_override(U(v->ExecuteBundle),ov_execute_bundle);
    kh_native_post(U(v->Reset),post_list_reset); kh_native_post(U(v->Release),post_list_release); kh_native_post(U(v->SetGraphicsRootSignature),post_list_rootsig);
    kh_native_post(U(v->SetDescriptorHeaps),post_list_heaps); kh_native_post(U(v->SetGraphicsRootDescriptorTable),post_list_table);
    kh_native_post(U(dev->lpVtbl->CreateRootSignature),post_create_rootsig);
    kh_native_post(U(dev->lpVtbl->CreateCommittedResource),post_create_resource); kh_native_post(U(dev->lpVtbl->CreatePlacedResource),post_create_resource);
    kh_native_post(U(v->ResourceBarrier),post_barrier); kh_native_post(U(v->ResolveSubresource),post_resolve);
    if(kh_native_trace){                                                        /* labels for the trace: one object of each common interface */
        ID3D12DescriptorHeap *dh=NULL; ID3D12Fence *fe=NULL; ID3D12Resource *rb=NULL; D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd,0,sizeof hd); hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
        D3D12_HEAP_PROPERTIES up; memset(&up,0,sizeof up); up.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd; memset(&bd,0,sizeof bd); bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=256; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        NAME_VT(dev,"ID3D12Device",dev_names); NAME_VT(cl,"ID3D12GraphicsCommandList",list_names); NAME_VT(q,"ID3D12CommandQueue",queue_names); NAME_VT(al,"ID3D12CommandAllocator",alloc_names);
        if(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&dh))){ NAME_VT(dh,"ID3D12DescriptorHeap",dheap_names); ID3D12DescriptorHeap_Release(dh); }
        if(SUCCEEDED(ID3D12Device_CreateFence(dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fe))){ NAME_VT(fe,"ID3D12Fence",fence_names); ID3D12Fence_Release(fe); }
        if(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev,&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&rb))){ NAME_VT(rb,"ID3D12Resource",res_names); ID3D12Resource_Release(rb); }
    }
    ID3D12GraphicsCommandList_Close(cl); ID3D12GraphicsCommandList_Release(cl); ID3D12CommandAllocator_Release(al); ID3D12CommandQueue_Release(q);
    done=1; pthread_mutex_unlock(&vm);
}
#endif
const char *kh_gfxc_device_name(void){ return vk_dev_name; }

/* ---- TASK38 (IOS-GRAPHICS-PLAN step 4): the device report, the memory census and the BC decision -----------------------------------
 * One block at the first device (lines `kh: gfx device report: ...`), about the physical device vkd3d really uses
 * (vkd3d_get_vk_physical_device), before the game gets that device:
 *   textureCompressionBC (and ASTC_LDR / ETC2, the alternatives of plan C1); the optimal-tiling features of the game's formats, mapped
 *   to Vulkan exactly as vkd3d maps them (vkd3d_get_vk_format): BC1/2/3 UNORM (DXGI 71/74/77), B8G8R8A8_UNORM (87) and its only depth
 *   format D32_FLOAT_S8X24_UINT (20 -> D32_SFLOAT_S8_UINT); framebuffer and sampled-image sample counts; memory heaps and types, and
 *   VK_EXT_memory_budget's numbers when the implementation offers it; which presentation extensions the implementation OFFERS (the
 *   enabled ones are the `presentation extensions` line); the memory census line (below).
 * A field this build or host does not have prints `absent (why)`, never a number.
 * gfx-no-bc: a BC format the game uses that cannot be SAMPLED in optimal tiling ends the run here, by name, before the game creates
 * its first texture (MoltenVK enables BC on iOS only "where supported by the GPU": plan R1; plan step C1 would then come first).
 *   KHNMC_GFX_TEST_NO_BC=1   TESTS ONLY: the report treats the three BC formats as not sampleable, to show that fatal on a device
 *                            that has them (it can only make a run fail, never pass).
 * The memory census (plan step 4, R11): one line `kh: gfx memory census (<when>): ...` in the report block and with every backend
 * report (every 300 presents and at exit: null_d3d12.c).  phys_footprint (Apple: task_info TASK_VM_INFO), os_proc_available_memory
 * (iOS), MoltenVK's gpuMemoryAllocated (iPhone build; needs MoltenVK's performance tracking, MVK_CONFIG_PERFORMANCE_TRACKING=1, or its
 * update count says 0), on Linux the process's RSS as the stand-in, the VK_EXT_memory_budget numbers, and the fixed budget that
 * QueryVideoMemoryInfo answers today. */
u64 kh_dxgi_budget_answer(void);                                     /* null_d3d12.c: QueryVideoMemoryInfo's (fixed) budget */
/* vk = 0: Vulkan format from vkd3d_get_vk_format.  The depth format has a fixed one: vkd3d's public vkd3d_get_vk_format looks depth
 * formats up through a DEVICE and is given none (vkd3d 2.1 utils.c 424-430, 442-468: an assertion, then a NULL dereference; found on
 * lavapipe in TASK38), so the report uses the row of vkd3d's own table (utils.c 128: D32_FLOAT_S8X24_UINT -> D32_SFLOAT_S8_UINT, never
 * overridden; only the D24S8 formats are) */
typedef struct { const char *name; DXGI_FORMAT dxgi; VkFormat vk; } RepFmt;
static const RepFmt rep_fmt[]={{"BC1_UNORM",DXGI_FORMAT_BC1_UNORM,0},{"BC2_UNORM",DXGI_FORMAT_BC2_UNORM,0},{"BC3_UNORM",DXGI_FORMAT_BC3_UNORM,0},
                               {"B8G8R8A8_UNORM",DXGI_FORMAT_B8G8R8A8_UNORM,0},{"D32_FLOAT_S8X24_UINT",DXGI_FORMAT_D32_FLOAT_S8X24_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT}};
static VkFormat rep_vk_format(int i){ return rep_fmt[i].vk?rep_fmt[i].vk:vkd3d_get_vk_format(rep_fmt[i].dxgi); }
static const char *const rep_iext[]={"VK_KHR_surface","VK_EXT_headless_surface","VK_EXT_metal_surface","VK_EXT_layer_settings"};
static const char *const rep_dext[]={"VK_KHR_swapchain","VK_GOOGLE_display_timing","VK_KHR_present_id","VK_KHR_present_wait","VK_EXT_metal_objects","VK_EXT_memory_budget"};
enum { NREPFMT=5, NBC=3, NREPIEXT=4, NREPDEXT=6, REP_DEXT_BUDGET=5 };
typedef struct { int printed, lines, tcbc, test_no_bc, unsampled, memlow_armed, iext[NREPIEXT], dext[NREPDEXT]; VkFormat vkf[NREPFMT]; VkFormatFeatureFlags feat[NREPFMT]; } RepState;
static RepState rep; static pthread_mutex_t rep_m=PTHREAD_MUTEX_INITIALIZER;
/* census state: the report's physical device and budget query, and the device of the last swap chain (MoltenVK's statistics) */
static pthread_mutex_t cen_m=PTHREAD_MUTEX_INITIALIZER; static VkPhysicalDevice cen_pd; static PFN_vkGetPhysicalDeviceMemoryProperties2 cen_mp2;
static int cen_budget; static ID3D12Device *cen_dev; static u64 cen_lines;
#if defined(__APPLE__)
static u64 cen_peak;                                                   /* the largest phys_footprint a census has seen */
#endif
/* the BC decision (pure; selftested): which of the game's BC formats (the first NBC of rep_fmt) cannot be SAMPLED in optimal tiling */
static int bc_unsampled(const VkFormatFeatureFlags *feat,char *why,size_t cap){
    int n=0; if(cap) why[0]=0;
    for(int i=0;i<NBC;i++) if(!(feat[i]&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){ size_t l=strlen(why); if(l<cap) snprintf(why+l,cap-l,"%s%s",n?", ":"",rep_fmt[i].name); n++; }
    return n;
}
static void feat_text(char *b,size_t cap,VkFormatFeatureFlags f){
    static const struct { VkFormatFeatureFlags bit; const char *n; } fb[]={{VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,"sampled"},{VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,"filter_linear"},
        {VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,"color_attachment"},{VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT,"blend"},{VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,"depth_stencil"},
        {VK_FORMAT_FEATURE_BLIT_SRC_BIT,"blit_src"},{VK_FORMAT_FEATURE_BLIT_DST_BIT,"blit_dst"},{VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,"transfer_src"},{VK_FORMAT_FEATURE_TRANSFER_DST_BIT,"transfer_dst"},
        {VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,"storage"}};
    size_t l=(size_t)snprintf(b,cap,"0x%x (",(unsigned)f); int k=0;
    for(size_t i=0;i<sizeof fb/sizeof*fb&&l<cap;i++) if(f&fb[i].bit) l+=(size_t)snprintf(b+l,cap-l,"%s%s",k++?" ":"",fb[i].n);
    if(l<cap) snprintf(b+l,cap-l,"%s)",k?"":"none");
}
static void sc_text(char *b,size_t cap,VkSampleCountFlags s){
    size_t l=0; int k=0; b[0]=0;
    for(u32 bit=1;bit<=64&&l<cap;bit<<=1) if(s&bit) l+=(size_t)snprintf(b+l,cap-l,"%s%u",k++?",":"",bit);
    if(!k) snprintf(b,cap,"none");
}
static int ext_in(const VkExtensionProperties *x,u32 n,const char *s){ for(u32 i=0;i<n;i++) if(!strcmp(x[i].extensionName,s)) return 1; return 0; }
/* the census text (one line).  dev: the device whose MoltenVK statistics are read (NULL = the last swap chain's) */
static void census_text(char *b,size_t cap,ID3D12Device *dev){
    size_t l=0; b[0]=0;
#define CAT(...) do{ if(l<cap) l+=(size_t)snprintf(b+l,cap-l,__VA_ARGS__); }while(0)
    pthread_mutex_lock(&cen_m);
#if defined(__APPLE__)
    { task_vm_info_data_t vi; mach_msg_type_number_t cnt=TASK_VM_INFO_COUNT; memset(&vi,0,sizeof vi);
      if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vi,&cnt)==KERN_SUCCESS){ u64 pf=(u64)vi.phys_footprint; if(pf>cen_peak) cen_peak=pf;
          CAT("phys_footprint=%llu MiB (peak seen %llu MiB)",P(pf>>20),P(cen_peak>>20)); }
      else CAT("phys_footprint=ERROR (task_info TASK_VM_INFO failed)"); }
#if TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    CAT(" os_proc_available_memory=%llu MiB",P((u64)os_proc_available_memory()>>20));
#else
    CAT(" os_proc_available_memory=absent (iOS only)");
#endif
#else
    CAT("phys_footprint=absent (Apple only: task_info TASK_VM_INFO) os_proc_available_memory=absent (iOS only)");
#endif
#ifdef KH_GFX_MVK_API
    { ID3D12Device *d=dev?dev:cen_dev;
      if(!d) CAT(" gpuMemoryAllocated=absent (no live swap chain to read it from)");
      else { MVKPerformanceStatistics ps; memset(&ps,0,sizeof ps); size_t sz=sizeof ps; VkResult r=vkGetPerformanceStatisticsMVK(vkd3d_get_vk_device(d),&ps,&sz);
          if(r!=VK_SUCCESS) CAT(" gpuMemoryAllocated=ERROR (vkGetPerformanceStatisticsMVK result %d, %zu of %zu bytes: MoltenVK's private API differs from the headers' version %d)",(int)r,sz,sizeof ps,MVK_PRIVATE_API_VERSION);
          else CAT(" gpuMemoryAllocated=%.0f KiB (max %.0f KiB, %u updates%s)",ps.device.gpuMemoryAllocated.latest,ps.device.gpuMemoryAllocated.maximum,ps.device.gpuMemoryAllocated.count,
                   ps.device.gpuMemoryAllocated.count?"":"; 0 = MoltenVK's performance tracking is off: MVK_CONFIG_PERFORMANCE_TRACKING=1"); } }
#else
    (void)dev; CAT(" gpuMemoryAllocated=absent (MoltenVK's statistics are read in the iPhone build only: KH_GFX_MVK_API)");
#endif
#if defined(__linux__)
    { long now=-1,peak=-1; FILE *s=fopen("/proc/self/status","r"); char ln[256];
      if(s){ while(fgets(ln,sizeof ln,s)){ if(!strncmp(ln,"VmRSS:",6)) now=strtol(ln+6,0,10)/1024; else if(!strncmp(ln,"VmHWM:",6)) peak=strtol(ln+6,0,10)/1024; } fclose(s); }
      CAT(" rss=%ld MiB (peak %ld MiB; /proc/self/status, Linux's stand-in for phys_footprint)",now,peak); }
#endif
    if(!cen_pd) CAT("; VK_EXT_memory_budget: no device yet");
    else if(!cen_budget) CAT("; VK_EXT_memory_budget=absent (not offered by this implementation)");
    else { VkPhysicalDeviceMemoryBudgetPropertiesEXT bp; memset(&bp,0,sizeof bp); bp.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 mp; memset(&mp,0,sizeof mp); mp.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2; mp.pNext=&bp; cen_mp2(cen_pd,&mp);
        CAT("; VK_EXT_memory_budget:"); for(u32 i=0;i<mp.memoryProperties.memoryHeapCount&&i<VK_MAX_MEMORY_HEAPS;i++) CAT(" heap%u budget=%llu MiB usage=%llu MiB",i,P(bp.heapBudget[i]>>20),P(bp.heapUsage[i]>>20)); }
    CAT("; QueryVideoMemoryInfo answers budget=%llu MiB (fixed: null_d3d12.c)",P(kh_dxgi_budget_answer()>>20));
    cen_lines++; pthread_mutex_unlock(&cen_m);
#undef CAT
}
static ID3D12Device *exit_stat_dev(void){ pthread_mutex_lock(&cen_m); ID3D12Device *d=cen_dev; pthread_mutex_unlock(&cen_m); return d; }   /* TASK43: MoltenVK's statistics at exit */
static void census_line(FILE *f,const char *when){ char b[2048]; census_text(b,sizeof b,NULL); fprintf(f,"kh: gfx memory census (%s): %s\n",when,b); }
/* ---- review X38 n=2 (IOS-GRAPHICS-PLAN step 5; verification correction 6): the gfx-memory-low soft stop -----------------------------------
 * At every Present (kh_vkd3d_present: after the suspend gate, before the frame readback), where os_proc_available_memory exists (iOS):
 * below MEMLOW_MARGIN_MIB the run ends BY NAME instead of by a jetsam kill that leaves nothing: `kh: FATAL: gfx-memory-low: ...`, a
 * census line, then kh_exit_process(3), which writes every exit report (the census among them) and the status `exit 3`.  Elsewhere
 * there is no reading and the stop is not armed; the device report says which.
 *   KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB=n   TESTS ONLY: a reading of n MiB.  Where a real reading exists the lower of the two counts,
 *                                           so it can only stop a run, never keep one going. */
#define MEMLOW_MARGIN_MIB 512ull
static atomic_int memlow_fired;
static int memlow_fires(int have,u64 avail_mib){ return have&&avail_mib<MEMLOW_MARGIN_MIB; }          /* the decision (pure; selftested) */
static int memlow_read(u64 *mib){                                                                    /* 1 and *mib when there is a reading */
    int have=0; u64 v=0;
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
    v=(u64)os_proc_available_memory()>>20; have=1;
#endif
    if(memlow_test>=0&&(!have||(u64)memlow_test<v)){ v=(u64)memlow_test; have=1; }
    *mib=v; return have;
}
static void memlow_check(u64 n){
    void kh_exit_process(int);
    u64 mib; int have=memlow_read(&mib);
    if(!memlow_fires(have,mib)||atomic_exchange(&memlow_fired,1)) return;
    fprintf(stderr,"kh: FATAL: gfx-memory-low: os_proc_available_memory=%llu MiB%s is below the margin of %llu MiB at present %llu: the run ends here with its reports "
                   "(status 3) instead of being killed by iOS (IOS-GRAPHICS-PLAN step 5)\n",P(mib),memlow_test>=0?" [TEST: KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB]":"",P(MEMLOW_MARGIN_MIB),P(n));
    census_line(stderr,"gfx-memory-low");
    kh_exit_process(3);
}
/* the report block, once per process, at the first device (kh_gfxc_device_new; no other lock held).  Ends the run with gfx-no-bc when
 * a BC format the game uses cannot be sampled: after the block is printed and every lock is released. */
#define REPL(...) do{ fprintf(stderr,"kh: gfx device report: " __VA_ARGS__); rep.lines++; }while(0)
static void dev_report(KCtx *g,ID3D12Device *dev){
    pthread_mutex_lock(&rep_m); if(rep.printed){ pthread_mutex_unlock(&rep_m); return; }
    rep.printed=1; const char *e=getenv("KHNMC_GFX_TEST_NO_BC"); rep.test_no_bc=e&&!strcmp(e,"1");
    VkInstance vi=vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(dev)); VkPhysicalDevice pd=vkd3d_get_vk_physical_device(dev);
#define RG(T,n,inst) T n=(T)vk_gipa(inst,#n)
    RG(PFN_vkGetPhysicalDeviceProperties,vkGetPhysicalDeviceProperties,vi); RG(PFN_vkGetPhysicalDeviceFeatures,vkGetPhysicalDeviceFeatures,vi);
    RG(PFN_vkGetPhysicalDeviceFormatProperties,vkGetPhysicalDeviceFormatProperties,vi); RG(PFN_vkGetPhysicalDeviceMemoryProperties,vkGetPhysicalDeviceMemoryProperties,vi);
    RG(PFN_vkEnumerateDeviceExtensionProperties,vkEnumerateDeviceExtensionProperties,vi); RG(PFN_vkEnumerateInstanceExtensionProperties,vkEnumerateInstanceExtensionProperties,NULL);
    RG(PFN_vkGetPhysicalDeviceMemoryProperties2,vkGetPhysicalDeviceMemoryProperties2,vi);
#undef RG
    if(!vkGetPhysicalDeviceMemoryProperties2) vkGetPhysicalDeviceMemoryProperties2=(PFN_vkGetPhysicalDeviceMemoryProperties2)vk_gipa(vi,"vkGetPhysicalDeviceMemoryProperties2KHR");
    const char *missing=!vkGetPhysicalDeviceProperties?"vkGetPhysicalDeviceProperties":!vkGetPhysicalDeviceFeatures?"vkGetPhysicalDeviceFeatures":!vkGetPhysicalDeviceFormatProperties?"vkGetPhysicalDeviceFormatProperties":
                        !vkGetPhysicalDeviceMemoryProperties?"vkGetPhysicalDeviceMemoryProperties":!vkEnumerateDeviceExtensionProperties?"vkEnumerateDeviceExtensionProperties":!vkEnumerateInstanceExtensionProperties?"vkEnumerateInstanceExtensionProperties":NULL;
    if(missing){ pthread_mutex_unlock(&rep_m); kh_fatal(g,"gfx-device-report: the Vulkan library (%s) gives no %s for the device report",vk_lib_name,missing); }
    VkPhysicalDeviceProperties pp; memset(&pp,0,sizeof pp); vkGetPhysicalDeviceProperties(pd,&pp);
    VkPhysicalDeviceFeatures ft; memset(&ft,0,sizeof ft); vkGetPhysicalDeviceFeatures(pd,&ft); rep.tcbc=(int)ft.textureCompressionBC;
    REPL("BEGIN: '%s' type=%s vendor=0x%x device=0x%x api=%u.%u.%u driver=0x%x (the physical device vkd3d uses); built with Vulkan headers %u.%u.%u\n",pp.deviceName,pd_type(pp.deviceType),pp.vendorID,pp.deviceID,
         VK_API_VERSION_MAJOR(pp.apiVersion),VK_API_VERSION_MINOR(pp.apiVersion),VK_API_VERSION_PATCH(pp.apiVersion),pp.driverVersion,
         VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));
    REPL("textureCompressionBC=%d (textureCompressionASTC_LDR=%d, textureCompressionETC2=%d)\n",(int)ft.textureCompressionBC,(int)ft.textureCompressionASTC_LDR,(int)ft.textureCompressionETC2);
    for(int i=0;i<NREPFMT;i++){ VkFormatProperties fp; memset(&fp,0,sizeof fp); rep.vkf[i]=rep_vk_format(i);
        if(rep.vkf[i]!=VK_FORMAT_UNDEFINED) vkGetPhysicalDeviceFormatProperties(pd,rep.vkf[i],&fp);
        rep.feat[i]=fp.optimalTilingFeatures; if(rep.test_no_bc&&i<NBC) rep.feat[i]&=~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        char t[300]; feat_text(t,sizeof t,rep.feat[i]);
        REPL("format %s (DXGI %d -> VkFormat %d%s): optimalTilingFeatures=%s%s\n",rep_fmt[i].name,(int)rep_fmt[i].dxgi,(int)rep.vkf[i],rep_fmt[i].vk?", vkd3d's table row":"",t,rep.test_no_bc&&i<NBC?" [TEST: KHNMC_GFX_TEST_NO_BC=1 removed sampled]":""); }
    { char a[64],b2[64],c[64],d[64],s1[64],s2[64]; const VkPhysicalDeviceLimits *L=&pp.limits;
      sc_text(a,sizeof a,L->framebufferColorSampleCounts); sc_text(b2,sizeof b2,L->framebufferDepthSampleCounts); sc_text(c,sizeof c,L->framebufferStencilSampleCounts);
      sc_text(d,sizeof d,L->framebufferNoAttachmentsSampleCounts); sc_text(s1,sizeof s1,L->sampledImageColorSampleCounts); sc_text(s2,sizeof s2,L->sampledImageDepthSampleCounts);
      REPL("sample counts: framebuffer color=%s depth=%s stencil=%s no-attachments=%s; sampled image color=%s depth=%s\n",a,b2,c,d,s1,s2); }
    VkPhysicalDeviceMemoryProperties mp; memset(&mp,0,sizeof mp); vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    for(u32 i=0;i<mp.memoryHeapCount&&i<VK_MAX_MEMORY_HEAPS;i++)
        REPL("memory heap %u: %llu MiB%s%s\n",i,P(mp.memoryHeaps[i].size>>20),mp.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT?" DEVICE_LOCAL":"",mp.memoryHeaps[i].flags&VK_MEMORY_HEAP_MULTI_INSTANCE_BIT?" MULTI_INSTANCE":"");
    { char t[1600]; size_t l=0; t[0]=0;
      for(u32 i=0;i<mp.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES&&l<sizeof t;i++){ VkMemoryPropertyFlags pf=mp.memoryTypes[i].propertyFlags;
          l+=(size_t)snprintf(t+l,sizeof t-l," %u:heap%u%s%s%s%s%s",i,mp.memoryTypes[i].heapIndex,pf&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT?",DEVICE_LOCAL":"",pf&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT?",HOST_VISIBLE":"",
                              pf&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT?",HOST_COHERENT":"",pf&VK_MEMORY_PROPERTY_HOST_CACHED_BIT?",HOST_CACHED":"",pf&VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT?",LAZILY_ALLOCATED":""); }
      REPL("memory types (%u):%s\n",mp.memoryTypeCount,t); }
    { u32 ni=0,nd=0; vkEnumerateInstanceExtensionProperties(NULL,&ni,NULL); vkEnumerateDeviceExtensionProperties(pd,NULL,&nd,NULL);
      VkExtensionProperties *xi=calloc(ni+1,sizeof*xi), *xd=calloc(nd+1,sizeof*xd);
      if(!xi||!xd){ pthread_mutex_unlock(&rep_m); kh_fatal(g,"gfx-device-report: out of memory"); }
      vkEnumerateInstanceExtensionProperties(NULL,&ni,xi); vkEnumerateDeviceExtensionProperties(pd,NULL,&nd,xd);
      char t[600]; size_t l=(size_t)snprintf(t,sizeof t,"instance");
      for(int i=0;i<NREPIEXT;i++){ rep.iext[i]=ext_in(xi,ni,rep_iext[i]); if(l<sizeof t) l+=(size_t)snprintf(t+l,sizeof t-l," %s=%d",rep_iext[i],rep.iext[i]); }
      if(l<sizeof t) l+=(size_t)snprintf(t+l,sizeof t-l,"; device");
      for(int i=0;i<NREPDEXT;i++){ rep.dext[i]=ext_in(xd,nd,rep_dext[i]); if(l<sizeof t) l+=(size_t)snprintf(t+l,sizeof t-l," %s=%d",rep_dext[i],rep.dext[i]); }
      free(xi); free(xd);
      REPL("extensions OFFERED by the implementation (1 = offered; what vkd3d enabled is the `presentation extensions` line): %s\n",t); }
    pthread_mutex_lock(&cen_m); cen_pd=pd; cen_mp2=vkGetPhysicalDeviceMemoryProperties2; cen_budget=rep.dext[REP_DEXT_BUDGET]&&vkGetPhysicalDeviceMemoryProperties2!=NULL; pthread_mutex_unlock(&cen_m);
    if(rep.dext[REP_DEXT_BUDGET]&&!vkGetPhysicalDeviceMemoryProperties2) REPL("VK_EXT_memory_budget is offered but there is no vkGetPhysicalDeviceMemoryProperties2: the census has no budget numbers\n");
    else REPL("VK_EXT_memory_budget=%d (%s)\n",rep.dext[REP_DEXT_BUDGET],rep.dext[REP_DEXT_BUDGET]?"its heap budget and usage are in the census line":"absent: no budget numbers in the census line");
    REPL("MoltenVK Metal features (argumentBuffersTier, supportedSampleCounts)=absent (not read: vkGetPhysicalDeviceMetalFeaturesMVK is declared in mvk_deprecated_api.h, which needs the IOSurface headers; the Vulkan sample counts are above)\n");
    { char t[2048]; census_text(t,sizeof t,dev); REPL("memory census: %s\n",t); }
    { u64 m; rep.memlow_armed=memlow_read(&m);                                                   /* review X38 n=2 */
      if(rep.memlow_armed) REPL("memory soft stop gfx-memory-low: armed: os_proc_available_memory below %llu MiB at a Present ends the run by name (status 3, reports written); now %llu MiB%s\n",
                                P(MEMLOW_MARGIN_MIB),P(m),memlow_test>=0?" [TEST: KHNMC_GFX_TEST_MEMORY_AVAILABLE_MIB]":"");
      else REPL("memory soft stop gfx-memory-low: not armed (no os_proc_available_memory on this platform: iOS only)\n"); }
    char why[128]; rep.unsampled=bc_unsampled(rep.feat,why,sizeof why);
    if(rep.unsampled) REPL("BC decision: NOT sampleable in optimal tiling: %s (textureCompressionBC=%d) -> named fatal gfx-no-bc: plan step C1 (CPU decode) comes before any game picture\n",why,rep.tcbc);
    else REPL("BC decision: BC1, BC2 and BC3 UNORM are sampleable in optimal tiling (textureCompressionBC=%d): the game's BC textures are used as they are\n",rep.tcbc);
    int unsampled=rep.unsampled, tcbc=rep.tcbc, test=rep.test_no_bc; rep.lines++;
    fprintf(stderr,"kh: gfx device report: END (%d lines)\n",rep.lines);
    pthread_mutex_unlock(&rep_m);
    if(unsampled) kh_fatal(g,"gfx-no-bc: the Vulkan device '%s' cannot sample %s in optimal tiling (textureCompressionBC=%d)%s: the game's textures are 71%% BC3 and cannot be drawn. "
                             "IOS-GRAPHICS-PLAN step C1 (decode BC on the CPU) or the ASTC transcode must come first",vk_dev_name,why,tcbc,test?" [TEST: KHNMC_GFX_TEST_NO_BC=1]":"");
}
#undef REPL
/* the vkd3d instance (made once) and a NEW device of at least `level` on it.  S_OK and *out, or the failing HRESULT (reported here) */
HRESULT kh_gfxc_device_new(KCtx *g,u32 level,u64 adapter_shown,ID3D12Device **out){
    (void)g; *out=NULL;
    pthread_mutex_lock(&vm); HRESULT hr=instance_get(); pthread_mutex_unlock(&vm);
    if(FAILED(hr)) return hr;
    struct vkd3d_optional_device_extensions_info ode; memset(&ode,0,sizeof ode); ode.type=VKD3D_STRUCTURE_TYPE_OPTIONAL_DEVICE_EXTENSIONS_INFO;
    ode.extensions=opt_dev_ext; ode.extension_count=(uint32_t)(sizeof opt_dev_ext/sizeof*opt_dev_ext);
    struct vkd3d_device_create_info dci; memset(&dci,0,sizeof dci); dci.type=VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.next=&ode; dci.minimum_feature_level=(D3D_FEATURE_LEVEL)level; dci.instance=vk_inst;
    ID3D12Device *dev=NULL; hr=vkd3d_create_device(&dci,&IID_ID3D12Device,(void**)&dev);
    if(FAILED(hr)||!dev){
        fprintf(stderr,"kh: gfx vkd3d: D3D12CreateDevice(adapter 0x%llx [ours, not passed on], feature level 0x%x) FAILED: vkd3d_create_device hr=0x%08x\n",P(adapter_shown),level,(unsigned)hr);
        return FAILED(hr)?hr:E_FAIL; }
    if(!kh_gfxc_pres_ext.seen_instance||!kh_gfxc_pres_ext.seen_device)
        kh_fatal(g,"gfx-gipa-wrapper: a vkd3d device was created but the gipa wrapper saw %s: vkd3d did not load %s through the wrapper",
                 !kh_gfxc_pres_ext.seen_instance?"no vkCreateInstance":"no vkCreateDevice",!kh_gfxc_pres_ext.seen_instance?"vkCreateInstance":"vkCreateDevice");
    pthread_mutex_lock(&vm); pres_ext_report(); pthread_mutex_unlock(&vm);
    dev_report(g,dev);                                                           /* TASK38: once, at the first device; may end the run (gfx-no-bc) */
    *out=dev; return S_OK;
}
/* D3D12CreateDevice(adapter, featureLevel, riid, **ppDevice): the adapter is OUR DXGI object and is not passed on.  The game gets the host
 * device itself (trampoline bridge) or the guest object that wraps it (wrapped bridge). */
void kh_vkd3d_create_device(KCtx *g){
    u32 level=(u32)A1(g); u64 riid=A2(g), pp=A3(g);
    gw_probe_tls=!pp;                                                              /* TASK43: a capability probe's VkPipelineCache gets no cache data */
    ID3D12Device *dev=NULL; HRESULT hr=kh_gfxc_device_new(g,level,A0(g),&dev);
    gw_probe_tls=0;
    if(FAILED(hr)){ if(pp) wr64(g,pp,0); SET_RAX(g,(u32)hr); return; }
#if KH_GFX_TRAMP
    if(!kh_gfx_wrap_on) need_native(g,dev,"ID3D12Device");
#endif
    if(!pp){                                                                    /* capability probe: S_FALSE, no device returned */
        ID3D12Device_Release(dev); atomic_fetch_add(&n_probe_devices,1);
        fprintf(stderr,"kh: gfx vkd3d: D3D12CreateDevice probe (ppDevice=NULL, feature level 0x%x) -> S_FALSE on %s\n",level,vk_dev_name);
        SET_RAX(g,1); return; }
    void *out=NULL; hr=ID3D12Device_QueryInterface(dev,(const IID*)hp(g,riid),&out);
#if KH_GFX_TRAMP
    if(!kh_gfx_wrap_on) learn_entry_points(g,dev);
#endif
    ID3D12Device_Release(dev);
    if(SUCCEEDED(hr)&&kh_gfx_wrap_on&&out!=(void*)dev) kh_fatal(g,"gfx-wrap-qi-identity: the new device %p answers the requested interface with another pointer (%p); the wrapped bridge keeps one guest object per real pointer",(void*)dev,out);
    u64 shown=SUCCEEDED(hr)&&out?shown_of(g,out,WK_DEVICE):0;
    if(FAILED(hr)) fprintf(stderr,"kh: gfx vkd3d: D3D12CreateDevice: the device does not support the requested interface (QueryInterface hr=0x%08x)\n",(unsigned)hr);
    else { atomic_fetch_add(&n_devices,1); fprintf(stderr,"kh: gfx vkd3d: D3D12CreateDevice(feature level 0x%x) -> device %p on %s%s\n",level,out,vk_dev_name,kh_gfx_wrap_on?" (wrapped)":""); if(kh_gfx_wrap_on) fprintf(stderr,"kh: gfx wrap: the game sees that device as guest object 0x%llx\n",P(shown)); }
    wr64(g,pp,shown); SET_RAX(g,(u32)hr);
}
/* D3D12SerializeRootSignature(desc, version, **blob, **error_blob): the blobs are vkd3d-utils objects (trampoline bridge: handed out as they are) */
void kh_vkd3d_serialize_rootsig(KCtx *g){
    if(kh_gfx_wrap_on){ kh_wrap_serialize_rootsig(g); return; }
#if KH_GFX_TRAMP
    HRESULT hr=D3D12SerializeRootSignature((const D3D12_ROOT_SIGNATURE_DESC*)hp(g,A0(g)),(D3D_ROOT_SIGNATURE_VERSION)(u32)A1(g),(ID3DBlob**)hp(g,A2(g)),A3(g)?(ID3DBlob**)hp(g,A3(g)):NULL);
    if(FAILED(hr)) fprintf(stderr,"kh: gfx vkd3d: D3D12SerializeRootSignature(version %u) FAILED hr=0x%08x\n",(unsigned)A1(g),(unsigned)hr);
    else need_native(g,(void*)rd64(g,A2(g)),"ID3DBlob");
    SET_RAX(g,(u32)hr);
#else
    kh_fatal(g,"gfx-vkd3d-needs-identity: D3D12SerializeRootSignature reached without the wrapped bridge in a build that has no trampoline bridge");
#endif
}

/* ---- swap chain (the guest object is null_d3d12.c's: +32 width, +36 height, +40 present counter, +48 latency event) ------------ */
#define SW_PTR 88                               /* guest swap-chain object: host Swap* */
#define NBUF 3
/* TASK36 (plan step 6): the back buffers are VkImages WE own, made on vkd3d's VkDevice with vkd3d's own settings for a committed
 * render target (flags 0; TRANSFER_SRC|TRANSFER_DST|COLOR_ATTACHMENT|SAMPLED; OPTIMAL; EXCLUSIVE; UNDEFINED; vkd3d's format for
 * B8G8R8A8_UNORM; memory of the first DEVICE_LOCAL type, vkd3d's rule for a DEFAULT heap) and wrapped with vkd3d_create_image_resource
 * and today's exact D3D12_RESOURCE_DESC.  The presenter reads them directly; Plan B (Metal) would export them.
 *   KHNMC_GFX_BACKBUFFERS=own        (default) INITIAL|PRESENT_STATE_TRANSITION, present_state COPY_SOURCE: D3D12 PRESENT is Vulkan
 *                                    TRANSFER_SRC_OPTIMAL with TRANSFER_READ at the TRANSFER stage (vkd3d command.c 2275-2279, 2373-2444)
 *                                    general    own images WITHOUT PRESENT_STATE_TRANSITION: PRESENT is GENERAL at the HOST stage
 *                                    committed  the old path: vkd3d's committed resources (the presenter cannot read those: named fatal)
 * vkd3d never destroys a wrapped VkImage (resource.c 988-989): at ResizeBuffers the old images are destroyed here, after the reference
 * check, the gate, a fence signalled on the queue and waited for (in slices), and the presenter's own submissions. */
typedef struct Swap { u64 obj; ID3D12CommandQueue *queue; ID3D12Device *dev; ID3D12Resource *buf[NBUF]; u32 w,h;
    ID3D12CommandAllocator *alloc; ID3D12GraphicsCommandList *list; ID3D12Fence *fence; u64 fence_val;
    ID3D12Resource *rb; u64 rb_size; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; u8 *seen;
    int bbmode; const KhVk *vk; VkImage img[NBUF]; VkDeviceMemory mem[NBUF]; KhPresenter *pres; } Swap;
static Swap *swap_of(KCtx *g,u64 o,const char *what){ Swap *s=(Swap*)(uintptr_t)rd64(g,o+SW_PTR); if(!s||s->obj!=o) kh_fatal(g,"gfx-swapchain-state: %s on swap chain 0x%llx that has no vkd3d back buffers",what,P(o)); return s; }
#define HRCHK(expr,what) do{ HRESULT hr_=(expr); if(FAILED(hr_)) kh_fatal(g,"gfx-vkd3d-call: %s failed, hr=0x%08x (swap chain %ux%u)",what,(unsigned)hr_,s->w,s->h); }while(0)
static void bb_image_make(KCtx *g,Swap *s,int i,const D3D12_RESOURCE_DESC *rd){
    const KhVk *v=s->vk; VkImageCreateInfo ic; memset(&ic,0,sizeof ic); ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ic.flags=0; ic.imageType=VK_IMAGE_TYPE_2D;
    ic.format=vkd3d_get_vk_format(DXGI_FORMAT_B8G8R8A8_UNORM); ic.extent.width=(u32)rd->Width; ic.extent.height=rd->Height; ic.extent.depth=1; ic.mipLevels=1; ic.arrayLayers=1;
    ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL; ic.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(ic.format==VK_FORMAT_UNDEFINED) kh_fatal(g,"gfx-backbuffer-image: vkd3d has no Vulkan format for B8G8R8A8_UNORM");
#if defined(VK_USE_PLATFORM_METAL_EXT)
    /* Apple, only when VK_EXT_metal_objects was enabled: each image can later be exported as an MTLTexture (Plan B needs no second change) */
    VkExportMetalObjectCreateInfoEXT ex; memset(&ex,0,sizeof ex); ex.sType=VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT; ex.exportObjectType=VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT;
    if(kh_gfxc_pres_ext.metal_objects) ic.pNext=&ex;
#endif
    VkResult r=kh_vk_create_image(v,&ic,&s->img[i]); if(r!=VK_SUCCESS) kh_fatal(g,"gfx-backbuffer-image: vkCreateImage(%ux%u B8G8R8A8) failed: VkResult %d",ic.extent.width,ic.extent.height,(int)r);
    kh_vko_note(VKO_IMAGE,1);
    VkMemoryRequirements mr; kh_vk_image_memreq(v,s->img[i],&mr); int t=kh_vk_memtype(v,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(t<0) kh_fatal(g,"gfx-backbuffer-image: no DEVICE_LOCAL memory type for a back buffer (type bits 0x%x)",mr.memoryTypeBits);
    VkMemoryAllocateInfo ma; memset(&ma,0,sizeof ma); ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize=mr.size; ma.memoryTypeIndex=(u32)t;
    r=kh_vk_alloc(v,&ma,&s->mem[i]); if(r!=VK_SUCCESS) kh_fatal(g,"gfx-backbuffer-image: vkAllocateMemory(%llu bytes, type %d) failed: VkResult %d",P(mr.size),t,(int)r);
    kh_vko_note(VKO_MEM,1);
    r=kh_vk_bind_image(v,s->img[i],s->mem[i]); if(r!=VK_SUCCESS) kh_fatal(g,"gfx-backbuffer-image: vkBindImageMemory failed: VkResult %d",(int)r);
    struct vkd3d_image_resource_create_info ri; memset(&ri,0,sizeof ri); ri.type=VKD3D_STRUCTURE_TYPE_IMAGE_RESOURCE_CREATE_INFO; ri.vk_image=s->img[i]; ri.desc=*rd;
    ri.flags=VKD3D_RESOURCE_INITIAL_STATE_TRANSITION|(s->bbmode==BB_OWN?VKD3D_RESOURCE_PRESENT_STATE_TRANSITION:0); ri.present_state=D3D12_RESOURCE_STATE_COPY_SOURCE;
    HRCHK(vkd3d_create_image_resource(s->dev,&ri,&s->buf[i]),"vkd3d_create_image_resource(own B8G8R8A8 back buffer)");
    atomic_fetch_add(&n_bb_images_made,1);
}
/* the images of the buffers just released: called once nothing can use them any more (bb_drain) */
static void bb_images_free(Swap *s){
    for(int i=0;i<NBUF;i++){ if(s->img[i]){ kh_vk_destroy_image(s->vk,s->img[i]); kh_vko_note(VKO_IMAGE,0); s->img[i]=VK_NULL_HANDLE; atomic_fetch_add(&n_bb_images_freed,1); }
        if(s->mem[i]){ kh_vk_free(s->vk,s->mem[i]); kh_vko_note(VKO_MEM,0); s->mem[i]=VK_NULL_HANDLE; } }
}
/* before own images are destroyed: the drain's Signal is an admission at the gate (task 39: this is a submission outside the game's
 * queue calls; it waits while the gate is set), then the fence is waited for in COUNTED slices (5 = gfx-backbuffer-drain; a stopped
 * process costs one slice; a slice that overlaps a set gate is not counted, review c1/c2), then the presenter's submissions */
static _Atomic u64 n_bb_drain_free;                                  /* drain-wait slices not counted because the gate was set */
static void bb_drain(KCtx *g,Swap *s,const char *why){
    kh_gfx_gate_enter("back-buffer drain (ResizeBuffers / release)");
    u64 want=++s->fence_val; kh_gfx_gate_assert_admitted("back-buffer drain: ID3D12CommandQueue::Signal"); HRCHK(ID3D12CommandQueue_Signal(s->queue,s->fence,want),"ID3D12CommandQueue::Signal(back-buffer drain)");
    kh_gfx_gate_leave();
    KhGateSlices sl; kh_gfx_gate_slices_begin(&sl);
    for(;;){ UINT64 done=ID3D12Fence_GetCompletedValue(s->fence);
        if(done==~0ull) kh_fatal(g,"gfx-device-removed: the fence reports a removed device during the back-buffer drain (%s)",why);
        if(done>=want) break;
        struct timespec ts={0,200000}; nanosleep(&ts,0);
        if(kh_gfx_gate_slices_expired(&sl,5)) kh_fatal(g,"gfx-backbuffer-drain: the queue did not reach the drain fence within 5 slices of %u ms with the gate clear (%u more under the gate; %s; fence at %llu, want %llu)",kh_stuck_slice_ms,sl.free_,why,P(done),P(want)); }
    if(sl.free_) atomic_fetch_add(&n_bb_drain_free,sl.free_);
    if(s->pres) kh_present_quiesce(s->pres);
    atomic_fetch_add(&n_bb_drains,1);
}
static void bb_info(const Swap *s,KhPresentBuffers *bb){ memset(bb,0,sizeof *bb); bb->w=s->w; bb->h=s->h; bb->n=NBUF; bb->general=s->bbmode==BB_GENERAL; for(int i=0;i<NBUF;i++) bb->image[i]=(u64)(uintptr_t)s->img[i]; }

static void buffers_make(KCtx *g,Swap *s,u32 w,u32 h){
    s->w=w; s->h=h;
    D3D12_HEAP_PROPERTIES def; memset(&def,0,sizeof def); def.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd; memset(&rd,0,sizeof rd); rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=w; rd.Height=h; rd.DepthOrArraySize=1; rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(s->bbmode==BB_COMMITTED){ for(int i=0;i<NBUF;i++){ HRCHK(ID3D12Device_CreateCommittedResource(s->dev,&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_PRESENT,NULL,&IID_ID3D12Resource,(void**)&s->buf[i]),"CreateCommittedResource(B8G8R8A8 back buffer, ALLOW_RENDER_TARGET, state PRESENT)"); } }
    else for(int i=0;i<NBUF;i++) bb_image_make(g,s,i,&rd);
    UINT64 total=0; ID3D12Device_GetCopyableFootprints(s->dev,&rd,0,1,0,&s->fp,NULL,NULL,&total);
    if(!total||s->fp.Footprint.RowPitch<4*w) kh_fatal(g,"gfx-vkd3d-call: GetCopyableFootprints for the back buffer gave %llu bytes, row pitch %u",P(total),s->fp.Footprint.RowPitch);
    D3D12_HEAP_PROPERTIES rbh; memset(&rbh,0,sizeof rbh); rbh.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd; memset(&bd,0,sizeof bd); bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=total; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRCHK(ID3D12Device_CreateCommittedResource(s->dev,&rbh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&s->rb),"CreateCommittedResource(readback buffer)");
    s->rb_size=total;
}
/* CreateSwapChainForHwnd(factory, queue, hwnd, desc1, ...) : called by null_d3d12.c after it built the guest object */
void kh_vkd3d_swap_attach(KCtx *g,u64 sc,u64 queue,u64 desc){
    if(!queue) kh_fatal(g,"gfx-swapchain-queue: CreateSwapChainForHwnd without a command queue");
    void *rq=real_of(g,queue,WK_QUEUE,"CreateSwapChainForHwnd: the command queue");
#if KH_GFX_TRAMP
    if(!kh_gfx_wrap_on) need_native(g,rq,"the swap chain's ID3D12CommandQueue");
#endif
    u32 fmt=desc?rd32(g,desc+8):0x57, count=desc?rd32(g,desc+28):NBUF, samples=desc?rd32(g,desc+16):1;
    if(fmt!=0x57||count!=NBUF||samples>1) kh_fatal(g,"gfx-swapchain-desc: only 3 x B8G8R8A8_UNORM (0x57) single-sample buffers are implemented; the game asked for %u x format 0x%x, %u samples",count,fmt,samples);
    Swap *s=calloc(1,sizeof *s); if(!s) kh_fatal(g,"gfx-swapchain-state: out of memory"); s->obj=sc; s->w=rd32(g,sc+32); s->h=rd32(g,sc+36);
    s->queue=(ID3D12CommandQueue*)rq; ID3D12CommandQueue_AddRef(s->queue);
    HRCHK(ID3D12CommandQueue_GetDevice(s->queue,&IID_ID3D12Device,(void**)&s->dev),"ID3D12CommandQueue::GetDevice");
    s->bbmode=bb_mode_test>=0?bb_mode_test:bb_mode_env;
    int pmode=kh_present_mode();
    if(pmode&&s->bbmode==BB_COMMITTED) kh_fatal(g,"gfx-present-needs-own-backbuffers: KHNMC_GFX_PRESENT=%s reads the back buffers as VkImages, which KHNMC_GFX_BACKBUFFERS=committed does not have (use own or general)",pmode==1?"headless":"layer");
    if(s->bbmode!=BB_COMMITTED) s->vk=kh_vk_for(s->dev);
    buffers_make(g,s,s->w,s->h);
    HRCHK(ID3D12Device_CreateCommandAllocator(s->dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&s->alloc),"CreateCommandAllocator(frame dump)");
    HRCHK(ID3D12Device_CreateCommandList(s->dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,s->alloc,NULL,&IID_ID3D12GraphicsCommandList,(void**)&s->list),"CreateCommandList(frame dump)");
    HRCHK(ID3D12GraphicsCommandList_Close(s->list),"ID3D12GraphicsCommandList::Close(frame dump)");
    HRCHK(ID3D12Device_CreateFence(s->dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&s->fence),"CreateFence(frame dump)");
    if(pmode){ KhPresentBuffers bb; bb_info(s,&bb); s->pres=kh_present_attach(s->dev,s->queue,&bb); }
    pthread_mutex_lock(&cen_m); cen_dev=s->dev; pthread_mutex_unlock(&cen_m);  /* TASK38: the census reads MoltenVK's statistics of this device */
    wr64(g,sc+SW_PTR,U(s));
    fprintf(stderr,"kh: gfx vkd3d: swap chain 0x%llx: %d x %ux%u B8G8R8A8 back buffers (%s%s) on device %p (queue %p), readback %llu bytes (row pitch %u)%s\n",P(sc),NBUF,s->w,s->h,bb_names[s->bbmode],
            s->bbmode==BB_OWN?": our VkImages, PRESENT = TRANSFER_SRC":s->bbmode==BB_GENERAL?": our VkImages, PRESENT = GENERAL":": vkd3d's committed resources",(void*)s->dev,(void*)s->queue,P(s->rb_size),s->fp.Footprint.RowPitch,s->pres?"; presenter attached":"");
}
/* GetBuffer(this, index, riid, **pp) */
void kh_vkd3d_swap_getbuffer(KCtx *g,u64 o){
    Swap *s=swap_of(g,o,"GetBuffer"); u32 i=(u32)A1(g);
    if(i>=NBUF||!A3(g)){ if(A3(g)) wr64(g,A3(g),0); SET_RAX(g,DXGI_INVALID_CALL); return; }
    void *out=NULL; HRESULT hr=ID3D12Resource_QueryInterface(s->buf[i],(const IID*)hp(g,A2(g)),&out);      /* AddRef through QueryInterface */
    if(SUCCEEDED(hr)&&kh_gfx_wrap_on&&out!=(void*)s->buf[i]) kh_fatal(g,"gfx-wrap-qi-identity: back buffer %u (%p) answers the requested interface with another pointer (%p)",i,(void*)s->buf[i],out);
    wr64(g,A3(g),SUCCEEDED(hr)&&out?shown_of(g,out,WK_RESOURCE):0); SET_RAX(g,(u32)hr);                    /* the same buffer is always the same pointer / the same guest object */
}
/* ResizeBuffers: every reference the game took through GetBuffer must be gone (DXGI_ERROR_INVALID_CALL otherwise, like DXGI) */
void kh_vkd3d_swap_resize(KCtx *g,u64 o,u32 w,u32 h){
    Swap *s=swap_of(g,o,"ResizeBuffers");
    for(int i=0;i<NBUF;i++){ ID3D12Resource_AddRef(s->buf[i]); ULONG rc=ID3D12Resource_Release(s->buf[i]);
        if(rc!=1){ fprintf(stderr,"kh: gfx vkd3d: ResizeBuffers(%ux%u) refused: back buffer %d still has %u outstanding reference(s) -> DXGI_ERROR_INVALID_CALL\n",w,h,i,(unsigned)rc-1); SET_RAX(g,DXGI_INVALID_CALL); return; } }
    for(int i=0;i<NBUF;i++){ drop_real(g,s->buf[i]); s->buf[i]=NULL; }                 /* our reference was the last one: a guest object of the buffer dies with it */
    ID3D12Resource_Release(s->rb); s->rb=NULL;
    if(s->bbmode!=BB_COMMITTED){ bb_drain(g,s,"ResizeBuffers"); bb_images_free(s); }   /* own images: nothing in flight uses them any more */
    buffers_make(g,s,w,h);
    if(s->pres){ KhPresentBuffers bb; bb_info(s,&bb); kh_present_resize(s->pres,&bb); }
    fprintf(stderr,"kh: gfx vkd3d: swap chain 0x%llx resized to %ux%u\n",P(o),w,h);
    SET_RAX(g,0);
}
/* ---- selftest access to a swap chain (gfx_present.c's presenter selftest) ---- */
int kh_vkd3d_swap_test_info(KCtx *g,u64 sc,KhSwapInfo *out){
    Swap *s=(Swap*)(uintptr_t)rd64(g,sc+SW_PTR); memset(out,0,sizeof *out); if(!s||s->obj!=sc) return 0;
    out->dev=s->dev; out->queue=s->queue; for(int i=0;i<NBUF;i++){ out->buf[i]=s->buf[i]; out->image[i]=(u64)(uintptr_t)s->img[i]; } out->bbmode=s->bbmode; out->w=s->w; out->h=s->h; out->pres=s->pres; return 1;
}
void kh_vkd3d_test_bbmode(int mode){ bb_mode_test=mode; }
/* the end of a selftest swap chain: presenter teardown, every reference of ours released, own images destroyed after the drain.  The
 * game's swap chain is never released this way (at process exit another thread may still be inside Present: the same reason the hash
 * log is only flushed, kh_gfx_vkd3d_finish). */
void kh_vkd3d_swap_test_release(KCtx *g,u64 sc){
    Swap *s=swap_of(g,sc,"swap chain release (selftest)");
    for(int i=0;i<NBUF;i++){ ID3D12Resource_AddRef(s->buf[i]); ULONG rc=ID3D12Resource_Release(s->buf[i]);
        if(rc!=1) kh_fatal(g,"gfx-selftest-swapchain: release of swap chain 0x%llx while back buffer %d still has %u outstanding reference(s)",P(sc),i,(unsigned)rc-1); }
    if(s->pres){ kh_present_teardown(s->pres); s->pres=NULL; }
    for(int i=0;i<NBUF;i++){ drop_real(g,s->buf[i]); s->buf[i]=NULL; }
    if(s->bbmode!=BB_COMMITTED){ bb_drain(g,s,"swap chain release"); bb_images_free(s); }
    ID3D12Resource_Release(s->rb); ID3D12GraphicsCommandList_Release(s->list); ID3D12CommandAllocator_Release(s->alloc); ID3D12Fence_Release(s->fence);
    pthread_mutex_lock(&cen_m); if(cen_dev==s->dev) cen_dev=NULL; pthread_mutex_unlock(&cen_m);   /* TASK38: no census read of a device that may be gone */
    ID3D12Device_Release(s->dev); ID3D12CommandQueue_Release(s->queue); free(s->seen); wr64(g,sc+SW_PTR,0); free(s);
}

/* ---- frame output --------------------------------------------------------------------------------------------------------------
 * Two consumers of the picture that was just presented, one readback for both:
 *   image dumps   KHNMC_GFX_DUMP=<dir>: frame-NNNNNN.ppm for every KHNMC_GFX_DUMP_EVERY-th present inside the present window
 *                 KHNMC_GFX_DUMP_FROM .. KHNMC_GFX_DUMP_TO (inclusive, default: all), full size, with a line in index.tsv;
 *   video stream  KHNMC_GFX_VIDEO=<file or FIFO>: raw RGB24 pictures, ONE PER VIRTUAL VBLANK (needs KHNMC_CLOCK=frames, vclock.c), so
 *                 the stream has a fixed cadence of 60 pictures per second of GAME time however slowly the host renders, and picture
 *                 k is the same instant as sample 800*k of the audio device clock.  A vblank without a new Present repeats the
 *                 previous picture (what a display shows); two Presents inside one vblank keep the later one.
 *                 KHNMC_GFX_VIDEO_FROM=<virtual vblank> | field[+<n>]  first picture: that vblank, or n vblanks after the first
 *                                                       FIELD_COMMAND.execute (the field is up); default: the first vblank
 *                 KHNMC_GFX_VIDEO_FRAMES=<n>            pictures to write (required)
 *                 KHNMC_GFX_VIDEO_SCALE=1|2             2 = each output pixel is the mean of a 2x2 block (1920x1080 -> 960x540)
 *                 KHNMC_GFX_VIDEO_EXIT=1                end the run (exit 0, with all reports) one vblank after the last picture
 *                                                       (the last picture is on screen for 1/60 s: its sound has to exist too)
 *                 <file>.tsv gets one line per picture: vblank, virtual time, present, game frame, new/repeat, audio sample.
 *                 A FIFO is opened when the first picture is due, so a reader (ffmpeg -f rawvideo) may start any time before. */
static FILE *video_f, *video_ix;
static u64 video_first;                                              /* vblank of picture 0 once known (0 = not yet) */
static pthread_mutex_t vid_m=PTHREAD_MUTEX_INITIALIZER;
static u8 *vid_buf; static u32 vid_w, vid_h; static int vid_have, vid_fresh, vid_armed, vid_done, vid_cut; static u64 vid_present, vid_gframe;
static _Atomic u64 vid_written, vid_repeats, vid_replaced, vid_captures, vid_before_first; static u64 vid_exit_at;
u64 kh_phase_field_cmds(void); u64 kh_input_game_frames(void); u64 kh_audio_vclock_t0(void); void kh_exit_process(int);

static void barrier(ID3D12GraphicsCommandList *l,ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER x; memset(&x,0,sizeof x); x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; ID3D12GraphicsCommandList_ResourceBarrier(l,1,&x);
}
/* copy back buffer idx to the readback buffer and map it; the caller unmaps with readback_done.  Its ExecuteCommandLists + Signal on the
 * game's queue are an admission at the suspend gate (task 39): `where` names the place (waits while the gate is set) */
static const u8 *readback(KCtx *g,Swap *s,u32 idx,u64 n,const char *where){
    HRCHK(ID3D12CommandAllocator_Reset(s->alloc),"ID3D12CommandAllocator::Reset(frame readback)");
    HRCHK(ID3D12GraphicsCommandList_Reset(s->list,s->alloc,NULL),"ID3D12GraphicsCommandList::Reset(frame readback)");
    barrier(s->list,s->buf[idx],D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src,dst; memset(&src,0,sizeof src); memset(&dst,0,sizeof dst);
    src.pResource=s->buf[idx]; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=0;
    dst.pResource=s->rb; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=s->fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(s->list,&dst,0,0,0,&src,NULL);
    barrier(s->list,s->buf[idx],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    HRCHK(ID3D12GraphicsCommandList_Close(s->list),"ID3D12GraphicsCommandList::Close(frame readback)");
    ID3D12CommandList *lists[1]={(ID3D12CommandList*)s->list};
    kh_gfx_gate_enter(where);
    kh_gfx_gate_assert_admitted("frame readback: ID3D12CommandQueue::ExecuteCommandLists"); ID3D12CommandQueue_ExecuteCommandLists(s->queue,1,lists);
    u64 want=++s->fence_val;
    kh_gfx_gate_assert_admitted("frame readback: ID3D12CommandQueue::Signal"); HRCHK(ID3D12CommandQueue_Signal(s->queue,s->fence,want),"ID3D12CommandQueue::Signal(frame readback)");
    kh_gfx_gate_leave();
    for(u64 waited=0;;waited++){                                               /* the copy runs after everything the game queued for this frame */
        UINT64 done=ID3D12Fence_GetCompletedValue(s->fence);
        if(done==~0ull) kh_fatal(g,"gfx-device-removed: the fence reports a removed device while reading back present %llu (GetDeviceRemovedReason hr=0x%08x)",P(n),(unsigned)ID3D12Device_GetDeviceRemovedReason(s->dev));
        if(done>=want) break;
        if(waited>=600000) kh_fatal(g,"gfx-dump-timeout: the readback of present %llu did not complete within 300 s (fence at %llu, want %llu)",P(n),P(done),P(want));
        struct timespec ts={0,500000}; nanosleep(&ts,0);
    }
    u8 *p=NULL; D3D12_RANGE rg={0,(SIZE_T)s->rb_size};
    HRCHK(ID3D12Resource_Map(s->rb,0,&rg,(void**)&p),"ID3D12Resource::Map(readback buffer)");
    if(!p) kh_fatal(g,"gfx-vkd3d-call: Map(readback buffer) returned a null pointer");
    return p+s->fp.Offset;
}
static void readback_done(Swap *s){ D3D12_RANGE none={0,0}; ID3D12Resource_Unmap(s->rb,0,&none); }
static void dump_frame(KCtx *g,Swap *s,const u8 *base,u32 idx,u64 n,u64 t0){
    u32 w=s->w,h=s->h,pitch=s->fp.Footprint.RowPitch;
    char path[1200]; snprintf(path,sizeof path,"%s/frame-%06llu.ppm",dump_dir,P(n));
    FILE *f=fopen(path,"wb"); if(!f) kh_fatal(g,"gfx-dump-write: cannot create %s (%s)",path,strerror(errno));
    u8 *row=malloc(3ull*w); if(!s->seen) s->seen=malloc(1u<<21); if(!row||!s->seen) kh_fatal(g,"gfx-dump-write: out of memory");
    memset(s->seen,0,1u<<21);
    fprintf(f,"P6\n%u %u\n255\n",w,h);
    u64 nonblack=0,sr=0,sg=0,sb=0,sa=0,distinct=0;
    for(u32 y=0;y<h;y++){ const u8 *q=base+(u64)y*pitch;
        for(u32 x=0;x<w;x++){ u8 b=q[4*x],gc=q[4*x+1],r=q[4*x+2],a=q[4*x+3]; row[3*x]=r; row[3*x+1]=gc; row[3*x+2]=b;
            sr+=r; sg+=gc; sb+=b; sa+=a; if(r|gc|b) nonblack++; u32 k=((u32)r<<16)|((u32)gc<<8)|b; if(!(s->seen[k>>3]&(1u<<(k&7)))){ s->seen[k>>3]|=(u8)(1u<<(k&7)); distinct++; } }
        if(fwrite(row,3,w,f)!=w) kh_fatal(g,"gfx-dump-write: short write to %s (%s)",path,strerror(errno)); }
    free(row);
    if(fclose(f)) kh_fatal(g,"gfx-dump-write: closing %s failed (%s)",path,strerror(errno));
    double px=(double)w*h;
    char idx_path[1200]; snprintf(idx_path,sizeof idx_path,"%s/index.tsv",dump_dir);
    FILE *ix=fopen(idx_path,"a"); if(!ix) kh_fatal(g,"gfx-dump-write: cannot append %s (%s)",idx_path,strerror(errno));
    if(ftell(ix)==0) fprintf(ix,"present\tfile\twidth\theight\tnonblack_pct\tmean_r\tmean_g\tmean_b\tmean_a\tdistinct_rgb\tdraws_total\tgame_frame\tvblank\n");
    fprintf(ix,"%llu\tframe-%06llu.ppm\t%u\t%u\t%.3f\t%.2f\t%.2f\t%.2f\t%.2f\t%llu\t%llu\t%llu\t%llu\n",P(n),P(n),w,h,100.0*nonblack/px,sr/px,sg/px,sb/px,sa/px,P(distinct),P(gfx_draws),P(kh_input_game_frames()),P(kh_vclock_on?kh_vclock_vblanks():0));
    if(fclose(ix)) kh_fatal(g,"gfx-dump-write: closing %s failed (%s)",idx_path,strerror(errno));
    atomic_fetch_add(&dumps_written,1);
    fprintf(stderr,"kh: gfx dump present=%llu back-buffer=%u file=%s %ux%u non-black=%.2f%% mean-rgb=(%.1f,%.1f,%.1f) mean-a=%.1f distinct-rgb=%llu readback-ms=%.1f\n",P(n),idx,path,w,h,100.0*nonblack/px,sr/px,sg/px,sb/px,sa/px,P(distinct),(kh_host_ns()-t0)/1e6);
}
/* two 64-bit hashes of a w x h BGRA picture whose rows are `pitch` bytes apart: over the colour bytes only (what the PPM dump holds), and over
 * all four channels.  The bytes between 4*w and the pitch are not part of the picture and are not hashed. */
static void hash_pixels(const u8 *base,u32 pitch,u32 w,u32 h,u64 *rgb,u64 *bgra){
    u64 a=0x9e3779b97f4a7c15ull, b=0xc2b2ae3d27d4eb4full; const u64 K=0xff51afd7ed558ccdull;
    for(u32 y=0;y<h;y++){ const u8 *q=base+(u64)y*pitch; u32 x=0;
        for(;x+2<=w;x+=2){ u64 v; memcpy(&v,q+4ull*x,8); b=(b^v)*K; b^=b>>32; v&=0x00ffffff00ffffffull; a=(a^v)*K; a^=a>>32; }
        if(x<w){ u32 v4; memcpy(&v4,q+4ull*x,4); u64 v=v4; b=(b^v)*K; b^=b>>32; v&=0x00ffffffull; a=(a^v)*K; a^=a>>32; } }
    *rgb=a; *bgra=b;
}
static void hash_frame(KCtx *g,Swap *s,const u8 *base,u64 n){
    u64 a,b; hash_pixels(base,s->fp.Footprint.RowPitch,s->w,s->h,&a,&b);
    if(fprintf(hash_f,"%llu\t%llu\t%llu\t%016llx\t%016llx\t%llu\t%u\t%u\n",P(n),P(kh_input_game_frames()),P(kh_vclock_on?kh_vclock_vblanks():0),P(a),P(b),P(gfx_draws),s->w,s->h)<0)
        kh_fatal(g,"gfx-hashlog: writing %s failed at present %llu (%s)",hash_path,P(n),strerror(errno));
    atomic_fetch_add(&hashes_written,1);
}
/* task 40, KHNMC_GFX_BANDLOG=<file> (the fill mode's GPU checks, tools/fill_compare.py tallcheck / bandpairs): per present the picture
 * split into the centred 16:9 band (ch = round(9w / 16) rows from row o = (h - ch) / 2) and the strips above and below it: the RGB
 * hash of each part (hash_pixels), the share of pixels that are not pure black, the mean of the RGB bytes, the mean step across each
 * band edge (|row o - row o-1| and |row o+ch - row o+ch-1| over the RGB bytes) and the 95th percentile of the steps between
 * neighbouring band rows (fill_compare.py condition (c2)).  On a picture of 16:9 or wider the band is the whole picture and the strip
 * columns read "-".
 * Task 40 review (finding 3): the exe's full-screen quads overshoot the band (the fades are GS y -1..417, so at 1920x1350 they cover
 * rows 132..1217, not 135..1214), so a fade over the band only makes its hard line 3 rows OUTSIDE the band edge, where edge_top /
 * edge_bottom (one row pair each) never look.  Three more columns: edge_window_rows W = ceil(2 ch / 416) (two GS rows at the band's
 * scale: 6 at 1920x1350) and the largest mean row step over the W row pairs at and beyond each band edge: edge_top_max over the pairs
 * (y | y+1) for y = o-W .. o-1, edge_bottom_max for y = o+ch-1 .. o+ch-2+W.  Steps inside the band (HUD edges near the band edge,
 * e.g. the command menu's at row 1210) are not in these windows: a band-only overlay always ends at or beyond the band edge. */
static void part_stats(const u8 *base,u32 pitch,u32 w,u32 y0,u32 n,double *nonblack,double *mean){
    u64 nb=0,sum=0; for(u32 y=y0;y<y0+n;y++){ const u8 *q=base+(u64)y*pitch; for(u32 x=0;x<w;x++){ u8 b=q[4*x],gc=q[4*x+1],r=q[4*x+2]; if(r|gc|b) nb++; sum+=(u64)r+gc+b; } }
    double px=(double)w*n; *nonblack=px?100.0*nb/px:0; *mean=px?sum/(3.0*px):0; }
static double row_step(const u8 *base,u32 pitch,u32 w,u32 y){            /* mean |row y+1 - row y| over the RGB bytes */
    const u8 *a=base+(u64)y*pitch,*b=a+pitch; u64 s=0; for(u32 x=0;x<w;x++) for(int c=0;c<3;c++){ int d=(int)b[4*x+c]-(int)a[4*x+c]; s+=(u64)(d<0?-d:d); } return s/(3.0*w); }
static int cmp_dbl(const void *a,const void *b){ double x=*(const double*)a,y=*(const double*)b; return x<y?-1:x>y; }
typedef struct { u32 o,ch,win; u64 hb,ht,hbt; double bn,bm,tn,tm,on,om,et,eb,etw,ebw,p95; } BandSt;
/* the band log's numbers for one w x h BGRA picture (pure; selftested in kh_gfxc_selftest_pure); 0 = out of host memory */
static int band_stats(const u8 *base,u32 pitch,u32 w,u32 h,BandSt *b){
    memset(b,0,sizeof *b); u32 ch=(u32)((9ull*w+8)/16); if(ch>h) ch=h; u32 o=(h-ch)/2; b->o=o; b->ch=ch;
    u64 hx; hash_pixels(base+(u64)o*pitch,pitch,w,ch,&b->hb,&hx); part_stats(base,pitch,w,o,ch,&b->bn,&b->bm);
    double *st=malloc(sizeof(double)*(ch?ch:1)); if(!st) return 0;
    u32 ns=0; for(u32 y=o;y+1<o+ch;y++) st[ns++]=row_step(base,pitch,w,y); qsort(st,ns,sizeof *st,cmp_dbl);
    b->p95=ns?st[(u32)(0.95*(ns-1)+0.5)]:0; free(st);
    if(o>0){ hash_pixels(base,pitch,w,o,&b->ht,&hx); hash_pixels(base+(u64)(o+ch)*pitch,pitch,w,h-o-ch,&b->hbt,&hx);
        part_stats(base,pitch,w,0,o,&b->tn,&b->tm); part_stats(base,pitch,w,o+ch,h-o-ch,&b->on,&b->om);
        b->et=row_step(base,pitch,w,o-1); b->eb=row_step(base,pitch,w,o+ch-1);
        b->win=(2*ch+415)/416; if(!b->win) b->win=1;
        for(u32 y=o>b->win?o-b->win:0;y<o;y++){ double v=row_step(base,pitch,w,y); if(v>b->etw) b->etw=v; }
        for(u32 y=o+ch-1;y<o+ch-1+b->win&&y+1<h;y++){ double v=row_step(base,pitch,w,y); if(v>b->ebw) b->ebw=v; } }
    return 1;
}
static void band_frame(KCtx *g,Swap *s,const u8 *base,u64 n){
    u32 w=s->w,h=s->h,pitch=s->fp.Footprint.RowPitch; BandSt b;
    if(!band_stats(base,pitch,w,h,&b)) kh_fatal(g,"gfx-bandlog: out of memory");
    char tail[480];
    if(b.o>0) snprintf(tail,sizeof tail,"%016llx\t%016llx\t%.4f\t%.4f\t%.4f\t%.3f\t%.3f\t%.3f\t%.4f\t%.4f\t%.4f\t%u\t%.4f\t%.4f",P(b.ht),P(b.hbt),b.bn,b.tn,b.on,b.bm,b.tm,b.om,b.et,b.eb,b.p95,b.win,b.etw,b.ebw);
    else snprintf(tail,sizeof tail,"-\t-\t%.4f\t-\t-\t%.3f\t-\t-\t-\t-\t%.4f\t-\t-\t-",b.bn,b.bm,b.p95);
    u32 o=b.o, ch=b.ch; u64 hb=b.hb;
    if(fprintf(band_f,"%llu\t%llu\t%u\t%u\t%u\t%u\t%016llx\t%s\n",P(n),P(kh_input_game_frames()),w,h,o,ch,P(hb),tail)<0)
        kh_fatal(g,"gfx-bandlog: writing %s failed at present %llu (%s)",band_path,P(n),strerror(errno));
    atomic_fetch_add(&bands_written,1);
}
/* BGRA rows -> RGB24, optionally the mean of each 2x2 block (rounded to nearest) */
static void video_convert(u8 *out,const u8 *base,u32 pitch,u32 w,u32 h,int scale){
    if(scale==1){ for(u32 y=0;y<h;y++){ const u8 *q=base+(u64)y*pitch; u8 *o=out+3ull*w*y; for(u32 x=0;x<w;x++){ o[3*x]=q[4*x+2]; o[3*x+1]=q[4*x+1]; o[3*x+2]=q[4*x]; } } return; }
    u32 ow=w/2,oh=h/2;
    for(u32 y=0;y<oh;y++){ const u8 *q0=base+(u64)(2*y)*pitch,*q1=q0+pitch; u8 *o=out+3ull*ow*y;
        for(u32 x=0;x<ow;x++){ const u8 *a=q0+8*x,*b=q1+8*x;
            o[3*x]=(u8)((a[2]+a[6]+b[2]+b[6]+2)>>2); o[3*x+1]=(u8)((a[1]+a[5]+b[1]+b[5]+2)>>2); o[3*x+2]=(u8)((a[0]+a[4]+b[0]+b[4]+2)>>2); } }
}
static int video_wanted(void){                       /* at a Present: is this picture needed for the stream? arms the stream when its start is known */
    if(!video_path[0]||vid_done) return 0;
    if(!vid_armed){
        u64 vb=kh_vclock_vblanks();
        if(video_from_field){ if(!kh_phase_field_cmds()) return 0; video_first=vb+1+video_lead; }
        else { if(vb+2<video_from) return 0; video_first=video_from>vb+1?video_from:vb+1; }
        vid_armed=1;
        fprintf(stderr,"kh: gfx video: armed at present %llu, virtual vblank %llu: picture 0 is virtual vblank %llu%s\n",P(gfx_presents),P(vb),P(video_first),video_from_field?" (the field is up)":"");
    }
    return 1;
}
static void video_capture(KCtx *g,Swap *s,const u8 *base,u64 n){
    u32 ow=s->w/(u32)video_scale, oh=s->h/(u32)video_scale;
    if(video_scale==2&&((s->w|s->h)&1)) kh_fatal(g,"gfx-video-scale: KHNMC_GFX_VIDEO_SCALE=2 needs even back-buffer dimensions, got %ux%u",s->w,s->h);
    pthread_mutex_lock(&vid_m);
    if(vid_buf&&(vid_w!=ow||vid_h!=oh)){ pthread_mutex_unlock(&vid_m); kh_fatal(g,"gfx-video-size: the back buffer changed from a %ux%u picture to %ux%u while the video stream is open",vid_w,vid_h,ow,oh); }
    if(!vid_buf){ vid_buf=malloc(3ull*ow*oh); vid_w=ow; vid_h=oh; if(!vid_buf){ pthread_mutex_unlock(&vid_m); kh_fatal(g,"gfx-video: out of memory"); } }
    video_convert(vid_buf,base,s->fp.Footprint.RowPitch,s->w,s->h,video_scale);
    if(vid_fresh) atomic_fetch_add(&vid_replaced,1);
    vid_have=1; vid_fresh=1; vid_present=n; vid_gframe=kh_input_game_frames(); atomic_fetch_add(&vid_captures,1);
    pthread_mutex_unlock(&vid_m);
}
static void video_close(void){
    if(video_f){ if(fclose(video_f)) fprintf(stderr,"kh: gfx video: ERROR: closing %s failed (%s)\n",video_path,strerror(errno)); video_f=NULL; }
    if(video_ix){ if(fclose(video_ix)) fprintf(stderr,"kh: gfx video: ERROR: closing the index of %s failed (%s)\n",video_path,strerror(errno)); video_ix=NULL; }
}
/* vclock.c calls this after every virtual vblank, on the timing thread */
static void video_vblank(KCtx *g,u64 k,u64 vnow){
    if(!video_path[0]) return;
    if(vid_exit_at&&k>=vid_exit_at){ fprintf(stderr,"kh: gfx video: KHNMC_GFX_VIDEO_EXIT=1: virtual vblank %llu, one after the last picture: ending the run\n",P(k)); kh_exit_process(0); }
    pthread_mutex_lock(&vid_m);
    if(vid_done||!vid_armed||k<video_first){ pthread_mutex_unlock(&vid_m); return; }
    if(!vid_have){ atomic_fetch_add(&vid_before_first,1); video_first=k+1; pthread_mutex_unlock(&vid_m); return; }   /* nothing presented since arming: picture 0 moves on */
    if(!video_f){
        video_f=fopen(video_path,"wb"); if(!video_f){ pthread_mutex_unlock(&vid_m); kh_fatal(g,"gfx-video-write: cannot open %s for writing (%s)",video_path,strerror(errno)); }
        char ip[1100]; snprintf(ip,sizeof ip,"%s.tsv",video_path); video_ix=fopen(ip,"w"); if(!video_ix){ pthread_mutex_unlock(&vid_m); kh_fatal(g,"gfx-video-write: cannot create %s (%s)",ip,strerror(errno)); }
        fprintf(video_ix,"picture\tvblank\tvirtual_ns\tpresent\tgame_frame\tnew\taudio_sample\n");
        fprintf(stderr,"kh: gfx video: stream %s opened: rawvideo rgb24 %ux%u, 60 pictures per second of game time, picture 0 = virtual vblank %llu (virtual time %llu ns), %llu pictures to come\n",video_path,vid_w,vid_h,P(k),P(vnow),P(video_max));
    }
    if(fwrite(vid_buf,3ull*vid_w,vid_h,video_f)!=vid_h){ int e=errno; pthread_mutex_unlock(&vid_m); kh_fatal(g,"gfx-video-write: short write to %s at picture %llu (%s); a FIFO reader that went away ends the stream",video_path,P(vid_written),strerror(e)); }
    u64 t0=kh_audio_vclock_t0(); long long as=t0&&vnow>=t0?(long long)(((unsigned __int128)(vnow-t0)*48000u)/1000000000u):-1;
    fprintf(video_ix,"%llu\t%llu\t%llu\t%llu\t%llu\t%d\t%lld\n",P(vid_written),P(k),P(vnow),P(vid_present),P(vid_gframe),vid_fresh,as);
    if(!vid_fresh) atomic_fetch_add(&vid_repeats,1);
    vid_fresh=0; u64 wn=atomic_fetch_add(&vid_written,1)+1; int end=wn>=video_max;
    if(end){ vid_done=1; video_close(); fprintf(stderr,"kh: gfx video: complete: %llu pictures (%llu repeats of the previous picture, %llu presents replaced by a later one before their vblank), last = virtual vblank %llu\n",P(wn),P(vid_repeats),P(vid_replaced),P(k)); }
    pthread_mutex_unlock(&vid_m);
    if(end&&video_exit) vid_exit_at=k+1;
}
/* Present / Present1: called by null_d3d12.c (which keeps the present counter, the phase log and the frame line) BEFORE the index advances */
void kh_vkd3d_present(KCtx *g,u64 o,u64 n){
    Swap *s=swap_of(g,o,"Present");
    /* TASK36: the suspend gate FIRST: the readback below submits on the game's queue (frame dumps, hash log, video) */
    kh_gfx_gate_park("Present (before the frame readback)");
    memlow_check(n);                                                    /* review X38 n=2: the gfx-memory-low soft stop, before a dump allocates */
    u32 idx=rd32(g,o+40)%NBUF;
    int want_dump=dump_dir[0]&&n>=dump_from&&n<=dump_to&&n%dump_every==0, want_video=video_wanted(), want_hash=hash_f!=NULL, want_band=band_f!=NULL;
    if(want_dump||want_video||want_hash||want_band){
        u64 t0=kh_host_ns(); const u8 *base=readback(g,s,idx,n,"Present (frame readback)");
        if(want_hash) hash_frame(g,s,base,n);
        if(want_band) band_frame(g,s,base,n);
        if(want_video) video_capture(g,s,base,n);
        if(want_dump) dump_frame(g,s,base,idx,n,t0);
        readback_done(s); }
    /* the presenter: after the readback, before null_d3d12.c advances the index.  A check present (review V36 n=1) is then compared with an
     * INDEPENDENT reference: a D3D12 readback of the same back buffer, queued AFTER the presenter's submission (so it does not close the
     * window between the game's work and the blit), gated like every other readback */
    if(s->pres&&kh_present_frame(g,s->pres,idx,n)){                       /* the readback's submission waits at the gate (an admission) */
        const u8 *ref=readback(g,s,idx,n,"Present (check-mode reference readback)"); kh_present_check(s->pres,n,ref,s->fp.Footprint.RowPitch); readback_done(s); }
}
/* process exit (loader.c, before the reports): an open video stream is closed here and nowhere else; the report says if it was cut short */
void kh_gfx_vkd3d_finish(void){
    /* The hash log is NOT closed here: a Present on another thread may still be inside its readback and write its line afterwards
     * (review 2026-11-17: fclose and hash_f=NULL raced with hash_frame, a fault at the end of a finished run).  The stream is
     * line-buffered, so a flush is all the exit needs. */
    if(hash_f){ if(fflush(hash_f)||ferror(hash_f)) fprintf(stderr,"kh: gfx hash log: ERROR: writing %s failed (%s)\n",hash_path,strerror(errno)); }
    if(band_f){ if(fflush(band_f)||ferror(band_f)) fprintf(stderr,"kh: gfx band log: ERROR: writing %s failed (%s)\n",band_path,strerror(errno)); }
    if(!video_path[0]) return; pthread_mutex_lock(&vid_m); if(video_f){ vid_cut=!vid_done; video_close(); } else if(!vid_done) vid_cut=vid_armed; vid_done=1; pthread_mutex_unlock(&vid_m); }
/* the stream's state in the report; caller holds vid_m.  kh_gfx_vkd3d_finish marks every stream done, so "complete" is every picture
 * written and a stream the run cut short reads INCOMPLETE (review U35 n=27: vid_done was checked first) */
static const char *video_state(void){
    return vid_cut?" (INCOMPLETE: the run ended first)":vid_written>=video_max?" (complete)":video_f?" (open)":vid_armed?" (armed, no picture written yet)":" (not started: its first vblank has not been reached)";
}
void kh_gfx_vkd3d_report(FILE *f){
    fprintf(f,"kh: gfx vkd3d: bridge=%s device=%s driver-mode=%s devices-created=%llu (+%llu probes) native-calls=%llu fence-events-signalled=%llu bundles: recorded=%llu executed=%llu (%llu replayed calls) live=%llu; default-descriptor-tables=%llu on %d tracked command lists; frame-dumps=%llu%s%s\n",kh_gfx_wrap_on?"wrapped":"trampoline",vk_dev_name,drv_system?"system":"lavapipe",
            P(n_devices),P(n_probe_devices),P(kh_native_calls),P(n_events_signalled),P(n_bundles_made),P(n_bundles_run),P(n_bundle_cmds),P(n_bundles_live),P(n_default_tables),nlst,P(dumps_written),dump_dir[0]?" in ":"",dump_dir);
    if(hash_path[0]) fprintf(f,"kh: gfx hash log: %llu presents hashed into %s\n",P(hashes_written),hash_path);
    if(band_path[0]) fprintf(f,"kh: gfx band log: %llu presents measured into %s\n",P(bands_written),band_path);
    fprintf(f,"kh: gfx vkd3d: back buffers %s: own images made %llu, destroyed %llu, drains %llu (wait slices not counted under the gate %llu); live Vulkan objects of the runtime %lld; gipa wrapper: %llu names, vkCreateInstance %llu, vkCreateDevice %llu; "
              "vkQueueSubmit by vkd3d %llu on %llu VkQueue(s) (while the suspend gate was set: admitted %llu, OUTSIDE an admission %llu)\n",
            bb_names[bb_mode_env],P(n_bb_images_made),P(n_bb_images_freed),P(n_bb_drains),P(atomic_load(&n_bb_drain_free)),kh_vko_live_total(),P(gw_names),P(gw_inst_calls),P(gw_dev_calls),
            P(atomic_load(&gw_submits)),P(atomic_load(&gw_queues_seen)),P(atomic_load(&gw_sub_set_admitted)),P(atomic_load(&gw_sub_set_outside)));
    kh_present_report(f); kh_gfx_gate_report(f);
    census_line(f,"report");                                                     /* TASK38: the memory census, every 300 presents and at exit */
    census_report(f);
    if(kh_gfx_wrap_on) kh_wrap_report(f);
    if(video_path[0]){
        pthread_mutex_lock(&vid_m); const char *state=video_state(); pthread_mutex_unlock(&vid_m);
        fprintf(f,"kh: gfx video: %s: %llu of %llu pictures written%s (rgb24 %ux%u, scale 1/%d, 60 per second of game time; picture 0 = virtual vblank %llu); repeats=%llu replaced-presents=%llu captures=%llu vblanks-before-the-first-picture=%llu%s\n",video_path,P(vid_written),P(video_max),
                state,vid_w,vid_h,video_scale,P(video_first),P(vid_repeats),P(vid_replaced),P(vid_captures),P(vid_before_first),
                kh_vclock_on?"":" [KHNMC_CLOCK is not frames]");
    }
}

/* ---- selftest (with KHNMC_GFX=vkd3d or vkd3dw): the game's call pattern through the guest ABI, then the picture is checked --------
 * The same test drives both bridges.  Every pointer it passes is a GUEST value: guest memory (m+...), an object the bridge handed out, or
 * GP(host address) = the guest address at which the lifted code would see that host address (the identity for the trampoline bridge). */
static int st_checks, st_fails;
#define T(c) do{ st_checks++; if(!(c)){ st_fails++; fprintf(stderr,"vkd3d selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static int st_rsp_ok;
int kh_gfxc_st_fails;                          /* guest-stack failures seen by kh_gfxc_st_call; each selftest that uses it adds them to its own count */
u64 kh_gfxc_st_call(KCtx *g,u64 target,void (*imp)(KCtx*),int n,const u64 *a){
    u64 saved[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(saved[0]-0x300)&~15ull; memset(hp(g,sp),0,0x100);
    for(int i=4;i<n;i++) wr64(g,sp+8ull*i,a[i]);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0;
    if(imp) imp(g); else kh_dispatch(g,target);
    st_rsp_ok=KH_RSP(g)==sp+8; if(!st_rsp_ok){ kh_gfxc_st_fails++; fprintf(stderr,"vkd3d selftest FAIL: guest rsp after the call is %llx, expected %llx\n",P(KH_RSP(g)),P(sp+8)); }
    u64 r=KH_RAX(g); KH_RSP(g)=saved[0]; KH_RCX(g)=saved[1]; KH_RDX(g)=saved[2]; KH_R8(g)=saved[3]; KH_R9(g)=saved[4];
    return r;
}
#define GPP(T,p) ((T)(uintptr_t)GP(p))         /* a host address as the pointer VALUE a guest structure holds */
static int ppm_is(const char *path,u32 w,u32 h,u8 r,u8 gg,u8 b){
    FILE *f=fopen(path,"rb"); if(!f){ fprintf(stderr,"vkd3d selftest: cannot open %s\n",path); return 0; }
    char hdr[64]; int hl=snprintf(hdr,sizeof hdr,"P6\n%u %u\n255\n",w,h); char got[64]; int ok=fread(got,1,(size_t)hl,f)==(size_t)hl&&!memcmp(got,hdr,(size_t)hl);
    u64 bad=0,cnt=0; int c0,c1,c2; while(ok&&(c0=fgetc(f))!=EOF){ c1=fgetc(f); c2=fgetc(f); cnt++; if(c0!=r||c1!=gg||c2!=b){ if(!bad) fprintf(stderr,"vkd3d selftest: pixel %llu is (%d,%d,%d), expected (%u,%u,%u)\n",P(cnt-1),c0,c1,c2,r,gg,b); bad++; } }
    fclose(f); return ok&&!bad&&cnt==(u64)w*h;
}
/* clear the current back buffer to a colour through the guest ABI and present it (the swap chain's buffers rest in state PRESENT) */
static int st_clear_present(KCtx *g,u64 m,u64 dev,u64 q,u64 sc,u64 al,u64 cl,u64 handle,float r,float gg,float b){
    u32 i=(u32)GC(VT(sc,36),sc); if((u32)GC(VT(sc,9),sc,i,GP(&IID_ID3D12Resource),m+1024)) return 0; u64 rt=rd64(g,m+1024); if(!rt) return 0;
    GC(VT(dev,20),dev,rt,0,handle);
    if((u32)GC(VT(al,8),al)||(u32)GC(VT(cl,10),cl,al,0)){ GC(VT(rt,2),rt); return 0; }
    D3D12_RESOURCE_BARRIER x; memset(&x,0,sizeof x); x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt; x.Transition.Subresource=0xffffffffu; x.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; memcpy(hp(g,m+1040),&x,sizeof x); GC(VT(cl,26),cl,1,m+1040);
    float col[4]={r,gg,b,1.0f}; memcpy(hp(g,m+1104),col,16); GC(VT(cl,48),cl,handle,m+1104,0,0);
    x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; x.Transition.StateAfter=0; memcpy(hp(g,m+1040),&x,sizeof x); GC(VT(cl,26),cl,1,m+1040);
    int ok=(u32)GC(VT(cl,9),cl)==0; wr64(g,m+1120,cl); GC(VT(q,10),q,1,m+1120); GC(VT(rt,2),rt);
    return ok&&(u32)GC(VT(sc,8),sc,1,0)==0;
}
static int raw_is(const u8 *p,size_t n,u8 r,u8 gg,u8 b){ for(size_t i=0;i<n;i+=3) if(p[i]!=r||p[i+1]!=gg||p[i+2]!=b) return 0; return 1; }
/* TASK36 selftest helpers (ext_in: with the device report, TASK38) */
int kh_vk_test_read_image(ID3D12Device *dev,ID3D12CommandQueue *q,u64 image,u32 w,u32 h,int general,u8 *out);
typedef struct { unsigned hold_ms; u64 d_at_clear, p_at_clear, f_at_clear, dr_at_clear, fr_at_clear; ID3D12Fence *fence; } GateObs;
static void *gate_obs_clear(void *a){ GateObs *o=(GateObs*)a; struct timespec ts={(time_t)(o->hold_ms/1000),(long)(o->hold_ms%1000)*1000000L}; nanosleep(&ts,0);
    o->d_at_clear=atomic_load(&dumps_written); o->p_at_clear=kh_gfx_gate_parks(); if(o->fence) o->f_at_clear=ID3D12Fence_GetCompletedValue(o->fence);
    o->dr_at_clear=atomic_load(&n_bb_drains); o->fr_at_clear=atomic_load(&n_bb_images_freed); kh_gfx_gate_clear(); return NULL; }

/* ---- selftest of the shared part that needs no device (every --selftest of a binary with the backend; review U35 n=2, 4, 27) ---- */
/* one IUnknown slot (AddRef 1, Release 2) through the object's vtable, as the lifted code calls it; neither slot reads the guest stack */
static u64 st_slot(KCtx *g,u64 o,int slot){ u64 sp0=KH_RSP(g); KH_RSP(g)=sp0-8; KH_RCX(g)=o; kh_dispatch(g,VT(o,slot)); u64 r=KH_RAX(g); KH_RSP(g)=sp0; return r; }
typedef struct { u64 al, bl, sp; int rounds; _Atomic int zero; } BRace;
static void *st_bundle_race(void *p){ BRace *r=(BRace*)p; KCtx *g=kh_new_ctx(); KH_RSP(g)=r->sp;      /* a guest thread of its own: the handlers set rax */
    for(int i=0;i<r->rounds;i++){ st_slot(g,r->al,1); st_slot(g,r->bl,1); if(!st_slot(g,r->al,2)) r->zero++; if(!st_slot(g,r->bl,2)) r->zero++; }
    free(g); return NULL; }
typedef struct { u64 h; _Atomic u32 res; _Atomic int done; } FWait;
static void st_fence_waiter(KCtx *g){ FWait *w=(FWait*)(uintptr_t)A0(g); atomic_store(&w->res,kh_wait(g,w->h,1500)); atomic_store(&w->done,1); KH_RET(g); }
int kh_gfxc_selftest_pure(KCtx *g){
    int n=0, f=0;
#define TP(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"vkd3d shared-part selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    /* n=4: AddRef and Release of a bundle and a bundle allocator on 8 threads at once (the game AddRefs both).  The test holds 2^20
     * references of its own, so a lost update can never take a count to 0 here; afterwards both counts must be exactly that again. */
    { bundle_classes(); u64 al=kh_gfxc_bundle_allocator_new(g), bl=kh_gfxc_bundle_new(g,al,0), live=n_bundles_live;
      TP(rd64(g,al+16)==1 && rd64(g,bl+16)==1 && kh_gfxc_is_bundle(g,bl));
      enum { NT=8 }; const u64 base=1ull<<20; u64 stk=ga_calloc(NT,256); wr64(g,al+16,base); wr64(g,bl+16,base);
      BRace r[NT]; pthread_t t[NT]; for(int i=0;i<NT;i++){ r[i].al=al; r[i].bl=bl; r[i].sp=stk+256ull*(u64)i+192; r[i].rounds=20000; atomic_store(&r[i].zero,0);
          if(pthread_create(&t[i],NULL,st_bundle_race,&r[i])) kh_fatal(g,"gfx-selftest-thread: the bundle reference-count selftest cannot start host thread %d",i); }
      int zero=0; for(int i=0;i<NT;i++){ pthread_join(t[i],NULL); zero+=atomic_load(&r[i].zero); }
      TP(rd64(g,al+16)==base && rd64(g,bl+16)==base); TP(zero==0 && n_bundles_live==live);
      if(rd64(g,al+16)==base && rd64(g,bl+16)==base){                               /* the last reference destroys, the one before does not */
          wr64(g,al+16,2); wr64(g,bl+16,2);
          TP(st_slot(g,bl,2)==1 && kh_gfxc_is_bundle(g,bl) && n_bundles_live==live); TP(st_slot(g,bl,2)==0 && n_bundles_live==live-1);
          TP(st_slot(g,al,2)==1 && rd64(g,al)==kh_com_vtbl(C_balloc)); TP(st_slot(g,al,2)==0); }
      ga_free(stk); }
    /* Releasing a bundle on both branches used to free the same native command array twice. */
    if(!ga_spread_on()){
        u64 al=kh_gfxc_bundle_allocator_new(g), bl=bundle_new(g,al,0,1);
        Bundle *b=bundle_of(g,bl);
        bundle_rec(g,b,12,1,2,3,4,0,al,8);
        KhRecords cut={0};
        kh_records_begin(&cut,0); kh_rewind_arena(&cut); kh_records_end(&cut);
        TP(st_slot(g,bl,2)==0);
        kh_records_begin(&cut,1); kh_rewind_arena(&cut); kh_records_end(&cut);
        b=bundle_of(g,bl);
        TP(b->n==1&&b->cmd[0].a[3]==4&&b->bn==16&&!memcmp(b->blob,hp(g,al),8));
        atomic_fetch_add(&n_bundles_live,1);
        TP(st_slot(g,bl,2)==0); TP(st_slot(g,al,2)==0);
        kh_records_free(&cut);
    }
    /* n=2: a fence completion is SetEvent on the game's event: two completions before either of two blocked waiters of an auto-reset
     * event has run release both (one blocked waiter per SetEvent, as on Windows) and leave the event reset */
    { u64 ev=kh_mk_event_h(0,0); FWait w[2]; for(int i=0;i<2;i++){ w[i].h=ev; atomic_store(&w[i].res,0xdeadu); atomic_store(&w[i].done,0); kh_spawn_hostfn_thread(g,"gfx_selftest_fence_waiter",st_fence_waiter,(u64)(uintptr_t)&w[i]); }
      int blocked=0; struct timespec ts={0,200000}; for(int i=0;i<15000&&!blocked;i++){ int wt=0; u64 sat=0; if(!kh_wobj_waitstate(ev,&wt,&sat)&&wt>=2) blocked=1; else nanosleep(&ts,0); }
      TP(blocked);
      u64 s0=n_events_signalled; sig_event((HANDLE)(uintptr_t)ev); sig_event((HANDLE)(uintptr_t)ev); TP(n_events_signalled==s0+2);
      int done=0; for(int i=0;i<15000&&done<2;i++){ done=atomic_load(&w[0].done)+atomic_load(&w[1].done); if(done<2) nanosleep(&ts,0); }
      TP(done==2 && atomic_load(&w[0].res)==0 && atomic_load(&w[1].res)==0); TP(kh_wait(g,ev,0)==0x102);
      h_close(ev); }
    /* n=27: the video report.  kh_gfx_vkd3d_finish marks every stream done; one the run cut short must still read INCOMPLETE */
    { pthread_mutex_lock(&vid_m); char sv_path[sizeof video_path]; memcpy(sv_path,video_path,sizeof video_path); FILE *sv_f=video_f, *sv_ix=video_ix;
      int sv_armed=vid_armed, sv_done=vid_done, sv_cut=vid_cut; u64 sv_max=video_max, sv_wr=vid_written; pthread_mutex_unlock(&vid_m);
      snprintf(video_path,sizeof video_path,"(selftest: no file)"); video_ix=NULL; video_max=1000; const char *st;
      /* open, 10 of 1000 pictures written, then the run ends */
      video_f=kh_tmpfile(); TP(video_f!=NULL); vid_armed=1; vid_done=vid_cut=0; atomic_store(&vid_written,10);
      pthread_mutex_lock(&vid_m); st=video_state(); pthread_mutex_unlock(&vid_m); TP(strstr(st,"(open)")!=NULL);
      kh_gfx_vkd3d_finish(); pthread_mutex_lock(&vid_m); st=video_state(); pthread_mutex_unlock(&vid_m); TP(video_f==NULL && strstr(st,"INCOMPLETE")!=NULL);
      /* armed, no picture written yet, then the run ends */
      video_f=NULL; vid_armed=1; vid_done=vid_cut=0; atomic_store(&vid_written,0);
      kh_gfx_vkd3d_finish(); pthread_mutex_lock(&vid_m); st=video_state(); pthread_mutex_unlock(&vid_m); TP(strstr(st,"INCOMPLETE")!=NULL);
      /* never armed: the run ended before its first vblank */
      video_f=NULL; vid_armed=0; vid_done=vid_cut=0; atomic_store(&vid_written,0);
      kh_gfx_vkd3d_finish(); pthread_mutex_lock(&vid_m); st=video_state(); pthread_mutex_unlock(&vid_m); TP(strstr(st,"not started")!=NULL);
      /* every picture written: the stream ended itself and stays complete through the finish */
      video_f=NULL; vid_armed=1; vid_done=1; vid_cut=0; atomic_store(&vid_written,1000);
      kh_gfx_vkd3d_finish(); pthread_mutex_lock(&vid_m); st=video_state(); pthread_mutex_unlock(&vid_m); TP(strstr(st,"(complete)")!=NULL);
      pthread_mutex_lock(&vid_m); memcpy(video_path,sv_path,sizeof video_path); video_f=sv_f; video_ix=sv_ix; vid_armed=sv_armed; vid_done=sv_done; vid_cut=sv_cut; video_max=sv_max; atomic_store(&vid_written,sv_wr); pthread_mutex_unlock(&vid_m); }
    /* task 40 review finding 3: the band log's edge windows.  A 1920x1350 picture of a smooth scene; a fade over the band only that
     * overshoots it by 3 rows (rows 132..1217, as the exe's GS y -1..417 quad at 1920x1350, run-Z40/png/fade-in-...-H4-off-p952.png)
     * is invisible to the one-row edges and seen by the windows; a HUD edge 5 rows inside the band (row 1210, as the command menu's)
     * is not in a window; a 16:9 picture has no strips. */
    { const u32 W=1920,H=1350,PI=4*W; u8 *px=malloc((size_t)PI*H); TP(px!=NULL);
      if(px){ BandSt b;
        #define SCENE() do{ for(u32 y=0;y<H;y++) for(u32 x=0;x<W;x++){ u8 *q=px+(size_t)y*PI+4ull*x; q[0]=(u8)(40+y/12+(x&7)); q[1]=(u8)(60+y/10); q[2]=(u8)(90+(x/64)); q[3]=255; } }while(0)
        SCENE(); for(u32 y=132;y<=1217;y++) for(u32 x=0;x<W;x++){ u8 *q=px+(size_t)y*PI+4ull*x; q[0]=(u8)(q[0]*2/5); q[1]=(u8)(q[1]*2/5); q[2]=(u8)(q[2]*2/5); }
        TP(band_stats(px,PI,W,H,&b)&&b.o==135&&b.ch==1080&&b.win==6);
        TP(b.et<1.0&&b.eb<1.0&&b.etw>20.0&&b.ebw>20.0);                         /* the one-row edges miss the seam, the windows see it */
        TP(b.etw>3.0*(b.p95>1?b.p95:1)&&b.ebw>3.0*(b.p95>1?b.p95:1));         /* fill_compare.py tallcheck (s) fails on it */
        SCENE(); for(u32 y=1150;y<=1210;y++) for(u32 x=0;x<W;x++){ u8 *q=px+(size_t)y*PI+4ull*x; q[0]=q[1]=q[2]=230; }
        TP(band_stats(px,PI,W,H,&b)&&row_step(px,PI,W,1210)>20.0&&b.ebw<1.0&&b.etw<1.0);   /* an edge inside the band is not a seam */
        TP(band_stats(px,PI,W,1080,&b)&&b.o==0&&b.win==0&&b.etw==0&&b.ebw==0);          /* 16:9: no strips, no windows */
        #undef SCENE
        free(px); } }
#undef TP
    fprintf(stderr,"kh: gfx vkd3d shared-part selftest: %d checks, %d failures (bundle reference counts on 8 threads, fence events, video report, band-log edge windows)\n",n,f);
    return f;
}
/* ---- TASK43 (plan step 12): the disk pipeline cache on a REAL device (lavapipe here, MoltenVK on the Mac and the phone) -----------
 * Runs last: the device of the suites above is alive and has drawn, so its VkPipelineCache holds the pipelines they made.  Run 1: its
 * data is written (only while parked: first refused with the gate clear); run 2: a second device is handed the file and the driver
 * takes it ("loaded N bytes"); then a truncated file, garbage, a file of another device and a driver that refuses the bytes: each by
 * name, counted, and the device still gets a cache; a capability probe gets nothing; the cap; a creation call inside the driver holds
 * the save off; the save at exit.  The files and the rules without a device: gfx_pipecache.c kh_pc_selftest. */
/* Review C43: the pattern frame is drawn through the empty cache and through the loaded one (the same picture); a failed creation is
 * loud; the three calls that must wait for a save that reads the cache do; a failing data call; the load guard's marker. */
typedef struct { _Atomic int go; } StPcHold;
static void *st_pc_hold(void *a){ StPcHold *h=(StPcHold*)a; gw_pipe_enter(); atomic_store(&h->go,1);
    while(atomic_load(&h->go)!=2){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    gw_pipe_leave(NULL,VK_NULL_HANDLE,VK_NULL_HANDLE,0,VK_SUCCESS,0,0); atomic_store(&h->go,3); return NULL; }
/* review C43 n=3 / n=8: a save on its own thread (it stands in the fetch, between the size call and the data call), and the three calls
 * that must wait for it: a pipeline creation that arrives, vkDestroyPipelineCache of the cache being read, vkDestroyDevice */
typedef struct { _Atomic int res, done; } StPcSave;
static void *st_pc_saver(void *a){ StPcSave *s=(StPcSave*)a; atomic_store(&s->res,kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)); atomic_store(&s->done,1); return NULL; }
typedef struct { int kind; VkDevice d; VkPipelineCache pc; _Atomic int done; u64 ret_ns; } StPcWaiter;
static void *st_pc_waiter(void *a){ StPcWaiter *w=(StPcWaiter*)a;
    if(w->kind==0){ gw_pipe_enter(); gw_pipe_leave(NULL,VK_NULL_HANDLE,VK_NULL_HANDLE,0,VK_SUCCESS,0,0); }
    else if(w->kind==1) gw_destroy_pipeline_cache(w->d,w->pc,NULL);
    else gw_destroy_device(w->d,NULL);
    w->ret_ns=kh_host_ns(); atomic_store(&w->done,1); return NULL; }
static u32 st_le32(const u8 *p){ return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static int st_pipecache_device(KCtx *g,u64 dev,u64 q,u64 fct){
    extern void rt_imp_d3d12__ord101(KCtx*);
    int n=0,f=0;
#define TC(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"gfx pipecache device selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    char dir[KH_TMP_MAX], path[KH_TMP_MAX+32], mark[KH_TMP_MAX+48], why[640]; kh_tmp_path(dir,sizeof dir,"khnmc-pipecache-dev-XXXXXX");
    if(!mkdtemp(dir)){ fprintf(stderr,"gfx pipecache device selftest FAIL: mkdtemp(%s): %s\n",dir,strerror(errno)); return 1; }
    snprintf(path,sizeof path,"%s/pipe.bin",dir); snprintf(mark,sizeof mark,"%s.loading",path);
    u64 m=ga_calloc(1,256); KhPcCounters c0,c1; KhPcDev id; memset(&id,0,sizeof id); struct stat st;
    char sv_dump[sizeof dump_dir]; memcpy(sv_dump,dump_dir,sizeof sv_dump); dump_dir[0]=0; u64 sv_presents=gfx_presents;      /* the pattern frames below are presented: no dump of them, the run's present count as it was */
    u8 *pic1=malloc(4u*96u*64u), *pic2=malloc(4u*96u*64u); int pat1=-1, pat2=-1; double dm1=0, dm2=0;
    pthread_mutex_lock(&gw_dm); GwDev *mn=gw_cache_newest(); int have=mn!=NULL&&mn->have_id; u64 cre=mn?mn->creations:0, gpc=gw_gp.calls, gpw=gw_gp.with_cache; if(mn) id=mn->id; pthread_mutex_unlock(&gw_dm);
    TC(have); TC(cre>0 && gpc>0 && gpw>=cre);                    /* the suites above made pipelines through this device's cache, and the wrapper counted them */
    TC(pic1!=NULL && pic2!=NULL);
    if(!have||!pic1||!pic2) goto out;
    /* review C43 n=1: the selftest's pattern frame, drawn on THIS device (its cache was made empty) before the cache is saved: the file of
     * run 1 holds what that draw's pipeline put into the cache, and the picture is the reference for the device of run 2 */
    { pat1=kh_present_test_pattern(g,m,fct,q,pic1,&dm1);
      pthread_mutex_lock(&gw_dm); u64 cre1=mn->creations; pthread_mutex_unlock(&gw_dm);
      if(pat1==3) fprintf(stderr,"kh: gfx pipecache device selftest: the pattern frames are NOT RUN (KHNMC_GFX_BACKBUFFERS=committed: no VkImage of ours to read back)\n");
      else { TC(pat1==0 && dm1<=2.0); TC(cre1>cre); cre=cre1; } }
    /* review C43 n=2: a pipeline creation that FAILS is said by the wrapper when it happens and counted; vkd3d drops the draw (the picture
     * is not the pattern) and goes on: the next frame creates the pipeline */
    if(pat1==0){ pthread_mutex_lock(&gw_dm); u64 f0=gw_gp.failed, s0=gw_fail_said, k0=gw_gp.calls; pthread_mutex_unlock(&gw_dm);
      double dmf=0; gw_test_fail_gp_next=1; int pf=kh_present_test_pattern(g,m,fct,q,pic2,&dmf);
      pthread_mutex_lock(&gw_dm); u64 f1=gw_gp.failed, s1=gw_fail_said, k1=gw_gp.calls; pthread_mutex_unlock(&gw_dm);
      TC(pf==0 && gw_test_fail_gp_next==0 && f1==f0+1 && k1==k0+1 && (s1==s0+1||s0>=8));
      { int flat=1; for(u32 i=0;i<96u*64u;i++) if(pic2[4*i]!=255||pic2[4*i+1]!=0||pic2[4*i+2]!=255){ flat=0; break; } TC(flat && dmf>8.0); }   /* the frame went on without its draw: the colour under it */
      pf=kh_present_test_pattern(g,m,fct,q,pic2,&dmf); TC(pf==0 && dmf<=2.0 && !memcmp(pic1,pic2,4u*96u*64u));       /* and without the failure the same picture again */
      pthread_mutex_lock(&gw_dm); cre=mn->creations; f1=gw_gp.failed; pthread_mutex_unlock(&gw_dm); TC(f1==f0+1); }
    kh_pc_test_config(path,0);
    /* run 1.  The selftest's thread is the one that renders: with the gate clear its save is REFUSED and nothing is written */
    TC(!kh_gfx_gate_is_set() && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_REFUSED && stat(path,&st)!=0);
    kh_gfx_gate_set(); kh_pc_counters(&c0); int r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); kh_pc_counters(&c1);
    TC(r==KH_PCS_WRITTEN && c1.save_written==c0.save_written+1 && c1.save_bytes>c0.save_bytes);
    size_t saved=(size_t)(c1.save_bytes-c0.save_bytes), len=0; uint8_t *file=NULL, *good=NULL; const uint8_t *pl=NULL;
    TC(kh_pc_file_load(path,&id,kh_pc_cap(),&file,&pl,&len,why,sizeof why)==KH_PC_OK && len==saved && len>=16);
    if(file&&len>=16){ good=malloc(len); if(good) memcpy(good,pl,len);
        /* the driver's own header (VkPipelineCacheHeaderVersionOne: length, version 1, vendorID, deviceID, then the UUID) names the device
         * our header names (the UUID only when the driver wrote that far: lavapipe writes 20 bytes) */
        TC(st_le32(pl+4)==1 && st_le32(pl+8)==id.vendor && st_le32(pl+12)==id.device && (len<32||!memcmp(pl+16,id.uuid,16)));
        char hx[80]; for(size_t i=0;i<len&&i<32;i++) snprintf(hx+2*i,3,"%02x",pl[i]);
        fprintf(stderr,"kh: gfx pipecache device selftest: the driver's cache data begins %s (%zu bytes in all)\n",hx,len); }
    free(file); file=NULL; TC(good!=NULL);
    { u64 fd0=gw_fetch_data_calls; kh_pc_counters(&c0); TC(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED); kh_pc_counters(&c1);   /* no pipeline since the save: the driver is not asked again */
      TC(c1.save_unchanged_clean==c0.save_unchanged_clean+1 && gw_fetch_data_calls==fd0); }
    kh_gfx_gate_clear();
    fprintf(stderr,"kh: gfx pipecache device selftest: run 1 wrote %zu bytes of the driver's cache data (%llu pipeline creation call(s) through that cache)\n",saved,P(cre));
    if(!good) goto out;
    /* run 2: another device is created; the wrapped vkCreatePipelineCache hands the driver the file's bytes and the driver takes them */
    { kh_pc_counters(&c0); pthread_mutex_lock(&gw_dm); u64 made0=gw_caches_made; pthread_mutex_unlock(&gw_dm);
      TC((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),m)==0); u64 d2=rd64(g,m); TC(d2!=0); kh_pc_counters(&c1);
      TC(c1.load_ok==c0.load_ok+1 && c1.load_bytes==c0.load_bytes+saved && c1.driver_refused==c0.driver_refused && c1.load_rejected==c0.load_rejected && c1.load_missing==c0.load_missing);
      TC(gw_init_bytes_last==(u64)saved);                                      /* the REAL vkCreatePipelineCache was called with the file's bytes as pInitialData */
      TC(stat(mark,&st)!=0);                                                   /* review C43 n=12: the load guard's marker is gone once the driver's call is back */
      pthread_mutex_lock(&gw_dm); GwDev *e2=gw_cache_newest(); int ok=e2&&e2!=mn&&e2->cache&&!e2->creations&&!e2->probe&&gw_caches_made==made0+1; pthread_mutex_unlock(&gw_dm); TC(ok);
      kh_gfx_gate_set(); TC(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED); kh_gfx_gate_clear();     /* the newest cache made no pipeline: nothing to write */
      /* review C43 n=1: THE DRAW THROUGH THE LOADED CACHE.  The same pattern frame on this device, whose VkPipelineCache the driver built
       * from the file's bytes: its pipeline is created through that cache (counted), and the picture is the first device's, byte for byte.
       * (On lavapipe the file is a 20-byte stub; on MoltenVK it holds the MSL of the pattern's shaders, and this is the draw that uses it.) */
      if(d2&&ok&&pat1==0){ u64 q2=0; memset(hp(g,m),0,64); TC((u32)GC(VT(d2,8),d2,m,GP(&IID_ID3D12CommandQueue),m+64)==0); q2=rd64(g,m+64); TC(q2!=0);
          if(q2){ pthread_mutex_lock(&gw_dm); u64 w0=gw_gp.with_cache; pthread_mutex_unlock(&gw_dm);
              pat2=kh_present_test_pattern(g,m,fct,q2,pic2,&dm2);
              pthread_mutex_lock(&gw_dm); u64 cr2=e2->creations, w1=gw_gp.with_cache; pthread_mutex_unlock(&gw_dm);
              TC(pat2==0 && dm2<=2.0); TC(cr2>=1 && w1>w0);                     /* a pipeline was created through the LOADED cache */
              unsigned long long diff=0; for(u32 i=0;i<4u*96u*64u;i++) diff+=pic1[i]!=pic2[i];
              TC(pat2==0 && diff==0);                                          /* the picture drawn through the loaded cache IS the one drawn through the empty cache */
              fprintf(stderr,"kh: gfx pipecache device selftest: the pattern frame drawn through the LOADED cache (%zu bytes loaded, %llu pipeline creation(s) through it) against the one drawn on the first device: %llu of %u bytes differ; against the CPU-made pattern: largest difference %.2f (first device %.2f)\n",
                      saved,P(cr2),diff,4u*96u*64u,dm2,dm1);
              /* what the loaded cache holds after that draw: the same bytes (nothing new was compiled), or more */
              kh_gfx_gate_set(); kh_pc_counters(&c0); r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); kh_pc_counters(&c1); kh_gfx_gate_clear(); TC(r==KH_PCS_UNCHANGED||r==KH_PCS_WRITTEN);
              fprintf(stderr,"kh: gfx pipecache device selftest: the loaded cache after its draw: %s\n",r==KH_PCS_UNCHANGED?"the driver's data is byte for byte the file's (the draw added nothing)":r==KH_PCS_WRITTEN?"the driver's data differs from the file's: written (see the saved line above)":"NOT SAVED");
              TC(GC(VT(q2,2),q2)==0); } }
      if(d2) GC(VT(d2,2),d2);
      pthread_mutex_lock(&gw_dm); ok=gw_cache_newest()==mn; pthread_mutex_unlock(&gw_dm); TC(ok); }                 /* destroyed with its device: the table forgot it */
    /* a capability probe (ppDevice NULL) makes and destroys a device: its cache is given nothing, the file is not even read */
    { kh_pc_counters(&c0); TC((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),0)==1); kh_pc_counters(&c1);
      TC(c1.probes==c0.probes+1 && c1.load_ok==c0.load_ok && c1.load_missing==c0.load_missing && c1.load_rejected==c0.load_rejected); }
    /* a truncated file, garbage, a file written by another device, a file whose last load never came back (the load guard's marker is
     * still there: review C43 n=12) and a driver that refuses bytes our checks passed: each is named in the log and counted under its
     * own name, the device gets an EMPTY cache and works */
    for(int k=0;k<5;k++){
        static const int kind[5]={KH_PC_TRUNCATED,KH_PC_GARBAGE,KH_PC_FOREIGN_DEVICE,KH_PC_LOAD_UNFINISHED,KH_PC_DRIVER_REFUSED}; int wrote=0;
        if(k==0){ uint8_t *img=malloc(KH_PC_HEADER_BYTES+len); if(img){ kh_pc_header_make(img,&id,good,len); memcpy(img+KH_PC_HEADER_BYTES,good,len);
                  FILE *fp=fopen(path,"wb"); if(fp){ size_t half=(KH_PC_HEADER_BYTES+len)/2; wrote=fwrite(img,1,half,fp)==half; fclose(fp); } free(img); } }
        else if(k==1){ FILE *fp=fopen(path,"wb"); if(fp){ u8 gb[1000]; for(unsigned i=0;i<sizeof gb;i++) gb[i]=(u8)(i*37+k); wrote=fwrite(gb,1,sizeof gb,fp)==sizeof gb; fclose(fp); } }
        else if(k==2){ KhPcDev other=id; other.uuid[3]^=0x5a; wrote=kh_pc_file_save(path,&other,good,len,why,sizeof why)==0; }
        else if(k==3){ wrote=kh_pc_file_save(path,&id,good,len,why,sizeof why)==0; FILE *fp=fopen(mark,"wb"); if(fp) fclose(fp); else wrote=0; }
        else { wrote=kh_pc_file_save(path,&id,good,len,why,sizeof why)==0; gw_test_refuse_next=1; }
        TC(wrote); kh_pc_counters(&c0);
        TC((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),m)==0); u64 dk=rd64(g,m); TC(dk!=0); kh_pc_counters(&c1);
        TC(c1.load_by[kind[k]]==c0.load_by[kind[k]]+1 && c1.load_rejected==c0.load_rejected+1 && c1.load_ok==c0.load_ok && c1.load_bytes==c0.load_bytes && (c1.driver_refused==c0.driver_refused+1)==(k==4));
        pthread_mutex_lock(&gw_dm); GwDev *ek=gw_cache_newest(); int ok=ek&&ek!=mn&&ek->cache!=VK_NULL_HANDLE; pthread_mutex_unlock(&gw_dm); TC(ok);    /* an empty cache was made all the same */
        TC(gw_test_refuse_next==0); TC(stat(mark,&st)!=0); if(k==3) TC(stat(path,&st)!=0);             /* no marker is left; the file of the unfinished load is gone */
        if(dk) GC(VT(dk,2),dk); }
    /* the cap: data above it is refused, said so, and the file keeps what it had (the good file of the last case) */
    { kh_pc_test_config(path,16); pthread_mutex_lock(&gw_dm); mn->creations++; pthread_mutex_unlock(&gw_dm);       /* a pipeline was made: there is something to save */
      u64 fd0=gw_fetch_data_calls;
      kh_gfx_gate_set(); kh_pc_counters(&c0); TC(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_OVER_CAP); kh_pc_counters(&c1); TC(c1.save_over_cap==c0.save_over_cap+1 && c1.save_written==c0.save_written);
      TC(gw_fetch_data_calls==fd0);                                            /* above the cap the driver's data is not even fetched */
      TC(kh_pc_file_load(path,&id,KH_PC_DEFAULT_CAP,&file,&pl,&len,why,sizeof why)==KH_PC_OK && len==saved && !memcmp(pl,good,len)); free(file); file=NULL;
      kh_pc_test_config(path,0);
      /* a pipeline-creation call inside the driver holds the save off: skipped as busy after its bound, written once the call is back */
      StPcHold h; atomic_store(&h.go,0); pthread_t th; int started=pthread_create(&th,NULL,st_pc_hold,&h)==0; TC(started);
      if(started){ for(int i=0;i<5000&&atomic_load(&h.go)!=1;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
          u64 t0=kh_host_ns(); kh_pc_counters(&c0); r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); u64 dt=kh_host_ns()-t0; kh_pc_counters(&c1);
          TC(r==KH_PCS_BUSY && c1.save_busy==c0.save_busy+1 && dt>=(u64)(GW_SAVE_WAIT_SLICES-1)*GW_SAVE_SLICE_MS*1000000ull && gw_fetch_slices_last==GW_SAVE_WAIT_SLICES);
          /* review C43 n=11: with the caller's bound 350 ms away the wait is cut to what is left less the margin (or, on a machine that
           * stalled the test, the save is not even started: skipped late); it never waits all its slices */
          kh_pc_counters(&c0); r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,kh_host_ns()+350000000ull); kh_pc_counters(&c1);
          TC((r==KH_PCS_BUSY && c1.save_busy==c0.save_busy+1 && gw_fetch_slices_last<GW_SAVE_WAIT_SLICES) || (r==KH_PCS_LATE && c1.save_late==c0.save_late+1));
          atomic_store(&h.go,2); pthread_join(th,NULL); }
      { const u64 ms=1000000ull, t=5000000000ull;                               /* the rule itself, on made-up times */
        TC(gw_wait_slices(t,0)==GW_SAVE_WAIT_SLICES && gw_wait_slices(t,t+2000*ms)==GW_SAVE_WAIT_SLICES && gw_wait_slices(t,t+(KH_PC_SAVE_MARGIN_MS+GW_SAVE_WAIT_SLICES*GW_SAVE_SLICE_MS)*ms)==GW_SAVE_WAIT_SLICES);
        TC(gw_wait_slices(t,t+(KH_PC_SAVE_MARGIN_MS+3*GW_SAVE_SLICE_MS)*ms)==3 && gw_wait_slices(t,t+(KH_PC_SAVE_MARGIN_MS+3*GW_SAVE_SLICE_MS-1)*ms)==2 && gw_wait_slices(t,t+(KH_PC_SAVE_MARGIN_MS+GW_SAVE_SLICE_MS-1)*ms)==0);
        TC(gw_wait_slices(t,t+KH_PC_SAVE_MARGIN_MS*ms)==0 && gw_wait_slices(t,t+1)==0 && gw_wait_slices(t,t)==0 && gw_wait_slices(t,t-1000*ms)==0); }
      TC(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN); TC(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED);
      /* review C43 n=3: a data call that ends VK_INCOMPLETE, writes nothing, or writes more than it announced is a named failure: nothing
       * is written, the file is as it was, the exclusion is not left held, and the cache stays to be saved */
      for(int k=1;k<=3;k++){ struct stat s0,s1; int h0=stat(path,&s0); pthread_mutex_lock(&gw_dm); mn->creations++; pthread_mutex_unlock(&gw_dm);
          gw_test_data_fault=k; u64 fd0=gw_fetch_data_calls; kh_pc_counters(&c0); r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); kh_pc_counters(&c1);
          pthread_mutex_lock(&gw_dm); int freed=!gw_saving&&!gw_save_dev, dirty=mn->creations!=mn->saved; pthread_mutex_unlock(&gw_dm);
          TC(r==KH_PCS_FAILED && c1.save_failed==c0.save_failed+1 && c1.save_written==c0.save_written && gw_fetch_data_calls==fd0+1 && gw_test_data_fault==0);
          TC(freed && dirty && !h0 && !stat(path,&s1) && s0.st_ino==s1.st_ino && s0.st_size==s1.st_size); }
      r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); TC(r==KH_PCS_WRITTEN||r==KH_PCS_UNCHANGED);
      kh_gfx_gate_clear(); }
    /* review C43 n=3 / n=8: WHILE A SAVE READS THE CACHE.  Two Vulkan devices of our own on the same physical device (made and noted the
     * way the wrapper notes vkd3d's), X with a VkPipelineCache through the wrapper (the newest live cache: the save reads it), Y without.
     * The save stands between its size call and its data call.  A pipeline creation that arrives, vkDestroyPipelineCache of X's cache and
     * vkDestroyDevice of Y each WAIT (counted), none returns before the save went on, and the save ends with the driver's bytes */
    { ID3D12Device *rdv=(ID3D12Device*)real_of(g,dev,WK_DEVICE,"pipecache selftest device"); VkPhysicalDevice pd=vkd3d_get_vk_physical_device(rdv);
      float prio=1.0f; VkDeviceQueueCreateInfo qi; memset(&qi,0,sizeof qi); qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex=0; qi.queueCount=1; qi.pQueuePriorities=&prio;
      VkDeviceCreateInfo di; memset(&di,0,sizeof di); di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
      VkDevice X=VK_NULL_HANDLE, Y=VK_NULL_HANDLE; VkPipelineCache PX=VK_NULL_HANDLE;
      TC(gw_real_cd!=NULL && gw_real_cd(pd,&di,NULL,&X)==VK_SUCCESS && X!=VK_NULL_HANDLE); if(X) TC(gw_real_cd(pd,&di,NULL,&Y)==VK_SUCCESS && Y!=VK_NULL_HANDLE);
      if(X&&Y){ gw_dev_note(pd,X); gw_dev_note(pd,Y);
          PFN_vkCreatePipelineCache cpc=(PFN_vkCreatePipelineCache)gw_gdpa(X,"vkCreatePipelineCache"); PFN_vkDestroyPipelineCache dpc=(PFN_vkDestroyPipelineCache)gw_gdpa(X,"vkDestroyPipelineCache");
          PFN_vkDestroyDevice ddx=(PFN_vkDestroyDevice)gw_gdpa(X,"vkDestroyDevice"), ddy=(PFN_vkDestroyDevice)gw_gdpa(Y,"vkDestroyDevice"); (void)gw_gdpa(X,"vkGetPipelineCacheData");
          TC(cpc==gw_create_pipeline_cache && dpc==gw_destroy_pipeline_cache && ddx==gw_destroy_device && ddy==gw_destroy_device);
          VkPipelineCacheCreateInfo pci; memset(&pci,0,sizeof pci); pci.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
          TC(gw_create_pipeline_cache(X,&pci,NULL,&PX)==VK_SUCCESS && PX!=VK_NULL_HANDLE);
          pthread_mutex_lock(&gw_dm); GwDev *ex=gw_cache_newest(); int isx=ex&&ex->d==X&&ex->cache==PX; if(isx) ex->creations++;      /* something to save */
          u64 w0=gw_pipe_waits, dw0=gw_destroy_waits, cd0=gw_caches_destroyed; pthread_mutex_unlock(&gw_dm); TC(isx);
          StPcSave sv; atomic_store(&sv.res,-1); atomic_store(&sv.done,0); StPcWaiter wt[3]; memset(wt,0,sizeof wt); pthread_t ts_, tw[3]; int nstart=0;
          kh_gfx_gate_set(); atomic_store(&gw_test_fetch_pause,1);
          int sstart=isx&&pthread_create(&ts_,NULL,st_pc_saver,&sv)==0; TC(sstart);
          if(sstart){ struct timespec ms1={0,1000000};
              for(int i=0;i<10000&&atomic_load(&gw_test_fetch_pause)!=2&&!atomic_load(&sv.done);i++) nanosleep(&ms1,0);
              int standing=atomic_load(&gw_test_fetch_pause)==2; TC(standing);                    /* the save stands in its fetch: gw_saving, the lock free */
              if(standing){
                  wt[0].kind=0; wt[1].kind=1; wt[1].d=X; wt[1].pc=PX; wt[2].kind=2; wt[2].d=Y;
                  for(int i=0;i<3;i++) if(pthread_create(&tw[i],NULL,st_pc_waiter,&wt[i])==0) nstart++; else break;
                  TC(nstart==3);
                  u64 pw=0, dw=0;
                  for(int i=0;i<10000;i++){ pthread_mutex_lock(&gw_dm); pw=gw_pipe_waits-w0; dw=gw_destroy_waits-dw0; pthread_mutex_unlock(&gw_dm);
                      if((pw>=1&&dw>=2)||atomic_load(&wt[0].done)||atomic_load(&wt[1].done)||atomic_load(&wt[2].done)) break; nanosleep(&ms1,0); }
                  TC(pw==1); TC(dw==2);                                                           /* each of the three found the save and waits (counted) */
                  { struct timespec ms50={0,50000000}; nanosleep(&ms50,0); }
                  pthread_mutex_lock(&gw_dm); int held=gw_saving&&gw_caches_destroyed==cd0&&gw_dev_of(Y,0)!=NULL&&gw_dev_of(X,0)!=NULL&&gw_dev_of(X,0)->cache==PX; pthread_mutex_unlock(&gw_dm);
                  TC(held && !atomic_load(&wt[0].done) && !atomic_load(&wt[1].done) && !atomic_load(&wt[2].done) && !atomic_load(&sv.done)); }
              u64 rel=kh_host_ns(); atomic_store(&gw_test_fetch_pause,3); pthread_join(ts_,NULL);
              TC(atomic_load(&sv.res)==KH_PCS_WRITTEN||atomic_load(&sv.res)==KH_PCS_UNCHANGED);  /* the data call came back with the bytes: nothing destroyed the cache under it */
              for(int i=0;i<nstart;i++){ pthread_join(tw[i],NULL); TC(wt[i].ret_ns>=rel); }
              pthread_mutex_lock(&gw_dm); int after=!gw_saving&&!gw_inflight&&gw_caches_destroyed==cd0+(nstart==3)&&(nstart<3||gw_dev_of(Y,0)==NULL); pthread_mutex_unlock(&gw_dm); TC(after);
              if(nstart==3){ PX=VK_NULL_HANDLE; Y=VK_NULL_HANDLE; } }
          atomic_store(&gw_test_fetch_pause,0); kh_gfx_gate_clear(); }
      if(PX) gw_destroy_pipeline_cache(X,PX,NULL); if(Y) gw_destroy_device(Y,NULL); if(X) gw_destroy_device(X,NULL);
      pthread_mutex_lock(&gw_dm); int back=gw_cache_newest()==mn; pthread_mutex_unlock(&gw_dm); TC(back); }
    /* at exit: refused while the process is not ending; then allowed with the gate clear (the bytes are the ones on disk: not rewritten) */
    { pthread_mutex_lock(&gw_dm); mn->creations++; pthread_mutex_unlock(&gw_dm);
      TC(kh_gfx_pipecache_save(KH_PC_AT_EXIT,0)==KH_PCS_REFUSED); kh_pc_test_exiting(1); r=kh_gfx_pipecache_save(KH_PC_AT_EXIT,0); kh_pc_test_exiting(0);
      TC(r==KH_PCS_UNCHANGED||r==KH_PCS_WRITTEN); pthread_mutex_lock(&gw_dm); int clean=mn->creations==mn->saved&&!gw_saving&&!gw_inflight; pthread_mutex_unlock(&gw_dm); TC(clean); }
out:
    free(good); free(pic1); free(pic2); unlink(path); unlink(mark); { char t2[KH_TMP_MAX+40]; snprintf(t2,sizeof t2,"%s.tmp",path); TC(stat(t2,&st)!=0); }
    TC(rmdir(dir)==0); kh_pc_test_config(NULL,0); ga_free(m);
    memcpy(dump_dir,sv_dump,sizeof sv_dump); gfx_presents=sv_presents;
    TC(!kh_gfx_gate_is_set() && kh_gfx_gate_admitted()==0);
    f+=kh_gfxc_st_fails; kh_gfxc_st_fails=0;
    fprintf(stderr,"kh: gfx pipecache device selftest: %d checks, %d failures (device %s: the pattern frame drawn through the empty cache and through the LOADED one: the same picture; a failed pipeline creation said and counted; run 1 writes the driver's cache data only while parked, run 2 hands it to a new device and the driver takes it; truncated, garbage, another device's file, an unfinished load and a refusing driver: named, counted, an empty cache; a probe gets nothing; the cap; a creation call in flight, the wait cut to the caller's bound; a data call that fails; a creation and both destroy calls wait for a save that reads the cache; the save at exit)\n",n,f,vk_dev_name);
#undef TC
    return f;
}
int kh_gfx_vkd3d_selftest(KCtx *g){
    extern void rt_imp_d3d12__ord101(KCtx*), rt_imp_d3d12__D3D12SerializeRootSignature(KCtx*), rt_imp_dxgi__CreateDXGIFactory1(KCtx*);
    st_checks=st_fails=0; kh_gfxc_st_fails=0; u64 nat0=kh_native_calls;
    u64 m=ga_calloc(1,2048), dev=0,q=0,fct=0,sc=0,rt=0,heap=0,al=0,cl=0,fence=0,blob=0,rs=0,rt1=0,vb=0,pso=0,rs0=0,blob0=0,ba=0,bl=0,rt2=0,rsT=0,blobT=0,psoT=0,srvh=0,samph=0;
    ID3DBlob *vsb=NULL,*psb=NULL,*errb=NULL,*ptb=NULL;
    char save_dir[sizeof dump_dir]; memcpy(save_dir,dump_dir,sizeof dump_dir); u64 save_every=dump_every, save_presents=gfx_presents, save_dumps=dumps_written;
    char tmp[900]; kh_tmp_path(tmp,sizeof tmp,"khnmc-vkd3d-selftest-XXXXXX");      /* $KH_TMPDIR, else /tmp (host_env.c) */
    if(!mkdtemp(tmp)){ fprintf(stderr,"vkd3d selftest FAIL: mkdtemp(%s): %s\n",tmp,strerror(errno)); return 1; }
    /* D3D12CreateDevice: probe form (ppDevice NULL -> S_FALSE), then a real device; its methods are native addresses */
    T((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),0)==1);
    T((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),m)==0); dev=rd64(g,m); T(dev!=0);
    if(!dev) goto done;
    if(kh_gfx_wrap_on){ T(kh_native_range_name(VT(dev,0),NULL)==NULL); T(kh_lookup(VT(dev,0))!=NULL); }      /* wrapped: a host-function slot of a guest-side vtable */
    else              { T(kh_native_range_name(VT(dev,0),NULL)!=NULL); T(kh_lookup(VT(dev,0))==NULL); }      /* trampoline: native vkd3d code */
    /* TASK36 step 1: vkd3d loaded Vulkan through the gipa wrapper, and the presentation extensions it enabled are exactly the requested
     * ones the implementation offers (optional chains: nothing forced, nothing dropped); the report line was printed */
    { T(gw_inst_calls>=1 && gw_dev_calls>=1 && gw_names>=10 && pres_ext_printed);
      ID3D12Device *rdv=(ID3D12Device*)real_of(g,dev,WK_DEVICE,"selftest device");
      VkInstance vi=vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(rdv)); VkPhysicalDevice vpd=vkd3d_get_vk_physical_device(rdv);
      PFN_vkEnumerateInstanceExtensionProperties eie=(PFN_vkEnumerateInstanceExtensionProperties)vk_gipa(NULL,"vkEnumerateInstanceExtensionProperties");
      PFN_vkEnumerateDeviceExtensionProperties ede=(PFN_vkEnumerateDeviceExtensionProperties)vk_gipa(vi,"vkEnumerateDeviceExtensionProperties");
      T(eie!=NULL && ede!=NULL);
      if(eie&&ede){ u32 ni=0,nd=0; eie(NULL,&ni,NULL); ede(vpd,NULL,&nd,NULL); VkExtensionProperties *xi=calloc(ni+1,sizeof*xi),*xd=calloc(nd+1,sizeof*xd);
        if(xi&&xd){ eie(NULL,&ni,xi); ede(vpd,NULL,&nd,xd); const KhPresExt *x=&kh_gfxc_pres_ext;
          T(x->surface==ext_in(xi,ni,"VK_KHR_surface")); T(x->headless==ext_in(xi,ni,"VK_EXT_headless_surface")); T(x->metal==ext_in(xi,ni,"VK_EXT_metal_surface"));
          T(x->layer_settings==ext_in(xi,ni,"VK_EXT_layer_settings")); T(x->swapchain==ext_in(xd,nd,"VK_KHR_swapchain")); T(x->display_timing==ext_in(xd,nd,"VK_GOOGLE_display_timing"));
#if defined(__APPLE__)
          T(x->metal_objects==ext_in(xd,nd,"VK_EXT_metal_objects"));
#else
          T(x->metal_objects==0);                                                               /* requested on Apple only */
#endif
          fprintf(stderr,"kh: vkd3d selftest: offered by the implementation: surface=%d headless=%d metal=%d layer_settings=%d swapchain=%d display_timing=%d (enabled: see the presentation extensions line)\n",
                  ext_in(xi,ni,"VK_KHR_surface"),ext_in(xi,ni,"VK_EXT_headless_surface"),ext_in(xi,ni,"VK_EXT_metal_surface"),ext_in(xi,ni,"VK_EXT_layer_settings"),ext_in(xd,nd,"VK_KHR_swapchain"),ext_in(xd,nd,"VK_GOOGLE_display_timing")); }
        /* TASK38 (plan step 4): the device report block was printed once, for the device vkd3d uses, and says what that device says */
        T(rep.printed==1 && !rep.test_no_bc);
        if(xi&&xd){ for(int i=0;i<NREPIEXT;i++) T(rep.iext[i]==ext_in(xi,ni,rep_iext[i])); for(int i=0;i<NREPDEXT;i++) T(rep.dext[i]==ext_in(xd,nd,rep_dext[i])); }
        free(xi); free(xd); }
      PFN_vkGetPhysicalDeviceFeatures gf=(PFN_vkGetPhysicalDeviceFeatures)vk_gipa(vi,"vkGetPhysicalDeviceFeatures");
      PFN_vkGetPhysicalDeviceFormatProperties gfp=(PFN_vkGetPhysicalDeviceFormatProperties)vk_gipa(vi,"vkGetPhysicalDeviceFormatProperties");
      PFN_vkGetPhysicalDeviceMemoryProperties gmp=(PFN_vkGetPhysicalDeviceMemoryProperties)vk_gipa(vi,"vkGetPhysicalDeviceMemoryProperties");
      T(gf&&gfp&&gmp);
      if(gf&&gfp&&gmp){ VkPhysicalDeviceFeatures ft; memset(&ft,0,sizeof ft); gf(vpd,&ft); T(rep.tcbc==(int)ft.textureCompressionBC);
        int same=0; for(int i=0;i<NREPFMT;i++){ VkFormatProperties fp; memset(&fp,0,sizeof fp); VkFormat vf=rep_vk_format(i); gfp(vpd,vf,&fp); same+=rep.vkf[i]==vf&&vf!=VK_FORMAT_UNDEFINED&&rep.feat[i]==fp.optimalTilingFeatures; }
        T(same==NREPFMT);                                                                        /* the features printed are the device's, for vkd3d's own format mapping */
        T(rep.vkf[2]==VK_FORMAT_BC3_UNORM_BLOCK && rep.vkf[4]==VK_FORMAT_D32_SFLOAT_S8_UINT);
        VkPhysicalDeviceMemoryProperties mp; memset(&mp,0,sizeof mp); gmp(vpd,&mp);
        T(rep.lines==16+(int)mp.memoryHeapCount);                                                /* BEGIN..END: 15 lines + one per heap (review X38 n=2: + the soft-stop line) */
        fprintf(stderr,"kh: vkd3d selftest: device report: %d lines, textureCompressionBC=%d, BC formats not sampleable %d, VK_EXT_memory_budget=%d\n",rep.lines,rep.tcbc,rep.unsampled,rep.dext[REP_DEXT_BUDGET]); }
      /* the BC decision on made-up feature sets: none missing, BC3 alone missing, all three missing (by name, in order) */
      { const VkFormatFeatureFlags S=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, L=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        VkFormatFeatureFlags all[NBC]={S|L,S,S|L}, no3[NBC]={S|L,S|L,L}, none[NBC]={L,0,L}; char why[128];
        T(bc_unsampled(all,why,sizeof why)==0 && !why[0]); T(bc_unsampled(no3,why,sizeof why)==1 && !strcmp(why,"BC3_UNORM"));
        T(bc_unsampled(none,why,sizeof why)==3 && !strcmp(why,"BC1_UNORM, BC2_UNORM, BC3_UNORM")); T(rep.unsampled==bc_unsampled(rep.feat,why,sizeof why)); }
      /* review X38 n=2: the gfx-memory-low decision (below 512 MiB, only with a reading), and where the stop is armed: iOS, or a test reading */
      { T(!memlow_fires(0,0) && memlow_fires(1,0) && memlow_fires(1,511) && !memlow_fires(1,512) && !memlow_fires(1,6000));
        u64 m=0; int have=memlow_read(&m); T(rep.memlow_armed==have && !atomic_load(&memlow_fired));
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
        T(have==1);
#else
        T(have==(memlow_test>=0) && (!have||m==(u64)memlow_test));
#endif
        fprintf(stderr,"kh: vkd3d selftest: gfx-memory-low soft stop %s (margin %llu MiB)\n",have?"armed":"not armed",P(MEMLOW_MARGIN_MIB)); }
      /* the census: what this host does not have says absent; what it has is a number; the budget numbers iff the extension is offered */
      { char t[2048]; u64 n0=cen_lines; census_text(t,sizeof t,rdv); T(cen_lines==n0+1);
        char want[96]; snprintf(want,sizeof want,"QueryVideoMemoryInfo answers budget=%llu MiB",P(kh_dxgi_budget_answer()>>20)); T(strstr(t,want)!=NULL);
        T((strstr(t,"VK_EXT_memory_budget: heap0 budget=")!=NULL)==(rep.dext[REP_DEXT_BUDGET]!=0) && (strstr(t,"VK_EXT_memory_budget=absent")!=NULL)==!rep.dext[REP_DEXT_BUDGET]);
#if defined(__linux__)
        T(strstr(t,"phys_footprint=absent")&&strstr(t,"os_proc_available_memory=absent")&&strstr(t," rss=")&&!strstr(t," rss=-1"));
#elif defined(__APPLE__)
        T(strstr(t,"phys_footprint=")&&!strstr(t,"phys_footprint=absent")&&!strstr(t,"phys_footprint=ERROR"));
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_MACCATALYST
        T(!strstr(t,"os_proc_available_memory=absent"));
#endif
#ifdef KH_GFX_MVK_API
        T(strstr(t,"gpuMemoryAllocated=")&&!strstr(t,"gpuMemoryAllocated=absent")&&!strstr(t,"gpuMemoryAllocated=ERROR"));
#else
        T(strstr(t,"gpuMemoryAllocated=absent")!=NULL);
#endif
        fprintf(stderr,"kh: vkd3d selftest: memory census: %s\n",t); } }
    T((u32)GC(VT(dev,7),dev)==1);                                                             /* GetNodeCount */
    T((u32)GC(VT(dev,15),dev,2)!=0);                                                           /* GetDescriptorHandleIncrementSize(RTV) */
    /* CreateCommandQueue(desc, riid, **pp) ; IUnknown through the trampoline */
    memset(hp(g,m),0,64); T((u32)GC(VT(dev,8),dev,m,GP(&IID_ID3D12CommandQueue),m+64)==0); q=rd64(g,m+64); T(q!=0); if(!q) goto done;
    T((u32)GC(VT(q,1),q)==2); T((u32)GC(VT(q,2),q)==1);                                          /* AddRef / Release */
    /* our DXGI factory and swap chain on that queue: 96x64 so the row pitch (512) differs from 4*width */
    T((u32)GI(rt_imp_dxgi__CreateDXGIFactory1,GP(&IID_ID3D12Device),m+72)==0); fct=rd64(g,m+72); T(fct!=0);
    { u64 d=m+128; memset(hp(g,d),0,48); wr32(g,d,96); wr32(g,d+4,64); wr32(g,d+8,0x57); wr32(g,d+16,1); wr32(g,d+24,0x30); wr32(g,d+28,3); wr32(g,d+36,3); wr32(g,d+44,0x42);
      T((u32)GC(VT(fct,15),fct,q,0xAB0001,d,0,0,m+80)==0); sc=rd64(g,m+80); T(sc!=0); }
    if(!sc) goto done;
    T(GC(VT(sc,36),sc)==0);                                                                    /* GetCurrentBackBufferIndex */
    T((u32)GC(VT(sc,9),sc,3,GP(&IID_ID3D12Resource),m+88)==DXGI_INVALID_CALL);
    T((u32)GC(VT(sc,9),sc,0,GP(&IID_ID3D12Resource),m+88)==0); rt=rd64(g,m+88); T(rt!=0&&(kh_native_range_name(VT(rt,0),NULL)!=NULL)==!kh_gfx_wrap_on); if(!rt) goto done;
    { T((u32)GC(VT(sc,9),sc,0,GP(&IID_ID3D12Resource),m+1024)==0); u64 again=rd64(g,m+1024); T(again==rt); if(again) GC(VT(again,2),again); }   /* the same buffer is the same pointer (the same guest object) every time */
    /* ID3D12Resource::GetDesc: struct result through the hidden pointer (rcx = this, rdx = result) */
    { u64 d=m+256; memset(hp(g,d),0xA5,64); T(GC(VT(rt,10),rt,d)==d); T(rd32(g,d)==3&&rd64(g,d+16)==96&&rd32(g,d+24)==64&&rd32(g,d+32)==0x57&&(rd32(g,d+48)&1)&&rd64(g,d+56)==0xa5a5a5a5a5a5a5a5ull); }
    /* RTV heap, descriptor handle through the hidden pointer, render-target view */
    { u64 d=m+320; memset(hp(g,d),0,16); wr32(g,d,2); wr32(g,d+4,1); T((u32)GC(VT(dev,14),dev,d,GP(&IID_ID3D12DescriptorHeap),m+96)==0); heap=rd64(g,m+96); T(heap!=0); }
    if(!heap) goto done;
    T(GC(VT(heap,9),heap,m+336)==m+336); u64 handle=rd64(g,m+336); T(handle!=0);
    GC(VT(dev,20),dev,rt,0,handle);                                                            /* CreateRenderTargetView (void) */
    /* allocator, command list (7 arguments: 3 on the stack) */
    T((u32)GC(VT(dev,9),dev,0,GP(&IID_ID3D12CommandAllocator),m+104)==0); al=rd64(g,m+104); T(al!=0);
    T((u32)GC(VT(dev,12),dev,0,0,al,0,GP(&IID_ID3D12GraphicsCommandList),m+112)==0); cl=rd64(g,m+112); T(cl!=0);
    if(!al||!cl) goto done;
    { u64 b=m+352; D3D12_RESOURCE_BARRIER x; memset(&x,0,sizeof x); x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt; x.Transition.Subresource=0xffffffffu; x.Transition.StateBefore=0; x.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
      u64 c0=gfx_clears,b0=gfx_barriers,e0=gfx_execs;
      memcpy(hp(g,b),&x,sizeof x); GC(VT(cl,26),cl,1,b);                                       /* ResourceBarrier PRESENT -> RENDER_TARGET */
      float col[4]={0.25f,0.5f,0.75f,1.0f}; memcpy(hp(g,m+384),col,16); GC(VT(cl,48),cl,handle,m+384,0,0);   /* ClearRenderTargetView (5 arguments) */
      x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; x.Transition.StateAfter=0; memcpy(hp(g,b),&x,sizeof x); GC(VT(cl,26),cl,1,b);
      T((u32)GC(VT(cl,9),cl)==0);                                                              /* Close */
      wr64(g,m+400,cl); GC(VT(q,10),q,1,m+400);                                                /* ExecuteCommandLists */
      T(gfx_clears==c0+1); T(gfx_barriers==b0+2); T(gfx_execs==e0+1); }
    /* fence completion signals an event of OUR kernel32 layer */
    T((u32)GC(VT(dev,36),dev,0,0,GP(&IID_ID3D12Fence),m+120)==0); fence=rd64(g,m+120); T(fence!=0);
    if(fence){ u64 ev=kh_mk_event_h(0,0); u64 s0=n_events_signalled; T((u32)GC(VT(q,14),q,fence,1)==0); T((u32)GC(VT(fence,9),fence,1,ev)==0); T(kh_wait(g,ev,60000)==0); T(GC(VT(fence,8),fence)==1); T(n_events_signalled==s0+1); h_close(ev); }
    /* Present with a dump: the file must hold the clear colour in every pixel */
    snprintf(dump_dir,sizeof dump_dir,"%s",tmp); dump_every=1; gfx_presents=1000000;
    T((u32)GC(VT(sc,8),sc,1,0)==0); T(GC(VT(sc,36),sc)==1); T(dumps_written==save_dumps+1);
    { char path[1200]; snprintf(path,sizeof path,"%s/frame-1000001.ppm",tmp); T(ppm_is(path,96,64,64,128,191)); unlink(path); snprintf(path,sizeof path,"%s/index.tsv",tmp); T(access(path,R_OK)==0); unlink(path); }
    /* TASK36 step 6: the back buffer the game cleared and presented IS our VkImage: a Vulkan copy of image 0 holds the clear colour
     * (BGRA 191,128,64); the game sees the same description as for a committed resource */
    { Swap *s=swap_of(g,sc,"selftest"); T(s->bbmode==bb_mode_env);
      { const char *e=getenv("KHNMC_GFX_BACKBUFFERS"); if(!e||!*e) T(bb_mode_env==BB_OWN); }                       /* the default is our own images */
      if(s->bbmode!=BB_COMMITTED){ u8 *px=malloc(4u*96u*64u); T(px!=NULL&&s->img[0]!=VK_NULL_HANDLE&&kh_vk_test_read_image(s->dev,s->queue,(u64)(uintptr_t)s->img[0],96,64,s->bbmode==BB_GENERAL,px)==0);
          if(px){ int okc=1; for(u32 k=0;k<96u*64u;k++) if(px[4*k]!=191||px[4*k+1]!=128||px[4*k+2]!=64){ okc=0; break; } T(okc); free(px); } }
      D3D12_HEAP_PROPERTIES def; memset(&def,0,sizeof def); def.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC want=ID3D12Resource_GetDesc(s->buf[0]); ID3D12Resource *cr=NULL;
      T(SUCCEEDED(ID3D12Device_CreateCommittedResource(s->dev,&def,D3D12_HEAP_FLAG_NONE,&want,D3D12_RESOURCE_STATE_PRESENT,NULL,&IID_ID3D12Resource,(void**)&cr)));
      if(cr){ D3D12_RESOURCE_DESC got=ID3D12Resource_GetDesc(cr); T(!memcmp(&got,&want,sizeof got)); ID3D12Resource_Release(cr); } }
    /* a bundle like the game's: created and recorded through OUR recorder objects, replayed by ExecuteBundle on the direct list; the picture
     * proves the replay (root signature, pipeline state, topology, a deep-copied vertex-buffer view and the draw all came from the bundle) */
    { static const char vs_src[]="float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.0, 1.0); }";
      static const char ps_src[]="float4 main() : SV_Target { return float4(1.0, 0.5, 0.25, 1.0); }";
      T(D3DCompile(vs_src,sizeof vs_src-1,"selftest_vs",NULL,NULL,"main","vs_4_0",0,0,&vsb,&errb)==S_OK&&vsb); T(D3DCompile(ps_src,sizeof ps_src-1,"selftest_ps",NULL,NULL,"main","ps_4_0",0,0,&psb,&errb)==S_OK&&psb);
      if(!vsb||!psb){ if(errb) fprintf(stderr,"vkd3d selftest: HLSL compiler: %s\n",(const char*)ID3D10Blob_GetBufferPointer(errb)); goto done; }
      D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd,0,sizeof rd); rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT; memcpy(hp(g,m+448),&rd,sizeof rd);
      T((u32)GI(rt_imp_d3d12__D3D12SerializeRootSignature,m+448,1,m+136,0)==0); blob0=rd64(g,m+136); if(!blob0) goto done;
      T((u32)GC(VT(dev,16),dev,0,GC(VT(blob0,3),blob0),GC(VT(blob0,4),blob0),GP(&IID_ID3D12RootSignature),m+144)==0); rs0=rd64(g,m+144); T(rs0!=0); if(!rs0) goto done;
      D3D12_INPUT_ELEMENT_DESC ie; memset(&ie,0,sizeof ie); ie.SemanticName=GPP(const char*,"POSITION"); ie.Format=DXGI_FORMAT_R32G32_FLOAT; ie.InputSlotClass=D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd,0,sizeof pd); pd.pRootSignature=(ID3D12RootSignature*)(uintptr_t)rs0;
      pd.VS.pShaderBytecode=GPP(const void*,ID3D10Blob_GetBufferPointer(vsb)); pd.VS.BytecodeLength=ID3D10Blob_GetBufferSize(vsb); pd.PS.pShaderBytecode=GPP(const void*,ID3D10Blob_GetBufferPointer(psb)); pd.PS.BytecodeLength=ID3D10Blob_GetBufferSize(psb);
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask=0xf; pd.SampleMask=0xffffffffu; pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE;
      pd.InputLayout.pInputElementDescs=GPP(const D3D12_INPUT_ELEMENT_DESC*,&ie); pd.InputLayout.NumElements=1; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count=1;
      u64 p0=gfx_psos; T((u32)GC(VT(dev,10),dev,GP(&pd),GP(&IID_ID3D12PipelineState),m+152)==0); pso=rd64(g,m+152); T(pso!=0); T(gfx_psos==p0+1); if(!pso) goto done;
      /* vertex buffer in an upload heap (CreateCommittedResource: 8 arguments), persistently mapped like the game's */
      D3D12_HEAP_PROPERTIES up; memset(&up,0,sizeof up); up.Type=D3D12_HEAP_TYPE_UPLOAD; memcpy(hp(g,m+560),&up,sizeof up);
      D3D12_RESOURCE_DESC bd; memset(&bd,0,sizeof bd); bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=256; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; memcpy(hp(g,m+592),&bd,sizeof bd);
      u64 r0=gfx_resources; T((u32)GC(VT(dev,27),dev,m+560,0,m+592,D3D12_RESOURCE_STATE_GENERIC_READ,0,GP(&IID_ID3D12Resource),m+160)==0); vb=rd64(g,m+160); T(vb!=0); T(gfx_resources==r0+1); if(!vb) goto done;
      T((u32)GC(VT(vb,8),vb,0,0,m+168)==0); u64 vmem=rd64(g,m+168); T(vmem!=0); if(!vmem) goto done;
      { float tri[6]={-1.0f,-1.0f, -1.0f,3.0f, 3.0f,-1.0f}; memcpy(hp(g,vmem),tri,sizeof tri); }
      u64 va=GC(VT(vb,11),vb); T(va!=0);                                                       /* GetGPUVirtualAddress */
      /* bundle allocator and bundle list come from the overridden creation calls: guest objects of ours, not vkd3d's */
      T((u32)GC(VT(dev,9),dev,D3D12_COMMAND_LIST_TYPE_BUNDLE,GP(&IID_ID3D12CommandAllocator),m+176)==0); ba=rd64(g,m+176); T(GA_OK(ba)&&rd64(g,ba)==kh_com_vtbl(C_balloc));
      T((u32)GC(VT(dev,12),dev,0,D3D12_COMMAND_LIST_TYPE_BUNDLE,ba,0,GP(&IID_ID3D12GraphicsCommandList),m+184)==0); bl=rd64(g,m+184); T(GA_OK(bl)&&rd64(g,bl)==kh_com_vtbl(C_bundle));
      if(!GA_OK(ba)||!GA_OK(bl)) goto done;
      T(GC(VT(bl,8),bl)==D3D12_COMMAND_LIST_TYPE_BUNDLE);
      T((u32)GC(VT(bl,9),bl)==0); T((u32)GC(VT(bl,9),bl)==(u32)E_FAIL);                          /* Close; a second Close fails */
      T((u32)GC(VT(bl,10),bl,ba,0)==0); T((u32)GC(VT(bl,10),bl,ba,0)==(u32)E_FAIL);              /* Reset; Reset while recording fails */
      GC(VT(bl,30),bl,rs0); GC(VT(bl,25),bl,pso); GC(VT(bl,20),bl,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      { D3D12_VERTEX_BUFFER_VIEW vv; vv.BufferLocation=va; vv.SizeInBytes=24; vv.StrideInBytes=8; memcpy(hp(g,m+640),&vv,sizeof vv); GC(VT(bl,44),bl,0,1,m+640); memset(hp(g,m+640),0xEE,sizeof vv); }   /* the view must have been copied */
      GC(VT(bl,12),bl,3,1,0,0);                                                                  /* DrawInstanced (5 arguments) */
      T((u32)GC(VT(bl,9),bl)==0);
      /* the frame on the direct list: target = back buffer 1, cleared to black, then the bundle */
      T((u32)GC(VT(sc,9),sc,1,GP(&IID_ID3D12Resource),m+192)==0); rt1=rd64(g,m+192); T(rt1!=0); if(!rt1) goto done;
      GC(VT(dev,20),dev,rt1,0,handle);
      T((u32)GC(VT(al,8),al)==0); T((u32)GC(VT(cl,10),cl,al,0)==0);
      D3D12_RESOURCE_BARRIER x; memset(&x,0,sizeof x); x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt1; x.Transition.Subresource=0xffffffffu; x.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      wr64(g,m+672,handle); GC(VT(cl,46),cl,1,m+672,0,0);                                        /* OMSetRenderTargets (5 arguments) */
      { D3D12_VIEWPORT vp={0,0,96,64,0,1}; memcpy(hp(g,m+688),&vp,sizeof vp); GC(VT(cl,21),cl,1,m+688); s32 rc[4]={0,0,96,64}; memcpy(hp(g,m+720),rc,16); GC(VT(cl,22),cl,1,m+720); }
      { float black[4]={0,0,0,1}; memcpy(hp(g,m+384),black,16); GC(VT(cl,48),cl,handle,m+384,0,0); }
      u64 d0=gfx_draws,br0=n_bundles_run,bc0=n_bundle_cmds,gb0=gfx_bundles;
      GC(VT(cl,27),cl,bl);                                                                       /* ExecuteBundle -> our replay */
      T(gfx_draws==d0+1); T(n_bundles_run==br0+1); T(n_bundle_cmds==bc0+5); T(gfx_bundles==gb0+1);
      x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; x.Transition.StateAfter=0; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      T((u32)GC(VT(cl,9),cl)==0); wr64(g,m+400,cl); GC(VT(q,10),q,1,m+400);
      T((u32)GC(VT(sc,8),sc,1,0)==0); T(GC(VT(sc,36),sc)==2);
      { char path[1200]; snprintf(path,sizeof path,"%s/frame-1000002.ppm",tmp); T(ppm_is(path,96,64,255,128,64)); unlink(path); snprintf(path,sizeof path,"%s/index.tsv",tmp); unlink(path); }
      u64 live=n_bundles_live; T(GC(VT(bl,2),bl)==0); bl=0; T(n_bundles_live==live-1); T(GC(VT(ba,2),ba)==0); ba=0;
      GC(VT(rt1,2),rt1); rt1=0;
      /* the game's undefined draw, made defined: a pixel shader samples t0 with s0 on a command list where NO descriptor table was set.
       * The default binding (first descriptor of each heap) must be what it reads: an SRV of back buffer 0, which still holds the first
       * clear colour, through a point sampler.  Without the default the Vulkan driver reads an unwritten descriptor. */
      static const char pt_src[]="Texture2D t : register(t0); SamplerState s : register(s0); float4 main() : SV_Target { return t.Sample(s, float2(0.5, 0.5)); }";
      T(D3DCompile(pt_src,sizeof pt_src-1,"selftest_pt",NULL,NULL,"main","ps_4_0",0,0,&ptb,&errb)==S_OK&&ptb); if(!ptb) goto done;
      D3D12_DESCRIPTOR_RANGE rg[2]; memset(rg,0,sizeof rg); rg[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors=1; rg[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; rg[1].NumDescriptors=1;
      D3D12_ROOT_PARAMETER rp[2]; memset(rp,0,sizeof rp); for(int i=0;i<2;i++){ rp[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[i].DescriptorTable.NumDescriptorRanges=1; rp[i].DescriptorTable.pDescriptorRanges=GPP(const D3D12_DESCRIPTOR_RANGE*,&rg[i]); rp[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL; }
      rd.NumParameters=2; rd.pParameters=GPP(const D3D12_ROOT_PARAMETER*,rp); memcpy(hp(g,m+448),&rd,sizeof rd);
      T((u32)GI(rt_imp_d3d12__D3D12SerializeRootSignature,m+448,1,m+136,0)==0); blobT=rd64(g,m+136); if(!blobT) goto done;
      T((u32)GC(VT(dev,16),dev,0,GC(VT(blobT,3),blobT),GC(VT(blobT,4),blobT),GP(&IID_ID3D12RootSignature),m+144)==0); rsT=rd64(g,m+144); T(rsT!=0); if(!rsT) goto done;
      pd.pRootSignature=(ID3D12RootSignature*)(uintptr_t)rsT; pd.PS.pShaderBytecode=GPP(const void*,ID3D10Blob_GetBufferPointer(ptb)); pd.PS.BytecodeLength=ID3D10Blob_GetBufferSize(ptb);
      T((u32)GC(VT(dev,10),dev,GP(&pd),GP(&IID_ID3D12PipelineState),m+152)==0); psoT=rd64(g,m+152); T(psoT!=0); if(!psoT) goto done;
      { u64 d=m+320; memset(hp(g,d),0,16); wr32(g,d,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); wr32(g,d+4,4); wr32(g,d+8,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE); T((u32)GC(VT(dev,14),dev,d,GP(&IID_ID3D12DescriptorHeap),m+200)==0); srvh=rd64(g,m+200);
        wr32(g,d,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER); wr32(g,d+4,2); T((u32)GC(VT(dev,14),dev,d,GP(&IID_ID3D12DescriptorHeap),m+208)==0); samph=rd64(g,m+208); }
      T(srvh&&samph); if(!srvh||!samph) goto done;
      GC(VT(srvh,9),srvh,m+336); GC(VT(dev,18),dev,rt,0,rd64(g,m+336));                         /* CreateShaderResourceView(back buffer 0, default view) in slot 0 */
      { D3D12_SAMPLER_DESC sd; memset(&sd,0,sizeof sd); sd.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT; sd.AddressU=sd.AddressV=sd.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sd.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER; sd.MaxLOD=1000.0f;
        memcpy(hp(g,m+760),&sd,sizeof sd); GC(VT(samph,9),samph,m+336); GC(VT(dev,22),dev,m+760,rd64(g,m+336)); }   /* CreateSampler in slot 0 */
      T((u32)GC(VT(sc,9),sc,2,GP(&IID_ID3D12Resource),m+192)==0); rt2=rd64(g,m+192); T(rt2!=0); if(!rt2) goto done;
      GC(VT(heap,9),heap,m+336); GC(VT(dev,20),dev,rt2,0,handle);
      T((u32)GC(VT(al,8),al)==0); T((u32)GC(VT(cl,10),cl,al,0)==0);
      u64 dt0=n_default_tables;
      GC(VT(cl,30),cl,rsT); wr64(g,m+832,srvh); wr64(g,m+840,samph); GC(VT(cl,28),cl,2,m+832);    /* root signature, descriptor heaps: and nothing else */
      T(n_default_tables==dt0+2);
      memset(&x,0,sizeof x); x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt; x.Transition.Subresource=0xffffffffu; x.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt2; x.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      wr64(g,m+672,handle); GC(VT(cl,46),cl,1,m+672,0,0); GC(VT(cl,21),cl,1,m+688); GC(VT(cl,22),cl,1,m+720);
      { float black[4]={0,0,0,1}; memcpy(hp(g,m+384),black,16); GC(VT(cl,48),cl,handle,m+384,0,0); }
      GC(VT(cl,25),cl,psoT); GC(VT(cl,20),cl,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      { D3D12_VERTEX_BUFFER_VIEW vv; vv.BufferLocation=va; vv.SizeInBytes=24; vv.StrideInBytes=8; memcpy(hp(g,m+640),&vv,sizeof vv); GC(VT(cl,44),cl,0,1,m+640); }
      GC(VT(cl,12),cl,3,1,0,0);
      x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; x.Transition.StateAfter=0; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      x.Transition.pResource=(ID3D12Resource*)(uintptr_t)rt; x.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; memcpy(hp(g,m+352),&x,sizeof x); GC(VT(cl,26),cl,1,m+352);
      T((u32)GC(VT(cl,9),cl)==0); wr64(g,m+400,cl); GC(VT(q,10),q,1,m+400);
      T((u32)GC(VT(sc,8),sc,1,0)==0); T(GC(VT(sc,36),sc)==0);
      { char path[1200]; snprintf(path,sizeof path,"%s/frame-1000003.ppm",tmp); T(ppm_is(path,96,64,64,128,191)); unlink(path); snprintf(path,sizeof path,"%s/index.tsv",tmp); unlink(path); }
      /* a table the game sets itself is not overridden: set both explicitly, then change nothing -> no new defaults on the same root signature / heaps */
      T((u32)GC(VT(al,8),al)==0); T((u32)GC(VT(cl,10),cl,al,0)==0); dt0=n_default_tables;
      GC(VT(cl,30),cl,rsT); GC(VT(cl,30),cl,rsT); GC(VT(cl,28),cl,2,m+832); GC(VT(cl,28),cl,2,m+832); T(n_default_tables==dt0+2);
      T((u32)GC(VT(cl,9),cl)==0);
      GC(VT(rt2,2),rt2); rt2=0; }
    /* ResizeBuffers: refused while the game holds a back buffer, then new buffers of the new size */
    T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==DXGI_INVALID_CALL);
    GC(VT(rt,2),rt); rt=0;
    { u64 im0=n_bb_images_made, if0=n_bb_images_freed, dr0=n_bb_drains; long long live0=atomic_load(&kh_vko_live[VKO_IMAGE]);
      if(bb_mode_env!=BB_COMMITTED){
          /* TASK36 review V36 n=6: the suspend gate is checked before the back-buffer drain.  ResizeBuffers with the gate set (held 120 ms by
           * another thread) parks there: when the gate is cleared nothing has been drained or freed yet, one more park is counted, and
           * the resize then completes */
          u64 p0=kh_gfx_gate_parks(); GateObs ob; memset(&ob,0,sizeof ob); ob.hold_ms=120; ob.dr_at_clear=ob.fr_at_clear=~0ull; kh_gfx_gate_set();
          pthread_t th; T(pthread_create(&th,NULL,gate_obs_clear,&ob)==0);
          u64 t0=kh_host_ns(); T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0); u64 dt=kh_host_ns()-t0; pthread_join(th,NULL);
          T(dt>=100000000ull && ob.p_at_clear==p0+1 && ob.dr_at_clear==dr0 && ob.fr_at_clear==if0 && !kh_gfx_gate_is_set());
          T(n_bb_images_made==im0+3 && n_bb_images_freed==if0+3 && n_bb_drains==dr0+1 && atomic_load(&kh_vko_live[VKO_IMAGE])==live0);   /* TASK36: the old images are destroyed after the drain */
          /* task 39: the drain's Signal is an ADMISSION: the gate set from inside it holds kh_gfx_gate_quiesce until the Signal returned */
          kh_gfx_gate_test_arm("back-buffer drain",50); T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0); T(kh_gfx_gate_test_result("back-buffer drain"));
          /* task 39 (review c1/c2): the drain's fence wait does not count slices while the gate is set.  The queue Waits on a fence that
           * another thread signals from the CPU; once the drain's Signal is queued (its fence value moved) that thread sets the gate,
           * holds it 300 ms (15 slices of 20 ms), clears it and signals.  Counting would be gfx-backbuffer-drain after 5 slices (100 ms) */
          { Swap *ss=swap_of(g,sc,"selftest"); ID3D12Fence *fw=NULL; unsigned sl0=kh_stuck_slice_ms;
            T(SUCCEEDED(ID3D12Device_CreateFence(ss->dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fw)));
            if(fw){ kh_stuck_slice_ms=20; T(SUCCEEDED(ID3D12CommandQueue_Wait(ss->queue,fw,1)));
                KhGateHoldSig hs; memset(&hs,0,sizeof hs); hs.fence=fw; hs.set_ms=5; hs.hold_ms=300; hs.after_change=(const volatile u64*)&ss->fence_val; hs.from=ss->fence_val;
                pthread_t th; T(pthread_create(&th,NULL,kh_gfx_test_hold_then_signal,&hs)==0);
                u64 fr0=atomic_load(&n_bb_drain_free), dr1=n_bb_drains, t0=kh_host_ns(); T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0); u64 t1=kh_host_ns(); pthread_join(th,NULL);
                T(hs.changed && n_bb_drains==dr1+1 && atomic_load(&n_bb_drain_free)>=fr0+5 && t1-t0>=300000000ull && t1>=hs.cleared_ns && !kh_gfx_gate_is_set());
                kh_stuck_slice_ms=sl0; ID3D12Fence_Release(fw); } } }
      else { T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0); T(n_bb_images_made==im0 && n_bb_images_freed==if0); } }
    T((u32)GC(VT(sc,9),sc,1,GP(&IID_ID3D12Resource),m+88)==0); rt=rd64(g,m+88);
    if(rt){ u64 d=m+256; GC(VT(rt,10),rt,d); T(rd64(g,d+16)==128&&rd32(g,d+24)==72); GC(VT(rt,2),rt); rt=0; }
    /* D3D12SerializeRootSignature import -> vkd3d-utils blob -> CreateRootSignature */
    { D3D12_ROOT_PARAMETER rp; memset(&rp,0,sizeof rp); rp.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; rp.Descriptor.ShaderRegister=2; rp.ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX; memcpy(hp(g,m+512),&rp,sizeof rp);
      D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd,0,sizeof rd); rd.NumParameters=1; rd.pParameters=(const D3D12_ROOT_PARAMETER*)(uintptr_t)(m+512); rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT; memcpy(hp(g,m+448),&rd,sizeof rd);
      T((u32)GI(rt_imp_d3d12__D3D12SerializeRootSignature,m+448,1,m+136,0)==0); blob=rd64(g,m+136); T(blob!=0);
      if(blob){ u64 ptr=GC(VT(blob,3),blob), size=GC(VT(blob,4),blob); T(ptr!=0&&size>=24&&rd32(g,ptr)==0x43425844u /* "DXBC" */);
          T((u32)GC(VT(dev,16),dev,0,ptr,size,GP(&IID_ID3D12RootSignature),m+144)==0); rs=rd64(g,m+144); T(rs!=0); } }
    /* ---- TASK18: picture conversion, dump window, video stream ------------------------------------------------------------------ */
    { /* BGRA rows with a row pitch above 4*width -> RGB24; scale 2 = mean of each 2x2 block, rounded to nearest */
      u8 src[2*20]; memset(src,0xEE,sizeof src); static const u8 px[2][4][4]={{{10,20,30,255},{12,22,32,0},{100,110,120,9},{101,111,121,9}},{{14,24,34,7},{16,26,36,7},{103,113,123,9},{102,112,122,9}}};
      for(int y=0;y<2;y++) memcpy(src+20*y,px[y],16);
      u8 o1[24], o2[6]; video_convert(o1,src,20,4,2,1); video_convert(o2,src,20,4,2,2);
      T(o1[0]==30&&o1[1]==20&&o1[2]==10&&o1[3]==32&&o1[9]==121&&o1[10]==111&&o1[11]==101&&o1[12]==34&&o1[21]==122&&o1[23]==102);
      T(o2[0]==33&&o2[1]==23&&o2[2]==13&&o2[3]==122&&o2[4]==112&&o2[5]==102);
      /* picture hashes: the row padding is not hashed, alpha only in the BGRA hash, every colour byte in both; an odd width too */
      u8 s2[2*20]; memcpy(s2,src,sizeof src); u64 a1,b1,a2,b2; hash_pixels(src,20,4,2,&a1,&b1);
      s2[16]^=0xff; s2[39]^=0xff; hash_pixels(s2,20,4,2,&a2,&b2); T(a1==a2&&b1==b2);                 /* padding bytes */
      s2[7]^=0x80; hash_pixels(s2,20,4,2,&a2,&b2); T(a1==a2&&b1!=b2); s2[7]^=0x80;                   /* alpha of pixel (1,0) */
      s2[20+12+2]^=1; hash_pixels(s2,20,4,2,&a2,&b2); T(a1!=a2&&b1!=b2); s2[20+12+2]^=1;             /* red of pixel (3,1) */
      hash_pixels(src,20,3,2,&a1,&b1); s2[8]^=4; hash_pixels(s2,20,3,2,&a2,&b2); T(a1!=a2&&b1!=b2); s2[8]^=4;   /* width 3: the last pixel of a row */
      s2[12]^=4; hash_pixels(s2,20,3,2,&a2,&b2); T(a1==a2&&b1==b2); }                                 /* width 3: pixel 3 is outside the picture */                      /* (30+32+34+36+2)/4 = 33 ; (120+121+123+122+2)/4 = 122 (121.5 rounds up) */
    if(heap&&al&&cl){ u64 handle2; GC(VT(heap,9),heap,m+336); handle2=rd64(g,m+336);
      /* dump window: with KHNMC_GFX_DUMP_FROM..TO only the presents inside are written */
      snprintf(dump_dir,sizeof dump_dir,"%s",tmp); dump_every=1; gfx_presents=2000000; u64 sf=dump_from, stt=dump_to, d0=dumps_written; dump_from=2000002; dump_to=2000002;
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.25f,0.5f,0.75f)); T(dumps_written==d0);       /* present 2000001: before the window */
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.25f,0.5f,0.75f)); T(dumps_written==d0+1);     /* 2000002: inside */
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.25f,0.5f,0.75f)); T(dumps_written==d0+1);     /* 2000003: after */
      { char path[1200]; snprintf(path,sizeof path,"%s/frame-2000002.ppm",tmp); T(ppm_is(path,128,72,64,128,191)); unlink(path); snprintf(path,sizeof path,"%s/frame-2000001.ppm",tmp); T(access(path,F_OK)!=0);
        snprintf(path,sizeof path,"%s/frame-2000003.ppm",tmp); T(access(path,F_OK)!=0); snprintf(path,sizeof path,"%s/index.tsv",tmp); unlink(path); }
      dump_from=sf; dump_to=stt; dump_dir[0]=0;
      /* TASK36: the suspend gate is checked FIRST in Present: while it is set no frame readback reaches the queue (the dump count at the
       * moment the gate is cleared is still the old one), and the Present goes on once it is cleared */
      { snprintf(dump_dir,sizeof dump_dir,"%s",tmp); dump_every=1; gfx_presents=3000000; u64 d0=dumps_written, p0=kh_gfx_gate_parks();
        GateObs ob; memset(&ob,0,sizeof ob); ob.hold_ms=120; ob.d_at_clear=~0ull; kh_gfx_gate_set(); pthread_t th; T(pthread_create(&th,NULL,gate_obs_clear,&ob)==0);
        u64 t0=kh_host_ns(); T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.5f,0.25f,0.125f)); u64 dt=kh_host_ns()-t0; pthread_join(th,NULL);
        T(dt>=100000000ull && ob.d_at_clear==d0 && ob.p_at_clear==p0+1 && dumps_written==d0+1 && !kh_gfx_gate_is_set());
        char path[1200]; snprintf(path,sizeof path,"%s/frame-3000001.ppm",tmp); T(ppm_is(path,128,72,128,64,32)); unlink(path); snprintf(path,sizeof path,"%s/index.tsv",tmp); unlink(path); dump_dir[0]=0; }
      /* task 39: the frame readback's ExecuteCommandLists + Signal are an ADMISSION: the gate set from inside them holds quiesce until they
       * returned; the dump is still written */
      { snprintf(dump_dir,sizeof dump_dir,"%s",tmp); dump_every=1; gfx_presents=3500000; u64 d0=dumps_written;
        kh_gfx_gate_test_arm("Present (frame readback)",50); T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.5f,0.25f,0.125f)); T(kh_gfx_gate_test_result("Present (frame readback)") && dumps_written==d0+1);
        char path[1200]; snprintf(path,sizeof path,"%s/frame-3500001.ppm",tmp); T(ppm_is(path,128,72,128,64,32)); unlink(path); snprintf(path,sizeof path,"%s/index.tsv",tmp); unlink(path); dump_dir[0]=0; }
      /* TASK36: ID3D12Fence::Signal from the CPU (wrapped bridge): not held while no queue may be blocked; held (parked until the gate is
       * cleared, the fence unchanged meanwhile) when fence signals are held, and by itself once a queue Wait was seen */
      if(kh_gfx_wrap_on&&fence){ ID3D12Fence *rf=(ID3D12Fence*)real_of(g,fence,WK_FENCE,"selftest fence"); u64 v0=ID3D12Fence_GetCompletedValue(rf), p0=kh_gfx_gate_parks();
        kh_gfx_gate_test_reset_queue_waits(); kh_gfx_gate_set(); T((u32)GC(VT(fence,10),fence,v0+1)==0); T(ID3D12Fence_GetCompletedValue(rf)==v0+1 && kh_gfx_gate_parks()==p0); kh_gfx_gate_clear();
        for(int round=0;round<2;round++){
            if(round==0) kh_gfx_gate_hold_fence_signals(1); else { kh_gfx_gate_hold_fence_signals(0); T((u32)GC(VT(q,15),q,fence,v0+1+round)==0); }   /* round 1: a queue Wait (on a value already reached) */
            GateObs ob; memset(&ob,0,sizeof ob); ob.hold_ms=80; ob.fence=rf; kh_gfx_gate_set(); pthread_t th; T(pthread_create(&th,NULL,gate_obs_clear,&ob)==0);
            T((u32)GC(VT(fence,10),fence,v0+2+round)==0); pthread_join(th,NULL);
            T(ob.f_at_clear==v0+1+round && ID3D12Fence_GetCompletedValue(rf)==v0+2+round && kh_gfx_gate_parks()==p0+1+(u64)round); }
        kh_gfx_gate_hold_fence_signals(0); kh_gfx_gate_test_reset_queue_waits(); }
      /* video stream: one picture per VIRTUAL VBLANK from the stream's first vblank on; a vblank without a new Present repeats the
       * previous picture, two Presents inside one vblank keep the later; the stream ends after KHNMC_GFX_VIDEO_FRAMES pictures.
       * 128x72 back buffer, scale 2 -> 64x36 pictures. */
      char sv_path[sizeof video_path]; memcpy(sv_path,video_path,sizeof video_path); int sv_scale=video_scale, sv_ff=video_from_field, sv_exit=video_exit; u64 sv_from=video_from, sv_max=video_max;
      kh_vclock_test_begin(7000000000000ull); kh_vclock_on_vblank(video_vblank);
      snprintf(video_path,sizeof video_path,"%s/video.raw",tmp); video_scale=2; video_from_field=0; video_exit=0; video_from=2; video_max=3; video_first=0;
      vid_have=vid_fresh=vid_armed=vid_done=vid_cut=0; atomic_store(&vid_written,0); atomic_store(&vid_repeats,0); atomic_store(&vid_replaced,0); atomic_store(&vid_captures,0); atomic_store(&vid_before_first,0);
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,1.0f,0.0f,0.5f)); T(vid_armed==1&&video_first==2&&vid_captures==1);   /* picture A = (255,0,128) */
      kh_vclock_test_vblank(g); T(vid_written==0&&video_f==NULL);                                   /* vblank 1: before the stream's first vblank */
      kh_vclock_test_vblank(g); T(vid_written==1&&video_f!=NULL);                                   /* vblank 2: picture 0 = A */
      kh_vclock_test_vblank(g); T(vid_written==2&&vid_repeats==1);                                  /* vblank 3: nothing new presented: A again */
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.5f,0.5f,0.5f)); T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,0.0f,1.0f,0.25f)); T(vid_replaced==1&&vid_captures==3);   /* B, then C = (0,255,64) inside the same vblank */
      kh_vclock_test_vblank(g); T(vid_written==3&&vid_done==1&&video_f==NULL&&vid_repeats==1);      /* vblank 4: picture 2 = C, the stream is complete and closed */
      T(st_clear_present(g,m,dev,q,sc,al,cl,handle2,1.0f,1.0f,1.0f)); T(vid_captures==3); kh_vclock_test_vblank(g); T(vid_written==3);   /* after the end: no capture, nothing written */
      { FILE *vf=fopen(video_path,"rb"); T(vf!=NULL); size_t one=64u*36u*3u; u8 *b=malloc(3*one+16); size_t got=vf&&b?fread(b,1,3*one+16,vf):0; if(vf) fclose(vf);
        T(got==3*one); if(got==3*one){ T(raw_is(b,one,255,0,128)); T(raw_is(b+one,one,255,0,128)); T(raw_is(b+2*one,one,0,255,64)); } free(b);
        char ip[1200]; snprintf(ip,sizeof ip,"%s.tsv",video_path); FILE *ix=fopen(ip,"r"); T(ix!=NULL);
        if(ix){ char ln[256]; unsigned long long pic,vb,vns,pr,gf; int nw; long long as; int rows=0, ok=1;
            if(!fgets(ln,sizeof ln,ix)||strncmp(ln,"picture\tvblank\t",15)) ok=0;
            while(fgets(ln,sizeof ln,ix)){ if(sscanf(ln,"%llu %llu %llu %llu %llu %d %lld",&pic,&vb,&vns,&pr,&gf,&nw,&as)!=7){ ok=0; break; }
                if(pic!=(unsigned)rows||vb!=(unsigned)rows+2||vns!=7000000000000ull+vb*1000000000ull/60ull||nw!=(rows==1?0:1)) ok=0; rows++; }
            fclose(ix); T(ok&&rows==3); unlink(ip); }
        unlink(video_path); }
      kh_vclock_test_end();
      memcpy(video_path,sv_path,sizeof video_path); video_scale=sv_scale; video_from_field=sv_ff; video_exit=sv_exit; video_from=sv_from; video_max=sv_max; video_first=0;
      vid_have=vid_fresh=vid_armed=vid_done=vid_cut=0; atomic_store(&vid_written,0); atomic_store(&vid_repeats,0); atomic_store(&vid_replaced,0); atomic_store(&vid_captures,0);
      pthread_mutex_lock(&vid_m); free(vid_buf); vid_buf=NULL; vid_w=vid_h=0; pthread_mutex_unlock(&vid_m); }
done:
    if(rt2) GC(VT(rt2,2),rt2); if(psoT) GC(VT(psoT,2),psoT); if(rsT) GC(VT(rsT,2),rsT); if(blobT) GC(VT(blobT,2),blobT); if(srvh) GC(VT(srvh,2),srvh); if(samph) GC(VT(samph,2),samph); if(ptb) ID3D10Blob_Release(ptb);
    if(bl) GC(VT(bl,2),bl); if(ba) GC(VT(ba,2),ba); if(rt1) GC(VT(rt1,2),rt1); if(vb) GC(VT(vb,2),vb); if(pso) GC(VT(pso,2),pso); if(rs0) GC(VT(rs0,2),rs0); if(blob0) GC(VT(blob0,2),blob0);
    if(vsb) ID3D10Blob_Release(vsb); if(psb) ID3D10Blob_Release(psb); if(errb) ID3D10Blob_Release(errb);
    if(rs) GC(VT(rs,2),rs); if(blob) GC(VT(blob,2),blob); if(fence) GC(VT(fence,2),fence); if(cl) GC(VT(cl,2),cl); if(al) GC(VT(al,2),al); if(heap) GC(VT(heap,2),heap); if(rt) GC(VT(rt,2),rt);
    memcpy(dump_dir,save_dir,sizeof dump_dir); dump_every=save_every; gfx_presents=save_presents; dumps_written=save_dumps;
    if(rmdir(tmp)){ st_fails++; fprintf(stderr,"vkd3d selftest FAIL: %s is not empty after the test (%s)\n",tmp,strerror(errno)); } ga_free(m);
    if(kh_gfx_wrap_on) T(kh_native_calls==nat0);                                  /* the wrapped bridge: not one call through the Windows-ABI trampoline */
    st_fails+=kh_gfxc_st_fails;
    fprintf(stderr,"kh: vkd3d selftest: %d checks, %d failures (device %s, %s bridge, %s addresses)\n",st_checks,st_fails,vk_dev_name,kh_gfx_wrap_on?"wrapped":"trampoline",KH_XLATE_MODE==0?"identity":"bias");
    /* TASK36 step 7: the presenter on the headless surface (its own suite line; SKIPPED when the extensions are missing) */
    if(dev&&q&&fct){ char sv[sizeof dump_dir]; memcpy(sv,dump_dir,sizeof sv); dump_dir[0]=0; st_fails+=kh_present_selftest(g,0,dev,q,fct); memcpy(dump_dir,sv,sizeof sv); }
    else fprintf(stderr,"kh: gfx present selftest: SKIPPED (the device selftest stopped before its swap chain existed)\n");
    if(kh_gfx_wrap_on) st_fails+=kh_wrap_selftest_device(g);                     /* wrapper identity, reference counts, by-value results, Map round trip */
    if(kh_gfx_wrap_on){ extern int kh_wrap_selftest_suspend(KCtx*); st_fails+=kh_wrap_selftest_suspend(g); }   /* TASK41: step 11 with real queues */
    if(dev&&q&&fct) st_fails+=st_pipecache_device(g,dev,q,fct);                  /* TASK43: step 12 with a real driver */
    else fprintf(stderr,"kh: gfx pipecache device selftest: SKIPPED (the device selftest stopped before its device, queue and factory existed)\n");
    return st_fails;
}
#endif
