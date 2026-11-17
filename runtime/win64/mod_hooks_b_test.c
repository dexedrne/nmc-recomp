/* Task 23: table-driven tests of the hooks in mod_hooks_b.c through the real alias dispatch (kh_dispatch -> f_<rva>), with synthetic
 * guest state.  Every outgoing guest call is recorded by the call sink (kh_modb_sink) and answered by the fixture; the expected call
 * sequences, register values and memory effects below are written from the DLL listing (addresses in the comments), flat and in
 * listing order, not from the hooks. */
#include "mod_config.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>

static int checks,failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod B FAIL line %d: %s\n",__LINE__,#c); } } while(0)

/* Raw dispatch like a lifted caller: a hook that replaces a guest function must pop its own return address. */
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
static int run_fatal(KCtx *g,void (*body)(KCtx *),char *out,size_t cap){
    FILE *log=tmpfile(); if(!log) kh_fatal(g,"mod B selftest: tmpfile failed");
    pid_t child=fork(); if(child<0) kh_fatal(g,"mod B selftest: fork failed");
    if(!child){ if(dup2(fileno(log),STDERR_FILENO)<0) _exit(98); body(g); _exit(99); }
    int status=0; if(waitpid(child,&status,0)!=child) kh_fatal(g,"mod B selftest: waitpid failed");
    rewind(log); size_t n=fread(out,1,cap-1,log); out[n]=0; fclose(log);
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
}

/* ---- fixture ----------------------------------------------------------------------------------------------------------------- */
#define HI8  0xdeadbeefcafe5a00ull      /* junk above AL in the result of a function the DLL tests with `test al,al` */
#define HI32 0x0badf00d00000000ull      /* junk above EAX in the result of a function the DLL tests with a 32-bit compare */
#define NCMD 8
typedef struct {
    KCtx *g;
    u64 self,obj,sheet,member[3],objent[3],rows,r2660,save,cmdrec,abil;
    int nrows;
    u8 alive[3];
    u8 r_3bfb30,r_3b27e0,r_39a7e0,r_3d7820,r_3b27c0,r_3aa4f0,r_3abb20,r_39a800,r_39a870;
    u32 r_3de250,r_3de130;
    s32 cmd_id[NCMD];                    /* 0x3b68b0(id) -> cmdrec + 0x30*k for cmd_id[k]==id (sign-extended id as passed) */
    u32 c5900[32];                       /* 0x3c5900(i) for i < 14; 0x3c5900(0x100+k) for the shortcut conversion */
    u64 ae0_at_3b20b0, ret_3e6d10;
    int null_row;                        /* 0x3b2660 returns 0 for this row index (3..6), 0 = never */
    /* item 2 hooks */
    struct { u32 rva; u64 val; } ov[24]; int nov;        /* plain return values by rva, looked up first */
    u64 items; u32 item_id[8];           /* 0x3e39a0(id) -> items + 0x20*k for item_id[k]==id, else 0 */
    int probe_hit, probes; u64 probe_val; /* 0x3f95f0: the probe_hit-th call (1-based) returns probe_val, the others 0 */
    u64 poke_at, poke_val;               /* 0x4ad240: also overwrites the qword at poke_at (order test) */
} Fx;
static Fx fx;
static struct kh_modb_mock mock;
static u64 fake(void *user,const struct kh_modb_call *c,int index){
    Fx *x=user; KCtx *g=x->g; (void)index;
    if(c->tail==2) return 0x0f0f0f0f00000000ull|c->rva;                      /* "the original returned this" */
    for(int i=0;i<x->nov;i++) if(x->ov[i].rva==c->rva) return x->ov[i].val;
    switch(c->rva){
    case 0x3e39a0: for(int k=0;k<8;k++) if(x->item_id[k]&&x->item_id[k]==(u32)c->a[0]) return x->items+0x20*(u64)k; return 0;
    case 0x3f95f0: x->probes++; return x->probes==x->probe_hit?x->probe_val:0;
    case 0x4ad240: if(x->poke_at) wr64(g,x->poke_at,x->poke_val); return 0xfeed000000000000ull|(u32)(c->a[0]*0x9E3779B1ull);
    case 0x3d5c50: return c->a[0]<3?x->member[c->a[0]]:0;
    case 0x3ba720: for(int i=0;i<3;i++) if(c->a[0]==x->member[i]) return HI8|x->alive[i]; return HI8;
    case 0x3dfeb0: for(int i=0;i<3;i++) if(c->a[0]==0x9140+(u64)i) return x->objent[i]; return 0;
    case 0x3e60f0: return HI32|(0x700+(c->a[0]&0xff));
    case 0x3b0f20: { u64 r=x->rows+0x10*(u64)x->nrows; x->nrows++; return r; }
    case 0x3bfb30: return HI8|x->r_3bfb30;
    case 0x3b27e0: return HI8|x->r_3b27e0;
    case 0x3b2660: return (int)c->a[1]==x->null_row?0:x->r2660+0x20*(c->a[1]-3);
    case 0x39a7e0: return HI8|x->r_39a7e0;
    case 0x3d7820: return HI8|x->r_3d7820;
    case 0x3b27c0: return HI8|x->r_3b27c0;
    case 0x3aa4f0: return HI8|x->r_3aa4f0;
    case 0x3abb20: return HI8|x->r_3abb20;
    case 0x39a800: return HI8|x->r_39a800;
    case 0x39a870: return HI8|x->r_39a870;
    case 0x3de250: return HI32|x->r_3de250;
    case 0x3de130: return HI32|x->r_3de130;
    case 0x152380: return x->save;
    case 0x3b68b0: for(int k=0;k<NCMD;k++) if((u32)x->cmd_id[k]==(u32)c->a[0]) return x->cmdrec+0x30*(u64)k; return 0;
    case 0x3d74c0: return x->abil;
    case 0x3c5900: { u32 i=(u32)c->a[0]; u32 v=i<14?x->c5900[i]:(i>=0x100&&i<0x100+18?x->c5900[14+(i-0x100)]:0x4444); return HI32|v; }
    case 0x3b20b0: x->ae0_at_3b20b0=rd64(g,x->self+0xae0); return 0x20b0;
    case 0x3e6d10: return x->ret_3e6d10;
    case 0x3b19b0: return 0x19b0aaaa5555ull;
    default: return 0x1000000ull+c->rva;
    }
}
static void fx_init(KCtx *g){
    memset(&fx,0,sizeof fx); fx.g=g;
    fx.self=ga_calloc(1,0xc00); fx.obj=ga_calloc(1,0xe00); fx.sheet=ga_calloc(1,0x200);
    for(int i=0;i<3;i++){ fx.member[i]=ga_calloc(1,0x920); fx.objent[i]=ga_calloc(1,0x50); }
    fx.rows=ga_calloc(1,0x10*KH_MODB_MAXCALLS); fx.r2660=ga_calloc(1,0x20*4); fx.save=ga_calloc(1,0x3700); fx.cmdrec=ga_calloc(1,0x30*NCMD); fx.abil=ga_calloc(1,0x40+2*0x80);
    fx.items=ga_calloc(8,0x20);
    mock.fake=fake; mock.user=&fx;
}
static void fx_free(void){
    ga_free(fx.self); ga_free(fx.obj); ga_free(fx.sheet); for(int i=0;i<3;i++){ ga_free(fx.member[i]); ga_free(fx.objent[i]); }
    ga_free(fx.rows); ga_free(fx.r2660); ga_free(fx.save); ga_free(fx.cmdrec); ga_free(fx.abil); ga_free(fx.items);
}
/* expected calls */
typedef struct { u32 rva; int tail; u64 a[4]; } Ex;
static Ex ex[KH_MODB_MAXCALLS]; static int nex;
static void E(u32 rva,u64 a0,u64 a1,u64 a2,u64 a3){ if(nex<KH_MODB_MAXCALLS){ ex[nex].rva=rva; ex[nex].tail=0; ex[nex].a[0]=a0; ex[nex].a[1]=a1; ex[nex].a[2]=a2; ex[nex].a[3]=a3; } nex++; }
static int seq_ok(const char *what){
    int ok=!mock.overflow&&mock.ncalls==nex&&nex<=KH_MODB_MAXCALLS;
    for(int i=0;ok&&i<nex;i++){ const struct kh_modb_call *c=&mock.c[i]; if(c->rva!=ex[i].rva||c->tail!=ex[i].tail||memcmp(c->a,ex[i].a,sizeof ex[i].a)||c->a[4]) ok=0; }
    if(!ok){
        fprintf(stderr,"  [%s] expected %d calls, recorded %d%s\n",what,nex,mock.ncalls,mock.overflow?" (overflow)":"");
        for(int i=0;i<nex||i<mock.ncalls;i++){
            int bad=i>=nex||i>=mock.ncalls||mock.c[i].rva!=ex[i].rva||mock.c[i].tail!=ex[i].tail||memcmp(mock.c[i].a,ex[i].a,sizeof ex[i].a);
            if(!bad) continue;
            if(i<nex) fprintf(stderr,"    #%d want %x%s(%llx,%llx,%llx,%llx)",i,ex[i].rva,ex[i].tail?" TAIL":"",(unsigned long long)ex[i].a[0],(unsigned long long)ex[i].a[1],(unsigned long long)ex[i].a[2],(unsigned long long)ex[i].a[3]);
            if(i<mock.ncalls) fprintf(stderr,"    got %x%s(%llx,%llx,%llx,%llx)",mock.c[i].rva,mock.c[i].tail?" TAIL":"",(unsigned long long)mock.c[i].a[0],(unsigned long long)mock.c[i].a[1],(unsigned long long)mock.c[i].a[2],(unsigned long long)mock.c[i].a[3]);
            fputc('\n',stderr); break;
        }
    }
    return ok;
}
static u64 run(KCtx *g,u32 rva,u64 a0){
    mock.ncalls=0; mock.overflow=0; fx.nrows=0; memset(hp(g,fx.rows),0xee,0x10*KH_MODB_MAXCALLS);
    kh_modb_sink=&mock; u64 r=kc(g,GS_BASE+rva,a0,0x1111,0x2222,0x3333); kh_modb_sink=NULL; return r;
}
static void ov(u32 rva,u64 val){ for(int i=0;i<fx.nov;i++) if(fx.ov[i].rva==rva){ fx.ov[i].val=val; return; } fx.ov[fx.nov].rva=rva; fx.ov[fx.nov].val=val; fx.nov++; }
static void ov_clear(void){ fx.nov=0; fx.probe_hit=0; fx.probes=0; fx.poke_at=0; memset(fx.item_id,0,sizeof fx.item_id); }
#define ORIG_RET(rva) (0x0f0f0f0f00000000ull|(rva))
static void EO(u32 rva,u64 a0,u64 a1,u64 a2,u64 a3){ if(nex<KH_MODB_MAXCALLS){ ex[nex].rva=rva; ex[nex].tail=2; ex[nex].a[0]=a0; ex[nex].a[1]=a1; ex[nex].a[2]=a2; ex[nex].a[3]=a3; } nex++; }   /* forward to the original */
/* dispatch with explicit rcx, rdx, r8, r9 and a fifth (stack) argument */
static u64 kc5(KCtx *g,u64 target,u64 a,u64 b,u64 c,u64 d,u64 e,u64 *entry_rsp){
    u64 save=KH_RSP(g),sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
    u64 sp=(save-0x100)&~0xFull; wr64(g,sp+0x20,e); sp-=8; wr64(g,sp,0x7ff7ffff0000ull);
    KH_RSP(g)=sp; KH_RCX(g)=a; KH_RDX(g)=b; KH_R8(g)=c; KH_R9(g)=d; if(entry_rsp) *entry_rsp=sp;
    kh_dispatch(g,target);
    u64 r=KH_RAX(g); int popped=KH_RSP(g)==sp+8;
    KH_RSP(g)=save; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3];
    T(popped);
    return r;
}
static u64 run4(KCtx *g,u32 rva,u64 a,u64 b,u64 c,u64 d,u64 e,u64 *entry_rsp){
    mock.ncalls=0; mock.overflow=0; fx.nrows=0; fx.probes=0;
    kh_modb_sink=&mock; u64 r=kc5(g,GS_BASE+rva,a,b,c,d,e,entry_rsp); kh_modb_sink=NULL; return r;
}
#define SEQ(what) do{ int ok_=seq_ok(what); checks++; if(!ok_){ failures++; fprintf(stderr,"mod B FAIL line %d: call sequence of %s\n",__LINE__,what); } }while(0)

