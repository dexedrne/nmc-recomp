/* null_d3d12.c : headless D3D12 / DXGI backend.  A "null device" that accepts the documented 75-method surface (docs/graphics/D3D12-SHIM.md),
 * counts and optionally logs every call, and returns plausible success values: one adapter (VendorId 0x106b, 8 GiB), one output, a swap chain
 * of three B8G8R8A8 buffers, fences that complete the instant they are signalled, upload buffers backed by real guest memory so the game's
 * persistent maps work, copyable footprints computed from the resource description.  Anything outside the documented surface is a named fatal.
 * Also: d3dcompiler_47!D3DStripShader (copy), D3D12SerializeRootSignature (blob holding the desc), D3D11CreateDevice (movie decode device). */
#include "khnmc_rt.h"
#include <time.h>

#define E_FAIL_ 0x80004005u
#define DXGI_NOT_FOUND 0x887A0002u
#define STRICT COM_STRICT
static int C_factory,C_adapter,C_output,C_swap,C_dev,C_queue,C_alloc,C_list,C_res,C_dheap,C_fence,C_pso,C_rootsig,C_heap,C_qheap,C_blob;
static u64 o_adapter,o_output;
static _Atomic u64 res_ids=1, heap_ids=1;
_Atomic u64 gfx_draws, gfx_clears, gfx_barriers, gfx_execs, gfx_presents, gfx_copies, gfx_psos, gfx_resources, gfx_buffers_bytes;
void kh_gfx_report(FILE *f);
void kh_gfx_tex_report(FILE *f);
static int log_gfx;
_Atomic u64 gfx_bundles, gfx_pso_binds;
/* KHNMC_GFX=vkd3d (gfx_vkd3d.c): the device is a host vkd3d device; only the DXGI objects below stay ours, and the swap chain gets real back buffers */
extern int kh_gfx_vkd3d_on;
void kh_gfx_vkd3d_init(void); void kh_vkd3d_create_device(KCtx *g); void kh_vkd3d_serialize_rootsig(KCtx *g); void kh_vkd3d_swap_attach(KCtx *g,u64 sc,u64 queue,u64 desc);
void kh_vkd3d_swap_getbuffer(KCtx *g,u64 o); void kh_vkd3d_swap_resize(KCtx *g,u64 o,u32 w,u32 h); void kh_vkd3d_present(KCtx *g,u64 o,u64 n); void kh_gfx_vkd3d_report(FILE *f); int kh_gfx_vkd3d_selftest(KCtx *g);
/* texture recorder: sampled textures (no RT/DS flag) by DXGI format and by size class, and the footprint uploads that fill them (CopyTextureRegion) */
static _Atomic u64 tx_res[128], tx_res_bytes[128], tx_up[128], tx_up_bytes[128], tx_distinct_up, tx_rtds, tx_sized[8], tx_up_sized[8], tx_up_total_bytes, tx_up_calls, tx_other_up;
static const char *size_names[8]={"<=64","128","256","512","1024","2048",">2048","1D/3D"};
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
    wr64(g,r+R_VA,(atomic_fetch_add(&res_ids,1))<<32); gfx_resources++;
    if(desc&&dim>=2&&dim<=4){
        u32 h=rd32(g,desc+24); u32 mips=rd16(g,desc+30),arr=rd16(g,desc+28),flags=rd32(g,desc+48); if(!mips) mips=1; if(!arr) arr=1;
        if(flags&3) tx_rtds++;
        else { u32 bw,bpb; fmt_info(fmt,&bw,&bpb); u64 sz=0; for(u32 m=0;m<mips;m++){ u64 W=w>>m; if(!W) W=1; u32 H=h>>m; if(!H) H=1; sz+=((W+bw-1)/bw)*bpb*((H+bw-1)/bw); }
               sz*=(dim==2||dim==3)?arr:1; if(fmt<128){ tx_res[fmt]++; tx_res_bytes[fmt]+=sz; } tx_sized[dim==3?size_class(w,h):7]++; }
    }
    if(dim==1&&w){ u64 d=ga_alloc(w<16?16:w,256); if(!d) kh_fatal(g,"null d3d12: guest arena exhausted for a %llu-byte buffer",(unsigned long long)w); memset(hp(g,d),0,w); wr64(g,r+R_DATA,d); wr64(g,r+R_SIZE,w); gfx_buffers_bytes+=w; }
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
        out_obj(g,ARG(g,6),sc); SET_RAX(g,0); return 1; }
    default: return 0; } }
