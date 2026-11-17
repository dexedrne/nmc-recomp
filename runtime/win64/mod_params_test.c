/* Task 11 selftest for runtime/mod_params.c: three kinds of evidence.
 *  1. transcripts: thousands of generated cases run through the port's f_40d810 / f_40dd70 with the call sink, hashed (regression golden;
 *     tools/native_params_oracle.c is the DLL-side counterpart that was written but NOT executed, see EXPECT_* below; kinds 64/65 are
 *     excluded from transcripts because they run std::unordered_map + TLS static-init code).
 *  2. hand-written expectations per kind / branch (call sequences with the sink, memory effects with the real lifted callees).
 *  3. kinds 64/65 map lifetime, forwarding to the real lifted originals, and named fatals (forked). */
#include "mod_config.h"
#include "mod_params_cases.h"
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>

static int checks, failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod params selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)

/* Transcript hashes.  REGRESSION GOLDENS of this port (recorded 2026-11-16), NOT oracle-validated: tools/native_params_oracle.c would
 * print the DLL's own transcript for the same cases, but executing the third-party DLL was not permitted in the Task 11 session (see implementation notes).
 * If an oracle run ever disagrees, the oracle wins: fix mod_params.c, then re-record. */
#define EXPECT_RANGE_CASES 3600
#define EXPECT_FRAME_CASES 3720
#define EXPECT_RANGE_HASH 0x5d45fbf33da7e1c1ull
#define EXPECT_FRAME_HASH 0x73471e28cc7fa6caull

static u64 mock_fake(void *user,const struct kh_mp_call *c){ return mpc_fake((const struct mpc_case*)user,c->rva); }

static uint64_t transcript(KCtx *g,u64 arena,int range,int cases,FILE *dump,int *ran){
    uint64_t h=0xcbf29ce484222325ull; static char line[8192]; struct mpc_case *c=malloc(sizeof *c);
    *ran=0;
    for(int i=0;i<cases;i++){
        mpc_make(c,range,(uint32_t)i,arena);
        if(range&&(c->kind==64||c->kind==65)) continue;
        memcpy(hp(g,arena),c->buf,MPC_ARENA);
        struct kh_mp_mock mock; memset(&mock,0,sizeof mock); mock.fake=mock_fake; mock.user=c;
        v128 x2=VZ; memcpy(&x2,&c->xmm2,4); KH_XMM(g,2)=x2;
        kh_mp_mock_sink=&mock;
        u64 sp=KH_RSP(g);
        kh_call(g,GS_BASE+(range?0x40d810:0x40dd70),arena+MPC_R,arena+MPC_C,0,0);
        kh_mp_mock_sink=NULL;
        T(KH_RSP(g)==sp); T(!mock.overflow);
        size_t n=mpc_line(line,sizeof line,c,mock.c,mock.ncalls,hp(g,arena));
        h=mpc_fnv(h,line,n); h=mpc_fnv(h,"\n",1);
        if(dump) fprintf(dump,"%s\n",line);
        (*ran)++;
    }
    free(c); return h;
}

/* ---- hand-written fixture (own layout, own values; independent of the generator) -------------------------------------- */
static u64 AR;                       /* arena guest address */
static struct kh_mp_mock mock;
static u64 fake_S, fake_T, fake_rec3eaae0, fake_rec3b68b0, fake_id;
static u64 hand_fake(void *u,const struct kh_mp_call *c){
    (void)u;
    switch(c->rva){ case 0x39b760: return fake_S; case 0x3d74e0: return fake_T; case 0x3eaae0: return fake_rec3eaae0; case 0x3b68b0: return fake_rec3b68b0; case 0x4ad240: return fake_id; default: return 0x77; }
}
#define OBJ(o) (AR+(o))
static void fixture(KCtx *g){
    memset(hp(g,AR),0,MPC_ARENA);
    wr64(g,OBJ(MPC_C+0x10),OBJ(MPC_A)); wr64(g,OBJ(MPC_C+0xd8),OBJ(MPC_OBJD8)); wr64(g,OBJ(MPC_C+0xe0),OBJ(MPC_OBJE0));
    wr64(g,OBJ(MPC_A+0x148),OBJ(MPC_T148)); wr64(g,OBJ(MPC_A+0x5b0),OBJ(MPC_SCRIPT)); wr64(g,OBJ(MPC_A+0x5c0),OBJ(MPC_SHEET));
    wr64(g,OBJ(MPC_A+0x7b0),OBJ(MPC_Q7B0)); wr64(g,OBJ(MPC_A+0x918),OBJ(MPC_Q918)); wr64(g,OBJ(MPC_A+0xdc8),OBJ(MPC_V));
    fake_S=OBJ(MPC_S); fake_T=OBJ(MPC_TACT); fake_rec3eaae0=OBJ(MPC_REC3EAA); fake_rec3b68b0=OBJ(MPC_REC3B68); fake_id=0xabcd;
}
#define A_ OBJ(MPC_A)
#define C_ OBJ(MPC_C)
#define R_ OBJ(MPC_R)
#define SH_ OBJ(MPC_SHEET)
#define S_ OBJ(MPC_S)
#define V_ OBJ(MPC_V)
static void setf(KCtx *g,u64 a,float f){ u32 v; memcpy(&v,&f,4); wr32(g,a,v); }
static float getf(KCtx *g,u64 a){ u32 v=rd32(g,a); float f; memcpy(&f,&v,4); return f; }
static void run(KCtx *g,int range,int mocked){
    memset(&mock,0,sizeof mock); mock.fake=hand_fake;
    kh_mp_mock_sink=mocked?&mock:NULL;
    u64 sp=KH_RSP(g);
    kh_call(g,GS_BASE+(range?0x40d810:0x40dd70),R_,C_,0,0);
    kh_mp_mock_sink=NULL;
    T(KH_RSP(g)==sp);
}
static void rrec(KCtx *g,int kind,u16 p1,u16 p2,u16 p3,u16 p4,u16 p5,u8 end){
    wr8(g,R_+2,end); wr8(g,R_+4,(u8)kind); wr16(g,R_+6,p1); wr16(g,R_+8,p2); wr16(g,R_+10,p3); wr16(g,R_+12,p4); wr16(g,R_+14,p5);
}
static void frec(KCtx *g,int op,s16 x,s16 y){ wr8(g,R_+2,(u8)op); wr16(g,R_+4,(u16)x); wr16(g,R_+6,(u16)y); }
/* mock log helpers */
static int callrva(int i){ return i<mock.ncalls?(int)mock.c[i].rva:-1; }
static int only_orig(int range){ return mock.ncalls==1 && mock.c[0].rva==(range?0x40d810u:0x40dd70u) && mock.c[0].a[0]==R_ && mock.c[0].a[1]==C_; }
static u32 f2u(float f){ u32 v; memcpy(&v,&f,4); return v; }

