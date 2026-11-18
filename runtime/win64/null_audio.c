/* Device discovery and the XAudio2 COM surface, two backends selected by KHNMC_AUDIO:
 *   (unset) | null   headless null device: valid voices with retained state, no rendering, no callbacks, buffers report drained.
 *   wav:<file>       software path: every IXAudio2 is a KmxEngine (audio_mix.c), a dedicated guest thread ("xa2-mix") runs one
 *                    processing pass per 10 ms device period (48 kHz, 480 frames), delivers the engine and voice callbacks in
 *                    XAudio2 order, hosts the engine's own XAPOs (guest code) through the IXAPO calling sequence, feeds the
 *                    volume meter and writes the mastering voice's output as a 16-bit stereo WAV.  Nothing is sent to a sound device.
 *   pull             the same software path; the output goes into the ring of khnmc_audio_out.h for a device that pulls
 *                    (kh_audio_out_pull), and the passes follow the device's consumption (TASK28; policy in that header).
 *   pull-test:<file> pull + a simulated device (runtime/audio_out.c) that writes what it pulled to <file>; the mixer's own output
 *                    of the same run goes to <file>.mix.wav (the WAV path) for a sample-for-sample comparison.
 * Voices are created hidden and published only when their sends and effect chain are in place (TASK28: the boot failure "the mastering
 * voice produces 8 channels" was a pass between CreateMasteringVoice's voice and its chain).  With KHNMC_CLOCK=frames the calls
 * that change what a pass computes are stamped with the virtual time and applied at the first pass later than their stamp
 * (audio_mix.c "deferred mode"; KHNMC_AUDIO_APPLY=immediate turns that off for comparisons), and the pass grid is anchored to the
 * first virtual vblank.
 * KHNMC_AUDIOLOG=<file> appends one line per created voice / submitted buffer / state change in either mode.
 * System effects: ord3 = volume meter (real levels in the software path), ord2 = reverb: audio passes unchanged, counted.
 * Not implemented, named fatals: operation sets (deferred calls), voice and output filters, xWMA and other compressed tags,
 * default down-mix matrices, a mastering voice at another rate than 48 kHz, a non-stereo mastering output. */
#include "khnmc_rt.h"
#include "khnmc_mix.h"
#include "khnmc_audio_out.h"
#include <time.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
static int C_xa2,C_src,C_sub,C_mast,C_xapo,C_enum,C_device,C_client;
#define XA2_E_INVALID 0x88960001u
/* ---- mode, log ---- */
static int soft_mode, soft_manual;                 /* soft_manual: tests drive the passes, no clock thread, no WAV */
static int out_pull, out_pulltest;                 /* KHNMC_AUDIO=pull / pull-test:<file> */
static int apply_mode;                             /* KHNMC_AUDIO_APPLY: 0 = stamped (default with KHNMC_CLOCK=frames), 1 = immediate */
static char pulltest_path[1024];
/* KHNMC_AUDIO_PEAKLOG=<file>[,<threshold>] (TASK28 level analysis): every pass whose device output exceeds the threshold (default 1.0 =
 * full scale) is written with the voices that carried signal in it (audio_mix.c kmx_engine_peak_dump), up to 400 passes */
static FILE *peak_f; static float peak_thr=1.0f; static u64 peak_n, peak_passes_over;
static _Atomic int ring_closed;                    /* the run's report has closed the output: later passes go nowhere */
static _Atomic int mix_quit;                       /* tests: the mixer thread ends at its next pass instant */
static void (*soft_test_mid_create)(KCtx *g,int slot);   /* selftest: called between the creation of a mixer voice and its sends / chain */
#ifdef KH_M28_VISIBLE_AT_ONCE_MUTANT               /* mutation check of the selftest only: voices take part from their creation on (the old behaviour) */
#define SOFT_HIDDEN 0u
#else
#define SOFT_HIDDEN KMX_VOICE_HIDDEN
#endif
/* KHNMC_AUDIO_WAV_GAIN_DB=<dB, 0 or negative>: gain applied to the device output on its way into the WAV (default 0 dB).  The battle
 * mix peaks above full scale (+5 dBFS seen; Windows' shared-mode engine has a limiter, this path has none) and a 16-bit file clips. */
static float wav_gain=1.0f; static double wav_gain_db;
static char wav_path[1024];
static FILE *alog_f; static int alog_init, alog_all; static pthread_mutex_t alog_m=PTHREAD_MUTEX_INITIALIZER;   /* alog_all: KHNMC_AUDIOLOG_ALL=1, no per-kind line limits (TASK28) */
static void alog(const char *fmt,...){
    pthread_mutex_lock(&alog_m);
    if(!alog_init){ alog_init=1; const char *a=getenv("KHNMC_AUDIOLOG_ALL"); alog_all=a&&*a=='1'; const char *p=getenv("KHNMC_AUDIOLOG"); if(p&&*p){ alog_f=fopen(p,"a"); if(!alog_f) kh_fatal(NULL,"audio: cannot open KHNMC_AUDIOLOG %s",p); setvbuf(alog_f,NULL,_IOLBF,0); } }
    if(alog_f){ if(kh_vclock_on) fprintf(alog_f,"[v=%llu vbl=%llu] ",(unsigned long long)kh_vclock_now_ns(),(unsigned long long)kh_vclock_vblanks());   /* frames clock: every line carries the virtual time and vblank */
        KThread *me=cur_thread(); if(me) fprintf(alog_f,"{%s} ",me->name);                                                 /* TASK28: the calling thread */
        va_list ap; va_start(ap,fmt); vfprintf(alog_f,fmt,ap); va_end(ap); fputc('\n',alog_f); }
    pthread_mutex_unlock(&alog_m);
}
#define RVA(a) ((unsigned long long)((a)-GS_BASE))
#define ULL(a) ((unsigned long long)(a))
static const u8 mmcls[16]={0x95,0x03,0xde,0xbc,0x2f,0xe5,0x7c,0x46,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e};
static const u8 enum_iid[16]={0xd2,0x64,0x56,0xa9,0x14,0x96,0x35,0x4f,0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6};
static const u8 client_iid[16]={0x4c,0xad,0xb9,0x1c,0xfa,0xdb,0x32,0x4c,0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2};
static const char *enum_methods[]={"QueryInterface","AddRef","Release","EnumAudioEndpoints","GetDefaultAudioEndpoint","GetDevice","RegisterEndpointNotificationCallback","UnregisterEndpointNotificationCallback"};
static const char *device_methods[]={"QueryInterface","AddRef","Release","Activate","OpenPropertyStore","GetId","GetState"};
static const char *client_methods[]={"QueryInterface","AddRef","Release","Initialize","GetBufferSize","GetStreamLatency","GetCurrentPadding","IsFormatSupported","GetMixFormat","GetDevicePeriod","Start","Stop","Reset","SetEventHandle","GetService"};
static const char *engine_methods[]={"QueryInterface","AddRef","Release","RegisterForCallbacks","UnregisterForCallbacks","CreateSourceVoice","CreateSubmixVoice","CreateMasteringVoice","StartEngine","StopEngine","CommitChanges","GetPerformanceData","SetDebugConfiguration"};
static const char *voice_methods[]={"GetVoiceDetails","SetOutputVoices","SetEffectChain","EnableEffect","DisableEffect","GetEffectState","SetEffectParameters","GetEffectParameters","SetFilterParameters","GetFilterParameters","SetOutputFilterParameters","GetOutputFilterParameters","SetVolume","GetVolume","SetChannelVolumes","GetChannelVolumes","SetOutputMatrix","GetOutputMatrix","DestroyVoice","Start","Stop","SubmitSourceBuffer","FlushSourceBuffers","Discontinuity","ExitLoop","GetState","SetFrequencyRatio","GetFrequencyRatio","SetSourceSampleRate"};
static void missing(KCtx *g,const char *c,const char *method){
    kh_fatal(g,"unimplemented COM method %s::%s (call-site RVA 0x%llx)",c,method,(unsigned long long)(guest_return_addr(g)-GS_BASE));
}
static u64 mix_format(KCtx *g){
    u64 p=ga_calloc(1,40);
    wr16(g,p,0xfffe); wr16(g,p+2,2); wr32(g,p+4,48000); wr32(g,p+8,384000);
    wr16(g,p+12,8); wr16(g,p+14,32); wr16(g,p+16,22); wr16(g,p+18,32); wr32(g,p+20,3);
    static const u8 ieee_float[16]={3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    memcpy(hp(g,p+24),ieee_float,16); return p;
}
static int h_device(KCtx *g,u64 o,int s,const char *c){
    int id=kh_com_cls_of(g,o);
    if(s<3) return 0;
    if(id==C_enum && s==4){ if(!A3(g)){ SET_RAX(g,0x80004003u); return 1; } wr64(g,A3(g),kh_com_obj(C_device,0)); }
    else if(id==C_device && s==3){
        if(!A4(g)){ SET_RAX(g,0x80004003u); return 1; }
        wr64(g,A4(g),0);
        if(!A1(g)||memcmp(hp(g,A1(g)),client_iid,16)){ SET_RAX(g,0x80004002u); return 1; }
        wr64(g,A4(g),kh_com_obj(C_client,0));
    } else if(id==C_device && s==5){
        if(!A1(g)){ SET_RAX(g,0x80004003u); return 1; }
        static const u16 name[]={'k','h','n','m','c','-','n','u','l','l',0};
        u64 p=ga_alloc(sizeof name,16); memcpy(hp(g,p),name,sizeof name); wr64(g,A1(g),p);
    } else if(id==C_client && s==8){
        if(!A1(g)){ SET_RAX(g,0x80004003u); return 1; } wr64(g,A1(g),mix_format(g));
    } else if(id==C_client && s==3){ /* Initialize: discovery-only client, no WASAPI stream. */
    } else if(id==C_client && s==4){
        if(!A1(g)){ SET_RAX(g,0x80004003u); return 1; } wr32(g,A1(g),480);
    } else missing(g,c,id==C_enum?enum_methods[s]:id==C_device?device_methods[s]:client_methods[s]);
    SET_RAX(g,0); return 1;
}
int kh_audio_cocreate(KCtx *g){
    if(!A0(g)||memcmp(hp(g,A0(g)),mmcls,16)) return 0;
    if(!A4(g)){ SET_RAX(g,0x80004003u); return 1; }
    wr64(g,A4(g),0);
    if(A1(g)){ SET_RAX(g,0x80040110u); return 1; }
    if(!A3(g)||memcmp(hp(g,A3(g)),enum_iid,16)){ SET_RAX(g,0x80004002u); return 1; }
    wr64(g,A4(g),kh_com_obj(C_enum,0)); SET_RAX(g,0); return 1;
}
typedef struct { u64 object; u32 enabled, channels, size; void *params; } Effect;
typedef struct Matrix { u64 dest; u32 src, dst; void *values; struct Matrix *next; } Matrix;
struct SoftVoice;
typedef struct { u32 flags, count; Effect *effects; u32 channels[64]; Matrix *matrices; struct SoftVoice *soft; } Voice;
static Voice *voice_state(KCtx *g,u64 o){ return (Voice*)(uintptr_t)rd64(g,o+48); }
static void clear_effects(Voice *v){
    for(u32 i=0;i<v->count;i++) free(v->effects[i].params);
    free(v->effects); v->effects=NULL; v->count=0;
}
static int effect_chain(KCtx *g,Voice *v,u64 chain){
    u32 n=chain?rd32(g,chain):0; u64 desc=n?rd64(g,chain+4):0;
    if(n>64 || (n&&!desc)) return 0;
    Effect *e=n?calloc(n,sizeof(*e)):NULL;
    if(n&&!e) kh_fatal(g,"null audio: effect allocation failed");
    for(u32 i=0;i<n;i++){
        u64 d=desc+16ull*i; e[i].object=rd64(g,d); e[i].enabled=!!rd32(g,d+8); e[i].channels=rd32(g,d+12);
        if(!e[i].channels||e[i].channels>64){ free(e); return 0; }
    }
    clear_effects(v); v->effects=e; v->count=n; return 1;
}
/* =========================================================================================================================
 * software path (KHNMC_AUDIO=wav:<file>)
 * ======================================================================================================================= */
typedef struct SoftFx { int kind; u64 obj, vtbl, xapo, params, fmt_in, fmt_out, lockp, procp, pbuf; u32 regflags, in_ch, out_ch, pcap; int locked, stat; void *blob; u32 blob_size; } SoftFx;
typedef struct SoftVoice SoftVoice;
typedef struct SoftEngine { KmxEngine *mx; u64 obj, cbs[8]; int ncb, inuse, manual; struct SoftEngine *next; SoftVoice *voices; } SoftEngine;
struct SoftVoice { u64 obj, cb; KmxVoice *mv; SoftEngine *eng; SoftFx *fx; u32 nfx; int fmt; SoftVoice *next, **prevp;
    u64 buffers, bytes; float ratio_min, ratio_max, vol_min, vol_max; int ratio_set, vol_set; };
typedef struct { u32 tag, ch, rate, bits, align; u64 voices, buffers, bytes, frames; } FmtStat;
typedef struct { u64 vtbl; char name[40]; u64 chains, process; } XapoStat;
static pthread_mutex_t soft_m=PTHREAD_MUTEX_INITIALIZER, wav_m=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t soft_cv=PTHREAD_COND_INITIALIZER;
static SoftEngine *engines; static KmxWav *wav; static int clock_started, wav_since_sync;
static FmtStat fmts[64]; static int nfmts; static XapoStat xapos[32]; static int nxapos;
static _Atomic u64 st_max_late_ns, st_max_pass_ns, st_pass_ns, st_spaced;
static _Atomic u64 st_cb_ns[6], st_cb_max[6], st_ecb_ns[2], st_ecb_max[2], st_fx_ns, st_fx_max;   /* wall time spent inside guest code on the mixer thread */
static u64 mono_ns(void);
#define TIMED(tot,max,call) do{ u64 t0_=mono_ns(); call; u64 d_=mono_ns()-t0_; tot+=d_; if(d_>max) max=d_; }while(0)
static _Atomic u64 st_cb[6], st_ecb[2], st_quanta, st_late, st_resync, st_setparams, st_reverb_params, st_submit_rejected, st_null_buffers, st_null_bytes, st_created[3];
static KmxStats st_dead;                         /* totals of engines that were released */
static u64 g_iid_xapo, g_iid_params;
#define GADDR(p) ((u64)((uintptr_t)(p)-gs_bias))
static const char *fmt_tag_name(u32 tag){ return tag==1?"PCM":tag==2?"MS-ADPCM":tag==3?"IEEE-float":tag==0x161?"WMAudio2":tag==0x162?"WMAudio3":tag==0x166?"XMA2":"other"; }
static int fmt_stat(u32 tag,u32 ch,u32 rate,u32 bits,u32 align){
    pthread_mutex_lock(&soft_m); int i;
    for(i=0;i<nfmts;i++) if(fmts[i].tag==tag&&fmts[i].ch==ch&&fmts[i].rate==rate&&fmts[i].bits==bits&&fmts[i].align==align) break;
    if(i==nfmts){ if(nfmts<64){ fmts[i]=(FmtStat){tag,ch,rate,bits,align,0,0,0,0}; nfmts++; } else i=63; }
    fmts[i].voices++; pthread_mutex_unlock(&soft_m); return i;
}
static u64 gcall(KCtx *g,u64 target,int n,const u64 *a){          /* guest call with any number of integer arguments (Win64) */
    u64 save=KH_RSP(g), sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(save-0x100)&~0xFull;
    for(int i=4;i<n;i++) wr64(g,sp+8ull*(u64)i,a[i]);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0;
    kh_dispatch(g,target);
    u64 r=KH_RAX(g); KH_RSP(g)=save; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3]; return r;
}
static u64 vslot(KCtx *g,u64 obj,int slot){ return rd64(g,rd64(g,obj)+8ull*(u64)slot); }
/* class name of a guest object from its MSVC RTTI (vtable[-1] -> complete object locator -> type descriptor) */
static void rtti_name(KCtx *g,u64 vtbl,char *out,size_t cap){
    snprintf(out,cap,"vtbl+0x%llx",RVA(vtbl));
    if(vtbl<GS_BASE+0x1000||vtbl>=GS_BASE+IMG_SIZE) return;
    u64 col=rd64(g,vtbl-8); if(col<GS_BASE+0x1000||col>=GS_BASE+IMG_SIZE-24) return;
    u64 td=GS_BASE+rd32(g,col+12); if(td<GS_BASE+0x1000||td>=GS_BASE+IMG_SIZE-80) return;
    const char *n=gstr(g,td+16); if(strncmp(n,".?AV",4)) return;
    size_t k=0; for(n+=4;*n&&*n!='@'&&k+1<cap;n++) out[k++]=*n; out[k]=0;
}
static int xapo_stat(KCtx *g,u64 vtbl){
    pthread_mutex_lock(&soft_m); int i;
    for(i=0;i<nxapos;i++) if(xapos[i].vtbl==vtbl) break;
    if(i==nxapos){ if(nxapos<32){ memset(&xapos[i],0,sizeof xapos[i]); xapos[i].vtbl=vtbl; rtti_name(g,vtbl,xapos[i].name,sizeof xapos[i].name); nxapos++; } else i=31; }
    xapos[i].chains++; pthread_mutex_unlock(&soft_m); return i;
}
static void *soft_alloc(size_t n){ u64 p=ga_alloc_src(n?n:64,64,GA_SRC_AUDIO); if(!p) return NULL; void *h=(void*)(uintptr_t)(p+gs_bias); memset(h,0,n); return h; }
static void soft_release(void *p){ ga_free(GADDR(p)); }
/* KMX_EV_* numbers are the IXAudio2VoiceCallback / IXAudio2EngineCallback vtable slots */
static void soft_voice_event(void *ctx,void *vu,int ev,u64 arg){
    KCtx *g=ctx; SoftVoice *sv=vu; if(!sv||!sv->cb) return;
    st_cb[ev]++; TIMED(st_cb_ns[ev],st_cb_max[ev],kh_call(g,vslot(g,sv->cb,ev),sv->cb,arg,0,0));
}
static void soft_engine_event(void *ctx,void *eu,int ev){
    KCtx *g=ctx; SoftEngine *e=eu; u64 cbs[8]; int n;
    pthread_mutex_lock(&soft_m); n=e->ncb; memcpy(cbs,e->cbs,sizeof cbs); pthread_mutex_unlock(&soft_m);
    for(int i=0;i<n;i++){ st_ecb[ev]++; TIMED(st_ecb_ns[ev],st_ecb_max[ev],kh_call(g,vslot(g,cbs[i],ev),cbs[i],0,0,0)); }
}
/* one guest XAPO, one quantum: IXAPO::Process(1, &in, 1, &out, IsEnabled) with XAPO_PROCESS_BUFFER_PARAMETERS {pBuffer, BufferFlags, ValidFrameCount} */
/* XAudio2 hands IXAPOParameters::SetParameters to an effect on its audio processing thread, never while IXAPO::Process runs (XAPO
 * rule: "no synchronization between SetParameters and Process is necessary"), and the engine's own XAPOs are written for that: its
 * IIR filter rebuilds its coefficients in SetParameters.  Here the game's threads reach SetParameters directly (IXAudio2Voice::
 * SetEffectParameters, issued for every voice by the sound driver's periodic handlers) while the mixer thread is inside Process:
 * one pass could run on half-written coefficients.  Seen once (TASK18, run v3: a single 480-frame burst at 16 times full scale in
 * the pass that followed a SetEffectParameters on a just-started voice; on the frames clock the handlers wake at the very instants
 * the passes start, which makes the overlap likely).  fx_m serialises the two guest calls; nothing else is held across them. */
