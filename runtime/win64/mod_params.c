/* Task 11: the two player-motion data hooks of KH2Tweaks_Refactor.dll, derived only from `objdump -d -Mintel` of the shipped DLL
 * (nothing executed) and cross-checked against the exe's own callees.
 *
 *   DLL 0x2df50 -> exe 0x40d810  range-trigger dispatcher   f(const Range *R [rcx], Ctx *C [rdx]); A = C+0x10 actor
 *     R: +2 u8 end-frame low byte, +4 s8 kind, +5 u8 nparam, +6.. u16 params.  DLL switch: signed kind 54..83 (30 entries, table 0x2ebb8).
 *   DLL 0x2ec30 -> exe 0x40dd70  frame-op dispatcher        f(const Frame *R, Ctx *C)
 *     R: +2 s8 op, +3 u8 nparam, +4 s16 X, +6 s16 Y.  DLL switch: signed op 50..80 (31 entries, table 0x2f02c; X 7-entry table 0x2f0a8).
 *
 * Both handlers run their extension and then ALWAYS forward to the original (DLL: call through slot 0x82058 / 0x82050, rcx/rdx unchanged).
 * Kinds outside the table go straight to the original.  The DLL's function-pointer slots are resolved by byte-signature scans of the exe; the
 * 20 callees below were re-derived from those scans (all unique hits on the pristine exe) and are the lifted functions here:
 *   39b760 getter (table at +bf3270, stride 0x58)   3b3e80 getter (unused result)  3c6dc0/3c8310 play-motion helpers  3bde30/3d74e0/3d78d0 vector+lookup+move
 *   3c0ad0 drive  3c0c30 HP  3c0cd0 MP (sheet+...)   4ad240 actor->id   3e1840 script entry   3eaae0/3b68b0 table records   3d9fe0 start effect
 *   3b4da0 set state  3db2f0 run by name  1a85e0 set flag from sheet  409b00 timed motion  3cb9f0 spawn
 * The DLL's console logging (std::cout) has no effect on state and is not reproduced.  Where the DLL would fault on a null pointer
 * (it never checks) this port stops with a named fatal that carries the kind and the call-site RVA instead of faulting silently. */
#include "mod_config.h"
#include <string.h>
#include <pthread.h>
#include <sys/types.h>

void orig_f_40d810(KCtx *g);
void orig_f_40dd70(KCtx *g);
struct kh_mp_mock *kh_mp_mock_sink;

#define RANGE_EXT_LO 54
#define RANGE_EXT_HI 83
#define FRAME_EXT_LO 50
#define FRAME_EXT_HI 80

/* ---- counters (per hook, per signed kind byte) -------------------------------------------------------------------- */
static _Atomic u64 rng_calls, rng_forward_only, rng_orig_calls;
static _Atomic u64 rng_seen[256], rng_acted[256];
static _Atomic u64 frm_calls, frm_forward_only, frm_orig_calls;
static _Atomic u64 frm_seen[256], frm_acted[256];
static _Atomic u64 map_inserts[2], map_restores[2], map_overwrites[2], map_absent_inactive[2];
static _Atomic u64 forward_only_kind[2][256];

/* ---- the two std::unordered_map<actor*,int> objects: raw actor pointer -> saved value --------------------------------- */
typedef struct { u64 key; s32 val; } MapEnt;
typedef struct { MapEnt *e; size_t n, cap; } Map;
static Map maps[2];
static pthread_mutex_t map_mu = PTHREAD_MUTEX_INITIALIZER;
static ssize_t map_find(Map *m,u64 key){ for(size_t i=0;i<m->n;i++) if(m->e[i].key==key) return (ssize_t)i; return -1; }
static void map_put(KCtx *g,Map *m,u64 key,s32 val){
    if(m->n==m->cap){ size_t nc=m->cap?m->cap*2:64; MapEnt *ne=realloc(m->e,nc*sizeof *ne); if(!ne) kh_fatal(g,"mod params: out of memory growing the actor map"); m->e=ne; m->cap=nc; }
    m->e[m->n].key=key; m->e[m->n].val=val; m->n++;
}
static void map_erase_at(Map *m,size_t i){ m->e[i]=m->e[m->n-1]; m->n--; }
size_t kh_mod_params_map_size(int which){ pthread_mutex_lock(&map_mu); size_t n=maps[which&1].n; pthread_mutex_unlock(&map_mu); return n; }
int kh_mod_params_map_get(int which,u64 actor,s32 *saved){
    pthread_mutex_lock(&map_mu); ssize_t i=map_find(&maps[which&1],actor); if(i>=0&&saved) *saved=maps[which&1].e[i].val; pthread_mutex_unlock(&map_mu); return i>=0;
}
void kh_mod_params_map_clear(void){ pthread_mutex_lock(&map_mu); maps[0].n=maps[1].n=0; pthread_mutex_unlock(&map_mu); }

