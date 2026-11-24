/* lane R8-SCALE: a per-look RENDER scale of the player (owner 2026-11-22: "axel looks kinda small").
 *
 * What the engine does (evidence: kh2fm.exe; run-R8/scale/implementation notes-SCALE.md):
 *   - YS::OBJ world matrix: f_3ccb90 builds actor+0x40 (4x4, row vectors; row 3 = actor+0x70 = the position) from the position
 *     actor+0x670, the rotation actor+0x680 and the object scale actor+0x30.xyz * (actor+0x3c * actor+0xaac) (f_1aa7d0 at
 *     0x1403ccd8c), and at its end the draw copy actor+0x808 (0x1403cd18f).  Game logic (the weapon attach, attack and body
 *     collision, effects) works from actor+0x40 and the instance's MODEL-space bone matrices ([actor+0x7b0]+0x50: 229 for P_EX100).
 *     Writing actor+0x30 would scale all of that, i.e. change NMC gameplay.
 *   - Drawing: every draw of a model instance enters one of three functions with (rcx = instance, rdx = view-projection,
 *     r8 = world matrix, r9 = the draw entry + 8, [rsp+0x28] = level of detail): f_1a5180 (the map pass f_188120, whose entry for an
 *     actor is actor+0x7b0, so r8 = actor+0x808; f_170450 / f_189090 through f_1a5490), f_1a5580 (f_18f660 / f_18f7f0) and
 *     f_1af000 (dk::ModelSprite f_18eee0, dk::SpotBlend f_18f240, f_1a5500 ...).  Each copies the world matrix into instance+0x2b8
 *     (rows 1 and 2 negated) and derives instance+0x274 (world x view) and instance+0x70 (x projection); only these three functions
 *     read instance+0x2b8.
 * The hook: for the active player's body instance ([0x142a105d0]+0x7b0) and its two weapon objects' instances ([player+0xd60] /
 * [player+0xd68], +0x7b0), while the player wears a look whose scale s != 1, the world matrix handed to the draw is replaced by a
 * copy scaled by s about the player's feet p:  rows 0..2 * s,  row 3 = p + k (row3 - p)  (k = s; a weapon farther than 200 units
 * from p, i.e. away from the hand, eases to k = 1 at 300 so a thrown weapon is not pushed outward).  p = the body's own row 3 (the
 * model origin is the feet: model y = 0); for a weapon p = actor+0x70, the matrix its hand attachment follows.  The body and its
 * weapons are drawn s times larger about the ground point and NO guest state changes: the actor, its bone matrices and every
 * collision stay exactly PC NMC.  The copy lives in host memory owned by this file (given to the draw by its guest address, as
 * runtime/null_audio.c GADDR); the draw reads it during the call only.  With s == 1 (every look but Axel, or KHNMC_LOOK_SCALE=1)
 * each hook is one comparison and the lifted body.
 *   KHNMC_LOOK_SCALE=<s>        Axel's scale, 0.5 .. 2.0 (default LS_AXEL_DEFAULT; 1 = off); anything else: named fatal look-scale-value.
 *   KHNMC_LOOK_SCALE_WEAPON=0   the weapons move with the hand but keep their size (default 1: they scale with him).
 *   KHNMC_LOOK_SCALE_LOG=<n>    test readback every n-th body draw (stderr `kh: look-scale:`) with a digest of the gameplay state.
 */
#include "khnmc_rt.h"
#include "kh_look.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 1 / 0.746: the Axel mesh was shrunk by 0.746 to sit on Roxas's joints (implementation notes section 3); this restores the KH3 export's
 * own size = KH2 Axel's (B_EX110: head joint 189.3 = 141.2 x 1.341, hair top 165.8 x 1.34 = 222.2 vs 221.0, hips 102.6 x 1.34 = 137.5
 * vs 133.2; run-MV/bonemap-check.txt) */
#define LS_AXEL_DEFAULT 1.34f
#define LS_ACTIVE (GS_BASE+0x2a105d0ull)

static int ls_ready; static float ls_axel=LS_AXEL_DEFAULT; static int ls_weapon=1; static long ls_log;
static void ls_init(void){
    const char *e=getenv("KHNMC_LOOK_SCALE");
    if(e && *e){
        char *end; float v=strtof(e,&end);
        if(*end || !(v>=0.5f && v<=2.0f)) kh_fatal(NULL,"look-scale-value: KHNMC_LOOK_SCALE=%s (expected 0.5 .. 2.0)",e);
        ls_axel=v;
    }
    const char *w=getenv("KHNMC_LOOK_SCALE_WEAPON");
    if(w && *w){
        if(strcmp(w,"0") && strcmp(w,"1")) kh_fatal(NULL,"look-scale-weapon-value: KHNMC_LOOK_SCALE_WEAPON=%s (expected 0 or 1)",w);
        ls_weapon=*w=='1';
    }
    const char *l=getenv("KHNMC_LOOK_SCALE_LOG");
    ls_log=l?atol(l):0;
    ls_ready=1;
}
/* the render scale of a look; 1 = drawn exactly as the exe draws it */
static float ls_scale(int look){
    if(!ls_ready) ls_init();
    return look==KH_LOOK_AXEL?ls_axel:1.0f;
}

