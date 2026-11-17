/* gfx_wrap.c : KHNMC_GFX=vkd3dw, the WRAPPED graphics bridge (TASK21).
 *
 * The trampoline bridge (gfx_vkd3d.c, KHNMC_GFX=vkd3d) hands HOST vkd3d objects to the lifted code and forwards every vtable call through
 * one Windows-ABI trampoline.  That needs guest pointer == host pointer and x86-64.  Here the lifted code never sees a host object:
 *
 *  - Every D3D12 object the game touches is a GUEST-side COM object (com.c: allocated in the guest arena, KH_HOSTFN vtable) that holds
 *    the real vkd3d interface pointer.  ONE guest object per real object: QueryInterface, GetDevice and IDXGISwapChain::GetBuffer give
 *    back the guest object the game already has (table real -> guest object).  The guest object has no reference count of its own:
 *    AddRef / Release go to the real object and return ITS count; when the real count reaches 0 the guest object is unlinked, its
 *    vtable pointer is cleared (a use after the last Release faults instead of working by accident) and its memory is freed.
 *  - Every method decodes its arguments from the KCtx (Win64: rcx, rdx, r8, r9, [rsp+8+8*i]; floats in xmm0..3 by position; a struct
 *    result comes back through the hidden pointer in rdx and rax = that pointer), translates every guest pointer (XL: host = guest +
 *    bias), unwraps interface arguments, and calls the real method through vkd3d's C API (COBJMACROS): the compiler emits whatever ABI
 *    vkd3d's own headers declare for the host (ms_abi on x86-64, the platform ABI elsewhere: vkd3d_windows.h).  No trampoline, no
 *    assembly.  A method without a wrapper is the named fatal gfx-wrap-unwrapped-method.
 *  - A HOST pointer the game must write through (ID3D12Resource::Map, ID3DBlob::GetBufferPointer) is handed out as `host - bias`: the
 *    guest address at which XL() yields exactly that host address (XL is an addition modulo 2^64, so this is exact for any address).
 *  - Values that are opaque to the game stay as they are: CPU / GPU descriptor handles (the game only adds multiples of the increment),
 *    GPU virtual addresses, fence values, event HANDLEs (ours: kernel32 handle-table ids).
 *  - Bundles, the default root descriptor tables, the swap chain, frame dumps, the census: the code of gfx_vkd3d.c, shared
 *    (gfx_vkd3d_int.h).  A bundle records REAL interface pointers (unwrapped at record time), so the replay is the same in both bridges.
 *
 * STRUCTURES OF THE WRAPPED METHODS THAT EMBED A POINTER OR AN INTERFACE POINTER, and how each is rebuilt on the host side (a private
 * copy in per-call scratch memory; the game's structure is never modified):
 *   D3D12_GRAPHICS_PIPELINE_STATE_DESC  (CreateGraphicsPipelineState)      x_pso
 *        pRootSignature                  interface -> real object
 *        VS / PS / DS / HS / GS .pShaderBytecode                           guest pointer -> host pointer (bytecode read in place)
 *        StreamOutput.pSODeclaration     array copied; each .SemanticName string pointer translated
 *        StreamOutput.pBufferStrides     pointer translated
 *        InputLayout.pInputElementDescs  array copied; each .SemanticName string pointer translated
 *        CachedPSO.pCachedBlob           pointer translated
 *   D3D12_RESOURCE_BARRIER[]            (ResourceBarrier)                  x_barriers
 *        Transition.pResource / Aliasing.pResourceBefore, pResourceAfter / UAV.pResource   interface -> real (by .Type; NULL stays NULL)
 *   D3D12_TEXTURE_COPY_LOCATION         (CopyTextureRegion, dst and src)   x_copyloc      pResource interface -> real
 *   D3D12_ROOT_SIGNATURE_DESC           (D3D12SerializeRootSignature)      x_rootsig
 *        pParameters                     array copied; DESCRIPTOR_TABLE parameters: .pDescriptorRanges array copied
 *        pStaticSamplers                 array copied
 *   ID3D12CommandList *[]               (ExecuteCommandLists)              x_ifaces       each interface -> real
 *   ID3D12DescriptorHeap *[]            (SetDescriptorHeaps, also when recorded in a bundle)  x_ifaces
 *   D3D12_FEATURE_DATA_FEATURE_LEVELS   (CheckFeatureSupport, feature 2)   x_feature_levels   pFeatureLevelsRequested array copied
 *   root-signature blob                 (CreateRootSignature)              pointer translated, read in place
 * Pointer arguments to plain data (no pointer inside; copied to aligned host storage or translated): D3D12_COMMAND_QUEUE_DESC,
 * D3D12_DESCRIPTOR_HEAP_DESC, D3D12_HEAP_PROPERTIES, D3D12_RESOURCE_DESC, D3D12_CLEAR_VALUE, D3D12_HEAP_DESC, D3D12_QUERY_HEAP_DESC,
 * the five view descs, D3D12_SAMPLER_DESC, D3D12_VIEWPORT[], D3D12_RECT[], D3D12_VERTEX_BUFFER_VIEW[], D3D12_INDEX_BUFFER_VIEW,
 * FLOAT[4] (blend factor, clear colour), D3D12_CPU_DESCRIPTOR_HANDLE[] (OMSetRenderTargets, CopyDescriptors), UINT[], D3D12_BOX,
 * D3D12_RANGE, IID / GUID, 16-bit names (SetName), private data, the pointer-free CheckFeatureSupport structures.
 * Results: interface out-pointers (wrapped), Map's data pointer (host - bias), GetCopyableFootprints' four output arrays, the two
 * clock values, and by hidden pointer D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_RESOURCE_DESC,
 * D3D12_DESCRIPTOR_HEAP_DESC, D3D12_RESOURCE_ALLOCATION_INFO, D3D12_HEAP_PROPERTIES, D3D12_HEAP_DESC, D3D12_COMMAND_QUEUE_DESC.
 * NOT translated, each a named fatal: SECURITY_ATTRIBUTES (CreateSharedHandle with attributes: vkd3d declares the type without a
 * definition), an interface stored with SetPrivateDataInterface, a version 1.1 root-signature desc, CheckFeatureSupport structures
 * outside the list in w_dev, and every method not listed in the tables below (render passes, compute, tiles, residency ...).
 *
 * Compiled out (no KH_GFX_VKD3D): only the skipped selftest remains. */
#define _GNU_SOURCE
#ifdef KH_GFX_VKD3D
#define COBJMACROS
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
#include <sys/wait.h>

#ifndef KH_GFX_VKD3D
int kh_wrap_selftest_pure(KCtx *g){ (void)g; fprintf(stderr,"kh: gfx wrap selftest: SKIPPED (the vkd3d backend is not compiled into this binary)\n"); return 0; }
#else
#include "gfx_vkd3d_int.h"

#define P(x) ((unsigned long long)(x))
#define U(x) ((u64)(uintptr_t)(x))
_Static_assert(sizeof(void*)==8&&sizeof(SIZE_T)==8,"the wrapped bridge assumes a 64-bit host: guest structures hold 8-byte pointers");
/* the guest structures are the Windows x64 ones; the host's must have the same layout (they do on every LP64 host: fixed-size fields) */
_Static_assert(sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC)==656&&sizeof(D3D12_INPUT_ELEMENT_DESC)==32&&sizeof(D3D12_SO_DECLARATION_ENTRY)==24,"pipeline-state desc layout");
_Static_assert(sizeof(D3D12_RESOURCE_DESC)==56&&sizeof(D3D12_RESOURCE_BARRIER)==32&&sizeof(D3D12_TEXTURE_COPY_LOCATION)==48&&sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT)==32,"resource structure layout");
_Static_assert(sizeof(D3D12_ROOT_SIGNATURE_DESC)==40&&sizeof(D3D12_ROOT_PARAMETER)==32&&sizeof(D3D12_DESCRIPTOR_RANGE)==20&&sizeof(D3D12_STATIC_SAMPLER_DESC)==52,"root-signature desc layout");
_Static_assert(sizeof(D3D12_VIEWPORT)==24&&sizeof(D3D12_RECT)==16&&sizeof(D3D12_VERTEX_BUFFER_VIEW)==16&&sizeof(D3D12_INDEX_BUFFER_VIEW)==16&&sizeof(D3D12_BOX)==24&&sizeof(D3D12_RANGE)==16,"view structure layout");
_Static_assert(sizeof(D3D12_HEAP_PROPERTIES)==20&&sizeof(D3D12_CLEAR_VALUE)==20&&sizeof(D3D12_HEAP_DESC)==48&&sizeof(D3D12_SAMPLER_DESC)==52&&sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)==24,"desc layout");

/* ---- kinds: one COM class per wrapped interface ------------------------------------------------------------------------------- */
#define IUNK "QueryInterface","AddRef","Release"
#define OBJ7 IUNK,"GetPrivateData","SetPrivateData","SetPrivateDataInterface","SetName"
static const char *const pso_names[]={OBJ7,"GetDevice","GetCachedBlob"};
static const char *const child_names[]={OBJ7,"GetDevice"};
static const char *const heap_names[]={OBJ7,"GetDevice","GetDesc"};
static const char *const blob_names[]={IUNK,"GetBufferPointer","GetBufferSize"};
#define MAXSLOT 96
/* nslots = the vtable the guest object gets: the interface's own methods plus spare slots, so that a call of a method of a LATER
 * interface version (ID3D12Device1.., ID3D12GraphicsCommandList1..) is the named fatal too instead of a read behind the vtable */
typedef struct { const char *iface, *cls; const char *const *names; int nnames, nslots; } WKind;
static WKind wk[WK_N]={
    [WK_DEVICE]   ={"ID3D12Device","vkd3dw:ID3D12Device",NULL,0,88},
    [WK_QUEUE]    ={"ID3D12CommandQueue","vkd3dw:ID3D12CommandQueue",NULL,0,24},
    [WK_ALLOCATOR]={"ID3D12CommandAllocator","vkd3dw:ID3D12CommandAllocator",NULL,0,12},
    [WK_LIST]     ={"ID3D12GraphicsCommandList","vkd3dw:ID3D12GraphicsCommandList",NULL,0,96},
    [WK_RESOURCE] ={"ID3D12Resource","vkd3dw:ID3D12Resource",NULL,0,20},
    [WK_DHEAP]    ={"ID3D12DescriptorHeap","vkd3dw:ID3D12DescriptorHeap",NULL,0,14},
    [WK_FENCE]    ={"ID3D12Fence","vkd3dw:ID3D12Fence",NULL,0,14},
    [WK_PSO]      ={"ID3D12PipelineState","vkd3dw:ID3D12PipelineState",pso_names,9,12},
    [WK_ROOTSIG]  ={"ID3D12RootSignature","vkd3dw:ID3D12RootSignature",child_names,8,12},
    [WK_HEAP]     ={"ID3D12Heap","vkd3dw:ID3D12Heap",heap_names,9,12},
    [WK_QHEAP]    ={"ID3D12QueryHeap","vkd3dw:ID3D12QueryHeap",child_names,8,12},
    [WK_BLOB]     ={"ID3DBlob","vkd3dw:ID3DBlob",blob_names,5,8},
};
static int C_w[WK_N]; static int classes_done;
static _Atomic u64 wcalls[WK_N][MAXSLOT], n_made[WK_N], n_dead[WK_N], n_maps;
static const char *mname(int kind,int s){ return s>=0&&s<wk[kind].nnames?wk[kind].names[s]:"(a method of a later interface version)"; }

/* ---- guest objects and the table real -> guest object ------------------------------------------------------------------------- */
#define W_REAL   32                            /* host pointer of the real interface */
#define W_COOKIE 40                            /* real ^ guest address ^ W_MAGIC: only a live wrapper of this bridge has it */
#define W_KIND   48
#define W_EXTRA  24
#define W_MAGIC  0x57524150766b6433ull
typedef struct { void *real; u64 obj; int kind; } WEnt;
static WEnt *wt; static u32 wt_cap, wt_n; static pthread_mutex_t wm=PTHREAD_MUTEX_INITIALIZER;
static u32 wt_hash(const void *p){ u64 x=U(p); x^=x>>33; x*=0xff51afd7ed558ccdull; x^=x>>33; return (u32)x; }
static WEnt *wt_find(const void *real){ if(!wt_cap) return NULL; u32 m=wt_cap-1,i=wt_hash(real)&m; while(wt[i].real){ if(wt[i].real==real) return &wt[i]; i=(i+1)&m; } return NULL; }
static void wt_put(KCtx *g,void *real,u64 obj,int kind){
    if((wt_n+1)*2>wt_cap){ u32 nc=wt_cap?wt_cap*2:1024; WEnt *nt=calloc(nc,sizeof *nt); if(!nt) kh_fatal(g,"gfx-wrap-table: out of memory for %u wrapper entries",nc);
        for(u32 k=0;k<wt_cap;k++) if(wt[k].real){ u32 i=wt_hash(wt[k].real)&(nc-1); while(nt[i].real) i=(i+1)&(nc-1); nt[i]=wt[k]; }
        free(wt); wt=nt; wt_cap=nc; }
    u32 m=wt_cap-1,i=wt_hash(real)&m; while(wt[i].real) i=(i+1)&m; wt[i].real=real; wt[i].obj=obj; wt[i].kind=kind; wt_n++;
}
static void wt_del(WEnt *e){                   /* linear probing: close the gap */
    u32 m=wt_cap-1,i=(u32)(e-wt),j=i;
    for(;;){ j=(j+1)&m; if(!wt[j].real) break; u32 k=wt_hash(wt[j].real)&m; if(i<=j?(i<k&&k<=j):(i<k||k<=j)) continue; wt[i]=wt[j]; i=j; }
    wt[i].real=NULL; wt[i].obj=0; wt_n--;
}
static ULONG real_refs(void *real){ IUnknown_AddRef((IUnknown*)real); return IUnknown_Release((IUnknown*)real); }
/* THE guest object of a real object.  fresh = the real object was just created for the game: an entry that is already there must then
 * belong to the same, still referenced object (count > 1); with a count of 1 it is a wrapper that outlived its object. */
static u64 w_get(KCtx *g,void *real,int kind,int fresh){
    if(!real) return 0;
    pthread_mutex_lock(&wm);
    WEnt *e=wt_find(real); u64 o;
    if(e){
        if(e->kind!=kind){ int k=e->kind; pthread_mutex_unlock(&wm); kh_fatal(g,"gfx-wrap-kind: real object %p is known as %s but is now handed out as %s",real,wk[k].iface,wk[kind].iface); }
        if(fresh&&real_refs(real)==1){ u64 old=e->obj; pthread_mutex_unlock(&wm);
            kh_fatal(g,"gfx-wrap-stale: a new %s was created at %p while guest object 0x%llx still stands for an earlier object at that address (it was never released through the bridge)",wk[kind].iface,real,P(old)); }
        o=e->obj;
    } else {
        o=kh_com_obj(C_w[kind],W_EXTRA); if(!o){ pthread_mutex_unlock(&wm); kh_fatal(g,"gfx-wrap-table: no guest memory for a %s wrapper",wk[kind].iface); }
        wr64(g,o+W_REAL,U(real)); wr64(g,o+W_COOKIE,U(real)^o^W_MAGIC); wr32(g,o+W_KIND,(u32)kind);
        wt_put(g,real,o,kind); atomic_fetch_add(&n_made[kind],1);
    }
    pthread_mutex_unlock(&wm); return o;
}
u64 kh_wrap_obj(KCtx *g,void *real,int kind){ return w_get(g,real,kind,0); }
static u64 w_lookup(const void *real){ pthread_mutex_lock(&wm); WEnt *e=wt_find(real); u64 o=e?e->obj:0; pthread_mutex_unlock(&wm); return o; }
/* caller holds wm: the real object is gone, so is its guest object */
static void w_kill(KCtx *g,WEnt *e){
    u64 o=e->obj; int kind=e->kind; void *real=e->real; wt_del(e);
    wr64(g,o,0); wr64(g,o+16,0); wr64(g,o+W_REAL,0); wr64(g,o+W_COOKIE,0); ga_free(o); atomic_fetch_add(&n_dead[kind],1);
    if(kind==WK_LIST) kh_gfxc_track_drop(U(real));
}
/* Release through the guest object.  The table lock is held across the real Release: a new object that vkd3d places at the same address
 * cannot be wrapped before the old entry is gone. */