static pthread_mutex_t fx_m=PTHREAD_MUTEX_INITIALIZER; static _Atomic u64 st_fx_waits;
#ifdef KH_FX_NOLOCK_MUTANT                           /* mutation check of the selftest only: the old, unserialised behaviour */
static void fx_lock(void){}
#define pthread_mutex_unlock_fx() ((void)0)
#else
static void fx_lock(void){ if(pthread_mutex_trylock(&fx_m)){ st_fx_waits++; pthread_mutex_lock(&fx_m); } }
#define pthread_mutex_unlock_fx() pthread_mutex_unlock(&fx_m)
#endif
static void soft_fx_process(void *ctx,void *fu,float *in,u32 ic,int in_silent,float *out,u32 oc,u32 frames,int enabled,int *out_silent){
    KCtx *g=ctx; SoftFx *f=fu; u64 p=f->procp; (void)ic; (void)oc;
    wr64(g,p,GADDR(in)); wr32(g,p+8,in_silent?0:1); wr32(g,p+12,frames);
    wr64(g,p+16,GADDR(out)); wr32(g,p+24,1); wr32(g,p+28,frames);
    u64 a[6]={f->xapo,1,p,1,p+16,(u64)(enabled?1:0)};
    fx_lock(); TIMED(st_fx_ns,st_fx_max,gcall(g,vslot(g,f->xapo,10),6,a)); pthread_mutex_unlock_fx();
    *out_silent=rd32(g,p+24)==0;
    __atomic_fetch_add(&xapos[f->stat].process,1,__ATOMIC_RELAXED);
}
static void soft_voice_free(void *vu){ free(vu); }
/* deferred mode: the stamp of a call is the guest clock (the virtual one with KHNMC_CLOCK=frames) */
static uint64_t soft_now(void *eu){ (void)eu; return kh_vclock_now_ns(); }
static _Atomic u64 st_params_deferred;
/* deferred mode: IXAPOParameters::SetParameters delivered on the mixer thread before the pass it belongs to, as XAudio2 does */
static void soft_fx_params(void *ctx,void *fu,const void *blob,uint32_t size){
    KCtx *g=ctx; SoftFx *f=fu; if(!f->params) return;
    if(size>f->pcap){ if(f->pbuf) ga_free(f->pbuf); f->pbuf=ga_alloc(size,16); f->pcap=size; if(!f->pbuf) kh_fatal(g,"audio: guest allocation of %u bytes of effect parameters failed",size); }
    memcpy(hp(g,f->pbuf),blob,size); st_params_deferred++;
    fx_lock(); kh_call(g,vslot(g,f->params,3),f->params,f->pbuf,size,0); pthread_mutex_unlock_fx();
}
static const KmxHost soft_host={soft_alloc,soft_release,soft_engine_event,soft_voice_event,soft_fx_process,soft_voice_free,soft_now,soft_fx_params};

