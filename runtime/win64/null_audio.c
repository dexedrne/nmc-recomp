/* null_audio.c : XAudio2 null engine (headless).  IXAudio2 + source/submix/mastering voices accept every call, record nothing, never
 * call back and report an empty queue (BuffersQueued = 0), 48 kHz stereo.  Exports resolved through LoadLibraryExW/GetProcAddress
 * (XAudio2CreateWithVersionInfo, XAudio2Create) and the three ordinal imports (ord1 XAudio2Create-like, ord2/ord3 effect factories). */
#include "khnmc_rt.h"
static int C_xa2,C_src,C_sub,C_mast,C_xapo;
void kh_seam_audio_register_procs(void);
static int h_xa2(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    switch(s){
    case 3: case 4: SET_RAX(g,0); return 1;                                                        /* (Un)RegisterForCallbacks */
    case 5: wr64(g,A1(g),kh_com_obj(C_src,32)); SET_RAX(g,0); return 1;                           /* CreateSourceVoice(**pp, fmt, flags, maxratio, cb, sends, fx) */
    case 6: wr64(g,A1(g),kh_com_obj(C_sub,32)); SET_RAX(g,0); return 1;                            /* CreateSubmixVoice */
    case 7: wr64(g,A1(g),kh_com_obj(C_mast,32)); SET_RAX(g,0); return 1;                           /* CreateMasteringVoice */
    case 8: case 9: case 10: SET_RAX(g,0); return 1;                                               /* StartEngine / StopEngine / CommitChanges */
    case 11: memset(hp(g,A1(g)),0,0x28); return 1;                                                 /* GetPerformanceData */
    case 12: return 1;                                                                             /* SetDebugConfiguration */
    default: return 0; } }
static int h_voice(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==0){ u64 d=A1(g); wr32(g,d,0); wr32(g,d+4,0); wr32(g,d+8,2); wr32(g,d+12,48000); return 1; }   /* GetVoiceDetails */
    if(s==25&&!strcmp(c,"xa2:IXAudio2SourceVoice")){ memset(hp(g,A1(g)),0,24); return 1; }              /* GetState: nothing queued */
    SET_RAX(g,0); return 1; }                                                                           /* every other voice method: S_OK */
static int h_xapo(KCtx *g,u64 o,int s,const char *c){ (void)g;(void)o;(void)s;(void)c; return 0; }
void kh_seam_audio_init(void){
    C_xa2=kh_com_class("xa2:IXAudio2",16,COM_STRICT,h_xa2); C_src=kh_com_class("xa2:IXAudio2SourceVoice",32,COM_NOUNK,h_voice); C_sub=kh_com_class("xa2:IXAudio2SubmixVoice",24,COM_NOUNK,h_voice);
    C_mast=kh_com_class("xa2:IXAudio2MasteringVoice",24,COM_NOUNK,h_voice); C_xapo=kh_com_class("xa2:effect",16,0,h_xapo);
    kh_seam_audio_register_procs();
}
IMP(xaudio2_9,ord1){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xa2,16)); SET_RAX(g,0); }       /* (IXAudio2**, flags, processor): movie audio engine */
IMP(xaudio2_9,ord2){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xapo,16)); SET_RAX(g,0); }       /* HRESULT f(IUnknown**): reverb effect factory */
IMP(xaudio2_9,ord3){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xapo,16)); SET_RAX(g,0); }       /* HRESULT f(IUnknown**): volume meter factory */
/* dynamically resolved creators: GetProcAddress returns these host-fn addresses */
static void xa2_create(KCtx *g){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xa2,16)); SET_RAX(g,0); KH_RET(g); }
void kh_seam_audio_register_procs(void){
    u64 a=kh_host_fn("xaudio2_9!XAudio2Create",xa2_create); kh_register_proc("xaudio2_9.dll","XAudio2Create",a);
    u64 b=kh_host_fn("xaudio2_9!XAudio2CreateWithVersionInfo",xa2_create); kh_register_proc("xaudio2_9.dll","XAudio2CreateWithVersionInfo",b);
}
