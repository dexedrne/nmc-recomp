/* Task 16 selftest: DLL 0x31260 kind 0 (stock item path, with the command-byte reversal) and the fall-through of DLL 0x30f60.
 * Two layers.  (1) Call transcript: every outgoing guest call of the apply path is recorded through kh_item_mock_sink and
 * compared with the sequence read from the DLL (this is the only way to see the "other user" flag and the raw amounts).
 * (2) Real lifted helpers (3c04c0, 3b68b0 + bsearch, 3c0c30, 3c0cd0, 3c07e0, the original 3c3da0) on a synthetic sheet with no
 * actor: final HP/MP against a model of the helpers' no-actor branches.  Expected amounts come from runtime/mod_item_cases.h. */
#include "mod_config.h"
#include "mod_item_cases.h"
#include <unistd.h>
#include <sys/wait.h>
void f_3c4b80(KCtx *g); void f_3c3da0(KCtx *g);
void orig_f_3c4b80(KCtx *g); void orig_f_3c3da0(KCtx *g);
static int checks, failures;
#define T(x) do { checks++; if(!(x)){ failures++; if(failures<=40) fprintf(stderr,"difficulty items FAIL line %d: %s\n",__LINE__,#x); } } while(0)

/* Sorted by id like the real table (03system "cmd": u32 version, s32 count, 0x30-byte records; exe 3b68b0 bsearches it). */
static const u16 test_cmd_ids[]={0,20,21,22,23,100,242,243,244,0x7fff};
#define NCMD ((int)(sizeof test_cmd_ids/sizeof *test_cmd_ids))
u64 kh_mod_items_test_cmds(KCtx *g){
    u64 table=ga_calloc(1,8+0x30*NCMD);
    if(!table) kh_fatal(g,"items selftest cannot allocate the cmd table");
    wr32(g,table,2); wr32(g,table+4,NCMD);
    for(int i=0;i<NCMD;i++){ wr16(g,table+8+0x30*i,test_cmd_ids[i]); wr16(g,table+8+0x30*i+2,3); }   /* +2 = 3: no shortcut redirect */
    return table;
}
static u64 cmd_record(KCtx *g,unsigned id){
    u64 table=rd64(g,GS_BASE+0x2a161e8);
    for(int i=0;i<NCMD;i++) if(test_cmd_ids[i]==id) return table+8+0x30*i;
    kh_fatal(g,"items selftest: command %u is not in the synthetic table",id);
}