/* ---- 0x3f29e0 (DLL 0x306c0 + 0x30290 + 0x30510) ------------------------------------------------------------------------------- */
typedef struct {
    u8 alive[3]; u32 f6c8[3]; u8 msheet[3]; u16 ent4c[3]; s32 count238;
    u16 extra0;                          /* first word of the DLL list 0x1800819d8 */
    u8 bfb30;
    u8 rows;                             /* 0x3b27e0: the reaction-row form of both menus */
    u8 a7e0; u8 g717008; u32 form; u8 d7820;
    u8 limit_flags; u16 b68; u8 b27c0; u8 self24; u8 aa4f0; u8 abb20;
    u32 de250, de130; s32 s3b8; u32 g749804; u8 a800, a870;
    u16 slot[4]; u16 ctype[4]; u16 cword[4]; s8 abn; u16 abw[6];
    u16 stale_limit[8];                  /* content of the DLL list 0x1800819e8 before the call */
} Case;
static const struct { u8 bit; u16 id; } limit_bits[4]={{0x04,0x2ba},{0x08,0x2bd},{0x10,0x2c0},{0x20,0x2ab}};   /* DLL 0x303b4 / 0x303d2 / 0x303e2 / 0x303f2 */
static void first_row(const Case *c,u64 list){                     /* DLL 0x3030b..0x30373 and 0x30582..0x305ee */
    E(0x39a7e0,0,0,0,0);
    u32 id;
    if(!c->a7e0) id=0x68;
    else if(c->g717008==9) id=0x88;
    else if(c->form==2) id=0x16d;
    else { E(0x3d7820,fx.obj,0,0,0); id=c->d7820?0x21d:1; }
    E(0x3b0f20,list,id,0,1);
}
static void reaction_rows(u64 list){                               /* DLL 0x302b3..0x302e7 / 0x30539..0x3056a */
    E(0x3b11d0,list,0,0,0);
    for(u32 i=3;i<7;i++){ u64 e=fx.r2660+0x20*(i-3); E(0x3b2660,fx.self,i,0,0); E(0x3b0f20,list,(u32)(s32)(s16)(0x8100+i),e+0xc,(u32)(s32)(s8)(i==4?0x85:i)); }
}
static void run_case(KCtx *g,const Case *c,int line){
    u64 self=fx.self,obj=fx.obj,L=kh_mod_b_lists(); u8 limit_want[16]; u16 lim[8];
    /* guest state */
    memset(hp(g,self),0,0xc00); memset(hp(g,obj),0,0xe00); memset(hp(g,fx.sheet),0,0x200); memset(hp(g,fx.save),0,0x3700); memset(hp(g,fx.cmdrec),0,0x30*NCMD); memset(hp(g,fx.abil),0,0x40+2*0x80);
    wr64(g,self+0xae0,obj); wr64(g,obj+0x5c0,fx.sheet); wr32(g,obj+0xde0,c->form); wr8(g,fx.sheet+0x1e7,c->limit_flags);
    wr32(g,self+0x238,(u32)c->count238); wr16(g,self+0xb68,c->b68); wr8(g,self+0x24,c->self24); wr32(g,self+0x3b8,(u32)c->s3b8);
    for(int i=0;i<3;i++){ memset(hp(g,fx.member[i]),0,0x920); fx.alive[i]=c->alive[i]; wr32(g,fx.member[i]+0x6c8,c->f6c8[i]); wr64(g,fx.member[i]+0x5c0,c->msheet[i]?0x5c0000+(u64)i:0);
        wr32(g,fx.member[i]+0x914,0x9140+(u32)i); wr16(g,fx.objent[i]+0x4c,c->ent4c[i]); }
    fx.r_3bfb30=c->bfb30; fx.r_3b27e0=c->rows; fx.r_39a7e0=c->a7e0; fx.r_3d7820=c->d7820; fx.r_3b27c0=c->b27c0; fx.r_3aa4f0=c->aa4f0; fx.r_3abb20=c->abb20;
    fx.r_3de250=c->de250; fx.r_3de130=c->de130; fx.r_39a800=c->a800; fx.r_39a870=c->a870; fx.null_row=0;
    for(u32 i=3;i<7;i++){ u64 e=fx.r2660+0x20*(i-3); wr16(g,e+2,(u16)(0x8100+i)); wr8(g,e+4,(u8)(i==4?0x85:i)); }
    for(int k=0;k<4;k++){ wr16(g,fx.save+0x36f8+2*(u64)k,c->slot[k]); fx.cmd_id[k]=(s32)(s16)c->slot[k]; wr16(g,fx.cmdrec+0x30*(u64)k+2,c->ctype[k]); wr16(g,fx.cmdrec+0x30*(u64)k+4,c->cword[k]); }
    for(int k=4;k<NCMD;k++) fx.cmd_id[k]=0x7fff0000+k;
    wr8(g,fx.abil+0x12,(u8)c->abn); for(int j=0;j<6;j++) wr16(g,fx.abil+0x34+2*(u64)j,c->abw[j]);
    u8 sv717008=rd8(g,GS_BASE+0x717008); u32 sv749804=rd32(g,GS_BASE+0x749804); wr8(g,GS_BASE+0x717008,c->g717008); wr32(g,GS_BASE+0x749804,c->g749804);
    u8 svL[32]; memcpy(svL,hp(g,L),32);
    wr16(g,L,c->extra0); wr16(g,L+2,0x3e1); wr16(g,L+4,0);
    for(int i=0;i<8;i++) wr16(g,L+0x10+2*(u64)i,c->stale_limit[i]);
    /* the DLL's limit list after the call: rebuilt only on the path 0x30389 (not the reaction-row form, form 3) */
    int rebuilt=!c->rows&&c->form==3;
    if(rebuilt){ memset(lim,0,sizeof lim); int n=0; for(int b=0;b<4;b++) if(c->limit_flags&limit_bits[b].bit) lim[n++]=limit_bits[b].id; }
    else memcpy(lim,c->stale_limit,sizeof lim);
    memcpy(limit_want,lim,16);
    /* expected calls, in listing order */
    nex=0; int row=0, party_rows=0; int row5=-1, row5_val=0; int srow[4];
    E(0x3b11d0,self+0x228,0,0,0);                                                                   /* 0x30731 */
    for(u32 i=0;i<3;i++){                                                                           /* 0x30737, 0x307ae, 0x30828 */
        E(0x3d5c50,i,0,0,0); E(0x3ba720,fx.member[i],0,0,0);
        if(!c->alive[i]||(c->f6c8[i]&0x400000)||!c->msheet[i]) continue;
        E(0x3dfeb0,0x9140+i,0,0,0); E(0x3e60f0,(u32)(s32)(s16)c->ent4c[i],0,0,0);
        E(0x3b0f20,self+0x228,(u32)(HI32|(0x700+((u32)(s32)(s16)c->ent4c[i]&0xff))),0,1); row++; party_rows++;
    }
    E(0x3f3000,self,0,0,0);                                                                         /* 0x308a5 */
    E(0x3b11d0,self+0xa8,0,0,0); E(0x3b1840,self+0xa8,self+0xb68,0,0);                              /* 0x308b2, 0x308c6 */
    if(c->extra0){ E(0x3b0f20,self+0xa8,0x3e7,0,1); row++; E(0x3b11d0,self+0x6a8,0,0,0); E(0x3b1840,self+0x6a8,L,0,0); }   /* 0x308cc..0x3090c */
    E(0x3f3300,self,0,0,0); E(0x3f3110,self,0,0,0); E(0x3e7b30,self+0x4a8,0,0,0);                    /* 0x30915, 0x3091e, 0x3092b */
    E(0x3bfb30,obj,0,0,0); if(c->bfb30) E(0x3e6d10,self+0xb76,0,0,0);                               /* 0x3093b, 0x30948 */
    E(0x3b11d0,self+0x3a8,0,0,0); E(0x3b1840,self+0x3a8,self+0xb76,0,0);                            /* 0x30955, 0x30969 */
    /* helper 0x30290: the list at this+0x28 */
    E(0x3b27e0,self,0,0,0);
    if(c->rows){ reaction_rows(self+0x28); row+=4; }
    else {
        E(0x3b11d0,self+0x28,0,0,0); first_row(c,self+0x28); row++;
        if(c->form==3){ E(0x3b11d0,self+0x628,0,0,0); E(0x3b1840,self+0x628,L+0x10,0,0); E(0x3b0f20,self+0x28,0x2c3,0,1); }   /* 0x30389..0x30438 */
        else E(0x3b0f20,self+0x28,c->b68?2:0x68,0,1);                                              /* 0x30417 */
        row++;
        E(0x3b27c0,self,0,0,0); E(0x3b0f20,self+0x28,c->b27c0?0x68:3,0,1); row++;                   /* 0x30448..0x30465 */
        if(c->self24&0x20){ E(0x3b0f20,self+0x28,0x68,0,1); row++; }                                /* 0x30471 */
        else {
            E(0x3aa4f0,obj,0,0,0);
            if(c->aa4f0){ E(0x3b0f20,self+0x28,5,0,1); row5=row++; if(c->form==6){ E(0x3abb20,obj,0,0,0); row5_val=c->abb20; } }   /* 0x304a8..0x304cc */
            else { E(0x3b0f20,self+0x28,4,0,1); row5=row++; row5_val=c->form!=0; }                   /* 0x304d2..0x304e9 */
        }
    }
    /* helper 0x30510: the list at this+0x328 */
    E(0x3b27e0,self,0,0,0);
    if(c->rows){ reaction_rows(self+0x328); row+=4; }
    else {
        E(0x3b11d0,self+0x328,0,0,0); first_row(c,self+0x328); row++;
        E(0x3de250,0,0,0,0); E(0x3b0f20,self+0x328,c->de250?0x13c:(c->s3b8<=0?0x68:0x48),0,1); row++;   /* 0x305f4..0x30621 */
        E(0x3de250,0,0,0,0);
        u32 id;
        if(c->de250==2){ E(0x3de130,0,0,0,0); }
        if(c->de250==2&&c->de130==0xc9){ E(0x3f3230,self,0,0,0); id=0x25d; }                         /* 0x3063f */
        else if(c->g749804&0x2000000u) id=0x68;                                                     /* 0x30656 */
        else { E(0x39a800,0,0,0,0); id=c->a800?0x49:0x68; }                                         /* 0x30662 */
        E(0x3b0f20,self+0x328,id,0,1); row++;
        E(0x39a870,0,0,0,0); E(0x3b0f20,self+0x328,c->a870?0x51:0x68,0,1); row++;                    /* 0x30684..0x306af */
    }
    E(0x3b11d0,self+0x2a8,0,0,0); E(0x152380,0,0,0,0);                                              /* 0x30986, 0x3098c */
    for(int k=0;k<4;k++){                                                                           /* 0x309a0..0x30a45 */
        u32 id=(u32)(s32)(s16)c->slot[k], flag;
        E(0x3b68b0,id,0,0,0);
        if(c->ctype[k]==3){ E(0x3d74c0,obj,0,0,0); flag=0; for(int j=0;j<c->abn;j++) if(c->abw[j]==c->cword[k]) flag++; }
        else { if(c->form==3) id=(u32)(s32)(s16)lim[k]; flag=1; }
        E(0x3b0f20,self+0x2a8,id,0,flag); srow[k]=row++;
    }
    KH_RAX(g)=0x5eed5eed5eed5eedull;
    u64 r=run(g,0x3f29e0,self);
    int ok=seq_ok("3f29e0");
    checks++; if(!ok){ failures++; fprintf(stderr,"mod B FAIL: 3f29e0 call sequence, case at line %d\n",line); }
    /* memory effects */
    int mem_ok=1;
    if(memcmp(hp(g,L+0x10),limit_want,16)) mem_ok=0;                                                /* 16 bytes zeroed, then the ids of the set bits */
    if(rd16(g,L)!=c->extra0||rd16(g,L+2)!=0x3e1) mem_ok=0;                                           /* the extra list is only read here */
    { int n=0; for(int i=0;i<3;i++){ if(!c->alive[i]||(c->f6c8[i]&0x400000)||!c->msheet[i]) continue; n++; }
      /* the mock never changes [this+0x238], so every taken member lands in the same slot: the last one wins */
      u64 last=0; for(int i=0;i<3;i++) if(c->alive[i]&&!(c->f6c8[i]&0x400000)&&c->msheet[i]) last=fx.member[i];
      if(n!=party_rows) mem_ok=0;
      if(rd64(g,self+0xb80+8*(u64)(s64)c->count238)!=last) mem_ok=0; }
    for(int k=0;k<4;k++) if(rd8(g,fx.rows+0x10*(u64)srow[k]+6)!=k+1) mem_ok=0;                       /* 0x30a3e: mov [rax+6],bl */
    for(int i=0;i<row;i++){ u8 want5=(i==row5&&row5_val)?5:0xee; if(rd8(g,fx.rows+0x10*(u64)i+5)!=want5) mem_ok=0; }   /* [row+5]=5 on exactly one row at most */
    for(int i=0;i<row;i++){ int is_s=0; for(int k=0;k<4;k++) if(srow[k]==i) is_s=1; if(!is_s&&rd8(g,fx.rows+0x10*(u64)i+6)!=0xee) mem_ok=0; }
    if(fx.nrows!=row) mem_ok=0;
    if(r!=fx.rows+0x10*(u64)srow[3]) mem_ok=0;                                                       /* RAX = the last 0x3b0f20 result */
    checks++; if(!mem_ok){ failures++; fprintf(stderr,"mod B FAIL: 3f29e0 memory effects, case at line %d\n",line); }
    wr8(g,GS_BASE+0x717008,sv717008); wr32(g,GS_BASE+0x749804,sv749804); memcpy(hp(g,L),svL,32);
}
#define BASE_CASE {.alive={1,1,1},.f6c8={0,0,0},.msheet={1,1,1},.ent4c={0x11,0x22,0x33},.count238=0,.extra0=0,.bfb30=0,.rows=0,.a7e0=1,.g717008=0,.form=0,.d7820=0, \
                   .limit_flags=0,.b68=0,.b27c0=0,.self24=0,.aa4f0=0,.abb20=0,.de250=0,.de130=0,.s3b8=0,.g749804=0,.a800=0,.a870=0, \
                   .slot={0x10,0x20,0x30,0x40},.ctype={1,1,1,1},.cword={0,0,0,0},.abn=0,.abw={0},.stale_limit={0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888}}