static void soft_fx_teardown(KCtx *g,SoftFx *fx,u32 n){
    for(u32 i=0;i<n;i++){ SoftFx *f=&fx[i];
        if(f->kind==KMX_FX_HOST){
            if(f->locked) kh_call(g,vslot(g,f->xapo,9),f->xapo,0,0,0);          /* UnlockForProcess */
            if(f->params) kh_call(g,vslot(g,f->params,2),f->params,0,0,0);
            if(f->xapo) kh_call(g,vslot(g,f->xapo,2),f->xapo,0,0,0);
            if(f->fmt_in) ga_free(f->fmt_in); if(f->fmt_out) ga_free(f->fmt_out); if(f->lockp) ga_free(f->lockp); if(f->procp) ga_free(f->procp); if(f->pbuf) ga_free(f->pbuf);
        } else if(f->obj){ u64 r=rd64(g,f->obj+16); if(r) wr64(g,f->obj+16,r-1); }
        free(f->blob);
    }
    free(fx);
}
static u64 float_format(KCtx *g,u32 ch,u32 rate){
    u64 p=ga_calloc(1,24); if(!p) kh_fatal(g,"audio: guest allocation failed");
    wr16(g,p,3); wr16(g,p+2,(u16)ch); wr32(g,p+4,rate); wr32(g,p+8,rate*ch*4); wr16(g,p+12,(u16)(ch*4)); wr16(g,p+14,32); wr16(g,p+16,0); return p;
}
/* XAudio2's sequence for one effect of a chain: QueryInterface(IXAPO, IXAPOParameters), GetRegistrationProperties, format checks, LockForProcess */
static u32 soft_xapo_open(KCtx *g,SoftFx *f,u32 rate,u32 frames){
    static const u8 iid_xapo[16]={0x84,0xb9,0x10,0xa4,0x39,0x98,0x19,0x48,0xa0,0xbe,0x28,0x56,0xae,0x6b,0x3a,0xdb};
    static const u8 iid_params[16]={0x66,0x5c,0xd9,0x26,0xf2,0x80,0x9a,0x49,0xad,0x54,0x5a,0xe7,0xf0,0x1c,0x6d,0x98};
    pthread_mutex_lock(&soft_m);
    if(!g_iid_xapo){ g_iid_xapo=ga_alloc(16,16); g_iid_params=ga_alloc(16,16); if(!g_iid_xapo||!g_iid_params) kh_fatal(g,"audio: guest allocation failed"); memcpy(hp(g,g_iid_xapo),iid_xapo,16); memcpy(hp(g,g_iid_params),iid_params,16); }
    pthread_mutex_unlock(&soft_m);
    u64 out=ga_calloc(1,16); if(!out) kh_fatal(g,"audio: guest allocation failed");
    u32 hr=(u32)kh_call(g,vslot(g,f->obj,0),f->obj,g_iid_xapo,out,0);
    if(hr||!rd64(g,out)){ alog("  effect obj=%llx (vtbl rva %llx) is not an IXAPO: QueryInterface %08x",ULL(f->obj),RVA(f->vtbl),hr); ga_free(out); return XA2_E_INVALID; }
    f->xapo=rd64(g,out); wr64(g,out,0);
    if(!(u32)kh_call(g,vslot(g,f->obj,0),f->obj,g_iid_params,out,0)) f->params=rd64(g,out);
    wr64(g,out,0); hr=(u32)kh_call(g,vslot(g,f->xapo,3),f->xapo,out,0,0);
    u64 props=rd64(g,out); ga_free(out);
    if(hr||!props){ alog("  XAPO %llx GetRegistrationProperties failed %08x",ULL(f->obj),hr); return XA2_E_INVALID; }
    f->regflags=rd32(g,props+0x418); ga_free(props);                               /* XAPO_REGISTRATION_PROPERTIES.Flags; block came from CoTaskMemAlloc */
    if(((f->regflags&1)||(f->regflags&0x20))&&f->in_ch!=f->out_ch){ alog("  XAPO %llx needs matching channels (flags %x) but the chain asks %u -> %u",ULL(f->obj),f->regflags,f->in_ch,f->out_ch); return XA2_E_INVALID; }
    f->fmt_in=float_format(g,f->in_ch,rate); f->fmt_out=float_format(g,f->out_ch,rate);
    hr=(u32)kh_call(g,vslot(g,f->xapo,5),f->xapo,f->fmt_in,f->fmt_out,0);           /* IsOutputFormatSupported(input, requested output, NULL) */
    if(!hr) hr=(u32)kh_call(g,vslot(g,f->xapo,4),f->xapo,f->fmt_out,f->fmt_in,0);   /* IsInputFormatSupported(output, requested input, NULL) */
    if(hr){ alog("  XAPO %llx rejects float32 %u ch -> %u ch at %u Hz: %08x",ULL(f->obj),f->in_ch,f->out_ch,rate,hr); return XA2_E_INVALID; }
    f->lockp=ga_calloc(1,32); f->procp=ga_calloc(1,32); if(!f->lockp||!f->procp) kh_fatal(g,"audio: guest allocation failed");
    wr64(g,f->lockp,f->fmt_in); wr32(g,f->lockp+8,frames); wr64(g,f->lockp+16,f->fmt_out); wr32(g,f->lockp+24,frames);   /* XAPO_LOCKFORPROCESS_PARAMETERS {pFormat, MaxFrameCount}, packed */
    u64 a[5]={f->xapo,1,f->lockp,1,f->lockp+16};
    hr=(u32)gcall(g,vslot(g,f->xapo,8),5,a);
    if(hr){ alog("  XAPO %llx LockForProcess failed %08x",ULL(f->obj),hr); return XA2_E_INVALID; }
    f->locked=1; f->stat=xapo_stat(g,f->vtbl); return 0;
}
static u32 soft_set_chain(KCtx *g,SoftVoice *sv,u64 chain){
    u32 n=chain?rd32(g,chain):0; u64 desc=n?rd64(g,chain+4):0;
    if(n>64||(n&&!desc)) return XA2_E_INVALID;
    SoftFx *nf=calloc(n?n:1,sizeof *nf); KmxFxDesc d[64]; if(!nf) kh_fatal(g,"audio: effect allocation failed");
    u32 ch=kmx_voice_in_channels(sv->mv),rate=kmx_voice_proc_rate(sv->mv),frames=kmx_voice_proc_frames(sv->mv),hr=0,built=0;
    for(u32 i=0;i<n&&!hr;i++){
        u64 dd=desc+16ull*i,obj=rd64(g,dd); u32 initial=rd32(g,dd+8),oc=rd32(g,dd+12); SoftFx *f=&nf[i];
        if(!obj||!oc||oc>64){ hr=XA2_E_INVALID; break; }
        built=i+1; f->obj=obj; f->in_ch=ch; f->out_ch=oc; f->vtbl=rd64(g,obj);
        if(f->vtbl==kh_com_vtbl(C_xapo)){
            f->kind=rd32(g,obj+32)==3?KMX_FX_METER:KMX_FX_PASS;
            if(oc!=ch) kh_fatal(g,"audio: system %s asked to convert %u -> %u channels: not implemented",f->kind==KMX_FX_METER?"volume meter":"reverb",ch,oc);
            wr64(g,obj+16,rd64(g,obj+16)+1);
        } else { f->kind=KMX_FX_HOST; hr=soft_xapo_open(g,f,rate,frames); }
        d[i].kind=f->kind; d[i].user=f; d[i].out_ch=oc; d[i].enabled=initial!=0; d[i].inplace=f->kind==KMX_FX_HOST&&(f->regflags&0x30)&&oc==ch;   /* XAPO_FLAG_INPLACE_SUPPORTED | _REQUIRED */
        ch=oc;
    }
    if(!hr){ int rc=kmx_voice_set_chain(sv->mv,d,n);
        if(rc==KMX_E_INVALID) hr=XA2_E_INVALID;
        else if(rc!=KMX_OK) kh_fatal(g,"audio: SetEffectChain cannot be applied (%s)",rc==KMX_E_TIMEOUT?"the mixer stayed inside this voice's callback for 10 s":"called from inside the voice's own callback"); }
    if(hr){ soft_fx_teardown(g,nf,built); return hr; }
    SoftFx *old=sv->fx; u32 nold=sv->nfx; sv->fx=nf; sv->nfx=n; soft_fx_teardown(g,old,nold); return 0;
}
static u32 soft_set_outputs(KCtx *g,SoftVoice *sv,u64 list){
    KmxVoice *dests[64]; u32 fl[64],n=0; int def=!list;
    if(list){ n=rd32(g,list); u64 p=n?rd64(g,list+4):0; if(n>64||(n&&!p)) return XA2_E_INVALID;
        for(u32 i=0;i<n;i++){ fl[i]=rd32(g,p+12ull*i); u64 o=rd64(g,p+12ull*i+4);                  /* XAUDIO2_SEND_DESCRIPTOR {Flags, pOutputVoice}, packed */
            if(!o||!GA_OK(o)||(rd64(g,o)!=kh_com_vtbl(C_sub)&&rd64(g,o)!=kh_com_vtbl(C_mast))) return XA2_E_INVALID;
            Voice *st=voice_state(g,o); if(!st||!st->soft) return XA2_E_INVALID;
            if(fl[i]&0x80) kh_fatal(g,"audio: XAUDIO2_SEND_USEFILTER (output filter) is not implemented");
            if(fl[i]) return XA2_E_INVALID;
            dests[i]=st->soft->mv; } }
    int rc=kmx_voice_set_outputs(sv->mv,dests,fl,n,def);
    if(rc==KMX_E_INVALID) return XA2_E_INVALID;
    if(rc!=KMX_OK) kh_fatal(g,"audio: SetOutputVoices cannot be applied (%s)",rc==KMX_E_UNSUPPORTED?"it changes the processing rate of a voice whose XAPOs are locked, or was called from the voice's own callback":"the mixer stayed inside this voice's callback for 10 s");
    return 0;
}
/* WAVEFORMATEX / WAVEFORMATEXTENSIBLE / ADPCMWAVEFORMAT -> KmxFormat.  0 ok, -1 malformed, 1 a tag this backend cannot decode */
static int soft_format(KCtx *g,u64 f,KmxFormat *k,u32 *tag_out){
    memset(k,0,sizeof *k); u32 tag=rd16(g,f),cb=rd16(g,f+16);
    if(tag==0xfffe){ if(cb<22) return -1; tag=rd32(g,f+24); if(rd32(g,f+28)!=0x00100000u||rd64(g,f+32)!=0x719b3800aa000080ull) tag=0xffff; }
    *tag_out=tag; k->channels=rd16(g,f+2); k->rate=rd32(g,f+4); k->block_align=rd16(g,f+12); k->bits=rd16(g,f+14);
    if(tag==1) k->tag=KMX_TAG_PCM; else if(tag==3) k->tag=KMX_TAG_FLOAT;
    else if(tag==2){ k->tag=KMX_TAG_ADPCM; if(cb<4) return -1; k->samples_per_block=rd16(g,f+18); k->ncoef=rd16(g,f+20); k->bits=4;
        if(k->ncoef>32||cb<4+4*k->ncoef) return -1;
        for(u32 i=0;i<k->ncoef;i++){ k->coef[i][0]=(s16)rd16(g,f+22+4*i); k->coef[i][1]=(s16)rd16(g,f+24+4*i); } }
    else return 1;
    return 0;
}
static void soft_link(SoftEngine *e,SoftVoice *sv){ pthread_mutex_lock(&soft_m); sv->eng=e; sv->next=e->voices; sv->prevp=&e->voices; if(sv->next) sv->next->prevp=&sv->next; e->voices=sv; pthread_mutex_unlock(&soft_m); }
static void soft_unlink(SoftVoice *sv){ pthread_mutex_lock(&soft_m); if(sv->prevp){ *sv->prevp=sv->next; if(sv->next) sv->next->prevp=sv->prevp; sv->prevp=NULL; sv->next=NULL; } pthread_mutex_unlock(&soft_m); }
static void voice_state_free(KCtx *g,u64 o,Voice *v){
    clear_effects(v); while(v->matrices){ Matrix *m=v->matrices; v->matrices=m->next; free(m->values); free(m); }
    free(v); wr64(g,o+48,0);
}
static void soft_destroy_voice(KCtx *g,Voice *st){
    SoftVoice *sv=st->soft; st->soft=NULL;
    if(kmx_voice_kind(sv->mv)==KMX_SOURCE){ u64 played=0,sc=0,sf=0,sl=0; u32 q=0; kmx_source_state(sv->mv,NULL,&q,&played); kmx_source_starvations(sv->mv,&sc,&sf,&sl);
        alog("  voice=%llx summary: buffers=%llu bytes=%llu queued-at-destroy=%u samples-played-in-last-stream=%llu volume %g..%g ratio %g..%g starvations=%llu (passes %llu..%llu)",ULL(sv->obj),ULL(sv->buffers),ULL(sv->bytes),q,ULL(played),
             (double)(sv->vol_set?sv->vol_min:1.0f),(double)(sv->vol_set?sv->vol_max:1.0f),(double)(sv->ratio_set?sv->ratio_min:1.0f),(double)(sv->ratio_set?sv->ratio_max:1.0f),ULL(sc),ULL(sf),ULL(sl)); }
    if(kmx_voice_quiesce(sv->mv)==KMX_E_TIMEOUT) kh_fatal(g,"audio: DestroyVoice waited 10 s for the mixer to leave the voice's callback (a guest lock is held across DestroyVoice and taken in the callback)");
    SoftFx *fx=sv->fx; u32 n=sv->nfx; sv->fx=NULL; sv->nfx=0;
    soft_fx_teardown(g,fx,n); soft_unlink(sv);
    kmx_voice_destroy(sv->mv);                                                     /* frees sv through soft_voice_free, now or at the end of the running pass */
}
/* creation of the mixer voice behind a guest voice object; returns an HRESULT */
static u32 soft_create_voice(KCtx *g,SoftEngine *e,int s,u64 vobj,Voice *st,u64 fmt,u64 cb,u64 sends,u64 chain){
    SoftVoice *sv=calloc(1,sizeof *sv); if(!sv) kh_fatal(g,"audio: voice allocation failed");
    sv->obj=vobj; sv->cb=cb; sv->fmt=-1; int rc;
    if(s==5){
        KmxFormat k; u32 tag=0; int fr=soft_format(g,fmt,&k,&tag);
        if(fr>0) kh_fatal(g,"audio: CreateSourceVoice with format tag 0x%x (%s, %u ch, %u Hz): this backend decodes PCM, IEEE float and MS-ADPCM only",tag,fmt_tag_name(tag),k.channels,k.rate);
        u32 mr=(u32)ARG(g,4); float maxratio; memcpy(&maxratio,&mr,4);
        rc=fr<0?KMX_E_INVALID:kmx_voice_create_source(e->mx,&k,st->flags|SOFT_HIDDEN,maxratio,sv,cb!=0,&sv->mv);
        if(rc==KMX_E_UNSUPPORTED) kh_fatal(g,"audio: CreateSourceVoice: %s with %u channels is not implemented",fmt_tag_name(tag),k.channels);
        if(rc==KMX_OK) sv->fmt=fmt_stat(tag,k.channels,k.rate,rd16(g,fmt+14),k.block_align);
    } else if(s==6) rc=kmx_voice_create_submix(e->mx,rd32(g,vobj+32),rd32(g,vobj+36),st->flags|SOFT_HIDDEN,(u32)ARG(g,5),sv,&sv->mv);
    else { rc=kmx_voice_create_master(e->mx,rd32(g,vobj+32),rd32(g,vobj+36),st->flags|SOFT_HIDDEN,sv,&sv->mv);
        if(rc==KMX_E_UNSUPPORTED) kh_fatal(g,"audio: CreateMasteringVoice at %u Hz: only the 48000 Hz device rate is implemented",rd32(g,vobj+36)); }
    if(rc!=KMX_OK){ free(sv); return XA2_E_INVALID; }
    st->soft=sv; soft_link(e,sv);
    if(soft_test_mid_create) soft_test_mid_create(g,s);                            /* selftest: a processing pass right here */
    u32 hr=s==7?0:soft_set_outputs(g,sv,sends);
    if(!hr) hr=soft_set_chain(g,sv,chain);
    if(hr){ soft_destroy_voice(g,st); return hr; }
    kmx_voice_publish(sv->mv);                     /* complete: from the next pass on (deferred mode: the first pass after this instant) */
    st_created[s-5]++; return 0;
}
static SoftEngine *soft_engine_of(KCtx *g,u64 o){ return (SoftEngine*)(uintptr_t)rd64(g,o+32); }
/* where a pass goes (soft_quantum): the WAV path only, or the ring of the pull device (pushed / discarded because no device is started /
 * dropped because the device stopped consuming) */
