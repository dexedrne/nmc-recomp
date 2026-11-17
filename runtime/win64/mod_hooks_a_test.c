/* Task 9 batch A: table-driven synthetic guest-state tests through the real alias dispatch (kh_call -> f_<rva>).
 * Every guest helper the DLL calls is redirected (kh_mod_a_set_target) to a recorder that logs registers, stack argument, xmm state
 * and the 32 bytes at the first argument, then returns a deterministic value; the forwards to the originals are redirected the same
 * way.  Expected call sequences are written from the DLL disassembly (independently of runtime/mod_hooks_a.c), not from the hooks. */
#include "mod_config.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>

static int checks,failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod A FAIL line %d: %s\n",__LINE__,#c); } } while(0)

/* Raw dispatch like a lifted caller: a hook replacing a guest function must pop its own return address (KH_RET / the original's ret);
 * kh_call alone cannot see a missing pop because it restores RSP afterwards. */
static u64 kc(KCtx *g,u64 target,u64 a,u64 b,u64 c,u64 d){
    u64 save=KH_RSP(g),sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(save-0x100)&~0xFull; wr64(g,sp+0x20,0); sp-=8; wr64(g,sp,0x7ff7ffff0000ull);
    KH_RSP(g)=sp; KH_RCX(g)=a; KH_RDX(g)=b; KH_R8(g)=c; KH_R9(g)=d;
    kh_dispatch(g,target);
    u64 r=KH_RAX(g); int popped=KH_RSP(g)==sp+8;
    KH_RSP(g)=save; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3];
    T(popped);
    return r;
}

typedef struct { int ext; u64 a[4]; u64 s5; u64 rsp; v128 x[4]; u8 mem[32]; int memok; } Rec;
#define MAXLOG 1024
static Rec lg[MAXLOG]; static int nlg;
static u64 rec_addr[KHA_N];
static u64 (*ret_cb)(KCtx *g,const Rec *r);
typedef struct { u64 a[4]; u64 rsp; v128 x[4]; } Fwd;
static Fwd fw; static int nfw;
#define FWD_RET 0x00000000F00D0001ull

static u64 default_ret(const Rec *r){
    switch(r->ext){
    case KHA_ENCODE: return 0xDEAD000000000000ull | (u32)(r->a[0]*0x9E3779B1ull);
    case KHA_REF_ACTOR: return 0x5000000ull+(r->a[0]&0xffff)*0x100+(r->a[1]&0xff);
    case KHA_SCRIPT_NEW: return 0x7777000ull;
    default: return 0x1111ull+(u64)r->ext;
    }
}
static void rec_fn(KCtx *g){
    int ext=-1; for(int i=0;i<KHA_N;i++) if(rec_addr[i]==g->aux[0]) ext=i;
    if(ext<0||nlg>=MAXLOG) kh_fatal(g,"mod A test recorder: unknown target or log full");
    Rec *r=&lg[nlg++]; memset(r,0,sizeof *r);
    r->ext=ext; r->a[0]=KH_RCX(g); r->a[1]=KH_RDX(g); r->a[2]=KH_R8(g); r->a[3]=KH_R9(g); r->rsp=KH_RSP(g);
    r->s5=rd64(g,KH_RSP(g)+0x28);
    for(int i=0;i<4;i++) r->x[i]=KH_XMM(g,i);
    if(ext==KHA_SCRIPT_RUN&&GA_OK(r->a[2])){ memcpy(r->mem,hp(g,r->a[2]),32); r->memok=1; }
    KH_RAX(g)=ret_cb?ret_cb(g,r):default_ret(r);
    KH_RET(g);
}
static void fwd_fn(KCtx *g){
    nfw++; fw.a[0]=KH_RCX(g); fw.a[1]=KH_RDX(g); fw.a[2]=KH_R8(g); fw.a[3]=KH_R9(g); fw.rsp=KH_RSP(g);
    for(int i=0;i<4;i++) fw.x[i]=KH_XMM(g,i);
    KH_RAX(g)=FWD_RET; KH_RET(g);
}
static void setup(void){
    static int done; char nm[48];
    for(int i=0;i<KHA_N;i++){ snprintf(nm,sizeof nm,"mod-a-test-ext-%d",i); rec_addr[i]=kh_host_fn(nm,rec_fn); kh_mod_a_set_target(i,rec_addr[i]); }
    for(int i=0;i<KHF_N;i++) kh_mod_a_set_fwd(i,fwd_fn);
    done=1; (void)done;
}
static void teardown(void){
    for(int i=0;i<KHA_N;i++) kh_mod_a_set_target(i,0);
    for(int i=0;i<KHF_N;i++) kh_mod_a_set_fwd(i,NULL);
    ret_cb=NULL;
}
/* RSP the hook sees: kh_call puts its frame 0x100 below the caller's rsp, 16-aligned, then pushes the return address. */
static u64 esp(u64 sp){ return ((sp-0x100)&~0xFull)-8; }
static void reset_log(void){ nlg=0; nfw=0; memset(&fw,0,sizeof fw); }
static int count_ext(int e){ int n=0; for(int i=0;i<nlg;i++) if(lg[i].ext==e) n++; return n; }
static float xf(v128 v){ float f; memcpy(&f,&v,4); return f; }
static double xd(v128 v){ double f; memcpy(&f,&v,8); return f; }
static int is_zero_vec(v128 v){ for(int i=0;i<16;i++) if(v[i]) return 0; return 1; }