static u32 w_release(KCtx *g,u64 o,void *real){
    pthread_mutex_lock(&wm);
    ULONG r=IUnknown_Release((IUnknown*)real);
    if(!r){ WEnt *e=wt_find(real); if(!e||e->obj!=o){ pthread_mutex_unlock(&wm); kh_fatal(g,"gfx-wrap-table: guest object 0x%llx released its real object %p, which the table does not map to it",P(o),real); } w_kill(g,e); }
    else wr64(g,o+16,r);
    pthread_mutex_unlock(&wm); return r;
}
/* the runtime drops ITS reference to a real object the game may hold too (swap-chain back buffers) */
u32 kh_wrap_release_real(KCtx *g,void *real){
    if(!real) return 0;
    pthread_mutex_lock(&wm);
    ULONG r=IUnknown_Release((IUnknown*)real);
    if(!r){ WEnt *e=wt_find(real); if(e) w_kill(g,e); }
    pthread_mutex_unlock(&wm); return r;
}

/* ---- translation context: guest -> host with an EXPLICIT bias, so the selftest can run every translator under a bias of its own ---- */
/* scratch: 3 KiB on the stack, then heap blocks, AS MANY AS THE CALL NEEDS: each block starts with a 16-byte header that links it to the
 * one before (chain), wx_done frees the chain.  (A fixed table of 8 blocks made a valid root signature a fatal: x_rootsig copies every
 * descriptor table's range array separately, and D3D12_MAX_ROOT_COST allows 64 tables.)  nbig counts the blocks of the current call. */
typedef struct WX { uintptr_t bias; KCtx *g; int synth; size_t used; int nbig; void *chain; u8 buf[3072] __attribute__((aligned(16))); } WX;
static void wx_init(WX *x,KCtx *g){ x->bias=(uintptr_t)XL(g,0); x->g=g; x->synth=0; x->used=0; x->nbig=0; x->chain=NULL; }
static void wx_done(WX *x){ while(x->chain){ void *prev; memcpy(&prev,x->chain,sizeof prev); free(x->chain); x->chain=prev; } x->nbig=0; x->used=0; }
static void *wx_alloc(WX *x,size_t n){
    n=(n+15)&~(size_t)15; if(x->used+n<=sizeof x->buf){ void *p=x->buf+x->used; x->used+=n; return p; }
    u8 *b=malloc(16+n); if(!b) kh_fatal(x->g,"gfx-wrap-scratch: out of memory (%zu bytes)",n);       /* malloc: 16-byte aligned, so b+16 is too */
    memcpy(b,&x->chain,sizeof x->chain); x->chain=b; x->nbig++; return b+16;
}
static inline void *wx_ptr(const WX *x,u64 a){ return a?(void*)(uintptr_t)(a+x->bias):NULL; }          /* NULL stays NULL */
static void *x_copy(WX *x,u64 ga,size_t n){ if(!ga||!n) return NULL; void *p=wx_alloc(x,n); memcpy(p,wx_ptr(x,ga),n); return p; }
static void x_count(const WX *x,u64 n,u64 max,const char *what){ if(n>max) kh_fatal(x->g,"gfx-wrap-desc: %s: a count of %llu is not plausible (limit %llu)",what,P(n),P(max)); }
/* interface argument -> real object.  kind < 0: any wrapped interface.  Anything that is not a live wrapper of this bridge is a named fatal */
static void *wx_unwrap(const WX *x,u64 obj,int kind,const char *what){
    if(!obj) return NULL;
    if(!x->synth&&!GA_OK(obj)) kh_fatal(x->g,"gfx-wrap-foreign-object: %s: 0x%llx is not an object of the wrapped bridge (not guest memory)",what,P(obj));
    const u8 *p=(const u8*)(uintptr_t)(obj+x->bias); u64 vt,real,ck; u32 k; memcpy(&vt,p,8); memcpy(&real,p+W_REAL,8); memcpy(&ck,p+W_COOKIE,8); memcpy(&k,p+W_KIND,4);
    if(k>=WK_N||vt!=kh_com_vtbl(C_w[k])||!real||ck!=(real^obj^W_MAGIC))
        kh_fatal(x->g,"gfx-wrap-foreign-object: %s: 0x%llx is not a live object of the wrapped bridge (vtable 0x%llx)%s",what,P(obj),P(vt),kind==WK_LIST&&!x->synth&&kh_gfxc_is_bundle(x->g,obj)?": it is a bundle":"");
    if(kind>=0&&(int)k!=kind) kh_fatal(x->g,"gfx-wrap-foreign-object: %s: 0x%llx is a %s, a %s is needed",what,P(obj),wk[k].iface,wk[kind].iface);
    return (void*)(uintptr_t)real;
}
void *kh_wrap_real(KCtx *g,u64 obj,int kind,const char *what){ if(!obj) return NULL; WX x; x.bias=(uintptr_t)XL(g,0); x.g=g; x.synth=0; return wx_unwrap(&x,obj,kind,what); }
#define PV(p) ((u64)(uintptr_t)(p))            /* the guest VALUE of a pointer field that was copied out of a guest structure */

/* D3D12_GRAPHICS_PIPELINE_STATE_DESC */
static const D3D12_GRAPHICS_PIPELINE_STATE_DESC *x_pso(WX *x,u64 ga){
    if(!ga) return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC *d=x_copy(x,ga,sizeof *d);
    d->pRootSignature=(ID3D12RootSignature*)wx_unwrap(x,PV(d->pRootSignature),WK_ROOTSIG,"CreateGraphicsPipelineState: pRootSignature");
    D3D12_SHADER_BYTECODE *sb[5]={&d->VS,&d->PS,&d->DS,&d->HS,&d->GS};
    for(int i=0;i<5;i++) sb[i]->pShaderBytecode=wx_ptr(x,PV(sb[i]->pShaderBytecode));
    if(d->StreamOutput.pSODeclaration){ UINT n=d->StreamOutput.NumEntries; x_count(x,n,4096,"stream-output declaration entries");
        D3D12_SO_DECLARATION_ENTRY *e=x_copy(x,PV(d->StreamOutput.pSODeclaration),sizeof *e*(size_t)n);
        for(UINT i=0;i<n;i++) e[i].SemanticName=wx_ptr(x,PV(e[i].SemanticName));
        d->StreamOutput.pSODeclaration=e; }
    d->StreamOutput.pBufferStrides=wx_ptr(x,PV(d->StreamOutput.pBufferStrides));
    if(d->InputLayout.pInputElementDescs){ UINT n=d->InputLayout.NumElements; x_count(x,n,4096,"input elements");
        D3D12_INPUT_ELEMENT_DESC *e=x_copy(x,PV(d->InputLayout.pInputElementDescs),sizeof *e*(size_t)n);
        for(UINT i=0;i<n;i++) e[i].SemanticName=wx_ptr(x,PV(e[i].SemanticName));
        d->InputLayout.pInputElementDescs=e; }
    d->CachedPSO.pCachedBlob=wx_ptr(x,PV(d->CachedPSO.pCachedBlob));
    return d;
}
/* D3D12_RESOURCE_BARRIER[n] */
static D3D12_RESOURCE_BARRIER *x_barriers(WX *x,u64 ga,u32 n){
    if(!ga||!n) return NULL; x_count(x,n,65536,"resource barriers");
    D3D12_RESOURCE_BARRIER *b=x_copy(x,ga,sizeof *b*(size_t)n);
    for(u32 i=0;i<n;i++) switch(b[i].Type){
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: b[i].Transition.pResource=(ID3D12Resource*)wx_unwrap(x,PV(b[i].Transition.pResource),WK_RESOURCE,"ResourceBarrier: Transition.pResource"); break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:   b[i].Aliasing.pResourceBefore=(ID3D12Resource*)wx_unwrap(x,PV(b[i].Aliasing.pResourceBefore),WK_RESOURCE,"ResourceBarrier: Aliasing.pResourceBefore");
                                                     b[i].Aliasing.pResourceAfter=(ID3D12Resource*)wx_unwrap(x,PV(b[i].Aliasing.pResourceAfter),WK_RESOURCE,"ResourceBarrier: Aliasing.pResourceAfter"); break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:        b[i].UAV.pResource=(ID3D12Resource*)wx_unwrap(x,PV(b[i].UAV.pResource),WK_RESOURCE,"ResourceBarrier: UAV.pResource"); break;
        default: kh_fatal(x->g,"gfx-wrap-barrier-type: ResourceBarrier: barrier %u of %u has type %u, which has no translation",i,n,(unsigned)b[i].Type); }
    return b;
}
/* D3D12_TEXTURE_COPY_LOCATION */
static const D3D12_TEXTURE_COPY_LOCATION *x_copyloc(WX *x,u64 ga,const char *what){
    if(!ga) return NULL; D3D12_TEXTURE_COPY_LOCATION *l=x_copy(x,ga,sizeof *l);
    l->pResource=(ID3D12Resource*)wx_unwrap(x,PV(l->pResource),WK_RESOURCE,what); return l;
}
/* D3D12_ROOT_SIGNATURE_DESC (version 1.0) */
static const D3D12_ROOT_SIGNATURE_DESC *x_rootsig(WX *x,u64 ga){
    if(!ga) return NULL; D3D12_ROOT_SIGNATURE_DESC *d=x_copy(x,ga,sizeof *d);
    if(d->pParameters){ UINT n=d->NumParameters; x_count(x,n,4096,"root parameters");
        D3D12_ROOT_PARAMETER *p=x_copy(x,PV(d->pParameters),sizeof *p*(size_t)n);
        for(UINT i=0;i<n;i++) if(p[i].ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE&&p[i].DescriptorTable.pDescriptorRanges){
            UINT nr=p[i].DescriptorTable.NumDescriptorRanges; x_count(x,nr,65536,"descriptor ranges");
            p[i].DescriptorTable.pDescriptorRanges=x_copy(x,PV(p[i].DescriptorTable.pDescriptorRanges),sizeof(D3D12_DESCRIPTOR_RANGE)*(size_t)nr); }
        d->pParameters=p; }
    if(d->pStaticSamplers){ UINT n=d->NumStaticSamplers; x_count(x,n,4096,"static samplers"); d->pStaticSamplers=x_copy(x,PV(d->pStaticSamplers),sizeof(D3D12_STATIC_SAMPLER_DESC)*(size_t)n); }
    return d;
}
/* an array of interface pointers (command lists, descriptor heaps) -> real pointers */
static void **x_ifaces(WX *x,u64 ga,u32 n,int kind,const char *what){
    if(!ga||!n) return NULL; x_count(x,n,65536,what);
    void **out=wx_alloc(x,sizeof(void*)*(size_t)n); const u8 *src=wx_ptr(x,ga);
    for(u32 i=0;i<n;i++){ u64 o; memcpy(&o,src+8ull*i,8); out[i]=wx_unwrap(x,o,kind,what); }
    return out;
}
/* D3D12_FEATURE_DATA_FEATURE_LEVELS */
static D3D12_FEATURE_DATA_FEATURE_LEVELS *x_feature_levels(WX *x,u64 ga){
    if(!ga) return NULL; D3D12_FEATURE_DATA_FEATURE_LEVELS *f=x_copy(x,ga,sizeof *f); x_count(x,f->NumFeatureLevels,256,"feature levels");
    f->pFeatureLevelsRequested=x_copy(x,PV(f->pFeatureLevelsRequested),sizeof(D3D_FEATURE_LEVEL)*(size_t)f->NumFeatureLevels); return f;
}

/* ---- the methods ------------------------------------------------------------------------------------------------------------------ */
#define CP(var,ga) ((ga)?(memcpy(&(var),hp(g,(ga)),sizeof(var)),&(var)):NULL)       /* plain structure argument: aligned host copy, or NULL */
static const IID *iidp(KCtx *g,u64 a,const char *what){ if(!a) kh_fatal(g,"gfx-wrap-null-iid: %s was called without an interface id",what); return (const IID*)hp(g,a); }
static inline u64 w_gp(KCtx *g,const void *p){ return U(p)-(u64)XL(g,0); }
/* a host address the game will read or write through: the guest address that XL() maps to it */
static u64 host_to_guest(KCtx *g,const void *p,const char *what){
    if(!p) return 0; u64 a=w_gp(g,p);
    if(!a) kh_fatal(g,"gfx-wrap-null-alias: %s returned host address %p, which is guest address 0 under the current bias",what,p);
    return a;
}
/* a creation result: the game gets the guest object of the new real object (or NULL with the failing HRESULT) */
static void w_out(KCtx *g,u64 pp,HRESULT hr,void *out,int kind){ if(pp) wr64(g,pp,SUCCEEDED(hr)&&out?w_get(g,out,kind,1):0); SET_RAX(g,(u32)hr); }
static D3D12_CPU_DESCRIPTOR_HANDLE cpuh(u64 v){ D3D12_CPU_DESCRIPTOR_HANDLE h; h.ptr=(SIZE_T)v; return h; }
static D3D12_GPU_DESCRIPTOR_HANDLE gpuh(u64 v){ D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr=v; return h; }
#define RES(a,what) ((ID3D12Resource*)kh_wrap_real(g,(a),WK_RESOURCE,what))

/* IUnknown, slots 0..2 */
static void w_unknown(KCtx *g,u64 o,void *real,int kind,int s){
    IUnknown *u=(IUnknown*)real;
    if(s==0){ u64 pp=A2(g); void *out=NULL;
        if(!pp){ SET_RAX(g,(u32)E_POINTER); return; }
        HRESULT hr=IUnknown_QueryInterface(u,iidp(g,A1(g),"QueryInterface"),&out);
        if(SUCCEEDED(hr)&&out&&out!=real) kh_fatal(g,"gfx-wrap-qi-identity: %s %p answers QueryInterface with another pointer (%p); the wrapped bridge keeps one guest object per real pointer",wk[kind].iface,real,out);
        wr64(g,pp,SUCCEEDED(hr)&&out?o:0); SET_RAX(g,(u32)hr); return; }             /* the same guest object, one more REAL reference */
    if(s==1){ ULONG r=IUnknown_AddRef(u); wr64(g,o+16,r); SET_RAX(g,r); return; }
    SET_RAX(g,w_release(g,o,real));
}
/* ID3D12Object slots 3..6 and ID3D12DeviceChild::GetDevice (slot 7) */
static int w_object(KCtx *g,void *real,int kind,int s){
    ID3D12Object *ob=(ID3D12Object*)real;
    switch(s){
    case 3: { u64 pg=A1(g), ps=A2(g), pd=A3(g); UINT size=ps?rd32(g,ps):0;       /* GetPrivateData(guid, *size, data): plain bytes only (see slot 5) */
        HRESULT hr=ID3D12Object_GetPrivateData(ob,(const GUID*)iidp(g,pg,"GetPrivateData"),ps?&size:NULL,pd?hp(g,pd):NULL); if(ps) wr32(g,ps,size); SET_RAX(g,(u32)hr); return 1; }
    case 4: SET_RAX(g,(u32)ID3D12Object_SetPrivateData(ob,(const GUID*)iidp(g,A1(g),"SetPrivateData"),(UINT)A2(g),A3(g)?hp(g,A3(g)):NULL)); return 1;
    case 5: if(A2(g)) kh_fatal(g,"gfx-wrap-private-interface: %s::SetPrivateDataInterface with an interface (0x%llx): GetPrivateData would hand its host pointer to the game; not translated",wk[kind].iface,P(A2(g)));
            SET_RAX(g,(u32)ID3D12Object_SetPrivateDataInterface(ob,(const GUID*)iidp(g,A1(g),"SetPrivateDataInterface"),NULL)); return 1;
    case 6: SET_RAX(g,(u32)ID3D12Object_SetName(ob,A1(g)?(const WCHAR*)hp(g,A1(g)):NULL)); return 1;      /* 16-bit characters: the vkd3d instance was created with wchar_size 2 */
    case 7: if(kind==WK_DEVICE) return 0;
        { u64 pp=A2(g); void *out=NULL; HRESULT hr=ID3D12DeviceChild_GetDevice((ID3D12DeviceChild*)real,iidp(g,A1(g),"GetDevice"),pp?&out:NULL);
          if(pp) wr64(g,pp,SUCCEEDED(hr)&&out?w_get(g,out,WK_DEVICE,0):0); SET_RAX(g,(u32)hr); return 1; }   /* the device object the game already has */
    }
    return 0;
}
/* CheckFeatureSupport structures without a pointer: copied in and out as bytes */
static int feature_is_plain(u32 f){ switch(f){ case D3D12_FEATURE_D3D12_OPTIONS: case D3D12_FEATURE_ARCHITECTURE: case D3D12_FEATURE_FORMAT_SUPPORT: case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS:
    case D3D12_FEATURE_FORMAT_INFO: case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: case D3D12_FEATURE_SHADER_MODEL: case D3D12_FEATURE_D3D12_OPTIONS1: case D3D12_FEATURE_ROOT_SIGNATURE:
    case D3D12_FEATURE_ARCHITECTURE1: case D3D12_FEATURE_D3D12_OPTIONS2: case D3D12_FEATURE_SHADER_CACHE: case D3D12_FEATURE_D3D12_OPTIONS3: case D3D12_FEATURE_D3D12_OPTIONS4: case D3D12_FEATURE_D3D12_OPTIONS5: return 1; }
    return 0; }