enum { SINK_WAV=0, SINK_RING=1, SINK_RING_STOPPED=2, SINK_RING_OVERRUN=3 };
static void soft_quantum(KCtx *g,u64 instant,int sink);
static u64 mono_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+(u64)ts.tv_nsec; }
static void sleep_until_ns(u64 t){ struct timespec d={(time_t)(t/1000000000ull),(long)(t%1000000000ull)}; kh_sleep_until(&d); }   /* absolute CLOCK_MONOTONIC (kh_compat.h) */
/* The device clock: one pass per 10 ms of CLOCK_MONOTONIC.  A real device is never late; this thread can be (no real-time priority).
 * When it is behind it catches up, but never back to back: at least 5 ms between pass starts (twice real time at most), so the
 * engine's own threads can refill the queues between passes as they would against a steady device.  More than a second behind =
 * the device played silence. */
#define CATCHUP_SPACING_NS 5000000ull
/* ---- KHNMC_AUDIO=pull: the mixer follows the device (the policy is written down in khnmc_audio_out.h) ---- */
#define PULL_OVERRUN_GUARD_NS 2000000000ull
static int pull_stalled;                           /* the device stopped consuming without kh_audio_out_stop: passes are dropped until it does */
static _Atomic u64 st_pull_waits, st_pull_wait_ns;
/* before a pass: SINK_RING when there is room below the target fill (waiting for the device to make it), else what to do with the pass */
static int pull_sink(void){
    if(!kh_audio_ring_started()) return SINK_RING_STOPPED;
    KhAudioOutFormat f; kh_audio_out_format(&f);
    if(pull_stalled){ if(kh_audio_ring_fill()+f.quantum_frames<=f.target_frames) pull_stalled=0; else return SINK_RING_OVERRUN; }
    u64 t0=0;
    for(;;){
        if(atomic_load(&mix_quit)||atomic_load(&ring_closed)) return SINK_RING_STOPPED;
        if(!kh_audio_ring_started()) return SINK_RING_STOPPED;
        u32 fill=kh_audio_ring_fill();
        if(fill+f.quantum_frames<=f.target_frames){ if(t0){ st_pull_wait_ns+=mono_ns()-t0; } return SINK_RING; }
        u64 now=mono_ns(); if(!t0){ t0=now; st_pull_waits++; }
        if(now-t0>=PULL_OVERRUN_GUARD_NS){ pull_stalled=1; st_pull_wait_ns+=now-t0; return SINK_RING_OVERRUN; }
        u64 ns=(u64)(fill+f.quantum_frames-f.target_frames)*1000000000ull/f.rate; if(ns<500000ull) ns=500000ull; if(ns>5000000ull) ns=5000000ull;
        sleep_until_ns(now+ns);                    /* the device time the excess stands for: the device thread never signals */
    }
}
/* KHNMC_CLOCK=frames (vclock.c): the device clock is the virtual display clock.  Pass n is due at t0 + n * 10 ms of VIRTUAL time; the
 * clock stops at that instant until the pass is done (this thread is a clock client), so the device is never late and never ahead:
 * exactly 800 sample frames per virtual vblank, WAV sample s = virtual time t0 + s/48000 s, on any host load.  No catch-up, no resync.
 * TASK28: the pass grid is anchored to the first virtual vblank (kh_vclock_origin): pass instants are origin + k * 10 ms whatever
 * vblank the engine was created in, so their phase against the vblanks (every third vblank is a pass instant) is the same in every
 * run; t0 = the first such instant at or after the engine's creation.  The pass applies the deferred calls stamped before its
 * instant (audio_mix.c) first.  In pull mode the pass waits for room in the ring BEFORE it runs, while the clock is held at its
 * instant: the device paces the virtual clock and with it the game. */
static _Atomic u64 mix_v_t0; static _Atomic int mix_virtual; static int mix_client=-1;
static int mix_anchor_waited, mix_unanchored; static u64 mix_anchor_wait_ns;      /* review of task 28: engine created before the first vblank */
u64 kh_audio_vclock_t0(void){ return atomic_load(&mix_virtual)?atomic_load(&mix_v_t0):0; }       /* virtual time of WAV sample 0 (0 = host clock / not started) */
static void mix_thread_virtual(KCtx *g){
    /* review of task 28: an engine created before the first WaitForVBlank (8 of 100 one-CPU starts) used to take t0 = the host instant
     * of creation, a host-phased grid.  It now waits (not idle: the clock cannot leave its origin before this thread registered) for the
     * first vblank and starts at its origin.  If none comes within the guard the grid follows the host: named here and in the report. */
    if(!kh_vclock_stepped()){ u64 h0=kh_host_ns();
        while(!kh_vclock_stepped()&&!atomic_load(&mix_quit)&&kh_host_ns()-h0<KH_VCLOCK_ANCHOR_GUARD_NS) kh_vclock_wait_stepped(10000000ull);
        mix_anchor_waited=1; mix_anchor_wait_ns=kh_host_ns()-h0; mix_unanchored=!kh_vclock_stepped();
        if(atomic_load(&mix_quit)&&!kh_vclock_stepped()) return; }                  /* a test stopped the engine first (mix_thread's epilogue follows) */
    u64 now=kh_vclock_now_ns(), org=kh_vclock_origin(), t0=now;
    if(org&&now>=org) t0=org+(now-org+9999999ull)/10000000ull*10000000ull;
    atomic_store(&mix_v_t0,t0); atomic_store(&mix_virtual,1);
    mix_client=kh_vclock_client_add("xa2-mix",t0);
    fprintf(stderr,"kh: audio: clock=frames: the XAudio2 device clock is the virtual display clock; pass 0 (WAV sample 0) is at virtual time %llu ns (%s%s)\n",ULL(t0),
            org?"on the 10 ms grid of the first virtual vblank":"the clock still follows the host: no WaitForVBlank within the guard, the grid is not anchored",
            mix_anchor_waited&&org?"; the engine was created before it and waited for it":"");
    for(u64 n=0;;n++){
        u64 due=t0+n*10000000ull;
        if(n) kh_vclock_client_due(mix_client,due);
        kh_vclock_wait_until(due);
        if(atomic_load(&mix_quit)) break;
        int sink=out_pull?pull_sink():SINK_WAV;
        u64 h=mono_ns(); soft_quantum(g,apply_mode?UINT64_MAX:due,sink); u64 dur=mono_ns()-h; st_pass_ns+=dur; if(dur>st_max_pass_ns) st_max_pass_ns=dur;
    }
    kh_vclock_client_drop(mix_client); mix_client=-1; atomic_store(&mix_virtual,0);
}
/* KHNMC_AUDIO=pull with the host clock: a pass whenever the device has made room; without a started device the WAV path's host clock
 * paces the (discarded) passes */
static void mix_thread_pull(KCtx *g){
    u64 t0=mono_ns(),n=0;
    for(;;){
        if(atomic_load(&mix_quit)) break;
        int sink=pull_sink();
        if(sink==SINK_RING){ u64 h=mono_ns(); soft_quantum(g,UINT64_MAX,SINK_RING); u64 dur=mono_ns()-h; st_pass_ns+=dur; if(dur>st_max_pass_ns) st_max_pass_ns=dur; t0=mono_ns(); n=0; continue; }
        u64 due=t0+(++n)*10000000ull,now=mono_ns();
        if(now<due){ if(due-now>10000000ull) due=now+10000000ull; sleep_until_ns(due); }
        else if(now-due>1000000000ull){ t0=now; n=0; }
        if(atomic_load(&mix_quit)) break;
        soft_quantum(g,UINT64_MAX,sink);
    }
}
static void mix_thread(KCtx *g){
    if(kh_vclock_on) mix_thread_virtual(g);
    else if(out_pull) mix_thread_pull(g);
    else {
        u64 t0=mono_ns(),n=0,last=0;
        for(;;){
            if(atomic_load(&mix_quit)) break;
            u64 due=t0+n*10000000ull,now=mono_ns();
            if(now<due) sleep_until_ns(due);
            else if(now-due>1000000000ull){
                u64 skip=(now-due)/10000000ull; st_resync+=skip; n+=skip;
                pthread_mutex_lock(&wav_m); for(u64 i=0;i<skip&&wav;i++) if(kmx_wav_write(wav,NULL,480)) kh_fatal(g,"audio: WAV write failed: %s",strerror(errno)); pthread_mutex_unlock(&wav_m);
                continue;
            } else {
                if(now-due>10000000ull) st_late++;
                if(now-due>st_max_late_ns) st_max_late_ns=now-due;
                if(last&&now<last+CATCHUP_SPACING_NS){ st_spaced++; sleep_until_ns(last+CATCHUP_SPACING_NS); }
            }
            if(atomic_load(&mix_quit)) break;
            last=mono_ns(); soft_quantum(g,UINT64_MAX,SINK_WAV); u64 dur=mono_ns()-last; st_pass_ns+=dur; if(dur>st_max_pass_ns) st_max_pass_ns=dur;
            n++;
        }
    }
    pthread_mutex_lock(&soft_m); clock_started=0; atomic_store(&mix_quit,0); pthread_cond_broadcast(&soft_cv); pthread_mutex_unlock(&soft_m); KH_RET(g);
}
static const char *peak_name(void *vu){ static char b[96]; SoftVoice *sv=vu; if(!sv){ b[0]=0; return b; }
    if(sv->fmt>=0) snprintf(b,sizeof b,"voice=%llx %s %u ch %u Hz",ULL(sv->obj),fmt_tag_name(fmts[sv->fmt].tag),fmts[sv->fmt].ch,fmts[sv->fmt].rate); else snprintf(b,sizeof b,"voice=%llx",ULL(sv->obj)); return b; }
