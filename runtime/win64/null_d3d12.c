/* null_d3d12.c : headless D3D12 / DXGI backend.  A "null device" that accepts the documented 75-method surface (docs/graphics/D3D12-SHIM.md),
 * counts and optionally logs every call, and returns plausible success values: one adapter (VendorId 0x106b, 8 GiB), one output, a swap chain
 * of three B8G8R8A8 buffers, fences that complete the instant they are signalled, upload buffers backed by real guest memory so the game's
 * persistent maps work, copyable footprints computed from the resource description.  Anything outside the documented surface is a named fatal.
 * Also: d3dcompiler_47!D3DStripShader (copy), D3D12SerializeRootSignature (blob holding the desc), D3D11CreateDevice (movie decode device). */
#include "khnmc_rt.h"
#include "khnmc_app.h"
#include "gfx_present.h"
#include <time.h>

#define E_FAIL_ 0x80004005u
#define DXGI_NOT_FOUND 0x887A0002u
#define STRICT COM_STRICT
static int C_factory,C_adapter,C_output,C_swap,C_dev,C_queue,C_alloc,C_list,C_res,C_dheap,C_fence,C_pso,C_rootsig,C_heap,C_qheap,C_blob;
static u64 o_adapter,o_output;
static _Atomic u64 res_ids=1, heap_ids=1;
_Atomic u64 gfx_draws, gfx_clears, gfx_barriers, gfx_execs, gfx_presents, gfx_copies, gfx_psos, gfx_resources, gfx_buffers_bytes;
/* task 30: a resource is destroyed at its last Release, as Direct3D destroys it: its buffer data (far zone) and the object itself
 * (id zone) go back to the arena.  gfx_resources / gfx_buffers_bytes stay what they were (created so far); these count what went. */
static _Atomic u64 gfx_res_freed, gfx_buffers_freed_bytes, gfx_buffers_freed;
static long vblank_ns=16666667;            /* the null backend's frame sleep; KHNMC_NULL_VBLANK_NS=n (1,000,000..500,000,000) overrides it (task 27: 120 ms frames) */
void kh_gfx_report(FILE *f);
void kh_gfx_tex_report(FILE *f);
static int log_gfx;
_Atomic u64 gfx_bundles, gfx_pso_binds;
/* KHNMC_GFX=vkd3d (gfx_vkd3d.c): the device is a host vkd3d device; only the DXGI objects below stay ours, and the swap chain gets real back buffers.
 * KHNMC_GFX=vkd3dw (gfx_wrap.c): the same vkd3d device behind guest-side wrapper objects (any address model, no Windows-ABI call). */
extern int kh_gfx_vkd3d_on;
void kh_gfx_wrap_init(void); int kh_wrap_selftest_pure(KCtx *g);
void kh_gfx_vkd3d_init(void); void kh_vkd3d_create_device(KCtx *g); void kh_vkd3d_serialize_rootsig(KCtx *g); void kh_vkd3d_swap_attach(KCtx *g,u64 sc,u64 queue,u64 desc);
void kh_vkd3d_swap_getbuffer(KCtx *g,u64 o); void kh_vkd3d_swap_resize(KCtx *g,u64 o,u32 w,u32 h); void kh_vkd3d_present(KCtx *g,u64 o,u64 n); void kh_gfx_vkd3d_report(FILE *f); int kh_gfx_vkd3d_selftest(KCtx *g);
/* texture recorder: sampled textures (no RT/DS flag) by DXGI format and by size class, and the footprint uploads that fill them (CopyTextureRegion) */
static _Atomic u64 tx_res[128], tx_res_bytes[128], tx_up[128], tx_up_bytes[128], tx_distinct_up, tx_rtds, tx_sized[8], tx_up_sized[8], tx_up_total_bytes, tx_up_calls, tx_other_up;
static const char *size_names[8]={"<=64","128","256","512","1024","2048",">2048","1D/3D"};
/* task 37 (runtime/aspect.c): the screen seam's size for the DXGI output (S2) and, with KHNMC_ASPECT_LOG=1, the size records (swap
 * chain, render-target / depth-stencil / multisampled textures, the GX sizes at each present) */
int kh_screen_size(int *w,int *h); extern int kh_aspect_log;
void kh_aspect_note_swapchain(KCtx *g,const char *what,u64 desc,u32 w,u32 h); void kh_aspect_note_texture(KCtx *g,u64 desc); void kh_aspect_note_present(KCtx *g,u64 present);
int kh_screen_check_present(KCtx *g,u32 scw,u32 sch,u64 present);      /* KHNMC_SCREEN set: the presented size is the screen's (else screen-not-applied) */
/* task 40, KHNMC_ASPECT_LOG=2 (the measurement M of NATIVE-FILL-PLAN.md step 4, runtime/aspect.c): per draw the kind of the bound
 * pipeline's vertex shader and, for the three VS that read MagicScale.zw, the x / y range of its POSITION in the vertex buffer; the
 * scissor rects.  The VS is identified by its DXBC checksum (bytes 4..19; D3DStripShader below is a copy) against the 26 vertex-shader
 * blobs in the image (graphics-spec SHADERS.md).  Nothing here runs or is allocated without KHNMC_ASPECT_LOG=2. */
extern int kh_aspect_m;
typedef struct { int kind, have; float ymin,ymax,xmin,xmax; int have_off, src, mssrc, nocbv; float ox,oy,msy,msw; u64 list, pso; int indexed; u32 count, start; s32 base; } KhMDraw;   /* = runtime/aspect.c */
void kh_aspect_m_draw(KCtx *g,const KhMDraw *d); void kh_aspect_m_rs_scissor(KCtx *g,u32 n,u64 rects);
static _Atomic u64 m_dt_binds;                         /* SetGraphicsRootDescriptorTable calls (task 40 review: how f_1121d0 binds cbConstantVS) */
static const u32 m_vs_rva[26]={0x586160,0x586710,0x586c80,0x5871b0,0x5877a0,0x588330,0x589080,0x589e70,0x58a980,0x58b530,0x58c0f0,0x58d7b0,0x58e3c0,
                                0x58ed60,0x58f650,0x590610,0x5915d0,0x592d00,0x593b10,0x594650,0x5951b0,0x5963f0,0x5973a0,0x5987d0,0x599640,0x59ac40};
static int m_vs_kind(KCtx *g,u64 bc,u64 n){            /* the kinds of runtime/aspect.c: 1..3 MagicScale 2D, 4 5871b0, 5 593b10, 6 59ac40, 7 3D, 8 unknown */
    if(!bc||n<20||!GA_OK(bc)||!GA_OK(bc+19)||rd32(g,bc)!=0x43425844u) return 8;
    for(int i=0;i<26;i++){ u64 b=GS_BASE+m_vs_rva[i]; if(rd32(g,b)!=0x43425844u) continue;
        if(!memcmp(hp(g,bc+4),hp(g,b+4),16)){ switch(m_vs_rva[i]){ case 0x586160: return 1; case 0x586710: return 2; case 0x586c80: return 3; case 0x5871b0: return 4;
                                                                    case 0x593b10: return 5; case 0x59ac40: return 6; default: return 7; } } }
    return 8;
}
int kh_null_m_vs_kind_test(KCtx *g,u64 bc,u64 n){ return m_vs_kind(g,bc,n); }   /* runtime/aspect.c selftest */
static u64 *m_res; static u64 m_res_n; static pthread_mutex_t m_res_mx=PTHREAD_MUTEX_INITIALIZER;   /* resource id (GPU VA >> 32) -> object */
static void m_res_set(u64 id,u64 r){ pthread_mutex_lock(&m_res_mx);
    if(id>=m_res_n){ u64 nn=m_res_n?m_res_n:4096; while(nn<=id) nn*=2; u64 *p=realloc(m_res,nn*8); if(!p) kh_fatal(NULL,"aspect M: out of host memory for the resource table"); memset(p+m_res_n,0,(nn-m_res_n)*8); m_res=p; m_res_n=nn; }
    m_res[id]=r; pthread_mutex_unlock(&m_res_mx); }
static u64 m_res_get(u64 id){ u64 r=0; pthread_mutex_lock(&m_res_mx); if(id<m_res_n) r=m_res[id]; pthread_mutex_unlock(&m_res_mx); return r; }
typedef struct { u64 list, pso, vb, ib, cbv[8]; u32 vbsize, vbstride, ibsize, ibfmt; } MList;
#define M_LISTS 128                                    /* per thread: the game records into about 27 bundles per frame, interleaved */
static __thread MList m_ls[M_LISTS]; static __thread int m_ls_next;
static MList *m_list(u64 list){ for(int i=0;i<M_LISTS;i++) if(m_ls[i].list==list) return &m_ls[i]; MList *l=&m_ls[m_ls_next]; m_ls_next=(m_ls_next+1)%M_LISTS; memset(l,0,sizeof *l); l->list=list; return l; }
#define R_DATA 32
#define R_SIZE 40
static _Atomic u64 m_why[6];                           /* MagicScale draws without vertices: no VB bound, VB resource unknown, indexed without IB, first vertex out of range, IB resource unknown, read */
void kh_aspect_m_null_report(FILE *f){ fprintf(f,"kh: aspect M: null backend, MagicScale draws: vertices read %llu; not read: no vertex buffer bound %llu, its resource unknown %llu, indexed without an index buffer %llu, index buffer resource unknown %llu, first vertex outside the buffer %llu; SetGraphicsRootDescriptorTable calls %llu\n",
    (unsigned long long)m_why[5],(unsigned long long)m_why[0],(unsigned long long)m_why[1],(unsigned long long)m_why[2],(unsigned long long)m_why[4],(unsigned long long)m_why[3],(unsigned long long)m_dt_binds); }