static int w_dev(KCtx *g,ID3D12Device *d,int s){
    switch(s){
    case 7: SET_RAX(g,ID3D12Device_GetNodeCount(d)); return 1;
    case 8: { D3D12_COMMAND_QUEUE_DESC qd; u64 pp=A3(g); void *out=NULL;         /* CreateCommandQueue(desc, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateCommandQueue(d,CP(qd,A1(g)),iidp(g,A2(g),"CreateCommandQueue"),pp?&out:NULL); w_out(g,pp,hr,out,WK_QUEUE); return 1; }
    case 9: { u32 type=(u32)A1(g); u64 pp=A3(g); void *out=NULL;                 /* CreateCommandAllocator(type, riid, **pp): vkd3d has no bundles, ours is the recorder's */
        if(type==D3D12_COMMAND_LIST_TYPE_BUNDLE){ if(!pp) kh_fatal(g,"gfx-bundle: CreateCommandAllocator(BUNDLE) without a result pointer"); wr64(g,pp,kh_gfxc_bundle_allocator_new(g)); SET_RAX(g,0); return 1; }
        HRESULT hr=ID3D12Device_CreateCommandAllocator(d,(D3D12_COMMAND_LIST_TYPE)type,iidp(g,A2(g),"CreateCommandAllocator"),pp?&out:NULL); w_out(g,pp,hr,out,WK_ALLOCATOR); return 1; }
    case 10: { WX x; wx_init(&x,g); u64 pp=A3(g); void *out=NULL;                /* CreateGraphicsPipelineState(desc, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateGraphicsPipelineState(d,x_pso(&x,A1(g)),iidp(g,A2(g),"CreateGraphicsPipelineState"),pp?&out:NULL); wx_done(&x);
        atomic_fetch_add(&gfx_psos,1); w_out(g,pp,hr,out,WK_PSO); return 1; }
    case 12: { u32 type=(u32)A2(g); u64 al=A3(g), pso=ARG(g,4), riid=ARG(g,5), pp=ARG(g,6); void *out=NULL;   /* CreateCommandList(node, type, allocator, pso, riid, **pp) */
        if(type==D3D12_COMMAND_LIST_TYPE_BUNDLE){ if(!pp) kh_fatal(g,"gfx-bundle: CreateCommandList(BUNDLE) without a result pointer"); wr64(g,pp,kh_gfxc_bundle_new(g,al,pso)); SET_RAX(g,0); return 1; }
        HRESULT hr=ID3D12Device_CreateCommandList(d,(UINT)A1(g),(D3D12_COMMAND_LIST_TYPE)type,(ID3D12CommandAllocator*)kh_wrap_real(g,al,WK_ALLOCATOR,"CreateCommandList: allocator"),
                                                  (ID3D12PipelineState*)kh_wrap_real(g,pso,WK_PSO,"CreateCommandList: initial pipeline state"),iidp(g,riid,"CreateCommandList"),pp?&out:NULL);
        w_out(g,pp,hr,out,WK_LIST); return 1; }
    case 13: { u32 feat=(u32)A1(g); u64 pd=A2(g); u32 size=(u32)A3(g); HRESULT hr;   /* CheckFeatureSupport(feature, data, size) */
        if(feat==D3D12_FEATURE_FEATURE_LEVELS){ if(size!=sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)||!pd){ SET_RAX(g,(u32)E_INVALIDARG); return 1; }
            WX x; wx_init(&x,g); D3D12_FEATURE_DATA_FEATURE_LEVELS *f=x_feature_levels(&x,pd); hr=ID3D12Device_CheckFeatureSupport(d,feat,f,size);
            wr32(g,pd+offsetof(D3D12_FEATURE_DATA_FEATURE_LEVELS,MaxSupportedFeatureLevel),(u32)f->MaxSupportedFeatureLevel); wx_done(&x); }
        else if(feature_is_plain(feat)){ u64 tmp[64]; if(size>sizeof tmp) kh_fatal(g,"gfx-wrap-desc: CheckFeatureSupport(feature %u) with %u bytes of data (limit %zu)",feat,size,sizeof tmp);
            if(pd&&size) memcpy(tmp,hp(g,pd),size);
            hr=ID3D12Device_CheckFeatureSupport(d,(D3D12_FEATURE)feat,pd?(void*)tmp:NULL,size); if(pd&&size) memcpy(hp(g,pd),tmp,size); }
        else kh_fatal(g,"gfx-wrap-feature: ID3D12Device::CheckFeatureSupport(feature %u, %u bytes): this feature's structure has no translation in the wrapped bridge",feat,size);
        SET_RAX(g,(u32)hr); return 1; }
    case 14: { D3D12_DESCRIPTOR_HEAP_DESC hd; u64 pp=A3(g); void *out=NULL;      /* CreateDescriptorHeap(desc, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateDescriptorHeap(d,CP(hd,A1(g)),iidp(g,A2(g),"CreateDescriptorHeap"),pp?&out:NULL); w_out(g,pp,hr,out,WK_DHEAP); return 1; }
    case 15: SET_RAX(g,ID3D12Device_GetDescriptorHandleIncrementSize(d,(D3D12_DESCRIPTOR_HEAP_TYPE)(u32)A1(g))); return 1;
    case 16: { u64 blob=A2(g), size=A3(g), pp=ARG(g,5); void *out=NULL;          /* CreateRootSignature(node, blob, size, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateRootSignature(d,(UINT)A1(g),blob?hp(g,blob):NULL,(SIZE_T)size,iidp(g,ARG(g,4),"CreateRootSignature"),pp?&out:NULL);
        if(SUCCEEDED(hr)&&out) kh_gfxc_rootsig_learn(g,U(out),hp(g,blob),(size_t)size);                 /* which root parameters are descriptor tables (default bindings) */
        w_out(g,pp,hr,out,WK_ROOTSIG); return 1; }
    case 17: { D3D12_CONSTANT_BUFFER_VIEW_DESC vd; ID3D12Device_CreateConstantBufferView(d,CP(vd,A1(g)),cpuh(A2(g))); return 1; }
    case 18: { D3D12_SHADER_RESOURCE_VIEW_DESC vd; ID3D12Device_CreateShaderResourceView(d,RES(A1(g),"CreateShaderResourceView"),CP(vd,A2(g)),cpuh(A3(g))); return 1; }
    case 19: { D3D12_UNORDERED_ACCESS_VIEW_DESC vd; ID3D12Device_CreateUnorderedAccessView(d,RES(A1(g),"CreateUnorderedAccessView"),RES(A2(g),"CreateUnorderedAccessView: counter"),CP(vd,A3(g)),cpuh(ARG(g,4))); return 1; }
    case 20: { D3D12_RENDER_TARGET_VIEW_DESC vd; ID3D12Device_CreateRenderTargetView(d,RES(A1(g),"CreateRenderTargetView"),CP(vd,A2(g)),cpuh(A3(g))); return 1; }
    case 21: { D3D12_DEPTH_STENCIL_VIEW_DESC vd; ID3D12Device_CreateDepthStencilView(d,RES(A1(g),"CreateDepthStencilView"),CP(vd,A2(g)),cpuh(A3(g))); return 1; }
    case 22: { D3D12_SAMPLER_DESC sd; ID3D12Device_CreateSampler(d,CP(sd,A1(g)),cpuh(A2(g))); return 1; }
    case 23: { u32 nd=(u32)A1(g), ns=(u32)ARG(g,4); WX x; wx_init(&x,g); x_count(&x,nd,65536,"CopyDescriptors destination ranges"); x_count(&x,ns,65536,"CopyDescriptors source ranges");
        /* CopyDescriptors(nDst, dstStarts, dstSizes, nSrc, srcStarts, srcSizes, type): handles and sizes are plain values */
        ID3D12Device_CopyDescriptors(d,nd,x_copy(&x,A2(g),8ull*nd),x_copy(&x,A3(g),4ull*nd),ns,x_copy(&x,ARG(g,5),8ull*ns),x_copy(&x,ARG(g,6),4ull*ns),(D3D12_DESCRIPTOR_HEAP_TYPE)(u32)ARG(g,7));
        wx_done(&x); return 1; }
    case 24: ID3D12Device_CopyDescriptorsSimple(d,(UINT)A1(g),cpuh(A2(g)),cpuh(A3(g)),(D3D12_DESCRIPTOR_HEAP_TYPE)(u32)ARG(g,4)); return 1;
    case 25: { u64 ret=A1(g); u32 n=(u32)A3(g); WX x; wx_init(&x,g); x_count(&x,n,4096,"GetResourceAllocationInfo descs");   /* (*ret, visible mask, count, descs): struct result */
        D3D12_RESOURCE_ALLOCATION_INFO r=ID3D12Device_GetResourceAllocationInfo(d,(UINT)A2(g),n,x_copy(&x,ARG(g,4),sizeof(D3D12_RESOURCE_DESC)*(size_t)n));
        wx_done(&x); memcpy(hp(g,ret),&r,sizeof r); SET_RAX(g,ret); return 1; }
    case 26: { u64 ret=A1(g); D3D12_HEAP_PROPERTIES r=ID3D12Device_GetCustomHeapProperties(d,(UINT)A2(g),(D3D12_HEAP_TYPE)(u32)A3(g)); memcpy(hp(g,ret),&r,sizeof r); SET_RAX(g,ret); return 1; }
    case 27: { D3D12_HEAP_PROPERTIES hprops; D3D12_RESOURCE_DESC rd; D3D12_CLEAR_VALUE cv; u64 pp=ARG(g,7); void *out=NULL;   /* CreateCommittedResource(heap props, heap flags, desc, state, clear, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateCommittedResource(d,CP(hprops,A1(g)),(D3D12_HEAP_FLAGS)(u32)A2(g),CP(rd,A3(g)),(D3D12_RESOURCE_STATES)(u32)ARG(g,4),CP(cv,ARG(g,5)),iidp(g,ARG(g,6),"CreateCommittedResource"),pp?&out:NULL);
        atomic_fetch_add(&gfx_resources,1); if(SUCCEEDED(hr)&&A3(g)) kh_gfxc_census_resource(&rd); w_out(g,pp,hr,out,WK_RESOURCE); return 1; }
    case 28: { D3D12_HEAP_DESC hd; u64 pp=A3(g); void *out=NULL;                 /* CreateHeap(desc, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateHeap(d,CP(hd,A1(g)),iidp(g,A2(g),"CreateHeap"),pp?&out:NULL); w_out(g,pp,hr,out,WK_HEAP); return 1; }
    case 29: { D3D12_RESOURCE_DESC rd; D3D12_CLEAR_VALUE cv; u64 pp=ARG(g,7); void *out=NULL;                          /* CreatePlacedResource(heap, offset, desc, state, clear, riid, **pp) */
        HRESULT hr=ID3D12Device_CreatePlacedResource(d,(ID3D12Heap*)kh_wrap_real(g,A1(g),WK_HEAP,"CreatePlacedResource: heap"),A2(g),CP(rd,A3(g)),(D3D12_RESOURCE_STATES)(u32)ARG(g,4),CP(cv,ARG(g,5)),iidp(g,ARG(g,6),"CreatePlacedResource"),pp?&out:NULL);
        atomic_fetch_add(&gfx_resources,1); if(SUCCEEDED(hr)&&A3(g)) kh_gfxc_census_resource(&rd); w_out(g,pp,hr,out,WK_RESOURCE); return 1; }
    case 31: { u64 ph=ARG(g,5); HANDLE h=NULL;                                   /* CreateSharedHandle(object, attributes, access, name, *handle) */
        if(A2(g)) kh_fatal(g,"gfx-wrap-security-attributes: ID3D12Device::CreateSharedHandle with SECURITY_ATTRIBUTES at 0x%llx: the structure embeds a pointer and vkd3d's headers do not define it; not translated",P(A2(g)));
        HRESULT hr=ID3D12Device_CreateSharedHandle(d,(ID3D12DeviceChild*)kh_wrap_real(g,A1(g),-1,"CreateSharedHandle: object"),NULL,(DWORD)A3(g),ARG(g,4)?(const WCHAR*)hp(g,ARG(g,4)):NULL,ph?&h:NULL);
        if(ph) wr64(g,ph,SUCCEEDED(hr)?U(h):0); SET_RAX(g,(u32)hr); return 1; }
    case 32: { u64 riid=A2(g), pp=A3(g); void *out=NULL; const IID *id=iidp(g,riid,"OpenSharedHandle");               /* OpenSharedHandle(handle, riid, **pp) */
        int kind=!memcmp(id,&IID_ID3D12Resource,sizeof *id)?WK_RESOURCE:!memcmp(id,&IID_ID3D12Heap,sizeof *id)?WK_HEAP:!memcmp(id,&IID_ID3D12Fence,sizeof *id)?WK_FENCE:-1;
        if(kind<0) kh_fatal(g,"gfx-wrap-shared-handle: ID3D12Device::OpenSharedHandle for an interface that is not ID3D12Resource, ID3D12Heap or ID3D12Fence");
        HRESULT hr=ID3D12Device_OpenSharedHandle(d,(HANDLE)(uintptr_t)A1(g),id,pp?&out:NULL); w_out(g,pp,hr,out,kind); return 1; }
    case 36: { u64 pp=ARG(g,4); void *out=NULL;                                  /* CreateFence(initial, flags, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateFence(d,A1(g),(D3D12_FENCE_FLAGS)(u32)A2(g),iidp(g,A3(g),"CreateFence"),pp?&out:NULL); w_out(g,pp,hr,out,WK_FENCE); return 1; }
    case 37: SET_RAX(g,(u32)ID3D12Device_GetDeviceRemovedReason(d)); return 1;
    case 38: { u64 pd=A1(g), pl=ARG(g,5), pr=ARG(g,6), prs=ARG(g,7), pt=ARG(g,8); u32 num=(u32)A3(g);                 /* GetCopyableFootprints(desc, first, count, base, layouts, rows, row sizes, total) */
        WX x; wx_init(&x,g); x_count(&x,num,65536,"GetCopyableFootprints subresources"); D3D12_RESOURCE_DESC rd; UINT64 total=0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *L=pl?wx_alloc(&x,sizeof *L*(size_t)num):NULL; UINT *R=pr?wx_alloc(&x,4ull*num):NULL; UINT64 *RS=prs?wx_alloc(&x,8ull*num):NULL;
        ID3D12Device_GetCopyableFootprints(d,CP(rd,pd),(UINT)A2(g),num,ARG(g,4),L,R,RS,pt?&total:NULL);
        if(L) memcpy(hp(g,pl),L,sizeof *L*(size_t)num); if(R) memcpy(hp(g,pr),R,4ull*num); if(RS) memcpy(hp(g,prs),RS,8ull*num); if(pt) wr64(g,pt,total);
        wx_done(&x); return 1; }
    case 39: { D3D12_QUERY_HEAP_DESC qd; u64 pp=A3(g); void *out=NULL;           /* CreateQueryHeap(desc, riid, **pp) */
        HRESULT hr=ID3D12Device_CreateQueryHeap(d,CP(qd,A1(g)),iidp(g,A2(g),"CreateQueryHeap"),pp?&out:NULL); w_out(g,pp,hr,out,WK_QHEAP); return 1; }
    }
    return 0;
}
static int w_queue(KCtx *g,ID3D12CommandQueue *q,int s){
    switch(s){
    case 10: { u32 n=(u32)A1(g); WX x; wx_init(&x,g);                            /* ExecuteCommandLists(count, lists) */
        ID3D12CommandQueue_ExecuteCommandLists(q,n,(ID3D12CommandList*const*)x_ifaces(&x,A2(g),n,WK_LIST,"ExecuteCommandLists")); wx_done(&x); atomic_fetch_add(&gfx_execs,1); return 1; }
    case 14: SET_RAX(g,(u32)ID3D12CommandQueue_Signal(q,(ID3D12Fence*)kh_wrap_real(g,A1(g),WK_FENCE,"ID3D12CommandQueue::Signal"),A2(g))); return 1;
    case 15: SET_RAX(g,(u32)ID3D12CommandQueue_Wait(q,(ID3D12Fence*)kh_wrap_real(g,A1(g),WK_FENCE,"ID3D12CommandQueue::Wait"),A2(g))); return 1;
    case 16: { UINT64 f=0; HRESULT hr=ID3D12CommandQueue_GetTimestampFrequency(q,A1(g)?&f:NULL); if(A1(g)) wr64(g,A1(g),f); SET_RAX(g,(u32)hr); return 1; }
    case 17: { UINT64 a=0,b=0; HRESULT hr=ID3D12CommandQueue_GetClockCalibration(q,A1(g)?&a:NULL,A2(g)?&b:NULL); if(A1(g)) wr64(g,A1(g),a); if(A2(g)) wr64(g,A2(g),b); SET_RAX(g,(u32)hr); return 1; }
    case 18: { u64 ret=A1(g); D3D12_COMMAND_QUEUE_DESC r=ID3D12CommandQueue_GetDesc(q); memcpy(hp(g,ret),&r,sizeof r); SET_RAX(g,ret); return 1; }
    }
    return 0;
}
static int w_alloc(KCtx *g,ID3D12CommandAllocator *a,int s){ if(s==8){ SET_RAX(g,(u32)ID3D12CommandAllocator_Reset(a)); return 1; } return 0; }
static int w_list(KCtx *g,ID3D12GraphicsCommandList *l,int s){
    switch(s){
    case 8: SET_RAX(g,(u32)ID3D12GraphicsCommandList_GetType(l)); return 1;
    case 9: SET_RAX(g,(u32)ID3D12GraphicsCommandList_Close(l)); return 1;
    case 10: { HRESULT hr=ID3D12GraphicsCommandList_Reset(l,(ID3D12CommandAllocator*)kh_wrap_real(g,A1(g),WK_ALLOCATOR,"ID3D12GraphicsCommandList::Reset: allocator"),
                                                          (ID3D12PipelineState*)kh_wrap_real(g,A2(g),WK_PSO,"ID3D12GraphicsCommandList::Reset: initial pipeline state"));
        if(SUCCEEDED(hr)) kh_gfxc_track_reset(g,U(l)); SET_RAX(g,(u32)hr); return 1; }
    case 11: ID3D12GraphicsCommandList_ClearState(l,(ID3D12PipelineState*)kh_wrap_real(g,A1(g),WK_PSO,"ClearState")); return 1;
    case 12: ID3D12GraphicsCommandList_DrawInstanced(l,(UINT)A1(g),(UINT)A2(g),(UINT)A3(g),(UINT)ARG(g,4)); atomic_fetch_add(&gfx_draws,1); return 1;
    case 13: ID3D12GraphicsCommandList_DrawIndexedInstanced(l,(UINT)A1(g),(UINT)A2(g),(UINT)A3(g),(INT)ARG(g,4),(UINT)ARG(g,5)); atomic_fetch_add(&gfx_draws,1); return 1;
    case 15: ID3D12GraphicsCommandList_CopyBufferRegion(l,RES(A1(g),"CopyBufferRegion: destination"),A2(g),RES(A3(g),"CopyBufferRegion: source"),ARG(g,4),ARG(g,5)); atomic_fetch_add(&gfx_copies,1); return 1;
    case 16: { WX x; wx_init(&x,g); D3D12_BOX box;                               /* CopyTextureRegion(dst location, x, y, z, src location, src box) */
        const D3D12_TEXTURE_COPY_LOCATION *dst=x_copyloc(&x,A1(g),"CopyTextureRegion: destination"), *src=x_copyloc(&x,ARG(g,5),"CopyTextureRegion: source");
        ID3D12GraphicsCommandList_CopyTextureRegion(l,dst,(UINT)A2(g),(UINT)A3(g),(UINT)ARG(g,4),src,CP(box,ARG(g,6))); wx_done(&x); atomic_fetch_add(&gfx_copies,1); return 1; }
    case 17: ID3D12GraphicsCommandList_CopyResource(l,RES(A1(g),"CopyResource: destination"),RES(A2(g),"CopyResource: source")); atomic_fetch_add(&gfx_copies,1); return 1;
    case 19: ID3D12GraphicsCommandList_ResolveSubresource(l,RES(A1(g),"ResolveSubresource: destination"),(UINT)A2(g),RES(A3(g),"ResolveSubresource: source"),(UINT)ARG(g,4),(DXGI_FORMAT)(u32)ARG(g,5));
             atomic_fetch_add(&gfx_copies,1); kh_gfxc_census_resolve(); return 1;
    case 20: ID3D12GraphicsCommandList_IASetPrimitiveTopology(l,(D3D12_PRIMITIVE_TOPOLOGY)(u32)A1(g)); return 1;
    case 21: { u32 n=(u32)A1(g); WX x; wx_init(&x,g); x_count(&x,n,4096,"viewports"); ID3D12GraphicsCommandList_RSSetViewports(l,n,x_copy(&x,A2(g),sizeof(D3D12_VIEWPORT)*(size_t)n)); wx_done(&x); return 1; }
    case 22: { u32 n=(u32)A1(g); WX x; wx_init(&x,g); x_count(&x,n,4096,"scissor rectangles"); ID3D12GraphicsCommandList_RSSetScissorRects(l,n,x_copy(&x,A2(g),sizeof(D3D12_RECT)*(size_t)n)); wx_done(&x); return 1; }
    case 23: { FLOAT f[4]; ID3D12GraphicsCommandList_OMSetBlendFactor(l,A1(g)?(memcpy(f,hp(g,A1(g)),16),f):NULL); return 1; }
    case 24: ID3D12GraphicsCommandList_OMSetStencilRef(l,(UINT)A1(g)); return 1;
    case 25: ID3D12GraphicsCommandList_SetPipelineState(l,(ID3D12PipelineState*)kh_wrap_real(g,A1(g),WK_PSO,"SetPipelineState")); atomic_fetch_add(&gfx_pso_binds,1); return 1;
    case 26: { u32 n=(u32)A1(g); WX x; wx_init(&x,g); D3D12_RESOURCE_BARRIER *b=x_barriers(&x,A2(g),n);                /* ResourceBarrier(count, barriers) */
        ID3D12GraphicsCommandList_ResourceBarrier(l,n,b); atomic_fetch_add(&gfx_barriers,n); kh_gfxc_census_barriers(b,n); wx_done(&x); return 1; }
    case 27: kh_gfxc_bundle_replay(g,l,A1(g)); return 1;                          /* ExecuteBundle: the recording is replayed here (vkd3d has no bundles) */
    case 28: { u32 n=(u32)A1(g); WX x; wx_init(&x,g); void **h=x_ifaces(&x,A2(g),n,WK_DHEAP,"SetDescriptorHeaps");      /* SetDescriptorHeaps(count, heaps) */
        ID3D12GraphicsCommandList_SetDescriptorHeaps(l,n,(ID3D12DescriptorHeap*const*)h); kh_gfxc_track_heaps(g,U(l),h?n:0,(const u64*)h); wx_done(&x); return 1; }
    case 30: { void *rs=kh_wrap_real(g,A1(g),WK_ROOTSIG,"SetGraphicsRootSignature"); ID3D12GraphicsCommandList_SetGraphicsRootSignature(l,(ID3D12RootSignature*)rs); kh_gfxc_track_rootsig(g,U(l),U(rs)); return 1; }
    case 32: ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l,(UINT)A1(g),gpuh(A2(g))); kh_gfxc_track_table(g,U(l),(u32)A1(g)); return 1;
    case 38: ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l,(UINT)A1(g),(D3D12_GPU_VIRTUAL_ADDRESS)A2(g)); return 1;
    case 43: { D3D12_INDEX_BUFFER_VIEW v; ID3D12GraphicsCommandList_IASetIndexBuffer(l,CP(v,A1(g))); return 1; }
    case 44: { u32 n=(u32)A2(g); WX x; wx_init(&x,g); x_count(&x,n,4096,"vertex-buffer views"); ID3D12GraphicsCommandList_IASetVertexBuffers(l,(UINT)A1(g),n,x_copy(&x,A3(g),sizeof(D3D12_VERTEX_BUFFER_VIEW)*(size_t)n)); wx_done(&x); return 1; }
    case 46: { u32 n=(u32)A1(g); BOOL single=(BOOL)(u32)A3(g); u32 cnt=single?(n?1u:0u):n; D3D12_CPU_DESCRIPTOR_HANDLE rt[8], ds;   /* OMSetRenderTargets(count, rt handles, single range, ds handle) */
        if(n>8) kh_fatal(g,"gfx-wrap-desc: OMSetRenderTargets with %u render targets (D3D12 allows 8)",n);
        if(A2(g)&&cnt) memcpy(rt,hp(g,A2(g)),8ull*cnt);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l,n,A2(g)&&cnt?rt:NULL,single,CP(ds,ARG(g,4))); return 1; }
    case 47: { float depth; memcpy(&depth,&KH_XMM(g,3),4); u32 nr=(u32)ARG(g,5); WX x; wx_init(&x,g); x_count(&x,nr,4096,"clear rectangles");   /* ClearDepthStencilView(dsv, flags, depth [xmm3], stencil, rects, rect array) */
        ID3D12GraphicsCommandList_ClearDepthStencilView(l,cpuh(A1(g)),(D3D12_CLEAR_FLAGS)(u32)A2(g),depth,(UINT8)ARG(g,4),nr,x_copy(&x,ARG(g,6),sizeof(D3D12_RECT)*(size_t)nr)); wx_done(&x);
        atomic_fetch_add(&gfx_clears,1); return 1; }
    case 48: { FLOAT col[4]; u32 nr=(u32)A3(g); WX x; wx_init(&x,g); x_count(&x,nr,4096,"clear rectangles");           /* ClearRenderTargetView(rtv, colour, rects, rect array) */
        if(!A2(g)) kh_fatal(g,"gfx-wrap-desc: ClearRenderTargetView without a colour"); memcpy(col,hp(g,A2(g)),16);
        ID3D12GraphicsCommandList_ClearRenderTargetView(l,cpuh(A1(g)),col,nr,x_copy(&x,ARG(g,4),sizeof(D3D12_RECT)*(size_t)nr)); wx_done(&x); atomic_fetch_add(&gfx_clears,1); return 1; }
    case 52: ID3D12GraphicsCommandList_BeginQuery(l,(ID3D12QueryHeap*)kh_wrap_real(g,A1(g),WK_QHEAP,"BeginQuery"),(D3D12_QUERY_TYPE)(u32)A2(g),(UINT)A3(g)); return 1;
    case 53: ID3D12GraphicsCommandList_EndQuery(l,(ID3D12QueryHeap*)kh_wrap_real(g,A1(g),WK_QHEAP,"EndQuery"),(D3D12_QUERY_TYPE)(u32)A2(g),(UINT)A3(g)); return 1;
    case 54: ID3D12GraphicsCommandList_ResolveQueryData(l,(ID3D12QueryHeap*)kh_wrap_real(g,A1(g),WK_QHEAP,"ResolveQueryData"),(D3D12_QUERY_TYPE)(u32)A2(g),(UINT)A3(g),(UINT)ARG(g,4),RES(ARG(g,5),"ResolveQueryData: destination"),ARG(g,6)); return 1;
    }
    return 0;
}
static int w_res(KCtx *g,ID3D12Resource *r,int s){
    switch(s){
    case 8: { D3D12_RANGE rg; void *p=NULL; u64 pp=A3(g);                         /* Map(subresource, read range, **data): the game gets host - bias */
        HRESULT hr=ID3D12Resource_Map(r,(UINT)A1(g),CP(rg,A2(g)),pp?&p:NULL);
        if(pp) wr64(g,pp,SUCCEEDED(hr)?host_to_guest(g,p,"ID3D12Resource::Map"):0);
        atomic_fetch_add(&n_maps,1); SET_RAX(g,(u32)hr); return 1; }
    case 9: { D3D12_RANGE rg; ID3D12Resource_Unmap(r,(UINT)A1(g),CP(rg,A2(g))); return 1; }
    case 10: { u64 ret=A1(g); D3D12_RESOURCE_DESC d=ID3D12Resource_GetDesc(r); memcpy(hp(g,ret),&d,sizeof d); SET_RAX(g,ret); return 1; }     /* struct result through the hidden pointer */
    case 11: SET_RAX(g,ID3D12Resource_GetGPUVirtualAddress(r)); return 1;
    case 12: { D3D12_BOX box; SET_RAX(g,(u32)ID3D12Resource_WriteToSubresource(r,(UINT)A1(g),CP(box,A2(g)),A3(g)?hp(g,A3(g)):NULL,(UINT)ARG(g,4),(UINT)ARG(g,5))); return 1; }
    case 13: { D3D12_BOX box; SET_RAX(g,(u32)ID3D12Resource_ReadFromSubresource(r,A1(g)?hp(g,A1(g)):NULL,(UINT)A2(g),(UINT)A3(g),(UINT)ARG(g,4),CP(box,ARG(g,5)))); return 1; }
    case 14: { D3D12_HEAP_PROPERTIES hprops; D3D12_HEAP_FLAGS fl=0; HRESULT hr=ID3D12Resource_GetHeapProperties(r,A1(g)?&hprops:NULL,A2(g)?&fl:NULL);
        if(SUCCEEDED(hr)){ if(A1(g)) memcpy(hp(g,A1(g)),&hprops,sizeof hprops); if(A2(g)) wr32(g,A2(g),(u32)fl); } SET_RAX(g,(u32)hr); return 1; }
    }
    return 0;
}
static int w_dheap(KCtx *g,ID3D12DescriptorHeap *h,int s){
    u64 ret=A1(g);
    switch(s){                                                                    /* all three: struct result through the hidden pointer, rax = that pointer */
    case 8: { D3D12_DESCRIPTOR_HEAP_DESC d=ID3D12DescriptorHeap_GetDesc(h); memcpy(hp(g,ret),&d,sizeof d); SET_RAX(g,ret); return 1; }
    case 9: { D3D12_CPU_DESCRIPTOR_HANDLE c=ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h); wr64(g,ret,(u64)c.ptr); SET_RAX(g,ret); return 1; }
    case 10: { D3D12_GPU_DESCRIPTOR_HANDLE c=ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h); wr64(g,ret,c.ptr); SET_RAX(g,ret); return 1; }
    }
    return 0;
}
static int w_fence(KCtx *g,ID3D12Fence *f,int s){
    switch(s){
    case 8: SET_RAX(g,ID3D12Fence_GetCompletedValue(f)); return 1;
    case 9: SET_RAX(g,(u32)ID3D12Fence_SetEventOnCompletion(f,A1(g),(HANDLE)(uintptr_t)A2(g))); return 1;     /* the HANDLE is an id of OUR handle table (pfn_signal_event) */
    case 10: SET_RAX(g,(u32)ID3D12Fence_Signal(f,A1(g))); return 1;
    }
    return 0;
}
static int w_heap(KCtx *g,ID3D12Heap *h,int s){ if(s==8){ u64 ret=A1(g); D3D12_HEAP_DESC d=ID3D12Heap_GetDesc(h); memcpy(hp(g,ret),&d,sizeof d); SET_RAX(g,ret); return 1; } return 0; }
static int w_blob(KCtx *g,ID3D10Blob *b,int s){
    if(s==3){ SET_RAX(g,host_to_guest(g,ID3D10Blob_GetBufferPointer(b),"ID3DBlob::GetBufferPointer")); return 1; }
    if(s==4){ SET_RAX(g,(u64)ID3D10Blob_GetBufferSize(b)); return 1; }
    return 0;
}
/* every vtable slot of every wrapped class ends here (com.c: com_thunk -> the class handler) */
static int w_call(KCtx *g,u64 o,int s,int kind){
    if(s>=MAXSLOT) kh_fatal(g,"gfx-wrap-unwrapped-method: %s vtable slot %d was called on guest object 0x%llx",wk[kind].iface,s,P(o));
    atomic_fetch_add_explicit(&wcalls[kind][s],1,memory_order_relaxed);
    u64 real=rd64(g,o+W_REAL);
    if(__builtin_expect(!real||rd64(g,o+W_COOKIE)!=(real^o^W_MAGIC)||rd32(g,o+W_KIND)!=(u32)kind,0))
        kh_fatal(g,"gfx-wrap-dead-object: %s::%s was called on guest object 0x%llx, which is not a live %s of the wrapped bridge (released already?)",wk[kind].iface,mname(kind,s),P(o),wk[kind].iface);
    void *r=(void*)(uintptr_t)real; int ok=1;
    if(__builtin_expect(kh_native_trace,0)) fprintf(stderr,"kh: wrap t%u %s::%s(%llx [real %p], %llx, %llx, %llx | %llx, %llx, %llx, %llx)\n",cur_thread()?cur_thread()->tid:0,wk[kind].iface,mname(kind,s),P(o),r,
                                                    P(A1(g)),P(A2(g)),P(A3(g)),P(ARG(g,4)),P(ARG(g,5)),P(ARG(g,6)),P(ARG(g,7)));
    if(s<3) w_unknown(g,o,r,kind,s);
    else if(kind!=WK_BLOB&&s<8&&w_object(g,r,kind,s)){ }
    else switch(kind){
        case WK_DEVICE: ok=w_dev(g,(ID3D12Device*)r,s); break;
        case WK_QUEUE: ok=w_queue(g,(ID3D12CommandQueue*)r,s); break;
        case WK_ALLOCATOR: ok=w_alloc(g,(ID3D12CommandAllocator*)r,s); break;
        case WK_LIST: ok=w_list(g,(ID3D12GraphicsCommandList*)r,s); break;
        case WK_RESOURCE: ok=w_res(g,(ID3D12Resource*)r,s); break;
        case WK_DHEAP: ok=w_dheap(g,(ID3D12DescriptorHeap*)r,s); break;
        case WK_FENCE: ok=w_fence(g,(ID3D12Fence*)r,s); break;
        case WK_HEAP: ok=w_heap(g,(ID3D12Heap*)r,s); break;
        case WK_BLOB: ok=w_blob(g,(ID3D10Blob*)r,s); break;
        default: ok=0; }                                                          /* pipeline state, root signature, query heap: nothing beyond GetDevice */
    if(!ok) kh_fatal(g,"gfx-wrap-unwrapped-method: %s::%s (vtable slot %d) was called on guest object 0x%llx and the wrapped bridge has no wrapper for it (rdx=%llx r8=%llx r9=%llx)",
                     wk[kind].iface,mname(kind,s),s,P(o),P(A1(g)),P(A2(g)),P(A3(g)));
    if(__builtin_expect(kh_native_trace,0)) fprintf(stderr,"kh: wrap t%u   -> %llx\n",cur_thread()?cur_thread()->tid:0,P(KH_RAX(g)));
    return 1;
}
#define HK(n) static int h_k##n(KCtx *g,u64 o,int s,const char *c){ (void)c; return w_call(g,o,s,n); }
HK(0) HK(1) HK(2) HK(3) HK(4) HK(5) HK(6) HK(7) HK(8) HK(9) HK(10) HK(11)
static const KhComHandler hk[WK_N]={h_k0,h_k1,h_k2,h_k3,h_k4,h_k5,h_k6,h_k7,h_k8,h_k9,h_k10,h_k11};
_Static_assert(WK_N==12,"one handler per wrapped interface");
void kh_wrap_classes(void){
    static pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&m);
    if(!classes_done){
        wk[WK_DEVICE].names=kh_gfxc_dev_names; wk[WK_DEVICE].nnames=kh_gfxc_dev_nnames; wk[WK_QUEUE].names=kh_gfxc_queue_names; wk[WK_QUEUE].nnames=kh_gfxc_queue_nnames;
        wk[WK_ALLOCATOR].names=kh_gfxc_alloc_names; wk[WK_ALLOCATOR].nnames=kh_gfxc_alloc_nnames; wk[WK_LIST].names=kh_gfxc_list_names; wk[WK_LIST].nnames=kh_gfxc_list_nnames;
        wk[WK_RESOURCE].names=kh_gfxc_res_names; wk[WK_RESOURCE].nnames=kh_gfxc_res_nnames; wk[WK_DHEAP].names=kh_gfxc_dheap_names; wk[WK_DHEAP].nnames=kh_gfxc_dheap_nnames;
        wk[WK_FENCE].names=kh_gfxc_fence_names; wk[WK_FENCE].nnames=kh_gfxc_fence_nnames;
        for(int k=0;k<WK_N;k++){ if(wk[k].nslots>MAXSLOT||wk[k].nnames>wk[k].nslots) kh_fatal(NULL,"gfx-wrap-classes: %s has %d methods and %d vtable slots (limit %d)",wk[k].iface,wk[k].nnames,wk[k].nslots,MAXSLOT);
            C_w[k]=kh_com_class(wk[k].cls,wk[k].nslots,COM_STRICT,hk[k]); }
        classes_done=1; }
    pthread_mutex_unlock(&m);
}