static void soft_quantum(KCtx *g,u64 instant,int sink){
    float acc[960]; int have=0; SoftEngine *list[16]; int n=0;
    pthread_mutex_lock(&soft_m);
    for(SoftEngine *e=engines;e&&n<16;e=e->next) if(!e->manual){ e->inuse++; list[n++]=e; }
    pthread_mutex_unlock(&soft_m);
    for(int i=0;i<n;i++){ const float *o=NULL; u32 oc=0;
        kmx_engine_pass_at(list[i]->mx,g,instant,&o,&oc);
        if(o){ if(oc!=2) kh_fatal(g,"audio: the mastering voice produces %u channels; only a stereo device is implemented",oc);
            if(!have){ memcpy(acc,o,sizeof acc); have=1; } else for(int k=0;k<960;k++) acc[k]+=o[k]; }
        KmxStats ks; kmx_engine_stats(list[i]->mx,&ks);
        if(ks.missing_default_matrix) kh_fatal(g,"audio: a voice sends to a destination with another channel count without SetOutputMatrix; XAudio2's default down-mix levels for that shape are not implemented");
        if(ks.deferred_failed) kh_fatal(g,"audio-deferred-call-refused: %llu XAudio2 call(s) stamped earlier no longer fitted their voice when their pass came (a queue of 64 buffers, or SetOutputVoices changed the voice in between; calls superseded by SetEffectChain are dropped and counted instead)",ULL(ks.deferred_failed));
    }
    if(peak_f&&have){ float pk=0.0f; for(int k=0;k<960;k++){ float a=fabsf(acc[k]); if(a>pk) pk=a; }
        if(pk>peak_thr){ peak_passes_over++; if(peak_n<400){ peak_n++; fprintf(peak_f,"pass %llu (%.2f s): device output peak %.4f (%+.2f dBFS)\n",ULL(st_quanta),(double)st_quanta/100.0,(double)pk,20.0*log10((double)pk));
            for(int i=0;i<n;i++) kmx_engine_peak_dump(list[i]->mx,peak_f,peak_name); fflush(peak_f); } } }
    pthread_mutex_lock(&soft_m); for(int i=0;i<n;i++) list[i]->inuse--; pthread_cond_broadcast(&soft_cv); pthread_mutex_unlock(&soft_m);
    pthread_mutex_lock(&wav_m);
    if(!atomic_load(&ring_closed)){
        if(have&&wav_gain!=1.0f) for(int k=0;k<960;k++) acc[k]*=wav_gain;        /* KHNMC_AUDIO_WAV_GAIN_DB: head-room for the file; the mix itself is untouched */
        int to_file=sink==SINK_WAV||sink==SINK_RING;                               /* pull-test: <file>.mix.wav holds exactly what entered the ring */
        if(wav&&to_file){ if(kmx_wav_write(wav,have?acc:NULL,480)) kh_fatal(g,"audio: WAV write failed: %s",strerror(errno));
            if(++wav_since_sync>=100){ wav_since_sync=0; if(kmx_wav_sync(wav)) kh_fatal(g,"audio: WAV header update failed: %s",strerror(errno)); } }
        if(sink==SINK_RING){ if(kh_audio_ring_push(have?acc:NULL,480)) kh_audio_ring_note(480,0); }
        else if(sink==SINK_RING_STOPPED) kh_audio_ring_note(0,1);
        else if(sink==SINK_RING_OVERRUN) kh_audio_ring_note(480,0);
    }
    pthread_mutex_unlock(&wav_m);
    st_quanta++;
}
static void soft_engine_new(KCtx *g,u64 obj){
    SoftEngine *e=calloc(1,sizeof *e); if(!e) kh_fatal(g,"audio: engine allocation failed");
    e->obj=obj; e->manual=soft_manual; e->mx=kmx_engine_create(&soft_host,e,48000); if(!e->mx) kh_fatal(g,"audio: mixer engine creation failed");
    if(kh_vclock_on&&!apply_mode) kmx_engine_set_deferred(e->mx,1);              /* frames clock: calls apply at the first pass after their virtual instant */
    if(peak_f) kmx_engine_set_peaks(e->mx,1);
    kmx_engine_start(e->mx);                                                        /* XAudio2 engines are created started */
    wr64(g,obj+32,(uintptr_t)e);
    pthread_mutex_lock(&soft_m); e->next=engines; engines=e; int start=!e->manual&&!clock_started; if(start) clock_started=1; pthread_mutex_unlock(&soft_m);
    if(start){
        pthread_mutex_lock(&wav_m);
        if(wav_path[0]&&!wav){ wav=kmx_wav_open(wav_path,48000,2); if(!wav) kh_fatal(g,"audio: cannot create the WAV file %s: %s",wav_path,strerror(errno)); }
        pthread_mutex_unlock(&wav_m);
        static int simdev_started; if(out_pulltest&&!simdev_started){ simdev_started=1; if(kh_audio_simdev_start(pulltest_path,0x2545f491u)) kh_fatal(g,"audio: cannot start the simulated device writing %s: %s",pulltest_path,strerror(errno)); }
        if(!kh_spawn_hostfn_thread(g,"xa2-mix",mix_thread,0)) kh_fatal(g,"audio: cannot start the mixer thread");
        kh_warn("audio: software XAudio2 started: 48000 Hz stereo, 10 ms passes, device clock = %s%s, output = %s%s%s; XAudio2 calls apply %s",
                out_pull?"the pull device's consumption":kh_vclock_on?"virtual display clock (KHNMC_CLOCK=frames)":"host clock",out_pull&&kh_vclock_on?" (passes on the virtual clock, paced by the device)":"",
                out_pull?(out_pulltest?"pull ring + simulated device -> ":"pull ring (kh_audio_out_pull)"):"WAV -> ",out_pulltest?pulltest_path:out_pull?"":wav_path,out_pulltest?" (mixer output also -> <file>.mix.wav)":"",
                kh_vclock_on&&!apply_mode?"at the first pass after their virtual instant (stamped)":"when they are made (immediate)");
    }
    alog("IXAudio2 created engine=%llx mode=%s",ULL(obj),e->manual?"soft-manual":"soft");
}
static void stats_add(KmxStats *a,const KmxStats *b){
    const u64 *s=(const u64*)b; u64 *d=(u64*)a; size_t n=offsetof(KmxStats,live_source)/sizeof(u64);
    for(size_t i=0;i<n;i++) d[i]+=s[i];
    if(b->peak_live_source>a->peak_live_source) a->peak_live_source=b->peak_live_source;
    if(b->peak_active_source>a->peak_active_source) a->peak_active_source=b->peak_active_source;
    if(b->peak_queued>a->peak_queued) a->peak_queued=b->peak_queued;
}
static void soft_engine_release(KCtx *g,u64 obj){                                   /* last Release of an IXAudio2 */
    SoftEngine *e=soft_engine_of(g,obj); if(!e) return;
    pthread_mutex_lock(&soft_m);
    for(SoftEngine **pp=&engines;*pp;pp=&(*pp)->next) if(*pp==e){ *pp=e->next; break; }
    unsigned stuck=0;                                                                /* 10 slices (kh_compat.h): a stopped process loses one slice, not the budget */
    while(e->inuse) if(kh_cond_wait_stuck_rt(&soft_cv,&soft_m,&stuck,10)==ETIMEDOUT){ pthread_mutex_unlock(&soft_m); kh_fatal(g,"audio: IXAudio2::Release waited 10 slices (about 10 s of this process running) for the running processing pass (called from a callback, or a guest lock is held)"); }
    pthread_mutex_unlock(&soft_m);
    for(;;){ pthread_mutex_lock(&soft_m); SoftVoice *sv=e->voices; pthread_mutex_unlock(&soft_m); if(!sv) break;
        Voice *st=voice_state(g,sv->obj); if(!st||st->soft!=sv) kh_fatal(g,"audio: voice list corrupted at engine release");
        u64 vo=sv->obj; alog("IXAudio2::Release destroys leftover voice=%llx",ULL(vo)); soft_destroy_voice(g,st); voice_state_free(g,vo,st); }
    KmxStats ks; kmx_engine_stats(e->mx,&ks); pthread_mutex_lock(&soft_m); stats_add(&st_dead,&ks); pthread_mutex_unlock(&soft_m);
    kmx_engine_destroy(e->mx); wr64(g,obj+32,0); free(e);
}
static int h_xa2_soft(KCtx *g,u64 o,int s,SoftEngine *e){
    switch(s){
    case 3:{ u64 cb=A1(g); if(!cb){ SET_RAX(g,XA2_E_INVALID); return 1; }
        pthread_mutex_lock(&soft_m); if(e->ncb>=8){ pthread_mutex_unlock(&soft_m); kh_fatal(g,"audio: more than 8 registered engine callbacks"); } e->cbs[e->ncb++]=cb; pthread_mutex_unlock(&soft_m);
        kmx_engine_set_callbacks(e->mx,1); break; }
    case 4:{ u64 cb=A1(g); int left;
        pthread_mutex_lock(&soft_m); for(int i=0;i<e->ncb;i++) if(e->cbs[i]==cb){ memmove(&e->cbs[i],&e->cbs[i+1],(size_t)(e->ncb-i-1)*8); e->ncb--; break; } left=e->ncb; pthread_mutex_unlock(&soft_m);
        kmx_engine_set_callbacks(e->mx,0); if(left) kmx_engine_set_callbacks(e->mx,1); break; }           /* returns after a running callback round */
    case 8: kmx_engine_start(e->mx); break;
    case 9: kmx_engine_stop(e->mx); break;
    case 10: case 12: break;                                                        /* CommitChanges: nothing can be pending (deferred calls are fatal); SetDebugConfiguration */
    case 11:{ KmxStats ks; kmx_engine_stats(e->mx,&ks); u64 p=A1(g); memset(hp(g,p),0,64);     /* XAUDIO2_PERFORMANCE_DATA: cycle counters stay 0 */
        wr32(g,p+28,480); wr32(g,p+32,(u32)ks.starvations); wr32(g,p+36,ks.active_source); wr32(g,p+40,ks.live_source); wr32(g,p+44,ks.live_submix); wr32(g,p+48,ks.active_source); wr32(g,p+52,ks.active_source+ks.live_submix); break; }
    default: return 0;
    }
    (void)o; SET_RAX(g,0); return 1;
}
static void log_chain(KCtx *g,const char *what,u64 v,u64 chain){
    u32 n=chain?rd32(g,chain):0; u64 desc=n?rd64(g,chain+4):0;
    for(u32 i=0;i<n&&i<64&&desc;i++){ u64 d=desc+16ull*i,ob=rd64(g,d),vt=ob?rd64(g,ob):0; char nm[48];
        if(vt==kh_com_vtbl(C_xapo)) snprintf(nm,sizeof nm,"%s",rd32(g,ob+32)==3?"system volume meter (ord3)":"system reverb (ord2)"); else rtti_name(g,vt,nm,sizeof nm);
        alog("  %s voice=%llx effect[%u] %s obj=%llx initial=%u outch=%u",what,ULL(v),i,nm,ULL(ob),rd32(g,d+8),rd32(g,d+12)); }
}
static void log_sends(KCtx *g,u64 v,u64 sends){
    u32 n=sends?rd32(g,sends):0; u64 p=n?rd64(g,sends+4):0;
    if(!sends){ alog("  sends voice=%llx default (mastering voice)",ULL(v)); return; }
    for(u32 i=0;i<n&&i<64&&p;i++) alog("  sends voice=%llx [%u] flags=%x out=%llx",ULL(v),i,rd32(g,p+12ull*i),ULL(rd64(g,p+12ull*i+4)));
}
static int h_xa2(KCtx *g,u64 o,int s,const char *c){
    SoftEngine *se=soft_engine_of(g,o);
    if(s>=3&&s!=11&&(s<5||s>7)) alog("IXAudio2::%s engine=%llx a1=%llx site=%llx",s<13?engine_methods[s]:"?",ULL(o),ULL(A1(g)),RVA(guest_return_addr(g)));
    if(se&&s==2&&COM_REFS(g,o)==1) soft_engine_release(g,o);
    if(se&&h_xa2_soft(g,o,s,se)) return 1;
    switch(s){
    case 0: case 1: case 2: return 0;
    case 3: case 4: case 8: case 9: case 10: case 12: break;
    case 5: case 6: case 7:{
        if(!A1(g)){ SET_RAX(g,0x80004003u); return 1; }
        if(s==5&&!A2(g)){ SET_RAX(g,0x80004003u); return 1; }
        u64 v=kh_com_obj(s==5?C_src:s==6?C_sub:C_mast,32);
        u32 channels=s==5?rd16(g,A2(g)+2):(u32)A2(g), rate=s==5?rd32(g,A2(g)+4):(u32)A3(g);
        wr32(g,v+32,channels?channels:2); wr32(g,v+36,rate?rate:48000); wr32(g,v+40,0x3f800000); wr32(g,v+44,0x3f800000);
        if(rd32(g,v+32)>64) kh_fatal(g,"null audio: invalid channel count");
        Voice *state=calloc(1,sizeof(*state)); if(!state) kh_fatal(g,"null audio: voice allocation failed");
        state->flags=s==5?(u32)A3(g):(u32)A4(g);
        for(u32 i=0;i<rd32(g,v+32);i++) state->channels[i]=0x3f800000;
        wr64(g,v+48,(uintptr_t)state);
        u64 chain=ARG(g,s==7?6:7);
        if(s==5){ u64 f=A2(g); alog("CreateSourceVoice voice=%llx tag=%x ch=%u rate=%u align=%u bits=%u cbSize=%u flags=%x maxratio=%g callback=%llx site=%llx",ULL(v),rd16(g,f),rd16(g,f+2),rd32(g,f+4),rd16(g,f+12),rd16(g,f+14),rd16(g,f+16),(u32)A3(g),(double)*(float*)hp(g,KH_RSP(g)+8+32),ULL(ARG(g,5)),RVA(guest_return_addr(g)));
            if(rd16(g,f)==2) alog("  adpcm samplesPerBlock=%u numCoef=%u",rd16(g,f+18),rd16(g,f+20));
            if(rd16(g,f)==0xfffe) alog("  extensible validBits=%u mask=%x subformat=%08x",rd16(g,f+18),rd32(g,f+20),rd32(g,f+24));
            log_sends(g,v,ARG(g,6)); }
        else if(s==6){ alog("CreateSubmixVoice voice=%llx ch=%u rate=%u flags=%x stage=%u site=%llx",ULL(v),(u32)A2(g),(u32)A3(g),(u32)ARG(g,4),(u32)ARG(g,5),RVA(guest_return_addr(g))); log_sends(g,v,ARG(g,6)); }
        else alog("CreateMasteringVoice voice=%llx ch=%u rate=%u flags=%x category=%u site=%llx",ULL(v),(u32)A2(g),(u32)A3(g),(u32)ARG(g,4),(u32)ARG(g,7),RVA(guest_return_addr(g)));
        log_chain(g,"create",v,chain);
        if(se){
            u32 hr=soft_create_voice(g,se,s,v,state,s==5?A2(g):0,s==5?ARG(g,5):0,s==7?0:ARG(g,6),chain);
            if(hr){ alog("  creation failed %08x",hr); free(state); wr64(g,v+48,0); wr64(g,A1(g),0); SET_RAX(g,hr); return 1; }
        } else {
            if(!effect_chain(g,state,chain)) kh_fatal(g,"null audio: invalid creation effect chain");
            if(s==5) fmt_stat(rd16(g,A2(g)),rd16(g,A2(g)+2),rd32(g,A2(g)+4),rd16(g,A2(g)+14),rd16(g,A2(g)+12));
            st_created[s-5]++;
        }
        wr64(g,A1(g),v); break;
    }
    case 11: memset(hp(g,A1(g)),0,64); return 1; /* XAUDIO2_PERFORMANCE_DATA is 64 bytes. */
    default: missing(g,c,s<13?engine_methods[s]:"unknown slot");
    }
    SET_RAX(g,0); return 1;
}
static KmxVoice *soft_dest(KCtx *g,u64 dest){                                       /* guest voice pointer of a matrix call -> mixer voice (NULL = the only send) */
    if(!dest) return NULL;
    if(!GA_OK(dest)||(rd64(g,dest)!=kh_com_vtbl(C_sub)&&rd64(g,dest)!=kh_com_vtbl(C_mast))) return (KmxVoice*)(uintptr_t)1;
    Voice *st=voice_state(g,dest); return st&&st->soft?st->soft->mv:(KmxVoice*)(uintptr_t)1;
}
static int h_voice_soft(KCtx *g,u64 o,int s,const char *c,Voice *v){
    SoftVoice *sv=v->soft; KmxVoice *mv=sv->mv; int source=kmx_voice_kind(mv)==KMX_SOURCE; float x1=*(float*)&KH_XMM(g,1); int rc=KMX_OK;
#define NODEFER(opset,what) do{ if((u32)(opset)) missing(g,c,"deferred " what " (operation sets are not implemented)"); }while(0)
    if(s==19&&kmx_voice_kind(mv)==KMX_MASTER){ wr32(g,A1(g),3); SET_RAX(g,0); return 1; }   /* GetChannelMask of the stereo device */
    if(s>18&&!source) missing(g,c,"unknown slot");
    switch(s){
    case 0: wr32(g,A1(g),v->flags); wr32(g,A1(g)+4,0); wr32(g,A1(g)+8,rd32(g,o+32)); wr32(g,A1(g)+12,rd32(g,o+36)); break;
    case 1:{ u32 hr=soft_set_outputs(g,sv,A1(g)); if(hr){ SET_RAX(g,hr); return 1; } break; }
    case 2:{ u32 hr=soft_set_chain(g,sv,A1(g)); if(hr){ SET_RAX(g,hr); return 1; } break; }
    case 3: case 4: NODEFER(A2(g),"EnableEffect/DisableEffect"); rc=kmx_voice_fx_enable(mv,(u32)A1(g),s==3); break;
    case 5:{ int on=0; if(!A2(g)) goto invalid; rc=kmx_voice_fx_enabled(mv,(u32)A1(g),&on); if(rc==KMX_OK) wr32(g,A2(g),(u32)on); break; }
    case 6: case 7:{
        u32 i=(u32)A1(g),size=(u32)A3(g); u64 p=A2(g); SoftFx *f=kmx_voice_fx_user(mv,i);
        if(!f||!p||!size) goto invalid;
        if(s==6){ NODEFER(A4(g),"SetEffectParameters");
            if(f->kind==KMX_FX_HOST){ if(!f->params) goto invalid; st_setparams++;
                if(kmx_engine_deferred(sv->eng->mx)){ if(kmx_voice_fx_params(mv,g,i,hp(g,p),size)!=KMX_OK) goto invalid; }   /* delivered on the mixer thread before the pass after this instant */
                else { fx_lock(); kh_call(g,vslot(g,f->params,3),f->params,p,size,0); pthread_mutex_unlock_fx(); } }      /* IXAPOParameters::SetParameters, never inside Process */
            else { void *data=malloc(size); if(!data) kh_fatal(g,"audio: effect parameters allocation failed"); memcpy(data,hp(g,p),size); free(f->blob); f->blob=data; f->blob_size=size; if(f->kind==KMX_FX_PASS) st_reverb_params++; }
        } else if(f->kind==KMX_FX_HOST){ if(!f->params) goto invalid; fx_lock(); kh_call(g,vslot(g,f->params,4),f->params,p,size,0); pthread_mutex_unlock_fx(); }                 /* IXAPOParameters::GetParameters */
        else if(f->kind==KMX_FX_METER){                                             /* XAUDIO2FX_VOLUMEMETER_LEVELS {float *pPeakLevels, *pRMSLevels; UINT32 ChannelCount}, packed */
            float pk[KMX_MAXCH],rms[KMX_MAXCH]; u32 n=size==20?rd32(g,p+16):0;
            if(size!=20||!n||n>KMX_MAXCH||kmx_voice_meter(mv,i,pk,rms,n)!=KMX_OK) goto invalid;
            if(rd64(g,p)) memcpy(hp(g,rd64(g,p)),pk,4*n); if(rd64(g,p+8)) memcpy(hp(g,rd64(g,p+8)),rms,4*n);
        } else { if(!f->blob) missing(g,c,"GetEffectParameters (system reverb whose parameters were never set)"); if(size!=f->blob_size) goto invalid; memcpy(hp(g,p),f->blob,size); }
        break;
    }
    case 12: NODEFER(A2(g),"SetVolume"); rc=kmx_voice_set_volume(mv,x1);
        if(rc==KMX_OK){ wr32(g,o+40,*(u32*)&KH_XMM(g,1)); if(!sv->vol_set||x1<sv->vol_min) sv->vol_min=x1; if(!sv->vol_set||x1>sv->vol_max) sv->vol_max=x1; sv->vol_set=1; } break;
    case 13: { float f=kmx_voice_volume(mv); memcpy(hp(g,A1(g)),&f,4); break; }
    case 14: NODEFER(A3(g),"SetChannelVolumes"); if(!A2(g)) goto invalid; rc=kmx_voice_set_channel_volumes(mv,(u32)A1(g),hp(g,A2(g))); break;
    case 15: if(!A2(g)) goto invalid; rc=kmx_voice_channel_volumes(mv,(u32)A1(g),hp(g,A2(g))); break;
    case 16: case 17:{
        u32 src=(u32)A2(g),dst=(u32)A3(g); u64 data=A4(g); KmxVoice *d=soft_dest(g,A1(g));
        if(d==(KmxVoice*)(uintptr_t)1||!data||!src||!dst||src>KMX_MAXCH||dst>KMX_MAXCH) goto invalid;
        if(s==16){ NODEFER(A5(g),"SetOutputMatrix"); rc=kmx_voice_set_matrix(mv,d,src,dst,hp(g,data)); }
        else { rc=kmx_voice_matrix(mv,d,src,dst,hp(g,data)); if(rc==KMX_E_UNSUPPORTED) missing(g,c,"GetOutputMatrix (XAudio2's default down-mix levels for this shape are not implemented)"); }
        break;
    }
    case 18: soft_destroy_voice(g,v); voice_state_free(g,o,v); break;
    case 19: if((u32)A1(g)) goto invalid; NODEFER(A2(g),"Start"); rc=kmx_source_start(mv); break;
    case 20: NODEFER(A2(g),"Stop"); rc=kmx_source_stop(mv,(u32)A1(g)); break;
    case 21:{                                                                       /* XAUDIO2_BUFFER, packed: Flags, AudioBytes, pAudioData @8, PlayBegin @16, PlayLength, LoopBegin, LoopLength, LoopCount, pContext @36 */
        u64 b=A1(g); if(!b) goto invalid;
        if(A2(g)) kh_fatal(g,"audio: SubmitSourceBuffer with an XAUDIO2_BUFFER_WMA (xWMA) is not implemented");
        KmxBuffer k; k.flags=rd32(g,b); k.bytes=rd32(g,b+4); u64 data=rd64(g,b+8); k.play_begin=rd32(g,b+16); k.play_length=rd32(g,b+20); k.loop_begin=rd32(g,b+24); k.loop_length=rd32(g,b+28); k.loop_count=rd32(g,b+32); k.context=rd64(g,b+36);
        if(data&&k.bytes&&(!GA_OK(data)||!GA_OK(data+k.bytes-1))) kh_fatal(g,"audio: SubmitSourceBuffer data %llx+%u lies outside guest memory",ULL(data),k.bytes);
        k.data=data?hp(g,data):NULL;
        rc=kmx_source_submit(mv,&k);
        pthread_mutex_lock(&soft_m); static u64 nlog; int show=alog_all||nlog<200||!(nlog&1023); nlog++;
        if(rc==KMX_OK&&sv->fmt>=0){ FmtStat *fs=&fmts[sv->fmt]; fs->buffers++; fs->bytes+=k.bytes; sv->buffers++; sv->bytes+=k.bytes; } pthread_mutex_unlock(&soft_m);
        if(rc!=KMX_OK){ st_submit_rejected++; show=1; }
        if(show) alog("Submit voice=%llx flags=%x bytes=%u playbegin=%u playlen=%u loopbegin=%u looplen=%u loopcount=%u ctx=%llx%s site=%llx",ULL(o),k.flags,k.bytes,k.play_begin,k.play_length,k.loop_begin,k.loop_length,k.loop_count,ULL(k.context),rc==KMX_OK?"":" REJECTED",RVA(guest_return_addr(g)));
        break;
    }
    case 22: rc=kmx_source_flush(mv); break;
    case 23: rc=kmx_source_discontinuity(mv); break;
    case 24: NODEFER(A1(g),"ExitLoop"); rc=kmx_source_exit_loop(mv); break;
    case 25:{ u64 ctx=0,played=0; u32 q=0; kmx_source_state(mv,&ctx,&q,&played);   /* XAUDIO2_VOICE_STATE, packed: pCurrentBufferContext, BuffersQueued @8, SamplesPlayed @12 */
        if((u32)A2(g)&0x100) played=0;                                              /* XAUDIO2_VOICE_NOSAMPLESPLAYED */
        u64 p=A1(g); wr64(g,p,ctx); wr32(g,p+8,q); wr64(g,p+12,played); break; }
    case 26: NODEFER(A2(g),"SetFrequencyRatio"); rc=kmx_source_set_ratio(mv,x1);
        if(rc==KMX_OK){ wr32(g,o+44,*(u32*)&KH_XMM(g,1)); if(!sv->ratio_set||x1<sv->ratio_min) sv->ratio_min=x1; if(!sv->ratio_set||x1>sv->ratio_max) sv->ratio_max=x1; sv->ratio_set=1; } break;
    case 27: { float f=kmx_source_ratio(mv); memcpy(hp(g,A1(g)),&f,4); break; }
    case 28: rc=kmx_source_set_rate(mv,(u32)A1(g)); if(rc==KMX_OK) wr32(g,o+36,(u32)A1(g)); break;
    default: missing(g,c,s<29?voice_methods[s]:"unknown slot");                     /* 8..11: voice and output filters */
    }
#undef NODEFER
    if(rc!=KMX_OK) goto invalid;
    SET_RAX(g,0); return 1;
invalid:
    SET_RAX(g,XA2_E_INVALID); return 1;
}
static void log_voice_call(KCtx *g,u64 o,int s){
    static _Atomic u32 nvol,nrat,nmat,nget,nstate;
    if(s==19||s==20) alog("%s voice=%llx flags=%x site=%llx",s==19?"Start":"Stop",ULL(o),(u32)A1(g),RVA(guest_return_addr(g)));
    else if(s==18||s==22||s==23||s==24||s==28||s==1||(s>=8&&s<=11)) alog("%s voice=%llx a1=%llx site=%llx",voice_methods[s],ULL(o),ULL(A1(g)),RVA(guest_return_addr(g)));
    else if(s==2){ alog("SetEffectChain voice=%llx",ULL(o)); log_chain(g,"set",o,A1(g)); }
    else if(s==3||s==4) alog("%s voice=%llx index=%u",voice_methods[s],ULL(o),(u32)A1(g));
    else if(s==6) alog("SetEffectParameters voice=%llx index=%u size=%u",ULL(o),(u32)A1(g),(u32)A3(g));
    else if(s==14) alog("SetChannelVolumes voice=%llx n=%u",ULL(o),(u32)A1(g));
    else if(s==12){ if(alog_all||nvol++<32) alog("SetVolume voice=%llx v=%g",ULL(o),(double)*(float*)&KH_XMM(g,1)); }
    else if(s==26){ if(alog_all||nrat++<32) alog("SetFrequencyRatio voice=%llx r=%g",ULL(o),(double)*(float*)&KH_XMM(g,1)); }
    else if(s==16){ if(alog_all||nmat++<32){ u32 sc=(u32)A2(g),dc=(u32)A3(g); u64 m=A4(g); char buf[400]; int k=0; buf[0]=0; if(m&&sc<=8&&dc<=8) for(u32 i=0;i<sc*dc&&k<380;i++) k+=snprintf(buf+k,sizeof buf-(size_t)k," %g",(double)*(float*)hp(g,m+4*i)); alog("SetOutputMatrix voice=%llx dest=%llx src=%u dst=%u levels=%s",ULL(o),ULL(A1(g)),sc,dc,buf); } }
    else if(s==7){ if(alog_all||nget++<4) alog("GetEffectParameters voice=%llx index=%u size=%u (further calls not logged)",ULL(o),(u32)A1(g),(u32)A3(g)); }
    else if(s==25){ if(alog_all||nstate++<16) alog("GetState voice=%llx site=%llx",ULL(o),RVA(guest_return_addr(g))); }
}
static int h_voice(KCtx *g,u64 o,int s,const char *c){
    int source=kh_com_cls_of(g,o)==C_src;
    Voice *v=voice_state(g,o);
    if(!v) missing(g,c,"method on destroyed voice");
    if(alog_f||!alog_init) log_voice_call(g,o,s);
    if(v->soft) return h_voice_soft(g,o,s,c,v);
    if(s==19 && kh_com_cls_of(g,o)==C_mast){ wr32(g,A1(g),3); SET_RAX(g,0); return 1; } /* GetChannelMask */
    if(s>18&&!source) missing(g,c,"unknown slot");
    switch(s){
    case 0: wr32(g,A1(g),v->flags); wr32(g,A1(g)+4,0); wr32(g,A1(g)+8,rd32(g,o+32)); wr32(g,A1(g)+12,rd32(g,o+36)); break;
    case 2: if(!effect_chain(g,v,A1(g))) goto invalid; break;
    case 3: case 4: case 5: case 6: case 7:{
        u32 i=(u32)A1(g); if(i>=v->count) goto invalid;
        Effect *e=&v->effects[i];
        if(s==3||s==4){ if((u32)A2(g)) missing(g,c,"deferred EnableEffect/DisableEffect"); e->enabled=s==3; }
        else if(s==5){ if(!A2(g)) goto invalid; wr32(g,A2(g),e->enabled); }
        else {
            u32 size=(u32)A3(g); if(!A2(g)||!size) goto invalid;
            if(s==6){
                if((u32)A4(g)) missing(g,c,"deferred SetEffectParameters");
                void *data=malloc(size); if(!data) kh_fatal(g,"null audio: effect parameters allocation failed");
                memcpy(data,hp(g,A2(g)),size); free(e->params); e->params=data; e->size=size;
            } else if(e->object && rd64(g,e->object)==kh_com_vtbl(C_xapo) && rd32(g,e->object+32)==3){
                /* MasterBusXA2 appends ord3 and reads its packed peak/RMS structure. */
                u64 p=A2(g); if(size!=20||rd32(g,p+16)!=e->channels||!rd64(g,p)||!rd64(g,p+8)) goto invalid;
                memset(hp(g,rd64(g,p)),0,e->channels*4); memset(hp(g,rd64(g,p+8)),0,e->channels*4);
            } else {
                if(!e->params) missing(g,c,"GetEffectParameters (uninitialized effect)");
                if(size!=e->size) goto invalid; memcpy(hp(g,A2(g)),e->params,size);
            }
        }
        break;
    }
    case 14: case 15:{
        u32 n=(u32)A1(g); if(n!=rd32(g,o+32)||!A2(g)) goto invalid;
        if(s==14){ if((u32)A3(g)) missing(g,c,"deferred SetChannelVolumes"); memcpy(v->channels,hp(g,A2(g)),4*n); }
        else memcpy(hp(g,A2(g)),v->channels,4*n);
        break;
    }
    case 16: case 17:{
        u32 src=(u32)A2(g),dst=(u32)A3(g); u64 dest=A1(g),data=A4(g);
        if(src!=rd32(g,o+32)||!dst||dst>64||!data) goto invalid;
        Matrix *m=v->matrices; while(m&&m->dest!=dest) m=m->next;
        if(s==16){
            if((u32)A5(g)) missing(g,c,"deferred SetOutputMatrix");
            if(!m){ m=calloc(1,sizeof(*m)); if(!m) kh_fatal(g,"null audio: matrix allocation failed"); m->dest=dest; m->next=v->matrices; v->matrices=m; }
            void *values=malloc(4*src*dst); if(!values) kh_fatal(g,"null audio: matrix allocation failed");
            memcpy(values,hp(g,data),4*src*dst); free(m->values); m->values=values; m->src=src; m->dst=dst;
        } else {
            if(!m) missing(g,c,"GetOutputMatrix (default routing not implemented)");
            if(m->src!=src||m->dst!=dst) goto invalid; memcpy(hp(g,data),m->values,4*src*dst);
        }
        break;
    }
    case 18:
        clear_effects(v); while(v->matrices){ Matrix *m=v->matrices; v->matrices=m->next; free(m->values); free(m); }
        free(v); wr64(g,o+48,0); break;
    case 21: if(A1(g)){ st_null_buffers++; st_null_bytes+=rd32(g,A1(g)+4); } break; /* null device: the buffer is dropped, counted for the report */
    case 19: case 20: case 22: case 23: case 24: break; /* documented drained null source */
    case 12: wr32(g,o+40,*(u32*)&KH_XMM(g,1)); break;
    case 13: wr32(g,A1(g),rd32(g,o+40)); break;
    case 25: memset(hp(g,A1(g)),0,24); break; /* empty source queue */
    case 26: wr32(g,o+44,*(u32*)&KH_XMM(g,1)); break;
    case 27: wr32(g,A1(g),rd32(g,o+44)); break;
    case 28: wr32(g,o+36,(u32)A1(g)); break;
    default: missing(g,c,s<29?voice_methods[s]:"unknown slot");
    }
    SET_RAX(g,0); return 1;
invalid:
    SET_RAX(g,0x88960001u); return 1;
}
static int h_xapo(KCtx *g,u64 o,int s,const char *c){
    (void)o; if(s<3) return 0;
    static const char *methods[]={"GetRegistrationProperties","IsInputFormatSupported","IsOutputFormatSupported","Initialize","Reset","LockForProcess","UnlockForProcess","Process","CalcInputFrames","CalcOutputFrames"};
    missing(g,c,s<13?methods[s-3]:"unknown slot"); return 0;
}
static u64 new_engine(KCtx *g){
    u64 o=kh_com_obj(C_xa2,16);
    if(soft_mode) soft_engine_new(g,o); else alog("IXAudio2 created engine=%llx mode=null",ULL(o));
    return o;
}
static void xa2_create(KCtx *g){
    kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); KH_RET(g); return; }
    wr64(g,A0(g),new_engine(g)); SET_RAX(g,0); KH_RET(g);
}
void (*kh_audio_pull_ready)(void);                 /* khnmc_audio_out.h: the app shell's device start (iOS: CoreAudio) */
void kh_seam_audio_init(void){
    const char *mode=getenv("KHNMC_AUDIO");
    if(mode&&*mode&&strcmp(mode,"null")){
        if(!strcmp(mode,"pull")){ soft_mode=1; out_pull=1; }
        else if(!strncmp(mode,"pull-test:",10)&&mode[10]&&strlen(mode+10)+9<sizeof wav_path){ soft_mode=1; out_pull=1; out_pulltest=1; snprintf(pulltest_path,sizeof pulltest_path,"%s",mode+10); snprintf(wav_path,sizeof wav_path,"%s.mix.wav",mode+10); }
        else if(!strncmp(mode,"wav:",4)&&mode[4]&&strlen(mode+4)<sizeof wav_path){ snprintf(wav_path,sizeof wav_path,"%s",mode+4); soft_mode=1; }
        else { fprintf(stderr,"kh: FATAL: KHNMC_AUDIO=%s not understood (use: null | wav:<file> | pull | pull-test:<file>)\n",mode); fflush(stdout); kh_host_exit(2); }
    }
    if(out_pull){ kh_audio_ring_setup(1,KH_AUDIO_OUT_RING_FRAMES,KH_AUDIO_OUT_TARGET_FRAMES); if(!out_pulltest&&kh_audio_pull_ready) kh_audio_pull_ready(); if(kh_vclock_pace_by_device()) fprintf(stderr,"kh: audio: KHNMC_AUDIO=%s with KHNMC_CLOCK=frames: the device paces the virtual clock (KHNMC_CLOCK_PACE=free implied; set it to realtime to keep the host-time floor as well)\n",mode); }                                   /* ring 16384 frames (341 ms; room for a target of 2 x 4096-frame slices), target fill 2048 (43 ms): above one 1024-frame callback + two passes */
    { const char *pl=getenv("KHNMC_AUDIO_PEAKLOG"); if(pl&&*pl){ char path[1024]; snprintf(path,sizeof path,"%s",pl); char *c=strrchr(path,','); if(c){ *c=0; peak_thr=(float)atof(c+1); if(!(peak_thr>0.0f)) peak_thr=1.0f; }
        peak_f=fopen(path,"w"); if(!peak_f){ fprintf(stderr,"kh: FATAL: KHNMC_AUDIO_PEAKLOG: cannot create %s\n",path); fflush(stdout); kh_host_exit(2); } } }
    { const char *a=getenv("KHNMC_AUDIO_APPLY");
      if(a&&*a){ if(!strcmp(a,"stamped")) apply_mode=0; else if(!strcmp(a,"immediate")) apply_mode=1;
          else { fprintf(stderr,"kh: FATAL: KHNMC_AUDIO_APPLY=%s not understood (stamped = with KHNMC_CLOCK=frames, calls apply at the first pass after their virtual instant, the default; immediate = when they are made, the behaviour before TASK28)\n",a); fflush(stdout); kh_host_exit(2); } } }
    { const char *e=getenv("KHNMC_AUDIO_WAV_GAIN_DB"); if(e&&*e){ char *end; double db=strtod(e,&end);
        if(*end||!(db<=0.0)||db<-60.0){ fprintf(stderr,"kh: FATAL: KHNMC_AUDIO_WAV_GAIN_DB=%s not understood (a gain in dB from -60 to 0)\n",e); fflush(stdout); kh_host_exit(2); }
        wav_gain_db=db; wav_gain=(float)pow(10.0,db/20.0);
        if(out_pull&&db!=0.0){ fprintf(stderr,"kh: FATAL: KHNMC_AUDIO_WAV_GAIN_DB applies to WAV files only; with KHNMC_AUDIO=%s the ring and the file would differ\n",mode); fflush(stdout); kh_host_exit(2); } } }
    C_enum=kh_com_class("xa2:IMMDeviceEnumerator",8,COM_STRICT,h_device);
    C_device=kh_com_class("xa2:IMMDevice",7,COM_STRICT,h_device);
    C_client=kh_com_class("xa2:IAudioClient",15,COM_STRICT,h_device);
    C_xa2=kh_com_class("xa2:IXAudio2",16,COM_STRICT,h_xa2);
    C_src=kh_com_class("xa2:IXAudio2SourceVoice",32,COM_STRICT|COM_NOUNK,h_voice);
    C_sub=kh_com_class("xa2:IXAudio2SubmixVoice",24,COM_STRICT|COM_NOUNK,h_voice);
    C_mast=kh_com_class("xa2:IXAudio2MasteringVoice",24,COM_STRICT|COM_NOUNK,h_voice);
    C_xapo=kh_com_class("xa2:effect",16,COM_STRICT,h_xapo);
    kh_register_proc("xaudio2_9.dll","XAudio2Create",kh_host_fn("xaudio2_9!XAudio2Create",xa2_create));
    kh_register_proc("xaudio2_9.dll","XAudio2CreateWithVersionInfo",kh_host_fn("xaudio2_9!XAudio2CreateWithVersionInfo",xa2_create));
}
IMP(xaudio2_9,ord1){ kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); return; } wr64(g,A0(g),new_engine(g)); SET_RAX(g,0); }
IMP(xaudio2_9,ord2){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xapo,16)); SET_RAX(g,0); }
IMP(xaudio2_9,ord3){ kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); return; } u64 o=kh_com_obj(C_xapo,16); wr32(g,o+32,3); wr64(g,A0(g),o); SET_RAX(g,0); }

