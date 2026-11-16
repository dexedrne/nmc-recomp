/* Headless device discovery and XAudio2: valid voices, no rendering or callbacks.
 * shortcut: buffers report drained without callbacks; add a render clock for playback. */
#include "khnmc_rt.h"
static int C_xa2,C_src,C_sub,C_mast,C_xapo,C_enum,C_device,C_client;
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
typedef struct { u32 flags, count; Effect *effects; u32 channels[64]; Matrix *matrices; } Voice;
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
static int h_xa2(KCtx *g,u64 o,int s,const char *c){
    (void)o;
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
        if(!effect_chain(g,state,ARG(g,s==5?8:s==6?7:6))) kh_fatal(g,"null audio: invalid creation effect chain");
        wr64(g,A1(g),v); break;
    }
    case 11: memset(hp(g,A1(g)),0,64); return 1; /* XAUDIO2_PERFORMANCE_DATA is 64 bytes. */
    default: missing(g,c,s<13?engine_methods[s]:"unknown slot");
    }
    SET_RAX(g,0); return 1;
}
static int h_voice(KCtx *g,u64 o,int s,const char *c){
    int source=kh_com_cls_of(g,o)==C_src;
    Voice *v=voice_state(g,o);
    if(!v) missing(g,c,"method on destroyed voice");
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
    case 19: case 20: case 21: case 22: case 23: case 24: break; /* documented drained null source */
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
static void xa2_create(KCtx *g){
    kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); KH_RET(g); return; }
    wr64(g,A0(g),kh_com_obj(C_xa2,16)); SET_RAX(g,0); KH_RET(g);
}
void kh_seam_audio_init(void){
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
IMP(xaudio2_9,ord1){ kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); return; } wr64(g,A0(g),kh_com_obj(C_xa2,16)); SET_RAX(g,0); }
IMP(xaudio2_9,ord2){ kh_cnt_audio_calls++; wr64(g,A0(g),kh_com_obj(C_xapo,16)); SET_RAX(g,0); }
IMP(xaudio2_9,ord3){ kh_cnt_audio_calls++; if(!A0(g)){ SET_RAX(g,0x80004003u); return; } u64 o=kh_com_obj(C_xapo,16); wr32(g,o+32,3); wr64(g,A0(g),o); SET_RAX(g,0); }