static void test_3f29e0(KCtx *g){
    u64 self=fx.self,sp=KH_RSP(g);
    /* 1. pending menu: tail jump to 0x3b19b0 (this+0x528, [this+0xbd8], the whole qword [this+0xbe0], r9 = 0), nothing else (DLL 0x306c6..0x306eb) */
    memset(hp(g,self),0,0xc00); wr64(g,self+0xbd8,0x123456789abcull); wr64(g,self+0xbe0,0x7654321080000001ull); wr64(g,self+0xae0,0);
    nex=0; E(0x3b19b0,self+0x528,0x123456789abcull,0x7654321080000001ull,0); ex[0].tail=1;
    T(run(g,0x3f29e0,self)==0x19b0aaaa5555ull); T(seq_ok("3f29e0 pending")); T(KH_RSP(g)==sp);
    /* 2. no sheet: plain return, no call, RAX untouched (DLL 0x306fe -> 0x30a69) */
    memset(hp(g,self),0,0xc00); memset(hp(g,fx.obj),0,0xe00); wr64(g,self+0xae0,fx.obj);
    nex=0; KH_RAX(g)=0xfeedf00d12345678ull; T(run(g,0x3f29e0,self)==0xfeedf00d12345678ull); T(seq_ok("3f29e0 no sheet"));
    /* 3. the table: one field at a time away from the base case, then combinations */
    { Case c=BASE_CASE; run_case(g,&c,__LINE__); }
    /* party rows: dead, flagged 0x400000, no sheet; the slot index is the sign-extended count; the command id is the sign-extended word */
    for(int i=0;i<3;i++){
        { Case c=BASE_CASE; c.alive[i]=0; run_case(g,&c,__LINE__); }
        { Case c=BASE_CASE; c.f6c8[i]=0x400000; run_case(g,&c,__LINE__); }
        { Case c=BASE_CASE; c.f6c8[i]=0xffbfffff; run_case(g,&c,__LINE__); }                         /* every other bit set: still taken */
        { Case c=BASE_CASE; c.msheet[i]=0; run_case(g,&c,__LINE__); }
        { Case c=BASE_CASE; c.ent4c[i]=0x8042; run_case(g,&c,__LINE__); }                            /* movsx: 0xffff8042 */
    }
    { Case c=BASE_CASE; c.alive[0]=c.alive[1]=c.alive[2]=0; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.count238=2; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.count238=-1; run_case(g,&c,__LINE__); }                                    /* movsxd: this+0xb78 */
    /* extra command list (0x3e7) */
    { Case c=BASE_CASE; c.extra0=0x2f1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.extra0=0x8000; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.bfb30=1; run_case(g,&c,__LINE__); }
    /* first row of both menus */
    { Case c=BASE_CASE; c.a7e0=0; c.g717008=9; c.form=2; c.d7820=1; run_case(g,&c,__LINE__); }       /* 0x68 whatever else */
    { Case c=BASE_CASE; c.g717008=9; c.form=2; c.d7820=1; run_case(g,&c,__LINE__); }                 /* 0x88 */
    { Case c=BASE_CASE; c.g717008=8; c.form=2; c.d7820=1; run_case(g,&c,__LINE__); }                 /* 0x16d */
    { Case c=BASE_CASE; c.d7820=1; run_case(g,&c,__LINE__); }                                        /* 0x21d */
    { Case c=BASE_CASE; c.d7820=0; run_case(g,&c,__LINE__); }                                        /* 1 */
    /* second row: limit form list from the flag byte (all 64 values of bits 0..5, plus bits 6/7 set), else [this+0xb68] */
    for(u32 f=0;f<64;f++){ Case c=BASE_CASE; c.form=3; c.limit_flags=(u8)f; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.form=3; c.limit_flags=0xc3; run_case(g,&c,__LINE__); }                     /* no limit bit: empty list */
    { Case c=BASE_CASE; c.form=3; c.limit_flags=0xff; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.b68=0x1234; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.form=4; c.limit_flags=0x3c; run_case(g,&c,__LINE__); }                     /* not form 3: the DLL list keeps its old content */
    /* third and fourth row */
    { Case c=BASE_CASE; c.b27c0=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.self24=0x20; c.aa4f0=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.self24=0xdf; run_case(g,&c,__LINE__); }                                    /* bit 5 clear */
    for(u32 form=0;form<8;form++) for(int a=0;a<2;a++) for(int b=0;b<2;b++){ Case c=BASE_CASE; c.form=form; c.aa4f0=(u8)a; c.abb20=(u8)b; c.limit_flags=0x14; run_case(g,&c,__LINE__); }
    /* reaction-row form of both menus (0x3b27e0): rows 3..6, sign-extended id word and count byte; the limit list is NOT rebuilt */
    { Case c=BASE_CASE; c.rows=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.rows=1; c.form=3; c.limit_flags=0x3c; run_case(g,&c,__LINE__); }           /* shortcut slots take the STALE list */
    /* sub menu (this+0x328) */
    for(u32 d=0;d<4;d++) for(int s=-1;s<2;s++){ Case c=BASE_CASE; c.de250=d; c.s3b8=s; c.de130=0xc9; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.de250=2; c.de130=0xc8; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.de250=2; c.de130=0xc8; c.g749804=0x2000000; c.a800=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.g749804=0x2000000; c.a800=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.g749804=0xfdffffff; c.a800=1; run_case(g,&c,__LINE__); }                   /* every other bit: 0x49 */
    { Case c=BASE_CASE; c.g749804=0xfdffffff; c.a800=0; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.a870=1; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.s3b8=(s32)0x80000000; run_case(g,&c,__LINE__); }                            /* signed compare: <= 0 */
    /* shortcut slots: sign-extended ids, type 3 = ability count (signed byte count, 16-bit compare), else flag 1 and the limit list in form 3 */
    { Case c=BASE_CASE; c.slot[0]=0x8001; c.slot[3]=0xffff; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.ctype[1]=3; c.cword[1]=0x8123; c.abn=5; c.abw[0]=0x8123; c.abw[1]=0x123; c.abw[2]=0x8123; c.abw[4]=0x8123; c.abw[5]=0x8123; run_case(g,&c,__LINE__); }   /* 3 of the first 5; the exe's 32-bit compare would count 0 */
    { Case c=BASE_CASE; c.ctype[0]=c.ctype[2]=3; c.cword[0]=7; c.cword[2]=9; c.abn=3; c.abw[0]=9; c.abw[1]=7; c.abw[2]=9; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.ctype[3]=3; c.cword[3]=7; c.abn=0; c.abw[0]=7; run_case(g,&c,__LINE__); }
    { Case c=BASE_CASE; c.ctype[3]=3; c.cword[3]=7; c.abn=-1; c.abw[0]=7; run_case(g,&c,__LINE__); }  /* count 0xff is negative: no loop (the exe would count 255 entries) */
    { Case c=BASE_CASE; c.form=3; c.limit_flags=0x3c; c.ctype[2]=3; c.cword[2]=1; c.abn=1; c.abw[0]=1; run_case(g,&c,__LINE__); }   /* slots 0,1,3 from the limit list, slot 2 keeps its id */
    { Case c=BASE_CASE; c.form=3; c.limit_flags=0x28; run_case(g,&c,__LINE__); }                     /* two limits: slots 2 and 3 get id 0 */
    { Case c=BASE_CASE; c.form=3; c.limit_flags=0; c.ctype[0]=2; c.ctype[1]=4; c.ctype[2]=0; c.ctype[3]=0xffff; run_case(g,&c,__LINE__); }
    T(KH_RSP(g)==sp);
}