static void range_hand(KCtx *g){
    const u32 R0=0x40d810;
    /* out-of-table kinds (signed byte): forwarded untouched, original runs once */
    const int outside[]={-128,-1,0,1,16,53,84,100,127,0xff-256};
    for(unsigned i=0;i<sizeof outside/sizeof *outside;i++){
        fixture(g); rrec(g,outside[i],1,2,3,4,5,9); wr8(g,C_+0x154,0x0f);
        run(g,1,1); T(only_orig(1)); T(rd8(g,C_+0x154)==0x0f);
    }
    /* 54/55: bit 33 of A+0x120 only */
    fixture(g); wr64(g,A_+0x120,0xffffffffffffffffull); rrec(g,54,0,0,0,0,0,0); run(g,1,1); T(rd64(g,A_+0x120)==0xfffffffdffffffffull); T(only_orig(1));
    rrec(g,55,0,0,0,0,0,0); run(g,1,1); T(rd64(g,A_+0x120)==0xffffffffffffffffull);
    wr64(g,A_+0x120,0); run(g,1,1); T(rd64(g,A_+0x120)==0x200000000ull);
    /* 56..59: C+0x154 |= 0x10<<(kind-56) */
    for(int k=56;k<=59;k++){ fixture(g); wr8(g,C_+0x154,0x05); rrec(g,k,0,0,0,0,0,0); run(g,1,1); T(rd8(g,C_+0x154)==(u8)(5|(0x10<<(k-56)))); T(only_orig(1)); }
    /* explicit no-op kinds: nothing but the original */
    const int noop[]={63,69,71,72,73,74,75,76,77,78,79,81};
    for(unsigned i=0;i<sizeof noop/sizeof *noop;i++){ fixture(g); rrec(g,noop[i],0,0,0,0,0,0); run(g,1,1); T(only_orig(1)); }
    /* 60: S.q8 & signext32(1<<(P2&31)), P1==0, actor flag bit 7 */
    fixture(g); wr32(g,A_+0x6c8,0x80); wr64(g,S_+8,1ull<<3); rrec(g,60,0,3,(u16)-5,0,0,0); run(g,1,1);
    T(mock.ncalls==5); T(callrva(0)==0x39b760&&callrva(1)==0x3bde30&&callrva(2)==0x3d74e0&&callrva(3)==0x3d78d0&&callrva(4)==(int)R0);
    T(mock.c[1].a[0]==V_+4); T(mock.c[2].a[0]==A_&&(u32)mock.c[2].a[1]==(u32)-5); T(mock.c[3].a[0]==A_&&mock.c[3].a[1]==fake_T&&mock.c[3].a[2]==V_+4);
    rrec(g,60,0,35,3,0,0,0); run(g,1,1); T(mock.ncalls==5);               /* only the low 5 bits of P2 select the bit */
    rrec(g,60,0,3,3,0,0,0); wr64(g,S_+8,0); run(g,1,1); T(mock.ncalls==2&&callrva(0)==0x39b760);   /* mask test fails: S read, then original */
    rrec(g,60,0,0,3,0,0,0); run(g,1,1); T(mock.ncalls==5);                /* P2==0 skips the mask test */
    rrec(g,60,1,0,3,0,0,0); run(g,1,1); T(mock.ncalls==2);                /* P1 != 0 */
    rrec(g,60,0,0,3,0,0,0); wr32(g,A_+0x6c8,0x7f); run(g,1,1); T(mock.ncalls==2);   /* actor bit 7 clear */
    wr32(g,A_+0x6c8,0x80); fake_T=0; run(g,1,1); T(mock.ncalls==4&&callrva(2)==0x3d74e0);  /* lookup fails: no move */
    fixture(g); fake_S=0; wr32(g,A_+0x6c8,0x80); rrec(g,60,0,0,3,0,0,0); run(g,1,1); T(mock.ncalls==2);       /* S null */
    /* sign-extended 32-bit mask quirk: P2&31==31 gives 0xffffffff80000000, which also hits S.q8 bits 32..63 */
    fixture(g); wr32(g,A_+0x6c8,0x80); wr64(g,S_+8,1ull<<40); rrec(g,60,0,31,0,0,0,0); run(g,1,1); T(mock.ncalls==5);
    wr64(g,S_+8,1ull<<20); run(g,1,1); T(mock.ncalls==2);
    /* 61: two motion helpers with 0.0f floats and sign-extended P3/P4 */
    fixture(g); wr64(g,S_+8,1ull<<2); rrec(g,61,0,2,0x8001,0x7fff,0,0); run(g,1,1);
    T(mock.ncalls==4&&callrva(0)==0x39b760&&callrva(1)==0x3c6dc0&&callrva(2)==0x3c8310&&callrva(3)==(int)R0);
    T(mock.c[1].a[0]==C_&&(u32)mock.c[1].a[1]==0xffff8001u&&mock.c[1].f2==0&&mock.c[1].f3==0);
    T(mock.c[2].a[0]==C_&&(u32)mock.c[2].a[1]==0x7fffu&&mock.c[2].a[4]==0&&mock.c[2].f2==0&&mock.c[2].f3==0);
    rrec(g,61,0,2,0x8001,0x7fff,0,0); wr64(g,S_+8,0); run(g,1,1); T(mock.ncalls==2);
    /* 62: the mask is 1>>(P1&31) (SAR in the DLL): only P1&31==0 passes; sheet+0x1d0 bit 0 must be set */
    fixture(g); wr32(g,SH_+0x1d0,1); wr64(g,S_+8,0xffffffffffffffffull); rrec(g,62,0,0,0,0x1234,0x8002,0); run(g,1,1);
    T(mock.ncalls==4&&callrva(1)==0x3c6dc0&&callrva(2)==0x3c8310); T((u32)mock.c[1].a[1]==0x1234u&&(u32)mock.c[2].a[1]==0xffff8002u);
    rrec(g,62,1,0,0,0x1234,2,0); run(g,1,1); T(mock.ncalls==2);          /* SHL would have matched sheet bit 0 only for P1==0; SAR for P1==1 gives 0 */
    rrec(g,62,32,0,0,0x1234,2,0); run(g,1,1); T(mock.ncalls==4);         /* 32&31==0 */
    rrec(g,62,0,1,0,0x1234,2,0); run(g,1,1); T(mock.ncalls==2);          /* P2 must be 0 */
    wr32(g,SH_+0x1d0,2); rrec(g,62,0,0,0,0x1234,2,0); run(g,1,1); T(mock.ncalls==2);
    /* 66/67/68 */
    fixture(g); rrec(g,66,0x8001,0,0,0,0,0); run(g,1,1); T(mock.ncalls==2&&callrva(0)==0x3c0ad0&&mock.c[0].a[0]==SH_&&(u32)mock.c[0].a[1]==0xffff8001u);
    rrec(g,67,5,0,0,0,0,0); run(g,1,1); T(mock.ncalls==2&&callrva(0)==0x3c0c30&&mock.c[0].a[0]==SH_&&(u32)mock.c[0].a[1]==5&&mock.c[0].a[2]==0&&mock.c[0].a[3]==0);
    rrec(g,68,0xfffe,0,0,0,0,0); run(g,1,1); T(mock.ncalls==2&&callrva(0)==0x3c0cd0&&(u32)mock.c[0].a[1]==0xfffffffeu&&mock.c[0].a[2]==0);
    /* 70: id, then script entry 0x11 with a zero-padded 3-qword argument array {id, sx16 P1, sx16 P2} */
    fixture(g); rrec(g,70,0xffff,0x1234,0,0,0,0); run(g,1,1);
    T(mock.ncalls==3&&callrva(0)==0x4ad240&&callrva(1)==0x3e1840); T(mock.c[0].a[0]==A_);
    T(mock.c[1].a[0]==OBJ(MPC_SCRIPT)&&mock.c[1].a[1]==0x11&&mock.c[1].a[3]==3&&mock.c[1].a[4]==0);
    T(mock.c[1].mem[0]==0xabcd&&mock.c[1].mem[1]==0xffffffffull&&mock.c[1].mem[2]==0x1234);
    /* 80: A+0x674 +/- P2/100 by P1 (1 add, 0 subtract, else nothing) */
    fixture(g); setf(g,A_+0x674,10.0f); rrec(g,80,1,250,0,0,0,0); run(g,1,1); T(getf(g,A_+0x674)==12.5f); T(only_orig(1));
    rrec(g,80,0,(u16)-100,0,0,0,0); run(g,1,1); T(getf(g,A_+0x674)==13.5f);
    rrec(g,80,2,50,0,0,0,0); run(g,1,1); T(getf(g,A_+0x674)==13.5f);
    /* 82: acts only for P1==8 with S.q0 bit 8 and actor bit 7 */
    fixture(g); wr32(g,A_+0x6c8,0x80); wr64(g,S_,0x100); rrec(g,82,8,0,0,0,0,0); run(g,1,1);
    T(mock.ncalls==5&&callrva(0)==0x39b760&&callrva(1)==0x3bde30&&callrva(2)==0x3d74e0&&callrva(3)==0x3d78d0); T((u32)mock.c[2].a[1]==8);
    rrec(g,82,7,0,0,0,0,0); run(g,1,1); T(mock.ncalls==2);                /* P1 != 8: mask 0 */
    rrec(g,82,8,0,0,0,0,0); wr64(g,S_,0xff); run(g,1,1); T(mock.ncalls==2);
    wr64(g,S_,0x100); wr32(g,A_+0x6c8,0); run(g,1,1); T(mock.ncalls==2);
    /* 83: exact P1 -> bit table, tested against S.q8 */
    {
        static const struct { int p1; u32 mask; } tab[]={{0,1},{1,2},{2,4},{4,0x10},{5,0x20},{6,0x40},{7,0x80},{8,0x100},{9,0x200},{10,0x400},{11,0x800},{12,0x1000},
            {15,0x8000},{16,0x10000},{17,0x20000},{18,0x40000},{19,0x80000},{24,0x1000000},{25,0x2000000}};
        for(int p1=0;p1<=40;p1++){
            u32 want=0; for(unsigned i=0;i<sizeof tab/sizeof *tab;i++) if(tab[i].p1==p1) want=tab[i].mask;
            fixture(g); rrec(g,83,(u16)p1,0,0,0,0,0);
            wr64(g,S_+8,want?want:0xffffffffffffffffull>>1); run(g,1,1);
            if(want) T(mock.ncalls==4&&callrva(0)==0x39b760&&callrva(1)==0x3b3e80&&callrva(2)==0x3cb9f0&&mock.c[2].a[0]==0&&mock.c[2].a[1]==A_&&(u8)mock.c[2].a[2]==1);
            else T(mock.ncalls==3&&callrva(2)==0x40d810);                  /* mask 0: 3cb9f0 is never reached */
            if(want){ wr64(g,S_+8,~(u64)want); run(g,1,1); T(mock.ncalls==3); }
        }
    }
}

