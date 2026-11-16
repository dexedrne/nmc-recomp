#include "khnmc_rt.h"
/* Scripted XInput pad 0; other input backends remain empty. Ranges are inclusive;
 * gaps release all controls, and later lines win when ranges overlap. */
#include <errno.h>
#include <limits.h>
typedef struct { u16 buttons; u8 lt,rt; s16 lx,ly,rx,ry; } ScriptPad;
_Static_assert(sizeof(ScriptPad)==12,"XINPUT_GAMEPAD layout");
typedef struct { u64 start,end; ScriptPad pad; } InputRange;
static InputRange *ranges;
static size_t nranges;
static int scripted;
static ScriptPad last_pad;
static u32 packet;
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
extern _Atomic u64 gfx_presents;
static const char *button_names[]={"DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT","START","BACK","LSTICK","RSTICK","LB","RB",NULL,NULL,"A","B","X","Y","LT","RT"};
static int number(const char *s,long long lo,long long hi,long long *v){
    char *end; errno=0; *v=strtoll(s,&end,10); return !errno && end!=s && !*end && *v>=lo && *v<=hi;
}
void kh_input_load(KCtx *g,const char *path){
    free(ranges); ranges=NULL; nranges=0; scripted=path!=NULL; packet=0; memset(&last_pad,0,sizeof last_pad);
    if(!path) return;
    FILE *f=fopen(path,"r"); if(!f) kh_fatal(g,"scripted XInput: cannot open %s",path);
    char line[1024]; unsigned lineno=0;
    while(fgets(line,sizeof line,f)){
        lineno++;
        if(strlen(line)==sizeof line-1 && line[sizeof line-2]!='\n') goto invalid;
        char *comment=strchr(line,'#'); if(comment) *comment=0;
        char *words[6],*save; int n=0;
        for(char *w=strtok_r(line," \t\r\n",&save);w;w=strtok_r(NULL," \t\r\n",&save)){
            if(n==6) goto invalid; words[n++]=w;
        }
        if(!n) continue;
        if(n!=3 && n!=5) goto invalid;
        long long a,b,x=0,y=0;
        if(!number(words[0],0,LLONG_MAX,&a)||!number(words[1],a,LLONG_MAX,&b)) goto invalid;
        if(n==5 && (!number(words[3],-32768,32767,&x)||!number(words[4],-32768,32767,&y))) goto invalid;
        ScriptPad pad={.lx=x,.ly=y};
        if(strcmp(words[2],"-")){
            char *buttons=words[2]; size_t len=strlen(buttons);
            if(buttons[0]=='+'||buttons[len-1]=='+'||strstr(buttons,"++")) goto invalid;
            char *bs; for(char *w=strtok_r(buttons,"+",&bs);w;w=strtok_r(NULL,"+",&bs)){
                int bit; for(bit=0;bit<18;bit++) if(button_names[bit]&&!strcmp(w,button_names[bit])) break;
                if(bit==18) goto invalid;
                if(bit==16) pad.lt=255; else if(bit==17) pad.rt=255; else pad.buttons|=1u<<bit;
            }
        }
        InputRange *next=realloc(ranges,(nranges+1)*sizeof *ranges);
        if(!next) kh_fatal(g,"scripted XInput: allocation failed"); ranges=next;
        ranges[nranges++]=(InputRange){a,b,pad};
        continue;
invalid: kh_fatal(g,"scripted XInput: invalid %s line %u",path,lineno);
    }
    if(ferror(f)) kh_fatal(g,"scripted XInput: read failed %s",path);
    fclose(f); kh_warn("scripted XInput pad 0: %zu ranges from %s",nranges,path);
}
u32 kh_input_state(KCtx *g,u32 slot,u64 out,u64 frame){
    if(!scripted||slot) return 1167;
    if(!out) return 87;
    ScriptPad pad={0};
    pthread_mutex_lock(&input_lock);
    for(size_t i=0;i<nranges;i++) if(frame>=ranges[i].start && frame<=ranges[i].end) pad=ranges[i].pad;
    if(!packet || memcmp(&last_pad,&pad,sizeof pad)){ packet++; last_pad=pad; }
    wr32(g,out,packet); wr16(g,out+4,pad.buttons); wr8(g,out+6,pad.lt); wr8(g,out+7,pad.rt);
    wr16(g,out+8,(u16)pad.lx); wr16(g,out+10,(u16)pad.ly); wr16(g,out+12,0); wr16(g,out+14,0);
    pthread_mutex_unlock(&input_lock); return 0;
}
static void input_get(KCtx *g){ kh_cnt_input_calls++; SET_RAX(g,kh_input_state(g,(u32)A0(g),A1(g),atomic_load(&gfx_presents))); }
IMP(xinput1_4,XInputGetState){ input_get(g); }
IMP(xinput1_4,XInputGetStateEx){ input_get(g); }
IMP(xinput1_4,ord100){ input_get(g); }
static int C_di,C_dev;
static int h_di(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==3){ wr64(g,A2(g),kh_com_obj(C_dev,16)); SET_RAX(g,0); return 1; }          /* CreateDevice(guid, **ppDevice, outer) */
    SET_RAX(g,0); return 1; }                                                          /* EnumDevices etc.: S_OK, no callbacks */
