/* Task 11: deterministic case generator + canonical transcript used by BOTH the runtime selftest (mod_params_test.c, which drives the
 * port's f_40d810 / f_40dd70 with the call sink) and the native DLL oracle (tools/native_params_oracle.c, which executes the shipped
 * DLL's own handler code on the same cases with stub callees).  Identical transcripts mean identical call sequences, identical
 * (masked) arguments and identical memory effects.  Plain C99 so both programs can include it.
 *
 * The arena is one block of 0x4000 bytes; all objects live at fixed offsets and every pointer stored in it is base+offset, so two runs
 * at different bases differ only by that base, which the transcript removes (pointer-looking words and 'p' arguments print as @offset). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "mod_params_call.h"

#define MPC_ARENA 0x4000
enum {
    MPC_R = 0x100, MPC_C = 0x200, MPC_OBJD8 = 0x400, MPC_OBJE0 = 0x500, MPC_A = 0x1000, MPC_SHEET = 0x1e00, MPC_S = 0x2000, MPC_V = 0x2100,
    MPC_T148 = 0x2200, MPC_Q918 = 0x2300, MPC_REC3EAA = 0x2400, MPC_REC3B68 = 0x2500, MPC_SCRIPT = 0x2600, MPC_Q7B0 = 0x2680, MPC_TACT = 0x2700
};
struct mpc_case {
    int range, kind;
    uint32_t index, id, xmm2;
    int s_null, t_null;
    uint64_t base;
    uint8_t buf[MPC_ARENA];
};

static inline uint64_t mpc_rng(uint64_t *s){ uint64_t x=*s; x^=x>>12; x^=x<<25; x^=x>>27; *s=x; return x*2685821657736338717ull; }
static inline void mpc_w8(struct mpc_case *c,uint64_t o,uint8_t v){ c->buf[o]=v; }
static inline void mpc_w16(struct mpc_case *c,uint64_t o,uint16_t v){ memcpy(c->buf+o,&v,2); }
static inline void mpc_w32(struct mpc_case *c,uint64_t o,uint32_t v){ memcpy(c->buf+o,&v,4); }
static inline void mpc_w64(struct mpc_case *c,uint64_t o,uint64_t v){ memcpy(c->buf+o,&v,8); }
static inline void mpc_wf(struct mpc_case *c,uint64_t o,float f){ memcpy(c->buf+o,&f,4); }
static inline uint16_t mpc_small(uint64_t *s){
    static const uint16_t set[]={0,0,1,2,3,4,5,6,7,8,9,12,15,19,24,25,31,32,33,0x7fff,0x8000,0xffff,0xfffe,0x100};
    uint64_t r=mpc_rng(s); return (r&3)==0?(uint16_t)(r>>8):set[(r>>8)%(sizeof set/sizeof *set)];
}
static inline float mpc_float(uint64_t *s){
    static const uint32_t bits[]={0,0x3f800000,0xbf800000,0x7fc00000,0x3f000000,0x80000000,0x7f800000,0xff800000,0x42c80000,0x41200000,0x7f7fffff};
    uint64_t r=mpc_rng(s); uint32_t v=(r&3)==0?(uint32_t)(r>>16):bits[(r>>8)%(sizeof bits/sizeof *bits)]; float f; memcpy(&f,&v,4); return f;
}

static inline void mpc_make(struct mpc_case *c,int range,uint32_t index,uint64_t base){
    uint64_t s=0x9e3779b97f4a7c15ull ^ ((uint64_t)index*0xd1342543de82ef95ull) ^ (range?0x1111:0x2222); if(!s) s=1;
    memset(c,0,sizeof *c);
    c->range=range; c->index=index; c->base=base; c->id=0x1000+index; c->kind=range?54+(int)(index%30):50+(int)(index%31);
    c->xmm2=(uint32_t)mpc_rng(&s);
    for(int i=0;i<MPC_ARENA;i+=8){ uint64_t r=mpc_rng(&s); memcpy(c->buf+i,&r,8); }
    int kind=c->kind;
    /* the record: range = {+2 end low byte, +4 kind, +5 nparam, +6.. params}, frame = {+2 op, +3 nparam, +4 X, +6 Y} */
    mpc_w8(c,MPC_R+4,(uint8_t)kind);
    if(range){
        mpc_w8(c,MPC_R+5,(uint8_t)(mpc_rng(&s)%6));
        uint16_t p1=(mpc_rng(&s)&1)?0:mpc_small(&s);
        mpc_w16(c,MPC_R+6,p1); mpc_w16(c,MPC_R+8,(mpc_rng(&s)&1)?0:mpc_small(&s)); mpc_w16(c,MPC_R+10,(mpc_rng(&s)&1)?0:mpc_small(&s));
        mpc_w16(c,MPC_R+12,mpc_small(&s)); mpc_w16(c,MPC_R+14,mpc_small(&s));
        if(kind==83 && (mpc_rng(&s)&3)) mpc_w16(c,MPC_R+6,(uint16_t)(mpc_rng(&s)%26));
        if(kind==82 && (mpc_rng(&s)&1)) mpc_w16(c,MPC_R+6,8);
    } else {
        mpc_w8(c,MPC_R+2,(uint8_t)kind); mpc_w8(c,MPC_R+3,(uint8_t)(mpc_rng(&s)%3));
        uint16_t x=mpc_small(&s); if(!(mpc_rng(&s)&3)) x=(uint16_t)(mpc_rng(&s)%9);
        mpc_w16(c,MPC_R+4,x); mpc_w16(c,MPC_R+6,mpc_small(&s));
    }
    /* context */
    mpc_w64(c,MPC_C+0x10,base+MPC_A); mpc_w64(c,MPC_C+0xd8,base+MPC_OBJD8); mpc_w64(c,MPC_C+0xe0,base+MPC_OBJE0);
    mpc_wf(c,MPC_C+0x44,mpc_float(&s));
    /* actor and its helper objects: every pointer the handlers dereference is valid unless the DLL itself tests it */
    mpc_w64(c,MPC_A+0x148,(!range&&kind==53&&(mpc_rng(&s)&1))?0:base+MPC_T148);
    mpc_w64(c,MPC_A+0x5b0,base+MPC_SCRIPT); mpc_w64(c,MPC_A+0x5c0,base+MPC_SHEET); mpc_w64(c,MPC_A+0x7b0,base+MPC_Q7B0);
    mpc_w64(c,MPC_A+0x918,base+MPC_Q918); mpc_w64(c,MPC_A+0xdc8,base+MPC_V);
    if(!range&&(kind==63||kind==64)&&!(mpc_rng(&s)&3)) mpc_w64(c,MPC_A+0x5c0,0);
    { uint32_t f=(uint32_t)mpc_rng(&s); if(mpc_rng(&s)&1) f|=0x80; else f&=~0x80u; mpc_w32(c,MPC_A+0x6c8,f); }
    mpc_wf(c,MPC_A+0x674,mpc_float(&s)); mpc_wf(c,MPC_A+0x138,mpc_float(&s));
    mpc_wf(c,MPC_SHEET+0x1c0,mpc_float(&s));
    { uint32_t d=(uint32_t)mpc_rng(&s); if(mpc_rng(&s)&1) d|=1; mpc_w32(c,MPC_SHEET+0x1d0,d); }
    for(int i=0;i<2;i++){
        uint64_t r=mpc_rng(&s), q=mpc_rng(&s); int mode=(int)(mpc_rng(&s)%3); uint64_t v=mode==0?r:(mode==1?(r|q):(r&q));
        if(i==0&&(mpc_rng(&s)&1)) v|=0x100;
        mpc_w64(c,MPC_S+8*i,v);
    }
    c->s_null = range && (kind==60||kind==61||kind==62) && !(mpc_rng(&s)&3);
    c->t_null = (range&&(kind==60||kind==82)) || (!range&&kind==80) ? (int)(mpc_rng(&s)&1) : 0;
}