/* ---- 0x3f27d0 (DLL 0x30a80) -------------------------------------------------------------------------------------------------- */
static void run_reset(KCtx *g,const u32 *v14,const u16 slot[4],const u16 ctype[4],const u16 cword[4],const u32 conv[4],int line){
    u64 self=fx.self,L=kh_mod_b_lists(); u8 svL[32]; memcpy(svL,hp(g,L),32);
    memset(hp(g,self),0xa5,0xc00); memset(hp(g,fx.save),0x5a,0x3700); memset(hp(g,fx.cmdrec),0,0x30*NCMD);
    for(int i=0;i<16;i++){ wr16(g,L+2*(u64)i,(u16)(0xe000+i)); }
    for(int i=0;i<14;i++) fx.c5900[i]=v14[i];
    for(int k=0;k<4;k++){ wr16(g,fx.save+0x36f8+2*(u64)k,slot[k]); fx.cmd_id[k]=(s32)(s16)slot[k]; wr16(g,fx.cmdrec+0x30*(u64)k+2,ctype[k]); wr16(g,fx.cmdrec+0x30*(u64)k+4,cword[k]); fx.c5900[14+k]=conv[k]; }
    for(int k=4;k<NCMD;k++) fx.cmd_id[k]=0x7fff0000+k;
    fx.ret_3e6d10=0x6d106d106d10ull; fx.ae0_at_3b20b0=1;
    nex=0;
    E(0x3b2080,self,0,0,0); E(0x3b1b70,self+0x28,0,0,0); E(0x3b1b70,self+0x328,0,0,0); E(0x3b20b0,self,0,0,0);      /* 0x30a92..0x30abd */
    u16 want_b68[8]; int nb=0; for(int i=0;i<8;i++) want_b68[i]=0xa5a5;
    for(u32 i=0;i<6;i++){ E(0x3c5900,i,0,0,0); if(v14[i]) want_b68[nb++]=(u16)v14[i]; }               /* 0x30ac3..0x30b76 */
    want_b68[nb]=0;
    u16 want_L[16]; int nl=0; for(int i=0;i<16;i++) want_L[i]=(u16)(0xe000+i);
    for(u32 i=6;i<14;i++){ E(0x3c5900,i,0,0,0); if(v14[i]) want_L[nl++]=(u16)v14[i]; }                /* 0x30b82..0x30c5f */
    want_L[nl]=0;                                                                                   /* 0x30c72 (nl <= 7 in these cases) */
    E(0x152380,0,0,0,0);
    u16 want_slot[4];
    for(int k=0;k<4;k++){                                                                           /* 0x30c7c..0x30d2b */
        E(0x3b68b0,(u32)(s32)(s16)slot[k],0,0,0); want_slot[k]=slot[k];
        if(ctype[k]==2){ E(0x3c5900,(u32)(s32)(s16)cword[k],0,0,0); want_slot[k]=(u16)conv[k]; }
    }
    E(0x3e6d10,self+0xb76,0,0,0);                                                                   /* 0x30d32 */
    u64 r=run(g,0x3f27d0,self);
    int ok=seq_ok("3f27d0"); checks++; if(!ok){ failures++; fprintf(stderr,"mod B FAIL: 3f27d0 call sequence, case at line %d\n",line); }
    int mem_ok=1;
    if(r!=fx.ret_3e6d10) mem_ok=0;                                                                   /* RAX = the last callee's */
    if(fx.ae0_at_3b20b0!=0) mem_ok=0;                                                                /* [this+0xae0] = 0 before 0x3b20b0 runs (0x30ab2) */
    if(rd64(g,self+0xae0)!=0||rd64(g,self+0xbd8)!=0||rd32(g,self+0xbe4)!=0||rd32(g,self+0xbe0)!=0xa5a5a5a5u||rd64(g,self+0xbe8)!=0xa5a5a5a5a5a5a5a5ull||rd64(g,self+0xbd0)!=0xa5a5a5a5a5a5a5a5ull) mem_ok=0;
    for(int i=0;i<8;i++){ u16 w=rd16(g,self+0xb68+2*(u64)i); if(i<=nb?w!=want_b68[i]:w!=0xa5a5) mem_ok=0; }   /* entries, terminator, nothing else */
    if(memcmp(hp(g,L),want_L,32)) mem_ok=0;                                                          /* entries, terminator, stale words behind it, limit list untouched */
    for(int k=0;k<4;k++) if(rd16(g,fx.save+0x36f8+2*(u64)k)!=want_slot[k]) mem_ok=0;
    if(rd16(g,fx.save+0x36f6)!=0x5a5a) mem_ok=0;                                                      /* the word in front of the four slots is not touched */
    checks++; if(!mem_ok){ failures++; fprintf(stderr,"mod B FAIL: 3f27d0 memory effects, case at line %d\n",line); }
    memcpy(hp(g,L),svL,32);
}
static void fatal_overflow(KCtx *g){
    for(int i=0;i<14;i++) fx.c5900[i]=0x300+(u32)i;                                                  /* all 8 extra entries present */
    for(int k=0;k<NCMD;k++) fx.cmd_id[k]=k; memset(hp(g,fx.save),0,0x3700);
    mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f27d0,fx.self,0,0,0);
}
static void fatal_null_obj(KCtx *g){ memset(hp(g,fx.self),0,0xc00); mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f29e0,fx.self,0,0,0); }
static void fatal_null_row(KCtx *g){
    Case c=BASE_CASE; c.rows=1; u64 self=fx.self;
    memset(hp(g,self),0,0xc00); memset(hp(g,fx.obj),0,0xe00); wr64(g,self+0xae0,fx.obj); wr64(g,fx.obj+0x5c0,fx.sheet);
    fx.r_3b27e0=1; fx.null_row=5; mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f29e0,self,0,0,0);
}
static void test_3f27d0(KCtx *g){
    u64 sp=KH_RSP(g);
    static const u16 s0[4]={0x10,0x20,0x30,0x40}, t0[4]={1,1,1,1}, w0[4]={0,0,0,0}; static const u32 c0[4]={0,0,0,0};
    { u32 v[14]={0}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }                                         /* nothing present: both lists = terminator only */
    { u32 v[14]={0x1f,0x20,0x21,0x22,0x23,0x24, 0x2f1,0x2f2,0x2f3,0x2f4,0x2f5,0x2f6,0x2f7,0}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }   /* 6 + 7: the terminator is the 8th word */
    { u32 v[14]={0,0x20,0,0x22,0,0x24, 0,0x2f2,0,0x2f4,0,0,0,0x2f8}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }   /* gaps; index 6 absent: the list still starts at word 0 */
    { u32 v[14]={0x10001,0,0,0,0,0, 0x2f1,0,0,0,0,0,0,0}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }      /* 32-bit test, 16-bit store */
    { u32 v[14]={0x10000,0,0,0,0,0, 0x10000,0,0,0,0,0,0,0}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }    /* non-zero eax whose low word is 0: stored as an early terminator */
    { u32 v[14]={0,0,0,0,0,0x24, 0,0,0,0,0,0,0x2f7,0x2f8}; run_reset(g,v,s0,t0,w0,c0,__LINE__); }
    /* shortcut slots: a type-2 command is replaced by 0x3c5900(sign-extended word [cmd+4]) */
    { u32 v[14]={0}; static const u16 s[4]={0x8001,0x20,0xffff,0x40}, t[4]={2,3,2,0}, w[4]={0x100,0x101,0x102,0x103}; static const u32 c[4]={0xabc1234,0x1111,0,0x3333};
      run_reset(g,v,s,t,w,c,__LINE__); }
    { u32 v[14]={0}; static const u16 s[4]={1,2,3,4}, t[4]={2,2,2,2}, w[4]={0x100,0x101,0x102,0x103}; static const u32 c[4]={5,6,7,8}; run_reset(g,v,s,t,w,c,__LINE__); }
    T(KH_RSP(g)==sp);
    char out[4096];
    T(run_fatal(g,fatal_overflow,out,sizeof out)==3); T(strstr(out,"mod-3f27d0-extra-list-overflow")!=NULL&&strstr(out,"__report_rangecheckfailure")!=NULL);
    T(run_fatal(g,fatal_null_obj,out,sizeof out)==3); T(strstr(out,"mod 3f29e0 (DLL 0x306c0): [this+0xae0] is not a valid guest pointer")!=NULL);
    T(run_fatal(g,fatal_null_row,out,sizeof out)==3); T(strstr(out,"the 0x3b2660 row")!=NULL);
}