/* ---- per-call context ------------------------------------------------------------------------------------------------ */
typedef struct { KCtx *g; u64 R, C, A, site; int range; int kind; v128 entry_xmm2; } Mp;
#define NEED(m,cond,what) do{ if(!(cond)) kh_fatal((m)->g,"mod params: %s kind %d: %s (call-site RVA 0x%llx); the DLL does not check this and would fault", \
    (m)->range?"range hook 40d810":"frame hook 40dd70",(m)->kind,(what),(unsigned long long)(m)->site); }while(0)

static float mp_rf(KCtx *g,u64 a){ u32 v=rd32(g,a); float f; memcpy(&f,&v,4); return f; }
static void mp_wf(KCtx *g,u64 a,float f){ u32 v; memcpy(&v,&f,4); wr32(g,a,v); }
static void mp_setxmm_f(KCtx *g,int i,float f){ v128 o=VZ; memcpy(&o,&f,4); KH_XMM(g,i)=o; }
static float mp_f32(u32 b){ float f; memcpy(&f,&b,4); return f; }

/* Every outgoing guest call goes through here so a test can record it instead (kh_mp_mock_sink). */
static u64 mock_record(Mp *m,struct kh_mp_call *c){
    struct kh_mp_mock *k=kh_mp_mock_sink;
    if(k->ncalls>=KH_MP_MAXCALLS){ k->overflow++; return 0; }
    if(c->rva==0x3e1840 && c->a[2]){ for(int i=0;i<3;i++) c->mem[i]=rd64(m->g,c->a[2]+8*i); }
    if(c->rva==0x3db2f0 && c->a[1]){ u64 v=0; memcpy(&v,hp(m->g,c->a[1]),5); c->mem[0]=v; }
    k->c[k->ncalls]=*c; u64 r=k->fake?k->fake(k->user,&k->c[k->ncalls]):0; k->ncalls++; return r;
}
static u64 mp_call(Mp *m,u32 rva,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){
    if(kh_mp_mock_sink){ struct kh_mp_call c; memset(&c,0,sizeof c); c.rva=rva; c.a[0]=a0; c.a[1]=a1; c.a[2]=a2; c.a[3]=a3; c.a[4]=a4; return mock_record(m,&c); }
    return kh_call5(m->g,GS_BASE+rva,a0,a1,a2,a3,a4);
}
static u64 mp_fcall(Mp *m,u32 rva,u64 a0,u64 a1,float f2,float f3,u64 a4){
    if(kh_mp_mock_sink){ struct kh_mp_call c; memset(&c,0,sizeof c); c.rva=rva; c.a[0]=a0; c.a[1]=a1; c.a[4]=a4; memcpy(&c.f2,&f2,4); memcpy(&c.f3,&f3,4); return mock_record(m,&c); }
    mp_setxmm_f(m->g,2,f2); mp_setxmm_f(m->g,3,f3);
    return kh_call5(m->g,GS_BASE+rva,a0,a1,0,0,a4);
}
/* 409b00(double xmm0 [ignored by the callee], Actor *rdx, float xmm2 [NEVER SET by the DLL]): the callee reads xmm2, which still holds the
 * caller's value (the frame-window lower bound of the dispatcher loop at exe 40e960).  Reproduce by passing the hook-entry xmm2 through. */