/* Expected call: ext plus the first n integer argument registers. */
typedef struct { int ext,n; u64 a[4]; } Ex;
static int match(const Ex *e,int count){
    if(nlg!=count) return 0;
    for(int i=0;i<count;i++){
        if(lg[i].ext!=e[i].ext) return 0;
        for(int k=0;k<e[i].n;k++) if(lg[i].a[k]!=e[i].a[k]) return 0;
    }
    return 1;
}
static void dump_log(const char *why){
    fprintf(stderr,"  [%s] recorded %d calls:",why,nlg);
    for(int i=0;i<nlg;i++) fprintf(stderr," (%d:%llx,%llx)",lg[i].ext,(unsigned long long)lg[i].a[0],(unsigned long long)lg[i].a[1]);
    fputc('\n',stderr);
}
#define CHECK_SEQ(arr) do { int ok_=match(arr,(int)(sizeof arr/sizeof *arr)); T(ok_); if(!ok_) dump_log(#arr); } while(0)

/* Run `body` in a forked child with stderr captured; returns the exit status and fills `out`. */
static int run_fatal(KCtx *g,void (*body)(KCtx *),char *out,size_t cap){
    FILE *log=tmpfile(); if(!log) kh_fatal(g,"mod A selftest: tmpfile failed");
    pid_t child=fork(); if(child<0) kh_fatal(g,"mod A selftest: fork failed");
    if(!child){ if(dup2(fileno(log),STDERR_FILENO)<0) _exit(98); body(g); _exit(99); }
    int status=0; if(waitpid(child,&status,0)!=child) kh_fatal(g,"mod A selftest: waitpid failed");
    rewind(log); size_t n=fread(out,1,cap-1,log); out[n]=0; fclose(log);
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
}

/* ---- predicates ------------------------------------------------------------------------------------------------------------ */
static void test_predicates(KCtx *g){
    /* Oracle: the shipped dualwield_forms=0,2,3,0,5,0 written as a membership set, and the mask built by bit position. */
    const int member[8]={1,0,1,1,0,1,0,0};
    u64 sp=KH_RSP(g);
    const u32 forms[]={0,1,2,3,4,5,6,7,8,0x2d,0xff,0x100,0xffffffffu,0x80000000u};
    const u64 rax_seeds[]={0x123456789abcde77ull,0xffffffffffffffffull,0,0x00000000000000ffull};
    for(unsigned i=0;i<sizeof forms/sizeof *forms;i++) for(unsigned s=0;s<sizeof rax_seeds/sizeof *rax_seeds;s++){
        u32 f=forms[i]; KH_RAX(g)=rax_seeds[s];
        u64 r=kc(g,GS_BASE+0x2fc860,f,0x1111111100000000ull|f,0,0);
        int in=f<8&&member[f];
        T(r==((rax_seeds[s]&~255ull)|(u64)in));      /* AL only; RAX bits 8..63 survive */
        T(KH_RSP(g)==sp);
    }
    /* upper half of RCX is ignored (cmp ecx,...) */
    KH_RAX(g)=0; T((kc(g,GS_BASE+0x2fc860,0xdead00000000ull|5,0,0,0)&255)==1);
    u32 mask=0; for(int b=0;b<=6;b++) if(b<8&&member[b]) mask|=1u<<b;
    T(mask==0x2d);
    KH_RAX(g)=0xfeedfacecafebeefull; T(kc(g,GS_BASE+0x2fc4e0,0,0,0,0)==mask);   /* eax write clears the upper half */
    KH_RAX(g)=0; T(kc(g,GS_BASE+0x2fc4e0,9,9,9,9)==0x2d); T(KH_RSP(g)==sp);
}