/* ---- the three item hooks: 0x3c4780 (DLL 0x31c60), 0x351390 (DLL 0x2d5c0), 0x368820 (DLL 0x2da70) ------------------------------- */
static u64 fatal_arg;
static void fatal_3c4780(KCtx *g){ ov_clear(); mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3c4780,0x999,0,0,0); }
static void fatal_351390(KCtx *g){ ov_clear(); ov(0x304df0,fx.self); ov(0x34bc50,1); ov(0x34bc40,fx.save); wr16(g,fx.save,0x999); mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x351390,0,0,0,0); }
static void fatal_368820(KCtx *g){ ov_clear(); mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x368820,0x999,0,0,0); }
static void test_items(KCtx *g){
    u64 sp=KH_RSP(g),it=fx.items,rec=fx.cmdrec; char out[4096];
    /* 0x3c4780: item word +0x10 -> command record byte +6 == 0xb, else the original with ecx = the id */
    static const struct { u16 cmdw; u8 kind6; int own; } c1[]={{0,0xb,0},{0x55,0xb,1},{0x55,0xa,0},{0x55,0xc,0},{0x55,0,0},{0x8005,0xb,1},{0x8005,0x0b^0x80,0}};
    for(unsigned i=0;i<sizeof c1/sizeof *c1;i++){
        ov_clear(); memset(hp(g,it),0,0x100); memset(hp(g,rec),0,0x30*NCMD);
        fx.item_id[2]=0x1234; wr16(g,it+0x40+0x10,c1[i].cmdw); fx.cmd_id[0]=(s32)(s16)c1[i].cmdw; for(int k=1;k<NCMD;k++) fx.cmd_id[k]=0x7fff0000+k; wr8(g,rec+6,c1[i].kind6);
        nex=0; E(0x3e39a0,0x1234,0,0,0);                                                                  /* DLL 0x31c6f */
        if(c1[i].cmdw) E(0x3b68b0,(u32)(s32)(s16)c1[i].cmdw,0,0,0);                                      /* 0x31c81: movsx ecx,word */
        if(!c1[i].own) EO(0x3c4780,0x1234,0x21,0x22,0x23);                                               /* 0x31c9f: jmp original, ecx = ebx (upper half of rcx dropped) */
        u64 r=run4(g,0x3c4780,0xabcdef0000001234ull,0x21,0x22,0x23,0,NULL);
        SEQ("3c4780"); T(r==(c1[i].own?((rec&~0xffull)|1):ORIG_RET(0x3c4780)));                           /* 0x31c89: mov al,1 on the record pointer */
    }
    T(run_fatal(g,fatal_3c4780,out,sizeof out)==3); T(strstr(out,"mod 3c4780 (DLL 0x31c60): the item record returned by 0x3e39a0")!=NULL);
    /* 0x351390: full replacement.  ids 0x112 / 0x113, then the kind byte through the DLL's 18-entry table */
    u64 menu=fx.self,list=fx.save+0x100;
    for(int row=-1;row<=3;row+=2) for(u32 kind=0;kind<0x1c;kind++){
        ov_clear(); memset(hp(g,it),0,0x100); memset(hp(g,fx.save),0,0x400);
        ov(0x304df0,menu); ov(0x34bc50,HI32|7); ov(0x34bc40,list);
        fx.item_id[1]=0x321; wr8(g,it+0x20+5,(u8)kind); wr16(g,list+6*(u64)(s64)row,0x321);
        nex=0; E(0x304df0,0,0,0,0); E(0x34bc50,menu,0,0,0); E(0x34bc40,menu,0,0,0); E(0x3e39a0,0x321,0,0,0);   /* 0x2d5cd, 0x2d5e0, 0x2d5f0, 0x2d5fc */
        u64 r=run4(g,0x351390,0xffffffff00000000ull|(u32)row,0,0,0,0,NULL);                              /* movsxd rdi,ecx */
        u64 want;
        if(kind<4||kind>0x15) want=(u64)(u32)(kind-4)&0xffffff00u;                                       /* 0x2d619 ja -> xor al,al on eax = kind-4 */
        else want=(kind<=8||kind>=20)?1:0;                                                               /* table 0x2d65c: kinds 4..8, 20, 21 */
        SEQ("351390"); T(r==want);
    }
    for(u32 id=0x111;id<=0x114;id++){                                                                     /* 0x2d602..0x2d60d: only 0x112 and 0x113 */
        ov_clear(); memset(hp(g,it),0,0x100); memset(hp(g,fx.save),0,0x400);
        ov(0x304df0,menu); ov(0x34bc50,1); ov(0x34bc40,list); fx.item_id[3]=id; wr8(g,it+0x60+5,9); wr16(g,list+12,(u16)id);
        nex=0; E(0x304df0,0,0,0,0); E(0x34bc50,menu,0,0,0); E(0x34bc40,menu,0,0,0); E(0x3e39a0,id,0,0,0);
        u64 r=run4(g,0x351390,2,0,0,0,0,NULL); SEQ("351390 ids");
        T(r==((id==0x112||id==0x113)?(((it+0x60)&~0xffull)|1):0));                                       /* mov al,1 on the item pointer; kind 9 otherwise -> 0 */
    }
    { ov_clear(); ov(0x304df0,menu); ov(0x34bc50,HI32);                                                    /* count 0: two calls, al cleared on the callee's rax (0x2d5e4) */
      nex=0; E(0x304df0,0,0,0,0); E(0x34bc50,menu,0,0,0); T(run4(g,0x351390,0,0,0,0,0,NULL)==HI32); SEQ("351390 empty"); }
    { ov_clear(); ov(0x304df0,menu); ov(0x34bc50,1); ov(0x34bc40,list); memset(hp(g,fx.save),0,0x400); wr16(g,list,0x113);   /* 0x113 with no record at all: still 1 */
      nex=0; E(0x304df0,0,0,0,0); E(0x34bc50,menu,0,0,0); E(0x34bc40,menu,0,0,0); E(0x3e39a0,0x113,0,0,0); T(run4(g,0x351390,0,0,0,0,0,NULL)==1); SEQ("351390 0x113"); }
    T(run_fatal(g,fatal_351390,out,sizeof out)==3); T(strstr(out,"mod 351390 (DLL 0x2d5c0): the item record returned by 0x3e39a0")!=NULL);
    /* 0x368820: kinds 1..24 through the DLL's table, everything else to the original */
    static const s8 cat[26]={-1, 0,0,0, 5,5,5,5,5, 0,0,0,0,0,0,0, 1,2,3,4, 5,5, -1, 0,0, -1};              /* by kind 0..25: DLL byte table 0x2db18 and its 7 targets */
    for(u32 kind=0;kind<0x102;kind+=(kind<0x20?1:0x37)){
        ov_clear(); memset(hp(g,it),0,0x100); fx.item_id[4]=0x77; wr8(g,it+0x80+5,(u8)kind);
        int k=(u8)kind; int c=k<26?cat[k]:-1;
        nex=0; E(0x3e39a0,0x77,0,0,0); if(c<0) EO(0x368820,0x77,0x31,0x32,0x33);
        u64 r=run4(g,0x368820,0x5555555500000077ull,0x31,0x32,0x33,0,NULL); SEQ("368820"); T(r==(c<0?ORIG_RET(0x368820):(u64)c));
    }
    T(run_fatal(g,fatal_368820,out,sizeof out)==3); T(strstr(out,"mod 368820 (DLL 0x2da70): the item record returned by 0x3e39a0")!=NULL);
    ov_clear(); T(KH_RSP(g)==sp); (void)fatal_arg;
}

/* ---- 0x4060e0 (DLL 0x33550), 0x3d1730 (DLL 0x2f970) ----------------------------------------------------------------------------- */
static void test_small(KCtx *g){
    u64 self=fx.self,who=fx.obj,list=fx.sheet;
    static const u32 types[]={0,1,9,0xb,0xf,0x10,0x11,0x64,0x7fffffff,0x80000000u,0xffffffffu};
    for(unsigned i=0;i<sizeof types/sizeof *types;i++){
        ov_clear(); ov(0x3ba720,HI8|1); wr32(g,self,types[i]);
        nex=0; E(0x3ba720,who,0,0,0); E(0x3b11d0,list,0,0,0); EO(0x4060e0,self,who,list,0x99);              /* 0x33572, 0x33590, 0x335e2 */
        T(run4(g,0x4060e0,self,who,list,0x99,0,NULL)==ORIG_RET(0x4060e0)); SEQ("4060e0");
    }
    { ov_clear(); ov(0x3ba720,HI8|1); wr32(g,self,0xa);                                                     /* 0x33588: [this]==0xa -> al = 1, nothing else */
      nex=0; E(0x3ba720,who,0,0,0); T(run4(g,0x4060e0,self,who,list,0,0,NULL)==(HI8|1)); SEQ("4060e0 type 0xa"); }
    { ov_clear(); ov(0x3ba720,HI8); wr32(g,self,0xa);                                                       /* 0x33576: the gate on ARGUMENT 2 fails: return what it returned */
      nex=0; E(0x3ba720,who,0,0,0); T(run4(g,0x4060e0,self,who,list,0,0,NULL)==HI8); SEQ("4060e0 gate"); }
    { ov_clear(); ov(0x3ba720,HI8); wr32(g,self,3); wr64(g,self+8,0);                                       /* the exe would go on here ([this+8] is null); the DLL does not */
      nex=0; E(0x3ba720,who,0,0,0); T(run4(g,0x4060e0,self,who,list,0,0,NULL)==HI8); SEQ("4060e0 gate, no owner"); }
    /* 0x3d1730: [[this]+0x2c] = id of argument 2 (the pointer in [this] is taken BEFORE the call), then the original */
    u64 inner=fx.abil,other=fx.r2660;
    ov_clear(); memset(hp(g,inner),0xcc,0x40); memset(hp(g,other),0xcc,0x40); wr64(g,self,inner); fx.poke_at=self; fx.poke_val=other;
    nex=0; E(0x4ad240,who,0,0,0); EO(0x3d1730,self,who,0x77,0x99);
    T(run4(g,0x3d1730,self,who,0x77,0x99,0,NULL)==ORIG_RET(0x3d1730)); SEQ("3d1730");
    T(rd32(g,inner+0x2c)==(u32)(who*0x9E3779B1ull)&&rd32(g,inner+0x28)==0xccccccccu&&rd32(g,inner+0x30)==0xccccccccu);
    T(rd32(g,other+0x2c)==0xccccccccu);
    ov_clear();
}