/* value the stubbed callee returns (identical in the runtime mock and in the oracle) */
static inline uint64_t mpc_fake(const struct mpc_case *c,uint32_t rva){
    switch(rva){
    case 0x39b760: return c->s_null?0:c->base+MPC_S;
    case 0x3d74e0: return c->t_null?0:c->base+MPC_TACT;
    case 0x3eaae0: return c->base+MPC_REC3EAA;
    case 0x3b68b0: return c->base+MPC_REC3B68;
    case 0x4ad240: return c->id;
    case 0x3b3e80: return 0x5151;
    default: return 0;
    }
}

/* argument masks per callee: p=64-bit pointer (printed relative to the arena), i=32-bit, b=8-bit, f=float xmm at that position,
 * c=pointed-to contents (kh_mp_call.mem), x=ignored by the callee.  The DLL leaves garbage in unused registers; those never print. */
struct mpc_spec { uint32_t rva; const char *name, *args; };
static const struct mpc_spec mpc_specs[]={
    {0x39b760,"get39b760","i"},{0x3c6dc0,"motion3c6dc0","piff"},{0x3c8310,"motion3c8310","piffi"},{0x3bde30,"vec3bde30","p"},{0x3d74e0,"find3d74e0","pi"},
    {0x3d78d0,"move3d78d0","ppp"},{0x3c0ad0,"drive3c0ad0","pi"},{0x3c0c30,"hp3c0c30","piii"},{0x3c0cd0,"mp3c0cd0","pii"},{0x4ad240,"id4ad240","p"},
    {0x3e1840,"script3e1840","picii"},{0x3b3e80,"get3b3e80",""},{0x3cb9f0,"spawn3cb9f0","ipb"},{0x3b4da0,"state3b4da0","pi"},{0x3d9fe0,"fx3d9fe0","ppii"},
    {0x3eaae0,"rec3eaae0","i"},{0x3db2f0,"byname3db2f0","pcp"},{0x1a85e0,"flag1a85e0","pi"},{0x409b00,"timed409b00","xpf"},{0x3b68b0,"rec3b68b0","i"},
    {0x40d810,"ORIG-range","pp"},{0x40dd70,"ORIG-frame","pp"},
};
static inline const struct mpc_spec *mpc_find_spec(uint32_t rva){ for(unsigned i=0;i<sizeof mpc_specs/sizeof *mpc_specs;i++) if(mpc_specs[i].rva==rva) return &mpc_specs[i]; return 0; }