static int h_adapter(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 7: { u32 i=(u32)A1(g); if(i==0){ out_obj(g,A2(g),o_output); SET_RAX(g,0); } else { out_obj(g,A2(g),0); SET_RAX(g,DXGI_NOT_FOUND); } return 1; }   /* EnumOutputs */
    case 8: case 10: { u64 d=A1(g); memset(hp(g,d),0,s==8?0x118:0x138); utf8_to_gw(g,d,128,"KHNMC null GPU (headless)"); wr32(g,d+0x100,0x106b); wr32(g,d+0x104,1); wr64(g,d+0x110,8ull<<30); wr64(g,d+0x128,0x1001); SET_RAX(g,0); return 1; }   /* GetDesc / GetDesc1 */
    case 14: { u64 info=ARG(g,3); wr64(g,info,6ull<<30); wr64(g,info+8,64ull<<20); wr64(g,info+16,3ull<<30); wr64(g,info+24,0); SET_RAX(g,0); return 1; }   /* QueryVideoMemoryInfo(node, group, info) */
    default: return 0; } }
static int h_output(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    if(s==10){ struct timespec ts={0,16666667}; nanosleep(&ts,0); SET_RAX(g,0); return 1; }                          /* WaitForVBlank: pace at 60 Hz */
    if(s==7){ u64 d=A1(g); memset(hp(g,d),0,0x60); utf8_to_gw(g,d,32,"\\\\.\\DISPLAY1");
        /* DesktopCoordinates RECT at +0x40, AttachedToDesktop +0x50 */ wr32(g,d+0x48,1920); wr32(g,d+0x4c,1080); wr32(g,d+0x50,1); wr64(g,d+0x58,0xAB0100); SET_RAX(g,0); return 1; }
    return 0; }