/* ---- the extra-file chain: 0x3a17f0 (DLL 0x33610), 0x3f6620 (DLL 0x300a0), 0x3f65a0 (DLL 0x2fd20), 0x3f6510 (DLL 0x30180) ------- */
static int probe_names_ok(int first,int n,const char *fmt,int base){
    for(int i=0;i<n;i++){ char want[16]; memset(want,0,sizeof want); snprintf(want,sizeof want,fmt,base+i); const struct kh_modb_call *c=&mock.c[first+i];
        if(c->rva!=0x3f95f0||!c->memok||memcmp(c->mem,want,16)) return 0; }
    return 1;
}
static void fatal_probe_3a17f0(KCtx *g){ ov_clear(); fx.probe_hit=4; fx.probe_val=0x100000000ull; mock.ncalls=0; fx.probes=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3a17f0,0,0,0,0); }
static void fatal_probe_3f6620(KCtx *g){ ov_clear(); fx.probe_hit=13; fx.probe_val=0x1c0; mock.ncalls=0; fx.probes=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f6620,0,0,0,0); }
static void fatal_probe_3f65a0(KCtx *g){ ov_clear(); ov(0x3e1730,0x5c0000); fx.probe_hit=1; fx.probe_val=0x20; mock.ncalls=0; fx.probes=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f65a0,0,0,0,0); }
static void fatal_script_3f65a0(KCtx *g){ ov_clear(); ov(0x3e1730,0); mock.ncalls=0; fx.probes=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x3f65a0,0,0,0,0); }
static void test_extra_files(KCtx *g){
    u64 names=kh_mod_b_names(); char out[4096];
    T(names&&!strcmp(gstr(g,names),"01common.bdx")&&!strcmp(gstr(g,names+16*4),"05common.bdx")&&!strcmp(gstr(g,names+16*5),"01effect.bar")&&!strcmp(gstr(g,names+16*24),"20effect.bar"));
    /* 0x3a17f0: five probes 01..05common.bdx, then the original with its arguments */
    ov_clear(); nex=0; for(int k=0;k<5;k++) E(0x3f95f0,names+16*(u64)k,0,0,0); EO(0x3a17f0,0x41,0x42,0x43,0x44);
    T(run4(g,0x3a17f0,0x41,0x42,0x43,0x44,0,NULL)==ORIG_RET(0x3a17f0)); SEQ("3a17f0"); T(probe_names_ok(0,5,"%02dcommon.bdx",1));
    /* 0x3f6620: twenty probes 01..20effect.bar (32-bit test of the result: a value with only the upper half set is "absent"), then the original */
    ov_clear(); fx.probe_hit=7; fx.probe_val=0x700000000ull;
    nex=0; for(int n=1;n<=20;n++) E(0x3f95f0,names+16*(u64)(4+n),0,0,0); EO(0x3f6620,0x41,0x42,0x43,0x44);
    T(run4(g,0x3f6620,0x41,0x42,0x43,0x44,0,NULL)==ORIG_RET(0x3f6620)); SEQ("3f6620"); T(probe_names_ok(0,20,"%02deffect.bar",1));
    /* 0x3f65a0: trigger 0x66 on the primary common script (new, run, reset, free), five probes, the original */
    u64 sv=rd64(g,GS_BASE+0x2a0f688); wr64(g,GS_BASE+0x2a0f688,0x1234500);
    ov_clear(); ov(0x3e1730,0x5c0000);
    nex=0; E(0x3e1730,0x1234500,0,0,0); E(0x3e1840,0x5c0000,0x66,0,0); E(0x3e1410,0x5c0000,0,0,0); E(0x3e1510,0x5c0000,0,0,0);   /* 0x2fd42..0x2fd85 */
    for(int k=0;k<5;k++) E(0x3f95f0,names+16*(u64)k,0,0,0); EO(0x3f65a0,0x41,0x42,0x43,0x44);
    T(run4(g,0x3f65a0,0x41,0x42,0x43,0x44,0,NULL)==ORIG_RET(0x3f65a0)); SEQ("3f65a0"); T(probe_names_ok(4,5,"%02dcommon.bdx",1));
    wr64(g,GS_BASE+0x2a0f688,sv);
    /* 0x3f6510: nothing to release, straight to the original */
    ov_clear(); nex=0; EO(0x3f6510,0x41,0x42,0x43,0x44); T(run4(g,0x3f6510,0x41,0x42,0x43,0x44,0,NULL)==ORIG_RET(0x3f6510)); SEQ("3f6510");
    /* a probe that finds a file stops loudly, with the name; 64-bit test in 0x3a17f0 / 0x3f65a0, 32-bit in 0x3f6620 */
    T(run_fatal(g,fatal_probe_3a17f0,out,sizeof out)==3); T(strstr(out,"mod-extra-file-not-ported: 3a17f0 (DLL 0x33610)")!=NULL&&strstr(out,"\"04common.bdx\"")!=NULL);
    T(run_fatal(g,fatal_probe_3f6620,out,sizeof out)==3); T(strstr(out,"mod-extra-file-not-ported: 3f6620 (DLL 0x300a0)")!=NULL&&strstr(out,"\"13effect.bar\"")!=NULL);
    T(run_fatal(g,fatal_probe_3f65a0,out,sizeof out)==3); T(strstr(out,"mod-extra-file-not-ported: 3f65a0 (DLL 0x2fd20)")!=NULL&&strstr(out,"\"01common.bdx\"")!=NULL);
    T(run_fatal(g,fatal_script_3f65a0,out,sizeof out)==3); T(strstr(out,"mod 3f65a0 (DLL 0x2fd20): common script allocation")!=NULL);
    ov_clear();
}