static void m_draw(KCtx *g,MList *l,int indexed,u32 count,u32 start,s32 base){
    int kind=l->pso?(int)rd32(g,l->pso+32):0; int have=0; float y0=1e30f,y1=-1e30f,x0=1e30f,x1=-1e30f;
    if(kind>=1&&kind<=3){ if(!l->vb||!l->vbstride) m_why[0]++; else if(!m_res_get(l->vb>>32)) m_why[1]++; else if(indexed&&!l->ib) m_why[2]++; else if(indexed&&!m_res_get(l->ib>>32)) m_why[4]++; }
    if(kind>=1&&kind<=3&&l->vb&&l->vbstride){
        u64 vr=m_res_get(l->vb>>32), vd=vr?rd64(g,vr+R_DATA):0, vn=vr?rd64(g,vr+R_SIZE):0, vo=l->vb&0xffffffffull; u32 po=rd32(g,l->pso+36);
        u64 id=0,in=0,io=0; if(indexed&&l->ib){ u64 ir=m_res_get(l->ib>>32); id=ir?rd64(g,ir+R_DATA):0; in=ir?rd64(g,ir+R_SIZE):0; io=l->ib&0xffffffffull; }
        if(vd&&(!indexed||id)){ u32 m=count<4096?count:4096;
            for(u32 i=0;i<m;i++){ s64 v;
                if(indexed){ u32 isz=l->ibfmt==42?4:2; u64 ia=io+(u64)(start+i)*isz; if(ia+isz>in||ia-io+isz>l->ibsize) break; v=(s64)base+(isz==4?(s64)rd32(g,id+ia):(s64)rd16(g,id+ia)); }
                else v=(s64)start+i;
                if(v<0) break; u64 a=vo+(u64)v*l->vbstride+po; if(a+8>vn||a+8>vo+l->vbsize) break;
                u32 bx=rd32(g,vd+a), by=rd32(g,vd+a+4); float fx,fy; memcpy(&fx,&bx,4); memcpy(&fy,&by,4);
                if(fx<x0) x0=fx; if(fx>x1) x1=fx; if(fy<y0) y0=fy; if(fy>y1) y1=fy; have=1; }
            if(have) m_why[5]++; else m_why[3]++; } }
    /* the GS offset (cbConstantVS row 1, xyOffset.xy, which the 2D VS subtracts from POSITION) and MagicScale (row 2).  Task 40 review
     * (finding 4): f_1121d0 (this = rcx = the GX device) stages cbConstantVS in GX: row 0 at +0x59e0, row 1 (xyOffset; from +0x138 /
     * +0x13c and +0x140 / +0x144) at +0x59f0, row 2 (MagicScale; copied from GX+0xa0, or from GX+0x90 under the test at 0x140112396)
     * at +0x5a00 (shard_0021.c L11239d, L112fee), and binds the buffer through a descriptor table (SetGraphicsRootDescriptorTable,
     * vtable +0x100), which the root-CBV search below never sees.  So: a bound root CBV holding exactly the staged rows confirms them
     * (src 1); a bound root CBV with MagicScale = GX+0xa0 but other rows is the task-40 rule and its values are used (src 2); else the
     * staged rows (src 3).  Each source is counted in the M report. */
    KhMDraw d; memset(&d,0,sizeof d); d.kind=kind; d.have=have; d.ymin=y0; d.ymax=y1; d.xmin=x0; d.xmax=x1; d.list=l->list; d.pso=l->pso; d.indexed=indexed; d.count=count; d.start=start; d.base=base;
    if(have){ const u64 gx=GS_BASE+0x8a0970ull, stg=gx+0x59f0; u32 sb[8]; memcpy(sb,hp(g,stg),32); int conf=0, old=-1; d.nocbv=1;
        d.mssrc=!memcmp(hp(g,stg+16),hp(g,gx+0xa0),16)?1:!memcmp(hp(g,stg+16),hp(g,gx+0x90),16)?2:3;
        for(int k=0;k<8;k++){ u64 c=l->cbv[k]; if(!c) continue; d.nocbv=0; u64 cr=m_res_get(c>>32), cd=cr?rd64(g,cr+R_DATA):0, cn=cr?rd64(g,cr+R_SIZE):0, co=c&0xffffffffull;
            if(!cd||co+48>cn) continue;
            if(!memcmp(hp(g,cd+co+16),sb,32)) conf=1; else if(old<0&&!memcmp(hp(g,cd+co+32),hp(g,gx+0xa0),16)) old=k; }
        u32 b[4];
        if(!conf&&old>=0){ u64 c=l->cbv[old], cr=m_res_get(c>>32), cd=rd64(g,cr+R_DATA), co=c&0xffffffffull; b[0]=rd32(g,cd+co+16); b[1]=rd32(g,cd+co+20); b[2]=rd32(g,cd+co+36); b[3]=rd32(g,cd+co+44); d.src=2; }
        else { b[0]=sb[0]; b[1]=sb[1]; b[2]=sb[5]; b[3]=sb[7]; d.src=conf?1:3; }
        memcpy(&d.ox,&b[0],4); memcpy(&d.oy,&b[1],4); memcpy(&d.msy,&b[2],4); memcpy(&d.msw,&b[3],4); d.have_off=1; }
    kh_aspect_m_draw(g,&d);
}
static void m_list_call(KCtx *g,u64 o,int s){
    switch(s){
    case 10: case 11: { MList *l=m_list(o); u64 ls=l->list; memset(l,0,sizeof *l); l->list=ls; if(s==10) l->pso=A2(g); return; }   /* Reset(alloc, pso) / ClearState */
    case 12: m_draw(g,m_list(o),0,(u32)A1(g),(u32)A3(g),0); return;                                              /* DrawInstanced(count, inst, start, startinst) */
    case 13: m_draw(g,m_list(o),1,(u32)A1(g),(u32)A3(g),(s32)(u32)ARG(g,4)); return;                             /* DrawIndexedInstanced(count, inst, startidx, base, startinst) */
    case 22: kh_aspect_m_rs_scissor(g,(u32)A1(g),A2(g)); return;                                                 /* RSSetScissorRects(n, rects) */
    case 25: m_list(o)->pso=A1(g); return;                                                                        /* SetPipelineState */
    case 32: m_dt_binds++; return;                                                                                /* SetGraphicsRootDescriptorTable (counted only) */
    case 38: if((u32)A1(g)<8) m_list(o)->cbv[(u32)A1(g)]=A2(g); return;                                          /* SetGraphicsRootConstantBufferView(i, va) */
    case 43: { MList *l=m_list(o); u64 v=A1(g); if(v&&GA_OK(v)){ l->ib=rd64(g,v); l->ibsize=rd32(g,v+8); l->ibfmt=rd32(g,v+12); } else l->ib=0; return; }   /* IASetIndexBuffer */
    case 44: { MList *l=m_list(o); u64 v=A3(g); if((u32)A1(g)==0&&(u32)A2(g)>=1&&v&&GA_OK(v)){ l->vb=rd64(g,v); l->vbsize=rd32(g,v+8); l->vbstride=rd32(g,v+12); } return; }   /* IASetVertexBuffers(start, n, views) */
    default: return; } }
/* at CreateGraphicsPipelineState: the VS kind at pso+32 and the POSITION0 byte offset at pso+36 (input layout at desc+0x228 / +0x230,
 * 32-byte D3D12_INPUT_ELEMENT_DESC: name, index +8, format +12, slot +16, offset +20) */
static void m_pso(KCtx *g,u64 d,u64 pso){
    wr32(g,pso+32,(u32)m_vs_kind(g,rd64(g,d+0x08),rd64(g,d+0x10))); wr32(g,pso+36,0);
    u64 el=rd64(g,d+0x228); u32 ne=rd32(g,d+0x230);
    for(u32 i=0;i<ne&&i<32&&el&&GA_OK(el+32ull*i+31);i++){ u64 e=el+32ull*i, nm=rd64(g,e); if(nm&&GA_OK(nm)&&!strncmp((const char*)hp(g,nm),"POSITION",9)&&rd32(g,e+8)==0&&rd32(g,e+16)==0){ wr32(g,pso+36,rd32(g,e+20)==0xffffffffu?0:rd32(g,e+20)); break; } }
}
#undef R_DATA
#undef R_SIZE
static const char *fmt_name(u32 f){ static __thread char b[16]; switch(f){ case 71: return "BC1_UNORM"; case 72: return "BC1_SRGB"; case 74: return "BC2_UNORM"; case 75: return "BC2_SRGB"; case 77: return "BC3_UNORM"; case 78: return "BC3_SRGB";
    case 87: return "B8G8R8A8_UNORM"; case 91: return "B8G8R8A8_SRGB"; case 28: return "R8G8B8A8_UNORM"; case 49: return "R8G8_UNORM"; case 61: return "R8_UNORM"; case 103: return "NV12"; case 98: return "BC7_UNORM"; default: snprintf(b,sizeof b,"fmt%u",f); return b; } }