/* ---- 405440 ---------------------------------------------------------------------------------------------------------------- */
static u64 gate_value;
static u64 ret_405440(KCtx *g,const Rec *r){
    if(r->ext==KHA_GATE) return gate_value;
    return default_ret(r);
}
static u64 globals_block,vec_block,entry_block;
static u64 ret_3e9(KCtx *g,const Rec *r){
    if(r->ext==KHA_GLOBALS) return globals_block;
    if(r->ext==KHA_FIND) return entry_block;
    return default_ret(r);
}
static void test_mapevent(KCtx *g){
    u64 sp=KH_RSP(g);
    u64 self=ga_calloc(1,0x400),ev=ga_calloc(1,0x40);
    const u64 r8=0xAAAA00000008ull,r9=0xBBBB00000009ull;
    /* Types other than 0x7f/0x3e8/0x3e9 only forward (the type is a SIGNED word). */
    const s16 plain[]={0,1,0x7e,0x80,0x3e7,0x3ea,-1,-0x7f,0x7fff,-32768};
    for(unsigned i=0;i<sizeof plain/sizeof *plain;i++){
        reset_log(); wr16(g,ev+2,(u16)plain[i]); wr16(g,ev+4,0x7fff); wr8(g,self+0x18c,0xff);
        KH_RAX(g)=0; u64 r=kc(g,GS_BASE+0x405440,self,ev,r8,r9);
        T(nlg==0); T(nfw==1); T(r==FWD_RET); T(fw.a[0]==self&&fw.a[1]==ev&&fw.a[2]==r8&&fw.a[3]==r9); T(fw.rsp==esp(sp)); T(KH_RSP(g)==sp);
    }
    /* type 0x7f: the shop argument (signed word) is forwarded only when >= 0x7d00 */
    const s16 args[]={0,1,0x7cff,0x7d00,0x7d01,0x7fff,-1,-0x7d00,-32768};
    for(unsigned i=0;i<sizeof args/sizeof *args;i++){
        reset_log(); wr16(g,ev+2,0x7f); wr16(g,ev+4,(u16)args[i]);
        u64 r=kc(g,GS_BASE+0x405440,self,ev,r8,r9);
        int fire=args[i]>=0x7d00;
        T(nlg==fire); if(fire){ T(lg[0].ext==KHA_SHOP_ARG&&lg[0].a[0]==(u64)(u32)(s32)args[i]); }
        T(nfw==1&&r==FWD_RET&&fw.a[0]==self&&fw.a[1]==ev&&fw.a[2]==r8&&fw.a[3]==r9&&fw.rsp==esp(sp));
    }
    /* type 0x3e9 */
    globals_block=ga_calloc(1,0xe00); vec_block=ga_calloc(1,0x40); entry_block=0x9990;
    wr64(g,globals_block+0xdc8,vec_block); ret_cb=ret_3e9;
    const s16 ws[]={0,1,-1,0x7fff,-32768,0x1234};
    for(int found=0;found<2;found++) for(unsigned i=0;i<sizeof ws/sizeof *ws;i++){
        reset_log(); entry_block=found?0x9990:0; wr16(g,ev+2,0x3e9); wr16(g,ev+4,(u16)ws[i]);
        kc(g,GS_BASE+0x405440,self,ev,r8,r9);
        Ex a[]={{KHA_GLOBALS,1,{self}},{KHA_VEC_PREP,1,{vec_block+4}},{KHA_FIND,2,{self,(u32)(s32)ws[i]}},{KHA_APPLY,3,{self,0x9990,vec_block+4}}};
        T(match(a,found?4:3)); T(nfw==1&&fw.a[0]==self&&fw.a[1]==ev&&fw.a[2]==r8&&fw.a[3]==r9&&fw.rsp==esp(sp));
    }
    /* type 0x3e8: truth table over the three condition bytes, gate result, word and flag byte */
    ret_cb=ret_405440;
    const u8 b18c_v[]={0,4,8,0x0c,0x10,0xfb,0xff};
    const u8 b18d_v[]={0,4,0x08,0x20,0xff};
    const u8 b194_v[]={0,0x20,0x40,0xdf,0xff};
    const u64 gates[]={0,1,0x1234500ull,0xffffffffffffff00ull};
    const s16 words[]={0,1,-1,0x7fff,-32768,99};
    const u8 flags[]={0,1,2};
    for(unsigned a=0;a<sizeof b18c_v;a++) for(unsigned b=0;b<sizeof b18d_v;b++) for(unsigned c=0;c<sizeof b194_v;c++)
    for(unsigned gi=0;gi<sizeof gates/sizeof *gates;gi++) for(unsigned wi=0;wi<sizeof words/sizeof *words;wi++) for(unsigned fi=0;fi<sizeof flags;fi++){
        u8 x=b18c_v[a],y=b18d_v[b],z=b194_v[c]; s16 w=words[wi]; u8 flag=flags[fi]; gate_value=gates[gi];
        reset_log(); wr16(g,ev,0x4321); wr16(g,ev+2,0x3e8); wr16(g,ev+4,(u16)w); wr8(g,ev+0x2f,flag);
        wr8(g,self+0x18c,x); wr8(g,self+0x18d,y); wr8(g,self+0x194,z);
        kc(g,GS_BASE+0x405440,self,ev,r8,r9);
        Ex e[8]; int n=0;
        int cond=((x|y)&4)||(x&8)||(z&0x20);
        if(cond){
            e[n++]=(Ex){KHA_GATE,1,{self}}; e[n++]=(Ex){KHA_ENCODE,1,{self}};
            if(w){
                if(gate_value){ e[n++]=(Ex){KHA_MOTION,2,{0,self}}; e[n++]=(Ex){KHA_OP_A,2,{self+0x158,(u32)((s32)w+1)}}; e[n++]=(Ex){KHA_OP_B,2,{self+0x158,4}}; }
                else { e[n++]=(Ex){KHA_OP_A,2,{self+0x158,(u32)(s32)w}}; e[n++]=(Ex){KHA_OP_B,2,{self+0x158,0}}; }
            }
            if(flag==1) e[n++]=(Ex){KHA_SHOP_ACT,4,{self,self,0x4321,0}};
        }
        int ok=match(e,n); T(ok); if(!ok){ fprintf(stderr,"  3e8 case %x %x %x gate=%llx w=%d flag=%d\n",x,y,z,(unsigned long long)gate_value,w,flag); dump_log("3e8"); }
        T(nfw==1&&fw.a[0]==self&&fw.a[1]==ev&&fw.a[2]==r8&&fw.a[3]==r9&&fw.rsp==esp(sp)); T(KH_RSP(g)==sp);
    }
    /* float arguments of the effect calls: xmm2/xmm3 zero for both ops, xmm0 = 30.0 double for the motion call; OP_B stack arg 0 */
    gate_value=1; wr16(g,ev,7); wr16(g,ev+2,0x3e8); wr16(g,ev+4,5); wr8(g,ev+0x2f,0); wr8(g,self+0x18c,4); wr8(g,self+0x18d,0); wr8(g,self+0x194,0);
    for(int i=0;i<4;i++) KH_XMM(g,i)=(v128){0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11};
    reset_log(); kc(g,GS_BASE+0x405440,self,ev,r8,r9);
    T(nlg==5);
    if(nlg==5){
        T(lg[2].ext==KHA_MOTION&&xd(lg[2].x[0])==30.0&&lg[2].a[1]==self);
        T(lg[3].ext==KHA_OP_A&&xf(lg[3].x[2])==0.0f&&xf(lg[3].x[3])==0.0f&&is_zero_vec(lg[3].x[2])&&is_zero_vec(lg[3].x[3]));
        T(lg[4].ext==KHA_OP_B&&is_zero_vec(lg[4].x[2])&&is_zero_vec(lg[4].x[3])&&lg[4].s5==0);
    }
    ret_cb=NULL; ga_free(globals_block); ga_free(vec_block); ga_free(self); ga_free(ev);
}