/* ---- tests: backend selection and manual passes ---- */
int kh_audio_test_mode(int mode){                                                   /* 0 = null, 1 = software with test-driven passes; returns the previous mode */
    int prev=soft_mode?(soft_manual?1:2):0;
    if(mode==2){ soft_mode=1; soft_manual=0; } else { soft_mode=mode!=0; soft_manual=mode!=0; }
    return prev;
}
/* one processing pass of a test engine on the calling guest thread; out = 480 stereo frames (silence without a mastering voice) */
u32 kh_audio_test_pass(KCtx *g,u64 engine,float *out,u32 *channels){
    SoftEngine *e=soft_engine_of(g,engine); if(!e||!e->manual) return 0;
    const float *o=NULL; u32 oc=0; if(kmx_engine_pass(e->mx,g,&o,&oc)!=KMX_OK) return 0;
    if(channels) *channels=oc; if(out){ if(o&&oc&&oc<=8) memcpy(out,o,sizeof(float)*480*oc); else memset(out,0,sizeof(float)*480*2); }
    KmxStats ks; kmx_engine_stats(e->mx,&ks); return ks.missing_default_matrix?0:1;
}
/* selftest of the frames clock: route the device output to `path` (NULL = restore), read the pass count, stop the virtual clock thread */
static char wav_path_saved[1024];
void kh_audio_test_wav(const char *path){ pthread_mutex_lock(&wav_m);
    if(path){ memcpy(wav_path_saved,wav_path,sizeof wav_path); snprintf(wav_path,sizeof wav_path,"%s",path); }
    else { if(wav){ KmxWavStats ws; kmx_wav_close(wav,&ws); wav=NULL; } memcpy(wav_path,wav_path_saved,sizeof wav_path); }
    pthread_mutex_unlock(&wav_m); }
