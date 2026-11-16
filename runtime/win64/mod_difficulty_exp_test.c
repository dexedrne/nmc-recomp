/* DLL 115f0 script AddExp path, with actual lifted gate/position/table/award helpers. */
#include "khnmc_rt.h"
#include <math.h>
static void exp_wf(KCtx *g,u64 p,float f){ u32 bits; memcpy(&bits,&f,4); wr32(g,p,bits); }
static s32 wrapmul(s32 a,s32 b){ return (s32)((u32)a*(u32)b); }
int kh_mod_exp_selftest(KCtx *g){
    int checks=0,failures=0;
#define ET(c) do{ checks++; if(!(c)){ failures++; fprintf(stderr,"AddExp selftest FAIL line %d: %s\n",__LINE__,#c); }}while(0)
    const u32 globals[]={0x2a0ff68,0x2a105d0,0x2a171c8,0x2ae5980,0x2ae5768,0x2a25030,0x2a25300};
    u64 saved[7]; for(int i=0;i<7;i++) saved[i]=rd64(g,GS_BASE+globals[i]);
    u32 olddiff=rd32(g,GS_BASE+0x9abd48),oldexp=rd32(g,GS_BASE+0x9acf90);
    u8 oldworld=rd8(g,GS_BASE+0x717008),parties[0x1040],codec[512];
    memcpy(parties,hp(g,GS_BASE+0x9abda0),sizeof parties);
    memcpy(codec,hp(g,GS_BASE+0x2b0d720),512); u32 guard=rd32(g,GS_BASE+0x2b0d920);
    memset(hp(g,GS_BASE+0x2b0d720),0xff,512); wr32(g,GS_BASE+0x2b0d920,0x80000000);
    u64 player=ga_calloc(1,0xe00),source=ga_calloc(1,0xe00),sheet=ga_calloc(1,0x278),params=ga_calloc(1,12);
    u64 objects=ga_calloc(1,0x68),bar=ga_calloc(1,0x20),sstm=ga_calloc(1,0x200),args=ga_calloc(2,8);
    u32 sourceid=kh_call(g,GS_BASE+0x4ad240,source,0,0,0);
    u32 entryid=kh_call(g,GS_BASE+0x4ad240,objects+8,0,0,0);
    u32 sstmid=kh_call(g,GS_BASE+0x4ad240,sstm,0,0,0);
    wr32(g,objects+4,1); wr32(g,objects+8,1); wr16(g,objects+8+0x4c,1);
    wr64(g,GS_BASE+0x2a25030,objects); wr64(g,GS_BASE+0x2a25300,1);
    wr32(g,player+8,entryid); wr64(g,player+0x5c0,sheet);
    wr64(g,GS_BASE+0x2a105d0,player); wr64(g,GS_BASE+0x2a171c8,player);
    wr64(g,GS_BASE+0x2a0ff68,0); wr64(g,GS_BASE+0x2ae5980,params); wr8(g,GS_BASE+0x717008,0);
    wr32(g,bar+4,1); wr16(g,bar+0x10,2); wr32(g,bar+0x14,0x6d747373); wr32(g,bar+0x18,sstmid);
    wr64(g,GS_BASE+0x2ae5768,bar);
    for(int ch=1;ch<=13;ch++){
        u64 p=kh_call(g,GS_BASE+0x3a14e0,GS_BASE+0x9abda0,ch,0,0); wr8(g,p+15,99);
    }
    wr32(g,args,sourceid); wr32(g,args+4,0xfeedbeef); wr32(g,args+12,0x12345678);
    u64 target=rd64(g,rd64(g,GS_BASE+0x7534a8)+16);
    ET((kh_call(g,GS_BASE+0x3ed110,0,0,0,0)&255)==1);
    const s32 inputs[]={0,1,2,3,4,5,7,100,32767,INT32_MAX,INT32_MIN,-1,-2,-100};
    const s16 scales[]={100,101,-100,32767};
    const s8 multipliers[]={1,3,-1,127};
    for(int d=0;d<4;d++) for(int low=0;low<2;low++) for(unsigned m=0;m<4;m++) for(unsigned sc=0;sc<4;sc++) for(unsigned i=0;i<sizeof inputs/sizeof *inputs;i++){
        wr32(g,GS_BASE+0x9abd48,d); wr16(g,params+10,scales[sc]);
        wr32(g,sheet,low?1:100); wr32(g,sheet+4,100); wr8(g,sheet+0x203,multipliers[m]); exp_wf(g,sstm+0x14c,0.5f);
        s32 expected=wrapmul(wrapmul(scales[sc],inputs[i]),100)/10000;
        if(low) expected=wrapmul(expected,multipliers[m]);
        if(d==3){ expected=wrapmul(expected,75)/100; if(!expected) expected=1; }
        if(expected>9999999) expected=9999999;
        wr32(g,GS_BASE+0x9acf90,0); wr32(g,args+8,inputs[i]); u64 sp=KH_RSP(g);
        kh_call(g,target,args,0,0,0);
        ET((s32)rd32(g,GS_BASE+0x9acf90)==expected); ET(KH_RSP(g)==sp);
        ET(rd32(g,args)==sourceid && rd32(g,args+4)==0xfeedbeef && rd32(g,args+12)==0x12345678);
    }
    /* COMISS/JB treats unordered HP ratio as skip-multiplier, including 0/0. */
    wr32(g,GS_BASE+0x9abd48,3); wr16(g,params+10,100); wr32(g,args+8,4);
    wr32(g,sheet,0); wr32(g,sheet+4,0); wr8(g,sheet+0x203,127); wr32(g,GS_BASE+0x9acf90,0);
    kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==3);
    /* The script syscall has no original detour; invalid modes retain its vanilla prefix, unscaled. */
    const s32 invalid[]={-1,4,INT32_MAX};
    wr32(g,sheet,100); wr32(g,sheet+4,100);
    for(unsigned i=0;i<3;i++){
        wr32(g,GS_BASE+0x9abd48,invalid[i]); wr32(g,args+8,4); wr32(g,GS_BASE+0x9acf90,0);
        kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==4);
        wr32(g,args+8,0); wr32(g,GS_BASE+0x9acf90,0); kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==0);
    }
    /* Equality takes the low-HP branch; pre-existing EXP saturates only at the save cap. */
    wr32(g,GS_BASE+0x9abd48,3); wr32(g,sheet,50); wr8(g,sheet+0x203,3);
    wr32(g,args+8,4); wr32(g,GS_BASE+0x9acf90,0); kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==9);
    wr32(g,GS_BASE+0x9acf90,9999998); kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==9999999);
    /* Disabled award gate does not read uninitialized params/sstm or change EXP. */
    wr64(g,GS_BASE+0x2a171c8,0); wr64(g,GS_BASE+0x2ae5980,0); wr64(g,GS_BASE+0x2ae5768,0);
    wr32(g,GS_BASE+0x9acf90,77); kh_call(g,target,args,0,0,0); ET(rd32(g,GS_BASE+0x9acf90)==77);
    for(int i=0;i<7;i++) wr64(g,GS_BASE+globals[i],saved[i]);
    wr32(g,GS_BASE+0x9abd48,olddiff); wr32(g,GS_BASE+0x9acf90,oldexp); wr8(g,GS_BASE+0x717008,oldworld);
    memcpy(hp(g,GS_BASE+0x9abda0),parties,sizeof parties); memcpy(hp(g,GS_BASE+0x2b0d720),codec,512); wr32(g,GS_BASE+0x2b0d920,guard);
    const u64 allocs[]={player,source,sheet,params,objects,bar,sstm,args}; for(unsigned i=0;i<8;i++) ga_free(allocs[i]);
    fprintf(stderr,"kh: AddExp selftest: %d checks, %d failures\n",checks,failures);
#undef ET
    return failures;
}