static int h_dev(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==9){ memset(hp(g,A2(g)),0,(size_t)(u32)A1(g)); SET_RAX(g,0); return 1; }    /* GetDeviceState(cb, data): all zero */
    SET_RAX(g,0); return 1; }
void kh_seam_input_init(void){ kh_input_load(NULL,getenv("KHNMC_INPUT_SCRIPT"));
    if(scripted){
        u64 get=kh_host_fn("XInputGetState",rt_imp_xinput1_4__XInputGetState);
        const char *aliases[]={"XInputGetState","XInputGetStateEx","#2","#100"};
        for(size_t i=0;i<sizeof aliases/sizeof *aliases;i++) kh_register_proc("xinput1_4.dll",aliases[i],get);
    }
    C_di=kh_com_class("di:IDirectInput8",16,0,h_di); C_dev=kh_com_class("di:IDirectInputDevice8",40,0,h_dev); }
IMP(xinput1_4,ord2){ input_get(g); }
IMP(xinput1_4,ord3){ kh_cnt_input_calls++; SET_RAX(g,scripted && (u32)A0(g)==0?0:1167); }
IMP(dinput8,DirectInput8Create){ kh_cnt_input_calls++; wr64(g,A3(g),kh_com_obj(C_di,16)); SET_RAX(g,0); }
IMP(setupapi,SetupDiGetClassDevsW){ kh_cnt_input_calls++; SET_RAX(g,(u64)-1); kh_set_last_error(13); }
IMP(setupapi,SetupDiEnumDeviceInterfaces){ kh_cnt_input_calls++; kh_set_last_error(259); SET_RAX(g,0); }
IMP(setupapi,SetupDiDestroyDeviceInfoList){ kh_cnt_input_calls++; SET_RAX(g,1); }
IMP(hid,HidD_GetHidGuid){ kh_cnt_input_calls++; memset(hp(g,A0(g)),0,16); }

/* The real probe is reached from DInput enumeration, absent on a null host.
 * Supply the known virtual candidate and bypass WMI only for that product id. */
#define SCRIPT_PRODUCT 0x028e045e
void orig_f_131ef0(KCtx *g);
void f_131ef0(KCtx *g){
    if(scripted && (u32)A1(g)==SCRIPT_PRODUCT){ SET_RAX(g,1); KH_RET(g); return; }
    orig_f_131ef0(g);
}
void kh_input_attach(KCtx *g){
    if(!scripted) return;
    u64 pad=GS_BASE+0x8bb2a8;
    wr32(g,pad+0x48,SCRIPT_PRODUCT);
    memset(hp(g,pad+0x50),0xff,24); /* identity profile for the virtual candidate */
    kh_call(g,GS_BASE+0x133970,GS_BASE+0x8bb290,pad,0,0);
    if((s32)rd32(g,pad+0x10)!=0 || !rd64(g,pad+0xb8)) kh_fatal(g,"scripted XInput: native backend installation failed");
    kh_warn("scripted XInput: native pad 0 backends installed");
}