static u64 mp_call409b00(Mp *m,u64 actor){
    double d30=30.0; u64 bits; memcpy(&bits,&d30,8);
    if(kh_mp_mock_sink){ struct kh_mp_call c; memset(&c,0,sizeof c); c.rva=0x409b00; c.a[1]=actor; c.x0=bits; memcpy(&c.f2,&m->entry_xmm2,4); return mock_record(m,&c); }
    v128 x0=VZ; memcpy(&x0,&d30,8); KH_XMM(m->g,0)=x0; KH_XMM(m->g,2)=m->entry_xmm2;
    return kh_call5(m->g,GS_BASE+0x409b00,0,actor,0,0,0);
}
static void mp_forward(Mp *m,void (*orig)(KCtx*),_Atomic u64 *counter){
    (*counter)++;
    if(kh_mp_mock_sink){ struct kh_mp_call c; memset(&c,0,sizeof c); c.rva=m->range?0x40d810:0x40dd70; c.a[0]=m->R; c.a[1]=m->C; mock_record(m,&c); return; }
    KH_RCX(m->g)=m->R; KH_RDX(m->g)=m->C;      /* the DLL passes the original rcx/rdx unchanged */
    orig(m->g);
}
static u64 mp_S(Mp *m){ return mp_call(m,0x39b760,0,0,0,0,0); }
static int mp_actor_ready(Mp *m){ return (rd32(m->g,m->A+0x6c8)>>7)&1; }
static u64 mp_mask_bit(u32 p){ return (u64)(s64)(s32)(1u<<(p&31)); }   /* movsxd of the 32-bit shl result */

/* ---- kind 64 / 65 shared lifetime ------------------------------------------------------------------------------------ */
/* Active while float(u8 R[+2] - 1) > C.f44 (comiss/jbe: equal, below and NaN all take the restore side). */
static int mp_override_active(Mp *m){
    float end=(float)((s32)rd8(m->g,m->R+2)-1), now=mp_rf(m->g,m->C+0x44);
    return end>now;
}
static void mp_override(Mp *m,int which,u32 P1){
    KCtx *g=m->g; u64 A=m->A; NEED(m,A,"actor pointer C[+0x10] is null");
    int active=mp_override_active(m);
    pthread_mutex_lock(&map_mu);
    ssize_t at=map_find(&maps[which],A);
    if(active){
        u64 sheet=rd64(g,A+0x5c0);
        if(at<0){
            pthread_mutex_unlock(&map_mu); NEED(m,sheet,"actor sheet A[+0x5c0] is null while saving the value"); pthread_mutex_lock(&map_mu);
            map_put(g,&maps[which],A,which==0?(s32)rd32(g,sheet+8):(s32)rd8(g,sheet+0x1ae));   /* kind 65 saves the byte zero-extended */
            map_inserts[which]++;
        } else map_overwrites[which]++;
        pthread_mutex_unlock(&map_mu);
        NEED(m,sheet,"actor sheet A[+0x5c0] is null while writing the override");
        if(which==0) wr32(g,sheet+8,(u32)(s32)(s16)P1); else wr8(g,sheet+0x1ae,(u8)P1);
    } else {
        if(at<0){ map_absent_inactive[which]++; pthread_mutex_unlock(&map_mu); return; }
        s32 saved=maps[which].e[at].val;
        pthread_mutex_unlock(&map_mu);
        u64 sheet=rd64(g,A+0x5c0); NEED(m,sheet,"actor sheet A[+0x5c0] is null while restoring");
        if(which==0) wr32(g,sheet+8,(u32)saved); else wr8(g,sheet+0x1ae,(u8)saved);
        pthread_mutex_lock(&map_mu); ssize_t again=map_find(&maps[which],A); if(again>=0) map_erase_at(&maps[which],(size_t)again); pthread_mutex_unlock(&map_mu);
        map_restores[which]++;
    }
}