static int size_class(u64 w,u32 h){ u64 m=w>h?w:h; return m<=64?0:m<=128?1:m<=256?2:m<=512?3:m<=1024?4:m<=2048?5:6; }

#define OBJ_EX 32
static u64 new_obj(int c,u64 extra){ return kh_com_obj(c,extra); }
static void out_obj(KCtx *g,u64 pp,u64 obj){ if(pp) wr64(g,pp,obj); }

/* ---- formats ----------------------------------------------------------------------------------------------------------- */
static void fmt_info(u32 f,u32 *bw,u32 *bytes){
    *bw=1; *bytes=4;
    if(f>=70&&f<=72){*bw=4;*bytes=8;} else if(f>=73&&f<=78){*bw=4;*bytes=16;} else if(f>=79&&f<=81){*bw=4;*bytes=8;} else if(f>=82&&f<=84){*bw=4;*bytes=16;} else if(f>=94&&f<=99){*bw=4;*bytes=16;}
    else if(f==61||f==65||f==62) *bytes=1; else if(f==49||f==85||f==86||f==115||f==54||f==56||f==35) *bytes=2; else if(f==2||f==3||f==4) *bytes=16; else if(f==20||f==16||f==17) *bytes=8; else if(f==103) *bytes=1;
}

/* ---- resource ----------------------------------------------------------------------------------------------------------- */
#define R_DATA 32
#define R_SIZE 40
#define R_VA 48
#define R_DIM 56
#define R_FMT 60
#define R_DESC 64
#define R_UP 120
static u64 new_resource(KCtx *g,u64 desc,int placed_dim_hint){
    u64 r=new_obj(C_res,128); u32 dim=desc?rd32(g,desc):placed_dim_hint; u64 w=desc?rd64(g,desc+16):0; u32 fmt=desc?rd32(g,desc+32):0;
    wr32(g,r+R_DIM,dim); wr32(g,r+R_FMT,fmt); if(desc) memcpy(hp(g,r+R_DESC),hp(g,desc),56);
    { u64 id=atomic_fetch_add(&res_ids,1); wr64(g,r+R_VA,id<<32); if(__builtin_expect(kh_aspect_m,0)) m_res_set(id,r); } gfx_resources++;   /* task 40: id -> object for M */
    if(desc&&dim>=2&&dim<=4){
        u32 h=rd32(g,desc+24); u32 mips=rd16(g,desc+30),arr=rd16(g,desc+28),flags=rd32(g,desc+48); if(!mips) mips=1; if(!arr) arr=1;
        if(__builtin_expect(kh_aspect_log,0)) kh_aspect_note_texture(g,desc);   /* task 37: render-target / depth-stencil / multisampled sizes */
        if(flags&3) tx_rtds++;
        else { u32 bw,bpb; fmt_info(fmt,&bw,&bpb); u64 sz=0; for(u32 m=0;m<mips;m++){ u64 W=w>>m; if(!W) W=1; u32 H=h>>m; if(!H) H=1; sz+=((W+bw-1)/bw)*bpb*((H+bw-1)/bw); }
               sz*=(dim==2||dim==3)?arr:1; if(fmt<128){ tx_res[fmt]++; tx_res_bytes[fmt]+=sz; } tx_sized[dim==3?size_class(w,h):7]++; }
    }
    if(dim==1&&w){ u64 d=ga_alloc_src(w<16?16:w,256,GA_SRC_GFX); if(!d) kh_fatal(g,"null d3d12: guest arena exhausted for a %llu-byte buffer",(unsigned long long)w); memset(hp(g,d),0,w); wr64(g,r+R_DATA,d); wr64(g,r+R_SIZE,w); gfx_buffers_bytes+=w; }
    return r;
}

static u64 o_device;
/* methods every object shares: IDXGIObject / ID3D12Object slots 3..6 (private data, SetName) and ID3D12DeviceChild::GetDevice (slot 7) */
static int common(KCtx *g,int s,const char *c){
    if(s>=3&&s<=6){ SET_RAX(g,0); return 1; }
    if(s==7&&!strncmp(c,"d3d12:",6)&&strcmp(c,"d3d12:ID3D12Device")){ if(A2(g)) wr64(g,A2(g),o_device); SET_RAX(g,0); return 1; }
    return 0;
}
/* ---- handlers ----------------------------------------------------------------------------------------------------------- */
static int h_factory(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 8: SET_RAX(g,0); return 1;                                       /* MakeWindowAssociation */
    case 12: { u32 i=(u32)A1(g); if(i==0){ out_obj(g,A2(g),o_adapter); SET_RAX(g,0); } else { out_obj(g,A2(g),0); SET_RAX(g,DXGI_NOT_FOUND); } return 1; }   /* EnumAdapters1 */
    case 7: { u32 i=(u32)A1(g); if(i==0){ out_obj(g,A2(g),o_adapter); SET_RAX(g,0); } else { out_obj(g,A2(g),0); SET_RAX(g,DXGI_NOT_FOUND); } return 1; }    /* EnumAdapters */
    case 15: { /* CreateSwapChainForHwnd(this, queue, hwnd, desc, fsdesc, restrict, **pp) */
        u64 sc=new_obj(C_swap,96); u64 d=ARG(g,3); wr32(g,sc+32,d?rd32(g,d):1280); wr32(g,sc+36,d?rd32(g,d+4):720); wr32(g,sc+40,0);
        KWaitObj *w=NULL; (void)w; extern u64 kh_mk_event_h(int manual,int initial); wr64(g,sc+48,kh_mk_event_h(1,1));
        if(kh_gfx_vkd3d_on) kh_vkd3d_swap_attach(g,sc,A1(g),d);
        if(__builtin_expect(kh_aspect_log,0)) kh_aspect_note_swapchain(g,"CreateSwapChainForHwnd",d,rd32(g,sc+32),rd32(g,sc+36));
        out_obj(g,ARG(g,6),sc); SET_RAX(g,0); return 1; }
    default: return 0; } }
/* TASK38: the budget QueryVideoMemoryInfo answers (fixed; IOS-GRAPHICS-PLAN R11: changing it can change the game's texture-heap sizing,
 * so it is its own measured step).  The vkd3d backend's memory census prints it next to what the device and the OS say. */
#define DXGI_BUDGET_ANSWER (6ull<<30)
u64 kh_dxgi_budget_answer(void){ return DXGI_BUDGET_ANSWER; }
static int h_adapter(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 7: { u32 i=(u32)A1(g); if(i==0){ out_obj(g,A2(g),o_output); SET_RAX(g,0); } else { out_obj(g,A2(g),0); SET_RAX(g,DXGI_NOT_FOUND); } return 1; }   /* EnumOutputs */
    case 8: case 10: { u64 d=A1(g); memset(hp(g,d),0,s==8?0x118:0x138); utf8_to_gw(g,d,128,"KHNMC null GPU (headless)"); wr32(g,d+0x100,0x106b); wr32(g,d+0x104,1); wr64(g,d+0x110,8ull<<30); wr64(g,d+0x128,0x1001); SET_RAX(g,0); return 1; }   /* GetDesc / GetDesc1 */
    case 14: { u64 info=ARG(g,3); wr64(g,info,DXGI_BUDGET_ANSWER); wr64(g,info+8,64ull<<20); wr64(g,info+16,3ull<<30); wr64(g,info+24,0); SET_RAX(g,0); return 1; }   /* QueryVideoMemoryInfo(node, group, info) */
    default: return 0; } }
static int h_output(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    if(s==10){                                                                                                        /* WaitForVBlank */
        if(kh_vclock_on){ u64 ra=guest_return_addr(g)-GS_BASE;                 /* KHNMC_CLOCK=frames: one virtual display period; rbx = GXDevice at the timing thread's two call sites (0x1288cc, 0x128f3b) */
            kh_vsync_note_virtual();
            kh_vclock_vblank(g,(ra==0x1288cf||ra==0x128f3e)&&GA_OK(KH_RBX(g))?KH_RBX(g):0); SET_RAX(g,0); return 1; }
        if(kh_vsync_wait()){ SET_RAX(g,0); return 1; }                       /* TASK36: a display tick source (iOS display link, KHNMC_DISPLAY=timer60) paces it (gfx_present.c) */
        kh_vsync_note_sleep();
        struct timespec ts={0,vblank_ns}; nanosleep(&ts,0); SET_RAX(g,0); return 1; }                                 /* host clock: pace at 60 Hz (KHNMC_NULL_VBLANK_NS: another frame time, for timing experiments) */
    if(s==7){ u64 d=A1(g); memset(hp(g,d),0,0x60); utf8_to_gw(g,d,32,"\\\\.\\DISPLAY1"); int sw,sh; kh_screen_size(&sw,&sh);     /* task 37 (S2): 1920x1080 unless KHNMC_SCREEN */
        /* DesktopCoordinates RECT at +0x40, AttachedToDesktop +0x50 */ wr32(g,d+0x48,(u32)sw); wr32(g,d+0x4c,(u32)sh); wr32(g,d+0x50,1); wr64(g,d+0x58,0xAB0100); SET_RAX(g,0); return 1; }
    return 0; }