/* ---- the command-menu icon hooks: 0x153850 (DLL 0x10340), 0x15aab0 (DLL 0x10590), 0x15aba0 (DLL 0x10100) ------------------------- */
static void set_layout(KCtx *g,const char *name){ memset(hp(g,GS_BASE+0x9babb0),0,32); strcpy(hp(g,GS_BASE+0x9babb0),name); }
static void fatal_15aba0(KCtx *g){ memset(hp(g,fx.self),0,0x40); mock.ncalls=0; kh_modb_sink=&mock; kc5(g,GS_BASE+0x15aba0,0,fx.sheet,1,2,fx.self,NULL); }
static int text_ok(KCtx *g,u64 out,u8 icon,u8 colour){
    static const u8 mid[4]={0xff,0x1c,0x00,0x09}, tail[5]={0x11,0x02,0x00,0x1c,0x00}; const u8 *b=hp(g,out);
    if(b[0]!=0x11||b[1]!=icon||memcmp(b+2,mid,4)||b[6]!=colour||memcmp(b+7,tail,5)) return 0;
    for(int i=12;i<32;i++) if(b[i]!=0xcc) return 0;                                                       /* the message is the callee's business */
    return 1;
}
static void test_icons(KCtx *g){
    char path[]="/tmp/khnmc-modb-XXXXXX", err[160]="", out_txt[4096]; int fd=mkstemp(path); FILE *f=fd>=0?fdopen(fd,"wb"):NULL; T(f!=NULL); if(!f) return;
    /* a table like the shipped one: 16 header bytes (count at +4), 16-byte entries: name, byte +0xc (0x15aab0), byte +0xe (0x153850) */
    static const struct { const char *name; u8 c,e; } ent[]={{"bb0",0xeb,0xec},{"tt6",0xe8,0xea},{"abcdefghijkl",0x11,0x22},{"dup",0x01,0x02},{"dup",0x03,0x04},{"",0x7e,0x7f}};
    u8 hdr[16]={0}; u32 n=sizeof ent/sizeof *ent; memcpy(hdr+4,&n,4); fwrite(hdr,1,16,f);
    for(u32 i=0;i<n;i++){ u8 e[16]; memset(e,0,16); memcpy(e,ent[i].name,strlen(ent[i].name)); e[0xc]=ent[i].c; e[0xd]=0xff; e[0xe]=ent[i].e; e[0xf]=0xff; fwrite(e,1,16,f); }
    fclose(f);
    T(kh_mod_b_load_command_menu(path,err,sizeof err)==1&&kh_mod_b_command_menu(NULL)==n);
    u8 svname[32]; memcpy(svname,hp(g,GS_BASE+0x9babb0),32);
    u64 outb=fx.sheet,list=fx.self,owner=fx.obj; wr64(g,list+8,owner);
    static const struct { const char *layout; int ca,c9; } lc[]={                                          /* -1: no entry matches (0xe2 / 0xe7) */
        {"tt6command.2dd",0xea,0xe8},{"bb0command.2dd",0xec,0xeb},{"abcdefghijklcommand.2dd",0x22,0x11},{"dupcommand.2dd",0x02,0x01},{"command.2dd",0x7f,0x7e},
        {"zz9command.2dd",-1,-1},{"TT6command.2dd",-1,-1},{"tt6command.2d",-1,-1},{"tt6command.2ddx",-1,-1},{"tt6",-1,-1},{"",-1,-1},{"abcdefghijkcommand.2dd",-1,-1}};
    for(unsigned i=0;i<sizeof lc/sizeof *lc;i++){
        set_layout(g,lc[i].layout); u8 a=0x55,b=0x56,c=0x57; kh_mod_b_icons(&a,&b,&c,1);
        nex=0; u64 r=run4(g,0x153850,0x1234,0,0,0,0,NULL); SEQ("153850");                               /* no guest call at all; ecx is not read */
        u8 want=(u8)(lc[i].ca<0?0xe2:lc[i].ca); T(r==(u64)(u32)(s32)(s8)want);                           /* 0x104c7: movsx eax,byte */
        kh_mod_b_icons(&a,&b,&c,0); T(a==0x55&&b==0x56&&c==want);
        memset(hp(g,outb),0xcc,32); ov_clear(); ov(0x3ad9a0,0x7e57000);
        nex=0; E(0x3ad9a0,0x4925,0,0,0); E(0x3ae260,0x7e57000,outb+0xc,0,0);                            /* 0x10773, 0x1077e */
        r=run4(g,0x15aab0,0x99,outb,0xffffff00000000d7ull,0xeeeeeeee00004925ull,0,NULL); SEQ("15aab0");
        want=(u8)(lc[i].c9<0?0xe7:lc[i].c9); T(r==outb); T(text_ok(g,outb,want,0xd7));
    }
    /* an empty table leaves both bytes as they were (0x1038f / 0x105e9: the loop is not entered) */
    f=fopen(path,"wb"); memset(hdr,0,16); fwrite(hdr,1,16,f); fclose(f); T(kh_mod_b_load_command_menu(path,err,sizeof err)==1&&kh_mod_b_command_menu(NULL)==0);
    { set_layout(g,"tt6command.2dd"); u8 a=0x55,b=0x96,c=0x97; kh_mod_b_icons(&a,&b,&c,1);
      nex=0; T(run4(g,0x153850,0,0,0,0,0,NULL)==0xffffff97u); SEQ("153850 empty table");
      memset(hp(g,outb),0xcc,32); ov_clear(); ov(0x3ad9a0,0x7e57000); nex=0; E(0x3ad9a0,5,0,0,0); E(0x3ae260,0x7e57000,outb+0xc,0,0);
      T(run4(g,0x15aab0,0,outb,3,5,0,NULL)==outb); SEQ("15aab0 empty table"); T(text_ok(g,outb,0x96,3)); }
    /* a file that is shorter than its count: resized, zero behind what was read (an all-zero entry is the name "") */
    f=fopen(path,"wb"); n=3; memset(hdr,0,16); memcpy(hdr+4,&n,4); fwrite(hdr,1,16,f); { u8 e[16]={'x','y',0,0,0,0,0,0,0,0,0,0,0x31,0,0x32,0}; fwrite(e,1,16,f); } fclose(f);
    T(kh_mod_b_load_command_menu(path,err,sizeof err)==1&&kh_mod_b_command_menu(NULL)==3);
    { set_layout(g,"command.2dd"); u8 a=0,b=0,c=0; kh_mod_b_icons(&a,&b,&c,1); nex=0; T(run4(g,0x153850,0,0,0,0,0,NULL)==0); SEQ("153850 short file"); }
    f=fopen(path,"wb"); fwrite(hdr,1,7,f); fclose(f); T(kh_mod_b_load_command_menu(path,err,sizeof err)==0&&kh_mod_b_command_menu(NULL)==0);   /* no full header: nothing loaded */
    unlink(path); T(kh_mod_b_load_command_menu(path,err,sizeof err)==0);
    /* 0x15aba0: the icon by the list's menu type [[arg5+8]+0xab0]; types without an entry keep the icon of the previous call */
    static const struct { s32 type; int icon; } tc[]={{0,-1},{1,0xde},{2,0xdd},{3,0xe2},{4,0xe2},{5,0xe3},{6,0xdc},{7,0xdc},{8,0xe1},{9,0xde},{10,0xe2},{11,0xe4},{12,0xe0},{13,-1},{15,-1},
        {16,-1},{0x63,-1},{0x64,0},{0x65,0},{0x6d,0},{0x6e,0},{0x6f,-1},{-1,-1},{0x7fffffff,-1},{(s32)0x80000001u,-1},{2,0xdd},{0x40,-1}};
    u8 prev=0x77; { u8 a=prev,b=0,c=0; kh_mod_b_icons(&a,&b,&c,1); }
    for(unsigned i=0;i<sizeof tc/sizeof *tc;i++){
        wr32(g,owner+0xab0,(u32)tc[i].type); memset(hp(g,outb),0xcc,32); ov_clear(); ov(0x3ad9a0,0x7e57000);
        nex=0; E(0x3ad9a0,0x321,0,0,0); E(0x3ae260,0x7e57000,outb+0xc,0,0);                             /* 0x10265, 0x1026e */
        u64 r=run4(g,0x15aba0,0x99,outb,0xaaaaaaaaaaaaaa1dull,0xbbbbbbbb00000321ull,list,NULL); SEQ("15aba0");
        if(tc[i].icon>=0) prev=(u8)tc[i].icon;
        T(r==outb); T(text_ok(g,outb,prev,0x1d));
    }
    T(run_fatal(g,fatal_15aba0,out_txt,sizeof out_txt)==3); T(strstr(out_txt,"mod 15aba0 (DLL 0x10100): the list's owner [arg5+8] is not a valid guest pointer")!=NULL);
    memcpy(hp(g,GS_BASE+0x9babb0),svname,32); ov_clear();
    /* back to the table of the data tree, and its shipped shape when it is there (29 entries, first "bb0" with eb / ec) */
    const char *root=getenv("KHNMC_DATA"); char host[4096]; int have=kh_resolve_ci(root?root:"data-full","dll/tweaks/command_menu_offsets.bin",host,sizeof host);
    if(have){ const u8 *e=NULL; T(kh_mod_b_load_command_menu(host,err,sizeof err)==1); u32 cnt=kh_mod_b_command_menu(&e);
        T(cnt==29&&e&&!memcmp(e,"bb0\0",4)&&e[0xc]==0xeb&&e[0xe]==0xec); }
    else { kh_mod_b_load_command_menu("/nonexistent/command_menu_offsets.bin",err,sizeof err); fprintf(stderr,"kh: mod B selftest: no dll/tweaks/command_menu_offsets.bin under %s: the shipped-table check is skipped\n",root?root:"data-full"); }
    { u8 a=0,b=0,c=0; kh_mod_b_icons(&a,&b,&c,1); }
}

/* ---- 0x31dd70 (DLL 0x2d670) ----------------------------------------------------------------------------------------------------- */
static void fatal_31dd70(KCtx *g){ ov_clear(); ov(0x2f6230,HI8); wr64(g,GS_BASE+0xbef528,0); mock.ncalls=0; kh_modb_sink=&mock; kc(g,GS_BASE+0x31dd70,fx.self,0,0,0); }
static void test_menu(KCtx *g){
    u64 self=fx.self,cur=fx.sheet,ids=fx.sheet+0x40,pad=fx.sheet+0x100,t=fx.abil; char out[4096];
    u64 sv1=rd64(g,GS_BASE+0xbef528),sv2=rd64(g,GS_BASE+0xbef538); u8 svw[0x30]; memcpy(svw,hp(g,GS_BASE+0x717008),0x30);
    wr64(g,GS_BASE+0xbef528,cur); wr64(g,GS_BASE+0xbef538,ids);
    for(int i=0;i<0x30;i++) wr8(g,GS_BASE+0x717008+(u64)i,(u8)(0x81+3*i));                                /* values >= 0x80: zero extension matters */
    for(int i=0;i<0x40;i++) wr8(g,t+(u64)i,(u8)(0xc0+i));                                                 /* negative bytes: sign extension matters */
    /* 1. 0x2f6230 answers: 0x151730, al = 1, no original (DLL 0x2d69f) */
    ov_clear(); ov(0x2f6230,HI8|1); ov(0x151730,0x1717171717171700ull);
    nex=0; E(0x2f5f30,self,0,0,0); E(0x2f6230,0,0,0,0); E(0x151730,0,0,0,0);
    T(run4(g,0x31dd70,self,0x61,0x62,0x63,0,NULL)==0x1717171717171701ull); SEQ("31dd70 early");
    /* 2. everything that goes to the original untouched */
    static const struct { int pad; u32 trig; u16 id; } pass[]={{0,0,0x200},{1,0xfeffffffu,0x200},{1,0x1000000,0x1ff},{1,0x1000000,0x205},{1,0x1000000,0},{1,0x1000000,0x7fff},{1,0xffffffffu,0x8200},{1,0,0x204}};
    for(unsigned i=0;i<sizeof pass/sizeof *pass;i++){
        ov_clear(); ov(0x2f6230,HI8); ov(0x39b760,pass[i].pad?pad:0); wr32(g,pad+8,pass[i].trig); wr16(g,cur,3); wr16(g,ids+6,pass[i].id);
        nex=0; E(0x2f5f30,self,0,0,0); E(0x2f6230,0,0,0,0); E(0x39b760,0,0,0,0); EO(0x31dd70,self,0x61,0x62,0x63);
        T(run4(g,0x31dd70,self,0x61,0x62,0x63,0,NULL)==ORIG_RET(0x31dd70)); SEQ("31dd70 pass");
    }
    /* 3. the five custom rows, confirm trigger set */
    for(u32 id=0x200;id<=0x204;id++) for(u32 row=0;row<6;row+=5){
        ov_clear(); ov(0x2f6230,HI8); ov(0x39b760,pad); wr32(g,pad+8,0x1000000); wr16(g,cur,(u16)row); wr16(g,ids+2*(u64)row,(u16)id); ov(0x3ad500,t);
        ov(0x152990,0x2929292929292955ull); ov(0x3aabe0,0x3a3a3a3a3a3a3a66ull); ov(0x31d850,0x3131313131313177ull);
        u64 esp=0; nex=0; E(0x2f5f30,self,0,0,0); E(0x2f6230,0,0,0,0); E(0x39b760,0,0,0,0);
        u8 want[10]; int jump=0; u64 ret=0;
        if(id==0x203){ E(0x3ad500,0,0,0,0); E(0x31d850,(u32)(s32)(s8)(0xc0+row+0x38),0,0,0); ret=0x3131313131313100ull; }   /* 0x2d7b7 */
        else {
            E(0x2eb790,0,0,0,0); E(0x1dd170,2,0,0,0); E(0x2f6500,0,0,0,0);
            if(id==0x202){ E(0x3ad500,0,0,0,0); E(0x3aabe0,(u32)((s32)(s8)0xe7+(s32)row),(u32)((s32)(s8)0xeb+(s32)row),0,0); ret=0x3a3a3a3a3a3a3a00ull; }   /* 0x2d7cc */
            else if(id==0x204){ E(0x3ad500,0,0,0,0); jump=1;                                              /* 0x2d718: bytes +0x30..+0x32, sign-extended bytes +0x33, +0x35, +0x37 */
                u8 w[10]={0xf0,0xf1,0xf2,0, 0xf3,0xff, 0xf5,0xff, 0xf7,0xff}; memcpy(want,w,10); }
            else { int o=id==0x201?0x20:0; jump=1; ret=0x2929292929292900ull;                              /* 0x2d801 / 0x2d876: world bytes, zero-extended */
                u8 w[10]={(u8)(0x81+3*o),(u8)(0x81+3*(o+1)),(u8)(0x81+3*(o+2)),0,(u8)(0x81+3*(o+4)),0,(u8)(0x81+3*(o+6)),0,(u8)(0x81+3*(o+8)),0}; memcpy(want,w,10); }
        }
        int jump_at=nex; if(jump) E(0x152990,0,1,1,1);
        if(id==0x204) EO(0x31dd70,self,0x61,0x62,0x63);                                                   /* 0x204 goes on into the original */
        u64 r=run4(g,0x31dd70,self,0x61,0x62,0x63,0,&esp);
        if(jump&&mock.ncalls>jump_at){ ex[jump_at].a[0]=mock.c[jump_at].a[0];                              /* the record lives in the hook's frame: below its stack pointer */
            T(mock.c[jump_at].a[0]<esp&&mock.c[jump_at].a[0]>=esp-0x80&&mock.c[jump_at].memok&&!memcmp(mock.c[jump_at].mem,want,10)); }
        SEQ("31dd70 custom row"); T(r==(id==0x204?ORIG_RET(0x31dd70):ret));
    }
    T(run_fatal(g,fatal_31dd70,out,sizeof out)==3); T(strstr(out,"mod 31dd70 (DLL 0x2d670): the menu cursor pointer")!=NULL);
    wr64(g,GS_BASE+0xbef528,sv1); wr64(g,GS_BASE+0xbef538,sv2); memcpy(hp(g,GS_BASE+0x717008),svw,0x30); ov_clear();
}

