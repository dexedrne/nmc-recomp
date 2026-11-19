/* selftest.c : runtime self-tests that need no game data beyond the mapped image: allocator, qsort/bsearch, printf engine, wait objects,
 * critical sections across threads, fibers (switch, resume on another thread), the two yield hooks.  `khnmc --selftest`. */
#include "khnmc_rt.h"
#include "settings.h"
#include <unistd.h>

void rt_hook_fiber_yield_150060(KCtx *g); void rt_hook_fiber_yield_150080(KCtx *g);
extern void rt_imp_api_ms_win_crt_utility_l1_1_0__qsort(KCtx *g); extern void rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch(KCtx *g);
extern void rt_imp_api_ms_win_crt_stdio_l1_1_0____stdio_common_vsprintf(KCtx *g);
extern void rt_imp_kernel32__EnterCriticalSection(KCtx *g); extern void rt_imp_kernel32__LeaveCriticalSection(KCtx *g); extern void rt_imp_kernel32__InitializeCriticalSection(KCtx *g);
extern void rt_imp_kernel32__SwitchToFiber(KCtx *g);
extern void rt_imp_ole32__CoCreateInstance(KCtx *g);
static int fails, checks;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)

/* a guest-callable int comparator implemented on the host: compares two u32 */
static void cmp_u32(KCtx *g){ u32 a=rd32(g,KH_RCX(g)),b=rd32(g,KH_RDX(g)); SET_RAX(g,(u32)(a<b?-1:a>b?1:0)); KH_RET(g); }
static void do_call(KCtx *g,void (*imp)(KCtx*),u64 a0,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5){
    u64 save=KH_RSP(g); u64 sp=(save-0x200)&~0xFull; wr64(g,sp+0x20,a4); wr64(g,sp+0x28,a5); sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3; imp(g); KH_RSP(g)=save; }

extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch(KCtx *g);
extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___lseeki64(KCtx *g);
extern void rt_imp_api_ms_win_crt_stdio_l1_1_0___read(KCtx *g);
static int io_selftest(KCtx *g){
    int fails=0,checks=0; char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"khnmc-io-XXXXXX");
    int fd=mkstemp(path); if(fd<0) return 1;
    const u64 offset=0x100000123ull;
    if(pwrite(fd,"seek",4,offset)!=4){ close(fd); unlink(path); return 1; } close(fd);
    u64 name=ga_strdup(path),out=ga_alloc(16,16),buf=ga_alloc(16,16);
    for(int secure=0;secure<2;secure++){
        wr32(g,out,-1);
        do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch,name,0x8000,0x40,0,out,secure);
        u64 result=KH_RAX(g); int opened=(s32)rd32(g,out);
        CHECK(result==0&&opened>=0);
        if(opened<0){ if((s64)result>0) close(result); continue; }
        do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0___lseeki64,opened,offset,SEEK_SET,0,0,0);
        CHECK(KH_RAX(g)==offset);
        do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0___read,opened,buf,4,0,0,0);
        CHECK(KH_RAX(g)==4&&!memcmp(hp(g,buf),"seek",4)); close(opened);
    }
    unlink(path); wr32(g,out,42);
    do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0___sopen_dispatch,name,0x8000,0x40,0,out,0);
    CHECK(KH_RAX(g)==2&&(s32)rd32(g,out)==-1);
    ga_free(name); ga_free(out); ga_free(buf);
    fprintf(stderr,"kh: io selftest: %d checks, %d failures\n",checks,fails); return fails;
}

