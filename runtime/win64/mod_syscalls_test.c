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
    FILE *f=kh_tmpfile(); if(!f) return 0;
    pid_t child=kh_fork();
    if(child==0){
        dup2(fileno(f),2);
        u64 sp=KH_RSP(g)-0x108; wr64(g,sp,GS_BASE+0x41c4fc); KH_RSP(g)=sp; A0(g)=args;
        kh_dispatch(g,rd64(g,rd64(g,GS_BASE+0x7534a8)+16*idx)); _exit(99);
    }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char output[32768]; size_t n=fread(output,1,sizeof output-1,f); output[n]=0; fclose(f);
    return WIFEXITED(status) && WEXITSTATUS(status)!=0 && WEXITSTATUS(status)!=99 && strstr(output,match) && strstr(output,"call-site RVA=0x41c4fa");
}
/* Task 23: recorder behind the engine-call seam of Set_Action / MapJump / PartyJump */
static struct { int n; u32 rva[8]; u64 a[8][5]; u8 mem[8][16]; } sc;
static u64 sc_hook(KCtx *g,u32 rva,const u64 *a){
    if(sc.n<8){ sc.rva[sc.n]=rva; memcpy(sc.a[sc.n],a,sizeof sc.a[0]); if(rva==0x152990&&GA_OK(a[0])&&GA_OK(a[0]+15)) memcpy(sc.mem[sc.n],hp(g,a[0]),16); }
    sc.n++; return rva==0x3b3e80?0x3e80aaa0ull:0x9900ull+rva;
}
static int fatal_text(KCtx *g,int idx,u64 args,const char *match){          /* like fatal_case, for fatals that are not the "unimplemented" one */
    FILE *f=kh_tmpfile(); if(!f) return 0;
    pid_t child=kh_fork();
    if(child==0){ dup2(fileno(f),2); u64 sp=KH_RSP(g)-0x108; wr64(g,sp,GS_BASE+0x41c4fc); KH_RSP(g)=sp; A0(g)=args; kh_dispatch(g,rd64(g,rd64(g,GS_BASE+0x7534a8)+16*idx)); _exit(99); }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char output[32768]; size_t n=fread(output,1,sizeof output-1,f); output[n]=0; fclose(f);
    return WIFEXITED(status) && WEXITSTATUS(status)==3 && strstr(output,match)!=NULL;
}
static u64 reset_seen_slot, reset_seen_rcx, reset_seen_ret;
static void reset_tail_probe(KCtx *g){ reset_seen_slot=rd64(g,GS_BASE+0x7534a8); reset_seen_rcx=KH_RCX(g); reset_seen_ret=rd64(g,KH_RSP(g)); KH_RAX(g)=0x5e7; KH_RET(g); }
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
    if(kh_fork_ok()){
    T(fatal_case(g,2,args,"index=2 name=AddFormExp"));          /* Set_Action (0) is implemented since Task 23 */
    T(fatal_case(g,50,args,"index=50 name=GetCurrentRC_MEM"));
    } else kh_selftest_not_run("mod syscall","two unimplemented syscalls (named fatal with the call site)",2);   /* iOS: no fork (host_env.c) */

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
    /* ---- Task 23: Set_Action (0), MapJump (8), GetGrowthTable (47), SetCameraMode (55), PartyJump (93); expectations from the DLL listing ---- */
    { extern u64 (*kh_mod_syscalls_call_hook)(KCtx *,u32,const u64 *);
      T(rd64(g,table+0*16+8)==2&&rd64(g,table+8*16+8)==9&&rd64(g,table+47*16+8)==0x40000000&&rd64(g,table+55*16+8)==1&&rd64(g,table+93*16+8)==0);   /* argc / return flags */
      /* 47 (DLL 0x128f0): slot 0 = handle of exe 0x1405c9030, through the real pointer codec; the upper half of the slot stays */
      u32 gt=invoke(g,47,args,0xaaaa5555,0); T(gt!=0xaaaa5555u&&kh_call(g,GS_BASE+0x4ad270,gt,0,0,0)==GS_BASE+0x5c9030);
      /* 55 (DLL 0x12a40): one byte to exe 0x140718ca8, the three bytes above it untouched; only once [0x142ae5768] is non-zero */
      u64 svG=rd64(g,GS_BASE+0x2ae5768); u32 svC=rd32(g,GS_BASE+0x718ca8);
      wr64(g,GS_BASE+0x2ae5768,0x10); wr32(g,GS_BASE+0x718ca8,0xa1b2c3d4u);
      T(invoke(g,55,args,0x12345ef,0)==0x12345ef); T(rd32(g,GS_BASE+0x718ca8)==0xa1b2c3efu);
      invoke(g,55,args,0x0b,0); T(rd32(g,GS_BASE+0x718ca8)==0xa1b2c30bu);
      wr64(g,GS_BASE+0x2ae5768,0); wr64(g,args,0x0b);
      if(kh_fork_ok()) T(fatal_text(g,55,args,"mod syscall SetCameraMode (table 3 index 55, DLL 0x12a40) before the DLL's worker thread"));
      else kh_selftest_not_run("mod syscall","SetCameraMode before the DLL's worker thread (named fatal)",1);   /* iOS: no fork (host_env.c) */
      T(rd32(g,GS_BASE+0x718ca8)==0xa1b2c30bu);
      wr64(g,GS_BASE+0x2ae5768,svG); wr32(g,GS_BASE+0x718ca8,svC);
      kh_mod_syscalls_call_hook=sc_hook;
      /* 0 (DLL 0x115c0): 0x3b4da0(decode(arg0), arg1 as a dword) */
      memset(&sc,0,sizeof sc); invoke(g,0,args,actor_id,0x1d);
      T(sc.n==1&&sc.rva[0]==0x3b4da0&&sc.a[0][0]==actor&&sc.a[0][1]==0x1d&&sc.a[0][2]==0&&sc.a[0][4]==0);
      memset(&sc,0,sizeof sc); invoke(g,0,args,actor_id,0xfffffff7u); T(sc.n==1&&sc.a[0][1]==0xfffffff7ull);       /* mov edx,ebx: zero-extended */
      /* 93 (DLL 0x14a90): 0x3cb9f0(0, 0x3b3e80(), 1) */
      memset(&sc,0,sizeof sc); invoke(g,93,args,0x77,0x88);
      T(sc.n==2&&sc.rva[0]==0x3b3e80&&sc.rva[1]==0x3cb9f0&&sc.a[1][0]==0&&sc.a[1][1]==0x3e80aaa0ull&&sc.a[1][2]==1&&sc.a[1][4]==0); T(rd32(g,args)==0x77);
      /* 8 (DLL 0x118b0): nine slots -> {byte, byte, byte, 0, word, word, word} in the handler's frame, then 0x152990(record, arg6, arg7, 0, arg8) */
      u64 a9=ga_calloc(9,8); static const u64 v9[9]={0xaaaaaaaa0001ff02ull,0xbbbbbbbb00000220ull,0xcccccccc00000182ull,0xdddddddd00012345ull,0xeeeeeeeeffff8001ull,0x1111111100000186ull,
                                                    0x2222222280000001ull,0x3333333300000007ull,0x44444444deadbeefull};
      for(int i=0;i<9;i++) wr64(g,a9+8*(u64)i,v9[i]);
      /* raw dispatch like the VM's call: the handler must leave the stack pointer exactly one return address higher (kh_call would hide a
       * handler that forgets to undo its scratch frame) */
      memset(&sc,0,sizeof sc); u64 sp9=KH_RSP(g), svc=KH_RCX(g);
      { u64 spx=((sp9-0x100)&~0xFull)-8; wr64(g,spx,GS_BASE+0x41c4fc); KH_RSP(g)=spx; KH_RCX(g)=a9; kh_dispatch(g,rd64(g,table+16*8)); T(KH_RSP(g)==spx+8); KH_RSP(g)=sp9; KH_RCX(g)=svc; sp9=spx; }
      static const u8 rec9[10]={0x02,0x20,0x82,0x00,0x45,0x23,0x01,0x80,0x86,0x01};
      T(sc.n==1&&sc.rva[0]==0x152990&&sc.a[0][1]==0x80000001ull&&sc.a[0][2]==7&&sc.a[0][3]==0&&sc.a[0][4]==0xdeadbeefull&&!memcmp(sc.mem[0],rec9,10));
      T(sc.a[0][0]<sp9&&sc.a[0][0]>=sp9-0x200);                                                                  /* the record is below the caller's stack pointer */
      { int same=1; for(int i=0;i<9;i++) if(rd64(g,a9+8*(u64)i)!=v9[i]) same=0; T(same); }                        /* no slot is written (no return value) */
      kh_mod_syscalls_call_hook=NULL; ga_free(a9);
      /* Set_Action on a handle that decodes to no object: 0x3b4da0 would read [NULL+0xc] on the PC */
      wr64(g,args,0); wr64(g,args+8,3);
      if(kh_fork_ok()) T(fatal_text(g,0,args,"mod syscall invalid guest memory"));
      else kh_selftest_not_run("mod syscall","Set_Action on a handle that decodes to no object (named fatal)",1); }   /* iOS: no fork */
    for(int i=0;i<6;i++) wr64(g,GS_BASE+globals[i],saved[i]);
    /* Script teardown must preserve the installed extension and the vanilla tables. */
    kh_call(g,GS_BASE+0x1f9a80,0,0,0,0); T(rd64(g,GS_BASE+0x7534a8)==table);
    /* Task 23: DLL 0x2d110 REPLACES 0x1f9a80: slot 3 = the table FIRST, then a tail jump to exe 0x3e1620 (never 0x3e1710(0)).
     * The stand-in for 0x3e1620 sees the table already in place, the caller's arguments and the caller's return address. */
    { extern KhFn kh_mod_syscalls_reset_tail; extern u64 kh_mod_syscalls_reset_calls(void);
      u64 before=kh_mod_syscalls_reset_calls(), sp0=KH_RSP(g);
      wr64(g,GS_BASE+0x7534a8,0); reset_seen_slot=1; reset_seen_rcx=0; reset_seen_ret=0;
      kh_mod_syscalls_reset_tail=reset_tail_probe;
      u64 r=kh_call(g,GS_BASE+0x1f9a80,0xabc,0,0,0);
      kh_mod_syscalls_reset_tail=NULL;
      T(reset_seen_slot==table); T(reset_seen_rcx==0xabc); T(reset_seen_ret==0x7ff7ffff0000ull); T(r==0x5e7ull); T(KH_RSP(g)==sp0);
      T(rd64(g,GS_BASE+0x7534a8)==table); T(kh_mod_syscalls_reset_calls()==before+1); }
    for(int i=0;i<11;i++) if(i!=3) T(rd64(g,GS_BASE+0x753490+8*i)==(vanilla[i]?GS_BASE+vanilla[i]:0));
    memcpy(hp(g,GS_BASE+0x2b0d720),codec_backup,512); wr32(g,GS_BASE+0x2b0d920,codec_guard);
    fprintf(stderr,"kh: mod syscall selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}