static void frame_hand(KCtx *g){
    const u32 F0=0x40dd70;
    const int outside[]={-128,-1,0,9,35,36,49,81,100,127};
    for(unsigned i=0;i<sizeof outside/sizeof *outside;i++){ fixture(g); frec(g,outside[i],7,9); run(g,0,1); T(only_orig(0)); }
    fixture(g); frec(g,50,-3,0); run(g,0,1); T(mock.ncalls==2&&callrva(0)==0x3b4da0&&mock.c[0].a[0]==A_&&(u32)mock.c[0].a[1]==0xfffffffdu&&callrva(1)==(int)F0);
    fixture(g); wr64(g,A_+0x20,~0ull); wr64(g,A_+0x28,~0ull); frec(g,51,0,0); run(g,0,1); T(rd64(g,A_+0x20)==0&&rd64(g,A_+0x28)==0&&only_orig(0));
    fixture(g); frec(g,52,0,0); run(g,0,1); T(getf(g,A_+0x134)==99.0f);
    frec(g,53,0,0); run(g,0,1); setf(g,OBJ(MPC_T148)+0xc,1.25f); run(g,0,1); T(getf(g,A_+0x134)==1.25f);
    wr64(g,A_+0x148,0); run(g,0,1); T(rd32(g,A_+0x134)==0x3e860a92u);
    fixture(g); setf(g,A_+0x138,1.0f); frec(g,54,0,0); run(g,0,1); T(getf(g,A_+0x138)==9.0f);
    frec(g,55,0,0); run(g,0,1); T(getf(g,A_+0x138)==16.0f);
    frec(g,56,1,2); run(g,0,1); T(only_orig(0)); T(getf(g,A_+0x138)==16.0f);
    fixture(g); frec(g,57,5,150); run(g,0,1); T(mock.ncalls==2&&callrva(0)==0x3c6dc0&&mock.c[0].a[0]==C_&&mock.c[0].a[1]==5&&mock.c[0].f2==f2u(1.5f)&&mock.c[0].f3==0);
    frec(g,58,-1,-250); run(g,0,1); T(callrva(0)==0x3c8310&&(u32)mock.c[0].a[1]==0xffffffffu&&mock.c[0].f2==f2u(-2.5f)&&mock.c[0].f3==0&&mock.c[0].a[4]==0);
    /* 59: X selects one of seven float slots; anything else (incl. negative) writes nothing */
    {
        static const u16 off[7]={0x128,0x12c,0x130,0x134,0x138,0x13c,0x140};
        for(int x=-2;x<=9;x++){
            fixture(g); for(int i=0;i<7;i++) setf(g,A_+off[i],-1.0f);
            frec(g,59,(s16)x,50); run(g,0,1);
            for(int i=0;i<7;i++) T(getf(g,A_+off[i])==((x==i)?0.5f:-1.0f));
        }
    }
    fixture(g); frec(g,60,0,-50); run(g,0,1); T(getf(g,OBJ(MPC_T148)+0x2c)==-0.5f);
    /* 61: effect start only for X in {0,1}, on [C+0xd8+8X]+0x2380 */
    fixture(g); frec(g,61,0,-7); run(g,0,1); T(mock.ncalls==2&&callrva(0)==0x3d9fe0&&mock.c[0].a[0]==OBJ(MPC_OBJD8)+0x2380&&mock.c[0].a[1]==A_&&(u32)mock.c[0].a[2]==0xfffffff9u&&mock.c[0].a[3]==0);
    frec(g,61,1,7); run(g,0,1); T(mock.c[0].a[0]==OBJ(MPC_OBJE0)+0x2380&&mock.c[0].a[2]==7);
    frec(g,61,2,7); run(g,0,1); T(only_orig(0));
    /* 62: record lookup always happens; X selects the s16 at +4+2X */
    fixture(g); wr16(g,OBJ(MPC_Q918)+0x4e,0x8001); wr16(g,fake_rec3eaae0+4,(u16)-9); wr16(g,fake_rec3eaae0+6,321); frec(g,62,0,0); run(g,0,1);
    T(mock.ncalls==3&&callrva(0)==0x3eaae0&&(u32)mock.c[0].a[0]==0xffff8001u&&callrva(1)==0x3d9fe0&&mock.c[1].a[0]==OBJ(MPC_OBJD8)+0x2380&&(u32)mock.c[1].a[2]==0xfffffff7u);
    frec(g,62,1,0); run(g,0,1); T((u32)mock.c[1].a[2]==321&&mock.c[1].a[0]==OBJ(MPC_OBJE0)+0x2380);
    frec(g,62,2,0); run(g,0,1); T(mock.ncalls==2&&callrva(0)==0x3eaae0);
    /* 63 / 64: sheet+8 dword (skipped on a null sheet) */
    fixture(g); wr32(g,SH_+8,0x55); frec(g,63,-2,0); run(g,0,1); T(rd32(g,SH_+8)==0xfffffffeu);
    frec(g,64,9,9); run(g,0,1); T(rd32(g,SH_+8)==0);
    wr64(g,A_+0x5c0,0); wr32(g,SH_+8,0x66); frec(g,63,7,0); run(g,0,1); T(only_orig(0)&&rd32(g,SH_+8)==0x66);
    /* 65: "%04d" truncated to four characters (snprintf size 5): the string is what the callee sees */
    {
        static const struct { s16 x; const char *s; } t[]={{7,"0007"},{-5,"-005"},{1234,"1234"},{32767,"3276"},{-32768,"-327"},{0,"0000"},{99,"0099"}};
        for(unsigned i=0;i<sizeof t/sizeof *t;i++){
            fixture(g); frec(g,65,t[i].x,0); run(g,0,1);
            char want[8]={0}; memcpy(want,t[i].s,4);
            T(mock.ncalls==2&&callrva(0)==0x3db2f0&&mock.c[0].a[0]==A_&&mock.c[0].a[2]==A_);
            u64 w=0; memcpy(&w,want,5); T(mock.c[0].mem[0]==w);
        }
    }
    fixture(g); frec(g,66,-4,0); run(g,0,1); T(callrva(0)==0x1a85e0&&mock.c[0].a[0]==OBJ(MPC_Q7B0)&&(u32)mock.c[0].a[1]==0xfffffffcu);
    frec(g,67,4,0); run(g,0,1); T(callrva(0)==0x1a85e0&&(u32)mock.c[0].a[1]==4);
    frec(g,68,4,0); run(g,0,1); T(only_orig(0)); frec(g,69,4,0); run(g,0,1); T(only_orig(0));
    /* 70: 409b00 receives the hook-entry xmm2 (the DLL never sets it) and the double 30.0 in xmm0 */
    fixture(g); frec(g,70,0,0); { v128 x=VZ; u32 b=0x41234567; memcpy(&x,&b,4); KH_XMM(g,2)=x; }
    memset(&mock,0,sizeof mock); mock.fake=hand_fake; kh_mp_mock_sink=&mock; kh_call(g,GS_BASE+0x40dd70,R_,C_,0,0); kh_mp_mock_sink=NULL;
    T(mock.ncalls==2&&callrva(0)==0x409b00&&mock.c[0].a[1]==A_&&mock.c[0].f2==0x41234567u&&mock.c[0].x0==0x403e000000000000ull);
    /* 71 / 73: only when sheet+0x1c0 is NOT > 0 (NaN counts as not >0); the callee gets -X or -s8[rec+0x1c] */
    fixture(g); setf(g,SH_+0x1c0,0.0f); frec(g,71,300,0); run(g,0,1); T(mock.ncalls==2&&callrva(0)==0x3c0cd0&&mock.c[0].a[0]==SH_&&(u32)mock.c[0].a[1]==(u32)-300&&mock.c[0].a[2]==0);
    setf(g,SH_+0x1c0,1.0f); run(g,0,1); T(only_orig(0));
    setf(g,SH_+0x1c0,NAN); run(g,0,1); T(mock.ncalls==2);
    fixture(g); wr8(g,fake_rec3b68b0+0x1c,0xf6); frec(g,73,2,0); run(g,0,1); T(mock.ncalls==3&&callrva(0)==0x3b68b0&&mock.c[0].a[0]==2&&callrva(1)==0x3c0cd0&&(u32)mock.c[1].a[1]==10);
    setf(g,SH_+0x1c0,3.0f); run(g,0,1); T(only_orig(0));
    for(int op=72;op<=79;op++) if(op!=73){ fixture(g); frec(g,op,1,1); run(g,0,1); T(only_orig(0)); }
    /* 80 */
    fixture(g); wr32(g,A_+0x6c8,0x80); frec(g,80,12,0); run(g,0,1); T(mock.ncalls==4&&callrva(0)==0x3bde30&&callrva(1)==0x3d74e0&&(u32)mock.c[1].a[1]==12&&callrva(2)==0x3d78d0);
    wr32(g,A_+0x6c8,0x7f); run(g,0,1); T(only_orig(0));
}

