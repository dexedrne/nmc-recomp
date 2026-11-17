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
#define COBJMACROS
#define INITGUID
#ifndef WIDL_C_INLINE_WRAPPERS
#define WIDL_C_INLINE_WRAPPERS
#endif
#include <vkd3d_windows.h>
#include <vkd3d_d3d12.h>
#include <vkd3d.h>
#include <vkd3d_utils.h>
#endif
#include "khnmc_rt.h"
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
                  "Rebuild with GFX_VKD3D=1 (the Makefile enables it on Linux when $(VKD3D_PREFIX)/lib/libvkd3d.so exists)");
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
#else
/* ---- backend ------------------------------------------------------------------------------------------------------------- */
#include "gfx_vkd3d_int.h"
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
static char dump_dir[1024]; static u64 dump_every=300; static _Atomic u64 dumps_written; static u64 dump_from, dump_to=~0ull;
static FILE *hash_f; static char hash_path[1024]; static _Atomic u64 hashes_written;
static char vk_dev_name[300]="(no Vulkan device created yet)";
static struct vkd3d_instance *vk_inst; static pthread_mutex_t vm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 n_devices, n_probe_devices, n_events_signalled, n_bundles_made, n_bundles_run, n_bundle_cmds, n_bundles_live, n_default_tables;
static int C_balloc, C_bundle;
static int h_balloc(KCtx *g,u64 o,int s,const char *c); static int h_bundle(KCtx *g,u64 o,int s,const char *c);
#define P(x) ((unsigned long long)(x))
#define U(x) ((u64)(uintptr_t)(x))

/* ---- start-up: driver selection, native ranges, dump configuration --------------------------------------------------------- */
static void driver_setup(void){
    const char *d=getenv("KHNMC_GFX_DRIVER");
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
    e=getenv("KHNMC_GFX_HASHLOG");
    if(e&&*e){ if(snprintf(hash_path,sizeof hash_path,"%s",e)>=(int)sizeof hash_path) kh_fatal(NULL,"gfx-hashlog: KHNMC_GFX_HASHLOG is too long");
        hash_f=fopen(hash_path,"w"); if(!hash_f) kh_fatal(NULL,"gfx-hashlog: cannot create %s (%s)",hash_path,strerror(errno));
        setvbuf(hash_f,NULL,_IOLBF,0); fprintf(hash_f,"present\tgame_frame\tvblank\trgb_hash\tbgra_hash\tdraws_total\twidth\theight\n"); }
    video_setup();
    e=getenv("KHNMC_GFX_TRACE"); kh_native_trace=e&&*e=='1';
    kh_fiber_host_stack=16ull<<20;          /* vkd3d and the Vulkan driver run on the calling fiber's host stack */
    C_balloc=kh_com_class("vkd3d:BundleAllocator",9,COM_STRICT,h_balloc); C_bundle=kh_com_class("vkd3d:BundleCommandList",60,COM_STRICT,h_bundle);
    kh_gfx_vkd3d_on=1;
    fprintf(stderr,"kh: gfx backend: %s; address model: %s; frame dump: %s%s every %llu presents",label,KH_XLATE_MODE==0?"identity":"bias",dump_dir[0]?"to ":"off",dump_dir,P(dump_every));
    if(dump_from||dump_to!=~0ull) fprintf(stderr,", presents %llu..%llu only",P(dump_from),P(dump_to));
    if(hash_f) fprintf(stderr,"; picture hash of every present: %s",hash_path);
    if(video_path[0]) fprintf(stderr,"; video stream: %s, %llu pictures from %s%s%llu, scale 1/%d%s",video_path,P(video_max),video_from_field?"the field":"virtual vblank ",video_from_field?" + ":"",P(video_from_field?video_lead:video_from),video_scale,video_exit?", then exit":"");
    fputc('\n',stderr);
}