/* ---- script wrappers ------------------------------------------------------------------------------------------------------- */
static u64 ret_null_script(KCtx *g,const Rec *r){ return r->ext==KHA_SCRIPT_NEW?0:default_ret(r); }
static void fatal_3a89a0(KCtx *g){ ret_cb=ret_null_script; kc(g,GS_BASE+0x3a89a0,0x1000,0,0,0); }
static void test_scripts(KCtx *g){
    u64 sp=KH_RSP(g);
    /* 3a89a0: common script trigger 0x65 */
    u64 oldg=rd64(g,GS_BASE+0x2a0f688); wr64(g,GS_BASE+0x2a0f688,0x4242000);
    const u64 self=0x1000,rdx=0x2222,r8=0x3333,r9=0x4444;
    reset_log(); u64 r=kc(g,GS_BASE+0x3a89a0,self,rdx,r8,r9);
    Ex a[]={{KHA_ACTOR_PRE,4,{self,rdx,r8,r9}},{KHA_SCRIPT_NEW,2,{0x4242000,0}},{KHA_SCRIPT_RUN,4,{0x7777000,0x65,0,0}},{KHA_SCRIPT_RESET,1,{0x7777000}},{KHA_SCRIPT_FREE,1,{0x7777000}}};
    CHECK_SEQ(a);
    T(nlg==5&&lg[2].s5==0);
    T(nfw==1&&r==FWD_RET&&fw.a[0]==self&&fw.a[1]==rdx&&fw.a[2]==r8&&fw.a[3]==r9&&fw.rsp==esp(sp)); T(KH_RSP(g)==sp);
    { char out[4096]; int st=run_fatal(g,fatal_3a89a0,out,sizeof out);
      T(st==3); T(strstr(out,"mod 3a89a0: common script allocation (3e1730) returned null")!=NULL); T(KH_RSP(g)==sp); ret_cb=NULL; }
    wr64(g,GS_BASE+0x2a0f688,oldg);

    /* 3d61d0: trigger 0x13 with four 8-byte slots */
    u64 self2=ga_calloc(1,0x800),ref1=ga_calloc(1,0x40),ref2=ga_calloc(1,0x40),p1=ga_calloc(1,16),p2=ga_calloc(1,16);
    wr64(g,p1,ref1); wr64(g,p2,ref2); wr32(g,ref1+0x18,0x11); wr32(g,ref1+0x1c,0x22); wr32(g,ref2+0x18,0x33); wr32(g,ref2+0x1c,0x44);
    for(int has=0;has<2;has++){
        wr64(g,self2+0x5b0,has?0x5150000:0);
        reset_log(); r=kc(g,GS_BASE+0x3d61d0,self2,p1,p2,0x9999);
        u64 act1=0x5000000+0x11*0x100+0x22,act2=0x5000000+0x33*0x100+0x44;
        Ex b[]={{KHA_ENCODE,1,{ref1}},{KHA_REF_ACTOR,2,{0x11,0x22}},{KHA_ENCODE,1,{act1}},{KHA_ENCODE,1,{ref2}},{KHA_REF_ACTOR,2,{0x33,0x44}},{KHA_ENCODE,1,{act2}},{KHA_SCRIPT_RUN,2,{0x5150000,0x13}}};
        if(has){
            CHECK_SEQ(b);
            u32 e[4]; u64 in[4]={ref1,act1,ref2,act2};
            for(int i=0;i<4;i++) e[i]=(u32)(default_ret(&(Rec){.ext=KHA_ENCODE,.a={in[i]}}));
            u8 want[32]; memset(want,0,32); for(int i=0;i<4;i++) memcpy(want+8*i,&e[i],4);
            T(lg[6].memok&&!memcmp(lg[6].mem,want,32));        /* four zero-initialised 8-byte slots, dword stores only */
            T(lg[6].a[2]<esp(sp)&&lg[6].a[2]>=esp(sp)-0x100);            /* inside the local frame below the hook's stack */
            T(lg[6].s5==0&&lg[6].a[3]==4);
        } else { T(match(b,6)); }
        T(nfw==1&&r==FWD_RET&&fw.a[0]==self2&&fw.a[1]==p1&&fw.a[2]==p2&&fw.a[3]==0x9999&&fw.rsp==esp(sp)); T(KH_RSP(g)==sp);
    }
    /* 3d60c0: trigger 6 with one slot, only when actor+5b0 != 0 */
    for(int has=0;has<2;has++){
        wr64(g,self2+0x5b0,has?0x5150000:0);
        reset_log(); r=kc(g,GS_BASE+0x3d60c0,self2,0x6a6a0,0x7b,0x8c);
        Ex c[]={{KHA_ENCODE,1,{0x6a6a0}},{KHA_SCRIPT_RUN,2,{0x5150000,6}}};
        if(has){
            CHECK_SEQ(c);
            u64 want=(u32)default_ret(&(Rec){.ext=KHA_ENCODE,.a={0x6a6a0}});
            T(lg[1].memok&&!memcmp(lg[1].mem,&want,8));        /* qword zeroed first: high dword stays 0 although ENCODE returned garbage above bit 31 */
            T(lg[1].s5==0&&lg[1].a[2]<esp(sp)&&lg[1].a[2]>=esp(sp)-0x100&&lg[1].a[3]==1);
        } else T(nlg==0);
        T(nfw==1&&r==FWD_RET&&fw.a[0]==self2&&fw.a[1]==0x6a6a0&&fw.a[2]==0x7b&&fw.a[3]==0x8c&&fw.rsp==esp(sp)); T(KH_RSP(g)==sp);
    }
    ga_free(self2); ga_free(ref1); ga_free(ref2); ga_free(p1); ga_free(p2);
}