/* ---- kinds 64/65: real lifted original + the map lifetime ---------------------------------------------------------------- */
static void map_tests(KCtx *g){
    kh_mod_params_map_clear();
    fixture(g); kh_mp_mock_sink=NULL;
    wr32(g,SH_+8,1234); wr8(g,SH_+0x1ae,77); setf(g,C_+0x44,5.0f);
    s32 saved;
    /* end byte 10 -> limit 9.0 > 5.0: override active, original value saved on first activation */
    rrec(g,64,(u16)-3,0,0,0,0,10); run(g,1,0);
    T(rd32(g,SH_+8)==(u32)-3); T(kh_mod_params_map_size(0)==1); T(kh_mod_params_map_get(0,A_,&saved)&&saved==1234); T(kh_mod_params_map_size(1)==0);
    /* second activation with a different value overwrites without re-saving */
    rrec(g,64,(u16)500,0,0,0,0,10); run(g,1,0); T(rd32(g,SH_+8)==500); T(kh_mod_params_map_get(0,A_,&saved)&&saved==1234);
    /* 65 has its own map and its own saved byte, written as the low byte of P1 */
    rrec(g,65,0x1ff,0,0,0,0,10); run(g,1,0); T(rd8(g,SH_+0x1ae)==0xff); T(kh_mod_params_map_get(1,A_,&saved)&&saved==77); T(kh_mod_params_map_size(0)==1);
    /* equality does NOT activate: limit 9.0 == now 9.0 restores and erases */
    setf(g,C_+0x44,9.0f); rrec(g,64,(u16)77,0,0,0,0,10); run(g,1,0); T(rd32(g,SH_+8)==1234); T(kh_mod_params_map_size(0)==0);
    T(rd8(g,SH_+0x1ae)==0xff);                                              /* map 1 untouched by kind 64 */
    rrec(g,65,0,0,0,0,0,10); run(g,1,0); T(rd8(g,SH_+0x1ae)==77); T(kh_mod_params_map_size(1)==0);
    /* inactive with nothing saved: no write at all */
    wr32(g,SH_+8,4321); run(g,1,0); rrec(g,64,5,0,0,0,0,10); run(g,1,0); T(rd32(g,SH_+8)==4321); T(kh_mod_params_map_size(0)==0);
    /* NaN time: comparison unordered -> restore side */
    setf(g,C_+0x44,5.0f); run(g,1,0); T(rd32(g,SH_+8)==5); setf(g,C_+0x44,NAN); rrec(g,64,9,0,0,0,0,10); run(g,1,0); T(rd32(g,SH_+8)==4321); T(kh_mod_params_map_size(0)==0);
    /* end byte 0: limit -1.0, never active for non-negative time; byte 1: limit 0.0 vs time -0.5 is active */
    setf(g,C_+0x44,0.0f); rrec(g,64,8,0,0,0,0,0); run(g,1,0); T(kh_mod_params_map_size(0)==0&&rd32(g,SH_+8)==4321);
    setf(g,C_+0x44,-0.5f); rrec(g,64,8,0,0,0,0,1); run(g,1,0); T(rd32(g,SH_+8)==8&&kh_mod_params_map_size(0)==1);
    setf(g,C_+0x44,1.0f); run(g,1,0); T(rd32(g,SH_+8)==4321&&kh_mod_params_map_size(0)==0);
    /* a second actor keeps an independent entry (key is the raw actor pointer, not the sheet) */
    u64 A2=ga_calloc(1,0xe00), sh2=ga_calloc(1,0x200), C2=ga_calloc(1,0x200);
    wr64(g,C2+0x10,A2); wr64(g,A2+0x5c0,sh2); wr32(g,sh2+8,999); setf(g,C2+0x44,0.0f);
    setf(g,C_+0x44,-1.0f); rrec(g,64,11,0,0,0,0,5); run(g,1,0);
    kh_mp_mock_sink=NULL; memset(&mock,0,sizeof mock);
    kh_call(g,GS_BASE+0x40d810,R_,C2,0,0); T(rd32(g,sh2+8)==11); T(kh_mod_params_map_size(0)==2);
    T(kh_mod_params_map_get(0,A_,&saved)&&saved==4321); T(kh_mod_params_map_get(0,A2,&saved)&&saved==999);
    setf(g,C2+0x44,100.0f); kh_call(g,GS_BASE+0x40d810,R_,C2,0,0); T(rd32(g,sh2+8)==999); T(kh_mod_params_map_size(0)==1&&kh_mod_params_map_get(0,A_,&saved));
    setf(g,C_+0x44,100.0f); run(g,1,0); T(rd32(g,SH_+8)==4321&&kh_mod_params_map_size(0)==0);
    ga_free(A2); ga_free(sh2); ga_free(C2);
    kh_mod_params_map_clear();
}