static int h_swap(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
    case 8: case 22: gfx_presents++;
        kh_phase_present(g,gfx_presents,gfx_draws,gfx_clears,gfx_pso_binds);
        fprintf(stderr,"kh: gfx frame=%llu bundles=%llu pso-binds=%llu ",(unsigned long long)gfx_presents,(unsigned long long)gfx_bundles,(unsigned long long)gfx_pso_binds);
        kh_gfx_report(stderr);
        if(gfx_presents%300==0) kh_gfx_tex_report(stderr);
        if(kh_gfx_vkd3d_on) kh_vkd3d_present(g,o,gfx_presents);            /* optional image dump of the buffer being presented */
        else kh_app_null_present_park();                                      /* TASK41 / review A41 (verification): the game stalls at its frame boundary while the app is SUSPENDED (background); a resign alone is the PC's focus loss (the game runs on); no GPU here */
        kh_screen_check_present(g,rd32(g,o+32),rd32(g,o+36),gfx_presents);  /* task 37 review: returns at once unless KHNMC_SCREEN is set */
        if(__builtin_expect(kh_aspect_log,0)) kh_aspect_note_present(g,gfx_presents);     /* task 37: GX size fields when they change */
        wr32(g,o+40,rd32(g,o+40)+1); SET_RAX(g,0); return 1;                 /* Present / Present1 */
    case 9: if(kh_gfx_vkd3d_on){ kh_vkd3d_swap_getbuffer(g,o); return 1; }
            { u32 i=(u32)A1(g)%3; u64 b=rd64(g,o+56+8*i); if(!b){ u64 desc=ga_alloc(56,16); memset(hp(g,desc),0,56); wr32(g,desc,3); wr64(g,desc+16,rd32(g,o+32)); wr32(g,desc+24,rd32(g,o+36)); wr16(g,desc+28,1); wr16(g,desc+30,1); wr32(g,desc+32,0x57); wr32(g,desc+36,1);
            b=new_resource(g,desc,3); ga_free(desc); wr64(g,o+56+8*i,b); }             /* the swap chain's own reference (count 1) */
            kh_com_ref_add(g,b); out_obj(g,A3(g),b); SET_RAX(g,0); return 1; }        /* GetBuffer(idx, riid, **pp): the caller gets a new reference (AddRef, as DXGI does) and releases it */
    case 10: SET_RAX(g,0); return 1;                                                                        /* SetFullscreenState */
    case 12: { u64 d=A1(g); memset(hp(g,d),0,0x48); wr32(g,d,rd32(g,o+32)); wr32(g,d+4,rd32(g,o+36)); wr32(g,d+0x10,0x57); wr32(g,d+0x1c,1); wr32(g,d+0x24,0x30); wr32(g,d+0x28,3); wr32(g,d+0x38,1); wr32(g,d+0x3c,3); wr32(g,d+0x40,0x42); SET_RAX(g,0); return 1; } /* GetDesc */
    case 13: if(__builtin_expect(kh_aspect_log,0)) kh_aspect_note_swapchain(g,"ResizeBuffers (asked)",0,(u32)A2(g),(u32)A3(g));   /* task 37: 0 = keep that side */
             if(kh_gfx_vkd3d_on){ u32 nw=(u32)A2(g)?(u32)A2(g):rd32(g,o+32), nh=(u32)A3(g)?(u32)A3(g):rd32(g,o+36); kh_vkd3d_swap_resize(g,o,nw,nh);
                 if(!(u32)KH_RAX(g)){ wr32(g,o+32,nw); wr32(g,o+36,nh); wr32(g,o+40,0); } return 1; }       /* real buffers: new size, back-buffer index restarts at 0 */
             wr32(g,o+32,(u32)A2(g)?(u32)A2(g):rd32(g,o+32)); wr32(g,o+36,(u32)A3(g)?(u32)A3(g):rd32(g,o+36)); SET_RAX(g,0); return 1;     /* ResizeBuffers(count,w,h,fmt,flags) */
    case 15: out_obj(g,A1(g),o_output); SET_RAX(g,0); return 1;                                             /* GetContainingOutput */
    case 18: { u64 d=A1(g); memset(hp(g,d),0,0x30); wr32(g,d,rd32(g,o+32)); wr32(g,d+4,rd32(g,o+36)); wr32(g,d+8,0x57); wr32(g,d+16,1); wr32(g,d+24,0x30); wr32(g,d+28,3); wr32(g,d+36,3); wr32(g,d+44,0x42); SET_RAX(g,0); return 1; }   /* GetDesc1 */
    case 19: { u64 d=A1(g); memset(hp(g,d),0,20); wr32(g,d+16,1); SET_RAX(g,0); return 1; }            /* GetFullscreenDesc: Windowed=TRUE */
    case 31: SET_RAX(g,0); return 1;                                                                        /* SetMaximumFrameLatency */
    case 33: SET_RAX(g,rd64(g,o+48)); return 1;                                                             /* GetFrameLatencyWaitableObject (manual-reset, always signalled) */
    case 36: SET_RAX(g,rd32(g,o+40)%3); return 1;                                                           /* GetCurrentBackBufferIndex */
    default: return 0; } }
/* KHNMC_GFX_PSOLOG=1: one line per CreateGraphicsPipelineState with a content key of the pipeline (hashes of the vertex and pixel
 * shader bytecode and of the fixed-function state of the D3D12 desc, no pointers), the game frame, the thread and the guest
 * return sites on the stack.  Two runs (two hosts) then compare as SETS of pipelines: which ones a run never created, and who
 * creates them.  The offsets are those of D3D12_GRAPHICS_PIPELINE_STATE_DESC on x64 (656 bytes). */