/* ---- range hook ------------------------------------------------------------------------------------------------------- */
static void range_extension(Mp *m,s32 type){
    KCtx *g=m->g; u64 R=m->R, C=m->C, A=m->A;
    u32 P1=rd16(g,R+6), P2=rd16(g,R+8), P3=rd16(g,R+10); s32 P4=(s16)rd16(g,R+12);
    u8 idx=(u8)type; rng_seen[idx]++;
#define ACT() (rng_acted[idx]++)
    switch(type){
    case 54: NEED(m,A,"actor pointer is null"); wr64(g,A+0x120,rd64(g,A+0x120)&~0x200000000ull); ACT(); break;
    case 55: NEED(m,A,"actor pointer is null"); wr64(g,A+0x120,rd64(g,A+0x120)|0x200000000ull); ACT(); break;
    case 56: case 57: case 58: case 59: wr8(g,C+0x154,rd8(g,C+0x154)|(u8)(0x10<<(type-56))); ACT(); break;
    case 60: {
        u64 S=mp_S(m);
        if(!S || (u16)P1 || ((u16)P2 && !(rd64(g,S+8)&mp_mask_bit(P2)))) break;
        NEED(m,A,"actor pointer is null");
        if(!mp_actor_ready(m)) break;
        u64 V=rd64(g,A+0xdc8); mp_call(m,0x3bde30,V+4,0,0,0,0);
        u64 T=mp_call(m,0x3d74e0,A,(u32)(s32)(s16)P3,0,0,0);
        if(T){ mp_call(m,0x3d78d0,A,T,V+4,0,0); ACT(); }
        break; }
    case 61: {
        u64 S=mp_S(m);
        if(!S || (u16)P1 || ((s16)P2 && !(rd64(g,S+8)&mp_mask_bit(P2)))) break;
        mp_fcall(m,0x3c6dc0,C,(u32)(s32)(s16)P3,0.0f,0.0f,0);
        mp_fcall(m,0x3c8310,C,(u32)P4,0.0f,0.0f,0); ACT(); break; }
    case 62: {
        u32 P5=rd16(g,R+14); NEED(m,A,"actor pointer is null"); u64 sheet=rd64(g,A+0x5c0);
        u64 S=mp_S(m);
        NEED(m,sheet,"actor sheet A[+0x5c0] is null");
        /* the DLL builds its mask with SAR, not SHL: 1>>(P1&31) is 1 only for P1&31==0 */
        s32 M=(s32)1>>(P1&31);
        if(!(rd32(g,sheet+0x1d0)&(u32)M) || !S || (u16)P2 || ((u16)P3 && !(rd64(g,S+8)&mp_mask_bit(P3)))) break;
        mp_fcall(m,0x3c6dc0,C,(u32)P4,0.0f,0.0f,0);
        mp_fcall(m,0x3c8310,C,(u32)(s32)(s16)P5,0.0f,0.0f,0); ACT(); break; }
    case 63: ACT(); break;                                   /* explicit no-op (jump table entry == common exit) */
    case 64: mp_override(m,0,P1); ACT(); break;
    case 65: mp_override(m,1,P1); ACT(); break;
    case 66: case 67: case 68: {
        NEED(m,A,"actor pointer is null"); u64 sheet=rd64(g,A+0x5c0); NEED(m,sheet,"actor sheet A[+0x5c0] is null (callee dereferences it)");
        u32 v=(u32)(s32)(s16)P1;
        if(type==66) mp_call(m,0x3c0ad0,sheet,v,0,0,0); else if(type==67) mp_call(m,0x3c0c30,sheet,v,0,0,0); else mp_call(m,0x3c0cd0,sheet,v,0,0,0);
        ACT(); break; }
    case 69: ACT(); break;                                   /* explicit no-op */
    case 70: {
        NEED(m,A,"actor pointer is null");
        u32 id=(u32)mp_call(m,0x4ad240,A,0,0,0,0);
        u64 save=KH_RSP(g); KH_RSP(g)=(save-0x180)&~15ull;   /* scratch for the 24-byte argument array (zero-filled, not VM-tagged) */
        u64 buf=KH_RSP(g)+0x40; memset(hp(g,buf),0,24);
        wr32(g,buf,id); wr32(g,buf+8,(u32)(s32)(s16)P1); wr32(g,buf+16,(u32)(s32)(s16)P2);
        mp_call(m,0x3e1840,rd64(g,A+0x5b0),0x11,buf,3,0);
        KH_RSP(g)=save; ACT(); break; }
    case 71: case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79: ACT(); break;   /* explicit no-ops */
    case 80: {
        NEED(m,A,"actor pointer is null");
        float x=(float)(s32)(s16)P2/100.0f;
        if((u16)P1==1) mp_wf(g,A+0x674,x+mp_rf(g,A+0x674));
        else if((u16)P1==0) mp_wf(g,A+0x674,mp_rf(g,A+0x674)-x);
        ACT(); break; }
    case 81: ACT(); break;                                   /* explicit no-op */
    case 82: {
        NEED(m,A,"actor pointer is null");
        u64 S=mp_S(m);
        if(!mp_actor_ready(m)) break;
        NEED(m,S,"39b760 returned null (the DLL dereferences it unchecked)");
        u64 mask=((u16)P1==8)?0x100:0;
        if(!(rd64(g,S)&mask)) break;
        u64 V=rd64(g,A+0xdc8); mp_call(m,0x3bde30,V+4,0,0,0,0);
        u64 T=mp_call(m,0x3d74e0,A,(u32)(s32)(s16)P1,0,0,0);
        if(T){ mp_call(m,0x3d78d0,A,T,V+4,0,0); ACT(); }
        break; }
    case 83: {
        NEED(m,A,"actor pointer is null");
        u64 S=mp_S(m); NEED(m,S,"39b760 returned null (the DLL dereferences it unchecked)");
        u32 mask;
        switch((u16)P1){
        case 0:mask=1;break; case 1:mask=2;break; case 2:mask=4;break; case 4:mask=0x10;break; case 5:mask=0x20;break; case 6:mask=0x40;break;
        case 7:mask=0x80;break; case 8:mask=0x100;break; case 9:mask=0x200;break; case 10:mask=0x400;break; case 11:mask=0x800;break;
        case 12:mask=0x1000;break; case 15:mask=0x8000;break; case 16:mask=0x10000;break; case 17:mask=0x20000;break; case 18:mask=0x40000;break;
        case 19:mask=0x80000;break; case 24:mask=0x1000000;break; case 25:mask=0x2000000;break; default: mask=0; }
        mp_call(m,0x3b3e80,0,0,0,0,0);                       /* pure getter, result unused by the DLL */
        if(rd64(g,S+8)&(u64)(s64)(s32)mask){ mp_call(m,0x3cb9f0,0,A,1,0,0); ACT(); }
        break; }
    default: kh_fatal(g,"mod params: range kind %d reached the extension switch but is not in 54..83 (call-site RVA 0x%llx)",type,(unsigned long long)m->site);
    }
#undef ACT
}