/* ---- forwarding to the real lifted originals --------------------------------------------------------------------------- */
static void real_tests(KCtx *g){
    fixture(g); kh_mp_mock_sink=NULL;
    /* vanilla range kinds still run: kind 0 sets C+0x34 bit 2, kind 16 sets C+0x154 bit 0, kind 3 sets C+0x34 bit 5 */
    rrec(g,0,0,0,0,0,0,0); run(g,1,0); T(rd32(g,C_+0x34)==4);
    rrec(g,16,0,0,0,0,0,0); run(g,1,0); T(rd8(g,C_+0x154)==1);
    rrec(g,3,0,0,0,0,0,0); run(g,1,0); T(rd32(g,C_+0x34)==0x24);
    /* extension kind + vanilla bits coexist in the same byte; the original does nothing for 54..83 */
    rrec(g,56,0,0,0,0,0,0); run(g,1,0); T(rd8(g,C_+0x154)==0x11); rrec(g,59,0,0,0,0,0,0); run(g,1,0); T(rd8(g,C_+0x154)==0x91);
    rrec(g,54,0,0,0,0,0,0); wr64(g,A_+0x120,~0ull); run(g,1,0); T(rd64(g,A_+0x120)==0xfffffffdffffffffull);
    rrec(g,55,0,0,0,0,0,0); run(g,1,0); T(rd64(g,A_+0x120)==0xffffffffffffffffull);
    rrec(g,80,1,100,0,0,0,0); setf(g,A_+0x674,0.5f); run(g,1,0); T(getf(g,A_+0x674)==1.5f);
    for(int k=63;k<=83;k++){ if(k==64||k==65||k==70||(k>=66&&k<=68)||k==60||k==61||k==62||k==82||k==83||k==80||k==54||k==55||(k>=56&&k<=59)) continue;
        fixture(g); rrec(g,k,0,0,0,0,0,0); u8 before[MPC_ARENA]; memcpy(before,hp(g,AR),MPC_ARENA); run(g,1,0); T(!memcmp(before,hp(g,AR),MPC_ARENA)); }
    /* vanilla frame ops 9..12 or bits into C+0x155; extension ops in the same hook still write their own fields */
    fixture(g); frec(g,9,0,0); run(g,0,0); T(rd8(g,C_+0x155)==1); frec(g,12,0,0); run(g,0,0); T(rd8(g,C_+0x155)==9);
    frec(g,51,0,0); wr64(g,A_+0x20,5); run(g,0,0); T(rd64(g,A_+0x20)==0);
    frec(g,55,0,0); run(g,0,0); T(getf(g,A_+0x138)==16.0f);
    frec(g,59,6,125); run(g,0,0); T(getf(g,A_+0x140)==1.25f);
    frec(g,63,3,0); wr32(g,SH_+8,0); run(g,0,0); T(rd32(g,SH_+8)==3);
    frec(g,64,3,0); run(g,0,0); T(rd32(g,SH_+8)==0);
    frec(g,60,0,300); run(g,0,0); T(getf(g,OBJ(MPC_T148)+0x2c)==3.0f);
    frec(g,81,0,0); u8 before[MPC_ARENA]; memcpy(before,hp(g,AR),MPC_ARENA); run(g,0,0); T(!memcmp(before,hp(g,AR),MPC_ARENA));
    for(int op=56;op<=80;op++){ if(op==56||op==68||op==69||op==72||(op>=74&&op<=79)){ frec(g,op,3,3); memcpy(before,hp(g,AR),MPC_ARENA); run(g,0,0); T(!memcmp(before,hp(g,AR),MPC_ARENA)); } }
}