struct fake_user { KCtx *g; u64 sheet, cmd; int null_sheet, null_cmd, poke; u32 poke_mp; };
static u64 fake(void *user,const struct kh_item_call *c){
    struct fake_user *u=user;
    switch(c->rva){
    case 0x3c04c0: return u->null_sheet?0:u->sheet;
    case 0x4ad270: return 0;
    case 0x152380: return GS_BASE+0x9a98b0;
    case 0x3b68b0: return u->null_cmd?0:u->cmd;
    case 0x3c0c30: if(u->poke) wr32(u->g,u->sheet+0x184,u->poke_mp); return 0x1111;
    case 0x3c07e0: return 0x7e0;
    default: return 0x2222;
    }
}
/* The DLL's call sequence for a kind-0 record, read from 31351..31557, 3155f..31613 and the exit 31ba4/31bb6. */
static int expect_stock(struct kh_item_call *e,u64 sheet,u32 target,u32 user,u32 mult,unsigned id,u32 pct,u16 cmdword,u32 actor_id,
                        u32 maxhp,u32 maxmp,u32 maxmp_after_hp,unsigned byte){
    int n=0;
#define E(r,a0,a1,a2,a3) (e[n++]=(struct kh_item_call){(r),{(a0),(a1),(a2),(a3)}})
    E(0x3c04c0,target,0xffffffffu,0,0);
    E(0x4ad270,actor_id,0,0,0);
    E(0x152380,0,0,0,0);
    E(0x3b68b0,(u32)(s32)(s16)cmdword,0,0,0);
    u32 hp_amount=(u32)kh_item_ref_amount(kh_item_ref_numer(pct,mult,maxhp),(int)byte);
    u32 mp_amount=(u32)kh_item_ref_amount(kh_item_ref_numer(pct,mult,maxmp),(int)byte);
    u64 flag=user!=0;                                   /* NOT user!=0 && user!=target: that is vanilla */
    switch(id){
    case 1: case 2: case 5: E(0x3c0c30,sheet,hp_amount,0,flag); break;
    case 3: case 6: E(0x3c0cd0,sheet,mp_amount,flag,0); break;
    case 4: case 7: E(0x3c0c30,sheet,hp_amount,0,flag); E(0x3c0cd0,sheet,mp_amount,0,0); break;
    case 0x83: E(0x3c0c30,sheet,maxhp,0,1); E(0x3c0cd0,sheet,(u32)(pct*maxmp_after_hp*mult),0,0); break;
    case 0x112: case 0x113: E(0x3c0ad0,sheet,(u32)(pct*100u),0,0); break;
    case 0x114: E(0x3c09d0,sheet,pct,0,0); break;
    case 0x115: E(0x3c0e30,sheet,pct,0,0); break;
    case 0x116: E(0x3c0a50,sheet,pct,0,0); break;
    case 0x117: E(0x3c0950,sheet,pct,0,0); break;
    default: break;                                     /* any other id: the lookup only */
    }
    E(0x3c07e0,sheet,0,0,0);
#undef E
    return n;
}
static int same_calls(const struct kh_item_mock *m,const struct kh_item_call *e,int n){
    if(m->overflow || m->ncalls!=n) return 0;
    for(int i=0;i<n;i++){
        if(m->c[i].rva!=e[i].rva) return 0;
        for(int k=0;k<4;k++) if(m->c[i].a[k]!=e[i].a[k]) return 0;
    }
    return 1;
}
static void describe(const char *what,const struct kh_item_call *c,int n){
    fprintf(stderr,"  %s:",what);
    for(int i=0;i<n;i++) fprintf(stderr," %x(%llx,%llx,%llx,%llx)",c[i].rva,(unsigned long long)c[i].a[0],(unsigned long long)c[i].a[1],
        (unsigned long long)c[i].a[2],(unsigned long long)c[i].a[3]);
    fprintf(stderr,"\n");
}

static void reset_sheet(KCtx *g,u64 sheet){
    memset(hp(g,sheet),0,0x278); wr32(g,GS_BASE+0x2a23950,0); wr32(g,sheet+0x260,2); wr32(g,sheet+0x264,100);
}
/* exe 3c0c30 without an actor (3c0c92..3c0cac), lane 0: cur+amount, raised to the floor at +8, else capped at the maximum. */
static s32 model_hp(s32 cur,s32 amount,s32 floor,s32 max){ s32 v=(s32)((u32)cur+(u32)amount); return v<floor?floor:v<=max?v:max; }
/* exe 3c0cd0 without an actor and without an MP charge (3c0da8..3c0dc6): negative sums become 0, else capped at the maximum. */
static s32 model_mp(s32 cur,s32 amount,s32 max){ s32 v=(s32)((u32)cur+(u32)amount); return v<0?0:v<=max?v:max; }

/* Run a call in a child and return its stderr; the child must die through a named fatal (exit status neither 0 nor 99). */
static int fatal_child(KCtx *g,int usable,u64 effect,u32 target,const char *needle1,const char *needle2){
    FILE *log=tmpfile(); if(!log) kh_fatal(g,"items selftest cannot open a fatal log");
    pid_t child=fork();
    if(child<0) kh_fatal(g,"items selftest fork failed");
    if(!child){ dup2(fileno(log),2); kh_call(g,GS_BASE+(usable?0x3c3da0:0x3c4b80),effect,target,0,100); _exit(99); }
    int status=0; waitpid(child,&status,0);
    rewind(log); char output[4096]; size_t len=fread(output,1,sizeof output-1,log); output[len]=0; fclose(log);
    int ok=WIFEXITED(status)&&WEXITSTATUS(status)!=0&&WEXITSTATUS(status)!=99&&strstr(output,needle1)&&(!needle2||strstr(output,needle2));
    if(!ok) fprintf(stderr,"  fatal child status=%d output: %.300s\n",status,output);
    return ok;
}