static int ls_ok(u64 a,u64 n){ return a && GA_OK(a) && GA_OK(a+n-1); }
static float ls_f(KCtx *g,u64 a){ u32 v=rd32(g,a); float f; memcpy(&f,&v,4); return f; }
static _Thread_local float ls_copy[16] __attribute__((aligned(16)));
#define LS_GADDR(p) ((u64)((uintptr_t)(p)-gs_bias))

/* 0 = not the player's, 1 = the body, 2 = a weapon */
static int ls_part(KCtx *g,u64 inst,u64 *player){
    u64 pl=rd64(g,LS_ACTIVE);
    if(!ls_ok(pl,0xd70)) return 0;
    *player=pl;
    if(rd64(g,pl+0x7b0)==inst) return 1;
    for(int j=0;j<2;j++){ u64 w=rd64(g,pl+0xd60+8ull*(u64)j); if(ls_ok(w,0x7b8) && rd64(g,w+0x7b0)==inst) return 2; }
    return 0;
}

/* ---- test readback (KHNMC_LOOK_SCALE_LOG) ---- */
static _Atomic unsigned long long ls_body, ls_weap, ls_scaled;
static unsigned long long ls_whist[16]; static float ls_wmax, ls_top=-99, ls_top1=-99;
static u64 ls_digest(KCtx *g,u64 pl){                        /* FNV-1a over the gameplay state the scale must not touch */
    u64 h=1469598103934665603ull;
    #define LS_MIX(a,n) do{ for(u64 i_=0;i_<(n);i_++){ h^=rd8(g,(a)+i_); h*=1099511628211ull; } }while(0)
    LS_MIX(pl+0x30,0x50); LS_MIX(pl+0x670,0x20);
    u64 inst=rd64(g,pl+0x7b0);
    if(ls_ok(inst,0x60)){ u64 b=rd64(g,inst+0x50); if(ls_ok(b,64*229)) LS_MIX(b,64*229); }
    for(int j=0;j<2;j++){ u64 w=rd64(g,pl+0xd60+8ull*(u64)j); if(ls_ok(w,0x680)){ LS_MIX(w+0x30,0x50); LS_MIX(w+0x670,0x10); } }
    u64 sheet=rd64(g,pl+0x5c0); if(ls_ok(sheet,0x190)){ LS_MIX(sheet,8); LS_MIX(sheet+0x180,8); }
    #undef LS_MIX
    return h;
}
static void ls_readback(KCtx *g,u64 inst,u64 pl,int look,float s,const float *m,int part,unsigned long long n){
    float px=m[12],py=m[13],pz=m[14];
    if(part==2){
        px=ls_f(g,pl+0x70); py=ls_f(g,pl+0x74); pz=ls_f(g,pl+0x78);
        float dx=m[12]-px,dy=m[13]-py,dz=m[14]-pz,dd=sqrtf(dx*dx+dy*dy+dz*dz);
        if(dd>ls_wmax) ls_wmax=dd;
        int k=(int)(dd/25.f); ls_whist[k>15?15:k]++;
        return;
    }
    /* what the draw used: instance+0x2b8 (world, rows 1 and 2 negated) and +0x70 (world x view x GS projection) */
    float d[16],P[16]; for(int i=0;i<16;i++){ d[i]=ls_f(g,inst+0x2b8+4ull*(u64)i); P[i]=ls_f(g,inst+0x70+4ull*(u64)i); }
    float sc=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    /* framing: the hair top (feet + 165.8 x s up; world up = -y) on the 512 x 416 GS frame about 2048 (aspect.c): 1 = top edge */
    for(int t=0;t<2;t++){
        float w[3]={px,py-165.8f*(t?1.f:s),pz},q[3];
        for(int r=0;r<3;r++) q[r]=((w[0]-d[12])*d[4*r]+(w[1]-d[13])*d[4*r+1]+(w[2]-d[14])*d[4*r+2])/(sc*sc);
        float cy=q[0]*P[1]+q[1]*P[5]+q[2]*P[9]+P[13], cw=q[0]*P[3]+q[1]*P[7]+q[2]*P[11]+P[15];
        if(cw>0){ float up=(2048.f-cy/cw)/208.f; if(t==0 && up>ls_top) ls_top=up; if(t==1 && up>ls_top1) ls_top1=up; }
    }
    if(n%(unsigned long long)ls_log!=1) return;
    float dsx=sc, dsy=sqrtf(d[4]*d[4]+d[5]*d[5]+d[6]*d[6]), dsz=sqrtf(d[8]*d[8]+d[9]*d[9]+d[10]*d[10]);
    float gsx=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
    /* the head (bone 140 of the 229-bone rig): its distance from the feet as the game places it and as drawn */
    float hg=0,hd=0; u64 b=rd64(g,inst+0x50);
    if(ls_ok(b,64*229)){
        u64 hb=b+64ull*140; float h[3]={ls_f(g,hb+48),ls_f(g,hb+52),ls_f(g,hb+56)}, sg[3]={1,-1,-1}, eg=0, ed=0;
        for(int c=0;c<3;c++){
            float wg=m[12+c], wd=d[12+c], pc=c==0?px:c==1?py:pz;
            for(int r=0;r<3;r++){ wg+=h[r]*sg[r]*m[4*r+c]; wd+=h[r]*d[4*r+c]; }
            eg+=(wg-pc)*(wg-pc); ed+=(wd-pc)*(wd-pc);
        }
        hg=sqrtf(eg); hd=sqrtf(ed);
    }
    char hist[200]; int hl=0; for(int k=0;k<16;k++) hl+=snprintf(hist+hl,sizeof hist-(size_t)hl,"%s%llu",k?",":"",ls_whist[k]);
    fprintf(stderr,"kh: look-scale: look %d s %.3f body draw %llu: game |x| %.4f | drawn |x| %.4f |y| %.4f |z| %.4f pivot %.3f %.3f %.3f "
            "drawn row3 %.3f %.3f %.3f | head from the feet game %.2f drawn %.2f ratio %.4f | hair top highest on screen %.3f (unscaled %.3f) | "
            "draws scaled %llu (weapon %llu) | weapon from the pivot max %.1f, 25-unit buckets %s | [0x142a11100]==player %d | digest %016llx\n",
            look,s,n,gsx,dsx,dsy,dsz,px,py,pz,d[12],d[13],d[14],hg,hd,hg!=0?hd/hg:0,ls_top,ls_top1,ls_scaled,ls_weap,ls_wmax,hist,
            rd64(g,GS_BASE+0x2a11100ull)==pl,(unsigned long long)ls_digest(g,pl));
}

