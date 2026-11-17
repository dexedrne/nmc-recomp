#include "khnmc_rt.h"
/* Scripted XInput pad 0; other input backends remain empty. Ranges are inclusive;
 * gaps release all controls, and later lines win when ranges overlap. */
#include <errno.h>
#include <unistd.h>
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
/* `@hold FRAME until initial-state N` : script frames >= FRAME are not sampled before the Initial Settings screen has reached state N.
 * The script clock is the present counter, but the game thread's progress per present depends on the machine's load (the render
 * thread keeps presenting while the game thread boots): a confirm press scripted for present 180 misses a screen that only reaches
 * its prompt at present 186, and the route then loops in the settings menu for the whole run.  With a hold the script waits at
 * FRAME-1 and is delayed by exactly the lateness; a run that reaches the state in time is sampled exactly as without the line. */
static long long hold_frame=-1; static int hold_state=-1; static _Atomic long long hold_shift; static _Atomic int hold_released;
/* `@clock game-frames` (needs a hold): after the hold is released the script advances with the GAME thread's frames instead of
 * presents.  A game frame = one call of 0x105e10, which the end-of-frame task 0x1548e0 / 0x154740 makes once per frame and the
 * settings menu's own loop (0x4edef9, nested inside one such call) once per iteration.  One game frame is one present on
 * an idle machine, so a script written in presents keeps its meaning; on a loaded machine the game thread makes fewer frames than
 * the render thread presents, and a script clocked by presents runs ahead of the menus it is driving. */
static int clock_game; static _Atomic u64 game_frames; static _Atomic long long clock_sf0, clock_gf0; static _Atomic u64 last_script_frame;
/* KH_RUN_GAME_FRAMES=n ends the run (exit 0, with the reports) when the game thread has made n frames: a route length that does not
 * depend on the machine's load, unlike KH_RUN_SECONDS, which then only serves as the watchdog. */
