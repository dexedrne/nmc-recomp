/* Synthetic VM calls use the real lifted helpers and fake guest structures, no game data. */
#include "khnmc_rt.h"
#include <unistd.h>
#include <sys/wait.h>
extern KhFn kh_lookup(u64 target);
static int checks, fails;
#define T(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"mod syscall selftest FAIL: %s:%d: %s\n",__FILE__,__LINE__,#c); } }while(0)
static u64 invoke(KCtx *g,int i,u64 args,u32 a,u32 b){
    wr64(g,args,0x1234567800000000ull|a); wr64(g,args+8,0x8765432100000000ull|b);
    u64 sp=KH_RSP(g); kh_call(g,rd64(g,rd64(g,GS_BASE+0x7534a8)+16*i),args,0,0,0);
    T(KH_RSP(g)==sp); T(rd32(g,args+4)==0x12345678); T(rd64(g,args+8)==(0x8765432100000000ull|b));
    return rd32(g,args);
}
static u32 idof(KCtx *g,u64 p){ return (u32)kh_call(g,GS_BASE+0x4ad240,p,0,0,0); }
static void wf(KCtx *g,u64 p,float v){ u32 b; memcpy(&b,&v,4); wr32(g,p,b); }
static int fatal_case(KCtx *g,int idx,u64 args,const char *match){
    FILE *f=tmpfile(); if(!f) return 0;
    pid_t child=fork();
    if(child==0){
        dup2(fileno(f),2);
        u64 sp=KH_RSP(g)-0x108; wr64(g,sp,GS_BASE+0x41c4fc); KH_RSP(g)=sp; A0(g)=args;
        kh_dispatch(g,rd64(g,rd64(g,GS_BASE+0x7534a8)+16*idx)); _exit(99);
    }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char output[32768]; size_t n=fread(output,1,sizeof output-1,f); output[n]=0; fclose(f);
    return WIFEXITED(status) && WEXITSTATUS(status)!=0 && WEXITSTATUS(status)!=99 && strstr(output,match) && strstr(output,"call-site RVA=0x41c4fa");
}
int kh_mod_syscalls_selftest(KCtx *g){
    u64 table=rd64(g,GS_BASE+0x7534a8), args=ga_calloc(2,8);
    T(table>=GS_BASE+IMG_SIZE); T(rd32(g,GS_BASE+0x7534a8)==(u32)table);
    T(rd32(g,GS_BASE+0x7534ac)==(u32)(table>>32));
    const u32 vanilla[]={0x752e00,0x755370,0x756b60,0,0x757250,0x789be0,0x787e80,0x7883f0,0x787530,0,0x787070};
    for(int i=0;i<11;i++) if(i!=3) T(rd64(g,GS_BASE+0x753490+8*i)==(vanilla[i]?GS_BASE+vanilla[i]:0));
    for(int i=0;i<114;i++){
        u64 target=rd64(g,table+16*i); T(kh_lookup(target)!=NULL); T(kh_host_fn_name(target)!=NULL);
        T((rd64(g,table+16*i+8)&~0x4000ffffull)==0);
        if(i) T(target!=rd64(g,table+16*(i-1)));
    }
    T(rd64(g,table+3*16+8)==0x40000002); T(rd64(g,table+69*16+8)==2);
    T(rd64(g,table+113*16+8)==0x40000001);
    T(fatal_case(g,0,args,"index=0 name=Set_Action"));
    T(fatal_case(g,50,args,"index=50 name=GetCurrentRC_MEM"));

    /* Snapshot globals touched by the fixtures; restore them before other selftests. */
    const u32 globals[]={0x2ae57b0,0x2a25370,0x2aea8a0,0x2a253d0,0x2a105d0,0x2a0ff68};
    u64 saved[6]; for(int i=0;i<6;i++) saved[i]=rd64(g,GS_BASE+globals[i]);
    /* Selftest runs before CRT constructors: supply an initialized pointer-codec registry. */
    u8 codec_backup[512]; memcpy(codec_backup,hp(g,GS_BASE+0x2b0d720),512);
    u32 codec_guard=rd32(g,GS_BASE+0x2b0d920);
    memset(hp(g,GS_BASE+0x2b0d720),0xff,512); wr32(g,GS_BASE+0x2b0d920,0x80000000);
    u64 actor=ga_calloc(1,0xe00),sheet=ga_calloc(1,0x300),party=ga_calloc(1,0x100),form=ga_calloc(1,0x40);
    u32 actor_id=idof(g,actor),sheet_id=idof(g,sheet),party_id=idof(g,party),form_id=idof(g,form);
    wr64(g,actor+0x5c0,sheet);
    /* ConvertPointer, including the DLL's surprising sign extension of the low half. */
    u32 converted=invoke(g,3,args,(u32)(actor>>32),(u32)actor);
    T(kh_call(g,GS_BASE+0x4ad270,converted,0,0,0)==actor);
    converted=invoke(g,3,args,1,0x80000001);
    T(kh_call(g,GS_BASE+0x4ad270,converted,0,0,0)==0xffffffff80000001ull);
    T(invoke(g,3,args,0,0)==0);

    wr32(g,sheet+0x180,10); wr32(g,sheet+0x184,20);
    invoke(g,13,args,sheet_id,7); T(rd32(g,sheet+0x180)==17);
    invoke(g,13,args,sheet_id,100); T(rd32(g,sheet+0x180)==20);
    invoke(g,13,args,sheet_id,(u32)-30); T(rd32(g,sheet+0x180)==0);

    u64 attack=ga_calloc(1,0x38); wr32(g,attack+4,1); wr32(g,attack+8,0x10200);
    wr64(g,GS_BASE+0x2ae57b0,attack);
    converted=invoke(g,15,args,1,2); T(kh_call(g,GS_BASE+0x4ad270,converted,0,0,0)==attack+8);

    /* SheetReload's no-partram path leaves the sheet untouched. */
    wr8(g,sheet,37); invoke(g,33,args,sheet_id,0); T(rd8(g,sheet)==37);
    u64 pad=GS_BASE+0xbf3270, pad0=rd64(g,pad),pad1=rd64(g,pad+8);
    wr64(g,pad,1ull<<4); wr64(g,pad+8,1ull<<5);
    T(invoke(g,34,args,5,0)==1); T(invoke(g,34,args,4,0)==0);
    T(invoke(g,35,args,4,0)==1); T(invoke(g,35,args,5,0)==0);
    /* x86 shift masks the bit number to five bits and sign extends the mask. */
    T(invoke(g,35,args,36,0)==1); wr64(g,pad,1ull<<60); T(invoke(g,35,args,31,0)==1);
    wr64(g,pad,pad0); wr64(g,pad+8,pad1);

    u64 items=ga_calloc(1,0x20),abilities=ga_calloc(1,0x18);
    wr32(g,items+4,1); wr16(g,items+8,42); wr8(g,items+10,2); wr16(g,items+14,9);
    wr32(g,abilities+4,1); wr16(g,abilities+8,9); wr16(g,abilities+10,0x123);
    wr64(g,GS_BASE+0x2a25370,items); wr64(g,GS_BASE+0x2aea8a0,abilities);
    wr32(g,sheet+0x250,party_id); wr32(g,sheet+0x254,form_id);
    wr16(g,party,42); wr8(g,party+0x10,1); wr16(g,party+0x14,42);
    wr8(g,party+0x11,1); wr16(g,party+0x24,42);
    wr16(g,party+0x54,0x8123); wr16(g,party+0xf2,0x8123); wr16(g,party+0x56,0x123);
    wr16(g,form,1); wr16(g,form+8,0x8123); wr16(g,form+0x36,0x8123);
    T(invoke(g,46,args,actor_id,0x123)==8); /* weapon+2 slots+2 enabled+weapon again+2 form */
    wr32(g,sheet+0x254,0); T(invoke(g,46,args,actor_id,0x123)==5);
    wr32(g,sheet+0x250,0); T(invoke(g,46,args,actor_id,0x123)==actor_id);

    u64 player=ga_calloc(1,0xdd8), command=ga_calloc(1,0xb50);
    wr64(g,GS_BASE+0x2a105d0,player); wr64(g,player+0xdd0,command); wr8(g,command+0xb42,0xa7);
    T(invoke(g,48,args,0,0)==0xa7);
    u64 byte=ga_calloc(1,16); u32 byte_id=idof(g,byte); wr32(g,byte,0xaabbccdd);
    invoke(g,69,args,byte_id,0x12345678); T(rd32(g,byte)==0xaabbcc78);

    /* Backyard count: real item lookup plus the save-data byte, then the flag-bit path. */
    wr16(g,items+8+0x12,7);
    u64 inventory=GS_BASE+0x9a98b0+0x3580+7; u8 inv=rd8(g,inventory); wr8(g,inventory,23);
    T(invoke(g,78,args,42,0)==23); wr8(g,inventory,inv);
    wr8(g,items+11,1); u64 flag=GS_BASE+0x9a98b0+0x36c0; u32 oldflag=rd32(g,flag); wr32(g,flag,1<<7);
    T(invoke(g,78,args,42,0)==1); wr32(g,flag,oldflag);

    /* PlayerParamApply: real helper updates the selected save party's level/stat bytes. */
    u64 params=ga_calloc(1,0x88); wr32(g,params+4,1); wr16(g,params+8,99);
    wr8(g,params+10,1); wr8(g,params+11,17); wr8(g,params+12,18); wr8(g,params+13,19); wr8(g,params+14,20);
    wr64(g,GS_BASE+0x2a253d0,params);
    /* Party index 1 is stored in the save structure; save/restore that record. */
    u64 save_party=kh_call(g,GS_BASE+0x3a14e0,GS_BASE+0x9abda0,1,0,0);
    u8 party_backup[0x100]; memcpy(party_backup,hp(g,save_party),sizeof party_backup);
    invoke(g,80,args,99,0); T(rd8(g,save_party+4)==17 && rd8(g,save_party+6)==18 && rd8(g,save_party+8)==19);
    memcpy(hp(g,save_party),party_backup,sizeof party_backup);

    /* Setter must affect a real drive award, not just retain an unused float. */
    invoke(g,87,args,0x40000000,0); /* 2.0 */
    wr64(g,GS_BASE+0x2a0ff68,0); wr8(g,sheet+0x1af,1); wr8(g,sheet+0x1b2,9);
    wf(g,sheet+0x1c0,0); float amount=3.75f; memcpy(&KH_XMM(g,1),&amount,4);
    kh_call(g,GS_BASE+0x3a9cf0,actor,0,0,0); T(rd8(g,sheet+0x1b0)==7);
    invoke(g,87,args,0x3f800000,0);
    for(int i=0;i<6;i++) wr64(g,GS_BASE+globals[i],saved[i]);
    /* Script teardown must preserve the installed extension and the vanilla tables. */
    kh_call(g,GS_BASE+0x1f9a80,0,0,0,0); T(rd64(g,GS_BASE+0x7534a8)==table);
    for(int i=0;i<11;i++) if(i!=3) T(rd64(g,GS_BASE+0x753490+8*i)==(vanilla[i]?GS_BASE+vanilla[i]:0));
    memcpy(hp(g,GS_BASE+0x2b0d720),codec_backup,512); wr32(g,GS_BASE+0x2b0d920,codec_guard);
    fprintf(stderr,"kh: mod syscall selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}