/* ---- the options whose detours are not ported: off in the shipped ini files, a named fatal when a tree turns one on ---------------- */
static char opt_root[64];
static void fatal_option(KCtx *g){ kh_mod_b_check_options(g,opt_root); }
static void put(const char *dir,const char *name,const char *text){ char p[256]; snprintf(p,sizeof p,"%s/dll/%s",dir,name); FILE *f=fopen(p,"w"); if(f){ fputs(text,f); fclose(f); } }
static void test_options(KCtx *g){
    char dir[]="/tmp/khnmc-modb-opt-XXXXXX", sub[96], out[4096]; if(!mkdtemp(dir)){ T(0); return; }
    snprintf(sub,sizeof sub,"%s/dll",dir); T(mkdir(sub,0700)==0); snprintf(opt_root,sizeof opt_root,"%s",dir);
    T(kh_mod_b_option_on(dir,"KH2_Tweaks_Retry.ini","retry")==-1);                                        /* no file */
    put(dir,"KH2_Tweaks_Retry.ini","[Settings]\r\nretry=off");                                             /* the shipped files: CRLF, no final newline */
    put(dir,"KH2_Tweaks_Savepoint.ini","[Settings]\r\nsavepoint_qol=off\r\n\r\n#Allows savepoints to restore Drive Gauge.\r\n");
    put(dir,"KH2_Tweaks_Worlds.ini","[Settings]\r\nworldpoint_amount=on\r\napply_worldsymbol_patch=on\r\npatch_gummiskip=off\r\nhalve_gummiship_plans=off\r\n");
    T(kh_mod_b_option_on(dir,"KH2_Tweaks_Retry.ini","retry")==0&&kh_mod_b_option_on(dir,"KH2_Tweaks_Savepoint.ini","savepoint_qol")==0&&kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","patch_gummiskip")==0);
    T(kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","worldpoint_amount")==1&&kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","no_such_key")==-1);
    put(dir,"KH2_Tweaks_Worlds.ini","[Settings]\r\nworldpoint_amount=True\r\napply_worldsymbol_patch=1\r\npatch_gummiskip=0\r\nhalve_gummiship_plans=false\r\nhalve_tinyship_plans=\r\n");
    T(kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","worldpoint_amount")==1&&kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","apply_worldsymbol_patch")==1);
    T(kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","patch_gummiskip")==0&&kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","halve_gummiship_plans")==0&&kh_mod_b_option_on(dir,"KH2_Tweaks_Worlds.ini","halve_tinyship_plans")==0);
    put(dir,"KH2_Tweaks_Worlds.ini","[Settings]\r\nworldpoint_amount=on\r\napply_worldsymbol_patch=on\r\npatch_gummiskip=off\r\nhalve_gummiship_plans=off\r\n");
    kh_mod_b_check_options(g,dir); T(1);                                                                    /* all off: returns */
    /* The DLL's boolean reader 0x2a840: the value (GetPrivateProfileStringA, then the DLL's trim of " \t\r\n") is lower-cased
     * (0x2a8f0) and is TRUE for exactly "on" (0x2a9ad), "true" (0x2a9d3) or "1" (0x2a9f9); everything else is FALSE (0x2aa85). */
    static const struct { const char *ini,*text,*msg; int on; } oc[]={
        {"KH2_Tweaks_Retry.ini","[Settings]\r\nretry=on\r\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\n retry = on \n","KH2_Tweaks_Retry.ini has retry set to on",1}, /* the value is trimmed */
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=on #comment\n",NULL,0},                              /* an inline comment is part of the value: not \"on\" */
        {"KH2_Tweaks_Retry.ini","[Other]\nretry=on\n[Settings]\nretry=off\n",NULL,0},                    /* only [Settings] */
        {"KH2_Tweaks_Savepoint.ini","[Settings]\nsavepoint_qol=ON\n","KH2_Tweaks_Savepoint.ini has savepoint_qol set to on",1},
        {"KH2_Tweaks_Worlds.ini","[Settings]\npatch_gummiskip=on\n","KH2_Tweaks_Worlds.ini has patch_gummiskip set to on",1},
        /* the two other spellings of TRUE, for each of the three options */
        {"KH2_Tweaks_Retry.ini","[Settings]\r\nretry=true\r\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\r\nretry=1","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\ttRuE \r\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Savepoint.ini","[Settings]\nsavepoint_qol=TRUE\n","KH2_Tweaks_Savepoint.ini has savepoint_qol set to on",1},
        {"KH2_Tweaks_Savepoint.ini","[Settings]\nsavepoint_qol=1\n","KH2_Tweaks_Savepoint.ini has savepoint_qol set to on",1},
        {"KH2_Tweaks_Worlds.ini","[Settings]\npatch_gummiskip=1\n","KH2_Tweaks_Worlds.ini has patch_gummiskip set to on",1},
        {"KH2_Tweaks_Worlds.ini","[Settings]\npatch_gummiskip=True\n","KH2_Tweaks_Worlds.ini has patch_gummiskip set to on",1},
        /* GetPrivateProfileStringA drops one pair of enclosing quotes; what is left goes through the DLL's own trim */
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\"on\"\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry='TRUE'\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\" 1 \"\n","KH2_Tweaks_Retry.ini has retry set to on",1},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\"\"on\"\"\n",NULL,0},                              /* one pair only */
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\"on\n",NULL,0},                                     /* no closing quote: the quote stays */
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry='on\"\n",NULL,0},
        /* FALSE: the three spellings the DLL tests for it (0x74374 "off", 0x72da8 "false", 0x74378 "0") and everything else */
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=false\n",NULL,0}, {"KH2_Tweaks_Retry.ini","[Settings]\nretry=0\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=\n",NULL,0},       {"KH2_Tweaks_Retry.ini","[Settings]\nretry=yes\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=2\n",NULL,0},      {"KH2_Tweaks_Retry.ini","[Settings]\nretry=10\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=01\n",NULL,0},     {"KH2_Tweaks_Retry.ini","[Settings]\nretry=tru\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=truee\n",NULL,0},  {"KH2_Tweaks_Retry.ini","[Settings]\nretry=onn\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=o\n",NULL,0},      {"KH2_Tweaks_Retry.ini","[Settings]\nretry=1 ;x\n",NULL,0},
        {"KH2_Tweaks_Retry.ini","[Settings]\nretry=#1\n",NULL,0},     {"KH2_Tweaks_Retry.ini","[Settings]\nretry=;true\n",NULL,0},
        {"KH2_Tweaks_Savepoint.ini","[Settings]\nsavepoint_qol=FALSE\n",NULL,0}, {"KH2_Tweaks_Worlds.ini","[Settings]\npatch_gummiskip=0\n",NULL,0}};
    for(unsigned i=0;i<sizeof oc/sizeof *oc;i++){
        put(dir,"KH2_Tweaks_Retry.ini","[Settings]\nretry=off\n"); put(dir,"KH2_Tweaks_Savepoint.ini","[Settings]\nsavepoint_qol=off\n"); put(dir,"KH2_Tweaks_Worlds.ini","[Settings]\npatch_gummiskip=off\n");
        put(dir,oc[i].ini,oc[i].text);
        int st=run_fatal(g,fatal_option,out,sizeof out);
        if(oc[i].on){ T(st==3); T(strstr(out,"mod-option-not-ported:")!=NULL&&strstr(out,oc[i].msg)!=NULL); } else T(st==99);
    }
    char p[256]; static const char *const fn[]={"KH2_Tweaks_Retry.ini","KH2_Tweaks_Savepoint.ini","KH2_Tweaks_Worlds.ini"};
    for(int i=0;i<3;i++){ snprintf(p,sizeof p,"%s/dll/%s",dir,fn[i]); unlink(p); } rmdir(sub); rmdir(dir);
}

int kh_mod_hooks_b_selftest(KCtx *g){
    checks=failures=0;
    T(kh_mod_b_lists()!=0&&GA_OK(kh_mod_b_lists())&&GA_OK(kh_mod_b_lists()+31));
    fx_init(g);
    test_3f29e0(g);
    test_3f27d0(g);
    test_items(g);
    test_small(g);
    test_extra_files(g);
    test_icons(g);
    test_menu(g);
    test_options(g);
    fx_free();
    T(kh_modb_sink==NULL);
    fprintf(stderr,"kh: mod B selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