/* ---- vkd3d instance --------------------------------------------------------------------------------------------------------- */
static HRESULT sig_event(HANDLE h){                                           /* called by vkd3d's fence worker thread */
    KWaitObj *w=h_get(U(h),HK_EVENT);
    if(!w) kh_fatal(NULL,"gfx-fence-event-handle: a fence completed and wants to signal handle %p, which is not an event of this runtime",h);
    pthread_mutex_lock(&w->m); w->signaled=1; pthread_cond_broadcast(&w->cv); pthread_mutex_unlock(&w->m);
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
static PFN_vkGetInstanceProcAddr vk_gipa; static const char *vk_lib_name;
/* caller holds vm */
static HRESULT instance_get(void){
    if(vk_inst) return S_OK;
    if(!vk_gipa){ char why[512]; why[0]=0; void *lib=NULL;
        for(size_t i=0;!lib&&i<sizeof vk_lib_names/sizeof*vk_lib_names;i++){ lib=dlopen(vk_lib_names[i],RTLD_NOW|RTLD_LOCAL);
            if(lib) vk_lib_name=vk_lib_names[i]; else { const char *e=dlerror(); size_t l=strlen(why); snprintf(why+l,sizeof why-l,"%s%s",l?"; ":"",e?e:vk_lib_names[i]); } }
        if(!lib) kh_fatal(NULL,"gfx-vulkan-loader: no Vulkan library of this OS can be loaded (%s)",why);
        vk_gipa=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
        if(!vk_gipa) kh_fatal(NULL,"gfx-vulkan-loader: %s has no vkGetInstanceProcAddr",vk_lib_name); }
    struct vkd3d_application_info app; memset(&app,0,sizeof app); app.type=VKD3D_STRUCTURE_TYPE_APPLICATION_INFO; app.application_name="khnmc"; app.api_version=VKD3D_API_VERSION_2_1;
    struct vkd3d_instance_create_info ici; memset(&ici,0,sizeof ici); ici.type=VKD3D_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.next=&app; ici.pfn_signal_event=sig_event; ici.wchar_size=2;
    ici.pfn_vkGetInstanceProcAddr=vk_gipa;
    HRESULT hr=vkd3d_create_instance(&ici,&vk_inst);
    if(FAILED(hr)){ vk_inst=NULL; fprintf(stderr,"kh: gfx vkd3d: vkd3d_create_instance FAILED hr=0x%08x (Vulkan library %s, VK_DRIVER_FILES=%s)\n",(unsigned)hr,vk_lib_name,getenv("VK_DRIVER_FILES")?getenv("VK_DRIVER_FILES"):"(unset)"); return hr; }
    /* every physical device the instance can see must be a CPU implementation unless the system driver was asked for; the two functions
     * come from the instance vkd3d created, through the library vkd3d uses */
    VkInstance vi=vkd3d_instance_get_vk_instance(vk_inst);
    PFN_vkEnumeratePhysicalDevices enum_pd=(PFN_vkEnumeratePhysicalDevices)vk_gipa(vi,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props=(PFN_vkGetPhysicalDeviceProperties)vk_gipa(vi,"vkGetPhysicalDeviceProperties");
    if(!enum_pd||!props) kh_fatal(NULL,"gfx-vulkan-loader: %s gives no vkEnumeratePhysicalDevices / vkGetPhysicalDeviceProperties for the instance",vk_lib_name);
    fprintf(stderr,"kh: gfx vkd3d: Vulkan library %s (handed to vkd3d as its vkGetInstanceProcAddr)\n",vk_lib_name);
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
typedef struct Bundle { u64 obj; int open; BCmd *cmd; u32 n,cap; u8 *blob; u32 bn,bcap; } Bundle;
#define B_PTR 32                               /* guest bundle object: host Bundle* */
#define NOBLOB 0xffffffffu
static Bundle *bundle_of(KCtx *g,u64 o){ return (Bundle*)(uintptr_t)rd64(g,o+B_PTR); }
static void bundle_rec(KCtx *g,Bundle *b,int slot,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4,u64 ptr,u32 len){
    if(!b->open) kh_fatal(g,"gfx-bundle-closed: ID3D12GraphicsCommandList method slot %d called on a bundle that is closed (no Reset since Close)",slot);
    if(b->n==b->cap){ b->cap=b->cap?b->cap*2:32; b->cmd=realloc(b->cmd,sizeof(BCmd)*b->cap); if(!b->cmd) kh_fatal(g,"gfx-bundle: out of memory"); }
    BCmd *c=&b->cmd[b->n++]; c->slot=(u8)slot; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3; c->a[4]=a4; c->blob=NOBLOB; c->blen=len;
    if(ptr&&len){ u32 need=(len+15)&~15u;
        if(b->bn+need>b->bcap){ b->bcap=(b->bn+need)*2+256; b->blob=realloc(b->blob,b->bcap); if(!b->blob) kh_fatal(g,"gfx-bundle: out of memory"); }
        memcpy(b->blob+b->bn,hp(g,ptr),len); c->blob=b->bn; b->bn+=need; }
}
/* what a bundle records for an interface argument: the REAL object (the wrapped bridge's game passes the guest object that wraps it) */
static u64 b_real(KCtx *g,u64 shown,int kind,const char *what){ return shown?U(real_of(g,shown,kind,what)):0; }
static int h_balloc(KCtx *g,u64 o,int s,const char *c){ (void)c;
    if(s>=3&&s<=6){ SET_RAX(g,0); return 1; }
    if(s==2){ u64 r=rd64(g,o+16); if(r) r--; wr64(g,o+16,r); SET_RAX(g,r); if(!r){ wr64(g,o,0); ga_free(o); } return 1; }
    if(s==8){ SET_RAX(g,0); return 1; }                                         /* Reset */
    return 0; }
static int h_bundle(KCtx *g,u64 o,int s,const char *c){ (void)c; Bundle *b=bundle_of(g,o);
    switch(s){
    case 2: { u64 r=rd64(g,o+16); if(r) r--; wr64(g,o+16,r); SET_RAX(g,r);
              if(!r){ free(b->cmd); free(b->blob); free(b); wr64(g,o,0); wr64(g,o+B_PTR,0); ga_free(o); atomic_fetch_sub(&n_bundles_live,1); } return 1; }   /* Release: the recording dies with the last reference */
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
u64 kh_gfxc_bundle_new(KCtx *g,u64 al,u64 pso){
    if(!GA_OK(al)||rd64(g,al)!=kh_com_vtbl(C_balloc)) kh_fatal(g,"gfx-bundle-allocator: CreateCommandList(BUNDLE) with allocator 0x%llx, which is not a bundle allocator",P(al));
    u64 o=kh_com_obj(C_bundle,16); Bundle *b=calloc(1,sizeof *b); if(!o||!b) kh_fatal(g,"gfx-bundle: out of memory");
    b->obj=o; b->open=1; wr64(g,o+B_PTR,U(b)); if(pso) bundle_rec(g,b,25,b_real(g,pso,WK_PSO,"CreateCommandList(BUNDLE): initial pipeline state"),0,0,0,0,0,0);
    atomic_fetch_add(&n_bundles_made,1); atomic_fetch_add(&n_bundles_live,1);
    return o;
}
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
static _Atomic u64 c_res_total, c_res_buffers, c_res_sampled, c_bar_transition, c_bar_aliasing, c_bar_uav, c_resolves, c_msaa_resources;
void kh_gfxc_census_resource(const D3D12_RESOURCE_DESC *d){
    if(!d) return; D3D12_RESOURCE_DESC c; memcpy(&c,d,sizeof c);                  /* the caller's pointer may be guest memory: no alignment assumed */
    u32 dim=c.Dimension, fmt=c.Format, samples=c.SampleDesc.Count, flags=c.Flags, w=(u32)c.Width, h=c.Height, mips=c.MipLevels;
    atomic_fetch_add(&c_res_total,1);
    if(dim==1){ atomic_fetch_add(&c_res_buffers,1); return; }
    if(samples>1) atomic_fetch_add(&c_msaa_resources,1);
    if(!(flags&3)&&samples<=1){ atomic_fetch_add(&c_res_sampled,1); return; }
    pthread_mutex_lock(&cm); int i; for(i=0;i<n_res_kinds;i++){ ResKind *k=&res_kinds[i]; if(k->dim==dim&&k->fmt==fmt&&k->samples==samples&&k->flags==flags&&k->w==w&&k->h==h&&k->mips==mips) break; }
    if(i==n_res_kinds&&n_res_kinds<48){ res_kinds[i]=(ResKind){dim,fmt,samples,flags,w,h,mips,0}; n_res_kinds++; }
    if(i<48) res_kinds[i].n++; pthread_mutex_unlock(&cm);
}
void kh_gfxc_census_resolve(void){ atomic_fetch_add(&c_resolves,1); }
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
    if(n_res_kinds){ fprintf(f,"kh: gfx vkd3d census: render-target / depth-stencil / multisampled textures (dimension format WxH mips samples flags = count):");
        for(int i=0;i<n_res_kinds;i++){ ResKind *k=&res_kinds[i]; fprintf(f," [%uD fmt%u %ux%u m%u s%u fl0x%x = %llu]",k->dim-1,k->fmt,k->w,k->h,k->mips,k->samples,k->flags,P(k->n)); } fputc('\n',f); }
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
/* the vkd3d instance (made once) and a NEW device of at least `level` on it.  S_OK and *out, or the failing HRESULT (reported here) */
HRESULT kh_gfxc_device_new(KCtx *g,u32 level,u64 adapter_shown,ID3D12Device **out){
    (void)g; *out=NULL;
    pthread_mutex_lock(&vm); HRESULT hr=instance_get(); pthread_mutex_unlock(&vm);
    if(FAILED(hr)) return hr;
    struct vkd3d_device_create_info dci; memset(&dci,0,sizeof dci); dci.type=VKD3D_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.minimum_feature_level=(D3D_FEATURE_LEVEL)level; dci.instance=vk_inst;
    ID3D12Device *dev=NULL; hr=vkd3d_create_device(&dci,&IID_ID3D12Device,(void**)&dev);
    if(FAILED(hr)||!dev){
        fprintf(stderr,"kh: gfx vkd3d: D3D12CreateDevice(adapter 0x%llx [ours, not passed on], feature level 0x%x) FAILED: vkd3d_create_device hr=0x%08x\n",P(adapter_shown),level,(unsigned)hr);
        return FAILED(hr)?hr:E_FAIL; }
    *out=dev; return S_OK;
}
/* D3D12CreateDevice(adapter, featureLevel, riid, **ppDevice): the adapter is OUR DXGI object and is not passed on.  The game gets the host
 * device itself (trampoline bridge) or the guest object that wraps it (wrapped bridge). */
void kh_vkd3d_create_device(KCtx *g){
    u32 level=(u32)A1(g); u64 riid=A2(g), pp=A3(g);
    ID3D12Device *dev=NULL; HRESULT hr=kh_gfxc_device_new(g,level,A0(g),&dev);
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
typedef struct Swap { u64 obj; ID3D12CommandQueue *queue; ID3D12Device *dev; ID3D12Resource *buf[NBUF]; u32 w,h;
    ID3D12CommandAllocator *alloc; ID3D12GraphicsCommandList *list; ID3D12Fence *fence; u64 fence_val;
    ID3D12Resource *rb; u64 rb_size; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; u8 *seen; } Swap;
static Swap *swap_of(KCtx *g,u64 o,const char *what){ Swap *s=(Swap*)(uintptr_t)rd64(g,o+SW_PTR); if(!s||s->obj!=o) kh_fatal(g,"gfx-swapchain-state: %s on swap chain 0x%llx that has no vkd3d back buffers",what,P(o)); return s; }
#define HRCHK(expr,what) do{ HRESULT hr_=(expr); if(FAILED(hr_)) kh_fatal(g,"gfx-vkd3d-call: %s failed, hr=0x%08x (swap chain %ux%u)",what,(unsigned)hr_,s->w,s->h); }while(0)

static void buffers_make(KCtx *g,Swap *s,u32 w,u32 h){
    s->w=w; s->h=h;
    D3D12_HEAP_PROPERTIES def; memset(&def,0,sizeof def); def.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd; memset(&rd,0,sizeof rd); rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=w; rd.Height=h; rd.DepthOrArraySize=1; rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count=1; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(int i=0;i<NBUF;i++){ HRCHK(ID3D12Device_CreateCommittedResource(s->dev,&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_PRESENT,NULL,&IID_ID3D12Resource,(void**)&s->buf[i]),"CreateCommittedResource(B8G8R8A8 back buffer, ALLOW_RENDER_TARGET, state PRESENT)"); }
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
    buffers_make(g,s,s->w,s->h);
    HRCHK(ID3D12Device_CreateCommandAllocator(s->dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&s->alloc),"CreateCommandAllocator(frame dump)");
    HRCHK(ID3D12Device_CreateCommandList(s->dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,s->alloc,NULL,&IID_ID3D12GraphicsCommandList,(void**)&s->list),"CreateCommandList(frame dump)");
    HRCHK(ID3D12GraphicsCommandList_Close(s->list),"ID3D12GraphicsCommandList::Close(frame dump)");
    HRCHK(ID3D12Device_CreateFence(s->dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&s->fence),"CreateFence(frame dump)");
    wr64(g,sc+SW_PTR,U(s));
    fprintf(stderr,"kh: gfx vkd3d: swap chain 0x%llx: %d x %ux%u B8G8R8A8 back buffers on device %p (queue %p), readback %llu bytes (row pitch %u)\n",P(sc),NBUF,s->w,s->h,(void*)s->dev,(void*)s->queue,P(s->rb_size),s->fp.Footprint.RowPitch);
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
    buffers_make(g,s,w,h);
    fprintf(stderr,"kh: gfx vkd3d: swap chain 0x%llx resized to %ux%u\n",P(o),w,h);
    SET_RAX(g,0);
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
/* copy back buffer idx to the readback buffer and map it; the caller unmaps with readback_done */
static const u8 *readback(KCtx *g,Swap *s,u32 idx,u64 n){
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
    ID3D12CommandQueue_ExecuteCommandLists(s->queue,1,lists);
    u64 want=++s->fence_val;
    HRCHK(ID3D12CommandQueue_Signal(s->queue,s->fence,want),"ID3D12CommandQueue::Signal(frame readback)");
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
    int want_dump=dump_dir[0]&&n>=dump_from&&n<=dump_to&&n%dump_every==0, want_video=video_wanted(), want_hash=hash_f!=NULL;
    if(!want_dump&&!want_video&&!want_hash) return;
    u64 t0=kh_host_ns(); u32 idx=rd32(g,o+40)%NBUF; const u8 *base=readback(g,s,idx,n);
    if(want_hash) hash_frame(g,s,base,n);
    if(want_video) video_capture(g,s,base,n);
    if(want_dump) dump_frame(g,s,base,idx,n,t0);
    readback_done(s);
}
/* process exit (loader.c, before the reports): an open video stream is closed here and nowhere else; the report says if it was cut short */
void kh_gfx_vkd3d_finish(void){
    if(hash_f){ if(fclose(hash_f)) fprintf(stderr,"kh: gfx hash log: ERROR: closing %s failed (%s)\n",hash_path,strerror(errno)); hash_f=NULL; }
    if(!video_path[0]) return; pthread_mutex_lock(&vid_m); if(video_f){ vid_cut=!vid_done; video_close(); } else if(!vid_done) vid_cut=vid_armed; vid_done=1; pthread_mutex_unlock(&vid_m); }
void kh_gfx_vkd3d_report(FILE *f){
    fprintf(f,"kh: gfx vkd3d: bridge=%s device=%s driver-mode=%s devices-created=%llu (+%llu probes) native-calls=%llu fence-events-signalled=%llu bundles: recorded=%llu executed=%llu (%llu replayed calls) live=%llu; default-descriptor-tables=%llu on %d tracked command lists; frame-dumps=%llu%s%s\n",kh_gfx_wrap_on?"wrapped":"trampoline",vk_dev_name,drv_system?"system":"lavapipe",
            P(n_devices),P(n_probe_devices),P(kh_native_calls),P(n_events_signalled),P(n_bundles_made),P(n_bundles_run),P(n_bundle_cmds),P(n_bundles_live),P(n_default_tables),nlst,P(dumps_written),dump_dir[0]?" in ":"",dump_dir);
    if(hash_path[0]) fprintf(f,"kh: gfx hash log: %llu presents hashed into %s\n",P(hashes_written),hash_path);
    census_report(f);
    if(kh_gfx_wrap_on) kh_wrap_report(f);
    if(video_path[0]){
        pthread_mutex_lock(&vid_m); int open=video_f!=NULL||vid_cut; pthread_mutex_unlock(&vid_m);
        fprintf(f,"kh: gfx video: %s: %llu of %llu pictures written%s (rgb24 %ux%u, scale 1/%d, 60 per second of game time; picture 0 = virtual vblank %llu); repeats=%llu replaced-presents=%llu captures=%llu vblanks-before-the-first-picture=%llu%s\n",video_path,P(vid_written),P(video_max),
                vid_done?" (complete)":vid_cut?" (INCOMPLETE: the run ended first)":open?" (open)":vid_armed?" (armed, no picture written yet)":" (not started: its first vblank has not been reached)",vid_w,vid_h,video_scale,P(video_first),P(vid_repeats),P(vid_replaced),P(vid_captures),P(vid_before_first),
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
int kh_gfx_vkd3d_selftest(KCtx *g){
    extern void rt_imp_d3d12__ord101(KCtx*), rt_imp_d3d12__D3D12SerializeRootSignature(KCtx*), rt_imp_dxgi__CreateDXGIFactory1(KCtx*);
    st_checks=st_fails=0; kh_gfxc_st_fails=0; u64 nat0=kh_native_calls;
    u64 m=ga_calloc(1,2048), dev=0,q=0,fct=0,sc=0,rt=0,heap=0,al=0,cl=0,fence=0,blob=0,rs=0,rt1=0,vb=0,pso=0,rs0=0,blob0=0,ba=0,bl=0,rt2=0,rsT=0,blobT=0,psoT=0,srvh=0,samph=0;
    ID3DBlob *vsb=NULL,*psb=NULL,*errb=NULL,*ptb=NULL;
    char save_dir[sizeof dump_dir]; memcpy(save_dir,dump_dir,sizeof dump_dir); u64 save_every=dump_every, save_presents=gfx_presents, save_dumps=dumps_written;
    char tmp[]="/tmp/khnmc-vkd3d-selftest-XXXXXX"; if(!mkdtemp(tmp)){ fprintf(stderr,"vkd3d selftest FAIL: mkdtemp\n"); return 1; }
    /* D3D12CreateDevice: probe form (ppDevice NULL -> S_FALSE), then a real device; its methods are native addresses */
    T((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),0)==1);
    T((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),m)==0); dev=rd64(g,m); T(dev!=0);
    if(!dev) goto done;
    if(kh_gfx_wrap_on){ T(kh_native_range_name(VT(dev,0),NULL)==NULL); T(kh_lookup(VT(dev,0))!=NULL); }      /* wrapped: a host-function slot of a guest-side vtable */
    else              { T(kh_native_range_name(VT(dev,0),NULL)!=NULL); T(kh_lookup(VT(dev,0))==NULL); }      /* trampoline: native vkd3d code */
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
    T((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0);
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
    if(kh_gfx_wrap_on) st_fails+=kh_wrap_selftest_device(g);                     /* wrapper identity, reference counts, by-value results, Map round trip */
    return st_fails;
}
#endif