int kh_mod_items_selftest(KCtx *g,u64 sheet){
    u32 olddiff=rd32(g,GS_BASE+0x9abd48);
    u64 rec=ga_calloc(1,0x18),fakecmd=ga_calloc(1,0x30),sp=KH_RSP(g);
    if(!rec || !fakecmd) kh_fatal(g,"items selftest allocation failed");
    struct kh_item_call e[KH_ITEM_MAXCALLS];
    struct fake_user user={.g=g,.sheet=sheet,.cmd=fakecmd};
    struct kh_item_mock mock={.fake=fake,.user=&user};
    struct kh_item_counts before,after;
    static const u32 users[]={0,2,1,0xffffffffu};          /* target is 2: "2" is a self-use */
    static const u16 cmdwords[]={23,0,0x7fff,0x8000,0xffff};
    unsigned run=0;

    /* The port's divider against plain C division on the numerators the tables produce is covered by every transcript below;
     * the boundaries of the two magic-number sequences are pinned here through the hook itself (pct 1, mult 1, max = n-9999). */
    kh_mod_items_reset();

    /* ---- (1a) transcript, restore ids 1..7 x command byte x percent x multiplier x maximum */
    kh_item_mock_sink=&mock;
    for(unsigned id=1;id<=7;id++) for(int b=0;b<KH_ITEM_N(kh_item_bytes);b++) for(int p=0;p<KH_ITEM_N(kh_item_pcts);p++)
    for(int m=0;m<KH_ITEM_N(kh_item_mults);m++) for(int x=0;x<KH_ITEM_N(kh_item_maxes);x++,run++){
        u32 maxhp=kh_item_maxes[x],maxmp=kh_item_maxes[(x+4)%KH_ITEM_N(kh_item_maxes)],mult=kh_item_mults[m],pct=kh_item_pcts[p];
        u32 who=users[run%4],actor_id=run&8?0x1234:0; u16 cmdword=cmdwords[run%5]; unsigned byte=kh_item_bytes[b]; int d=(int)(run%4);
        reset_sheet(g,sheet); wr32(g,sheet+4,maxhp); wr32(g,sheet+0x184,maxmp); wr32(g,sheet+0x268,actor_id);
        memset(hp(g,rec),0,0x18); wr16(g,rec,id); wr8(g,rec+4,pct); wr16(g,rec+0x10,cmdword);
        wr8(g,fakecmd+0x2f,byte); wr32(g,GS_BASE+0x9abd48,d);
        mock.ncalls=mock.overflow=0; user.poke=0;
        u64 r=kh_call(g,GS_BASE+0x3c4b80,rec,2,who,mult);
        int n=expect_stock(e,sheet,2,who,mult,id,pct,cmdword,actor_id,maxhp,maxmp,maxmp,byte);
        int ok=same_calls(&mock,e,n)&&r==0x7e0&&KH_RSP(g)==sp;
        T(ok);
        if(!ok&&failures<=8){ fprintf(stderr,"  id=%u byte=%u pct=%u mult=%u maxhp=%u maxmp=%u user=%u\n",id,byte,pct,mult,maxhp,maxmp,who); describe("expected",e,n); describe("got",mock.c,mock.ncalls); }
    }
    /* Division boundaries: numerators 0, +-1 around every multiple of 10000 up to 3 and the int32 extremes, both divisors. */
    {
        static const s32 numers[]={0,1,-1,9999,10000,10001,-9999,-10000,-10001,19999,20000,20001,-19999,-20000,-20001,29999,30000,30001,
                                   INT32_MAX,INT32_MAX-1,INT32_MIN,INT32_MIN+1,2147480000,-2147480000,123456789,-123456789};
        for(unsigned i=0;i<sizeof numers/sizeof *numers;i++) for(int b=0;b<KH_ITEM_N(kh_item_bytes);b++){
            u32 max=(u32)numers[i]-9999u; unsigned byte=kh_item_bytes[b];         /* pct 1, mult 1: numerator = max + 9999 */
            reset_sheet(g,sheet); wr32(g,sheet+4,max); wr32(g,sheet+0x184,max);
            memset(hp(g,rec),0,0x18); wr16(g,rec,4); wr8(g,rec+4,1); wr16(g,rec+0x10,22); wr8(g,fakecmd+0x2f,byte);
            wr32(g,GS_BASE+0x9abd48,3); mock.ncalls=mock.overflow=0;
            kh_call(g,GS_BASE+0x3c4b80,rec,2,0,1);
            s32 want=byte==2?numers[i]/-10000:numers[i]/10000;
            T(mock.ncalls==7 && mock.c[4].rva==0x3c0c30 && (s32)(u32)mock.c[4].a[1]==want && mock.c[4].a[1]==(u32)want);
            T(mock.ncalls==7 && mock.c[5].rva==0x3c0cd0 && (s32)(u32)mock.c[5].a[1]==want);
        }
    }
    /* ---- (1b) transcript, the other ids: 0x83, 0x112..0x117 and ids the DLL only looks the command up for */
    static const u16 other_ids[]={0x83,0x112,0x113,0x114,0x115,0x116,0x117,0,8,9,13,14,0x82,0x84,0x101,0x102,0x105,0x106,0x111,0x118,0xffff};
    static const u32 other_mults[]={1,100,250,0xffffffffu};
    for(unsigned i=0;i<sizeof other_ids/sizeof *other_ids;i++) for(int b=0;b<KH_ITEM_N(kh_item_bytes);b++) for(int p=0;p<KH_ITEM_N(kh_item_pcts);p++)
    for(unsigned m=0;m<4;m++) for(int x=0;x<KH_ITEM_N(kh_item_maxes);x++,run++){
        u32 maxhp=kh_item_maxes[x],maxmp=kh_item_maxes[(x+7)%KH_ITEM_N(kh_item_maxes)],mult=other_mults[m],pct=kh_item_pcts[p];
        u32 who=users[run%4]; u16 cmdword=cmdwords[run%5]; unsigned id=other_ids[i],byte=kh_item_bytes[b];
        reset_sheet(g,sheet); wr32(g,sheet+4,maxhp); wr32(g,sheet+0x184,maxmp);
        memset(hp(g,rec),0,0x18); wr16(g,rec,id); wr8(g,rec+4,pct); wr16(g,rec+0x10,cmdword);
        wr8(g,fakecmd+0x2f,byte); wr32(g,GS_BASE+0x9abd48,run%4);
        mock.ncalls=mock.overflow=0; user.poke=1; user.poke_mp=maxmp^0x55;   /* the HP helper changes max MP: item 0x83 must re-read it */
        u64 r=kh_call(g,GS_BASE+0x3c4b80,rec,2,who,mult);
        int n=expect_stock(e,sheet,2,who,mult,id,pct,cmdword,0,maxhp,maxmp,maxmp^0x55,byte);
        int ok=same_calls(&mock,e,n)&&r==0x7e0;
        T(ok);
        if(!ok&&failures<=8){ fprintf(stderr,"  id=0x%x byte=%u pct=%u mult=%u maxhp=%u maxmp=%u user=%u\n",id,byte,pct,mult,maxhp,maxmp,who); describe("expected",e,n); describe("got",mock.c,mock.ncalls); }
    }
    user.poke=0;
    /* Counters: every application above was counted once in its id slot; reversed counts only ids 1..7 with byte 2. */
    kh_mod_items_counts(&after);
    {
        u64 total=0,reversed=0,restore=0,others=0;
        for(int d=0;d<4;d++){ reversed+=after.reversed[d]; for(int s=0;s<KH_ITEM_SLOTS;s++){ total+=after.apply[d][s]; if(s<7) restore+=after.apply[d][s]; else others+=after.apply[d][s]; } }
        u64 per=(u64)KH_ITEM_N(kh_item_pcts)*KH_ITEM_N(kh_item_mults)*KH_ITEM_N(kh_item_maxes);
        T(total==run+26u*KH_ITEM_N(kh_item_bytes));
        T(restore==7u*KH_ITEM_N(kh_item_bytes)*per+26u*KH_ITEM_N(kh_item_bytes));
        T(reversed==7u*per+26u);
        T(others==total-restore);
        for(int d=0;d<4;d++) T(after.apply[d][0]>0 && after.apply[d][7]>0 && after.apply[d][KH_ITEM_SLOTS-1]>0);
        T(after.hp_calls>0 && after.mp_calls>0 && after.noop[0]+after.noop[1]+after.noop[2]+after.noop[3]==0);
    }
    /* Named fatals of the apply path: a null sheet and a null command record (shipped DLL faults at both). */
    {
        memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr8(g,rec+4,40); wr16(g,rec+0x10,23); wr32(g,GS_BASE+0x9abd48,3);
        user.null_sheet=1; T(fatal_child(g,0,rec,2,"mod item effect: no sheet for character 2","0x31bb6")); user.null_sheet=0;
        user.null_cmd=1; T(fatal_child(g,0,rec,2,"mod item effect: no cmd record for command 23 of item 0x1","0x31482")); user.null_cmd=0;
    }
    kh_item_mock_sink=NULL;

    /* ---- (2a) real helpers, ids 1..7 x {normal, reversed}: final HP/MP on a sheet without an actor */
    static const struct { s32 hp,maxhp,floor,mp,maxmp; } sheets[]={
        {30,60,0,10,120},{60,60,0,120,120},{1,90,0,0,140},{90,180,0,70,180},
        {24,60,0,99,100},        /* Potion 40% of 60 = 24: the reversed Potion lands exactly on 0 */
        {25,60,0,100,100},{200,255,1,255,255},{5,120,5,3,220},{0,0,0,0,0},{100,1000,-50,500,1000},
    };
    static const u32 real_mults[]={0,1,100,150,250};
    static const u16 real_cmds[]={0,23,20,21,22,242,243,244};     /* index = item id, as shipped */
    u64 orig_apply=kh_host_fn("items-test-original-apply",orig_f_3c4b80);
    u64 orig_usable=kh_host_fn("items-test-original-usable",orig_f_3c3da0);
    unsigned exact_zero=0,went_negative_hp=0;
    for(unsigned s=0;s<sizeof sheets/sizeof *sheets;s++) for(unsigned id=1;id<=7;id++) for(int reversed=0;reversed<2;reversed++)
    for(int p=0;p<KH_ITEM_N(kh_item_pcts);p++) for(unsigned m=0;m<5;m++,run++){
        u32 pct=kh_item_pcts[p],mult=real_mults[m],who=users[run%4]; u64 cmd=cmd_record(g,real_cmds[id]);
        s32 hp_amount=kh_item_ref_amount(kh_item_ref_numer(pct,mult,(u32)sheets[s].maxhp),reversed?2:0);
        s32 mp_amount=kh_item_ref_amount(kh_item_ref_numer(pct,mult,(u32)sheets[s].maxmp),reversed?2:0);
        int does_hp=id==1||id==2||id==5||id==4||id==7,does_mp=id==3||id==6||id==4||id==7;
        s32 want_hp=does_hp?model_hp(sheets[s].hp,hp_amount,sheets[s].floor,sheets[s].maxhp):sheets[s].hp;
        s32 want_mp=does_mp?model_mp(sheets[s].mp,mp_amount,sheets[s].maxmp):sheets[s].mp;
        for(int pass=0;pass<(reversed?1:2);pass++){              /* pass 1: the vanilla original on the same state (byte 0 only) */
            reset_sheet(g,sheet);
            wr32(g,sheet,(u32)sheets[s].hp); wr32(g,sheet+4,(u32)sheets[s].maxhp); wr32(g,sheet+8,(u32)sheets[s].floor);
            wr32(g,sheet+0x180,(u32)sheets[s].mp); wr32(g,sheet+0x184,(u32)sheets[s].maxmp);
            memset(hp(g,rec),0,0x18); wr16(g,rec,id); wr8(g,rec+4,pct); wr16(g,rec+0x10,real_cmds[id]);
            wr8(g,cmd+0x2f,reversed?2:0); wr32(g,GS_BASE+0x9abd48,reversed?3:run%4);
            kh_call(g,pass?orig_apply:GS_BASE+0x3c4b80,rec,2,who,mult);
            T((s32)rd32(g,sheet)==want_hp);
            T((s32)rd32(g,sheet+0x180)==want_mp);
            T(rd32(g,sheet+4)==(u32)sheets[s].maxhp && rd32(g,sheet+0x184)==(u32)sheets[s].maxmp && rd32(g,sheet+8)==(u32)sheets[s].floor
              && rd32(g,sheet+0x264)==100 && KH_RSP(g)==sp);
        }
        wr8(g,cmd+0x2f,0);
        if(reversed && does_hp && want_hp==0 && sheets[s].hp>0) exact_zero++;
        if(reversed && does_hp && hp_amount<0) went_negative_hp++;
    }
    T(exact_zero>0);            /* a reversed restore item can take HP to exactly 0 (floor 0) */
    T(went_negative_hp>0);
    /* Shipped numbers, spelled out: max HP 60, Potion 40%, multiplier 100. */
    {
        u64 cmd=cmd_record(g,23); memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr8(g,rec+4,40); wr16(g,rec+0x10,23);
        for(int reversed=0;reversed<2;reversed++){
            reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60); wr32(g,sheet+0x180,7); wr32(g,sheet+0x184,120);
            wr8(g,cmd+0x2f,reversed?2:0); wr32(g,GS_BASE+0x9abd48,reversed?3:1);
            kh_call(g,GS_BASE+0x3c4b80,rec,2,0,100);
            T(rd32(g,sheet)==(reversed?6u:54u)); T(rd32(g,sheet+0x180)==7);
            /* The byte alone decides, not the difficulty: byte 2 on Standard still drains, byte 0 on Less Than Nothing still heals. */
            reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60);
            wr32(g,GS_BASE+0x9abd48,reversed?1:3);
            kh_call(g,GS_BASE+0x3c4b80,rec,2,0,100);
            T(rd32(g,sheet)==(reversed?6u:54u));
        }
        /* Elixir (255%) reversed: HP to the floor and MP to 0 in one use; Megalixir record the same per target. */
        wr16(g,rec,4); wr8(g,rec+4,255); wr16(g,rec+0x10,22); cmd=cmd_record(g,22); wr8(g,cmd+0x2f,2);
        reset_sheet(g,sheet); wr32(g,sheet,60); wr32(g,sheet+4,60); wr32(g,sheet+0x180,120); wr32(g,sheet+0x184,120);
        wr32(g,GS_BASE+0x9abd48,3); kh_call(g,GS_BASE+0x3c4b80,rec,2,0,100);
        T(rd32(g,sheet)==0); T(rd32(g,sheet+0x180)==0);
        wr8(g,cmd+0x2f,0); wr8(g,cmd_record(g,23)+0x2f,0);
    }
    /* A reversed Ether during an MP charge: the no-actor branch of 3c0cd0 leaves a non-positive amount alone (3c0d30). */
    {
        u64 cmd=cmd_record(g,21); memset(hp(g,rec),0,0x18); wr16(g,rec,3); wr8(g,rec+4,100); wr16(g,rec+0x10,21); wr8(g,cmd+0x2f,2);
        reset_sheet(g,sheet); wr32(g,sheet+0x180,0); wr32(g,sheet+0x184,120); wr32(g,sheet+0x1bc,0x40a00000); wr32(g,sheet+0x1c0,0x41200000);
        wr32(g,GS_BASE+0x9abd48,3); kh_call(g,GS_BASE+0x3c4b80,rec,2,0,100);
        T(rd32(g,sheet+0x180)==0 && rd32(g,sheet+0x1bc)==0x40a00000 && rd32(g,sheet+0x1c0)==0x41200000);
        wr8(g,cmd+0x2f,0);
    }
    /* ---- (2b) item 0x83 with real helpers: full HP, and the MP amount is the undivided product (vanilla would add 1 here) */
    {
        memset(hp(g,rec),0,0x18); wr16(g,rec,0x83); wr8(g,rec+4,1); wr16(g,rec+0x10,0);
        for(int pass=0;pass<2;pass++){
            reset_sheet(g,sheet); wr32(g,sheet,3); wr32(g,sheet+4,60); wr32(g,sheet+0x180,10); wr32(g,sheet+0x184,50);
            wr32(g,GS_BASE+0x9abd48,1);
            kh_call(g,pass?orig_apply:GS_BASE+0x3c4b80,rec,2,0,1);
            T(rd32(g,sheet)==60);
            T(rd32(g,sheet+0x180)==(pass?11u:50u));
            T(rd32(g,sheet+0x264)==100);
        }
    }
    /* ---- (2c) an invalid difficulty still enters the complete original (existing gate), reversed byte or not */
    {
        static const s32 invalid[]={-1,4,INT32_MAX}; u64 cmd=cmd_record(g,23);
        memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr8(g,rec+4,40); wr16(g,rec+0x10,23); wr8(g,cmd+0x2f,2);
        kh_mod_items_counts(&before);
        for(unsigned i=0;i<3;i++){
            reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60); wr32(g,GS_BASE+0x9abd48,(u32)invalid[i]);
            kh_call(g,GS_BASE+0x3c4b80,rec,2,0,100);
            T(rd32(g,sheet)==54); T(rd32(g,sheet+0x264)==100);
            T((kh_call(g,GS_BASE+0x3c3da0,rec,2,0,0)&255)==1); T(rd32(g,sheet+0x264)==100);
        }
        kh_mod_items_counts(&after);
        T(!memcmp(&before,&after,sizeof before));
        wr8(g,cmd+0x2f,0);
    }
    /* ---- (2d) DLL no-op apply kinds 25..29 and >31: nothing happens even for a stock id (the vanilla switch would heal) */
    {
        static const u8 noop_kinds[]={25,26,27,28,29,32,33,100,255};
        memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr8(g,rec+4,40); wr16(g,rec+0x10,23);
        kh_mod_items_counts(&before);
        for(unsigned i=0;i<sizeof noop_kinds;i++) for(int d=0;d<4;d++){
            reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60); wr32(g,sheet+0x180,5); wr32(g,sheet+0x184,120);
            wr8(g,rec+5,noop_kinds[i]); wr32(g,GS_BASE+0x9abd48,d);
            kh_call(g,GS_BASE+0x3c4b80,rec,2,1,100);
            T(rd32(g,sheet)==30 && rd32(g,sheet+0x180)==5 && rd32(g,sheet+4)==60 && rd32(g,sheet+0x184)==120);
            T(rd32(g,sheet+0x264)==100 && KH_RSP(g)==sp);
        }
        kh_mod_items_counts(&after);
        for(int d=0;d<4;d++) T(after.noop[d]==before.noop[d]+sizeof noop_kinds);
        /* An active kind is never forwarded to the vanilla function, stock id or not. */
        static const u8 active_apply[]={1,2,3,10,13,24,30,31},active_usable[]={1,2,3,10,13,21,23,24};
        wr32(g,GS_BASE+0x9abd48,3); reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60);
        for(unsigned i=0;i<sizeof active_apply;i++){
            char want[32]; snprintf(want,sizeof want,"id=0x1 kind=%u",active_apply[i]); wr8(g,rec+5,active_apply[i]);
            T(fatal_child(g,0,rec,2,"unimplemented mod item effect DLL RVA=0x31260",want));
        }
        for(unsigned i=0;i<sizeof active_usable;i++){
            char want[32]; snprintf(want,sizeof want,"id=0x1 kind=%u",active_usable[i]); wr8(g,rec+5,active_usable[i]);
            T(fatal_child(g,1,rec,2,"unimplemented mod item effect DLL RVA=0x30f60",want));
        }
        /* A null sheet with the real acquire (character 0). */
        wr8(g,rec+5,0); T(fatal_child(g,0,rec,0,"mod item effect: no sheet for character 0",NULL));
        wr8(g,rec+5,25); T(fatal_child(g,0,rec,0,"mod item effect: no sheet for character 0",NULL));
        wr8(g,rec+5,0);
    }
    /* ---- (2e) usability: the command byte is not consulted, the answer is the original's, and the handler's own acquisition
     * stays (one extra reference per call with a sheet; none without). */
    {
        static const struct { s32 hp,maxhp,mp,maxmp; u32 charge; u8 drive,drivemax; } states[]={
            {30,60,10,120,0,0,0},{60,60,120,120,0,5,5},{60,60,0,120,0x41200000,2,5},{1,60,119,120,0,0,3},{61,60,121,120,0,7,5},
        };
        static const u16 ids[]={1,2,3,4,5,6,7,0x83,0x112,0x113,0,8};
        static const u8 kinds[]={0,22,25,255};
        kh_mod_items_counts(&before); unsigned calls=0,usable_yes=0,usable_no=0;
        for(unsigned s=0;s<sizeof states/sizeof *states;s++) for(unsigned i=0;i<sizeof ids/sizeof *ids;i++) for(int byte=0;byte<=2;byte+=2)
        for(unsigned k=0;k<sizeof kinds;k++) for(int d=0;d<4;d++){
            unsigned id=ids[i]; u16 cmdword=id>=1&&id<=7?real_cmds[id]:0; u64 cmd=cmd_record(g,cmdword); u64 r[2];
            memset(hp(g,rec),0,0x18); wr16(g,rec,id); wr8(g,rec+4,40); wr8(g,rec+5,kinds[k]); wr16(g,rec+0x10,cmdword); wr8(g,cmd+0x2f,byte);
            wr32(g,GS_BASE+0x9abd48,d);
            for(int pass=0;pass<2;pass++){
                reset_sheet(g,sheet); wr32(g,sheet,(u32)states[s].hp); wr32(g,sheet+4,(u32)states[s].maxhp);
                wr32(g,sheet+0x180,(u32)states[s].mp); wr32(g,sheet+0x184,(u32)states[s].maxmp); wr32(g,sheet+0x1c0,states[s].charge);
                wr8(g,sheet+0x1b1,states[s].drive); wr8(g,sheet+0x1b2,states[s].drivemax);
                r[pass]=kh_call(g,pass?orig_usable:GS_BASE+0x3c3da0,rec,2,0,0);
                T(rd32(g,sheet+0x264)==(pass?100u:101u));
            }
            T((r[0]&255)==(r[1]&255)); T(KH_RSP(g)==sp);
            if(r[0]&255) usable_yes++; else usable_no++;
            wr8(g,cmd+0x2f,0); calls++;
        }
        T(usable_yes>0 && usable_no>0);
        kh_mod_items_counts(&after);
        T(after.usable[0]+after.usable[1]+after.usable[2]+after.usable[3]==before.usable[0]+before.usable[1]+before.usable[2]+before.usable[3]+calls);
        /* No sheet (character 0): the original answers 0 and nothing is retained anywhere. */
        memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr16(g,rec+0x10,23); wr32(g,GS_BASE+0x9abd48,3); reset_sheet(g,sheet);
        T((kh_call(g,GS_BASE+0x3c3da0,rec,0,0,0)&255)==0); T(rd32(g,sheet+0x264)==100);
    }
    /* ---- (2f) each path pops exactly its own return address (kh_call restores RSP itself, so enter the hooks directly) */
    {
        u64 cmd=cmd_record(g,23); memset(hp(g,rec),0,0x18); wr16(g,rec,1); wr8(g,rec+4,40); wr16(g,rec+0x10,23); wr8(g,cmd+0x2f,2);
        for(int path=0;path<3;path++){
            reset_sheet(g,sheet); wr32(g,sheet,30); wr32(g,sheet+4,60); wr32(g,GS_BASE+0x9abd48,3); wr8(g,rec+5,path==1?25:0);
            u64 save=KH_RSP(g),rcx=KH_RCX(g),rdx=KH_RDX(g),r8=KH_R8(g),r9=KH_R9(g),entry=((save-0x100)&~15ull)-8;
            wr64(g,entry,0x1122334455667788ull); KH_RSP(g)=entry; KH_RCX(g)=rec; KH_RDX(g)=2; KH_R8(g)=0; KH_R9(g)=100;
            if(path==2) f_3c3da0(g); else f_3c4b80(g);
            T(KH_RSP(g)==entry+8); T(rd64(g,entry)==0x1122334455667788ull);
            KH_RSP(g)=save; KH_RCX(g)=rcx; KH_RDX(g)=rdx; KH_R8(g)=r8; KH_R9(g)=r9;
        }
        wr8(g,rec+5,0); wr8(g,cmd+0x2f,0);
    }
    /* ---- (3) WriteByte observer: only stores to +2f of a record of the live cmd table count */
    {
        u64 table=rd64(g,GS_BASE+0x2a161e8),first=table+8; kh_mod_items_reset();
        kh_mod_item_writebyte(g,first+0x30*1+0x2f,2); kh_mod_item_writebyte(g,first+0x30*4+0x2f,2); kh_mod_item_writebyte(g,first+0x30*4+0x2f,0);
        kh_mod_item_writebyte(g,first+0x30*(NCMD-1)+0x2f,7);
        kh_mod_items_counts(&after);
        T(after.cmd2f_writes==4 && after.cmd2f_two==2 && after.cmd2f_zero==1 && after.cmd2f_other==1);
        kh_mod_item_writebyte(g,first+0x2e,2); kh_mod_item_writebyte(g,first+0x30,2); kh_mod_item_writebyte(g,table+7,2);
        kh_mod_item_writebyte(g,first+0x30*NCMD+0x2f,2); kh_mod_item_writebyte(g,rec,2); kh_mod_item_writebyte(g,0,2);
        kh_mod_items_counts(&before);
        T(before.cmd2f_writes==4 && before.cmd2f_two==2);
        wr64(g,GS_BASE+0x2a161e8,0); kh_mod_item_writebyte(g,first+0x2f,2); wr64(g,GS_BASE+0x2a161e8,table);
        kh_mod_items_counts(&before); T(before.cmd2f_writes==4);
    }
    kh_mod_items_reset();
    wr32(g,GS_BASE+0x9abd48,olddiff); ga_free(rec); ga_free(fakecmd);
    fprintf(stderr,"kh: difficulty items selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