static int log_pso;
static u64 pso_fnv(KCtx *g,u64 a,u64 n){ u64 h=0xcbf29ce484222325ull; if(!a||!GA_OK(a)||(n&&!GA_OK(a+n-1))) return 0; const u8 *p=hp(g,a); for(u64 i=0;i<n;i++){ h^=p[i]; h*=0x100000001b3ull; } return h; }
static void pso_log(KCtx *g,u64 d){
    extern void kh_guest_bt(KCtx *,FILE *); extern u64 kh_game_frames(void);
    u64 vs=rd64(g,d+0x08), vsn=rd64(g,d+0x10), ps=rd64(g,d+0x18), psn=rd64(g,d+0x20);
    u64 st=pso_fnv(g,d+0x78,0x228-0x78)^(pso_fnv(g,d+0x238,0x278-0x238)*31)^((u64)rd32(g,d+0x230)<<56);     /* blend .. depth-stencil, strip-cut .. node mask, input element count */
    char *bt=NULL; size_t btn=0; FILE *m=open_memstream(&bt,&btn); char sites[400]; sites[0]=0;
    if(m){ kh_guest_bt(g,m); fclose(m); size_t o=0; int k=0;
        for(char *p=bt;p&&(p=strstr(p,"return-site rva 0x"))&&k<10;k++){ p+=18; size_t l=strspn(p,"0123456789abcdef"); if(o+l+2<sizeof sites){ memcpy(sites+o,p,l); o+=l; sites[o++]=' '; sites[o]=0; } }
        free(bt); }
    KThread *t=cur_thread();
    fprintf(stderr,"kh: gfx pso #%llu game-frame=%llu present=%llu thread=%u key=%016llx-%016llx-%016llx vs-bytes=%llu ps-bytes=%llu rtv0=%u nrt=%u dsv=%u topo=%u sites: %s\n",(unsigned long long)gfx_psos,
            (unsigned long long)kh_game_frames(),(unsigned long long)gfx_presents,t?t->tid:0,(unsigned long long)pso_fnv(g,vs,vsn),(unsigned long long)pso_fnv(g,ps,psn),(unsigned long long)st,
            (unsigned long long)vsn,(unsigned long long)psn,rd32(g,d+0x244),rd32(g,d+0x240),rd32(g,d+0x264),rd32(g,d+0x23c),sites);
}
static int h_dev(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 8: { u64 q=new_obj(C_queue,32); out_obj(g,ARG(g,3),q); SET_RAX(g,0); return 1; }                  /* CreateCommandQueue(desc, riid, **pp) */
    case 9: out_obj(g,ARG(g,3),new_obj(C_alloc,16)); SET_RAX(g,0); return 1;                               /* CreateCommandAllocator(type, riid, **pp) */
    case 10: { if(__builtin_expect(log_pso,0)) pso_log(g,A1(g));
             u64 p=new_obj(C_pso,16); if(__builtin_expect(kh_aspect_m,0)) m_pso(g,A1(g),p);                  /* task 40: M */
             out_obj(g,ARG(g,3),p); gfx_psos++; SET_RAX(g,0); return 1; }                                   /* CreateGraphicsPipelineState(desc, riid, **pp) */
    case 12: out_obj(g,ARG(g,6),new_obj(C_list,16)); SET_RAX(g,0); return 1;                               /* CreateCommandList(node,type,alloc,pso,riid,**pp) */
    case 13: { u32 feat=(u32)A1(g); u64 d=A2(g); u32 sz=(u32)A3(g);
        if(feat==4){ u32 sc=rd32(g,d+4); wr32(g,d+12,sc<=8?1:0); }                                          /* MULTISAMPLE_QUALITY_LEVELS: NumQualityLevels */
        else memset(hp(g,d),0,sz); SET_RAX(g,0); return 1; }
    case 14: { u64 hd=A1(g); u64 h=new_obj(C_dheap,48); u64 id=atomic_fetch_add(&heap_ids,1); wr32(g,h+32,rd32(g,hd)); wr32(g,h+36,rd32(g,hd+4)); wr32(g,h+40,rd32(g,hd+8)); wr64(g,h+48,0x10000000ull+0x1000000ull*id); wr64(g,h+56,0x800000000ull+0x1000000ull*id); out_obj(g,ARG(g,3),h); SET_RAX(g,0); return 1; }
    case 7: SET_RAX(g,1); return 1;                                                                         /* GetNodeCount */
    case 15: SET_RAX(g,32); return 1;                                                                       /* GetDescriptorHandleIncrementSize */
    case 16: out_obj(g,ARG(g,5),new_obj(C_rootsig,16)); SET_RAX(g,0); return 1;                            /* CreateRootSignature(node, blob, size, riid, **pp) */
    case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24: return 1;                      /* Create*View / CreateSampler / CopyDescriptors*: no-ops (void) */
    case 25: { /* GetResourceAllocationInfo(visibleMask, numDescs, descs): result struct returned through the hidden pointer in rdx */
        u64 out=A1(g); u32 n=(u32)A3(g); u64 descs=ARG(g,4); u64 total=0;
        for(u32 i=0;i<n;i++){ u64 d=descs+56ull*i; u32 dim=rd32(g,d); u64 w=rd64(g,d+16); u32 h=rd32(g,d+24); u32 depth=rd16(g,d+28); u32 mips=rd16(g,d+30); u32 fmt=rd32(g,d+32); if(!mips) mips=1; if(!depth) depth=1;
            if(dim==1){ total+=(w+65535)&~65535ull; continue; } u32 bw,bpb; fmt_info(fmt,&bw,&bpb); u64 sz=0; for(u32 m=0;m<mips;m++){ u64 W=w>>m; if(!W) W=1; u32 H=h>>m; if(!H) H=1; sz+=((W+bw-1)/bw)*bpb*((H+bw-1)/bw); } total+=((sz*depth)+65535)&~65535ull; }
        wr64(g,out,total); wr64(g,out+8,65536); SET_RAX(g,out); return 1; }
    case 27: { u64 r=new_resource(g,ARG(g,3),0); out_obj(g,ARG(g,7),r); SET_RAX(g,0); return 1; }          /* CreateCommittedResource(heapprops, flags, desc, state, clear, riid, **pp) */
    case 28: { u64 hh=new_obj(C_heap,16); out_obj(g,ARG(g,3),hh); SET_RAX(g,0); return 1; }                /* CreateHeap(desc, riid, **pp) */
    case 29: { u64 r=new_resource(g,ARG(g,3),0); out_obj(g,ARG(g,7),r); SET_RAX(g,0); return 1; }          /* CreatePlacedResource(heap, offset, desc, state, clear, riid, **pp) */
    case 36: { u64 f=new_obj(C_fence,16); wr64(g,f+32,A1(g)); out_obj(g,ARG(g,4),f); SET_RAX(g,0); return 1; }   /* CreateFence(initial, flags, riid, **pp) */
    case 37: SET_RAX(g,0); return 1;                                                                        /* GetDeviceRemovedReason */
    case 38: { /* GetCopyableFootprints(desc, first, num, base, layouts, rows, rowsizes, total) */
        u64 d=A1(g); u32 first=(u32)A2(g),num=(u32)A3(g); u64 base=ARG(g,4),pl=ARG(g,5),pr=ARG(g,6),prs=ARG(g,7),pt=ARG(g,8);
        u32 fmt=rd32(g,d+32); u64 w=rd64(g,d+16); u32 h=rd32(g,d+24); u32 mips=rd16(g,d+30); if(!mips) mips=1; u32 bw,bpb; fmt_info(fmt,&bw,&bpb); u64 run=base,total=0;
        for(u32 i=0;i<num;i++){ u32 sub=first+i; u32 mip=sub%mips; u64 W=w>>mip; if(!W) W=1; u32 H=h>>mip; if(!H) H=1; u64 rowsz=((W+bw-1)/bw)*bpb; u32 rows=(H+bw-1)/bw; u64 pitch=(rowsz+255)&~255ull;
            u64 off=(run+511)&~511ull; if(pl){ u64 L=pl+32ull*i; wr64(g,L,off); wr32(g,L+8,fmt); wr32(g,L+12,(u32)W); wr32(g,L+16,H); wr32(g,L+20,1); wr32(g,L+24,(u32)pitch); }
            if(pr) wr32(g,pr+4ull*i,rows); if(prs) wr64(g,prs+8ull*i,rowsz); run=off+pitch*(rows?rows-1:0)+rowsz; total=run-base; }
        if(pt) wr64(g,pt,total); return 1; }
    case 39: out_obj(g,ARG(g,3),new_obj(C_qheap,16)); SET_RAX(g,0); return 1;                              /* CreateQueryHeap(desc, riid, **pp) */
    default: return 0; } }
static int h_queue(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 10: gfx_execs++; return 1;                                                                         /* ExecuteCommandLists */
    case 14: { u64 f=A1(g); if(f) wr64(g,f+32,A2(g)); SET_RAX(g,0); return 1; }                           /* Signal(fence,value): completes immediately */
    case 15: SET_RAX(g,0); return 1;                                                                        /* Wait */
    case 16: wr64(g,A1(g),1000000000ull); SET_RAX(g,0); return 1;                                          /* GetTimestampFrequency */
    case 17: wr64(g,A1(g),kh_qpc()); wr64(g,A2(g),kh_qpc()); SET_RAX(g,0); return 1;                       /* GetClockCalibration */
    default: return 0; } }
static int h_fence(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
    case 8: SET_RAX(g,rd64(g,o+32)); return 1;                                                              /* GetCompletedValue */
    case 9: { u64 v=A1(g),ev=A2(g); if(rd64(g,o+32)<v) kh_fatal(g,"null d3d12: SetEventOnCompletion(%llu) but the fence only reached %llu (no GPU to complete it)",(unsigned long long)v,(unsigned long long)rd64(g,o+32));
        if(ev) kh_event_set(ev); SET_RAX(g,0); return 1; }
    case 10: wr64(g,o+32,A1(g)); SET_RAX(g,0); return 1;                                                    /* Signal */
    default: return 0; } }
static int h_alloc(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c; if(s==8){ SET_RAX(g,0); return 1; } return 0; }
static int h_list(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    if(__builtin_expect(kh_aspect_m,0)) m_list_call(g,o,s);                                                 /* task 40: M (records only) */
    switch(s){
    case 9: case 10: SET_RAX(g,0); return 1;                                                                /* Close / Reset */
    case 11: return 1;                                                                                      /* ClearState */
    case 12: case 13: gfx_draws++; return 1;                                                                /* Draw(Indexed)Instanced */
    case 16: gfx_copies++; { /* CopyTextureRegion(this, pDst, x, y, z, pSrc, pSrcBox) */
        u64 dl=A1(g),sl=ARG(g,5);
        if(!GA_OK(dl)||!GA_OK(sl)) kh_fatal(g,"null d3d12: CopyTextureRegion with invalid location pointers (dst=%llx src=%llx)",(unsigned long long)dl,(unsigned long long)sl);
        u64 dr=rd64(g,dl); u32 dt=rd32(g,dl+8),st=rd32(g,sl+8);
        if(dt==0&&st==1){                                                                                    /* upload-heap footprint -> texture subresource */
            if(!GA_OK(dr)||kh_com_cls_of(g,dr)!=C_res) kh_fatal(g,"null d3d12: CopyTextureRegion destination %llx is not a resource",(unsigned long long)dr);
            u32 fmt=rd32(g,dr+R_FMT),dim=rd32(g,dr+R_DIM); u32 bw,bpb; u32 ffmt=rd32(g,sl+24),fw=rd32(g,sl+28),fh=rd32(g,sl+32),fd=rd32(g,sl+36),pitch=rd32(g,sl+40);
            fmt_info(ffmt,&bw,&bpb); u64 bytes=(u64)pitch*(((u64)fh+bw-1)/bw)*(fd?fd:1);
            tx_up_calls++; tx_up_total_bytes+=bytes;
            if(fmt<128){ tx_up[fmt]++; tx_up_bytes[fmt]+=bytes; }
            { u64 w=rd64(g,dr+R_DESC+16); u32 h=rd32(g,dr+R_DESC+24); tx_up_sized[dim==3?size_class(w,h):7]++; }
            if(!rd32(g,dr+R_UP)){ wr32(g,dr+R_UP,1); tx_distinct_up++; }
        } else tx_other_up++;
        return 1; }
    case 15: case 17: gfx_copies++; return 1;                                                               /* CopyBufferRegion / CopyResource */
    case 19: gfx_copies++; return 1;                                                                        /* ResolveSubresource */
    case 27:
        if(!A1(g)||kh_com_cls_of(g,A1(g))!=C_list) kh_fatal(g,"null d3d12: ExecuteBundle requires a command list");
        gfx_bundles++; return 1; /* Bundle commands already update the null recorder when recorded. */
    case 25: gfx_pso_binds++; return 1;
    case 20: case 21: case 22: case 23: case 24: return 1;                                         /* topology, viewports, scissors, blend factor, stencil ref, SetPipelineState */
    case 26: gfx_barriers+=A1(g); return 1;                                                                 /* ResourceBarrier */
    case 28: case 30: case 32: case 38: case 43: case 44: case 46: return 1;                                /* SetDescriptorHeaps, root signature, tables, CBVs, IA buffers, OMSetRenderTargets */
    case 47: case 48: gfx_clears++; return 1;                                                               /* Clear DSV / RTV */
    case 53: case 54: return 1;                                                                             /* EndQuery / ResolveQueryData */
    default: return 0; } }