u64 kh_audio_test_quanta(void){ return st_quanta; }
float kh_audio_test_wav_gain(float gain){ float prev=wav_gain; wav_gain=gain; return prev; }
int kh_audio_test_stop_clock(KCtx *g){             /* asks the virtual clock thread to end at its next pass instant; the caller moves the clock; 1 = it ended */
    atomic_store(&mix_quit,1);
    for(int i=0;i<400;i++){ pthread_mutex_lock(&soft_m); int run=clock_started; pthread_mutex_unlock(&soft_m); if(!run) return 1; kh_vclock_test_vblank(g); struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    return 0; }
/* TASK28 selftests: a function called between the creation of a mixer voice and its sends / chain (NULL = none); stop a host-clock
 * mixer thread (1 = it ended) */
void kh_audio_test_mid_create(void (*fn)(KCtx*,int)){ soft_test_mid_create=fn; }
void kh_audio_test_pull(int on){ out_pull=on; kh_audio_ring_setup(on,KH_AUDIO_OUT_RING_FRAMES,KH_AUDIO_OUT_TARGET_FRAMES); pull_stalled=0; atomic_store(&ring_closed,0); }
int kh_audio_test_stop_mixer(void){
    atomic_store(&mix_quit,1);
    for(int i=0;i<3000;i++){ pthread_mutex_lock(&soft_m); int run=clock_started; pthread_mutex_unlock(&soft_m); if(!run) return 1; struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    return 0; }
void kh_audio_test_counters(u64 *voice_cb,u64 *engine_cb){ for(int i=0;i<6;i++) voice_cb[i]=st_cb[i]; engine_cb[0]=st_ecb[0]; engine_cb[1]=st_ecb[1]; }

/* ---- end-of-run report; finalises the WAV ---- */
void kh_audio_report(FILE *f){
    KmxWavStats ws; int had=0; memset(&ws,0,sizeof ws);
    pthread_mutex_lock(&wav_m);
    atomic_store(&ring_closed,1);                                                   /* later passes go nowhere: the WAV and the ring end at the same pass */
    if(wav){ had=1; if(kmx_wav_close(wav,&ws)) fprintf(f,"kh: audio: ERROR: the WAV header of %s could not be finalised: %s\n",wav_path,strerror(errno)); wav=NULL; }
    pthread_mutex_unlock(&wav_m);
    if(out_pulltest&&kh_audio_simdev_finish(f)) fprintf(f,"kh: audio-out: ERROR: the simulated device's file %s could not be finalised: %s\n",pulltest_path,strerror(errno));
    u64 created=st_created[0]+st_created[1]+st_created[2];
    if(!created&&!had&&!st_null_buffers) return;
    KmxStats t=st_dead;
    pthread_mutex_lock(&soft_m); SoftEngine *list[16]; int n=0; for(SoftEngine *e=engines;e&&n<16;e=e->next) list[n++]=e; pthread_mutex_unlock(&soft_m);
    for(int i=0;i<n;i++){ KmxStats ks; kmx_engine_stats(list[i]->mx,&ks); stats_add(&t,&ks); }
    fprintf(f,"kh: audio: backend=%s voices created: source=%llu submix=%llu mastering=%llu\n",soft_mode?"software":"null",ULL(st_created[0]),ULL(st_created[1]),ULL(st_created[2]));
    for(int i=0;i<nfmts;i++) fprintf(f,"kh: audio: source format %s(0x%x) %u ch %u Hz %u bit align %u: voices=%llu buffers=%llu bytes=%llu\n",fmt_tag_name(fmts[i].tag),fmts[i].tag,fmts[i].ch,fmts[i].rate,fmts[i].bits,fmts[i].align,ULL(fmts[i].voices),ULL(fmts[i].buffers),ULL(fmts[i].bytes));
    if(!soft_mode){ fprintf(f,"kh: audio: null device: %llu submitted buffers (%llu bytes) dropped, no callbacks\n",ULL(st_null_buffers),ULL(st_null_bytes)); return; }
    fprintf(f,"kh: audio: passes=%llu (%.2f s) clock: passes started >10 ms late=%llu (worst %.1f ms), catch-up passes spaced=%llu, resync-quanta=%llu; pass time mean %.3f ms worst %.1f ms\n",ULL(t.passes),(double)t.passes/100.0,ULL(st_late),(double)st_max_late_ns/1e6,ULL(st_spaced),ULL(st_resync),st_quanta?(double)st_pass_ns/(double)st_quanta/1e6:0.0,(double)st_max_pass_ns/1e6);
    if(atomic_load(&mix_virtual)) fprintf(f,"kh: audio: clock=frames: WAV sample 0 = virtual time %llu ns; passes are tied to the virtual clock (late / catch-up / resync do not exist in this mode); pass grid %s\n",ULL(atomic_load(&mix_v_t0)),
                                          mix_unanchored?"NOT ANCHORED (engine created before the first vblank and none came within the guard: a determinism break)":mix_anchor_waited?"anchored (the engine was created before the first vblank and waited for it)":"anchored");
    fprintf(f,"kh: audio: destroyed voices=%llu peak live sources=%u peak started sources=%u peak queue depth=%u\n",ULL(t.voices_destroyed),t.peak_live_source,t.peak_active_source,t.peak_queued);
    fprintf(f,"kh: audio: buffers submitted=%llu (%llu bytes) rejected=%llu flushed=%llu started=%llu ended=%llu loop-ends=%llu stream-ends=%llu starvations=%llu; frames decoded=%llu resampled=%llu\n",ULL(t.buffers_submitted),ULL(t.bytes_submitted),ULL(st_submit_rejected),ULL(t.buffers_flushed),ULL(t.buffers_started),ULL(t.buffers_ended),ULL(t.loops_ended),ULL(t.streams_ended),ULL(t.starvations),ULL(t.frames_decoded),ULL(t.frames_resampled));
    fprintf(f,"kh: audio: guest callbacks: OnVoiceProcessingPassStart=%llu PassEnd=%llu OnStreamEnd=%llu OnBufferStart=%llu OnBufferEnd=%llu OnLoopEnd=%llu; engine OnProcessingPassStart=%llu PassEnd=%llu\n",ULL(st_cb[0]),ULL(st_cb[1]),ULL(st_cb[2]),ULL(st_cb[3]),ULL(st_cb[4]),ULL(st_cb[5]),ULL(st_ecb[0]),ULL(st_ecb[1]));
    fprintf(f,"kh: audio: mixer-thread time inside guest code, total / worst ms: OnVoiceProcessingPassStart %.0f / %.1f, PassEnd %.0f / %.1f, OnStreamEnd %.0f / %.1f, OnBufferStart %.0f / %.1f, OnBufferEnd %.0f / %.1f, OnLoopEnd %.0f / %.1f, engine PassStart %.0f / %.1f, engine PassEnd %.0f / %.1f, XAPO Process %.0f / %.1f\n",
            st_cb_ns[0]/1e6,st_cb_max[0]/1e6,st_cb_ns[1]/1e6,st_cb_max[1]/1e6,st_cb_ns[2]/1e6,st_cb_max[2]/1e6,st_cb_ns[3]/1e6,st_cb_max[3]/1e6,st_cb_ns[4]/1e6,st_cb_max[4]/1e6,st_cb_ns[5]/1e6,st_cb_max[5]/1e6,st_ecb_ns[0]/1e6,st_ecb_max[0]/1e6,st_ecb_ns[1]/1e6,st_ecb_max[1]/1e6,st_fx_ns/1e6,st_fx_max/1e6);
    fprintf(f,"kh: audio: effects: volume-meter passes=%llu; system reverb passed through unchanged (no DSP) on %llu voice-passes, its parameters set %llu times; guest XAPO SetParameters=%llu (%llu times a Process or SetParameters call waited for the other)\n",ULL(t.fx_meter),ULL(t.fx_pass_through),ULL(st_reverb_params),ULL(st_setparams),ULL(st_fx_waits));
    for(int i=0;i<nxapos;i++) fprintf(f,"kh: audio: guest XAPO %s (vtbl rva 0x%llx): locked in %llu chains, Process calls=%llu\n",xapos[i].name,RVA(xapos[i].vtbl),ULL(xapos[i].chains),ULL(xapos[i].process));
    fprintf(f,"kh: audio: creation: voice-passes skipped while a voice was not published yet=%llu; calls %s: queued=%llu applied=%llu dropped with their voice=%llu dropped by a later SetEffectChain of their voice=%llu refused=%llu, effect parameters delivered on the mixer thread=%llu\n",
            ULL(t.hidden_skips),kh_vclock_on&&!apply_mode?"stamped (applied at the first pass after their virtual instant)":"immediate",ULL(t.deferred_ops),ULL(t.deferred_applied),ULL(t.deferred_dropped),ULL(t.deferred_superseded),ULL(t.deferred_failed),ULL(st_params_deferred));
    if(peak_f){ fprintf(f,"kh: audio: level analysis: %llu passes with a device output above %.3f, the first %llu written to KHNMC_AUDIO_PEAKLOG\n",ULL(peak_passes_over),(double)peak_thr,ULL(peak_n)); fflush(peak_f); }
    if(out_pull){ KhAudioOutStats os; KhAudioOutFormat of; kh_audio_out_stats(&os); kh_audio_out_format(&of);
        fprintf(f,"kh: audio-out: pull ring %u frames, target fill %u: produced=%llu frames, pulled=%llu, delivered=%llu, underruns=%llu (%llu zero frames), overruns=%llu (%llu frames dropped), passes without a device=%llu, pulls=%llu starts=%llu stops=%llu, fill max=%u min while started=%u; mixer waits for the device=%llu (%.1f s)\n",
                of.ring_frames,of.target_frames,ULL(os.produced_frames),ULL(os.pulled_frames),ULL(os.delivered_frames),ULL(os.underruns),ULL(os.underrun_frames),ULL(os.overruns),ULL(os.overrun_frames),ULL(os.stopped_passes),ULL(os.pulls),ULL(os.starts),ULL(os.stops),os.max_fill,os.min_fill_started,ULL(st_pull_waits),st_pull_wait_ns/1e9); }
    if(had){ double sec=(double)ws.frames/48000.0,rms=ws.frames?sqrt(ws.sum_sq/((double)ws.frames*2.0)):0.0;
        if(wav_gain!=1.0f) fprintf(f,"kh: audio: wav gain %.2f dB (x%.4f) applied to the file (KHNMC_AUDIO_WAV_GAIN_DB); the figures below are after it\n",wav_gain_db,(double)wav_gain);
        fprintf(f,"kh: audio: wav %s: %llu frames (%.2f s) 48000 Hz 16-bit stereo, peak=%.4f (%.1f dBFS) rms=%.5f (%.1f dBFS) clipped samples=%llu non-silent=%.2f s\n",wav_path,ULL(ws.frames),sec,(double)ws.peak,ws.peak>0?20.0*log10((double)ws.peak):-999.0,rms,rms>0?20.0*log10(rms):-999.0,ULL(ws.clipped_samples),(double)ws.nonsilent_frames/48000.0); }
}
