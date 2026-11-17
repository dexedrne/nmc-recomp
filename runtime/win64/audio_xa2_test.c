/* audio_xa2_test.c : the software XAudio2 path exercised through the real guest ABI (COM vtables, packed XAudio2 structures, Win64
 * stack arguments), with test callback objects and a test XAPO built from host-function COM classes.  The passes are driven by the
 * test (kh_audio_test_pass), no clock thread and no WAV.  Part of `khnmc --selftest` (kh_audio_soft_selftest). */
#include "khnmc_rt.h"
#include "khnmc_audio_out.h"
#include "khnmc_mix.h"
#include <math.h>
#include <time.h>
#include <unistd.h>

int kh_audio_test_mode(int mode);
u32 kh_audio_test_pass(KCtx *g,u64 engine,float *out,u32 *channels);
void kh_audio_test_counters(u64 *voice_cb,u64 *engine_cb);
extern void rt_imp_xaudio2_9__ord1(KCtx*),rt_imp_xaudio2_9__ord2(KCtx*),rt_imp_xaudio2_9__ord3(KCtx*);

static int fails, checks;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"audio xa2 selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
#define NEAR(a,b,eps) (fabs((double)(a)-(double)(b))<=(eps))
#define E_INVALID 0x88960001u

static u64 call8(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5,u64 a6,u64 a7){
    u64 saved[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(saved[0]-0x200)&~15ull;
    memset(hp(g,sp),0,0x80); wr64(g,sp+0x20,a4); wr64(g,sp+0x28,a5); wr64(g,sp+0x30,a6); wr64(g,sp+0x38,a7);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=obj; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    kh_dispatch(g,rd64(g,rd64(g,obj)+slot*8));
    u64 r=KH_RAX(g);
    KH_RSP(g)=saved[0]; KH_RCX(g)=saved[1]; KH_RDX(g)=saved[2]; KH_R8(g)=saved[3]; KH_R9(g)=saved[4];
    return r;
}
static u32 vc(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3){ return (u32)call8(g,obj,slot,a1,a2,a3,0,0,0,0); }
static u32 vcf(KCtx *g,u64 obj,int slot,float x,u64 a2){ *(float*)&KH_XMM(g,1)=x; return (u32)call8(g,obj,slot,0,a2,0,0,0,0,0); }   /* (float, OperationSet) */
static void imp(KCtx *g,void (*f)(KCtx*),u64 a0){ u64 save=KH_RSP(g); u64 sp=(save-0x200)&~0xFull; sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RCX(g)=a0; f(g); KH_RSP(g)=save; }

/* ---- test callback objects ---- */
typedef struct { int slot; u64 arg; u64 obj; } CbEv;
static CbEv cblog[2048]; static int ncb;
static u64 cb_voice;                /* voice the voice-callback object belongs to */
static int cb_action;               /* 1: GetState + Stop(PLAY_TAILS) in PassStart and Start in PassEnd, like VoiceXA2 does when its stream stalls */
static u32 cb_seen_queued; static int C_vcb, C_ecb, C_tx, C_txp;
static int h_vcb(KCtx *g,u64 o,int s,const char *c){
    (void)c; if(ncb<2048){ cblog[ncb].slot=s; cblog[ncb].arg=A1(g); cblog[ncb].obj=o; ncb++; }
    if(cb_action==1&&cb_voice){
        if(s==0){ u64 st=ga_calloc(1,32); vc(g,cb_voice,25,st,0,0); cb_seen_queued=rd32(g,st+8); ga_free(st); vc(g,cb_voice,20,0x20,0,0); }
        if(s==1) vc(g,cb_voice,19,0,0,0);
    }
    SET_RAX(g,0); return 1;
}
static int h_ecb(KCtx *g,u64 o,int s,const char *c){ (void)c; if(ncb<2048){ cblog[ncb].slot=100+s; cblog[ncb].arg=0; cblog[ncb].obj=o; ncb++; } SET_RAX(g,0); return 1; }
static const char *cbseq(u64 obj){                       /* voice callback slots as digits (0 pass start .. 5 loop end), engine callbacks as A / B */
    static char s[1024]; int n=0;
    for(int i=0;i<ncb&&n<1000;i++){ if(obj&&cblog[i].obj!=obj) continue; s[n++]=cblog[i].slot>=100?(char)('A'+cblog[i].slot-100):(char)('0'+cblog[i].slot); }
    s[n]=0; return s;
}
/* ---- test XAPO: IXAPO object {+32 gain(float), +36 refs seen via AddRef/Release are the COM refcount at +16} and its IXAPOParameters twin ---- */
static struct { int qi_xapo, qi_params, regprops, in_ok, out_ok, lock, unlock, process, last_enabled, last_in_flags; u32 lock_in_ch, lock_out_ch, lock_rate, lock_frames, last_frames, flags; u64 params_obj; float gain; int silent_out;
                int slow_ms; _Atomic int in_process, overlap, setp; } tx;
static const u8 IID_XAPO[16]={0x84,0xb9,0x10,0xa4,0x39,0x98,0x19,0x48,0xa0,0xbe,0x28,0x56,0xae,0x6b,0x3a,0xdb};
static const u8 IID_PARAMS[16]={0x66,0x5c,0xd9,0x26,0xf2,0x80,0x9a,0x49,0xad,0x54,0x5a,0xe7,0xf0,0x1c,0x6d,0x98};
static const u8 IID_UNK[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static int h_tx(KCtx *g,u64 o,int s,const char *c){
    (void)c;
    switch(s){
    case 0:{ const u8 *iid=hp(g,A1(g)); u64 out=A2(g);
        if(!memcmp(iid,IID_XAPO,16)||!memcmp(iid,IID_UNK,16)){ tx.qi_xapo++; wr64(g,out,o); wr64(g,o+16,rd64(g,o+16)+1); SET_RAX(g,0); }
        else if(!memcmp(iid,IID_PARAMS,16)&&tx.params_obj){ tx.qi_params++; wr64(g,out,tx.params_obj); wr64(g,tx.params_obj+16,rd64(g,tx.params_obj+16)+1); SET_RAX(g,0); }
        else { wr64(g,out,0); SET_RAX(g,0x80004002u); }
        return 1; }
    case 1: case 2: return 0;                                                   /* default refcounting at +16 */
    case 3:{ u64 p=ga_calloc(1,0x42c); wr32(g,p+0x418,tx.flags); wr64(g,A1(g),p); tx.regprops++; SET_RAX(g,0); return 1; }   /* freed by the host like CoTaskMemFree */
    case 4: case 5:{ u64 other=A1(g),req=A2(g); if(s==4) tx.in_ok++; else tx.out_ok++;
        SET_RAX(g,(rd16(g,req)==3&&rd16(g,req+14)==32&&rd16(g,other)==3&&A3(g)==0)?0:0x88970001u); return 1; }
    case 8:{ u64 in=A2(g),out=ARG(g,4); tx.lock++;
        if(A1(g)!=1||A3(g)!=1){ SET_RAX(g,0x80070057u); return 1; }
        u64 fi=rd64(g,in),fo=rd64(g,out); tx.lock_in_ch=rd16(g,fi+2); tx.lock_out_ch=rd16(g,fo+2); tx.lock_rate=rd32(g,fi+4); tx.lock_frames=rd32(g,in+8);
        SET_RAX(g,rd32(g,out+8)==tx.lock_frames&&rd32(g,fo+4)==tx.lock_rate?0:0x80070057u); return 1; }
    case 9: tx.unlock++; SET_RAX(g,0); return 1;
    case 10:{ u64 in=A2(g),out=ARG(g,4); u32 en=(u32)ARG(g,5),n=rd32(g,in+12); tx.process++; tx.last_enabled=(int)en; tx.last_in_flags=(int)rd32(g,in+8); tx.last_frames=n;
        if(tx.slow_ms){ atomic_store(&tx.in_process,1); struct timespec ts={0,(long)tx.slow_ms*1000000L}; nanosleep(&ts,0); atomic_store(&tx.in_process,0); }   /* a Process that takes a while: is SetParameters kept out? */
        float *src=hp(g,rd64(g,in)),*dst=hp(g,rd64(g,out)); u32 ic=tx.lock_in_ch,oc=tx.lock_out_ch; float gn=en?tx.gain:1.0f;
        for(u32 k=0;k<n;k++){ if(ic==oc) for(u32 ch=0;ch<oc;ch++) dst[k*oc+ch]=src[k*ic+ch]*gn; else { float sum=0; for(u32 ch=0;ch<ic;ch++) sum+=src[k*ic+ch]; for(u32 ch=0;ch<oc;ch++) dst[k*oc+ch]=sum*gn; } }
        wr32(g,out+8,tx.silent_out?0:1); wr32(g,out+12,n); SET_RAX(g,0); return 1; }
    case 6: case 7: case 11: case 12: SET_RAX(g,0); return 1;
    }
    return 0;
}
static int h_txp(KCtx *g,u64 o,int s,const char *c){
    (void)c; (void)o;
    if(s==3){ atomic_fetch_add(&tx.setp,1); if(atomic_load(&tx.in_process)) atomic_fetch_add(&tx.overlap,1); if((u32)A2(g)==4) memcpy(&tx.gain,hp(g,A1(g)),4); SET_RAX(g,0); return 1; }        /* SetParameters(const void*, UINT32) */
    if(s==4){ if((u32)A2(g)==4) memcpy(hp(g,A1(g)),&tx.gain,4); SET_RAX(g,0); return 1; }        /* GetParameters */
    return 0;
}
static u64 mk_chain(KCtx *g,u32 n,const u64 *objs,const u32 *initial,const u32 *outch){
    u64 chain=ga_calloc(1,16),desc=ga_calloc(n?n:1,16); wr32(g,chain,n); wr64(g,chain+4,desc);
    for(u32 i=0;i<n;i++){ wr64(g,desc+16ull*i,objs[i]); wr32(g,desc+16ull*i+8,initial[i]); wr32(g,desc+16ull*i+12,outch[i]); }
    return chain;
}
static void free_chain(KCtx *g,u64 chain){ ga_free(rd64(g,chain+4)); ga_free(chain); }
static u64 mk_sends(KCtx *g,u32 n,const u64 *voices,u32 flags){
    u64 list=ga_calloc(1,16),d=ga_calloc(n?n:1,12); wr32(g,list,n); wr64(g,list+4,d);
    for(u32 i=0;i<n;i++){ wr32(g,d+12ull*i,flags); wr64(g,d+12ull*i+4,voices[i]); }
    return list;
}
static u64 mk_pcm_format(KCtx *g,u32 ch,u32 rate,u32 bits){
    u64 f=ga_calloc(1,24); wr16(g,f,1); wr16(g,f+2,(u16)ch); wr32(g,f+4,rate); wr32(g,f+8,rate*ch*bits/8); wr16(g,f+12,(u16)(ch*bits/8)); wr16(g,f+14,(u16)bits); return f;
}
static u64 mk_buffer(KCtx *g,u32 flags,u64 data,u32 bytes,u64 ctx){
    u64 b=ga_calloc(1,48); wr32(g,b,flags); wr32(g,b+4,bytes); wr64(g,b+8,data); wr64(g,b+36,ctx); return b;
}
static u32 create_source(KCtx *g,u64 engine,u64 out,u64 fmt,u64 cb,u64 sends,u64 chain){
    float mr=2.0f; u32 bits; memcpy(&bits,&mr,4);
    return (u32)call8(g,engine,5,out,fmt,0,bits,cb,sends,chain);
}

int kh_audio_soft_selftest(KCtx *g){
    fails=checks=0; ncb=0; memset(&tx,0,sizeof tx);
    int prev=kh_audio_test_mode(1);
    C_vcb=kh_com_class("xa2:test-IXAudio2VoiceCallback",7,COM_STRICT|COM_NOUNK,h_vcb);
    C_ecb=kh_com_class("xa2:test-IXAudio2EngineCallback",3,COM_STRICT|COM_NOUNK,h_ecb);
    C_tx=kh_com_class("xa2:test-IXAPO",13,COM_STRICT,h_tx);
    C_txp=kh_com_class("xa2:test-IXAPOParameters",5,COM_STRICT,h_txp);
    u64 out=ga_calloc(1,128); float mix[960]; u32 oc=0; u64 vcb0[6],ecb0[2],vcb1[6],ecb1[2];
    kh_audio_test_counters(vcb0,ecb0);
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(KH_RAX(g)==0&&engine!=0);
    CHECK(kh_audio_test_pass(g,engine,mix,&oc)==1&&oc==0&&mix[0]==0.0f);                     /* no mastering voice: silence */
    u64 ecb=kh_com_obj(C_ecb,0); CHECK(vc(g,engine,3,ecb,0,0)==0&&vc(g,engine,3,0,0,0)==E_INVALID);
    /* mastering voice: 2 ch, chain = test XAPO (gain 0.5, in place, with IXAPOParameters) + the system volume meter */
    u64 xapo=kh_com_obj(C_tx,16); tx.params_obj=kh_com_obj(C_txp,0); tx.gain=0.5f; tx.flags=0x10;        /* XAPO_FLAG_INPLACE_SUPPORTED */
    imp(g,rt_imp_xaudio2_9__ord3,out); u64 meter=rd64(g,out);
    imp(g,rt_imp_xaudio2_9__ord2,out); u64 reverb=rd64(g,out);
    u64 refs_x=COM_REFS(g,xapo),refs_p=COM_REFS(g,tx.params_obj),refs_m=COM_REFS(g,meter);
    u64 objs[3]={xapo,meter,0}; u32 ini[3]={1,1,1},och[3]={2,2,2};
    u64 chain=mk_chain(g,2,objs,ini,och);
    CHECK((u32)call8(g,engine,7,out,2,48000,0,0,chain,0)==0); u64 master=rd64(g,out); CHECK(master!=0);
    CHECK(tx.qi_xapo==1&&tx.qi_params==1&&tx.regprops==1&&tx.in_ok==1&&tx.out_ok==1&&tx.lock==1&&tx.lock_in_ch==2&&tx.lock_out_ch==2&&tx.lock_rate==48000&&tx.lock_frames==480);
    CHECK(COM_REFS(g,xapo)==refs_x+1&&COM_REFS(g,tx.params_obj)==refs_p+1&&COM_REFS(g,meter)==refs_m+1);
    CHECK((u32)call8(g,engine,7,out,2,48000,0,0,0,0)==E_INVALID&&rd64(g,out)==0);              /* a second mastering voice */
    /* 44.1 kHz stereo submix (default send = master) and a PCM16 stereo source with a callback, a send to the submix and the system reverb */
    CHECK((u32)call8(g,engine,6,out,2,44100,0,0,0,0)==0); u64 submix=rd64(g,out);
    u64 fmt=mk_pcm_format(g,2,48000,16),vcb=kh_com_obj(C_vcb,0),sends=mk_sends(g,1,&submix,0);
    u64 robjs[1]={reverb}; u64 rchain=mk_chain(g,1,robjs,ini,och);
    CHECK(create_source(g,engine,out,fmt,vcb,sends,rchain)==0); u64 src=rd64(g,out); CHECK(src!=0); cb_voice=src;
    { u64 bad=mk_sends(g,1,&src,0); CHECK(create_source(g,engine,out,fmt,vcb,bad,0)==E_INVALID&&rd64(g,out)==0); ga_free(rd64(g,bad+4)); ga_free(bad); }   /* a source voice is not a destination */
    { u64 f8=mk_pcm_format(g,2,48000,12); CHECK(create_source(g,engine,out,f8,0,0,0)==E_INVALID); ga_free(f8); }
    memset(hp(g,out),0xff,32); vc(g,src,0,out,0,0); CHECK(rd32(g,out)==0&&rd32(g,out+8)==2&&rd32(g,out+12)==48000);
    /* DC 0.5 on the left, -0.25 on the right; matrix swaps the channels; voice volume 0.5 */
    u32 frames=48000; u64 pcm=ga_alloc(frames*4,16); for(u32 i=0;i<frames;i++){ wr16(g,pcm+4*i,16384); wr16(g,pcm+4*i+2,(u16)(s16)-8192); }
    u64 m=ga_alloc(64,16); float sw[4]={0,1,1,0}; memcpy(hp(g,m),sw,16);
    CHECK((u32)call8(g,src,16,submix,2,2,m,0,0,0)==0&&(u32)call8(g,src,16,submix,2,3,m,0,0,0)==E_INVALID&&(u32)call8(g,src,16,master,2,2,m,0,0,0)==E_INVALID);
    memset(hp(g,m),0,16); CHECK((u32)call8(g,src,17,submix,2,2,m,0,0,0)==0&&!memcmp(hp(g,m),sw,16));
    CHECK(vcf(g,src,12,0.5f,0)==0); vc(g,src,13,out,0,0); CHECK(rd32(g,out)==0x3f000000);
    u64 buf=mk_buffer(g,0x40,pcm,frames*4,0x1234567890ull);
    CHECK(vc(g,src,21,buf,0,0)==0);
    memset(hp(g,out),0xa5,32); CHECK(vc(g,src,25,out,0,0)==0&&rd64(g,out)==0x1234567890ull&&rd32(g,out+8)==1&&rd64(g,out+12)==0&&rd32(g,out+20)==0xa5a5a5a5);   /* 20-byte packed XAUDIO2_VOICE_STATE */
    /* not started: silence, voice pass callbacks with BytesRequired 0, engine callbacks around everything */
    ncb=0; CHECK(kh_audio_test_pass(g,engine,mix,&oc)==1&&oc==2&&mix[0]==0.0f&&mix[959]==0.0f);
    CHECK(!strcmp(cbseq(0),"A01B")&&cblog[1].arg==0&&cblog[1].obj==vcb&&cblog[0].obj==ecb);
    CHECK(vc(g,src,19,1,0,0)==E_INVALID&&vc(g,src,19,0,0,0)==0);
    ncb=0; kh_audio_test_pass(g,engine,mix,&oc);
    CHECK(!strcmp(cbseq(0),"A031B")&&cblog[2].arg==0x1234567890ull);
    /* source 48k -> submix 44.1k (source resampler) -> master 48k (fixed-rate resampler, 2 frames of delay) -> XAPO x0.5 -> meter.
     * left out = right in (-0.25) * volume 0.5 * 0.5 = -0.0625 ; right out = 0.5 * 0.5 * 0.5 = 0.125 */
    CHECK(mix[0]==0.0f&&NEAR(mix[2*479],-0.0625,1e-6)&&NEAR(mix[2*479+1],0.125,1e-6));
    kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[0],-0.0625,1e-6)&&NEAR(mix[1],0.125,1e-6)&&NEAR(mix[958],-0.0625,1e-6));
    CHECK(tx.process==3&&tx.last_enabled==1&&tx.last_frames==480&&tx.last_in_flags==1);
    vc(g,src,25,out,0,0); CHECK(rd32(g,out+8)==1&&rd64(g,out+12)==960); vc(g,src,25,out,0x100,0); CHECK(rd64(g,out+12)==0);   /* XAUDIO2_VOICE_NOSAMPLESPLAYED */
    /* volume meter through GetEffectParameters (XAUDIO2FX_VOLUMEMETER_LEVELS) */
    wr64(g,out,out+32); wr64(g,out+8,out+48); wr32(g,out+16,2); memset(hp(g,out+32),0xa5,32);
    CHECK(vc(g,master,7,1,out,20)==0); float pk[2],rm[2]; memcpy(pk,hp(g,out+32),8); memcpy(rm,hp(g,out+48),8);
    CHECK(NEAR(pk[0],0.0625,1e-6)&&NEAR(pk[1],0.125,1e-6)&&NEAR(rm[0],0.0625,1e-6)&&NEAR(rm[1],0.125,1e-6)&&rd64(g,out+40)==0xa5a5a5a5a5a5a5a5ull);
    wr32(g,out+16,3); CHECK(vc(g,master,7,1,out,20)==E_INVALID); wr32(g,out+16,2); CHECK(vc(g,master,7,1,out,16)==E_INVALID&&vc(g,master,7,5,out,20)==E_INVALID);
    /* XAPO parameters travel through IXAPOParameters; a disabled XAPO is still called with IsEnabled = FALSE */
    float gnew=0.25f; memcpy(hp(g,out),&gnew,4); CHECK((u32)call8(g,master,6,0,out,4,0,0,0,0)==0&&tx.gain==0.25f);
    wr32(g,out,0); CHECK(vc(g,master,7,0,out,4)==0&&rd32(g,out)==0x3e800000);
    kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[959],0.0625,1e-6));
    CHECK(vc(g,master,4,0,0,0)==0); vc(g,master,5,0,out,0); CHECK(rd32(g,out)==0);
    kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[959],0.25,1e-6)&&tx.last_enabled==0);
    CHECK(vc(g,master,3,0,0,0)==0&&vc(g,master,3,9,0,0)==E_INVALID);
    /* the system reverb has no DSP: its parameter blob is retained, audio passes unchanged (the level above already shows it) */
    wr32(g,out,0x11223344); CHECK((u32)call8(g,src,6,0,out,4,0,0,0,0)==0); wr32(g,out,0); CHECK(vc(g,src,7,0,out,4)==0&&rd32(g,out)==0x11223344&&vc(g,src,7,0,out,8)==E_INVALID);
    /* master and submix volume, channel volumes */
    CHECK(vcf(g,master,12,2.0f,0)==0&&vcf(g,submix,12,0.5f,0)==0);
    kh_audio_test_pass(g,engine,mix,&oc); kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[959],0.125*0.25*2.0*0.5*2.0,1e-6));
    vcf(g,master,12,1.0f,0); vcf(g,submix,12,1.0f,0);
    float cvs[2]={0.0f,1.0f}; memcpy(hp(g,out),cvs,8); CHECK(vc(g,src,14,2,out,0)==0&&vc(g,src,14,3,out,0)==E_INVALID);
    kh_audio_test_pass(g,engine,mix,&oc); kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[958],-0.03125,1e-6)&&mix[959]==0.0f);     /* source left muted: nothing reaches the right output */
    cvs[0]=1.0f; memcpy(hp(g,out),cvs,8); vc(g,src,14,2,out,0);
    /* frequency ratio 2: twice the frames per pass */
    vc(g,src,25,out,0,0); u64 p0=rd64(g,out+12); CHECK(vcf(g,src,26,2.0f,0)==0); vc(g,src,27,out,0,0); CHECK(rd32(g,out)==0x40000000);
    kh_audio_test_pass(g,engine,mix,&oc); vc(g,src,25,out,0,0); CHECK(rd64(g,out+12)-p0>=958&&rd64(g,out+12)-p0<=962); vcf(g,src,26,1.0f,0);
    CHECK(vcf(g,src,26,0.0f,0)==E_INVALID);
    /* callbacks that call back: GetState + Stop(PLAY_TAILS) in the pass start, Start in the pass end (what VoiceXA2 does while its stream stalls) */
    kh_audio_test_pass(g,engine,mix,&oc);                                                      /* let the channel-volume ramp of the previous pass settle */
    cb_action=1; ncb=0; cb_seen_queued=99; kh_audio_test_pass(g,engine,mix,&oc); cb_action=0;
    CHECK(cb_seen_queued==1&&!strcmp(cbseq(vcb),"01")&&NEAR(mix[1],0.0625,1e-6)&&mix[959]==0.0f);   /* stopped for this pass only: just the 2 delayed frames of the submix resampler are heard */
    kh_audio_test_pass(g,engine,mix,&oc); kh_audio_test_pass(g,engine,mix,&oc); CHECK(NEAR(mix[959],0.0625,1e-6));
    /* Stop, flush (the playing buffer goes too), OnBufferEnd on the next pass, nothing else */
    CHECK(vc(g,src,20,0,0,0)==0&&vc(g,src,22,0,0,0)==0); vc(g,src,25,out,0,0); CHECK(rd32(g,out+8)==0&&rd64(g,out)==0);
    ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(!strcmp(cbseq(vcb),"041")&&cblog[2].arg==0x1234567890ull);
    /* a short END_OF_STREAM buffer: start, end, stream end; loop fields; invalid buffers are rejected */
    wr32(g,buf+4,400); wr64(g,buf+36,7); CHECK(vc(g,src,21,buf,0,0)==0); vc(g,src,19,0,0,0);
    ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(!strcmp(cbseq(vcb),"03421")&&cblog[2].arg==7&&cblog[3].arg==7);
    vc(g,src,25,out,0,0); CHECK(rd32(g,out+8)==0&&rd64(g,out+12)==0);
    wr32(g,buf,0); wr32(g,buf+4,400); wr32(g,buf+24,10); wr32(g,buf+28,20); wr32(g,buf+32,3); CHECK(vc(g,src,21,buf,0,0)==0);
    ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(!strcmp(cbseq(vcb),"0355541"));
    wr32(g,buf+32,0); CHECK(vc(g,src,21,buf,0,0)==E_INVALID);                                 /* loop region without a loop count */
    wr32(g,buf+24,0); wr32(g,buf+28,0); wr32(g,buf+16,100); CHECK(vc(g,src,21,buf,0,0)==E_INVALID);   /* PlayBegin beyond the data */
    wr32(g,buf+16,0); wr32(g,buf+4,402); CHECK(vc(g,src,21,buf,0,0)==E_INVALID);
    /* MS-ADPCM source (ADPCMWAVEFORMAT, 50 bytes) sent straight to the master; the hand-built block of audio_mix_test.c */
    u64 af=ga_calloc(1,64); wr16(g,af,2); wr16(g,af+2,1); wr32(g,af+4,48000); wr32(g,af+8,48000*11/10); wr16(g,af+12,11); wr16(g,af+14,4); wr16(g,af+16,32); wr16(g,af+18,10); wr16(g,af+20,7);
    for(int i=0;i<7;i++){ wr16(g,af+22+4*i,(u16)kmx_adpcm_std_coef[i][0]); wr16(g,af+24+4*i,(u16)kmx_adpcm_std_coef[i][1]); }
    CHECK(create_source(g,engine,out,af,0,0,0)==0); u64 asrc=rd64(g,out);
    static const u8 blk[11]={1,0x10,0x00,0xE8,0x03,0x84,0x03,0x3F,0x78,0x40,0xC1}; static const s16 ref[10]={900,1000,1148,1280,1524,1464,1860,2256,2164,2218};
    u64 ad=ga_alloc(16,16); memcpy(hp(g,ad),blk,11); u64 ab=mk_buffer(g,0x40,ad,11,0);
    float one=1.0f; memcpy(hp(g,out),&one,4); memset(hp(g,out+4),0,4); CHECK((u32)call8(g,asrc,16,0,1,2,out,0,0,0)==0);         /* mono -> left only; NULL destination = the single send */
    CHECK(vc(g,asrc,21,ab,0,0)==0&&vc(g,asrc,19,0,0,0)==0); vc(g,master,4,0,0,0);
    kh_audio_test_pass(g,engine,mix,&oc); int ok=1; for(int i=0;i<10;i++) if(mix[2*i]!=(float)ref[i]/32768.0f||mix[2*i+1]!=0.0f) ok=0;
    CHECK(ok&&mix[20]==0.0f); vc(g,master,3,0,0,0);
    CHECK(vc(g,asrc,18,0,0,0)==0);
    /* IEEE float through WAVEFORMATEXTENSIBLE */
    u64 xf=ga_calloc(1,48); wr16(g,xf,0xfffe); wr16(g,xf+2,1); wr32(g,xf+4,48000); wr32(g,xf+8,48000*4); wr16(g,xf+12,4); wr16(g,xf+14,32); wr16(g,xf+16,22); wr16(g,xf+18,32); wr32(g,xf+20,4); wr32(g,xf+24,3); wr32(g,xf+28,0x00100000); wr64(g,xf+32,0x719b3800aa000080ull);
    CHECK(create_source(g,engine,out,xf,0,0,0)==0); u64 fsrc=rd64(g,out); float fv[4]={0.5f,-0.5f,0.25f,1.0f}; u64 fd=ga_alloc(16,16); memcpy(hp(g,fd),fv,16); u64 fb=mk_buffer(g,0,fd,16,0);
    CHECK(vc(g,fsrc,21,fb,0,0)==0&&vc(g,fsrc,19,0,0,0)==0); vc(g,master,4,0,0,0); kh_audio_test_pass(g,engine,mix,&oc); vc(g,master,3,0,0,0);
    CHECK(mix[0]==0.5f&&mix[1]==0.5f&&mix[2]==-0.5f&&mix[6]==1.0f&&mix[8]==0.0f);              /* default mono -> stereo levels: both fronts */
    CHECK(vc(g,fsrc,28,24000,0,0)==0); vc(g,fsrc,0,out,0,0); CHECK(rd32(g,out+12)==24000);
    CHECK(vc(g,fsrc,18,0,0,0)==0);
    /* SetOutputVoices: source moves from the submix to the master (NULL list); SetEffectChain replaces and removes chains */
    CHECK(vc(g,src,1,0,0,0)==0);
    { u64 none=mk_sends(g,0,NULL,0); CHECK(vc(g,src,1,none,0,0)==0); ga_free(rd64(g,none+4)); ga_free(none); CHECK(vc(g,src,1,0,0,0)==0); }
    { u64 flt=mk_sends(g,1,&submix,0x40); CHECK(vc(g,src,1,flt,0,0)==E_INVALID); ga_free(rd64(g,flt+4)); ga_free(flt); }
    CHECK(vc(g,src,2,0,0,0)==0&&vc(g,src,7,0,out,4)==E_INVALID);                               /* chain removed: no effect 0 any more */
    int lk=tx.lock,ul=tx.unlock;
    u64 two[2]={xapo,xapo}; u32 och2[2]={1,2}; tx.flags=0; u64 chain2=mk_chain(g,2,two,ini,och2);  /* 2 -> 1 -> 2 with the same test XAPO twice, out of place */
    CHECK(vc(g,master,2,chain2,0,0)==0&&tx.lock==lk+2&&tx.unlock==ul+1&&COM_REFS(g,meter)==refs_m&&COM_REFS(g,xapo)==refs_x+2);
    tx.flags=1; CHECK(vc(g,master,2,chain2,0,0)==E_INVALID&&COM_REFS(g,xapo)==refs_x+2);        /* XAPO_FLAG_CHANNELS_MUST_MATCH against a 2 -> 1 request: refused, old chain kept */
    tx.flags=0x10; CHECK(vc(g,master,2,0,0,0)==0&&COM_REFS(g,xapo)==refs_x&&COM_REFS(g,tx.params_obj)==refs_p&&tx.unlock==ul+3);
    /* GetPerformanceData, StopEngine / StartEngine, UnregisterForCallbacks */
    memset(hp(g,out),0xa5,96); vc(g,engine,11,out,0,0); CHECK(rd64(g,out)==0&&rd32(g,out+28)==480&&rd32(g,out+40)==1&&rd32(g,out+44)==1&&rd64(g,out+64)==0xa5a5a5a5a5a5a5a5ull);
    CHECK(vc(g,engine,9,0,0,0)==0); ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(ncb==0&&oc==0); CHECK(vc(g,engine,8,0,0,0)==0);
    vc(g,engine,4,ecb,0,0); ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(!strcmp(cbseq(0),"01"));
    /* DestroyVoice: no callback afterwards; leftover voices die with the engine and release their effects */
    CHECK(vc(g,src,18,0,0,0)==0); ncb=0; kh_audio_test_pass(g,engine,mix,&oc); CHECK(ncb==0);
    CHECK(vc(g,master,2,chain,0,0)==0&&COM_REFS(g,xapo)==refs_x+1);
    kh_audio_test_counters(vcb1,ecb1);
    CHECK(vcb1[0]-vcb0[0]>10&&vcb1[0]-vcb0[0]==vcb1[1]-vcb0[1]&&vcb1[3]-vcb0[3]==3&&vcb1[4]-vcb0[4]==3&&vcb1[2]-vcb0[2]==1&&vcb1[5]-vcb0[5]==3&&ecb1[0]-ecb0[0]==ecb1[1]-ecb0[1]&&ecb1[0]>ecb0[0]);
    CHECK(vc(g,engine,2,0,0,0)==0);                                                            /* last Release: master and submix destroyed, XAPO unlocked and released */
    CHECK(COM_REFS(g,xapo)==refs_x&&COM_REFS(g,meter)==refs_m&&COM_REFS(g,tx.params_obj)==refs_p&&rd64(g,master+48)==0&&rd64(g,submix+48)==0);
    CHECK(kh_audio_test_pass(g,engine,mix,&oc)==0);
    /* the null backend is untouched by the above */
    kh_audio_test_mode(0);
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 ne=rd64(g,out); CHECK(rd64(g,ne+32)==0&&kh_audio_test_pass(g,ne,mix,&oc)==0); vc(g,ne,2,0,0,0);
    free_chain(g,chain); free_chain(g,chain2); free_chain(g,rchain); ga_free(rd64(g,sends+4)); ga_free(sends);
    ga_free(fmt); ga_free(pcm); ga_free(m); ga_free(buf); ga_free(af); ga_free(ad); ga_free(ab); ga_free(xf); ga_free(fd); ga_free(fb); ga_free(out);
    kh_audio_test_mode(prev);
    fprintf(stderr,"kh: audio xa2 selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

/* ---- KHNMC_CLOCK=frames: the device clock is the virtual display clock (TASK18) ------------------------------------------------
 * The real clock thread ("xa2-mix") runs on a stepped virtual clock that only this test moves, and writes a WAV:
 *   - while the clock stands, no pass is made, however much host time goes by;
 *   - a virtual vblank is exactly 800 sample frames: 30 vblanks = 0.5 s = the passes at 0, 10, ..., 500 ms;
 *   - a sound submitted and started after vblank 30 begins at the first pass after that instant (sample 24,480 = 510 ms), inside the
 *     picture interval that follows vblank 30 (samples 24,000..24,800), and plays its 4,800 frames bit-exactly (through the file gain
 *     of KHNMC_AUDIO_WAV_GAIN_DB, here 0.5: 16384 -> 8192, -8192 -> -4096);
 *   - the WAV length is the number of passes, nothing is skipped or inserted. */
void kh_audio_test_wav(const char *path); u64 kh_audio_test_quanta(void); int kh_audio_test_stop_clock(KCtx *g); u64 kh_audio_vclock_t0(void);
int kh_audio_vclock_selftest(KCtx *g){
    int c0=checks, f0=fails;
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"khnmc-vclock-wav-XXXXXX"); int fd=mkstemp(path); CHECK(fd>=0); if(fd<0) return 1; close(fd);
    const u64 T0=9000000000000ull; kh_vclock_test_begin(T0);
    extern float kh_audio_test_wav_gain(float); float prev_gain=kh_audio_test_wav_gain(0.5f);        /* the file gain (KHNMC_AUDIO_WAV_GAIN_DB) at -6.02 dB: exact halves */
    int prev=kh_audio_test_mode(2); kh_audio_test_wav(path);
    u64 out=ga_calloc(1,128), q0=kh_audio_test_quanta();
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(KH_RAX(g)==0&&engine!=0);
    for(int i=0;i<3000&&(!kh_audio_vclock_t0()||kh_audio_test_quanta()==q0);i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    CHECK(kh_audio_vclock_t0()==T0); CHECK(kh_audio_test_quanta()==q0+1);                          /* pass 0 at the instant the device starts */
    CHECK((u32)call8(g,engine,7,out,2,48000,0,0,0,0)==0); u64 master=rd64(g,out); CHECK(master!=0);
    u64 fmt=mk_pcm_format(g,2,48000,16); CHECK(create_source(g,engine,out,fmt,0,0,0)==0); u64 src=rd64(g,out); CHECK(src!=0);
    { struct timespec ts={0,60000000}; nanosleep(&ts,0); } CHECK(kh_audio_test_quanta()==q0+1);     /* 60 ms of host time, 0 ms of guest time: no pass */
    for(int i=0;i<30;i++) kh_vclock_test_vblank(g);
    CHECK(kh_now_ns()==T0+500000000ull); CHECK(kh_audio_test_quanta()==q0+51);                     /* 0.5 s: passes at 0 .. 500 ms, all done when the vblank returns */
    u32 frames=4800; u64 pcm=ga_alloc(frames*4,16); for(u32 i=0;i<frames;i++){ wr16(g,pcm+4*i,16384); wr16(g,pcm+4*i+2,(u16)(s16)-8192); }
    u64 buf=mk_buffer(g,0x40,pcm,frames*4,0);
    CHECK(vc(g,src,21,buf,0,0)==0&&vc(g,src,19,0,0,0)==0);                                         /* SubmitSourceBuffer + Start "during the frame after vblank 30" */
    for(int i=0;i<12;i++) kh_vclock_test_vblank(g);
    CHECK(kh_now_ns()==T0+700000000ull); CHECK(kh_audio_test_quanta()==q0+71);
    { struct timespec ts={0,40000000}; nanosleep(&ts,0); } CHECK(kh_audio_test_quanta()==q0+71);
    CHECK(kh_audio_test_stop_clock(g)==1); CHECK(kh_audio_test_quanta()==q0+71);                  /* the clock thread ends at its next instant without a pass */
    kh_audio_test_wav(NULL);
    CHECK(vc(g,src,18,0,0,0)==0); CHECK(vc(g,master,18,0,0,0)==0); vc(g,engine,2,0,0,0);
    kh_audio_test_mode(prev); kh_vclock_test_end(); kh_audio_test_wav_gain(prev_gain);
    FILE *f=fopen(path,"rb"); CHECK(f!=NULL);
    if(f){ unsigned char h[44]; CHECK(fread(h,1,44,f)==44&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVEfmt ",8)&&!memcmp(h+36,"data",4));
        u32 rate,dlen; u16 ch,bits; memcpy(&ch,h+22,2); memcpy(&rate,h+24,4); memcpy(&bits,h+34,2); memcpy(&dlen,h+40,4);
        CHECK(ch==2&&rate==48000&&bits==16); CHECK(dlen==71u*480u*4u);                              /* 71 passes, 34,080 frames: 42 vblanks * 800 + the pass due at the last instant */
        s16 *d=malloc(dlen?dlen:4); size_t got=d?fread(d,1,dlen,f):0; CHECK(got==dlen);
        long first=-1,last=-1,wrong=0; u32 n=dlen/4;
        if(d&&got==dlen) for(u32 i=0;i<n;i++){ if(d[2*i]||d[2*i+1]){ if(first<0) first=(long)i; last=(long)i; if(d[2*i]!=8192||d[2*i+1]!=-4096) wrong++; } }
        CHECK(first==51*480); CHECK(last==51*480+4800-1); CHECK(wrong==0);
        CHECK(first>30*800&&first<=31*800);                                                         /* in the sound of the picture interval that follows vblank 30 */
        if(first!=51*480||wrong) fprintf(stderr,"audio vclock selftest: first non-zero frame %ld, last %ld, wrong samples %ld\n",first,last,wrong);
        free(d); fclose(f); }
    unlink(path); ga_free(out); ga_free(fmt); ga_free(pcm); ga_free(buf);
    fprintf(stderr,"kh: audio vclock selftest: %d checks, %d failures\n",checks-c0,fails-f0);
    return fails-f0;
}

/* ---- review of task 28: an engine created BEFORE the first vblank (the clock still the host's) ---------------------------------------
 * 8 of 100 one-CPU starts created the XAudio2 engine before the first WaitForVBlank; its mixer then took t0 = the host instant of
 * creation and ran passes on the host clock until stepping began: a pass grid whose phase against the vblanks depended on the host.
 * Now: no pass before the first vblank, pass 0 at its origin, then one pass per 10 ms of virtual time. */
int kh_audio_vclock_anchor_selftest(KCtx *g){
    int c0=checks, f0=fails;
    if(kh_vclock_stepped()){ CHECK(!"the clock is already stepped (KHNMC_CLOCK=frames selftest?)"); return fails-f0; }
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"khnmc-anchor-wav-XXXXXX"); int fd=mkstemp(path); CHECK(fd>=0); if(fd<0) return 1; close(fd);
    int on0=kh_vclock_on; kh_vclock_on=1;                                                       /* KHNMC_CLOCK=frames before the first WaitForVBlank */
    int prev=kh_audio_test_mode(2); kh_audio_test_wav(path);
    u64 out=ga_calloc(1,128), q0=kh_audio_test_quanta();
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(KH_RAX(g)==0&&engine!=0);
    { struct timespec ts={0,50000000}; nanosleep(&ts,0); }
    CHECK(kh_audio_test_quanta()==q0); CHECK(kh_audio_vclock_t0()==0);                          /* 50 ms of host time before the first vblank: no pass, no grid yet */
    const u64 T0=11000000000000ull; kh_vclock_test_begin(T0);                                    /* the first vblank: the clock leaves the host clock at T0 */
    for(int i=0;i<3000&&(!kh_audio_vclock_t0()||kh_audio_test_quanta()==q0);i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    CHECK(kh_audio_vclock_t0()==T0); CHECK(kh_audio_test_quanta()==q0+1);                       /* pass 0 at the origin */
    for(int i=0;i<3;i++) kh_vclock_test_vblank(g);
    CHECK(kh_audio_test_quanta()==q0+6);                                                         /* 50 ms: passes at 0, 10, .., 50 ms */
    CHECK(kh_audio_test_stop_clock(g)==1);
    kh_audio_test_wav(NULL); vc(g,engine,2,0,0,0);
    kh_audio_test_mode(prev); kh_vclock_test_end(); kh_vclock_on=on0;
    unlink(path); ga_free(out);
    fprintf(stderr,"kh: audio vclock anchor selftest: %d checks, %d failures\n",checks-c0,fails-f0);
    return fails-f0;
}

/* ---- SetParameters never runs inside Process (the XAPO rule the engine's effects rely on) ------------------------------------------
 * The test XAPO's Process takes 60 ms; a second guest thread calls IXAudio2Voice::SetEffectParameters 15 ms into it.  The call must
 * wait for Process to return (so it takes about 45 ms), and the XAPO must never see SetParameters while its Process is running. */
typedef struct { u64 voice, blob; _Atomic int done; _Atomic u64 took_ns; u32 hr; } FxSetter;
u64 kh_host_ns(void);
static void fx_setter_thread(KCtx *g){ FxSetter *s=(FxSetter*)(uintptr_t)A0(g);
    { struct timespec ts={0,15000000}; nanosleep(&ts,0); }
    u64 t=kh_host_ns(); s->hr=(u32)call8(g,s->voice,6,0,s->blob,4,0,0,0,0); atomic_store(&s->took_ns,kh_host_ns()-t); atomic_store(&s->done,1); KH_RET(g); }
int kh_audio_fx_serial_selftest(KCtx *g){
    int c0=checks, f0=fails; memset(&tx,0,sizeof tx);
    int prev=kh_audio_test_mode(1);
    if(!C_tx){ C_tx=kh_com_class("xa2:test-IXAPO",13,COM_STRICT,h_tx); C_txp=kh_com_class("xa2:test-IXAPOParameters",5,COM_STRICT,h_txp); }
    u64 out=ga_calloc(1,128); float mix[960]; u32 oc=0;
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(engine!=0);
    u64 xapo=kh_com_obj(C_tx,16); tx.params_obj=kh_com_obj(C_txp,0); tx.gain=0.5f; tx.flags=0x10;
    u64 objs[1]={xapo}; u32 ini[1]={1},och[1]={2}; u64 chain=mk_chain(g,1,objs,ini,och);
    CHECK((u32)call8(g,engine,7,out,2,48000,0,0,chain,0)==0); u64 master=rd64(g,out); CHECK(master!=0);
    CHECK(kh_audio_test_pass(g,engine,mix,&oc)==1); int p0=tx.process; CHECK(p0>=1);
    static FxSetter fs; memset(&fs,0,sizeof fs); fs.voice=master; fs.blob=ga_alloc(16,16); float gnew=0.25f; memcpy(hp(g,fs.blob),&gnew,4);
    tx.slow_ms=60; atomic_store(&tx.setp,0); atomic_store(&tx.overlap,0);
    kh_spawn_hostfn_thread(g,"audio_fx_setter",fx_setter_thread,(u64)(uintptr_t)&fs);
    CHECK(kh_audio_test_pass(g,engine,mix,&oc)==1); CHECK(tx.process==p0+1);                       /* 60 ms inside the XAPO's Process */
    for(int i=0;i<3000&&!atomic_load(&fs.done);i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    CHECK(atomic_load(&fs.done)==1&&fs.hr==0); CHECK(atomic_load(&tx.setp)==1); CHECK(atomic_load(&tx.overlap)==0);
    CHECK(atomic_load(&fs.took_ns)>=25000000ull);                                                  /* it waited for Process instead of running through it */
    CHECK(tx.gain==0.25f);                                                                         /* and was delivered */
    tx.slow_ms=0; u64 t=kh_host_ns(); CHECK((u32)call8(g,master,6,0,fs.blob,4,0,0,0,0)==0); CHECK(kh_host_ns()-t<20000000ull);   /* no Process running: at once */
    CHECK(vc(g,master,18,0,0,0)==0); vc(g,engine,2,0,0,0);
    kh_audio_test_mode(prev); ga_free(out); ga_free(fs.blob);
    fprintf(stderr,"kh: audio fx serialisation selftest: %d checks, %d failures\n",checks-c0,fails-f0);
    return fails-f0;
}

/* ---- TASK28: a voice takes part in the processing passes only once its creation is complete ------------------------------------------
 * The boot failure "the mastering voice produces 8 channels; only a stereo device is implemented" (1 of 5 software-mixer starts in task
 * 27, 1 of 100 / 3 of 60 on one CPU in task 28): the engine creates its mastering voice with 8 input channels and an effect chain that
 * folds 8 -> 6 -> 2.  The runtime created the mixer voice first and then hosted the chain's XAPOs (QueryInterface, registration
 * properties, format checks, LockForProcess: guest calls); a pass of the mixer thread that came in between read the voice without its
 * chain and produced 8 channels.  The same window let a pass call the voice callbacks of a source voice whose CreateSourceVoice had not
 * returned yet.  These tests FORCE that interleaving: a pass is run inside the window, (A) by the test itself, (B) by the real mixer
 * thread (two passes of its own clock), and must see neither the half-built mastering voice nor the half-built source voice.
 * With the fix compiled out (-DKH_M28_VISIBLE_AT_ONCE_MUTANT) A fails 2 checks and B ends the process with the boot failure's FATAL
 * (m28/selftest-mutant.out). */
void kh_audio_test_mid_create(void (*fn)(KCtx*,int)); int kh_audio_test_stop_mixer(void);
static void sleep_ms_test(int ms){ struct timespec ts={0,(long)ms*1000000L}; nanosleep(&ts,0); }
static u64 mc_engine, mc_q0; static int mc_calls, mc_oc[8], mc_cb[8], mc_waited[8]; static float mc_mix[480*8];
static void mid_create_manual(KCtx *g,int slot){ (void)slot; if(mc_calls<8){ u32 oc=99; int n0=ncb; kh_audio_test_pass(g,mc_engine,mc_mix,&oc); mc_oc[mc_calls]=(int)oc; mc_cb[mc_calls]=ncb-n0; } mc_calls++; }
static void mid_create_thread(KCtx *g,int slot){ (void)g; (void)slot;                  /* hold the creating thread until the mixer thread has made two passes */
    u64 q=kh_audio_test_quanta(); int ok=0; for(int i=0;i<2000&&!ok;i++){ if(kh_audio_test_quanta()>=q+2) ok=1; else { struct timespec ts={0,1000000}; nanosleep(&ts,0); } }
    if(mc_calls<8) mc_waited[mc_calls]=ok; mc_calls++; }
int kh_audio_creation_selftest(KCtx *g){
    int c0=checks, f0=fails; memset(&tx,0,sizeof tx);
    if(!C_tx){ C_tx=kh_com_class("xa2:test-IXAPO",13,COM_STRICT,h_tx); C_txp=kh_com_class("xa2:test-IXAPOParameters",5,COM_STRICT,h_txp); }
    if(!C_vcb) C_vcb=kh_com_class("xa2:test-IXAudio2VoiceCallback",7,COM_STRICT|COM_NOUNK,h_vcb);
    u64 out=ga_calloc(1,128); u32 oc=0;
    /* A: passes driven by the test (manual mode) */
    int prev=kh_audio_test_mode(1);
    imp(g,rt_imp_xaudio2_9__ord1,out); mc_engine=rd64(g,out); CHECK(mc_engine!=0);
    u64 xapo=kh_com_obj(C_tx,16); tx.gain=1.0f; tx.flags=0;                                          /* 8 -> 2: sums its inputs (like the engine's 8 -> 6 -> 2) */
    u64 objs[1]={xapo}; u32 ini[1]={1},och[1]={2}; u64 chain=mk_chain(g,1,objs,ini,och);
    mc_calls=0; memset(mc_oc,0,sizeof mc_oc); kh_audio_test_mid_create(mid_create_manual);
    CHECK((u32)call8(g,mc_engine,7,out,8,48000,0,0,chain,0)==0); u64 master=rd64(g,out); CHECK(master!=0);
    CHECK(mc_calls==1); CHECK(mc_oc[0]==0);                                                         /* the pass inside CreateMasteringVoice: no device output yet */
    CHECK(tx.lock_in_ch==8&&tx.lock_out_ch==2);
    kh_audio_test_mid_create(NULL); CHECK(kh_audio_test_pass(g,mc_engine,mc_mix,&oc)==1&&oc==2);  /* complete: stereo from the next pass on */
    u64 vcb=kh_com_obj(C_vcb,0), fmt=mk_pcm_format(g,2,48000,16);
    kh_audio_test_mid_create(mid_create_manual); ncb=0;
    CHECK(create_source(g,mc_engine,out,fmt,vcb,0,0)==0); u64 src=rd64(g,out); CHECK(src!=0);
    CHECK(mc_calls==2); CHECK(mc_cb[1]==0);                                                         /* no callback for a voice whose CreateSourceVoice has not returned */
    kh_audio_test_mid_create(NULL); ncb=0; kh_audio_test_pass(g,mc_engine,mc_mix,&oc); CHECK(!strcmp(cbseq(vcb),"01"));
    CHECK(vc(g,src,18,0,0,0)==0); CHECK(vc(g,master,18,0,0,0)==0); vc(g,mc_engine,2,0,0,0);
    fprintf(stderr,"kh: audio creation selftest, passes driven by the test: %d checks, %d failures\n",checks-c0,fails-f0);
    /* B: the real mixer thread (host clock) makes two passes while CreateMasteringVoice is between its voice and its chain */
    kh_audio_test_mode(2); u64 q0=kh_audio_test_quanta(); (void)mc_q0;
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(engine!=0);
    for(int i=0;i<2000&&kh_audio_test_quanta()==q0;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    mc_calls=0; memset(mc_waited,0,sizeof mc_waited); kh_audio_test_mid_create(mid_create_thread);
    CHECK((u32)call8(g,engine,7,out,8,48000,0,0,chain,0)==0); master=rd64(g,out); CHECK(master!=0);
    kh_audio_test_mid_create(NULL);
    CHECK(mc_calls==1&&mc_waited[0]==1);                                                            /* two passes of the mixer thread ran inside the window ... */
    u64 q1=kh_audio_test_quanta(); for(int i=0;i<2000&&kh_audio_test_quanta()<q1+3;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    CHECK(kh_audio_test_quanta()>=q1+3);                                                            /* ... and the thread goes on (no FATAL) with the stereo master */
    CHECK(kh_audio_test_stop_mixer()==1);
    CHECK(vc(g,master,18,0,0,0)==0); vc(g,engine,2,0,0,0);
    kh_audio_test_mode(prev); free_chain(g,chain); ga_free(fmt); ga_free(out);
    fprintf(stderr,"kh: audio creation selftest: %d checks, %d failures\n",checks-c0,fails-f0);
    return fails-f0;
}

/* ---- TASK28: KHNMC_AUDIO=pull, the mixer thread follows the device (khnmc_audio_out.h) ------------------------------------------------
 * With the host clock: no device = passes on the host clock, discarded and counted; a device that pulls as fast as the ring allows gets
 * 2 s of audio in far less than 2 s of host time (the passes follow its consumption, not CLOCK_MONOTONIC), no frame is lost or
 * repeated (produced = delivered + what is still in the ring), the ring never holds more than the target fill; a device at real time
 * keeps the fill at the target; after kh_audio_out_stop the mixer is back on the host clock. */
void kh_audio_test_pull(int on);
int kh_audio_pull_selftest(KCtx *g){
    int c0=checks, f0=fails; u64 out=ga_calloc(1,128);
    int prev=kh_audio_test_mode(2); kh_audio_test_pull(1);
    imp(g,rt_imp_xaudio2_9__ord1,out); u64 engine=rd64(g,out); CHECK(engine!=0);
    CHECK((u32)call8(g,engine,7,out,2,48000,0,0,0,0)==0); u64 master=rd64(g,out); CHECK(master!=0);
    KhAudioOutStats a,b; KhAudioOutFormat f; kh_audio_out_format(&f); CHECK(f.target_frames==KH_AUDIO_OUT_TARGET_FRAMES&&f.ring_frames==KH_AUDIO_OUT_RING_FRAMES);   /* 2048 / 16384 (review of task 28: was 8192) */
    { struct timespec ts={0,150000000}; nanosleep(&ts,0); }
    kh_audio_out_stats(&a); CHECK(a.stopped_passes>=8&&a.stopped_passes<=40&&a.produced_frames==0);      /* no device: ~15 host-clock passes in 150 ms, discarded */
    CHECK(kh_audio_out_start()==0);
    static float buf[1024*2]; u64 t0=kh_host_ns(), got=0;
    while(got<96000&&kh_host_ns()-t0<3000000000ull){ got+=kh_audio_out_pull(buf,1024); struct timespec ts={0,100000}; nanosleep(&ts,0); }
    u64 dt=kh_host_ns()-t0; kh_audio_out_stats(&b);
    CHECK(got>=96000); CHECK(dt<1500000000ull);                                                     /* 2 s of audio, paced by the device, not by the host clock */
    CHECK(b.overruns==0); CHECK(b.produced_frames==b.delivered_frames+kh_audio_ring_fill());        /* nothing lost, nothing repeated */
    CHECK(b.max_fill<=f.target_frames);
    u64 t1=kh_host_ns(); for(int i=0;i<30;i++){ kh_audio_out_pull(buf,480); sleep_ms_test(10); }      /* a device at real time */
    u32 fill=kh_audio_ring_fill(); CHECK(fill+480>=f.target_frames-480&&fill<=f.target_frames); (void)t1;
    kh_audio_out_stats(&a); kh_audio_out_stop();
    { struct timespec ts={0,100000000}; nanosleep(&ts,0); }
    kh_audio_out_stats(&b); CHECK(b.stopped_passes>=a.stopped_passes+5);                            /* stopped: back on the host clock, discarded */
    CHECK(kh_audio_test_stop_mixer()==1);
    CHECK(vc(g,master,18,0,0,0)==0); vc(g,engine,2,0,0,0);
    kh_audio_test_pull(0); kh_audio_test_mode(prev); ga_free(out);
    fprintf(stderr,"kh: audio pull selftest: %d checks, %d failures (2 s of audio pulled in %.0f ms of host time; %llu underruns while the device ran ahead)\n",checks-c0,fails-f0,(double)dt/1e6,(unsigned long long)b.underruns);
    return fails-f0;
}