/* Release of a resource (task 30).  The null device keeps no reference of its own to a resource (views, barriers, copies and
 * command lists only read it during the call; ExecuteCommandLists runs nothing), the swap chain holds one per back buffer
 * (GetBuffer), so the last Release is the end of the resource as on Windows: its buffer data and the object are freed.  The
 * vtable pointer and class word are cleared first: a use after the last Release faults (or is the named fatal "is not a
 * resource" of CopyTextureRegion) instead of working by accident on memory that another resource may already hold. */
static void res_release(KCtx *g,u64 o){
    int last=0; u64 r=kh_com_ref_drop(g,o,&last); SET_RAX(g,r);
    if(!last) return;
    u64 d=rd64(g,o+R_DATA), n=rd64(g,o+R_SIZE);
    if(d){ wr64(g,o+R_DATA,0); ga_free(d); gfx_buffers_freed++; gfx_buffers_freed_bytes+=n; }
    if(__builtin_expect(kh_aspect_m,0)) m_res_set(rd64(g,o+R_VA)>>32,0);                                       /* task 40: M's id table */
    gfx_res_freed++; wr64(g,o,0); wr64(g,o+8,0); ga_free(o);
}
static int h_res(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
    case 2: res_release(g,o); return 1;                                                                     /* Release */
    case 8: { u64 d=rd64(g,o+R_DATA); if(A3(g)) wr64(g,A3(g),d); SET_RAX(g,d?0:E_FAIL_); return 1; }       /* Map(sub, range, **ppData) */
    case 9: return 1;                                                                                       /* Unmap */
    case 10: { u64 out=A1(g); memcpy(hp(g,out),hp(g,o+R_DESC),56); SET_RAX(g,out); return 1; }              /* GetDesc (hidden return pointer in rdx) */
    case 11: SET_RAX(g,rd64(g,o+R_VA)); return 1;                                                           /* GetGPUVirtualAddress */
    default: return 0; } }
static int h_dheap(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
    case 8: { u64 out=A1(g); wr32(g,out,rd32(g,o+32)); wr32(g,out+4,rd32(g,o+36)); wr32(g,out+8,rd32(g,o+40)); wr32(g,out+12,0); SET_RAX(g,out); return 1; }
    case 9: { u64 out=A1(g); wr64(g,out,rd64(g,o+48)); SET_RAX(g,out); return 1; }                          /* GetCPUDescriptorHandleForHeapStart */
    case 10: { u64 out=A1(g); wr64(g,out,rd64(g,o+56)); SET_RAX(g,out); return 1; }                         /* GetGPUDescriptorHandleForHeapStart */
    default: return 0; } }
static int h_blob(KCtx *g,u64 o,int s,const char *c){ (void)c; if(s==3){ SET_RAX(g,rd64(g,o+32)); return 1; } if(s==4){ SET_RAX(g,rd64(g,o+40)); return 1; } return 0; }
static int h_misc(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)g;(void)o;(void)s;(void)c; return 0; }

u64 kh_mk_event_h(int manual,int initial);
u64 kh_null_output_obj(void){ return o_output; }                           /* task 37 selftest (runtime/aspect.c): the DXGI output (S2) */
static u64 mk_blob(KCtx *g,u64 src,u64 size){ u64 b=new_obj(C_blob,16); u64 d=ga_alloc(size?size:16,16); if(src&&size) memcpy(hp(g,d),hp(g,src),size); wr64(g,b+32,d); wr64(g,b+40,size); return b; }

void kh_seam_gfx_init(void){
    const char *e=getenv("KH_LOG_GFX"); log_gfx=e&&*e=='1'; (void)log_gfx;
    e=getenv("KHNMC_GFX_PSOLOG"); log_pso=e&&*e=='1';
    e=getenv("KHNMC_NULL_VBLANK_NS"); if(e&&*e){ long v=strtol(e,0,10); if(v<1000000||v>500000000) kh_fatal(NULL,"KHNMC_NULL_VBLANK_NS=%s: expected 1000000..500000000 nanoseconds",e);
        vblank_ns=v; fprintf(stderr,"kh: gfx: null backend frame sleep %ld ns instead of 16666667 (KHNMC_NULL_VBLANK_NS)\n",v); }
    kh_vsync_init();                                                          /* TASK36: KHNMC_DISPLAY=timer60 (the Linux stand-in for the display link) */
    e=getenv("KHNMC_GFX");                                                  /* graphics backend: unset / "null" = this recorder, "vkd3d" = gfx_vkd3d.c, "vkd3dw" = gfx_wrap.c */
    if(e&&!strcmp(e,"vkd3d")) kh_gfx_vkd3d_init();
    else if(e&&!strcmp(e,"vkd3dw")) kh_gfx_wrap_init();
    else if(e&&*e&&strcmp(e,"null")) kh_fatal(NULL,"gfx-backend-unknown: KHNMC_GFX=%s (known backends: null, vkd3d, vkd3dw)",e);
    kh_present_init(kh_gfx_vkd3d_on);                                         /* TASK36 review n=5: KHNMC_GFX_PRESENT in every build, with every backend */
    { extern void kh_app_test_init(void); kh_app_test_init(); }                /* TASK41: KHNMC_TEST_GATE / KHNMC_TEST_SUSPEND (nothing when unset) */
    C_factory=kh_com_class("dxgi:IDXGIFactory4",44,STRICT,h_factory); C_adapter=kh_com_class("dxgi:IDXGIAdapter3",24,STRICT,h_adapter); C_output=kh_com_class("dxgi:IDXGIOutput",24,STRICT,h_output);
    C_swap=kh_com_class("dxgi:IDXGISwapChain3",44,STRICT,h_swap); C_dev=kh_com_class("d3d12:ID3D12Device",48,STRICT,h_dev); C_queue=kh_com_class("d3d12:ID3D12CommandQueue",20,STRICT,h_queue);
    /* the game creates and releases about 27 allocator + list pairs per frame (bundles): they are freed at their last Release, the other classes are few and kept */
    C_alloc=kh_com_class("d3d12:ID3D12CommandAllocator",12,STRICT|COM_FREE,h_alloc); C_list=kh_com_class("d3d12:ID3D12GraphicsCommandList",72,STRICT|COM_FREE,h_list); C_res=kh_com_class("d3d12:ID3D12Resource",16,STRICT,h_res);
    C_dheap=kh_com_class("d3d12:ID3D12DescriptorHeap",12,STRICT,h_dheap); C_fence=kh_com_class("d3d12:ID3D12Fence",12,STRICT,h_fence); C_pso=kh_com_class("d3d12:ID3D12PipelineState",10,STRICT,h_misc);
    C_rootsig=kh_com_class("d3d12:ID3D12RootSignature",9,STRICT,h_misc); C_heap=kh_com_class("d3d12:ID3D12Heap",10,STRICT,h_misc); C_qheap=kh_com_class("d3d12:ID3D12QueryHeap",9,STRICT,h_misc);
    C_blob=kh_com_class("d3dcompiler:ID3DBlob",5,STRICT,h_blob);
    o_adapter=kh_com_obj(C_adapter,0); o_output=kh_com_obj(C_output,0);
}
void kh_gfx_report(FILE *f){ fprintf(f,"kh: gfx: draws=%llu clears=%llu barriers=%llu execs=%llu presents=%llu copies=%llu psos=%llu resources=%llu upload/buffer bytes=%llu\n",(unsigned long long)gfx_draws,(unsigned long long)gfx_clears,(unsigned long long)gfx_barriers,(unsigned long long)gfx_execs,(unsigned long long)gfx_presents,(unsigned long long)gfx_copies,(unsigned long long)gfx_psos,(unsigned long long)gfx_resources,(unsigned long long)gfx_buffers_bytes); }

/* task 30: what the null device holds now (exit report and every map report): resources created / destroyed at their last Release,
 * buffer data allocated / freed.  Nothing for the vkd3d backends: their resources are vkd3d's. */
