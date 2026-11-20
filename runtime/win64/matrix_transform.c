#include "khnmc_rt.h"

/* f_13f900 transforms one 4-vector. The guest computes the four output
 * components independently, in the same multiply/add order. */
void orig_f_13f900(KCtx *);
static int overlaps_spills(u64 addr,unsigned size,u64 sp){
    return addr<sp && addr+size>sp-0x78;
}
void f_13f900(KCtx *g){
    u64 sp=KH_RSP(g),matrix=A0(g),out=A1(g),vec=A2(g);
    /* MOVAPS faults on a misaligned stack. Use the lift for arguments that
     * overlap its spill slots and could observe interleaved saves/restores. */
    if((sp&15)!=8 || overlaps_spills(matrix,64,sp) ||
       overlaps_spills(vec,16,sp) || overlaps_spills(out,16,sp)){
        orig_f_13f900(g); return;
    }
    float y=LD(float,vec+4), x=LD(float,vec);
    ST(v128,sp-0x18,KH_XMM(g,6));
    v4f b=LD(v4f,matrix+16);
    ST(v128,sp-0x28,KH_XMM(g,7));
    float z=LD(float,vec+8);
    ST(v128,sp-0x38,KH_XMM(g,8));
    v4f c=LD(v4f,matrix+32);
    ST(v128,sp-0x48,KH_XMM(g,9));
    float w=LD(float,vec+12);
    ST(v128,sp-0x58,KH_XMM(g,10));
    v4f d=LD(v4f,matrix+48);
    ST(v128,sp-0x68,KH_XMM(g,11));
    v4f a=LD(v4f,matrix);
    ST(v128,sp-0x78,KH_XMM(g,12));

    v4f r=((a*x+b*y)+c*z)+d*w;  /* built with -ffp-contract=off */
    if(__builtin_expect((r[0]!=r[0] || r[1]!=r[1] || r[2]!=r[2] || r[3]!=r[3]),0)){
        for(int i=0;i<4;i++) if(r[i]!=r[i]){
            float ab=kh_addf32(kh_mulf32(a[i],x),kh_mulf32(b[i],y));
            r[i]=kh_addf32(kh_addf32(ab,kh_mulf32(c[i],z)),kh_mulf32(d[i],w));
        }
    }
    ST(v4f,out,r);
    KH_XMM(g,0)=(v128)(v4f){kh_mulf32(d[2],w),d[2],d[2],d[2]};
    KH_XMM(g,1)=(v128)(v4f){kh_mulf32(c[2],z),c[2],c[2],c[2]};
    KH_XMM(g,2)=(v128)(v4f){r[2],a[2],a[2],a[2]};
    KH_XMM(g,3)=(v128)(v4f){x,0,0,0};
    KH_XMM(g,4)=(v128)(v4f){r[1],a[1],a[1],a[1]};
    KH_XMM(g,5)=(v128)(v4f){y,0,0,0};
    KH_R64(g,KO_R11)=sp; KH_RAX(g)=out;
    KH_R64(g,KO_CC_OP)=4; KH_R64(g,KO_CC_DEP1)=sp-0x88;
    KH_R64(g,KO_CC_DEP2)=0x88; KH_R64(g,KO_CC_NDEP)=0;
    KH_R64(g,KO_RIP)=GS_BASE+0x13fa8d;
    (void)LD(u64,sp); KH_RET(g);
}

int kh_transform_selftest(KCtx *g){
    u64 block=ga_alloc(512,16);
    if(!block) kh_fatal(g,"matrix-transform-selftest-allocation");
    u8 initial[512],expected[512]; u32 seed=1; int fails=0;
    for(int test=0;test<256;test++){
        KCtx oracle=*g;
        for(size_t i=0;i<sizeof initial;i+=4){
            seed=seed*1664525u+1013904223u;
            u32 bits=test%2?seed:(seed&0x807fffff)|0x40000000;
            memcpy(initial+i,&bits,4);
        }
        unsigned offset=test%4;
        KH_RSP(&oracle)=block+248; KH_RCX(&oracle)=block+268+offset;
        KH_R8(&oracle)=block+336+offset; KH_RDX(&oracle)=block+360+offset;
        if(test%8==3){
            static const u32 edge[]={0x80000000,1,0x80000001,0x7f800000,0xff800000,0x7f800123,0xffc04567,0x7f7fffff};
            for(unsigned i=0;i<16;i++) memcpy(initial+268+offset+4*i,&edge[(i+test/8)%8],4);
            for(unsigned i=0;i<4;i++) memcpy(initial+336+offset+4*i,&edge[(i+test/16)%8],4);
        }
        if(test%8==0) KH_RDX(&oracle)=KH_RCX(&oracle)+3;
        if(test%8==1) KH_RDX(&oracle)=KH_R8(&oracle);
        if(test%8==2) KH_RDX(&oracle)=KH_RSP(&oracle)-0x78;
        for(int i=0;i<13;i++) memcpy(&KH_XMM(&oracle,i),initial+16*i,16);
        KCtx fast=oracle;
        memcpy(hp(g,block),initial,sizeof initial); orig_f_13f900(&oracle);
        memcpy(expected,hp(g,block),sizeof expected);
        memcpy(hp(g,block),initial,sizeof initial); f_13f900(&fast);
        if(memcmp(oracle.st,fast.st,sizeof fast.st) || memcmp(expected,hp(g,block),sizeof expected)){
            fprintf(stderr,"kh: matrix-transform mismatch case %d\n",test); fails++; break;
        }
    }
    ga_free(block);
    fprintf(stderr,"kh: matrix-transform selftest: %d failures\n",fails); return fails;
}
