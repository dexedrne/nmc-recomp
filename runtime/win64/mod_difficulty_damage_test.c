/* DLL 32d30 formulas checked through the alias hook and real lifted helpers. */
#include "khnmc_rt.h"
#include <limits.h>
#include <math.h>
void orig_f_3c12f0(KCtx *);
static int checks,failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"damage selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)
static void wf(KCtx *g,u64 p,float f){ u32 n; memcpy(&n,&f,4); wr32(g,p,n); }
static u32 idof(KCtx *g,u64 p){ return kh_call(g,GS_BASE+0x4ad240,p,0,0,0); }
/* Compute in 64 bits and explicitly narrow at x86's arithmetic boundaries. */
static s32 wrapped(s64 n){ return (s32)(u32)n; }
int kh_mod_damage_selftest(KCtx *g){
    u32 olddiff=rd32(g,GS_BASE+0x9abd48),oldflags=rd32(g,GS_BASE+0x2a11400);
    u64 oldactors=rd64(g,GS_BASE+0x2a171c8),oldbar=rd64(g,GS_BASE+0x2ae5768),sp=KH_RSP(g);
    u8 codec[512]; memcpy(codec,hp(g,GS_BASE+0x2b0d720),512);
    u32 guard=rd32(g,GS_BASE+0x2b0d920);
    memset(hp(g,GS_BASE+0x2b0d720),0xff,512); wr32(g,GS_BASE+0x2b0d920,0x80000000);
    u64 actor=ga_calloc(1,0xe00),target=ga_calloc(1,0xe00),sheet=ga_calloc(1,0x300),victim=ga_calloc(1,0x300);
    u64 packet=ga_calloc(1,0x20),attack=ga_calloc(1,0x40);
    u32 actorid=idof(g,actor),targetid=idof(g,target);
    wr64(g,GS_BASE+0x2a171c8,target); wr32(g,target+0xa90,actorid);
    wr64(g,actor+0x5c0,sheet); wr64(g,target+0x5c0,victim);
    wr32(g,sheet+0x268,actorid); wr32(g,victim+0x268,targetid); wr32(g,packet+0x10,actorid); wr32(g,packet+0xc,actorid);
    wr32(g,GS_BASE+0x2a11400,0); /* Disable the real lose-count adjustment. */
    wr32(g,victim,1000); wr32(g,victim+4,1000);
    wr32(g,sheet+0x198,INT32_MAX); wr32(g,sheet+0x19c,INT32_MIN);
    wr32(g,victim+0x1a0,INT32_MAX); wr32(g,victim+0x1a4,INT32_MIN);
    memset(hp(g,victim+0x1a8),100,7); wr16(g,attack+6,100);
    T((kh_call(g,GS_BASE+0x3ba720,actor,0,0,0)&255)==1);
    T((kh_call(g,GS_BASE+0x3ba720,target,0,0,0)&255)==1);
    const s32 values[]={0,1,2,3,99,100,101,1000,2146,2147,32767,-1,-32768};
    const s32 percentages[4][3]={{50,150,100},{100,100,100},{200,125,100},{200,125,100}};
    for(int d=0;d<4;d++) for(int direction=0;direction<3;direction++){
        wr32(g,GS_BASE+0x9abd48,d);
        wr32(g,target+0x6c8,direction==0?4:0); wr32(g,actor+0x6c8,direction==1?4:0);
        for(unsigned i=0;i<sizeof values/sizeof *values;i++){
            wr16(g,sheet+0x188,values[i]);
            s32 first=wrapped((s64)values[i]*1000000+999999)/1000000;
            s32 expected=first;
            if(direction<2) expected=wrapped((s64)first*percentages[d][direction]+99)/100;
            s32 result=kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0);
            if(result!=expected) fprintf(stderr,"damage case d=%d dir=%d value=%d expected=%d actual=%d\n",d,direction,values[i],expected,result);
            T(result==expected); T(KH_RSP(g)==sp);
        }
        /* Signed bonus plus +99 BEFORE multiply is a shipped element quirk. */
        wr16(g,sheet+0x188,101); wr8(g,attack+9,1);
        const s16 bonuses[]={0,1,50,100,-100,32767,-32768};
        for(unsigned i=0;i<sizeof bonuses/sizeof *bonuses;i++){
            wr16(g,sheet+0x218,bonuses[i]);
            s32 first=101, expected;
            if(direction<2) first=(first*percentages[d][direction]+99)/100;
            expected=wrapped((s64)(bonuses[i]+99)*first)/100;
            T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==expected);
        }
        wr8(g,attack+9,0);
    }
    /* Signed defence and bonus stats differ from vanilla's unsigned reads. */
    wr32(g,GS_BASE+0x9abd48,1); wr32(g,target+0x6c8,0); wr32(g,actor+0x6c8,0);
    wr16(g,sheet+0x188,1); wr16(g,victim+0x18c,0xffff);
    T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==2);
    wr16(g,victim+0x18c,0); wr16(g,sheet+0x188,100); wf(g,sheet+0x1c0,1.0f); wr16(g,sheet+0x226,0xffff);
    T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==99);
    wf(g,sheet+0x1c0,0); wr8(g,attack+5,2); wr16(g,sheet+0x18c,123);
    T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==123);
    wr16(g,sheet+0x18c,0);
    /* Unknown stat kinds use the incoming caller home dword at entry+10. */
    wr8(g,attack+5,3);
    u64 call_sp=((sp-0x100)&~15ull)-8;
    wr32(g,call_sp+0x10,47);
    T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==47);
    /* Magic type 5/6 return before difficulty/element/low-HP scaling. */
    wr8(g,attack+5,1); wr16(g,sheet+0x18a,101); wr32(g,victim+16,321);
    const s16 powers[]={0,1,3,100,32767,-1,-32768};
    for(int d=0;d<4;d++) for(int type=5;type<=6;type++) for(unsigned i=0;i<sizeof powers/sizeof *powers;i++){
        wr32(g,GS_BASE+0x9abd48,d); wr8(g,attack+4,type); wr16(g,attack+6,powers[i]);
        s32 expected=type==5?wrapped((s64)powers[i]*321*101+9999)/10000:wrapped((s64)powers[i]*101+99)/100;
        T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,1)==expected);
    }
    /* The DLL reads sstm+180 only after float multiply / CVTTSS2SI comparison. */
    u64 bar=ga_calloc(1,0x20),sstm=ga_calloc(1,0x200);
    wr32(g,bar+4,1); wr16(g,bar+0x10,2); wr32(g,bar+0x14,0x6d747373); wr32(g,bar+0x18,idof(g,sstm));
    wr64(g,GS_BASE+0x2ae5768,bar);
    const struct { s32 current,max; float threshold; int applies; } low_hp[]={
        {0,100,0,1}, {1,100,0,0}, {49,100,0.5f,1}, {50,100,0.5f,1}, {51,100,0.5f,0},
        {100,100,1,1}, {INT32_MIN,100,NAN,1}, {0,100,NAN,0},
        {INT32_MIN,100,INFINITY,1}, {INT32_MAX,INT32_MAX,1,0}, {-50,-100,0.5f,1}, {-49,-100,0.5f,0}
    };
    const s32 low_multipliers[]={0,1,125,-1,INT32_MAX};
    wr8(g,attack+5,0); wr8(g,attack+4,0); wr16(g,attack+6,100); wr16(g,sheet+0x188,101);
    wr32(g,target+0x6c8,4); wr32(g,actor+0x6c8,0);
    for(int d=0;d<4;d++) for(unsigned i=0;i<sizeof low_hp/sizeof *low_hp;i++) for(unsigned j=0;j<sizeof low_multipliers/sizeof *low_multipliers;j++){
        wr32(g,GS_BASE+0x9abd48,d); wr32(g,victim,low_hp[i].current); wr32(g,victim+4,low_hp[i].max);
        wf(g,victim+0x248,low_hp[i].threshold); wr32(g,sstm+0x180,low_multipliers[j]);
        s32 expected=(101*percentages[d][0]+99)/100;
        if(low_hp[i].applies) expected=wrapped((s64)expected*low_multipliers[j]+99)/100;
        T((s32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==expected); T(KH_RSP(g)==sp);
    }
    wr32(g,victim,1000); wr32(g,victim+4,1000); wf(g,victim+0x248,0);
    /* Invalid indices retain the complete vanilla function. */
    wr8(g,attack+5,0); wr8(g,attack+4,0); wr16(g,attack+6,100); wr16(g,sheet+0x188,101);
    u64 original=kh_host_fn("difficulty-test-damage-original",orig_f_3c12f0);
    const s32 invalid[]={-1,4,INT32_MIN,INT32_MAX};
    for(int direction=0;direction<3;direction++) for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
        wr32(g,target+0x6c8,direction==0?4:0); wr32(g,actor+0x6c8,direction==1?4:0);
        wr32(g,GS_BASE+0x9abd48,invalid[i]);
        u32 expected=kh_call(g,original,victim,packet,attack,0);
        T((u32)kh_call(g,GS_BASE+0x3c12f0,victim,packet,attack,0)==expected);
        T(KH_RSP(g)==sp);
    }
    wr32(g,GS_BASE+0x9abd48,olddiff); wr32(g,GS_BASE+0x2a11400,oldflags); wr64(g,GS_BASE+0x2a171c8,oldactors); wr64(g,GS_BASE+0x2ae5768,oldbar);
    memcpy(hp(g,GS_BASE+0x2b0d720),codec,512); wr32(g,GS_BASE+0x2b0d920,guard);
    ga_free(actor); ga_free(target); ga_free(sheet); ga_free(victim); ga_free(packet); ga_free(attack); ga_free(bar); ga_free(sstm);
    fprintf(stderr,"kh: difficulty damage selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