void kh_gfx_mem_report(FILE *f){
    if(kh_gfx_vkd3d_on) return;
    u64 rc=gfx_resources, rf=gfx_res_freed, bb=gfx_buffers_bytes, bf=gfx_buffers_freed_bytes;
    fprintf(f,"kh: gfx memory: resources created %llu, destroyed at their last Release %llu, live %llu; buffer data %llu MiB allocated, %llu MiB freed (%llu buffers), live %llu MiB\n",
            (unsigned long long)rc,(unsigned long long)rf,(unsigned long long)(rc-rf),(unsigned long long)(bb>>20),(unsigned long long)(bf>>20),(unsigned long long)gfx_buffers_freed,(unsigned long long)((bb-bf)>>20));
}
void kh_gfx_res_counts(u64 *created,u64 *destroyed,u64 *bytes,u64 *freed_bytes){ if(created) *created=gfx_resources; if(destroyed) *destroyed=gfx_res_freed; if(bytes) *bytes=gfx_buffers_bytes; if(freed_bytes) *freed_bytes=gfx_buffers_freed_bytes; }

void kh_gfx_tex_report(FILE *f){
    if(kh_gfx_vkd3d_on){ kh_gfx_vkd3d_report(f); return; }                    /* vkd3d owns the textures: no recorder statistics, the backend line instead */
    u64 tr=0,trb=0,tu=0,tub=0; for(int i=0;i<128;i++){ tr+=tx_res[i]; trb+=tx_res_bytes[i]; tu+=tx_up[i]; tub+=tx_up_bytes[i]; }
    fprintf(f,"kh: gfx tex: sampled-textures-created=%llu (%llu GPU bytes) rt/ds-created=%llu; footprint uploads=%llu (%llu bytes) into %llu distinct textures; other texture copies=%llu\n",(unsigned long long)tr,(unsigned long long)trb,(unsigned long long)tx_rtds,(unsigned long long)tu,(unsigned long long)tub,(unsigned long long)tx_distinct_up,(unsigned long long)tx_other_up);
    fprintf(f,"kh: gfx tex: created by format:"); for(int i=0;i<128;i++) if(tx_res[i]) fprintf(f," %s=%llu(%llu B)",fmt_name((u32)i),(unsigned long long)tx_res[i],(unsigned long long)tx_res_bytes[i]);
    fprintf(f,"\nkh: gfx tex: uploaded by format:"); for(int i=0;i<128;i++) if(tx_up[i]) fprintf(f," %s=%llu(%llu B)",fmt_name((u32)i),(unsigned long long)tx_up[i],(unsigned long long)tx_up_bytes[i]);
    fprintf(f,"\nkh: gfx tex: created by size (max edge):"); for(int i=0;i<8;i++) if(tx_sized[i]) fprintf(f," %s=%llu",size_names[i],(unsigned long long)tx_sized[i]);
    fprintf(f,"\nkh: gfx tex: uploaded by size (max edge):"); for(int i=0;i<8;i++) if(tx_up_sized[i]) fprintf(f," %s=%llu",size_names[i],(unsigned long long)tx_up_sized[i]);
    fprintf(f,"\n");
}

/* ---- free functions (imports) ------------------------------------------------------------------------------------------- */
IMP(dxgi,CreateDXGIFactory1){ kh_cnt_gfx_calls++; u64 fct=kh_com_obj(C_factory,0); wr64(g,A1(g),fct); SET_RAX(g,0); }
IMP(d3d12,ord101){ kh_cnt_gfx_calls++; /* D3D12CreateDevice(adapter, featureLevel, riid, **ppDevice) */ if(kh_gfx_vkd3d_on){ kh_vkd3d_create_device(g); return; } u64 pp=A3(g); if(!o_device) o_device=kh_com_obj(C_dev,0); if(pp){ wr64(g,pp,o_device); } SET_RAX(g,0); }
/* the null blob: 0x40 bytes as before, holding the 40-byte Win64 D3D12_ROOT_SIGNATURE_DESC and zeros (review U35 n=9: 64 bytes were read) */
IMP(d3d12,D3D12SerializeRootSignature){ kh_cnt_gfx_calls++; if(kh_gfx_vkd3d_on){ kh_vkd3d_serialize_rootsig(g); return; }
    u64 b=mk_blob(g,0,0x40), d=rd64(g,b+32); memset(hp(g,d),0,0x40); if(A0(g)) memcpy(hp(g,d),hp(g,A0(g)),40);
    wr64(g,A2(g),b); if(A3(g)) wr64(g,A3(g),0); SET_RAX(g,0); }
IMP(d3dcompiler_47,D3DStripShader){ kh_cnt_gfx_calls++; u64 b=mk_blob(g,A0(g),A1(g)); wr64(g,A3(g),b); SET_RAX(g,0); }