/* ---- named fatals --------------------------------------------------------------------------------------------------------- */
static void fatal_case(KCtx *g,int range,int kind,u64 callsite,void (*prep)(KCtx*),const char *needle){
    FILE *cap=kh_tmpfile(); if(!cap) kh_fatal(g,"mod params selftest: tmpfile failed");
    u64 sp=KH_RSP(g);
    pid_t child=kh_fork(); if(child<0) kh_fatal(g,"mod params selftest: fork failed");
    if(!child){
        if(dup2(fileno(cap),STDERR_FILENO)<0) _exit(98);
        fixture(g); if(range) rrec(g,kind,1,1,1,1,1,9); else frec(g,kind,1,1);
        if(prep) prep(g);
        kh_mp_mock_sink=NULL;
        KH_RSP(g)=sp-8; wr64(g,sp-8,GS_BASE+callsite); KH_RCX(g)=R_; KH_RDX(g)=C_;
        kh_dispatch(g,GS_BASE+(range?0x40d810:0x40dd70));
        _exit(99);
    }
    int status=0; T(waitpid(child,&status,0)==child);
    rewind(cap); char out[16384]; size_t n=fread(out,1,sizeof out-1,cap); out[n]=0; fclose(cap);
    T(WIFEXITED(status)&&WEXITSTATUS(status)==3); T(strstr(out,"mod params:")!=NULL); T(strstr(out,needle)!=NULL);
    char want[64]; snprintf(want,sizeof want,"call-site RVA 0x%llx",(unsigned long long)callsite); T(strstr(out,want)!=NULL);
    char kn[64]; snprintf(kn,sizeof kn,"kind %d:",kind); T(strstr(out,kn)!=NULL);
    T(KH_RSP(g)==sp);
}
static void prep_null_actor(KCtx *g){ wr64(g,C_+0x10,0); }
static void prep_null_sheet(KCtx *g){ wr64(g,A_+0x5c0,0); }
static void prep_null_t148(KCtx *g){ wr64(g,A_+0x148,0); }
static void prep_null_sheet_active(KCtx *g){ wr64(g,A_+0x5c0,0); setf(g,C_+0x44,-5.0f); }
static void fatal_tests(KCtx *g){
    if(!kh_fork_ok()){ kh_selftest_not_run("mod params","seven named fatals of the range and frame hooks (7 checks each)",49); return; }   /* iOS: no fork (host_env.c) */
    fatal_case(g,1,54,0x40e7b3,prep_null_actor,"range hook 40d810 kind 54: actor pointer");
    fatal_case(g,1,66,0x40e7b3,prep_null_sheet,"range hook 40d810 kind 66: actor sheet");
    fatal_case(g,1,62,0x40e7b3,prep_null_sheet,"kind 62: actor sheet");
    fatal_case(g,1,64,0x40e7b3,prep_null_sheet_active,"kind 64: actor sheet");
    fatal_case(g,0,60,0x40e9d7,prep_null_t148,"frame hook 40dd70 kind 60: A[+0x148] is null");
    fatal_case(g,0,50,0x40e9d7,prep_null_actor,"frame hook 40dd70 kind 50: actor pointer");
    fatal_case(g,0,71,0x40e9d7,prep_null_sheet,"kind 71: actor sheet");
}