static u64 run_game_frames;
u64 kh_game_frames(void){ return atomic_load(&game_frames); }      /* frames the game thread has made (diagnostic logs) */
void orig_f_105e10(KCtx *g);
void f_105e10(KCtx *g){
    u64 n=atomic_fetch_add(&game_frames,1)+1;
    { extern void kh_idtable_game_frame(u64); kh_idtable_game_frame(n); extern void kh_moose_game_frame(KCtx *); kh_moose_game_frame(g); }
    if(run_game_frames && n>=run_game_frames){ extern void kh_exit_process(int); kh_warn("input: KH_RUN_GAME_FRAMES=%llu reached, ending the run",(unsigned long long)run_game_frames); kh_exit_process(0); }
    orig_f_105e10(g);
}
long long kh_phase_initial_first(int state);
static u64 script_frame(u64 present){
    if(hold_frame<0) return present;
    if(!atomic_load(&hold_released)){
        long long first=kh_phase_initial_first(hold_state);
        if(first<0) return present<(u64)hold_frame?present:(u64)hold_frame-1;
        long long shift=first>hold_frame-1?first-(hold_frame-1):0; int z=0;
        atomic_store(&hold_shift,shift);
        atomic_store(&clock_sf0,(long long)present-shift>0?(long long)present-shift:0); atomic_store(&clock_gf0,(long long)atomic_load(&game_frames));
        if(atomic_compare_exchange_strong(&hold_released,&z,1))
            fprintf(stderr,"kh: scripted XInput: hold at script frame %lld released: InitialSetting state %d first seen at present %lld, script delayed by %lld presents%s\n",hold_frame,hold_state,first,shift,clock_game?"; from here the script follows game frames":"");
    }
    if(clock_game){ long long f=atomic_load(&clock_sf0)+((long long)atomic_load(&game_frames)-atomic_load(&clock_gf0)); return f>0?(u64)f:0; }
    long long sh=atomic_load(&hold_shift); return present>(u64)sh?present-(u64)sh:0;
}
u64 kh_input_game_frames(void){ return atomic_load(&game_frames); }
void kh_input_report(FILE *f){
    if(!scripted) return;
    fprintf(f,"kh: input report: presents=%llu game-frames=%llu last-script-frame=%llu clock=%s hold=%s\n",(unsigned long long)atomic_load(&gfx_presents),(unsigned long long)atomic_load(&game_frames),
            (unsigned long long)atomic_load(&last_script_frame),clock_game?"game-frames":"presents",hold_frame<0?"none":atomic_load(&hold_released)?"released":"NEVER RELEASED (the awaited state was not reached)");
}
int kh_input_hold_pending(void){ return scripted && hold_frame>=0 && !atomic_load(&hold_released); }
void kh_input_load(KCtx *g,const char *path){
    free(ranges); ranges=NULL; nranges=0; scripted=path!=NULL; packet=0; memset(&last_pad,0,sizeof last_pad);
    hold_frame=-1; hold_state=-1; atomic_store(&hold_shift,0); atomic_store(&hold_released,0); clock_game=0;
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
        if(!strcmp(words[0],"@hold")){
            long long hf,hs;
            if(n!=5||strcmp(words[2],"until")||strcmp(words[3],"initial-state")||!number(words[1],1,LLONG_MAX,&hf)||!number(words[4],0,63,&hs)||hold_frame>=0) goto invalid;
            hold_frame=hf; hold_state=(int)hs; continue;
        }
        if(!strcmp(words[0],"@clock")){ if(n!=2||strcmp(words[1],"game-frames")||clock_game) goto invalid; clock_game=1; continue; }
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
    if(clock_game && hold_frame<0) kh_fatal(g,"scripted XInput: %s: @clock game-frames needs an @hold line (the point from which game frames are counted)",path);
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
static void input_get(KCtx *g){ kh_cnt_input_calls++; u64 sf=script_frame(atomic_load(&gfx_presents)); atomic_store(&last_script_frame,sf); SET_RAX(g,kh_input_state(g,(u32)A0(g),A1(g),sf)); }
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
    { const char *e=getenv("KH_RUN_GAME_FRAMES"); if(e&&*e) run_game_frames=strtoull(e,0,0); }
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

/* selftest of the `@hold` directive: the script clock before, at and after the awaited state, late and early */
int kh_input_hold_selftest(KCtx *g){
    int checks=0,fails=0;
#define ICHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"input hold selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    void kh_phase_test_set_initial_first(int state,long long present);
    long long saved=kh_phase_initial_first(3);
    char path[]="/tmp/khnmc-hold-XXXXXX"; int fd=mkstemp(path); ICHECK(fd>=0); if(fd<0) return fails;
    const char *txt="# hold test\n@hold 165 until initial-state 3\n180 182 A\n"; ICHECK(write(fd,txt,strlen(txt))==(ssize_t)strlen(txt)); close(fd);
    u64 out=ga_alloc(32,16);
    kh_phase_test_set_initial_first(3,-1); kh_input_load(g,path);
    ICHECK(kh_input_hold_pending()==1);
    ICHECK(script_frame(100)==100 && script_frame(164)==164 && script_frame(165)==164 && script_frame(5000)==164);   /* held */
    ICHECK(kh_input_state(g,0,out,script_frame(182))==0 && rd16(g,out+4)==0);                                         /* A at 180 is not sampled yet */
    kh_phase_test_set_initial_first(3,186);                                                                             /* the prompt comes up late */
    ICHECK(script_frame(186)==164 && script_frame(187)==165 && script_frame(202)==180 && script_frame(1000)==978);
    ICHECK(kh_input_hold_pending()==0);
    ICHECK(kh_input_state(g,0,out,script_frame(201))==0 && rd16(g,out+4)==0);
    ICHECK(kh_input_state(g,0,out,script_frame(202))==0 && rd16(g,out+4)==0x1000);                                    /* A, 16 presents after the prompt */
    kh_phase_test_set_initial_first(3,101); kh_input_load(g,path);                                                      /* the prompt is up in time: nothing changes */
    ICHECK(script_frame(101)==101 && script_frame(164)==164 && script_frame(165)==165 && script_frame(180)==180 && kh_input_hold_pending()==0);
    kh_input_load(g,NULL); ICHECK(script_frame(165)==165 && script_frame(9999)==9999);                                  /* no script, no hold */
    /* @clock game-frames: after the release the script follows the game thread's frames, whatever the present counter does */
    { char p2[]="/tmp/khnmc-hold-XXXXXX"; int f2=mkstemp(p2); ICHECK(f2>=0);
      if(f2>=0){ const char *t2="@hold 165 until initial-state 3\n@clock game-frames\n180 182 A\n"; ICHECK(write(f2,t2,strlen(t2))==(ssize_t)strlen(t2)); close(f2);
        u64 sv_gf=atomic_load(&game_frames);
        kh_phase_test_set_initial_first(3,-1); kh_input_load(g,p2); atomic_store(&game_frames,1000);
        ICHECK(script_frame(100)==100 && script_frame(400)==164);                                                       /* before the release: presents, held */
        kh_phase_test_set_initial_first(3,304); ICHECK(script_frame(304)==164);                                         /* released at present 304, game frame 1000 */
        atomic_store(&game_frames,1010); ICHECK(script_frame(600)==174 && script_frame(9000)==174);                     /* 10 game frames later, however many presents */
        atomic_store(&game_frames,1016); ICHECK(script_frame(601)==180 && kh_input_state(g,0,out,script_frame(601))==0 && rd16(g,out+4)==0x1000);
        kh_phase_test_set_initial_first(3,101); kh_input_load(g,p2); atomic_store(&game_frames,50);                     /* in time: anchored at the present of the first poll after the sighting */
        ICHECK(script_frame(120)==120); atomic_store(&game_frames,110); ICHECK(script_frame(500)==180);
        atomic_store(&game_frames,sv_gf); unlink(p2); } }
    unlink(path); ga_free(out); kh_phase_test_set_initial_first(3,saved);
    kh_input_load(g,getenv("KHNMC_INPUT_SCRIPT"));
    fprintf(stderr,"kh: input hold selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}