/* ---- task 30 selftest: a resource is destroyed at its last Release ------------------------------------------------------------ */
static u64 st_call8(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5,u64 a6,u64 a7){   /* this + 7 arguments through the guest ABI */
    u64 saved[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(saved[0]-0x200)&~15ull; memset(hp(g,sp),0,0x80); wr64(g,sp+0x20,a4); wr64(g,sp+0x28,a5); wr64(g,sp+0x30,a6); wr64(g,sp+0x38,a7);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RCX(g)=obj; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    kh_dispatch(g,rd64(g,rd64(g,obj)+8ull*(u64)slot)); u64 r=KH_RAX(g);
    KH_RSP(g)=saved[0]; KH_RCX(g)=saved[1]; KH_RDX(g)=saved[2]; KH_R8(g)=saved[3]; KH_R9(g)=saved[4]; return r;
}
static u64 far_live(void){ extern int ga_zone_get(int,u64*,u64*,u64*,u64*,u64*); u64 l=0; ga_zone_get(1,NULL,NULL,NULL,NULL,&l); return l; }
static int block_freed(KCtx *g,u64 p){ return rd32(g,p-16+4)==0xF4EEF4EEu; }      /* the arena's block header (core.c BHdr): magic MAGIC_FREE once ga_free took it */
typedef struct { KCtx *g; u64 obj; int rounds; _Atomic int lasts; } RefRace;
static void *ref_race(void *p){ RefRace *r=(RefRace*)p; for(int i=0;i<r->rounds;i++) kh_com_ref_add(r->g,r->obj);
    for(int i=0;i<r->rounds;i++){ int last=0; kh_com_ref_drop(r->g,r->obj,&last); if(last) r->lasts++; } return NULL; }
static int release_selftest(KCtx *g){
    int checks=0, fails=0;
#define RCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"kh: gfx release selftest FAIL line %d: %s\n",__LINE__,#c); } }while(0)
    /* the reference count itself: 8 host threads x 20,000 AddRef + Release on one object end at the count they started from, and no
     * Release of theirs is the last one (the object's own reference stays) */
    { int ck=kh_com_class("selftest:refcount",3,0,NULL); u64 o=kh_com_obj(ck,16); RefRace rr={g,o,20000,0}; pthread_t t[8];
      for(int i=0;i<8;i++) pthread_create(&t[i],NULL,ref_race,&rr); for(int i=0;i<8;i++) pthread_join(t[i],NULL);
      RCHECK(rd64(g,o+16)==1 && rr.lasts==0);
      int last=0; RCHECK(kh_com_ref_drop(g,o,&last)==0 && last==1); last=5; RCHECK(kh_com_ref_drop(g,o,&last)==0 && last==0 && rd64(g,o+16)==0);   /* 1 -> 0 is the last; 0 stays 0 and is not "last" again */
      ga_free(o); }
    if(kh_gfx_vkd3d_on){ fprintf(stderr,"kh: gfx release selftest: %d checks, %d failures (the null device's resources are not used with KHNMC_GFX=vkd3d*: those checks not run)\n",checks,fails); return fails; }
    u64 dev=kh_com_obj(C_dev,0), m=ga_alloc(0x200,16); memset(hp(g,m),0,0x200);
    u64 desc=m, pp=m+0x40, heap=m+0x80;
    u64 r0=gfx_resources, f0=gfx_res_freed, b0=gfx_buffers_bytes, fb0=gfx_buffers_freed_bytes;
    static const u64 sizes[2]={3u<<20 /* a page run */, 1000 /* a size-class block */};
    for(int k=0;k<2;k++){
        u64 w=sizes[k]; memset(hp(g,desc),0,56); wr32(g,desc,1); wr64(g,desc+16,w); wr32(g,desc+24,1); wr16(g,desc+28,1); wr16(g,desc+30,1); wr32(g,desc+36,1);
        u64 fl=far_live(); wr64(g,pp,0);
        RCHECK(st_call8(g,dev,27,heap,0,desc,0,0,0,pp)==0);                          /* CreateCommittedResource(heap, flags, desc, state, clear, riid, **pp) */
        u64 r=rd64(g,pp), vt=r?rd64(g,r):0; RCHECK(r && kh_com_cls_of(g,r)==C_res && rd64(g,r+16)==1);
        u64 d=r?rd64(g,r+R_DATA):0; RCHECK(d && rd64(g,r+R_SIZE)==w && far_live()>=fl+w && gfx_resources==r0+(u64)k+1);
        if(!r||!d) break;
        wr64(g,pp,0); RCHECK(st_call8(g,r,8,0,0,pp,0,0,0,0)==0 && rd64(g,pp)==d);       /* Map: the data pointer */
        wr8(g,d+w-1,0x5a);
        RCHECK(st_call8(g,r,1,0,0,0,0,0,0,0)==2);                                      /* AddRef */
        RCHECK(st_call8(g,r,2,0,0,0,0,0,0,0)==1 && rd64(g,r)==vt && rd64(g,r+R_DATA)==d && rd8(g,d+w-1)==0x5a && gfx_res_freed==f0+(u64)k);   /* Release: still referenced, nothing freed */
        RCHECK(st_call8(g,r,2,0,0,0,0,0,0,0)==0);                                      /* the last Release */
        RCHECK(rd64(g,r)!=vt && gfx_res_freed==f0+(u64)k+1 && gfx_buffers_freed_bytes==fb0+(k?sizes[0]:0)+w);   /* destroyed: vtable gone, counted */
        RCHECK(far_live()==fl && block_freed(g,r));                                  /* buffer data and object back in the arena */
        RCHECK(gfx_resources==r0+(u64)k+1 && gfx_buffers_bytes==b0+(k?sizes[0]:0)+w); /* created-so-far counters unchanged by the release */
    }
    /* a texture (no buffer data) is destroyed too; QueryInterface counts as a reference */
    { memset(hp(g,desc),0,56); wr32(g,desc,3); wr64(g,desc+16,64); wr32(g,desc+24,64); wr16(g,desc+28,1); wr16(g,desc+30,1); wr32(g,desc+32,71); wr32(g,desc+36,1);
      u64 fl=far_live(); wr64(g,pp,0); RCHECK(st_call8(g,dev,29,heap,0,desc,0,0,0,pp)==0);   /* CreatePlacedResource */
      u64 r=rd64(g,pp); RCHECK(r && !rd64(g,r+R_DATA) && far_live()==fl);
      if(r){ u64 q=m+0x100; wr64(g,q,0); RCHECK(st_call8(g,r,0,0,q,0,0,0,0,0)==0 && rd64(g,q)==r && rd64(g,r+16)==2);
             RCHECK(st_call8(g,r,2,0,0,0,0,0,0,0)==1 && kh_com_cls_of(g,r)==C_res); RCHECK(st_call8(g,r,2,0,0,0,0,0,0,0)==0 && block_freed(g,r)); } }
    /* the swap chain keeps its back buffers: GetBuffer hands out a NEW reference, the caller's Release does not destroy the buffer */
    { u64 sc=new_obj(C_swap,96); wr32(g,sc+32,64); wr32(g,sc+36,32); u64 f1=gfx_res_freed;
      wr64(g,pp,0); RCHECK(st_call8(g,sc,9,1,0,pp,0,0,0,0)==0); u64 b=rd64(g,pp); RCHECK(b && rd64(g,sc+56+8)==b && rd64(g,b+16)==2);
      if(b){ u64 vt=rd64(g,b); RCHECK(st_call8(g,b,2,0,0,0,0,0,0,0)==1 && rd64(g,b)==vt && gfx_res_freed==f1);
             wr64(g,pp,0); RCHECK(st_call8(g,sc,9,1,0,pp,0,0,0,0)==0 && rd64(g,pp)==b && rd64(g,b+16)==2);    /* the same buffer again, alive */
             RCHECK(st_call8(g,b,2,0,0,0,0,0,0,0)==1 && kh_com_cls_of(g,b)==C_res); } }
    ga_free(m); ga_free(dev);
    fprintf(stderr,"kh: gfx release selftest: %d checks, %d failures\n",checks,fails);
#undef RCHECK
    return fails;
}
static void *st_unsuspend_120(void *a){ (void)a; struct timespec ts={0,120000000}; nanosleep(&ts,0); kh_app_test_suspended(0); return NULL; }   /* TASK41 */
static void *st_gate_clear_300(void *a){ (void)a; struct timespec ts={0,300000000}; nanosleep(&ts,0); kh_gfx_gate_clear(); return NULL; }  /* review A41: the safety net of a park that must not happen */
int kh_gfx_selftest(KCtx *g){
    u64 list=new_obj(C_list,16), bundle=new_obj(C_list,16);
    u64 before=gfx_bundles;
    kh_call(g,rd64(g,rd64(g,list)+27*8),list,bundle,0,0);
    int fail=gfx_bundles!=before+1;
    gfx_bundles=before;
    u64 swap=new_obj(C_swap,96), out=ga_alloc(48,16);
    memset(hp(g,out),0xa5,48);
    kh_call(g,rd64(g,rd64(g,swap)+19*8),swap,out,0,0);
    fail+=rd32(g,out+16)!=1 || rd32(g,out+20)!=0xa5a5a5a5u || rd64(g,out+24)!=0xa5a5a5a5a5a5a5a5ull;
    ga_free(out);
    int checks=2;
    /* review U35 n=9: the null D3D12SerializeRootSignature reads the 40-byte D3D12_ROOT_SIGNATURE_DESC and nothing after it (it read
     * 64 bytes); the blob keeps its 0x40 bytes, the 24 after the descriptor are zero */
    if(!kh_gfx_vkd3d_on){ u64 d=ga_alloc(64,16), pp=ga_alloc(16,16); memset(hp(g,d),0xAA,64); memset(hp(g,d),0x11,40); wr64(g,pp,0); wr64(g,pp+8,0x55);
        u64 sv[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)}, sp=((sv[0]-0x100)&~15ull)-8; wr64(g,sp,0x7ff7ffff0000ull);
        KH_RSP(g)=sp; KH_RCX(g)=d; KH_RDX(g)=1; KH_R8(g)=pp; KH_R9(g)=pp+8; rt_imp_d3d12__D3D12SerializeRootSignature(g);   /* (desc, version 1, **blob, **error) */
        int ok=KH_RSP(g)==sp+8&&(u32)KH_RAX(g)==0; KH_RSP(g)=sv[0]; KH_RCX(g)=sv[1]; KH_RDX(g)=sv[2]; KH_R8(g)=sv[3]; KH_R9(g)=sv[4];
        u64 b=rd64(g,pp), bd=b?rd64(g,b+32):0; ok=ok&&b&&bd&&rd64(g,b+40)==0x40&&rd64(g,pp+8)==0;
        for(int i=0;ok&&i<64;i++) ok=rd8(g,bd+(u64)i)==(i<40?0x11:0);
        checks++; if(!ok){ fail++; fprintf(stderr,"kh: gfx selftest FAIL: D3D12SerializeRootSignature (null device): the blob is not the 40-byte descriptor followed by zeros\n"); }
        if(bd) ga_free(bd); if(b) ga_free(b); ga_free(d); ga_free(pp); }
    /* TASK41 / review A41 (verification): the null backend's Present parks while the app is SUSPENDED (the game stalls at its frame boundary):
     * suspended for 120 ms by another thread, Present returns only after that, one null park counted.  It does NOT park at the gate set
     * alone (a resign: Control Center, a call banner = the PC's focus loss, the game runs on); a thread clears that gate after 300 ms as a
     * safety net, and Present must return well before */
    if(!kh_gfx_vkd3d_on){ u64 sc=new_obj(C_swap,96); wr32(g,sc+32,64); wr32(g,sc+36,32); u64 np0=gfx_presents, p0=kh_app_null_parks(), gp0=kh_gfx_gate_parks();
        kh_app_test_suspended(1); pthread_t th; int ok=pthread_create(&th,NULL,st_unsuspend_120,NULL)==0; u64 t0=kh_host_ns();
        kh_call(g,rd64(g,rd64(g,sc)+8*8),sc,1,0,0); u64 dt=kh_host_ns()-t0; if(ok) pthread_join(th,NULL);
        ok=ok&&dt>=100000000ull&&kh_app_null_parks()==p0+1&&!kh_app_suspended()&&gfx_presents==np0+1; gfx_presents=np0;
        checks++; if(!ok){ fail++; fprintf(stderr,"kh: gfx selftest FAIL: the null backend's Present did not park while suspended (%.1f ms, null parks +%llu)\n",dt/1e6,(unsigned long long)(kh_app_null_parks()-p0)); kh_app_test_suspended(0); }
        kh_gfx_gate_set(); ok=pthread_create(&th,NULL,st_gate_clear_300,NULL)==0; t0=kh_host_ns();
        kh_call(g,rd64(g,rd64(g,sc)+8*8),sc,1,0,0); dt=kh_host_ns()-t0; if(ok) pthread_join(th,NULL); else kh_gfx_gate_clear();
        ok=ok&&dt<100000000ull&&kh_gfx_gate_parks()==gp0&&kh_app_null_parks()==p0+1&&gfx_presents==np0+1; gfx_presents=np0;
        checks++; if(!ok){ fail++; fprintf(stderr,"kh: gfx selftest FAIL: the null backend's Present parked at a gate set alone (a resign: %.1f ms, gate parks +%llu)\n",dt/1e6,(unsigned long long)(kh_gfx_gate_parks()-gp0)); } }
    fprintf(stderr,"kh: gfx selftest: %d checks, %d failures\n",checks,fail);
    fail+=release_selftest(g);                                                /* task 30: resources destroyed at their last Release */
    fail+=kh_wrap_selftest_pure(g);                                           /* the wrapped bridge's translation layer: needs no device, runs with every backend */
    if(kh_gfx_vkd3d_on) fail+=kh_gfx_vkd3d_selftest(g);                       /* KHNMC_GFX=vkd3d / vkd3dw ./khnmc --selftest: device, clear, present, image dump */
    return fail;
}