int kh_mod_params_selftest(KCtx *g){
    AR=ga_calloc(1,MPC_ARENA); if(!AR) kh_fatal(g,"mod params selftest: arena allocation failed");
    u64 saved_s=rd64(g,GS_BASE+0x2a171c8); (void)saved_s;
    kh_mod_params_map_clear();
    int ran;
    FILE *dump=getenv("KH_MP_TRANSCRIPT")?fopen(getenv("KH_MP_TRANSCRIPT"),"w"):NULL;
    uint64_t hr=transcript(g,AR,1,EXPECT_RANGE_CASES,dump,&ran); T(ran==EXPECT_RANGE_CASES-240);
    int ran2; uint64_t hf=transcript(g,AR,0,EXPECT_FRAME_CASES,dump,&ran2); T(ran2==EXPECT_FRAME_CASES);
    if(dump) fclose(dump);
    fprintf(stderr,"kh: mod params transcript range cases=%d hash=%016llx frame cases=%d hash=%016llx\n",ran,(unsigned long long)hr,ran2,(unsigned long long)hf);
    T(hr==EXPECT_RANGE_HASH); T(hf==EXPECT_FRAME_HASH);
    range_hand(g); frame_hand(g); map_tests(g); real_tests(g); fatal_tests(g);
    ga_free(AR);
    fprintf(stderr,"kh: mod params selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