/* ---- 3a7cb0 ---------------------------------------------------------------------------------------------------------------- */
static u64 save_block;
static u64 ret_save(KCtx *g,const Rec *r){ return r->ext==KHA_SAVE?save_block:default_ret(r); }
static void test_summon(KCtx *g){
    u64 sp=KH_RSP(g); save_block=ga_calloc(1,0x4000); ret_cb=ret_save;
    const u8 flags[]={0,1,2,0x80,0xff};
    for(unsigned i=0;i<sizeof flags;i++){
        wr8(g,save_block+0x3525,flags[i]); reset_log();
        for(int k=0;k<4;k++){ v128 v; memset(&v,0x30+k,16); float f=(float)(k+1)*1.5f; memcpy(&v,&f,4); KH_XMM(g,k)=v; }
        v128 saved[4]; for(int k=0;k<4;k++) saved[k]=KH_XMM(g,k);
        KH_RAX(g)=0; u64 r=kc(g,GS_BASE+0x3a7cb0,0xC0DE0000ull,0xFFFFFFFF00000005ull,0xABCD0008ull,0x77);
        T(count_ext(KHA_SAVE)==1);
        if(flags[i]){
            T(nlg==2&&lg[1].ext==KHA_SUMMON&&lg[1].a[0]==0xABCD0008ull);
            T(nlg==2&&!memcmp(&lg[1].x[1],&saved[3],16));      /* xmm1 = xmm3 (movaps) for the summon call */
        } else T(nlg==1);
        T(nfw==1&&r==FWD_RET&&fw.a[0]==0xC0DE0000ull&&fw.a[1]==5&&fw.a[2]==0xABCD0008ull&&fw.a[3]==0x77&&fw.rsp==esp(sp));
        T(!memcmp(&fw.x[3],&saved[3],16));                     /* xmm3 (the float argument) restored for the original */
        for(int k=0;k<3;k++) T(!memcmp(&fw.x[k],&saved[k],16));
        T(KH_RSP(g)==sp);
    }
    ret_cb=NULL; ga_free(save_block);
}