/* D3D12SerializeRootSignature(desc, version, **blob, **error blob): the blobs are vkd3d-utils objects behind guest blob objects */
void kh_wrap_serialize_rootsig(KCtx *g){
    u32 ver=(u32)A1(g); u64 pb=A2(g), pe=A3(g);
    if(ver!=D3D_ROOT_SIGNATURE_VERSION_1_0) kh_fatal(g,"gfx-wrap-rootsig-version: D3D12SerializeRootSignature with version %u: only the version 1.0 desc layout is translated",ver);
    WX x; wx_init(&x,g); ID3DBlob *blob=NULL,*err=NULL;
    HRESULT hr=D3D12SerializeRootSignature(x_rootsig(&x,A0(g)),(D3D_ROOT_SIGNATURE_VERSION)ver,pb?&blob:NULL,pe?&err:NULL); wx_done(&x);
    if(FAILED(hr)) fprintf(stderr,"kh: gfx vkd3d: D3D12SerializeRootSignature(version %u) FAILED hr=0x%08x\n",ver,(unsigned)hr);
    if(pb) wr64(g,pb,blob?w_get(g,blob,WK_BLOB,1):0); if(pe) wr64(g,pe,err?w_get(g,err,WK_BLOB,1):0);
    SET_RAX(g,(u32)hr);
}