static inline int mpc_fmt_ptr(char *o,size_t n,uint64_t v,uint64_t base){
    if(v>=base && v<base+MPC_ARENA) return snprintf(o,n,"@%llx",(unsigned long long)(v-base));
    return snprintf(o,n,"%llx",(unsigned long long)v);
}
/* appends one call to out; returns the new length */
static inline size_t mpc_fmt_call(char *out,size_t cap,size_t len,const struct kh_mp_call *c,uint64_t base){
    const struct mpc_spec *sp=mpc_find_spec(c->rva);
    char t[256]; size_t l=0;
    if(!sp){ l+=(size_t)snprintf(t+l,sizeof t-l,"UNKNOWN-RVA-%x",c->rva); }
    else {
        l+=(size_t)snprintf(t+l,sizeof t-l,"%s(",sp->name);
        for(unsigned i=0;sp->args[i];i++){
            if(i) l+=(size_t)snprintf(t+l,sizeof t-l,",");
            switch(sp->args[i]){
            case 'p': l+=(size_t)mpc_fmt_ptr(t+l,sizeof t-l,c->a[i],base); break;
            case 'i': l+=(size_t)snprintf(t+l,sizeof t-l,"%x",(unsigned)(uint32_t)c->a[i]); break;
            case 'b': l+=(size_t)snprintf(t+l,sizeof t-l,"%x",(unsigned)(uint8_t)c->a[i]); break;
            case 'f': l+=(size_t)snprintf(t+l,sizeof t-l,"f%x",(unsigned)(i==2?c->f2:c->f3)); break;
            case 'x': l+=(size_t)snprintf(t+l,sizeof t-l,"-"); break;
            case 'c':
                if(c->rva==0x3e1840) l+=(size_t)snprintf(t+l,sizeof t-l,"[%llx %llx %llx]",(unsigned long long)c->mem[0],(unsigned long long)c->mem[1],(unsigned long long)c->mem[2]);
                else l+=(size_t)snprintf(t+l,sizeof t-l,"'%010llx'",(unsigned long long)(c->mem[0]&0xffffffffffull));
                break;
            }
        }
        if(c->rva==0x409b00) l+=(size_t)snprintf(t+l,sizeof t-l,"d0=%llx",(unsigned long long)c->x0);
        l+=(size_t)snprintf(t+l,sizeof t-l,")");
    }
    if(len+l+2<cap){ memcpy(out+len,t,l); out[len+l]=';'; out[len+l+1]=0; return len+l+1; }
    return len;
}
static inline uint64_t mpc_fnv(uint64_t h,const void *p,size_t n){ const uint8_t *b=p; for(size_t i=0;i<n;i++){ h^=b[i]; h*=0x100000001b3ull; } return h; }
static inline uint64_t mpc_hash_arena(const uint8_t *a,uint64_t base){
    uint64_t h=0xcbf29ce484222325ull;
    for(int i=0;i<MPC_ARENA;i+=8){ uint64_t v; memcpy(&v,a+i,8); if(v>=base&&v<base+MPC_ARENA) v=0xA5A5000000000000ull|(v-base); h=mpc_fnv(h,&v,8); }
    return h;
}
/* one transcript line for a finished case */
static inline size_t mpc_line(char *out,size_t cap,const struct mpc_case *c,const struct kh_mp_call *calls,int ncalls,const uint8_t *arena){
    size_t len=(size_t)snprintf(out,cap,"%s k=%d i=%u s0=%d t0=%d calls=%d ",c->range?"R":"F",c->kind,c->index,c->s_null,c->t_null,ncalls);
    for(int i=0;i<ncalls;i++) len=mpc_fmt_call(out,cap,len,&calls[i],c->base);
    len+=(size_t)snprintf(out+len,cap-len,"arena=%016llx",(unsigned long long)mpc_hash_arena(arena,c->base));
    return len;
}