/* ---- 4014a0 ---------------------------------------------------------------------------------------------------------------- */
typedef struct { s32 id; u16 w; u8 b6; } AbEnt;
static AbEnt ab_table[16]; static int ab_n; static u64 ab_mem[16];
static u64 lst1,lst2;
static u32 id_first=0x501,id_second=0x502;
static u64 ret_4014a0(KCtx *g,const Rec *r){
    if(r->ext==KHA_DECODE){ if((u32)r->a[0]==id_first) return lst1; if((u32)r->a[0]==id_second) return lst2; return 0; }
    if(r->ext==KHA_ABILITY_INFO){ for(int i=0;i<ab_n;i++) if(ab_table[i].id==(s32)(u32)r->a[0]) return ab_mem[i]; return 0; }
    return default_ret(r);
}
static const int group_bit[5][4]={{2,3,4,5},{6,7,8,9},{10,11,12,13},{14,15,16,17},{181,182,183,184}};
static int bt(const u8 *bits,int n){ return (bits[n>>3]>>(n&7))&1; }
static u64 fatal_list;
static void fatal_ab_noinfo(KCtx *g){ ret_cb=ret_4014a0; kc(g,GS_BASE+0x4014a0,fatal_list,0,0,0); }
static void fatal_ab_nosheet(KCtx *g){ kc(g,GS_BASE+0x4014a0,fatal_list,0,0,0); }
static void test_abilities(KCtx *g){
    u64 sp=KH_RSP(g);
    u64 list=ga_calloc(1,0x100),sheet=ga_calloc(1,0x300); lst1=ga_calloc(1,0x100); lst2=ga_calloc(1,0x100);
    wr64(g,list+0x28,sheet); wr32(g,sheet+0x250,id_first); wr32(g,sheet+0x254,id_second);
    ab_n=6;
    ab_table[0]=(AbEnt){0x10,1,1};  /* word==1: add even though byte6 != 0 */
    ab_table[1]=(AbEnt){0x11,0,0};  /* byte6 == 0: add */
    ab_table[2]=(AbEnt){0x12,0,1};  /* byte6 != 0: add only when there is no second list */
    ab_table[3]=(AbEnt){0x13,2,0xff}; /* word != 1 exactly */
    ab_table[4]=(AbEnt){0x14,0x101,3}; /* word low byte 1 but word != 1: not the extension */
    ab_table[5]=(AbEnt){0x3fff,1,0x7f};
    for(int i=0;i<ab_n;i++){ ab_mem[i]=ga_calloc(1,0x18); wr16(g,ab_mem[i]+0xc,ab_table[i].w); wr8(g,ab_mem[i]+6,ab_table[i].b6); }
    ret_cb=ret_4014a0;
    const u16 first_entries[]={0x8010,0x8011,0x8012,0x8013,0x8014,0x0010,0x7fff,0xbfff,0x8000|0x3fff,0x8012};
    for(int has_second=0;has_second<2;has_second++){
        memset(hp(g,lst1),0,0x100); memset(hp(g,lst2),0,0x100); memset(hp(g,list),0,0x28);
        for(unsigned i=0;i<sizeof first_entries/sizeof *first_entries;i++) wr16(g,lst1+0x54+2*i,first_entries[i]);
        /* second list: bit15 entries decoded as 15-bit signed values; zero after decoding is skipped, negative ids pass through */
        const u16 second_entries[]={0x8021,0x0022,0x8000,0xc000,0xffff,0x8001,0xbfff};
        for(unsigned i=0;i<sizeof second_entries/sizeof *second_entries;i++) wr16(g,lst2+8+2*i,second_entries[i]);
        u32 second_id=has_second?id_second:0x777777; /* unmatched id -> DECODE returns 0 */
        wr32(g,sheet+0x254,second_id);
        reset_log(); KH_RAX(g)=0;
        kc(g,GS_BASE+0x4014a0,list,0,0,0);
        Ex e[40]; int n=0;
        e[n++]=(Ex){KHA_DECODE,1,{id_first}}; e[n++]=(Ex){KHA_DECODE,1,{second_id}};
        for(unsigned i=0;i<sizeof first_entries/sizeof *first_entries;i++){
            u16 en=first_entries[i]; if(!(en&0x8000)) continue;
            s32 id=((s32)(s16)(u16)(en<<1))>>1;
            e[n++]=(Ex){KHA_ABILITY_INFO,1,{(u32)id}};
            int add=0;
            for(int k=0;k<ab_n;k++) if(ab_table[k].id==id){ add=ab_table[k].w==1||ab_table[k].b6==0||!has_second; }
            if(add) e[n++]=(Ex){KHA_ABILITY_ADD,2,{list,(u32)id}};
        }
        if(has_second){
            const s32 adds[]={0x21,-0x4000+0,-1,1,0x3fff};
            /* 0x8021 -> 0x21; 0x8000 -> 0 skipped; 0xc000 -> -0x4000; 0xffff -> -1; 0x8001 -> 1; 0xbfff -> 0x3fff */
            for(unsigned k=0;k<sizeof adds/sizeof *adds;k++) e[n++]=(Ex){KHA_ABILITY_ADD,2,{list,(u32)adds[k]}};
        }
        int ok=match(e,n); T(ok); if(!ok) dump_log(has_second?"4014a0 two lists":"4014a0 one list");
        T(KH_RSP(g)==sp);
    }
    /* level bytes: exhaustive over each group's four bits (plus other-group noise); untouched when none present */
    ret_cb=NULL; reset_log();
    wr32(g,sheet+0x250,0); wr32(g,sheet+0x254,0);        /* DECODE(0) = 0 for both: no add calls */
    ret_cb=ret_4014a0; id_first=0x501; id_second=0x502;
    for(int grp=0;grp<5;grp++) for(int mask=0;mask<16;mask++) for(int noise=0;noise<4;noise++){
        u8 bits[0x20]; memset(bits,0,sizeof bits);
        if(noise==1) memset(bits,0xff,0x1c);
        if(noise==2) for(int i=0;i<0x1c;i++) bits[i]=(u8)(i*37+11);
        for(int k=0;k<5;k++) for(int j=0;j<4;j++){ int b=group_bit[k][j],on;
            if(k==grp) on=(mask>>j)&1; else on=noise==0?0:noise==1?1:bt(bits,b);
            if(on) bits[b>>3]|=1<<(b&7); else bits[b>>3]&=~(1<<(b&7)); }
        memcpy(hp(g,list),bits,0x20); wr64(g,list+0x28,sheet);
        for(int k=0;k<5;k++) wr8(g,list+0x20+k,0xEE);
        wr32(g,sheet+0x250,0); wr32(g,sheet+0x254,0);
        reset_log(); kc(g,GS_BASE+0x4014a0,list,0,0,0);
        T(nlg==2);
        for(int k=0;k<5;k++){
            int want=0; for(int j=0;j<4;j++) if(bt(bits,group_bit[k][j])){ want=j+1; break; }   /* first present, lowest bit first */
            T(rd8(g,list+0x20+k)==(want?want:0xEE));
        }
        T(!memcmp(hp(g,list),bits,0x20)&&rd64(g,list+0x28)==sheet);   /* bitset itself untouched with empty lists */
    }
    /* fatal paths: null sheet, and an ability id with no table entry */
    char out[4096]; fatal_list=list;
    wr64(g,list+0x28,0); T(run_fatal(g,fatal_ab_nosheet,out,sizeof out)==3); T(strstr(out,"mod 4014a0: ability list")&&strstr(out,"no actor block"));
    wr64(g,list+0x28,sheet); wr32(g,sheet+0x250,id_first); wr32(g,sheet+0x254,id_second);
    memset(hp(g,lst1),0,0x100); wr16(g,lst1+0x54,0xc123);   /* decodes negative: no table entry (the DLL dereferences null) */
    T(run_fatal(g,fatal_ab_noinfo,out,sizeof out)==3); T(strstr(out,"mod 4014a0: no ability table entry")&&strstr(out,"decoded id -16093"));
    T(KH_RSP(g)==sp);
    ret_cb=NULL;
    for(int i=0;i<ab_n;i++) ga_free(ab_mem[i]);
    ga_free(list); ga_free(sheet); ga_free(lst1); ga_free(lst2);
}