/* ---- report ------------------------------------------------------------------------------------------------------------------------- */
void kh_wrap_report(FILE *f){
    u64 total=0; for(int k=0;k<WK_N;k++) for(int s=0;s<MAXSLOT;s++) total+=wcalls[k][s];
    fprintf(f,"kh: gfx wrap: wrapped calls=%llu, calls through the Windows-ABI trampoline=%llu, Map calls=%llu; guest objects made / gone (live):",P(total),P(kh_native_calls),P(n_maps));
    for(int k=0;k<WK_N;k++) if(n_made[k]) fprintf(f," %s %llu/%llu (%llu)",wk[k].iface+(strncmp(wk[k].iface,"ID3D12",6)?0:6),P(n_made[k]),P(n_dead[k]),P(n_made[k]-n_dead[k]));
    fputc('\n',f);
    for(int k=0;k<WK_N;k++){ u64 t=0; for(int s=0;s<MAXSLOT;s++) t+=wcalls[k][s]; if(!t) continue;
        fprintf(f,"kh: gfx wrap calls %s (%llu):",wk[k].iface,P(t)); for(int s=0;s<MAXSLOT;s++) if(wcalls[k][s]) fprintf(f," %s=%llu",mname(k,s),P(wcalls[k][s])); fputc('\n',f); }
}

/* ---- selftests ------------------------------------------------------------------------------------------------------------------------ */
static int st_n, st_f;
#define T(c) do{ st_n++; if(!(c)){ st_f++; fprintf(stderr,"gfx wrap selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
/* SYNTHETIC guest memory: a host block that stands for the guest addresses SG_BASE.. .  Its bias (host - guest) is neither 0 nor the
 * process' real bias, so a translator that forgot a pointer, or used the wrong translation, cannot pass by accident in any build. */
#define SG_BASE 0x5a7700000000ull
typedef struct { u8 *mem; size_t used, cap; } SynG;
static u64 sg_put(SynG *s,const void *data,size_t n,size_t align){
    s->used=(s->used+align-1)&~(align-1); if(s->used+n>s->cap){ fprintf(stderr,"gfx wrap selftest: synthetic memory exhausted\n"); abort(); }
    u64 a=SG_BASE+s->used; if(data) memcpy(s->mem+s->used,data,n); else memset(s->mem+s->used,0,n); s->used+=n; return a;
}
static void *sg_host(SynG *s,u64 a){ return s->mem+(a-SG_BASE); }
/* the image of a wrapper object for a (fake) real pointer, in synthetic memory */
static u64 sg_obj(SynG *s,int kind,u64 real){
    u8 img[32+W_EXTRA]; memset(img,0,sizeof img); s->used=(s->used+15)&~(size_t)15; u64 a=SG_BASE+s->used, vt=kh_com_vtbl(C_w[kind]), ck=real^a^W_MAGIC; u32 k=(u32)kind;
    memcpy(img,&vt,8); memcpy(img+W_REAL,&real,8); memcpy(img+W_COOKIE,&ck,8); memcpy(img+W_KIND,&k,4); return sg_put(s,img,sizeof img,16);
}
#define GV(T,a) ((T)(uintptr_t)(a))            /* a guest address as the pointer VALUE inside a guest structure */
static int outside(const SynG *s,const void *p){ return (const u8*)p<s->mem||(const u8*)p>=s->mem+s->cap; }
static void w_forget(KCtx *g,void *real){ pthread_mutex_lock(&wm); WEnt *e=wt_find(real); if(e) w_kill(g,e); pthread_mutex_unlock(&wm); }   /* test objects on fake pointers: no real Release */
/* the named fatals: each case runs in a child, which must end with exit code 3 (kh_fatal) and the expected name on stderr */
static int fatal_case(KCtx *g,int which,const char *expect){
    FILE *f=tmpfile(); if(!f) return 0;
    fflush(stderr); pid_t child=fork();
    if(child==0){ dup2(fileno(f),2); void *fake=(void*)(uintptr_t)0x7e5700aa00000000ull;
        switch(which){
        case 0: { u64 o=kh_wrap_obj(g,fake,WK_DEVICE); GC(VT(o,11),o,0,0,0); break; }                      /* ID3D12Device::CreateComputePipelineState */
        case 1: { u64 o=kh_wrap_obj(g,fake,WK_LIST); GC(VT(o,14),o,1,1,1); break; }                        /* ID3D12GraphicsCommandList::Dispatch */
        case 2: { u64 o=kh_wrap_obj(g,fake,WK_DEVICE); GC(VT(o,44),o,0,0,0); break; }                      /* a slot of ID3D12Device1 */
        case 3: { u64 junk=ga_calloc(1,64); kh_wrap_real(g,junk,WK_RESOURCE,"selftest argument"); break; } /* guest memory that is no wrapper */
        case 4: { u64 o=kh_wrap_obj(g,fake,WK_FENCE); kh_wrap_real(g,o,WK_RESOURCE,"selftest argument"); break; }   /* a wrapper of another interface */
        case 5: { u64 o=kh_wrap_obj(g,fake,WK_FENCE), fn=VT(o,8); w_forget(g,fake); GC(fn,o); break; }     /* a method on a released object */
        case 6: { u64 o=kh_wrap_obj(g,fake,WK_RESOURCE); GC(VT(o,5),o,GP(&IID_ID3D12Device),o); break; }   /* SetPrivateDataInterface with an interface */
        case 7: { u64 o=kh_wrap_obj(g,fake,WK_DEVICE); GC(VT(o,13),o,D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES,ga_calloc(1,64),32); break; }   /* an untranslated feature structure */
        }
        _exit(99); }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char out[8192]; size_t n=fread(out,1,sizeof out-1,f); out[n]=0; fclose(f);
    int ok=WIFEXITED(status)&&WEXITSTATUS(status)==3&&strstr(out,expect)!=NULL;
    if(!ok) fprintf(stderr,"gfx wrap selftest: fatal case %d: exit status 0x%x, expected exit code 3 with '%s'; child said: %.300s\n",which,status,expect,out);
    return ok;
}
/* D3D12SerializeRootSignature as the wrapped bridge runs it (rt_imp_d3d12__D3D12SerializeRootSignature -> kh_vkd3d_serialize_rootsig ->
 * kh_wrap_serialize_rootsig, then the import's ret), callable here without a device, where the import itself takes the null path */