void f_40d810(KCtx *g){
    Mp m; memset(&m,0,sizeof m);
    m.g=g; m.R=A0(g); m.C=A1(g); m.range=1; m.site=rd64(g,KH_RSP(g))-GS_BASE; m.entry_xmm2=KH_XMM(g,2);
    s32 type=(s8)rd8(g,m.R+4); m.kind=type;
    rng_calls++;
    if(type<RANGE_EXT_LO||type>RANGE_EXT_HI){
        /* The DLL's own range check (type-54 > 29 -> original); kinds 0..53 are vanilla, others have no handler anywhere. */
        rng_forward_only++; forward_only_kind[0][(u8)type]++;
        mp_forward(&m,orig_f_40d810,&rng_orig_calls); return;
    }
    m.A=rd64(g,m.C+0x10);
    range_extension(&m,type);
    mp_forward(&m,orig_f_40d810,&rng_orig_calls);
}

/* ---- frame hook ------------------------------------------------------------------------------------------------------ */
static void frame_extension(Mp *m,s32 op){
    KCtx *g=m->g; u64 R=m->R, C=m->C, A=m->A;
    s32 X=(s16)rd16(g,R+4), Y=(s16)rd16(g,R+6);
    u8 idx=(u8)op; frm_seen[idx]++;
#define ACT() (frm_acted[idx]++)
    switch(op){
    case 50: NEED(m,A,"actor pointer is null"); mp_call(m,0x3b4da0,A,(u32)X,0,0,0); ACT(); break;
    case 51: NEED(m,A,"actor pointer is null"); wr64(g,A+0x20,0); wr64(g,A+0x28,0); ACT(); break;
    case 52: NEED(m,A,"actor pointer is null"); wr32(g,A+0x134,0x42c60000u); ACT(); break;       /* 99.0f */
    case 53: { NEED(m,A,"actor pointer is null"); u64 T=rd64(g,A+0x148); wr32(g,A+0x134,T?rd32(g,T+0xc):0x3e860a92u); ACT(); break; }
    case 54: NEED(m,A,"actor pointer is null"); mp_wf(g,A+0x138,mp_rf(g,A+0x138)+8.0f); ACT(); break;
    case 55: NEED(m,A,"actor pointer is null"); wr32(g,A+0x138,0x41800000u); ACT(); break;       /* 16.0f */
    case 56: ACT(); break;                                                                         /* explicit no-op */
    case 57: mp_fcall(m,0x3c6dc0,C,(u32)X,(float)Y/100.0f,0.0f,0); ACT(); break;
    case 58: mp_fcall(m,0x3c8310,C,(u32)X,(float)Y/100.0f,0.0f,0); ACT(); break;
    case 59: {
        NEED(m,A,"actor pointer is null");
        static const u16 off[7]={0x128,0x12c,0x130,0x134,0x138,0x13c,0x140};
        if((u32)X<=6){ mp_wf(g,A+off[X],(float)Y/100.0f); ACT(); }
        break; }
    case 60: { NEED(m,A,"actor pointer is null"); u64 T=rd64(g,A+0x148); NEED(m,T,"A[+0x148] is null"); mp_wf(g,T+0x2c,(float)Y/100.0f); ACT(); break; }
    case 61: {
        if((u32)X<=1){ u64 obj=rd64(g,C+0xd8+8*(u64)X); NEED(m,A,"actor pointer is null"); mp_call(m,0x3d9fe0,obj+0x2380,A,(u32)Y,0,0); ACT(); }
        break; }
    case 62: {
        NEED(m,A,"actor pointer is null"); u64 q918=rd64(g,A+0x918); NEED(m,q918,"A[+0x918] is null");
        u64 T=mp_call(m,0x3eaae0,(u32)(s32)(s16)rd16(g,q918+0x4e),0,0,0,0);
        if((u32)X<=1){
            NEED(m,T,"3eaae0 returned null (the DLL dereferences it unchecked)");
            u64 obj=rd64(g,C+0xd8+8*(u64)X);
            mp_call(m,0x3d9fe0,obj+0x2380,A,(u32)(s32)(s16)rd16(g,T+4+2*(u64)X),0,0); ACT();
        }
        break; }
    case 63: case 64: {
        NEED(m,A,"actor pointer is null"); u64 sheet=rd64(g,A+0x5c0);
        if(sheet){ wr32(g,sheet+8,op==63?(u32)X:0); ACT(); }
        break; }
    case 65: {
        NEED(m,A,"actor pointer is null");
        char tmp[16]; snprintf(tmp,sizeof tmp,"%04d",(int)X);       /* DLL: snprintf(buf5,5,"%04d",X) into a zeroed 5-byte buffer */
        u8 five[5]={0}; memcpy(five,tmp,strnlen(tmp,4));
        u64 save=KH_RSP(g); KH_RSP(g)=(save-0x180)&~15ull; u64 buf=KH_RSP(g)+0x40;
        memcpy(hp(g,buf),five,5);
        mp_call(m,0x3db2f0,A,buf,A,0,0);
        KH_RSP(g)=save; ACT(); break; }
    case 66: case 67: {
        NEED(m,A,"actor pointer is null"); u64 q7b0=rd64(g,A+0x7b0); NEED(m,q7b0,"A[+0x7b0] is null (callee dereferences it)");
        mp_call(m,0x1a85e0,q7b0,(u32)X,0,0,0); ACT(); break; }
    case 68: case 69: ACT(); break;                                                               /* explicit no-ops */
    case 70: NEED(m,A,"actor pointer is null"); mp_call409b00(m,A); ACT(); break;
    case 71: {
        NEED(m,A,"actor pointer is null"); u64 sheet=rd64(g,A+0x5c0); NEED(m,sheet,"actor sheet A[+0x5c0] is null");
        if(!(mp_rf(g,sheet+0x1c0)>0.0f)){ mp_call(m,0x3c0cd0,sheet,(u32)(-X),0,0,0); ACT(); }
        break; }
    case 72: ACT(); break;                                                                        /* explicit no-op */
    case 73: {
        NEED(m,A,"actor pointer is null"); u64 sheet=rd64(g,A+0x5c0); NEED(m,sheet,"actor sheet A[+0x5c0] is null");
        if(!(mp_rf(g,sheet+0x1c0)>0.0f)){
            u64 T=mp_call(m,0x3b68b0,(u32)X,0,0,0,0); NEED(m,T,"3b68b0 returned null (the DLL dereferences it unchecked)");
            mp_call(m,0x3c0cd0,sheet,(u32)(-(s32)(s8)rd8(g,T+0x1c)),0,0,0); ACT();
        }
        break; }
    case 74: case 75: case 76: case 77: case 78: case 79: ACT(); break;                           /* explicit no-ops */
    case 80: {
        NEED(m,A,"actor pointer is null");
        if(rd8(g,A+0x6c8)&0x80){
            u64 V=rd64(g,A+0xdc8); mp_call(m,0x3bde30,V+4,0,0,0,0);
            u64 T=mp_call(m,0x3d74e0,A,(u32)X,0,0,0);
            if(T){ mp_call(m,0x3d78d0,A,T,V+4,0,0); ACT(); }
        }
        break; }
    default: kh_fatal(g,"mod params: frame op %d reached the extension switch but is not in 50..80 (call-site RVA 0x%llx)",op,(unsigned long long)m->site);
    }
#undef ACT
}