static int h_swap(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
    case 8: case 22: gfx_presents++;
        kh_phase_present(g,gfx_presents,gfx_draws,gfx_clears,gfx_pso_binds);
        fprintf(stderr,"kh: gfx frame=%llu bundles=%llu pso-binds=%llu ",(unsigned long long)gfx_presents,(unsigned long long)gfx_bundles,(unsigned long long)gfx_pso_binds);
        kh_gfx_report(stderr);
        if(gfx_presents%300==0) kh_gfx_tex_report(stderr);
        if(kh_gfx_vkd3d_on) kh_vkd3d_present(g,o,gfx_presents);            /* optional image dump of the buffer being presented */
        wr32(g,o+40,rd32(g,o+40)+1); SET_RAX(g,0); return 1;                 /* Present / Present1 */
    case 9: if(kh_gfx_vkd3d_on){ kh_vkd3d_swap_getbuffer(g,o); return 1; }
            { u32 i=(u32)A1(g)%3; u64 b=rd64(g,o+56+8*i); if(!b){ u64 desc=ga_alloc(56,16); memset(hp(g,desc),0,56); wr32(g,desc,3); wr64(g,desc+16,rd32(g,o+32)); wr32(g,desc+24,rd32(g,o+36)); wr16(g,desc+28,1); wr16(g,desc+30,1); wr32(g,desc+32,0x57); wr32(g,desc+36,1);
            b=new_resource(g,desc,3); ga_free(desc); wr64(g,o+56+8*i,b); } out_obj(g,A3(g),b); SET_RAX(g,0); return 1; }   /* GetBuffer(idx, riid, **pp) */
    case 10: SET_RAX(g,0); return 1;                                                                        /* SetFullscreenState */
    case 12: { u64 d=A1(g); memset(hp(g,d),0,0x48); wr32(g,d,rd32(g,o+32)); wr32(g,d+4,rd32(g,o+36)); wr32(g,d+0x10,0x57); wr32(g,d+0x1c,1); wr32(g,d+0x24,0x30); wr32(g,d+0x28,3); wr32(g,d+0x38,1); wr32(g,d+0x3c,3); wr32(g,d+0x40,0x42); SET_RAX(g,0); return 1; } /* GetDesc */
    case 13: if(kh_gfx_vkd3d_on){ u32 nw=(u32)A2(g)?(u32)A2(g):rd32(g,o+32), nh=(u32)A3(g)?(u32)A3(g):rd32(g,o+36); kh_vkd3d_swap_resize(g,o,nw,nh);
                 if(!(u32)KH_RAX(g)){ wr32(g,o+32,nw); wr32(g,o+36,nh); wr32(g,o+40,0); } return 1; }       /* real buffers: new size, back-buffer index restarts at 0 */
             wr32(g,o+32,(u32)A2(g)?(u32)A2(g):rd32(g,o+32)); wr32(g,o+36,(u32)A3(g)?(u32)A3(g):rd32(g,o+36)); SET_RAX(g,0); return 1;     /* ResizeBuffers(count,w,h,fmt,flags) */
    case 15: out_obj(g,A1(g),o_output); SET_RAX(g,0); return 1;                                             /* GetContainingOutput */
    case 18: { u64 d=A1(g); memset(hp(g,d),0,0x30); wr32(g,d,rd32(g,o+32)); wr32(g,d+4,rd32(g,o+36)); wr32(g,d+8,0x57); wr32(g,d+16,1); wr32(g,d+24,0x30); wr32(g,d+28,3); wr32(g,d+36,3); wr32(g,d+44,0x42); SET_RAX(g,0); return 1; }   /* GetDesc1 */
    case 19: { u64 d=A1(g); memset(hp(g,d),0,20); wr32(g,d+16,1); SET_RAX(g,0); return 1; }            /* GetFullscreenDesc: Windowed=TRUE */
    case 31: SET_RAX(g,0); return 1;                                                                        /* SetMaximumFrameLatency */
    case 33: SET_RAX(g,rd64(g,o+48)); return 1;                                                             /* GetFrameLatencyWaitableObject (manual-reset, always signalled) */
    case 36: SET_RAX(g,rd32(g,o+40)%3); return 1;                                                           /* GetCurrentBackBufferIndex */
    default: return 0; } }