static void st_serialize_imp(KCtx *g){ kh_wrap_serialize_rootsig(g); KH_RET(g); }
/* no device needed: runs in every --selftest of a binary that has the backend */
int kh_wrap_selftest_pure(KCtx *g){
    st_n=st_f=0; kh_gfxc_st_fails=0; kh_wrap_classes();
    SynG s; s.cap=1u<<17; s.used=64; s.mem=malloc(s.cap); if(!s.mem){ fprintf(stderr,"gfx wrap selftest FAIL: out of memory\n"); return 1; } memset(s.mem,0xCD,s.cap);
    WX x; x.bias=(uintptr_t)s.mem-(uintptr_t)SG_BASE; x.g=g; x.synth=1; x.used=0; x.nbig=0; x.chain=NULL;
    T(x.bias!=0); T(x.bias!=(uintptr_t)XL(g,0));
    T(wx_ptr(&x,0)==NULL); T(wx_ptr(&x,SG_BASE+100)==(void*)(s.mem+100));
    u64 fr1=0x7e57000000002000ull, fr2=0x7e57000000003000ull, frs=0x7e57000000001230ull;
    u64 o1=sg_obj(&s,WK_RESOURCE,fr1), o2=sg_obj(&s,WK_RESOURCE,fr2), ors=sg_obj(&s,WK_ROOTSIG,frs);
    T(wx_unwrap(&x,o1,WK_RESOURCE,"selftest")==(void*)(uintptr_t)fr1); T(wx_unwrap(&x,ors,-1,"selftest")==(void*)(uintptr_t)frs); T(wx_unwrap(&x,0,WK_RESOURCE,"selftest")==NULL);
    /* D3D12_GRAPHICS_PIPELINE_STATE_DESC: root signature, four of five bytecode pointers (GS stays NULL), input layout with two semantic
     * names, stream-output declaration with a name, strides, cached blob */
    { static const u8 vsb[24]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24}, psb[40]={0xA0,0xA1,0xA2,0xA3,0xA4}, dsb[8]={0xD5}, hsb[12]={0x45}, cache[12]={0xCA,0xC4,0xE0};
      u64 a_vs=sg_put(&s,vsb,sizeof vsb,4), a_ps=sg_put(&s,psb,sizeof psb,4), a_ds=sg_put(&s,dsb,sizeof dsb,4), a_hs=sg_put(&s,hsb,sizeof hsb,4), a_cache=sg_put(&s,cache,sizeof cache,1);
      u64 a_pos=sg_put(&s,"POSITION",9,1), a_tex=sg_put(&s,"TEXCOORD",9,1), a_so=sg_put(&s,"SV_Position",12,1);
      D3D12_INPUT_ELEMENT_DESC ie[2]; memset(ie,0,sizeof ie); ie[0].SemanticName=GV(const char*,a_pos); ie[0].Format=DXGI_FORMAT_R32G32B32_FLOAT;
      ie[1].SemanticName=GV(const char*,a_tex); ie[1].SemanticIndex=1; ie[1].Format=DXGI_FORMAT_R32G32_FLOAT; ie[1].AlignedByteOffset=12; u64 a_ie=sg_put(&s,ie,sizeof ie,8);
      D3D12_SO_DECLARATION_ENTRY so; memset(&so,0,sizeof so); so.SemanticName=GV(const char*,a_so); so.ComponentCount=4; so.OutputSlot=1; u64 a_sod=sg_put(&s,&so,sizeof so,8);
      UINT strides[2]={16,32}; u64 a_str=sg_put(&s,strides,sizeof strides,4);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd,0,sizeof pd); pd.pRootSignature=GV(ID3D12RootSignature*,ors);
      pd.VS.pShaderBytecode=GV(const void*,a_vs); pd.VS.BytecodeLength=sizeof vsb; pd.PS.pShaderBytecode=GV(const void*,a_ps); pd.PS.BytecodeLength=sizeof psb;
      pd.DS.pShaderBytecode=GV(const void*,a_ds); pd.DS.BytecodeLength=sizeof dsb; pd.HS.pShaderBytecode=GV(const void*,a_hs); pd.HS.BytecodeLength=sizeof hsb;
      pd.StreamOutput.pSODeclaration=GV(const D3D12_SO_DECLARATION_ENTRY*,a_sod); pd.StreamOutput.NumEntries=1; pd.StreamOutput.pBufferStrides=GV(const UINT*,a_str); pd.StreamOutput.NumStrides=2;
      pd.InputLayout.pInputElementDescs=GV(const D3D12_INPUT_ELEMENT_DESC*,a_ie); pd.InputLayout.NumElements=2; pd.CachedPSO.pCachedBlob=GV(const void*,a_cache); pd.CachedPSO.CachedBlobSizeInBytes=sizeof cache;
      pd.SampleMask=0x12345678u; pd.NumRenderTargets=3; pd.RTVFormats[2]=DXGI_FORMAT_B8G8R8A8_UNORM; pd.DSVFormat=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; pd.SampleDesc.Count=8; pd.NodeMask=7; pd.BlendState.RenderTarget[7].RenderTargetWriteMask=0xf;
      u64 a_pd=sg_put(&s,&pd,sizeof pd,8); u8 before[sizeof pd]; memcpy(before,sg_host(&s,a_pd),sizeof pd);
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *r=x_pso(&x,a_pd);
      T(r&&outside(&s,r)); T(!memcmp(before,sg_host(&s,a_pd),sizeof pd));                       /* a private copy; the game's structure is untouched */
      T(U(r->pRootSignature)==frs);
      T(r->VS.pShaderBytecode==sg_host(&s,a_vs)&&r->VS.BytecodeLength==sizeof vsb&&!memcmp(r->VS.pShaderBytecode,vsb,sizeof vsb));
      T(r->PS.pShaderBytecode==sg_host(&s,a_ps)&&r->PS.BytecodeLength==sizeof psb&&!memcmp(r->PS.pShaderBytecode,psb,sizeof psb));
      T(r->DS.pShaderBytecode==sg_host(&s,a_ds)&&r->DS.BytecodeLength==sizeof dsb); T(r->HS.pShaderBytecode==sg_host(&s,a_hs)&&r->HS.BytecodeLength==sizeof hsb);
      T(r->GS.pShaderBytecode==NULL&&r->GS.BytecodeLength==0);                                  /* NULL stays NULL (it must not become the bias) */
      T(r->InputLayout.NumElements==2&&outside(&s,r->InputLayout.pInputElementDescs));
      T(r->InputLayout.pInputElementDescs[0].SemanticName==(const char*)sg_host(&s,a_pos)&&!strcmp(r->InputLayout.pInputElementDescs[0].SemanticName,"POSITION")&&r->InputLayout.pInputElementDescs[0].Format==DXGI_FORMAT_R32G32B32_FLOAT);
      T(r->InputLayout.pInputElementDescs[1].SemanticName==(const char*)sg_host(&s,a_tex)&&!strcmp(r->InputLayout.pInputElementDescs[1].SemanticName,"TEXCOORD")&&r->InputLayout.pInputElementDescs[1].SemanticIndex==1&&r->InputLayout.pInputElementDescs[1].AlignedByteOffset==12);
      T(r->StreamOutput.NumEntries==1&&outside(&s,r->StreamOutput.pSODeclaration)&&r->StreamOutput.pSODeclaration[0].SemanticName==(const char*)sg_host(&s,a_so)&&!strcmp(r->StreamOutput.pSODeclaration[0].SemanticName,"SV_Position")&&r->StreamOutput.pSODeclaration[0].ComponentCount==4&&r->StreamOutput.pSODeclaration[0].OutputSlot==1);
      T(r->StreamOutput.pBufferStrides==(const UINT*)sg_host(&s,a_str)&&r->StreamOutput.NumStrides==2&&r->StreamOutput.pBufferStrides[1]==32);
      T(r->CachedPSO.pCachedBlob==sg_host(&s,a_cache)&&r->CachedPSO.CachedBlobSizeInBytes==sizeof cache);
      T(r->SampleMask==0x12345678u&&r->NumRenderTargets==3&&r->RTVFormats[2]==DXGI_FORMAT_B8G8R8A8_UNORM&&r->DSVFormat==DXGI_FORMAT_D32_FLOAT_S8X24_UINT&&r->SampleDesc.Count==8&&r->NodeMask==7&&r->BlendState.RenderTarget[7].RenderTargetWriteMask==0xf);
      T(x_pso(&x,0)==NULL); wx_done(&x); }
    /* D3D12_RESOURCE_BARRIER[]: the three union members, a NULL interface, flags and states kept; 200 barriers use the heap path */
    { D3D12_RESOURCE_BARRIER b[4]; memset(b,0,sizeof b);
      b[0].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[0].Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY; b[0].Transition.pResource=GV(ID3D12Resource*,o1); b[0].Transition.Subresource=0xffffffffu; b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b[0].Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b[1].Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING; b[1].Aliasing.pResourceBefore=NULL; b[1].Aliasing.pResourceAfter=GV(ID3D12Resource*,o2);
      b[2].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b[2].UAV.pResource=GV(ID3D12Resource*,o1);
      b[3].Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING; b[3].Aliasing.pResourceBefore=GV(ID3D12Resource*,o2); b[3].Aliasing.pResourceAfter=GV(ID3D12Resource*,o1);
      u64 a_b=sg_put(&s,b,sizeof b,8); const D3D12_RESOURCE_BARRIER *r=x_barriers(&x,a_b,4);
      T(r&&outside(&s,r)); T(!memcmp(b,sg_host(&s,a_b),sizeof b));
      T(U(r[0].Transition.pResource)==fr1&&r[0].Transition.Subresource==0xffffffffu&&r[0].Transition.StateBefore==D3D12_RESOURCE_STATE_RENDER_TARGET&&r[0].Transition.StateAfter==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE&&r[0].Flags==D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
      T(r[1].Aliasing.pResourceBefore==NULL&&U(r[1].Aliasing.pResourceAfter)==fr2); T(U(r[2].UAV.pResource)==fr1); T(U(r[3].Aliasing.pResourceBefore)==fr2&&U(r[3].Aliasing.pResourceAfter)==fr1);
      T(x_barriers(&x,0,4)==NULL); T(x_barriers(&x,a_b,0)==NULL); wx_done(&x);
      D3D12_RESOURCE_BARRIER *many=calloc(200,sizeof *many); for(int i=0;i<200;i++){ many[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; many[i].Transition.pResource=GV(ID3D12Resource*,i&1?o1:o2); many[i].Transition.Subresource=(UINT)i; }
      u64 a_m=sg_put(&s,many,200*sizeof *many,8); r=x_barriers(&x,a_m,200); int ok=x.nbig==1; for(int i=0;i<200;i++) ok&=U(r[i].Transition.pResource)==(i&1?fr1:fr2)&&r[i].Transition.Subresource==(UINT)i;
      T(ok); wx_done(&x); T(x.nbig==0&&x.used==0); free(many); }
    /* D3D12_TEXTURE_COPY_LOCATION: both union members */
    { D3D12_TEXTURE_COPY_LOCATION l1,l2; memset(&l1,0,sizeof l1); memset(&l2,0,sizeof l2); l1.pResource=GV(ID3D12Resource*,o1); l1.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; l1.SubresourceIndex=5;
      l2.pResource=GV(ID3D12Resource*,o2); l2.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; l2.PlacedFootprint.Offset=0x123456789abull; l2.PlacedFootprint.Footprint.Format=DXGI_FORMAT_BC3_UNORM; l2.PlacedFootprint.Footprint.Width=256; l2.PlacedFootprint.Footprint.Height=128; l2.PlacedFootprint.Footprint.Depth=1; l2.PlacedFootprint.Footprint.RowPitch=1024;
      u64 a1=sg_put(&s,&l1,sizeof l1,8), a2=sg_put(&s,&l2,sizeof l2,8); const D3D12_TEXTURE_COPY_LOCATION *r1=x_copyloc(&x,a1,"selftest"), *r2=x_copyloc(&x,a2,"selftest");
      T(U(r1->pResource)==fr1&&r1->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&r1->SubresourceIndex==5);
      T(U(r2->pResource)==fr2&&r2->PlacedFootprint.Offset==0x123456789abull&&r2->PlacedFootprint.Footprint.Format==DXGI_FORMAT_BC3_UNORM&&r2->PlacedFootprint.Footprint.Width==256&&r2->PlacedFootprint.Footprint.Height==128&&r2->PlacedFootprint.Footprint.RowPitch==1024);
      T(x_copyloc(&x,0,"selftest")==NULL); T(!memcmp(&l2,sg_host(&s,a2),sizeof l2)); wx_done(&x); }
    /* D3D12_ROOT_SIGNATURE_DESC: a table with two ranges, a CBV whose register / space words must NOT be taken for a pointer, constants,
     * a static sampler; then the proof that vkd3d reads the rebuilt structure like the original: both serialise to the same blob */
    { D3D12_DESCRIPTOR_RANGE rg[2]; memset(rg,0,sizeof rg); rg[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors=2; rg[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_CBV; rg[1].NumDescriptors=1; rg[1].BaseShaderRegister=3; rg[1].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
      D3D12_DESCRIPTOR_RANGE rgb[1]; memset(rgb,0,sizeof rgb); rgb[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rgb[0].NumDescriptors=1; rgb[0].BaseShaderRegister=5;
      u64 a_rg=sg_put(&s,rg,sizeof rg,4), a_rgb=sg_put(&s,rgb,sizeof rgb,4);
      D3D12_ROOT_PARAMETER rp[4]; memset(rp,0,sizeof rp);
      rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges=1; rp[0].DescriptorTable.pDescriptorRanges=GV(const D3D12_DESCRIPTOR_RANGE*,a_rgb); rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
      rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; rp[1].Descriptor.ShaderRegister=0x11223344u; rp[1].Descriptor.RegisterSpace=0x55667788u; rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
      rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[2].Constants.ShaderRegister=1; rp[2].Constants.Num32BitValues=4;
      rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[3].DescriptorTable.NumDescriptorRanges=2; rp[3].DescriptorTable.pDescriptorRanges=GV(const D3D12_DESCRIPTOR_RANGE*,a_rg); rp[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
      u64 a_rp=sg_put(&s,rp,sizeof rp,8);
      D3D12_STATIC_SAMPLER_DESC ss; memset(&ss,0,sizeof ss); ss.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR; ss.AddressU=ss.AddressV=ss.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; ss.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER; ss.MaxLOD=1000.0f; ss.ShaderRegister=9; ss.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
      u64 a_ss=sg_put(&s,&ss,sizeof ss,4);
      D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd,0,sizeof rd); rd.NumParameters=4; rd.pParameters=GV(const D3D12_ROOT_PARAMETER*,a_rp); rd.NumStaticSamplers=1; rd.pStaticSamplers=GV(const D3D12_STATIC_SAMPLER_DESC*,a_ss); rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
      u64 a_rd=sg_put(&s,&rd,sizeof rd,8); const D3D12_ROOT_SIGNATURE_DESC *r=x_rootsig(&x,a_rd);
      T(r&&outside(&s,r)&&r->NumParameters==4&&outside(&s,r->pParameters)&&r->Flags==D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
      T(r->pParameters[0].DescriptorTable.NumDescriptorRanges==1&&r->pParameters[0].DescriptorTable.pDescriptorRanges[0].BaseShaderRegister==5);
      T(r->pParameters[1].ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV&&r->pParameters[1].Descriptor.ShaderRegister==0x11223344u&&r->pParameters[1].Descriptor.RegisterSpace==0x55667788u);
      T(r->pParameters[2].Constants.ShaderRegister==1&&r->pParameters[2].Constants.Num32BitValues==4);
      T(r->pParameters[3].DescriptorTable.NumDescriptorRanges==2&&r->pParameters[3].DescriptorTable.pDescriptorRanges[1].RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_CBV&&r->pParameters[3].DescriptorTable.pDescriptorRanges[1].OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND&&r->pParameters[3].DescriptorTable.pDescriptorRanges[1].BaseShaderRegister==3&&r->pParameters[3].ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL);
      T(r->NumStaticSamplers==1&&r->pStaticSamplers[0].ShaderRegister==9&&r->pStaticSamplers[0].MaxLOD==1000.0f);
      T(!memcmp(rp,sg_host(&s,a_rp),sizeof rp));
      rp[1].Descriptor.ShaderRegister=2; rp[1].Descriptor.RegisterSpace=0; memcpy(sg_host(&s,a_rp),rp,sizeof rp); wx_done(&x); r=x_rootsig(&x,a_rd);   /* a register vkd3d accepts */
      D3D12_ROOT_PARAMETER hrp[4]; memcpy(hrp,rp,sizeof rp); hrp[0].DescriptorTable.pDescriptorRanges=rgb; hrp[3].DescriptorTable.pDescriptorRanges=rg;
      D3D12_ROOT_SIGNATURE_DESC hrd=rd; hrd.pParameters=hrp; hrd.pStaticSamplers=&ss;
      ID3DBlob *b1=NULL,*b2=NULL; HRESULT h1=D3D12SerializeRootSignature(r,D3D_ROOT_SIGNATURE_VERSION_1_0,&b1,NULL), h2=D3D12SerializeRootSignature(&hrd,D3D_ROOT_SIGNATURE_VERSION_1_0,&b2,NULL);
      T(h1==S_OK&&h2==S_OK&&b1&&b2);
      if(b1&&b2){ T(ID3D10Blob_GetBufferSize(b1)==ID3D10Blob_GetBufferSize(b2)&&ID3D10Blob_GetBufferSize(b1)>=24&&!memcmp(ID3D10Blob_GetBufferPointer(b1),ID3D10Blob_GetBufferPointer(b2),ID3D10Blob_GetBufferSize(b1))); }
      if(b1) ID3D10Blob_Release(b1); if(b2) ID3D10Blob_Release(b2); T(x_rootsig(&x,0)==NULL); wx_done(&x); }
    /* the largest version-1.0 root signature: D3D12_MAX_ROOT_COST (64) descriptor tables (one DWORD each) of one or two ranges, and two
     * static samplers.  Every range array is its own copy, most of them past the stack scratch (review finding: with a cap of 8 heap
     * blocks, 52 one-range tables were the fatal gfx-wrap-scratch).  (1) x_rootsig under the synthetic bias, (2) the import
     * D3D12SerializeRootSignature through the guest ABI under the process bias: both must give the blob of the same desc built with host
     * pointers. */
    { enum { NT=D3D12_MAX_ROOT_COST }; static D3D12_DESCRIPTOR_RANGE hrg[NT][2]; D3D12_ROOT_PARAMETER hrp[NT], grp[NT]; D3D12_STATIC_SAMPLER_DESC hss[2];
      memset(hrg,0,sizeof hrg); memset(hrp,0,sizeof hrp); memset(hss,0,sizeof hss);
      for(int i=0;i<NT;i++){ int nr=1+(i&1);
          for(int k=0;k<nr;k++){ hrg[i][k].RangeType=k?D3D12_DESCRIPTOR_RANGE_TYPE_CBV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV; hrg[i][k].NumDescriptors=1u+(UINT)k; hrg[i][k].BaseShaderRegister=(UINT)(4*i+k); hrg[i][k].OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND; }
          hrp[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; hrp[i].DescriptorTable.NumDescriptorRanges=(UINT)nr; hrp[i].DescriptorTable.pDescriptorRanges=hrg[i];
          hrp[i].ShaderVisibility=i%3?D3D12_SHADER_VISIBILITY_PIXEL:D3D12_SHADER_VISIBILITY_ALL; }
      for(int k=0;k<2;k++){ hss[k].Filter=D3D12_FILTER_MIN_MAG_MIP_POINT; hss[k].AddressU=hss[k].AddressV=hss[k].AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP; hss[k].MaxLOD=8.0f; hss[k].ShaderRegister=(UINT)k; hss[k].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL; }
      D3D12_ROOT_SIGNATURE_DESC hrd; memset(&hrd,0,sizeof hrd); hrd.NumParameters=NT; hrd.pParameters=hrp; hrd.NumStaticSamplers=2; hrd.pStaticSamplers=hss; hrd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
      ID3DBlob *bh=NULL; HRESULT hh=D3D12SerializeRootSignature(&hrd,D3D_ROOT_SIGNATURE_VERSION_1_0,&bh,NULL); T(hh==S_OK&&bh&&ID3D10Blob_GetBufferSize(bh)>=24);
      const u8 *hb=bh?ID3D10Blob_GetBufferPointer(bh):NULL; size_t hn=bh?ID3D10Blob_GetBufferSize(bh):0;
      /* (1) synthetic bias */
      memcpy(grp,hrp,sizeof hrp); for(int i=0;i<NT;i++) grp[i].DescriptorTable.pDescriptorRanges=GV(const D3D12_DESCRIPTOR_RANGE*,sg_put(&s,hrg[i],sizeof(D3D12_DESCRIPTOR_RANGE)*grp[i].DescriptorTable.NumDescriptorRanges,4));
      D3D12_ROOT_SIGNATURE_DESC grd=hrd; grd.pParameters=GV(const D3D12_ROOT_PARAMETER*,sg_put(&s,grp,sizeof grp,8)); grd.pStaticSamplers=GV(const D3D12_STATIC_SAMPLER_DESC*,sg_put(&s,hss,sizeof hss,4));
      u64 a_rd=sg_put(&s,&grd,sizeof grd,8); const D3D12_ROOT_SIGNATURE_DESC *r=x_rootsig(&x,a_rd);
      T(r&&r->NumParameters==NT&&outside(&s,r->pParameters)&&r->NumStaticSamplers==2&&outside(&s,r->pStaticSamplers)&&!memcmp(r->pStaticSamplers,hss,sizeof hss));
      int ok=r!=NULL; for(int i=0;ok&&i<NT;i++){ const D3D12_ROOT_PARAMETER *p=&r->pParameters[i]; UINT nr=hrp[i].DescriptorTable.NumDescriptorRanges;
          ok&=p->ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE&&p->ShaderVisibility==hrp[i].ShaderVisibility&&p->DescriptorTable.NumDescriptorRanges==nr&&p->DescriptorTable.pDescriptorRanges
              &&outside(&s,p->DescriptorTable.pDescriptorRanges)&&!memcmp(p->DescriptorTable.pDescriptorRanges,hrg[i],sizeof(D3D12_DESCRIPTOR_RANGE)*nr); }
      T(ok); T(x.nbig>8);                                                                            /* past the old cap of 8 heap blocks */
      ID3DBlob *bs=NULL; HRESULT hs=r?D3D12SerializeRootSignature(r,D3D_ROOT_SIGNATURE_VERSION_1_0,&bs,NULL):E_FAIL;
      T(hs==S_OK&&bs&&hb&&ID3D10Blob_GetBufferSize(bs)==hn&&!memcmp(ID3D10Blob_GetBufferPointer(bs),hb,hn)); if(bs) ID3D10Blob_Release(bs);
      wx_done(&x); T(x.nbig==0&&x.used==0&&x.chain==NULL);
      /* (2) the import, guest ABI, process bias: the same desc in guest memory; the blob comes back as a guest object */
      size_t need=sizeof grd+sizeof grp+sizeof hss+sizeof hrg+64; u64 m=ga_calloc(1,need), at=m;
      #define GPUT(src,n) ({ u64 a_=(at+15)&~15ull; memcpy(hp(g,a_),(src),(n)); at=a_+(n); a_; })
      for(int i=0;i<NT;i++) grp[i].DescriptorTable.pDescriptorRanges=GV(const D3D12_DESCRIPTOR_RANGE*,GPUT(hrg[i],sizeof(D3D12_DESCRIPTOR_RANGE)*grp[i].DescriptorTable.NumDescriptorRanges));
      grd.pParameters=GV(const D3D12_ROOT_PARAMETER*,GPUT(grp,sizeof grp)); grd.pStaticSamplers=GV(const D3D12_STATIC_SAMPLER_DESC*,GPUT(hss,sizeof hss));
      u64 a_g=GPUT(&grd,sizeof grd), a_out=GPUT("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",16);
      #undef GPUT
      T(at<=m+need); wr64(g,a_out,0x1111); wr64(g,a_out+8,0x2222);
      T((u32)GI(st_serialize_imp,a_g,D3D_ROOT_SIGNATURE_VERSION_1_0,a_out,a_out+8)==S_OK); u64 bo=rd64(g,a_out); T(bo!=0&&rd64(g,a_out+8)==0);
      if(bo){ u64 gptr=GC(VT(bo,3),bo), gn=GC(VT(bo,4),bo);                                       /* ID3DBlob::GetBufferPointer (host - bias), GetBufferSize */
          T(gptr!=0&&gn==hn&&hb&&!memcmp(hp(g,gptr),hb,hn)); T((u32)GC(VT(bo,2),bo)==0); }
      ga_free(m); if(bh) ID3D10Blob_Release(bh); }
    /* arrays of interface pointers (command lists with a NULL in between, descriptor heaps) */
    { u64 fl[3]={0x7e57000000004000ull,0x7e57000000005000ull,0x7e57000000006000ull}, arr[4]; arr[0]=sg_obj(&s,WK_LIST,fl[0]); arr[1]=0; arr[2]=sg_obj(&s,WK_LIST,fl[1]); arr[3]=sg_obj(&s,WK_LIST,fl[2]);
      u64 a=sg_put(&s,arr,sizeof arr,1); void **r=x_ifaces(&x,a+0,4,WK_LIST,"selftest lists");
      T(r&&U(r[0])==fl[0]&&r[1]==NULL&&U(r[2])==fl[1]&&U(r[3])==fl[2]); T(x_ifaces(&x,0,4,WK_LIST,"selftest")==NULL); T(x_ifaces(&x,a,0,WK_LIST,"selftest")==NULL);
      u64 hh[2]; hh[0]=sg_obj(&s,WK_DHEAP,0x7e57000000007000ull); hh[1]=sg_obj(&s,WK_DHEAP,0x7e57000000008000ull); u64 ah=sg_put(&s,hh,sizeof hh,8); r=x_ifaces(&x,ah,2,WK_DHEAP,"selftest heaps");
      T(U(r[0])==0x7e57000000007000ull&&U(r[1])==0x7e57000000008000ull); wx_done(&x); }
    /* D3D12_FEATURE_DATA_FEATURE_LEVELS, and a plain array at an odd guest address (the host copy is aligned) */
    { D3D_FEATURE_LEVEL lv[3]={D3D_FEATURE_LEVEL_9_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_12_0}; u64 a_lv=sg_put(&s,lv,sizeof lv,4);
      D3D12_FEATURE_DATA_FEATURE_LEVELS fd; memset(&fd,0,sizeof fd); fd.NumFeatureLevels=3; fd.pFeatureLevelsRequested=GV(const D3D_FEATURE_LEVEL*,a_lv); u64 a_fd=sg_put(&s,&fd,sizeof fd,8);
      D3D12_FEATURE_DATA_FEATURE_LEVELS *f=x_feature_levels(&x,a_fd); T(f&&f->NumFeatureLevels==3&&outside(&s,f->pFeatureLevelsRequested)&&f->pFeatureLevelsRequested[1]==D3D_FEATURE_LEVEL_11_0&&f->pFeatureLevelsRequested[2]==D3D_FEATURE_LEVEL_12_0);
      D3D12_VIEWPORT vp[2]={{0,0,1920,1080,0,1},{8,16,320,240,0.25f,0.75f}}; s.used|=1; u64 a_vp=sg_put(&s,vp,sizeof vp,1); T(a_vp&1);
      D3D12_VIEWPORT *v=x_copy(&x,a_vp,sizeof vp); T(v&&!(U(v)&15)&&outside(&s,v)&&v[1].Width==320.0f&&v[1].MaxDepth==0.75f&&!memcmp(v,vp,sizeof vp)); T(x_copy(&x,0,16)==NULL); wx_done(&x); }
    /* the same translators under the process' REAL bias, on real guest memory and a real guest object (identity build: bias 0) */
    { WX rx; wx_init(&rx,g); T(rx.bias==(uintptr_t)XL(g,0)&&!rx.synth);
      u64 m=ga_calloc(1,1024); void *fake=(void*)(uintptr_t)0x7e57000000009000ull; u64 o=kh_wrap_obj(g,fake,WK_ROOTSIG);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd,0,sizeof pd); pd.pRootSignature=GV(ID3D12RootSignature*,o); pd.PS.pShaderBytecode=GV(const void*,m+800); pd.PS.BytecodeLength=16; memcpy(hp(g,m),&pd,sizeof pd); wr32(g,m+800,0x43425844u);
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *r=x_pso(&rx,m); T((void*)r->pRootSignature==fake&&r->PS.pShaderBytecode==hp(g,m+800)&&*(const u32*)r->PS.pShaderBytecode==0x43425844u&&r->VS.pShaderBytecode==NULL);
      wx_done(&rx); w_forget(g,fake); ga_free(m); }
    /* wrapper identity on fake real pointers: one guest object per real object; the table through growth and removal */
    { void *f1=(void*)(uintptr_t)0x7e57000000010000ull,*f2=(void*)(uintptr_t)0x7e57000000020000ull; u64 made0=n_made[WK_RESOURCE], dead0=n_dead[WK_RESOURCE];
      u64 a=kh_wrap_obj(g,f1,WK_RESOURCE), b=kh_wrap_obj(g,f1,WK_RESOURCE), c=kh_wrap_obj(g,f2,WK_RESOURCE);
      T(a&&a==b); T(c&&c!=a); T(n_made[WK_RESOURCE]==made0+2);
      T(GA_OK(a)&&rd64(g,a)==kh_com_vtbl(C_w[WK_RESOURCE])); { extern KhFn kh_lookup(u64); T(kh_lookup(VT(a,8))!=NULL&&kh_host_fn_name(VT(a,8))!=NULL); }   /* a guest object whose vtable slots are KH_HOSTFN addresses */
      T(kh_wrap_real(g,a,WK_RESOURCE,"selftest")==f1&&kh_wrap_real(g,c,-1,"selftest")==f2); T(kh_wrap_real(g,0,WK_RESOURCE,"selftest")==NULL); T(kh_wrap_obj(g,NULL,WK_RESOURCE)==0);
      T(w_lookup(f1)==a&&w_lookup(f2)==c); w_forget(g,f1); T(w_lookup(f1)==0&&w_lookup(f2)==c); T(n_dead[WK_RESOURCE]==dead0+1);
      u64 a2=kh_wrap_obj(g,f1,WK_RESOURCE); T(a2&&w_lookup(f1)==a2); w_forget(g,f1); w_forget(g,f2); T(w_lookup(f2)==0);
      enum { NF=3000 }; u64 *objs=malloc(sizeof(u64)*NF); u32 n0=wt_n; int ok=1;
      for(int i=0;i<NF;i++){ objs[i]=kh_wrap_obj(g,(void*)(uintptr_t)(0x7e58000000000000ull+0x1000ull*(u64)i),WK_FENCE); ok&=objs[i]!=0; }
      T(ok&&wt_n==n0+NF&&wt_cap>=2*NF);
      ok=1; for(int i=0;i<NF;i++) ok&=w_lookup((void*)(uintptr_t)(0x7e58000000000000ull+0x1000ull*(u64)i))==objs[i]; T(ok);
      for(int i=0;i<NF;i+=2) w_forget(g,(void*)(uintptr_t)(0x7e58000000000000ull+0x1000ull*(u64)i));
      ok=1; for(int i=0;i<NF;i++) ok&=w_lookup((void*)(uintptr_t)(0x7e58000000000000ull+0x1000ull*(u64)i))==(i&1?objs[i]:0); T(ok); T(wt_n==n0+NF/2);
      for(int i=1;i<NF;i+=2) w_forget(g,(void*)(uintptr_t)(0x7e58000000000000ull+0x1000ull*(u64)i)); T(wt_n==n0); free(objs); }
    /* a host address handed to the game is host - bias: XL() of it is the host address again, and a guest write lands there */
    { u8 *h=malloc(64); memset(h,0,64); u64 ga=host_to_guest(g,h,"selftest"); T((void*)XL(g,ga)==(void*)h); T(hp(g,ga+8)==(void*)(h+8)); T(ga==GP(h)); T(host_to_guest(g,NULL,"selftest")==0);
      wr32(g,ga+4,0xfeedf00du); u32 v; memcpy(&v,h+4,4); T(v==0xfeedf00du); h[9]=0x5a; T(rd8(g,ga+9)==0x5a); T(KH_XLATE_MODE==0?ga==U(h):ga!=U(h)); free(h); }
    /* named fatals */
    T(fatal_case(g,0,"gfx-wrap-unwrapped-method: ID3D12Device::CreateComputePipelineState"));
    T(fatal_case(g,1,"gfx-wrap-unwrapped-method: ID3D12GraphicsCommandList::Dispatch"));
    T(fatal_case(g,2,"gfx-wrap-unwrapped-method: ID3D12Device::(a method of a later interface version) (vtable slot 44)"));
    T(fatal_case(g,3,"gfx-wrap-foreign-object: selftest argument"));
    T(fatal_case(g,4,"is a ID3D12Fence, a ID3D12Resource is needed"));
    T(fatal_case(g,5,"gfx-wrap-dead-object: ID3D12Fence::GetCompletedValue"));
    T(fatal_case(g,6,"gfx-wrap-private-interface"));
    T(fatal_case(g,7,"gfx-wrap-feature"));
    free(s.mem); st_f+=kh_gfxc_st_fails;
    fprintf(stderr,"kh: gfx wrap selftest: %d checks, %d failures (structure translation under a synthetic bias of 0x%llx; process bias 0x%llx)\n",st_n,st_f,P(x.bias),P(XL(g,0)));
    return st_f;
}
/* KHNMC_GFX=vkd3dw --selftest: what needs real objects.  Everything goes through the guest ABI (the vtable slots), like the lifted code */
int kh_wrap_selftest_device(KCtx *g){
    extern void rt_imp_d3d12__ord101(KCtx*);
    st_n=st_f=0; kh_gfxc_st_fails=0; u64 nat0=kh_native_calls;
    u64 m=ga_calloc(1,2048), dev=0,q=0,f1=0,f2=0,up=0,rb=0,hrtv=0,hsrv=0,al=0,cl=0,fence=0;
    T((u32)GI(rt_imp_d3d12__ord101,0,0xb000,GP(&IID_ID3D12Device),m)==0); dev=rd64(g,m); T(dev!=0); if(!dev) goto done;
    ID3D12Device *rdev=kh_wrap_real(g,dev,WK_DEVICE,"selftest"); T(rdev!=NULL);
    /* identity: the device is a guest object; QueryInterface for three interfaces gives that same object and one real reference each */
    T(GA_OK(dev)&&rd64(g,dev)==kh_com_vtbl(C_w[WK_DEVICE])&&(void*)(uintptr_t)dev!=(void*)rdev); T(kh_wrap_obj(g,rdev,WK_DEVICE)==dev);
    { ULONG base=real_refs(rdev); const IID *ids[3]={&IID_IUnknown,&IID_ID3D12Object,&IID_ID3D12Device};
      for(int i=0;i<3;i++){ wr64(g,m+8,0x1111); T((u32)GC(VT(dev,0),dev,GP(ids[i]),m+8)==0); T(rd64(g,m+8)==dev); T(real_refs(rdev)==base+1); T((u32)GC(VT(dev,2),dev)==base); T(real_refs(rdev)==base); }
      static const GUID bogus={0x12345678,0x9abc,0xdef0,{1,2,3,4,5,6,7,8}};
      wr64(g,m+8,0x1111); T((u32)GC(VT(dev,0),dev,GP(&bogus),m+8)==(u32)E_NOINTERFACE); T(rd64(g,m+8)==0); T(real_refs(rdev)==base);
      T((u32)GC(VT(dev,0),dev,GP(&IID_ID3D12Device),0)==(u32)E_POINTER);
      T((u32)GC(VT(dev,1),dev)==base+1&&COM_REFS(g,dev)==base+1); T((u32)GC(VT(dev,2),dev)==base&&COM_REFS(g,dev)==base); }   /* AddRef / Release return the REAL count */
    /* a child's GetDevice gives the device object the game already has */
    memset(hp(g,m+16),0,16); T((u32)GC(VT(dev,8),dev,m+16,GP(&IID_ID3D12CommandQueue),m+32)==0); q=rd64(g,m+32); T(q!=0&&q!=dev); if(!q) goto done;
    { ULONG base=real_refs(rdev); wr64(g,m+8,0); T((u32)GC(VT(q,7),q,GP(&IID_ID3D12Device),m+8)==0); T(rd64(g,m+8)==dev); T(real_refs(rdev)==base+1); T((u32)GC(VT(dev,2),dev)==base); }
    /* reference counts follow the real object; the guest object goes with the last reference */
    T((u32)GC(VT(dev,36),dev,7,0,GP(&IID_ID3D12Fence),m+40)==0); f1=rd64(g,m+40); T((u32)GC(VT(dev,36),dev,0,0,GP(&IID_ID3D12Fence),m+40)==0); f2=rd64(g,m+40);
    T(f1&&f2&&f1!=f2); if(!f1||!f2) goto done;
    { void *r1=kh_wrap_real(g,f1,WK_FENCE,"selftest"); u64 dead0=n_dead[WK_FENCE]; T(GC(VT(f1,8),f1)==7&&GC(VT(f2,8),f2)==0);           /* GetCompletedValue: 64-bit result */
      T((u32)GC(VT(f1,1),f1)==2); T((u32)GC(VT(f1,1),f1)==3); T(real_refs(r1)==3); T((u32)GC(VT(f1,2),f1)==2); T((u32)GC(VT(f1,2),f1)==1); T(w_lookup(r1)==f1);
      T((u32)GC(VT(f1,2),f1)==0); T(w_lookup(r1)==0); T(n_dead[WK_FENCE]==dead0+1); f1=0;
      T(kh_wrap_real(g,f2,WK_FENCE,"selftest")!=NULL); T((u32)GC(VT(f2,10),f2,5)==0&&GC(VT(f2,8),f2)==5); }                                 /* the other fence is untouched; Signal */
    /* results by value: descriptor handles, descs and the allocation info come back through the hidden pointer (rax = that pointer) */
    { u64 d=m+64; memset(hp(g,d),0,16); wr32(g,d,D3D12_DESCRIPTOR_HEAP_TYPE_RTV); wr32(g,d+4,4); T((u32)GC(VT(dev,14),dev,d,GP(&IID_ID3D12DescriptorHeap),m+48)==0); hrtv=rd64(g,m+48);
      wr32(g,d,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); wr32(g,d+4,8); wr32(g,d+8,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE); T((u32)GC(VT(dev,14),dev,d,GP(&IID_ID3D12DescriptorHeap),m+56)==0); hsrv=rd64(g,m+56); }
    T(hrtv&&hsrv); if(!hrtv||!hsrv) goto done;
    { ID3D12DescriptorHeap *r1=kh_wrap_real(g,hrtv,WK_DHEAP,"selftest"), *r2=kh_wrap_real(g,hsrv,WK_DHEAP,"selftest"); u64 o=m+96;
      memset(hp(g,o),0xA5,32); T(GC(VT(hrtv,9),hrtv,o)==o); T(rd64(g,o)==(u64)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(r1).ptr&&rd64(g,o)!=0&&rd64(g,o+8)==0xa5a5a5a5a5a5a5a5ull);
      memset(hp(g,o),0xA5,32); T(GC(VT(hsrv,10),hsrv,o)==o); T(rd64(g,o)==ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(r2).ptr&&rd64(g,o)!=0&&rd64(g,o+8)==0xa5a5a5a5a5a5a5a5ull);
      memset(hp(g,o),0xA5,32); T(GC(VT(hsrv,8),hsrv,o)==o); T(rd32(g,o)==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV&&rd32(g,o+4)==8&&rd32(g,o+8)==D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE&&rd64(g,o+16)==0xa5a5a5a5a5a5a5a5ull);
      T((u32)GC(VT(dev,15),dev,D3D12_DESCRIPTOR_HEAP_TYPE_RTV)==ID3D12Device_GetDescriptorHandleIncrementSize(rdev,D3D12_DESCRIPTOR_HEAP_TYPE_RTV)); }
    /* an upload buffer and a readback buffer */
    { D3D12_HEAP_PROPERTIES hprops; memset(&hprops,0,sizeof hprops); hprops.Type=D3D12_HEAP_TYPE_UPLOAD; memcpy(hp(g,m+160),&hprops,sizeof hprops);
      D3D12_RESOURCE_DESC bd; memset(&bd,0,sizeof bd); bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=4096; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; memcpy(hp(g,m+192),&bd,sizeof bd);
      T((u32)GC(VT(dev,27),dev,m+160,0,m+192,D3D12_RESOURCE_STATE_GENERIC_READ,0,GP(&IID_ID3D12Resource),m+256)==0); up=rd64(g,m+256);
      hprops.Type=D3D12_HEAP_TYPE_READBACK; memcpy(hp(g,m+160),&hprops,sizeof hprops);
      T((u32)GC(VT(dev,27),dev,m+160,0,m+192,D3D12_RESOURCE_STATE_COPY_DEST,0,GP(&IID_ID3D12Resource),m+264)==0); rb=rd64(g,m+264);
      T(up&&rb&&up!=rb); if(!up||!rb) goto done;
      ID3D12Resource *rup=kh_wrap_real(g,up,WK_RESOURCE,"selftest");
      u64 o=m+320; memset(hp(g,o),0xA5,72); T(GC(VT(up,10),up,o)==o); T(rd32(g,o)==D3D12_RESOURCE_DIMENSION_BUFFER&&rd64(g,o+16)==4096&&rd32(g,o+24)==1&&rd32(g,o+36)==1&&rd64(g,o+56)==0xa5a5a5a5a5a5a5a5ull);   /* GetDesc */
      T(GC(VT(up,11),up)==ID3D12Resource_GetGPUVirtualAddress(rup)&&GC(VT(up,11),up)!=0);
      { D3D12_RESOURCE_ALLOCATION_INFO ri=ID3D12Device_GetResourceAllocationInfo(rdev,0,1,&bd); memset(hp(g,o),0xA5,32); T(GC(VT(dev,25),dev,o,0,1,m+192)==o); T(rd64(g,o)==ri.SizeInBytes&&rd64(g,o+8)==ri.Alignment&&ri.SizeInBytes>=4096&&rd64(g,o+16)==0xa5a5a5a5a5a5a5a5ull); }
      { D3D12_HEAP_PROPERTIES got; D3D12_HEAP_FLAGS fl=0; T((u32)GC(VT(up,14),up,m+400,m+432)==0); memcpy(&got,hp(g,m+400),sizeof got); T(SUCCEEDED(ID3D12Resource_GetHeapProperties(rup,NULL,&fl))&&got.Type==D3D12_HEAP_TYPE_UPLOAD&&rd32(g,m+432)==(u32)fl); }
      { static const u16 nm[]={'V','e','r','t','e','x','R','e','s','o','u','r','c','e',0}; memcpy(hp(g,m+448),nm,sizeof nm); T((u32)GC(VT(up,6),up,m+448)==0); }                        /* SetName: 16-bit characters */
      /* Map: the game's address is host - bias; what it writes there is in the buffer the real interface maps, and the GPU copies it */
      void *preal=NULL; T(SUCCEEDED(ID3D12Resource_Map(rup,0,NULL,&preal))&&preal); wr64(g,m+480,0); memset(hp(g,m+488),0,16);
      T((u32)GC(VT(up,8),up,0,m+488,m+480)==0); u64 ga=rd64(g,m+480); T(ga!=0&&ga==GP(preal)&&(void*)XL(g,ga)==preal); T(KH_XLATE_MODE==0?ga==U(preal):ga!=U(preal));
      if(!ga||!preal) goto done;
      for(u32 i=0;i<4096;i+=4) wr32(g,ga+i,0x9e3779b9u*(i+1));                              /* written the way the lifted code writes: through XL() */
      { int ok=1; for(u32 i=0;i<4096;i+=4){ u32 v; memcpy(&v,(u8*)preal+i,4); ok&=v==0x9e3779b9u*(i+1); } T(ok); }
      GC(VT(up,9),up,0,0); ID3D12Resource_Unmap(rup,0,NULL);
      T((u32)GC(VT(dev,9),dev,0,GP(&IID_ID3D12CommandAllocator),m+272)==0); al=rd64(g,m+272); T((u32)GC(VT(dev,12),dev,0,0,al,0,GP(&IID_ID3D12GraphicsCommandList),m+280)==0); cl=rd64(g,m+280);
      T((u32)GC(VT(dev,36),dev,0,0,GP(&IID_ID3D12Fence),m+288)==0); fence=rd64(g,m+288); T(al&&cl&&fence); if(!al||!cl||!fence) goto done;
      T((u32)GC(VT(cl,8),cl)==D3D12_COMMAND_LIST_TYPE_DIRECT);
      u64 c0=gfx_copies,e0=gfx_execs; GC(VT(cl,15),cl,rb,0,up,0,4096); T(gfx_copies==c0+1);                                             /* CopyBufferRegion (6 arguments) */
      T((u32)GC(VT(cl,9),cl)==0); wr64(g,m+296,cl); GC(VT(q,10),q,1,m+296); T(gfx_execs==e0+1); T((u32)GC(VT(q,14),q,fence,1)==0);
      { int done=0; for(int i=0;i<120000&&!done;i++){ done=GC(VT(fence,8),fence)>=1; if(!done){ struct timespec ts={0,500000}; nanosleep(&ts,0); } } T(done); if(!done) goto done; }
      { D3D12_RANGE all={0,4096}; memcpy(hp(g,m+488),&all,sizeof all); wr64(g,m+480,0); T((u32)GC(VT(rb,8),rb,0,m+488,m+480)==0); u64 gb=rd64(g,m+480); T(gb!=0&&gb!=ga);
        if(gb){ int ok=1; for(u32 i=0;i<4096;i+=4) ok&=rd32(g,gb+i)==0x9e3779b9u*(i+1); T(ok); memset(hp(g,m+488),0,16); GC(VT(rb,9),rb,0,m+488); } } }
    /* CheckFeatureSupport: a pointer-free structure and the one with a pointer, against the real answers */
    { D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms, ref; memset(&ms,0,sizeof ms); ms.Format=DXGI_FORMAT_B8G8R8A8_UNORM; ms.SampleCount=4; ref=ms; memcpy(hp(g,m+512),&ms,sizeof ms);
      HRESULT hr=ID3D12Device_CheckFeatureSupport(rdev,D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&ref,sizeof ref);
      T((u32)GC(VT(dev,13),dev,D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,m+512,sizeof ms)==(u32)hr); T(!memcmp(hp(g,m+512),&ref,sizeof ref));
      D3D_FEATURE_LEVEL lv[3]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_12_0}; memcpy(hp(g,m+560),lv,sizeof lv);
      D3D12_FEATURE_DATA_FEATURE_LEVELS fl, fref; memset(&fl,0,sizeof fl); fl.NumFeatureLevels=3; fref=fl; fl.pFeatureLevelsRequested=GV(const D3D_FEATURE_LEVEL*,m+560); fref.pFeatureLevelsRequested=lv; memcpy(hp(g,m+592),&fl,sizeof fl);
      hr=ID3D12Device_CheckFeatureSupport(rdev,D3D12_FEATURE_FEATURE_LEVELS,&fref,sizeof fref);
      T((u32)GC(VT(dev,13),dev,D3D12_FEATURE_FEATURE_LEVELS,m+592,sizeof fl)==(u32)hr); T(hr==S_OK&&rd32(g,m+592+16)==(u32)fref.MaxSupportedFeatureLevel&&fref.MaxSupportedFeatureLevel>=D3D_FEATURE_LEVEL_11_0&&rd64(g,m+592+8)==m+560); }
    T((u32)GC(VT(dev,37),dev)==0); T((u32)GC(VT(dev,7),dev)==1);
    /* wrappers neither route calls: each answer is compared with the real object's */
    { ID3D12CommandQueue *rq=kh_wrap_real(g,q,WK_QUEUE,"selftest"); ID3D12Fence *rf=kh_wrap_real(g,fence,WK_FENCE,"selftest"); ID3D12Resource *rup=kh_wrap_real(g,up,WK_RESOURCE,"selftest"); u64 o=m+640;
      D3D12_COMMAND_QUEUE_DESC qd=ID3D12CommandQueue_GetDesc(rq); memset(hp(g,o),0xA5,32); T(GC(VT(q,18),q,o)==o); T(!memcmp(hp(g,o),&qd,sizeof qd)&&rd64(g,o+16)==0xa5a5a5a5a5a5a5a5ull);         /* ID3D12CommandQueue::GetDesc */
      D3D12_HEAP_PROPERTIES chp=ID3D12Device_GetCustomHeapProperties(rdev,0,D3D12_HEAP_TYPE_UPLOAD); memset(hp(g,o),0xA5,32); T(GC(VT(dev,26),dev,o,0,D3D12_HEAP_TYPE_UPLOAD)==o); T(!memcmp(hp(g,o),&chp,sizeof chp)&&rd32(g,o+20)==0xa5a5a5a5u);   /* GetCustomHeapProperties */
      T((u32)GC(VT(q,15),q,fence,1)==(u32)ID3D12CommandQueue_Wait(rq,rf,1));                                                   /* ID3D12CommandQueue::Wait */
      /* private data: bytes go in and come back; an interface is the named fatal (tested above), removing one is forwarded */
      static const GUID pg={0x4b484e4d,0x4321,0x1221,{0x49,0x32,0x31,0x77,0x72,0x61,0x70,0x21}}; wr64(g,o,0x1122334455667788ull);
      T((u32)GC(VT(up,4),up,GP(&pg),8,o)==0); wr64(g,o+8,0); wr32(g,o+16,8); T((u32)GC(VT(up,3),up,GP(&pg),o+16,o+8)==0); T(rd64(g,o+8)==0x1122334455667788ull&&rd32(g,o+16)==8);
      { UINT sz=8; u64 back=0, same=0x1122334455667788ull; T(ID3D12Resource_GetPrivateData(rup,&pg,&sz,&back)==S_OK&&back==same);
        ID3D12Resource *rrb=kh_wrap_real(g,rb,WK_RESOURCE,"selftest"); HRESULT h1=ID3D12Resource_SetPrivateData(rrb,&pg,8,&same), h2=ID3D12Resource_SetPrivateDataInterface(rrb,&pg,NULL);   /* the same two steps on another resource, directly */
        T(h1==S_OK&&(u32)GC(VT(up,5),up,GP(&pg),0)==(u32)h2); }
      /* a heap, its desc, a buffer placed in it */
      { u64 heap=0, placed=0; D3D12_HEAP_DESC hd; memset(&hd,0,sizeof hd); hd.SizeInBytes=1u<<16; hd.Properties.Type=D3D12_HEAP_TYPE_UPLOAD; hd.Alignment=1u<<16; hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS; memcpy(hp(g,m+704),&hd,sizeof hd);
        T((u32)GC(VT(dev,28),dev,m+704,GP(&IID_ID3D12Heap),m+768)==0); heap=rd64(g,m+768); T(heap!=0);
        if(heap){ ID3D12Heap *rh=kh_wrap_real(g,heap,WK_HEAP,"selftest"); D3D12_HEAP_DESC got=ID3D12Heap_GetDesc(rh); memset(hp(g,m+800),0xA5,64); T(GC(VT(heap,8),heap,m+800)==m+800); T(!memcmp(hp(g,m+800),&got,sizeof got)&&got.SizeInBytes==(1u<<16)&&rd64(g,m+848)==0xa5a5a5a5a5a5a5a5ull);
          T((u32)GC(VT(dev,29),dev,heap,0,m+192,D3D12_RESOURCE_STATE_GENERIC_READ,0,GP(&IID_ID3D12Resource),m+776)==0); placed=rd64(g,m+776); T(placed!=0&&placed!=up);
          if(placed){ u64 d=m+864; T(GC(VT(placed,10),placed,d)==d); T(rd32(g,d)==D3D12_RESOURCE_DIMENSION_BUFFER&&rd64(g,d+16)==4096); T(GC(VT(placed,2),placed)==0); }
          T(GC(VT(heap,2),heap)==0); } }
      /* descriptors: a constant-buffer view, then the two copy calls (plain values and two arrays of plain values) */
      { ID3D12DescriptorHeap *rs=kh_wrap_real(g,hsrv,WK_DHEAP,"selftest"); u64 start=(u64)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rs).ptr, inc=ID3D12Device_GetDescriptorHandleIncrementSize(rdev,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cb; cb.BufferLocation=ID3D12Resource_GetGPUVirtualAddress(rup); cb.SizeInBytes=256; memcpy(hp(g,m+928),&cb,sizeof cb); u64 w0=wcalls[WK_DEVICE][17]+wcalls[WK_DEVICE][23]+wcalls[WK_DEVICE][24];
        GC(VT(dev,17),dev,m+928,start+inc); GC(VT(dev,24),dev,1,start+2*inc,start+inc,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        wr64(g,m+944,start+3*inc); wr32(g,m+952,1); wr64(g,m+960,start+inc); wr32(g,m+968,1); GC(VT(dev,23),dev,1,m+944,m+952,1,m+960,m+968,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        T(wcalls[WK_DEVICE][17]+wcalls[WK_DEVICE][23]+wcalls[WK_DEVICE][24]==w0+3); }
      /* the shared-handle pair of the movie path: vkd3d's own answer comes back, and no host pointer with it */
      { HANDLE hh=NULL; void *ro=NULL; wr64(g,o,0x1111); T((u32)GC(VT(dev,31),dev,up,0,0x10000000,0,o)==(u32)ID3D12Device_CreateSharedHandle(rdev,(ID3D12DeviceChild*)rup,NULL,0x10000000,NULL,&hh)); T(rd64(g,o)==U(hh));
        wr64(g,o,0x1111); HRESULT hr=ID3D12Device_OpenSharedHandle(rdev,NULL,&IID_ID3D12Resource,&ro); T((u32)GC(VT(dev,32),dev,0,GP(&IID_ID3D12Resource),o)==(u32)hr); T(FAILED(hr)?rd64(g,o)==0:rd64(g,o)!=0); }
      /* ClearState on a recording list leaves it usable */
      T((u32)GC(VT(al,8),al)==0); T((u32)GC(VT(cl,10),cl,al,0)==0); GC(VT(cl,11),cl,0); T((u32)GC(VT(cl,9),cl)==0); }
done:
    if(cl) GC(VT(cl,2),cl); if(al) GC(VT(al,2),al); if(fence) GC(VT(fence,2),fence); if(rb) GC(VT(rb,2),rb); if(up) GC(VT(up,2),up); if(hsrv) GC(VT(hsrv,2),hsrv); if(hrtv) GC(VT(hrtv,2),hrtv);
    if(f1) GC(VT(f1,2),f1); if(f2) GC(VT(f2,2),f2);
    if(q){ void *rq=kh_wrap_real(g,q,WK_QUEUE,"selftest"); T(GC(VT(q,2),q)==0); T(w_lookup(rq)==0); }
    if(dev){ void *rd=kh_wrap_real(g,dev,WK_DEVICE,"selftest"); T(GC(VT(dev,2),dev)==0); T(w_lookup(rd)==0); }                              /* the device goes when its last child and the game's reference are gone */
    T(kh_native_calls==nat0);
    ga_free(m); st_f+=kh_gfxc_st_fails;
    fprintf(stderr,"kh: gfx wrap device selftest: %d checks, %d failures (device %s, %s addresses, bias 0x%llx)\n",st_n,st_f,kh_gfxc_device_name(),KH_XLATE_MODE==0?"identity":"bias",P(XL(g,0)));
    return st_f;
}
#endif
