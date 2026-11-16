/* null_mf.c : Media Foundation / D3D11 movie-decode null backend.  MFStartup succeeds, attributes/media-type/DXGI-manager objects
 * accept every call, D3D11CreateDevice yields a null device, and MFCreateSourceReaderFromURL fails with
 * MF_E_UNSUPPORTED_BYTESTREAM_TYPE (no movie is ever decoded in a headless run). */
#include "khnmc_rt.h"
static int C_attr,C_mgr,C_mt,C_d3d11,C_ctx;
static int h_lenient(KCtx *g,u64 o,int s,const char *c){ (void)g;(void)o;(void)s;(void)c; return 0; }
void kh_seam_mf_init(void){
    C_attr=kh_com_class("mf:IMFAttributes",48,0,h_lenient); C_mgr=kh_com_class("mf:IMFDXGIDeviceManager",16,0,h_lenient); C_mt=kh_com_class("mf:IMFMediaType",56,0,h_lenient);
    C_d3d11=kh_com_class("d3d11:ID3D11Device",48,0,h_lenient); C_ctx=kh_com_class("d3d11:ID3D11DeviceContext",64,0,h_lenient);
}
IMP(mfplat,MFStartup){ kh_cnt_mf_calls++; SET_RAX(g,0); }
IMP(mfplat,MFShutdown){ kh_cnt_mf_calls++; SET_RAX(g,0); }
IMP(mfplat,MFCreateAttributes){ kh_cnt_mf_calls++; wr64(g,A0(g),kh_com_obj(C_attr,16)); SET_RAX(g,0); }
IMP(mfplat,MFCreateDXGIDeviceManager){ kh_cnt_mf_calls++; wr32(g,A0(g),1); wr64(g,A1(g),kh_com_obj(C_mgr,16)); SET_RAX(g,0); }
IMP(mfplat,MFCreateMediaType){ kh_cnt_mf_calls++; wr64(g,A0(g),kh_com_obj(C_mt,16)); SET_RAX(g,0); }
IMP(mfplat,MFllMulDiv){ kh_cnt_mf_calls++; __int128 p=(__int128)(s64)A0(g)*(s64)A1(g)+(s64)A3(g); s64 c=(s64)A2(g); SET_RAX(g,c?(u64)(s64)(p/c):0); }
IMP(mfreadwrite,MFCreateSourceReaderFromURL){ kh_cnt_mf_calls++; wr64(g,A2(g),0); SET_RAX(g,0xC00D36C4u); }
IMP(d3d11,D3D11CreateDevice){ kh_cnt_mf_calls++; /* (adapter, type, sw, flags, levels*, n, sdk, **dev, *level, **ctx) */ if(ARG(g,7)) wr64(g,ARG(g,7),kh_com_obj(C_d3d11,16)); if(ARG(g,8)) wr32(g,ARG(g,8),0xb100); if(ARG(g,9)) wr64(g,ARG(g,9),kh_com_obj(C_ctx,16)); SET_RAX(g,0); }