static void ls_draw(KCtx *g,void (*orig)(KCtx *)){
    u64 inst=A0(g),pl=0,src=A2(g);
    int look=kh_look_enabled();
    float s=ls_scale(look);
    if(s==1.0f && !ls_log){ orig(g); return; }
    int part=ls_part(g,inst,&pl);
    if(!part || !ls_ok(src,64)){ orig(g); return; }
    float m[16]; for(int i=0;i<16;i++) m[i]=ls_f(g,src+4ull*(u64)i);
    unsigned long long n=part==1?++ls_body:++ls_weap;
    if(s!=1.0f){
        float px,py,pz,k=s,rs=(part==2 && !ls_weapon)?1.0f:s;
        if(part==1){ px=m[12]; py=m[13]; pz=m[14]; }
        else {
            px=ls_f(g,pl+0x70); py=ls_f(g,pl+0x74); pz=ls_f(g,pl+0x78);
            float dx=m[12]-px,dy=m[13]-py,dz=m[14]-pz,dd=sqrtf(dx*dx+dy*dy+dz*dz);
            if(dd>=300.f) k=1.f; else if(dd>200.f) k=s+(1.f-s)*(dd-200.f)/100.f;
        }
        float *o=ls_copy;
        for(int r=0;r<3;r++){ for(int c=0;c<3;c++) o[4*r+c]=m[4*r+c]*rs; o[4*r+3]=m[4*r+3]; }
        o[12]=px+k*(m[12]-px); o[13]=py+k*(m[13]-py); o[14]=pz+k*(m[14]-pz); o[15]=m[15];
        KH_R8(g)=LS_GADDR(o);
        ls_scaled++;
    }
    orig(g);
    if(ls_log>0) ls_readback(g,inst,pl,look,s,m,part,n);
}
void orig_f_1a5180(KCtx *); void f_1a5180(KCtx *g){ ls_draw(g,orig_f_1a5180); }
void orig_f_1a5580(KCtx *); void f_1a5580(KCtx *g){ ls_draw(g,orig_f_1a5580); }
void orig_f_1af000(KCtx *); void f_1af000(KCtx *g){ ls_draw(g,orig_f_1af000); }