/* ---- 29efa0 ---------------------------------------------------------------------------------------------------------------- */
static u64 name_entry; static u32 want_selector;
static u64 ret_shop(KCtx *g,const Rec *r){
    if(r->ext==KHA_SHOP_SEL) return 0xCAFE00000000ull|(want_selector=(u32)(r->a[0]*3+1));
    if(r->ext==KHA_NAME_ENTRY) return name_entry;
    return default_ret(r);
}
static void fatal_shop(KCtx *g){ name_entry=0; ret_cb=ret_shop; kc(g,GS_BASE+0x29efa0,0x54,ga_calloc(1,0x110),0,0); }
static void test_shopface(KCtx *g){
    u64 sp=KH_RSP(g);
    name_entry=ga_calloc(1,0x60); u64 out=ga_calloc(1,0x110); ret_cb=ret_shop;
    struct { const char *raw; const char *want; } cases[]={
        {"P_LM100","menu/shopface/p_lm100.bin"},{"p_ex100_30","menu/shopface/p_ex100_30.bin"},{"N_LM010","menu/shopface/n_lm010.bin"},
        {"MiXeD_Case9","menu/shopface/mixed_case9.bin"},{"","menu/shopface/.bin"},{"[\\]^_`@AZaz{|}~09","menu/shopface/[\\]^_`@azaz{|}~09.bin"},
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
        memset(hp(g,name_entry),0,0x60); memcpy(hp(g,name_entry)+8,cases[i].raw,strlen(cases[i].raw));
        memset(hp(g,out),0xEE,0x110); reset_log(); KH_RAX(g)=0x42;
        kc(g,GS_BASE+0x29efa0,0x54+i,out,0,0);
        T(!strcmp(gstr(g,out),cases[i].want));
        T(nlg==2&&lg[0].ext==KHA_SHOP_SEL&&lg[0].a[0]==0x54+i&&lg[1].ext==KHA_NAME_ENTRY&&lg[1].a[0]==want_selector);
        T(nfw==0);                                     /* full replacement: the original never runs */
        T(rd8(g,out+strlen(cases[i].want)+1)==0xEE);   /* nothing written past the terminator */
        T(KH_RSP(g)==sp);
    }
    /* A full 32-byte name (no terminator in the field) must not read or write past 32 bytes. */
    memset(hp(g,name_entry),'Q',0x60); memset(hp(g,out),0xEE,0x110);
    kc(g,GS_BASE+0x29efa0,1,out,0,0);
    char want[64]; snprintf(want,sizeof want,"menu/shopface/%s.bin","qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq");
    T(!strcmp(gstr(g,out),want));
    /* Resolution through the loose-tree file layer: mixed-case file names under menu/shopface, found via the 1063d0 size query. */
    char dir[]="/tmp/khnmc-shop-XXXXXX",p[512],lp[512];
    if(!mkdtemp(dir)) kh_fatal(g,"mod A selftest: mkdtemp failed");
    snprintf(p,sizeof p,"%s/menu",dir); mkdir(p,0700); snprintf(p,sizeof p,"%s/menu/ShopFace",dir); mkdir(p,0700);
    snprintf(p,sizeof p,"%s/menu/ShopFace/P_LM100.BIN",dir); FILE *f=fopen(p,"wb"); if(!f) kh_fatal(g,"mod A selftest: cannot create fixture");
    fwrite("shopface-bytes",1,14,f); fclose(f);
    snprintf(lp,sizeof lp,"%s/log",dir);
    const char *oldroot=getenv("KHNMC_DATA"),*oldlog=getenv("KHNMC_FILELOG");
    char *root=oldroot?strdup(oldroot):NULL,*log=oldlog?strdup(oldlog):NULL;
    setenv("KHNMC_DATA",dir,1); setenv("KHNMC_FILELOG",lp,1);
    u64 m0=0,m1=0,dummy;
    memset(hp(g,name_entry),0,0x60); memcpy(hp(g,name_entry)+8,"P_LM100",7);
    kh_mod_a_counts(9,&m0,&dummy);
    kc(g,GS_BASE+0x29efa0,0x54,out,0,0);
    kh_mod_a_counts(9,&m1,&dummy);
    T(m1==m0);                                         /* resolved: not counted as loose-missing */
    u64 size=kh_call(g,GS_BASE+0x1063d0,0,out,0,0);
    T(size==14);
    memset(hp(g,name_entry),0,0x60); memcpy(hp(g,name_entry)+8,"P_ZZ999",7);
    kc(g,GS_BASE+0x29efa0,0x54,out,0,0);
    kh_mod_a_counts(9,&m1,&dummy);
    T(m1==m0+1);                                       /* absent file is counted (the file layer reports the MISS) */
    if(root) setenv("KHNMC_DATA",root,1); else unsetenv("KHNMC_DATA");
    if(log) setenv("KHNMC_FILELOG",log,1); else unsetenv("KHNMC_FILELOG");
    free(root); free(log);
    snprintf(p,sizeof p,"%s/menu/ShopFace/P_LM100.BIN",dir); unlink(p);
    snprintf(p,sizeof p,"%s/menu/ShopFace",dir); rmdir(p); snprintf(p,sizeof p,"%s/menu",dir); rmdir(p); unlink(lp); rmdir(dir);
    char outbuf[4096];
    T(run_fatal(g,fatal_shop,outbuf,sizeof outbuf)==3); T(strstr(outbuf,"mod 29efa0: no shop-face name entry for id 0x54"));
    ret_cb=NULL; ga_free(name_entry); ga_free(out);
}

int kh_mod_hooks_a_selftest(KCtx *g){
    u64 sp=KH_RSP(g);
    u64 c0[9],e0[9]; for(int i=0;i<9;i++) kh_mod_a_counts(i,&c0[i],&e0[i]);
    test_predicates(g);
    setup();
    test_mapevent(g);
    test_scripts(g);
    test_summon(g);
    test_abilities(g);
    test_shopface(g);
    teardown();
    /* every hook was exercised and its counters moved */
    for(int i=0;i<9;i++){ u64 c,e; kh_mod_a_counts(i,&c,&e); T(c>c0[i]); if(i!=1) T(e>e0[i]); }
    T(KH_RSP(g)==sp);
    fprintf(stderr,"kh: mod A selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