/* Exercise the actual guest vtables and Win64 stack arguments. */
static u64 audio_call8(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5,u64 a6,u64 a7){
    u64 saved[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(saved[0]-0x200)&~15ull;
    memset(hp(g,sp),0,0x80); wr64(g,sp+0x20,a4); wr64(g,sp+0x28,a5); wr64(g,sp+0x30,a6); wr64(g,sp+0x38,a7);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp;
    KH_RCX(g)=obj; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    kh_dispatch(g,rd64(g,rd64(g,obj)+slot*8));
    u64 result=KH_RAX(g);
    KH_RSP(g)=saved[0]; KH_RCX(g)=saved[1]; KH_RDX(g)=saved[2]; KH_R8(g)=saved[3]; KH_R9(g)=saved[4];
    return result;
}
static u64 audio_call6(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3,u64 a4,u64 a5){ return audio_call8(g,obj,slot,a1,a2,a3,a4,a5,0,0); }
static u64 audio_call(KCtx *g,u64 obj,int slot,u64 a1,u64 a2,u64 a3){
    return audio_call6(g,obj,slot,a1,a2,a3,0,0);
}
static int audio_selftest(KCtx *g){
    int fails=0,checks=0;
    extern int kh_audio_test_mode(int); int saved_mode=kh_audio_test_mode(0);   /* these checks describe the null backend, whatever KHNMC_AUDIO says */
    static const u8 mmcls[16]={0x95,0x03,0xde,0xbc,0x2f,0xe5,0x7c,0x46,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e};
    u64 clsid=ga_alloc(16,16),out=ga_calloc(1,96);
    memcpy(hp(g,clsid),mmcls,16);
    do_call(g,rt_imp_ole32__CoCreateInstance,clsid,0,1,GS_BASE+0x61be48,out,0);
    CHECK(KH_RAX(g)==0 && rd64(g,out)!=0);
    if(fails) goto done;
    u64 enumerator=rd64(g,out);
    if(getenv("KH_SELFTEST_AUDIO_FATAL")) audio_call(g,enumerator,3,0,1,out);
    CHECK(audio_call(g,enumerator,4,0,0,0)==0x80004003u);
    CHECK(audio_call(g,enumerator,4,0,0,out)==0 && rd64(g,out)!=0);
    u64 device=rd64(g,out);
    CHECK(audio_call(g,device,5,out,0,0)==0 && rd16(g,rd64(g,out))=='k');
    ga_free(rd64(g,out));
    CHECK(kh_call5(g,rd64(g,rd64(g,device)+24),device,GS_BASE+0x61be28,1,0,out)==0 && rd64(g,out)!=0);
    u64 client=rd64(g,out);
    CHECK(audio_call(g,client,8,out,0,0)==0 && rd64(g,out)!=0);
    u64 fmt=rd64(g,out);
    CHECK(rd16(g,fmt)==0xfffe && rd16(g,fmt+2)==2 && rd32(g,fmt+4)==48000);
    CHECK(rd32(g,fmt+8)==384000 && rd16(g,fmt+12)==8 && rd16(g,fmt+14)==32 && rd16(g,fmt+16)==22);
    CHECK(rd16(g,fmt+18)==32 && rd32(g,fmt+20)==3 && rd32(g,fmt+24)==3 && rd64(g,fmt+32)==0x719b3800aa000080ull);
    CHECK(audio_call(g,client,4,out,0,0)==0 && rd32(g,out)==480);
    CHECK(audio_call(g,client,3,0,0,0)==0);
    extern void rt_imp_kernel32__LoadLibraryExW(KCtx*),rt_imp_kernel32__GetProcAddress(KCtx*);
    do_call(g,rt_imp_kernel32__LoadLibraryExW,GS_BASE+0x61be58,0,0,0,0,0);
    u64 module=KH_RAX(g);
    const char *creators[]={"XAudio2CreateWithVersionInfo","XAudio2Create"};
    for(int i=0;i<2;i++){
        u64 name=ga_strdup(creators[i]);
        do_call(g,rt_imp_kernel32__GetProcAddress,module,name,0,0,0,0);
        u64 creator=KH_RAX(g); CHECK(creator!=0);
        if(creator){ CHECK(kh_call(g,creator,out,0,0,0)==0 && rd64(g,out)!=0); audio_call(g,rd64(g,out),2,0,0,0); }
        ga_free(name);
    }
    extern void rt_imp_xaudio2_9__ord1(KCtx*);
    do_call(g,rt_imp_xaudio2_9__ord1,out,0,0,0,0,0);
    u64 engine=rd64(g,out);
    CHECK(KH_RAX(g)==0 && engine!=0);
    for(int slot=5;slot<=7;slot++){
        CHECK(audio_call(g,engine,slot,out,slot==5?fmt:2,slot==5?0:48000)==0 && rd64(g,out)!=0);
        u64 voice=rd64(g,out);
        memset(hp(g,out),0xff,96);
        audio_call(g,voice,0,out,0,0);
        CHECK(rd32(g,out+8)==2 && rd32(g,out+12)==48000);
        if(slot==5){ audio_call(g,voice,25,out,0,0); CHECK(rd64(g,out)==0 && rd32(g,out+8)==0 && rd64(g,out+16)==0); }
        if(slot==7) CHECK(audio_call(g,voice,19,out,0,0)==0 && rd32(g,out)==3);
        /* Retained parameters must survive caller buffer changes. */
        wr32(g,out,0x3f000000); wr32(g,out+4,0x3e800000);
        CHECK(audio_call(g,voice,14,2,out,0)==0);
        memset(hp(g,out),0,8); audio_call(g,voice,15,2,out,0);
        CHECK(rd32(g,out)==0x3f000000 && rd32(g,out+4)==0x3e800000);
        CHECK(audio_call(g,voice,14,3,out,0)==0x88960001u);
        u64 chain=ga_calloc(1,32), effect=ga_calloc(1,16);
        wr32(g,chain,1); wr64(g,chain+4,effect); wr32(g,effect+8,1); wr32(g,effect+12,2);
        if(slot==5){
            CHECK(audio_call8(g,engine,5,out,fmt,0,0,0,0,chain)==0);
            u64 created=rd64(g,out); wr32(g,out,0);
            CHECK(audio_call(g,created,5,0,out,0)==0 && rd32(g,out)==1);
            audio_call(g,created,18,0,0,0);
        }
        CHECK(audio_call(g,voice,2,chain,0,0)==0);
        wr32(g,out,0x12345678); CHECK(kh_call5(g,rd64(g,rd64(g,voice)+6*8),voice,0,out,4,0xdeadbeef00000000ull)==0);
        wr32(g,out,0); CHECK(audio_call(g,voice,7,0,out,4)==0 && rd32(g,out)==0x12345678);
        audio_call(g,voice,5,0,out,0); CHECK(rd32(g,out)==1);
        CHECK(audio_call(g,voice,4,0,0,0)==0); audio_call(g,voice,5,0,out,0); CHECK(rd32(g,out)==0);
        CHECK(audio_call(g,voice,3,0,0,0)==0); audio_call(g,voice,5,0,out,0); CHECK(rd32(g,out)==1);
        CHECK(audio_call(g,voice,7,1,out,4)==0x88960001u);
        CHECK(audio_call(g,voice,7,0,out,8)==0x88960001u);
        CHECK(audio_call(g,voice,2,0,0,0)==0);
        wr32(g,out,0x3f000000); wr32(g,out+4,0); wr32(g,out+8,0); wr32(g,out+12,0x3e800000);
        CHECK(audio_call6(g,voice,16,voice,2,2,out,0)==0);
        memset(hp(g,out),0xff,16);
        CHECK(audio_call6(g,voice,17,voice,2,2,out,0)==0);
        CHECK(rd32(g,out)==0x3f000000 && rd32(g,out+4)==0 && rd32(g,out+8)==0 && rd32(g,out+12)==0x3e800000);
        CHECK(audio_call6(g,voice,17,voice,2,3,out,0)==0x88960001u);
        *(u32*)&KH_XMM(g,1)=0x3e800000; audio_call(g,voice,12,0,0,0);
        audio_call(g,voice,13,out,0,0); CHECK(rd32(g,out)==0x3e800000);
        extern void rt_imp_xaudio2_9__ord3(KCtx*);
        do_call(g,rt_imp_xaudio2_9__ord3,out,0,0,0,0,0);
        u64 meter=rd64(g,out); wr64(g,effect,meter); wr32(g,effect+8,1);
        CHECK(audio_call(g,voice,2,chain,0,0)==0);
        wr64(g,out,out+32); wr64(g,out+8,out+48); wr32(g,out+16,2);
        memset(hp(g,out+32),0xa5,32);
        CHECK(audio_call(g,voice,7,0,out,20)==0);
        CHECK(rd64(g,out)==out+32 && rd64(g,out+8)==out+48 && rd32(g,out+16)==2);
        CHECK(rd64(g,out+32)==0 && rd64(g,out+48)==0 && rd64(g,out+40)==0xa5a5a5a5a5a5a5a5ull && rd64(g,out+56)==0xa5a5a5a5a5a5a5a5ull);
        wr32(g,out+16,3); CHECK(audio_call(g,voice,7,0,out,20)==0x88960001u);
        audio_call(g,meter,2,0,0,0);
        ga_free(chain); ga_free(effect);
        audio_call(g,voice,18,0,0,0);
    }
    memset(hp(g,out),0xa5,96); audio_call(g,engine,11,out,0,0);
    CHECK(rd64(g,out)==0 && rd64(g,out+56)==0 && rd64(g,out+64)==0xa5a5a5a5a5a5a5a5ull);
    audio_call(g,client,2,0,0,0); audio_call(g,device,2,0,0,0); audio_call(g,enumerator,2,0,0,0); audio_call(g,engine,2,0,0,0);
    extern void rt_imp_ole32__CoTaskMemFree(KCtx*);
    do_call(g,rt_imp_ole32__CoTaskMemFree,fmt,0,0,0,0,0);
    do_call(g,rt_imp_ole32__CoCreateInstance,clsid,0,1,clsid,out,0);
    CHECK(KH_RAX(g)==0x80004002u && rd64(g,out)==0);
    ga_free(clsid); ga_free(out);
done:
    kh_audio_test_mode(saved_mode);
    fprintf(stderr,"kh: audio selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

/* ---- fibers ---- */
static u64 main_fiber, fibA, fibB; static _Atomic int step; static int order[16]; static int nord;
static void fiberA(KCtx *g){ u64 p=KH_RCX(g); order[nord++]=100+(int)p; kh_fiber_switch(g,main_fiber); order[nord++]=101; kh_fiber_switch(g,main_fiber); order[nord++]=102; kh_fiber_switch(g,main_fiber); for(;;) kh_fiber_switch(g,main_fiber); }
static void fiberB(KCtx *g){ (void)g; order[nord++]=200; kh_fiber_switch(g,fibA); for(;;) kh_fiber_switch(g,main_fiber); }
static _Atomic int mig_done;
static void migrate_proc(KCtx *g){          /* runs on a second thread: converts itself and resumes fiber A there */
    u64 mf=kh_fiber_convert(g,0); (void)mf; main_fiber=mf; /* this thread's own "main fiber" for the yield target */
    order[nord++]=300; kh_fiber_switch(g,fibA); order[nord++]=301; mig_done=1; kh_fiber_convert_to_thread(g); KH_RET(g);
}
static void sleep_ms(int ms){ usleep((useconds_t)ms*1000); }

static void input_selftest(KCtx *g){
    extern void kh_input_load(KCtx *,const char *);
    extern u32 kh_input_state(KCtx *,u32,u64,u64);
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"khnmc-input-XXXXXX"); int fd=mkstemp(path); CHECK(fd>=0);
    FILE *f=fdopen(fd,"w"); CHECK(f!=NULL);
    fputs("10 12 A+START+LT+RT -32768 32767\n20 20 B+X+Y+BACK+LB+RB+DPAD_UP+DPAD_DOWN+DPAD_LEFT+DPAD_RIGHT+LSTICK+RSTICK\n30 31 - 1 -1\n",f); fclose(f);
    u64 out=ga_alloc(32,16); memset(hp(g,out),0xa5,32);
    kh_input_load(g,NULL); CHECK(kh_input_state(g,0,out,10)==1167 && rd32(g,out)==0xa5a5a5a5);
    kh_input_load(g,path); unlink(path);
    CHECK(kh_input_state(g,1,out,10)==1167);
    CHECK(kh_input_state(g,0,out,9)==0 && rd32(g,out)>0 && rd64(g,out+4)==0);
    u32 packet=rd32(g,out);
    CHECK(kh_input_state(g,0,out,10)==0 && rd32(g,out)==packet+1);
    CHECK(rd16(g,out+4)==0x1010 && rd8(g,out+6)==255 && rd8(g,out+7)==255);
    CHECK(rd16(g,out+8)==0x8000 && rd16(g,out+10)==0x7fff && rd32(g,out+12)==0 && rd32(g,out+16)==0xa5a5a5a5);
    extern void rt_imp_xinput1_4__ord2(KCtx *),rt_imp_xinput1_4__ord100(KCtx *),rt_imp_xinput1_4__XInputGetState(KCtx *),rt_imp_xinput1_4__XInputGetStateEx(KCtx *);
    void (*getters[])(KCtx *)={rt_imp_xinput1_4__ord2,rt_imp_xinput1_4__ord100,rt_imp_xinput1_4__XInputGetState,rt_imp_xinput1_4__XInputGetStateEx};
    for(int i=0;i<4;i++){ do_call(g,getters[i],0,out,0,0,0,0); CHECK(KH_RAX(g)==0 && rd64(g,out+4)==0); }
    kh_input_state(g,0,out,10);
    packet=rd32(g,out); kh_input_state(g,0,out,12); CHECK(rd32(g,out)==packet);
    kh_input_state(g,0,out,13); CHECK(rd32(g,out)==packet+1 && rd64(g,out+4)==0 && rd32(g,out+12)==0);
    kh_input_state(g,0,out,20); CHECK(rd16(g,out+4)==(0xe3ef & ~0xc0)); /* both sticks belong to appearance */
    kh_input_state(g,0,out,30); CHECK(rd32(g,out+4)==0 && rd16(g,out+8)==1 && rd16(g,out+10)==0xffff);
    kh_input_state(g,0,out,3001); CHECK(rd64(g,out+4)==0);
    extern void kh_input_attach(KCtx *);
    kh_call(g,GS_BASE+0x1335a0,0xab0001,0,0,0);
    kh_input_attach(g);
    CHECK(rd32(g,GS_BASE+0x8bb2a8+0x10)==0 && rd64(g,GS_BASE+0x8bb2a8+0xb8)!=0);
    extern _Atomic u64 gfx_presents; u64 saved_present=gfx_presents; gfx_presents=10;
    u64 data=ga_calloc(1,128);
    kh_call(g,GS_BASE+0x134d10,GS_BASE+0x8bb2a8,data,0,0);
    CHECK(rd64(g,GS_BASE+0x8bb2a8+0x18)==0x4308);
    CHECK(rd16(g,data)==0x4308);
    gfx_presents=saved_present; ga_free(data);
    /* the host pad (2026-11-18, the iPhone's controller): with no script it is pad 0, every field including the right stick; a change
     * makes a new packet number; with it off again pad 0 is absent (1167).  A script, when loaded, always wins. */
    { extern void kh_input_host_pad_set(u16,u8,u8,s16,s16,s16,s16); extern int kh_input_host_test(int);
      kh_input_load(g,NULL); int was=kh_input_host_test(1);
      kh_input_host_pad_set(0x1000|0x0200|0x0010,200,7,-12345,23456,32767,-32768);
      CHECK(kh_input_state(g,0,out,1)==0); u32 p0=rd32(g,out);
      CHECK(rd16(g,out+4)==0x1210 && rd8(g,out+6)==200 && rd8(g,out+7)==7 && (s16)rd16(g,out+8)==-12345 && (s16)rd16(g,out+10)==23456
            && (s16)rd16(g,out+12)==32767 && (s16)rd16(g,out+14)==-32768);
      CHECK(kh_input_state(g,0,out,2)==0 && rd32(g,out)==p0);                                   /* unchanged: the same packet */
      kh_input_host_pad_set(0,0,0,0,0,100,0); CHECK(kh_input_state(g,0,out,3)==0 && rd32(g,out)==p0+1 && rd16(g,out+4)==0 && (s16)rd16(g,out+12)==100);
      kh_input_host_pad_set(0x1000,0,0,0,0,100,0); kh_input_host_pad_set(0,0,0,0,0,100,0);              /* a tap between two polls */
      CHECK(kh_input_state(g,0,out,4)==0 && rd16(g,out+4)==0x1000 && rd32(g,out)==p0+2);                 /* seen once ... */
      CHECK(kh_input_state(g,0,out,5)==0 && rd16(g,out+4)==0 && rd32(g,out)==p0+3);                      /* ... then released */
      CHECK(kh_input_state(g,1,out,3)==1167);                                                   /* pad 0 only */
      kh_input_host_pad_set(0xc0,0,0,0,0,0,0);
      kh_input_host_pad_set(0,0,0,0,0,0,0);
      CHECK(kh_stick_action(1000)==CHORD_CLICK && kh_stick_action(1001)==CHORD_IDLE);
      kh_input_host_pad_set(0x40,0,0,0,0,0,0);
      kh_input_host_pad_set(0x80,0,0,0,0,0,0);
      CHECK(kh_stick_action(1002)==CHORD_IDLE);
      kh_input_host_pad_set(0xc0,0,0,0,0,0,0);
      CHECK(kh_input_state(g,0,out,6)==0 && !(rd16(g,out+4)&0xc0));
      kh_input_host_pad_set(0x40,0,0,0,0,0,0);
      CHECK(kh_input_state(g,0,out,7)==0 && !(rd16(g,out+4)&0xc0));
      kh_input_host_pad_set(0,0,0,0,0,0,0);
      kh_input_state(g,0,out,8);
      kh_input_host_pad_set(0x80,0,0,0,0,0,0);
      CHECK(kh_input_state(g,0,out,9)==0 && (rd16(g,out+4)&0xc0)==0x80);
      kh_input_host_test(0); CHECK(kh_input_state(g,0,out,4)==1167);
      kh_input_host_test(was); }
    kh_input_load(g,getenv("KHNMC_INPUT_SCRIPT")); ga_free(out);
}

static int selftest_all(KCtx *g);
/* review U35 routes n=8/9: the route suites load synthetic routes through kh_input_load, which reads KHNMC_ROUTE_END.  Inherited `exit`
 * made the first synthetic route's end call kh_exit_process(0): the selftest ended there with status 0, the later suites never ran and
 * failures already counted were lost.  The variable is removed for the whole selftest and put back afterwards. */
int kh_selftest(KCtx *g){
    const char *e=getenv("KHNMC_ROUTE_END"); char *sv=e?strdup(e):NULL;
    if(e && !sv) kh_fatal(g,"selftest: allocation failed (saving KHNMC_ROUTE_END)");
    unsetenv("KHNMC_ROUTE_END");
    int f=selftest_all(g);
    if(sv){ setenv("KHNMC_ROUTE_END",sv,1); free(sv); }
    return f;
}
static int selftest_all(KCtx *g){
    { const char *only=getenv("KHNMC_SELFTEST_ONLY");            /* task 31: one suite alone (a mutation check runs it many times); a partial run is said to be one and is no pass of the selftest */
      if(only&&*only){
          /* TASK36: also `vsync` (display pacing) and `gfx` (the graphics suites: null device, wrap, and with KHNMC_GFX=vkd3d* the device,
           * back-buffer and presenter selftests): mutation checks of those run them many times */
          if(!strcmp(only,"vsync")){ extern int kh_vsync_selftest(void); int f=kh_vsync_selftest();
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=vsync): only the suite `vsync` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"gfx")){ extern int kh_gfx_selftest(KCtx *); int f=kh_gfx_selftest(g);
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=gfx): only the graphics suites ran, %d failures\n",f); return f; }
          if(!strcmp(only,"movies")){ extern int kh_movies_selftest(KCtx *); int f=kh_movies_selftest(g);      /* task 44: for its mutation checks */
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=movies): only the suite `movies` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"aspect")){ extern int kh_aspect_selftest(KCtx *); int f=kh_aspect_selftest(g);      /* task 37: for its mutation checks */
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=aspect): only the suite `aspect` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"cutscenes")){ extern int kh_cutscenes_selftest(KCtx *); int f=kh_cutscenes_selftest(g);   /* task 50: for its mutation checks */
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=cutscenes): only the suite `cutscenes` ran, %d failures\n",f); return f; }
          /* task 39: `gate` (the admission-counted suspend gate), `ordered` (memory ordering: the option really on in this binary) and
           * `membench` (the cost of guest loads / stores plain against acquire/release, for the Mac mini and the phone) */
          if(!strcmp(only,"gate")){ extern int kh_gfx_gate_selftest(void); int f=kh_gfx_gate_selftest();
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=gate): only the suite `gfx gate` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"ordered")){ extern int kh_ordered_mem_selftest(KCtx *); int f=kh_ordered_mem_selftest(g);
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=ordered): only the suite `memory ordering` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"membench")){ extern int kh_membench(KCtx *); int f=kh_membench(g);
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=membench): the memory-ordering microbenchmark only, %d failures\n",f); return f; }
          /* TASK41: `app` (the app's life cycle: resign, suspend, resume, the stuck budgets under the gate) for its mutation checks */
          if(!strcmp(only,"app")){ extern int kh_app_life_selftest(void); int f=kh_app_life_selftest();
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=app): only the suite `app life` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"pipecache")){ extern int kh_pc_selftest(void); int f=kh_pc_selftest();                 /* TASK43: for its mutation checks */
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=pipecache): only the suite `gfx pipecache` ran, %d failures\n",f); return f; }
          if(!strcmp(only,"hostlog")){ extern int kh_host_log_selftest(void); extern int kh_gfx_frameline_selftest(void); extern int kh_gfx_vklog_selftest(void); int f=kh_host_log_selftest()+kh_gfx_frameline_selftest()+kh_gfx_vklog_selftest();
              fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=hostlog): only the suites `host log`, `gfx frame line` and `gfx vkd3d log` ran, %d failures\n",f); return f; }
          if(strcmp(only,"route31")) kh_fatal(g,"KHNMC_SELFTEST_ONLY=%s: the suites that can run alone are route31, vsync, gfx, aspect, gate, app, ordered, membench, pipecache and hostlog",only);
          extern int kh_input_route31_selftest(KCtx *); int f=kh_input_route31_selftest(g);
          fprintf(stderr,"kh: selftest: PARTIAL RUN (KHNMC_SELFTEST_ONLY=route31): only the suite `input route Q31` ran, %d failures\n",f); return f?f:0; } }
    extern int kh_sync_slot_selftest(KCtx *);
    fails+=kh_sync_slot_selftest(g);
    CHECK(rd64(g,GS_BASE+0x7534a8)!=0); /* Task 6: table 3 must exist before scripts run. */
    extern int kh_mod_static_selftest(KCtx *);
    fails+=kh_mod_static_selftest(g);    /* Task 12: first, before any other test can touch the edited tables or the mod buffer */
    extern int kh_mod_neverland_selftest(KCtx *);
    fails+=kh_mod_neverland_selftest(g); /* Task 19: NeverlandFly.dll (image, signatures, the three re-lifted functions, the row-step thread) */
    fails+=kh_mod_syscalls_selftest(g);
    extern int kh_mod_caps_selftest(KCtx *);
    fails+=kh_mod_caps_selftest(g);
    extern int kh_mod_damage_selftest(KCtx *);
    fails+=kh_mod_damage_selftest(g);
    extern int kh_mod_exp_selftest(KCtx *);
    fails+=kh_mod_exp_selftest(g);
    extern int kh_mod_hooks_selftest(KCtx *);
    fails+=kh_mod_hooks_selftest(g);
    extern int kh_mod_hooks_a_selftest(KCtx *);
    fails+=kh_mod_hooks_a_selftest(g);
    { extern int kh_mod_hooks_b_selftest(KCtx *); fails+=kh_mod_hooks_b_selftest(g); }   /* Task 23: the remaining DLL detours */
    extern int kh_mod_params_selftest(KCtx *);
    fails+=kh_mod_params_selftest(g);
    extern int kh_bgload_selftest(KCtx *);
    fails+=kh_bgload_selftest(g);
    extern int kh_sync_selftest(KCtx *);
    fails+=kh_sync_selftest(g);
    { extern int kh_sync_vclock_selftest(KCtx *); fails+=kh_sync_vclock_selftest(g); }   /* review of task 28: virtual time-outs of KHNMC_CLOCK_SYNC=all without lock-order deadlocks */
    { extern int kh_compat_selftest(void); fails+=kh_compat_selftest(); }      /* task 20: the host primitives that differ between Linux and Darwin */
    { extern int kh_vsync_selftest(void); fails+=kh_vsync_selftest(); }        /* TASK36: WaitForVBlank paced by display ticks (gfx_present.c) */
    { extern int kh_gfx_gate_selftest(void); fails+=kh_gfx_gate_selftest(); }  /* task 39: the admission-counted suspend gate (gfx_present.c) */
    { extern int kh_app_life_selftest(void); fails+=kh_app_life_selftest(); }  /* TASK41: resign / suspend / resume, stuck budgets under the gate (app_life.c) */
    { extern int kh_pc_selftest(void); fails+=kh_pc_selftest(); }              /* TASK43: the disk pipeline cache's file and rules, no device (gfx_pipecache.c) */
    { extern int kh_host_log_selftest(void); fails+=kh_host_log_selftest(); }  /* TASK43: the log file's size cap (host_env.c) */
    { extern int kh_gfx_frameline_selftest(void); fails+=kh_gfx_frameline_selftest(); }   /* TASK43: the per-present line's interval and the frame-time counter (null_d3d12.c) */
    { extern int kh_gfx_vklog_selftest(void); fails+=kh_gfx_vklog_selftest(); }   /* review C43 n=2: vkd3d's log, de-duplicated (null_d3d12.c) */
    { extern int kh_ordered_mem_selftest(KCtx *); fails+=kh_ordered_mem_selftest(g); }   /* task 39: KH_ORDERED_MEM (runtime/ordered_mem.c) */
    { extern int kh_loader_selftest(void); fails+=kh_loader_selftest(); }      /* review U35 n=24: the bias-mode reservation keeps its low guard or is a named fatal */
    { extern int kh_autosave_selftest(KCtx *); fails+=kh_autosave_selftest(g); }
    { extern int kh_xion_selftest(KCtx *); fails+=kh_xion_selftest(g); }
    input_selftest(g);
    { extern int kh_input_hold_selftest(KCtx *); fails+=kh_input_hold_selftest(g); }
    { extern int kh_input_route_selftest(KCtx *); fails+=kh_input_route_selftest(g); }   /* task 27: state-driven route steps */
    { extern int kh_input_route29_selftest(KCtx *); fails+=kh_input_route29_selftest(g); }   /* task 29: @goto, command menu, player / map / item readers */
    { extern int kh_input_route30_selftest(KCtx *); fails+=kh_input_route30_selftest(g); }   /* task 30: @fight, the programs reader */
    { extern int kh_data_deny_selftest(KCtx *); extern int kh_events_stub_selftest(KCtx *); extern int kh_music_log_selftest(KCtx *);   /* task 32 */
      fails+=kh_data_deny_selftest(g)+kh_events_stub_selftest(g)+kh_music_log_selftest(g); }
    { extern int kh_cutscenes_selftest(KCtx *); fails+=kh_cutscenes_selftest(g); }           /* task 50: KHNMC_CUTSCENES (runtime/cutscenes.c) */
    { extern int kh_input_route31_selftest(KCtx *); fails+=kh_input_route31_selftest(g); }   /* task 31: objective / message / reaction / menu readers, @react, @goto object */
    { extern int kh_warp_selftest(KCtx *); fails+=kh_warp_selftest(g); }                     /* task 33: the debug warp (runtime/warp.c) */
    { extern int kh_input_route33_selftest(KCtx *); fails+=kh_input_route33_selftest(g); }   /* task 33: the @warp / @warp list steps */
    { extern int kh_aspect_selftest(KCtx *); fails+=kh_aspect_selftest(g); }                 /* task 37: KHNMC_SCREEN (S1, S2), r / o / b, H1 (runtime/aspect.c) */
    { extern int kh_movies_selftest(KCtx *); fails+=kh_movies_selftest(g); }                 /* task 44: the empty movie of the movie seam (runtime/null_mf.c) */
    /* allocator */
    u64 p[64]; for(int i=0;i<64;i++){ p[i]=ga_alloc((u64)(i*37+1),(i%5==0)?64:16); CHECK(p[i]&&!(p[i]&15)); if(i%5==0) CHECK(!(p[i]&63)); memset(hp(g,p[i]),0xA5,(size_t)(i*37+1)); }
    for(int i=0;i<64;i+=2) ga_free(p[i]); u64 q=ga_calloc(10,10); CHECK(q&&rd64(g,q)==0&&rd64(g,q+88)==0); q=ga_realloc(q,5000); CHECK(q&&ga_usable(q)>=5000);
    u64 big=ga_alloc(3<<20,16); CHECK(big); memset(hp(g,big),1,3<<20); ga_free(big); u64 big2=ga_alloc(3<<20,4096); CHECK(big2&&!(big2&4095)); ga_free(big2);
    for(int i=1;i<64;i+=2) ga_free(p[i]); ga_free(q);
    /* qsort (Microsoft algorithm) + bsearch */
    u32 v[40]; for(int i=0;i<40;i++) v[i]=(u32)((i*2654435761u)>>7)%1000; u64 arr=ga_alloc(sizeof v,16); memcpy(hp(g,arr),v,sizeof v); u64 cmp=kh_host_fn("selftest_cmp",cmp_u32);
    do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__qsort,arr,40,4,cmp,0,0); u32 *o=hp(g,arr); int sorted=1; for(int i=1;i<40;i++) if(o[i-1]>o[i]) sorted=0; CHECK(sorted);
    u32 key=o[17]; u64 kp=ga_alloc(16,16); wr32(g,kp,key); do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch,kp,arr,40,4,cmp,0); CHECK(KH_RAX(g)!=0&&rd32(g,KH_RAX(g))==key);
    wr32(g,kp,100000); do_call(g,rt_imp_api_ms_win_crt_utility_l1_1_0__bsearch,kp,arr,40,4,cmp,0); CHECK(KH_RAX(g)==0);
    /* printf engine: Win64 va_list = array of 8-byte slots */
    u64 fmt=ga_strdup("%d|%5.2f|%s|%llu|%x|%c|%-4d|%p|%ls"); u64 buf=ga_alloc(256,16); u64 va=ga_alloc(128,16); u64 s1=ga_strdup("hi"); u64 ws=ga_alloc(16,16); wr16(g,ws,'w'); wr16(g,ws+2,'z'); wr16(g,ws+4,0);
    double d=3.14159; u64 dv; memcpy(&dv,&d,8); wr64(g,va,(u64)(u32)-42); wr64(g,va+8,dv); wr64(g,va+16,s1); wr64(g,va+24,1234567890123ull); wr64(g,va+32,0xBEEF); wr64(g,va+40,'Q'); wr64(g,va+48,7); wr64(g,va+56,0x140001000ull); wr64(g,va+64,ws);
    do_call(g,rt_imp_api_ms_win_crt_stdio_l1_1_0____stdio_common_vsprintf,0,buf,256,fmt,0,va);
    CHECK(!strcmp(gstr(g,buf),"-42| 3.14|hi|1234567890123|beef|Q|7   |0000000140001000|wz"));
    if(strcmp(gstr(g,buf),"-42| 3.14|hi|1234567890123|beef|Q|7   |0000000140001000|wz")) fprintf(stderr,"  got: %s\n",gstr(g,buf));
    /* critical section + events across threads */
    u64 cs=ga_alloc(64,16); do_call(g,rt_imp_kernel32__InitializeCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__EnterCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__EnterCriticalSection,cs,0,0,0,0,0); do_call(g,rt_imp_kernel32__LeaveCriticalSection,cs,0,0,0,0,0);
    CHECK(rd32(g,cs+0xC)==1); do_call(g,rt_imp_kernel32__LeaveCriticalSection,cs,0,0,0,0,0); CHECK(rd64(g,cs+0x10)==0);
    /* fibers: main converts, A yields back, B hands over to A, then A is resumed on ANOTHER thread */
    main_fiber=kh_fiber_convert(g,0); CHECK(main_fiber!=0);
    fibA=kh_fiber_create(g,0,kh_host_fn("selftest_fiberA",fiberA),7); fibB=kh_fiber_create(g,0,kh_host_fn("selftest_fiberB",fiberB),0);
    kh_fiber_switch(g,fibA); CHECK(nord==1&&order[0]==107);                       /* A started and yielded */
    kh_fiber_switch(g,fibB); CHECK(nord==3&&order[1]==200&&order[2]==101);          /* B ran, switched to A, A continued and yielded to main */
    u64 mf_save=main_fiber; (void)mf_save;
    /* A is now suspended at its second yield; resume it from a second host thread (migration) */
    u64 a=kh_host_fn("selftest_migrate",migrate_proc); kh_spawn_hostfn_thread(g,"selftest_migrate",migrate_proc,0); (void)a;
    for(int i=0;i<200&&!mig_done;i++) sleep_ms(10);
    CHECK(mig_done==1); CHECK(order[3]==300&&order[4]==102&&order[5]==301);
    main_fiber=mf_save;
    kh_fiber_delete(g,fibB); kh_fiber_delete(g,fibA); CHECK(kh_fiber_convert_to_thread(g)==1);
    /* yield hooks: from==to so 0x135470 returns immediately; checks the register plumbing and the stores */
    u64 task=ga_alloc(0x100,16),P=ga_alloc(0x40,16),Q=ga_alloc(0x40,16),X=ga_alloc(0x10,16); memset(hp(g,task),0,0x100); memset(hp(g,P),0,0x40); memset(hp(g,Q),0,0x40);
    wr64(g,task+0x70,P); wr64(g,P,X); wr64(g,task+0x58,Q); wr64(g,Q+0x28,X);
    do_call(g,rt_hook_fiber_yield_150060,task,0x1234,0,0,0,0); CHECK(rd32(g,P+0x1c)==0x1234);
    float c; u32 cu=rd32(g,GS_BASE+0x717480); memcpy(&c,&cu,4); float x1=2.5f; memcpy(&KH_XMM(g,1),&x1,4); do_call(g,rt_hook_fiber_yield_150080,task,0,0,0,0,0); float r; u32 ru=rd32(g,P+0x20); memcpy(&r,&ru,4); CHECK(r==2.5f-c);
    fprintf(stderr,"kh: selftest: %d checks, %d failures\n",checks,fails);
    if(getenv("KH_SELFTEST_FATAL")){ extern void rt_imp_gdi32__MoveToEx(KCtx*); do_call(g,rt_imp_gdi32__MoveToEx,1,2,3,4,0,0); }   /* demonstrates the loud failure of an unimplemented import */
    extern int kh_gfx_selftest(KCtx *); extern int kh_hd_selftest(KCtx *); extern int kh_native_selftest(KCtx *); extern int kh_audio_mix_selftest(void); extern int kh_audio_soft_selftest(KCtx *); extern int kh_audio_vclock_selftest(KCtx *); extern int kh_audio_vclock_anchor_selftest(KCtx *); extern int kh_audio_fx_serial_selftest(KCtx *); extern int kh_audio_creation_selftest(KCtx *); extern int kh_audio_pull_selftest(KCtx *); extern int kh_audio_out_selftest(void); extern int kh_stream_selftest(KCtx *);
    fails += kh_vclock_selftest(g) + io_selftest(g) + audio_selftest(g) + kh_audio_mix_selftest() + kh_audio_soft_selftest(g) + kh_audio_vclock_selftest(g) + kh_audio_vclock_anchor_selftest(g) + kh_audio_fx_serial_selftest(g) + kh_audio_creation_selftest(g) + kh_audio_out_selftest() + kh_audio_pull_selftest(g) + kh_stream_selftest(g) + kh_file_selftest(g) + kh_hd_selftest(g) + kh_gfx_selftest(g) + kh_native_selftest(g);
    /* task 17, after everything else: these move the arena's high-water mark by hundreds of MiB (the enlarged pools, the stress placement), and
     * some older fixtures assume the low heap addresses of an untouched arena (mod_syscalls_test splits a heap pointer into two halves) */
    { extern int kh_ga_selftest(KCtx *); extern int kh_idtable_selftest(KCtx *); extern int kh_moose_selftest(KCtx *); fails += kh_ga_selftest(g) + kh_idtable_selftest(g) + kh_moose_selftest(g); }
    fails+=kh_host_env_selftest();          /* task 26: only in a KH_IOS_PATHS build (container directories, the conf file, the status hook) */
    kh_selftest_not_run_report(stderr);     /* task 26: fatal-path cases a KH_IOS_PATHS build (no fork()) did not run; prints nothing when all ran */
    return fails;
}