void f_40dd70(KCtx *g){
    Mp m; memset(&m,0,sizeof m);
    m.g=g; m.R=A0(g); m.C=A1(g); m.range=0; m.site=rd64(g,KH_RSP(g))-GS_BASE; m.entry_xmm2=KH_XMM(g,2);
    s32 op=(s8)rd8(g,m.R+2); m.kind=op;
    frm_calls++;
    if(op<FRAME_EXT_LO||op>FRAME_EXT_HI){
        frm_forward_only++; forward_only_kind[1][(u8)op]++;
        mp_forward(&m,orig_f_40dd70,&frm_orig_calls); return;
    }
    m.A=rd64(g,m.C+0x10);
    frame_extension(&m,op);
    mp_forward(&m,orig_f_40dd70,&frm_orig_calls);
}

void kh_mod_params_dump(void){
    fprintf(stderr,"kh: mod params range hook 40d810 calls=%llu forward-only(kind outside 54..83)=%llu original-calls=%llu\n",
        (unsigned long long)rng_calls,(unsigned long long)rng_forward_only,(unsigned long long)rng_orig_calls);
    for(int k=0;k<256;k++) if(rng_seen[k]) fprintf(stderr,"kh: mod params range kind=%d seen=%llu acted=%llu\n",(int)(s8)k,(unsigned long long)rng_seen[k],(unsigned long long)rng_acted[k]);
    for(int k=0;k<256;k++) if(forward_only_kind[0][k]) fprintf(stderr,"kh: mod params range forward-only kind=%d calls=%llu\n",(int)(s8)k,(unsigned long long)forward_only_kind[0][k]);
    fprintf(stderr,"kh: mod params frame hook 40dd70 calls=%llu forward-only(op outside 50..80)=%llu original-calls=%llu\n",
        (unsigned long long)frm_calls,(unsigned long long)frm_forward_only,(unsigned long long)frm_orig_calls);
    for(int k=0;k<256;k++) if(frm_seen[k]) fprintf(stderr,"kh: mod params frame op=%d seen=%llu acted=%llu\n",(int)(s8)k,(unsigned long long)frm_seen[k],(unsigned long long)frm_acted[k]);
    for(int k=0;k<256;k++) if(forward_only_kind[1][k]) fprintf(stderr,"kh: mod params frame forward-only op=%d calls=%llu\n",(int)(s8)k,(unsigned long long)forward_only_kind[1][k]);
    for(int w=0;w<2;w++) fprintf(stderr,"kh: mod params map%d(%s) size=%zu inserts=%llu overwrites=%llu restores=%llu inactive-absent=%llu\n",w,w?"resist byte 65":"min HP 64",
        kh_mod_params_map_size(w),(unsigned long long)map_inserts[w],(unsigned long long)map_overwrites[w],(unsigned long long)map_restores[w],(unsigned long long)map_absent_inactive[w]);
}

#include "rewind.h"
void kh_rewind_maps(KhRecords *r){
    for(unsigned i=0;i<2;i++){
        Map *m=&maps[i]; KH_RECORD(r,m->n);
        if(m->n>m->cap){
            MapEnt *p=realloc(m->e,m->n*sizeof *p); if(!p) kh_fatal(NULL,"savestate-actor-map-memory");
            m->e=p; m->cap=m->n;
        }
        kh_record(r,m->e,m->n*sizeof *m->e);
    }
}