static int h_dev(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)o;(void)c;
    switch(s){
    case 8: { u64 q=new_obj(C_queue,32); out_obj(g,ARG(g,3),q); SET_RAX(g,0); return 1; }                  /* CreateCommandQueue(desc, riid, **pp) */
    case 9: out_obj(g,ARG(g,3),new_obj(C_alloc,16)); SET_RAX(g,0); return 1;                               /* CreateCommandAllocator(type, riid, **pp) */
    case 10: out_obj(g,ARG(g,3),new_obj(C_pso,16)); gfx_psos++; SET_RAX(g,0); return 1;                    /* CreateGraphicsPipelineState(desc, riid, **pp) */
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
static int h_res(KCtx *g,u64 o,int s,const char *c){ if(common(g,s,c)) return 1; (void)c;
    switch(s){
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
static u64 mk_blob(KCtx *g,u64 src,u64 size){ u64 b=new_obj(C_blob,16); u64 d=ga_alloc(size?size:16,16); if(src&&size) memcpy(hp(g,d),hp(g,src),size); wr64(g,b+32,d); wr64(g,b+40,size); return b; }

void kh_seam_gfx_init(void){
    const char *e=getenv("KH_LOG_GFX"); log_gfx=e&&*e=='1'; (void)log_gfx;
    e=getenv("KHNMC_GFX");                                                    /* graphics backend: unset / "null" = this recorder, "vkd3d" = gfx_vkd3d.c */
    if(e&&!strcmp(e,"vkd3d")) kh_gfx_vkd3d_init();
    else if(e&&*e&&strcmp(e,"null")) kh_fatal(NULL,"gfx-backend-unknown: KHNMC_GFX=%s (known backends: null, vkd3d)",e);
    C_factory=kh_com_class("dxgi:IDXGIFactory4",44,STRICT,h_factory); C_adapter=kh_com_class("dxgi:IDXGIAdapter3",24,STRICT,h_adapter); C_output=kh_com_class("dxgi:IDXGIOutput",24,STRICT,h_output);
    C_swap=kh_com_class("dxgi:IDXGISwapChain3",44,STRICT,h_swap); C_dev=kh_com_class("d3d12:ID3D12Device",48,STRICT,h_dev); C_queue=kh_com_class("d3d12:ID3D12CommandQueue",20,STRICT,h_queue);
    C_alloc=kh_com_class("d3d12:ID3D12CommandAllocator",12,STRICT,h_alloc); C_list=kh_com_class("d3d12:ID3D12GraphicsCommandList",72,STRICT,h_list); C_res=kh_com_class("d3d12:ID3D12Resource",16,STRICT,h_res);
    C_dheap=kh_com_class("d3d12:ID3D12DescriptorHeap",12,STRICT,h_dheap); C_fence=kh_com_class("d3d12:ID3D12Fence",12,STRICT,h_fence); C_pso=kh_com_class("d3d12:ID3D12PipelineState",10,STRICT,h_misc);
    C_rootsig=kh_com_class("d3d12:ID3D12RootSignature",9,STRICT,h_misc); C_heap=kh_com_class("d3d12:ID3D12Heap",10,STRICT,h_misc); C_qheap=kh_com_class("d3d12:ID3D12QueryHeap",9,STRICT,h_misc);
    C_blob=kh_com_class("d3dcompiler:ID3DBlob",5,STRICT,h_blob);
    o_adapter=kh_com_obj(C_adapter,0); o_output=kh_com_obj(C_output,0);
}
void kh_gfx_report(FILE *f){ fprintf(f,"kh: gfx: draws=%llu clears=%llu barriers=%llu execs=%llu presents=%llu copies=%llu psos=%llu resources=%llu upload/buffer bytes=%llu\n",(unsigned long long)gfx_draws,(unsigned long long)gfx_clears,(unsigned long long)gfx_barriers,(unsigned long long)gfx_execs,(unsigned long long)gfx_presents,(unsigned long long)gfx_copies,(unsigned long long)gfx_psos,(unsigned long long)gfx_resources,(unsigned long long)gfx_buffers_bytes); }

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
IMP(d3d12,D3D12SerializeRootSignature){ kh_cnt_gfx_calls++; if(kh_gfx_vkd3d_on){ kh_vkd3d_serialize_rootsig(g); return; } u64 b=mk_blob(g,A0(g),0x40); wr64(g,A2(g),b); if(A3(g)) wr64(g,A3(g),0); SET_RAX(g,0); }
IMP(d3dcompiler_47,D3DStripShader){ kh_cnt_gfx_calls++; u64 b=mk_blob(g,A0(g),A1(g)); wr64(g,A3(g),b); SET_RAX(g,0); }

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
    fprintf(stderr,"kh: gfx selftest: 2 checks, %d failures\n",fail);
    if(kh_gfx_vkd3d_on) fail+=kh_gfx_vkd3d_selftest(g);                       /* KHNMC_GFX=vkd3d ./khnmc --selftest: device, clear, present, image dump */
    return fail;
}
