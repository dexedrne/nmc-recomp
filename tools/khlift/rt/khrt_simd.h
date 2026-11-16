/* khrt_simd.h : float policy helpers and 128-bit SIMD helpers (vx_*) used by emitted code.  See ../POLICY.md.
 *
 * ONE policy (x86 SSE semantics reproduced bit-exactly on every host):
 *   rounding     round-to-nearest-even; MXCSR is architecturally 0x1F80 (all exceptions masked, DAZ=FTZ=0).  Float->int
 *                conversions honour the rounding-mode operand VEX supplies (kh_create_mxcsr/sseround), out-of-range -> 0x80..0.
 *   denormals    full IEEE gradual underflow (no FTZ/DAZ).  The runtime must keep the host FPU in that mode (arm64: FPCR.FZ=0).
 *   NaN          an op that produces a NaN returns, in order: the first operand if it is a NaN (quieted), else the second
 *                operand if it is a NaN (quieted), else the x86 default QNaN (0xFFC00000 / 0xFFF8000000000000, sign SET).
 *                Implemented by an explicit fix-up on every host (KH_NANFIX=1, the default) -- the host op's own NaN choice is
 *                never trusted (arm64 differs from x86, and gcc/clang may commute operands even on x86-64).
 *   min / max    x86: a<b ? a : b (min), a>b ? a : b (max): second operand returned on NaN or equal (so min(+0,-0)=-0 operand
 *                order matters) -- emitted as the literal ternary, never fmin/fmax.
 *   compares     ordered/quiet C comparisons (false when either operand is NaN), UN = isnan(a)||isnan(b).
 *   rcpps/rsqrtps  bit-exact model of AMD Zen 3/4 silicon (12-bit table), see POLICY.md section 5.
 */
#pragma once
#ifndef KH_NANFIX
#  define KH_NANFIX 1          /* ALWAYS on by default, also on x86-64: a C compiler may swap the operands of a commutative
                                  add/mul, which changes which NaN payload wins; only the explicit fix-up is order-exact. */
#endif

static inline u32 kh_fb32(float x){ u32 u; memcpy(&u,&x,4); return u; }
static inline float kh_bf32(u32 u){ float x; memcpy(&x,&u,4); return x; }
static inline u64 kh_fb64(double x){ u64 u; memcpy(&u,&x,8); return u; }
static inline double kh_bf64(u64 u){ double x; memcpy(&x,&u,8); return x; }
/* NaN result of a binary op with at least one NaN result produced by the host: rebuild per x86 rules */
static inline float kh_nan2_32(float a,float b){
    u32 ua=kh_fb32(a), ub=kh_fb32(b);
    if((ua&0x7fffffffu)>0x7f800000u) return kh_bf32(ua|0x400000u);
    if((ub&0x7fffffffu)>0x7f800000u) return kh_bf32(ub|0x400000u);
    return kh_bf32(0xffc00000u);
}
static inline double kh_nan2_64(double a,double b){
    u64 ua=kh_fb64(a), ub=kh_fb64(b);
    if((ua&0x7fffffffffffffffull)>0x7ff0000000000000ull) return kh_bf64(ua|0x8000000000000ull);
    if((ub&0x7fffffffffffffffull)>0x7ff0000000000000ull) return kh_bf64(ub|0x8000000000000ull);
    return kh_bf64(0xfff8000000000000ull);
}
#if KH_NANFIX
#define KFIX32(r,a,b) ({ float _r=(r); (_r!=_r) ? kh_nan2_32((a),(b)) : _r; })
#define KFIX64(r,a,b) ({ double _r=(r); (_r!=_r) ? kh_nan2_64((a),(b)) : _r; })
#else
#define KFIX32(r,a,b) (r)
#define KFIX64(r,a,b) (r)
#endif

/* scalar float ops (VEX Iop_AddF32 ... and the lane kernels below) */
static inline float  kh_addf32(float a,float b){ return KFIX32(a+b,a,b); }
static inline float  kh_subf32(float a,float b){ return KFIX32(a-b,a,b); }
static inline float  kh_mulf32(float a,float b){ return KFIX32(a*b,a,b); }
static inline float  kh_divf32(float a,float b){ return KFIX32(a/b,a,b); }
static inline double kh_addf64(double a,double b){ return KFIX64(a+b,a,b); }
static inline double kh_subf64(double a,double b){ return KFIX64(a-b,a,b); }
static inline double kh_mulf64(double a,double b){ return KFIX64(a*b,a,b); }
static inline double kh_divf64(double a,double b){ return KFIX64(a/b,a,b); }
static inline float  kh_sqrtf32(float a){ return KFIX32(sqrtf(a),a,a); }
static inline double kh_sqrtf64(double a){ return KFIX64(sqrt(a),a,a); }
static inline float  kh_negf32(float a){ return kh_bf32(kh_fb32(a)^0x80000000u); }
static inline double kh_negf64(double a){ return kh_bf64(kh_fb64(a)^0x8000000000000000ull); }
static inline float  kh_absf32(float a){ return kh_bf32(kh_fb32(a)&0x7fffffffu); }
static inline double kh_absf64(double a){ return kh_bf64(kh_fb64(a)&0x7fffffffffffffffull); }
static inline double kh_f32tof64(float a){
#if KH_NANFIX
    if(a!=a){ u32 u=kh_fb32(a); return kh_bf64(((u64)(u&0x80000000u)<<32)|0x7ff8000000000000ull|((u64)(u&0x7fffffu)<<29)); }
#endif
    return (double)a; }
static inline float kh_f64tof32(double a){
#if KH_NANFIX
    if(a!=a){ u64 u=kh_fb64(a); return kh_bf32((u32)((u>>32)&0x80000000u)|0x7fc00000u|(u32)((u&0xfffffffffffffull)>>29)); }
#endif
    return (float)a; }
static inline float  kh_min32(float a,float b){ return a<b?a:b; }
static inline float  kh_max32(float a,float b){ return a>b?a:b; }
static inline double kh_min64(double a,double b){ return a<b?a:b; }
static inline double kh_max64(double a,double b){ return a>b?a:b; }
/* reciprocal estimates: bit-exact model of AMD Zen 3/4 RCPPS and RSQRTPS (POLICY.md section 5): the result depends only on
 * sign, exponent and the top 12 mantissa bits; table = measured hardware output at a reference exponent, exponent shifted per input.
 * Verified against silicon for all 2^32 inputs (tools/tests/rcp_model_check.c). */
extern const u32 kh_rcp_tab[3*4096];
static inline float kh_rcp32(float a){
    u32 x=kh_fb32(a), s=x&0x80000000u, e=(x>>23)&0xff, m=x&0x7fffffu;
    if(e==0) return kh_bf32(s|0x7f800000u);
    if(e==255) return kh_bf32(m ? (x|0x400000u) : s);
    u32 r=kh_rcp_tab[m>>11]; int re=(int)((r>>23)&0xff)-((int)e-127);
    if(re>=255) return kh_bf32(s|0x7f800000u);
    if(re<=0) return kh_bf32(s);
    return kh_bf32(s|((u32)re<<23)|(r&0x7fffffu));
}
static inline float kh_rsqrt32(float a){
    u32 x=kh_fb32(a), s=x&0x80000000u, e=(x>>23)&0xff, m=x&0x7fffffu;
    if(e==0) return kh_bf32(s|0x7f800000u);
    if(e==255) return kh_bf32(m ? (x|0x400000u) : (s ? 0xffc00000u : 0u));
    if(s) return kh_bf32(0xffc00000u);
    int odd=(int)(e&1); u32 r=kh_rcp_tab[(odd?8192:4096)+(m>>11)];
    int re=(int)((r>>23)&0xff)-(((int)e-(odd?127:128))>>1);
    if(re>=255||re<=0) return 0.0f;
    return kh_bf32(((u32)re<<23)|(r&0x7fffffu));
}

/* ---- whole-vector / lane moves ---- */
static inline u64 vx_V128to64(v128 a){ u64 r; memcpy(&r,&a,8); return r; }
static inline u64 vx_V128HIto64(v128 a){ u64 r; memcpy(&r,((u8*)&a)+8,8); return r; }
static inline u32 vx_V128to32(v128 a){ u32 r; memcpy(&r,&a,4); return r; }
static inline v128 vx_64HLtoV128(u64 h,u64 l){ v128 x; memcpy(&x,&l,8); memcpy(((u8*)&x)+8,&h,8); return x; }
static inline v128 vx_64UtoV128(u64 l){ v128 x=VZ; memcpy(&x,&l,8); return x; }
static inline v128 vx_32UtoV128(u32 l){ v128 x=VZ; memcpy(&x,&l,4); return x; }
static inline v128 vx_ZeroHI96ofV128(v128 a){ v128 r=VZ; memcpy(&r,&a,4); return r; }
static inline v128 vx_ZeroHI64ofV128(v128 a){ v128 r=VZ; memcpy(&r,&a,8); return r; }
static inline v128 vx_ZeroHI112ofV128(v128 a){ v128 r=VZ; memcpy(&r,&a,2); return r; }
static inline v128 vx_ZeroHI120ofV128(v128 a){ v128 r=VZ; memcpy(&r,&a,1); return r; }
static inline v128 vx_SetV128lo32(v128 a,u32 l){ memcpy(&a,&l,4); return a; }
static inline v128 vx_SetV128lo64(v128 a,u64 l){ memcpy(&a,&l,8); return a; }
static inline v128 vx_ShlV128(v128 a,u8 n){ u128 x; memcpy(&x,&a,16); x=(n>=128)?0:(x<<n); memcpy(&a,&x,16); return a; }
static inline v128 vx_ShrV128(v128 a,u8 n){ u128 x; memcpy(&x,&a,16); x=(n>=128)?0:(x>>n); memcpy(&a,&x,16); return a; }
static inline v128 vx_Dup8x16(u8 x){ v16u8 r; for(int i=0;i<16;i++) r[i]=x; return (v128)r; }
static inline v128 vx_Dup16x8(u16 x){ v8u16 r; for(int i=0;i<8;i++) r[i]=x; return (v128)r; }
static inline v128 vx_Dup32x4(u32 x){ v4u32 r; for(int i=0;i<4;i++) r[i]=x; return (v128)r; }

/* ---- float lane ops: 32F x4 / 0x4, 64F x2 / 0x2 ---- */
#define KH_DEF_FBIN(NAME,OP32,OP64) \
static inline v128 vx_##NAME##32Fx4(v128 A,v128 B){ v4f a=(v4f)A,b=(v4f)B,r; for(int i=0;i<4;i++) r[i]=OP32(a[i],b[i]); return (v128)r; } \
static inline v128 vx_##NAME##32F0x4(v128 A,v128 B){ v4f a=(v4f)A,b=(v4f)B,r=a; r[0]=OP32(a[0],b[0]); return (v128)r; } \
static inline v128 vx_##NAME##64Fx2(v128 A,v128 B){ v2d a=(v2d)A,b=(v2d)B,r; for(int i=0;i<2;i++) r[i]=OP64(a[i],b[i]); return (v128)r; } \
static inline v128 vx_##NAME##64F0x2(v128 A,v128 B){ v2d a=(v2d)A,b=(v2d)B,r=a; r[0]=OP64(a[0],b[0]); return (v128)r; }
KH_DEF_FBIN(Add,kh_addf32,kh_addf64)
KH_DEF_FBIN(Sub,kh_subf32,kh_subf64)
KH_DEF_FBIN(Mul,kh_mulf32,kh_mulf64)
KH_DEF_FBIN(Div,kh_divf32,kh_divf64)
KH_DEF_FBIN(Min,kh_min32,kh_min64)
KH_DEF_FBIN(Max,kh_max32,kh_max64)
static inline v128 vx_Sqrt32Fx4(v128 A){ v4f a=(v4f)A,r; for(int i=0;i<4;i++) r[i]=kh_sqrtf32(a[i]); return (v128)r; }
static inline v128 vx_Sqrt32F0x4(v128 A){ v4f a=(v4f)A,r=a; r[0]=kh_sqrtf32(a[0]); return (v128)r; }
static inline v128 vx_Sqrt64Fx2(v128 A){ v2d a=(v2d)A,r; for(int i=0;i<2;i++) r[i]=kh_sqrtf64(a[i]); return (v128)r; }
static inline v128 vx_Sqrt64F0x2(v128 A){ v2d a=(v2d)A,r=a; r[0]=kh_sqrtf64(a[0]); return (v128)r; }
#define KH_DEF_FCMP(NAME,EXPR) \
static inline v128 vx_Cmp##NAME##32Fx4(v128 A,v128 B){ v4f a=(v4f)A,b=(v4f)B; v4i32 r; for(int i=0;i<4;i++) r[i]=(EXPR(a[i],b[i]))?-1:0; return (v128)r; } \
static inline v128 vx_Cmp##NAME##32F0x4(v128 A,v128 B){ v4f a=(v4f)A,b=(v4f)B; v4i32 r=(v4i32)a; r[0]=(EXPR(a[0],b[0]))?-1:0; return (v128)r; } \
static inline v128 vx_Cmp##NAME##64Fx2(v128 A,v128 B){ v2d a=(v2d)A,b=(v2d)B; v2i64 r; for(int i=0;i<2;i++) r[i]=(EXPR(a[i],b[i]))?-1:0; return (v128)r; } \
static inline v128 vx_Cmp##NAME##64F0x2(v128 A,v128 B){ v2d a=(v2d)A,b=(v2d)B; v2i64 r=(v2i64)a; r[0]=(EXPR(a[0],b[0]))?-1:0; return (v128)r; }
#define KH_CEQ(x,y) ((x)==(y))
#define KH_CLT(x,y) ((x)<(y))
#define KH_CLE(x,y) ((x)<=(y))
#define KH_CUN(x,y) (((x)!=(x))||((y)!=(y)))
KH_DEF_FCMP(EQ,KH_CEQ)
KH_DEF_FCMP(LT,KH_CLT)
KH_DEF_FCMP(LE,KH_CLE)
KH_DEF_FCMP(UN,KH_CUN)
static inline v128 vx_Neg32Fx4(v128 A){ v4u32 a=(v4u32)A; a^=0x80000000u; return (v128)a; }
static inline v128 vx_Neg64Fx2(v128 A){ v2u64 a=(v2u64)A; a^=0x8000000000000000ull; return (v128)a; }
static inline v128 vx_Abs32Fx4(v128 A){ v4u32 a=(v4u32)A; a&=0x7fffffffu; return (v128)a; }
static inline v128 vx_Abs64Fx2(v128 A){ v2u64 a=(v2u64)A; a&=0x7fffffffffffffffull; return (v128)a; }
static inline v128 vx_RecipEst32Fx4(v128 A){ v4f a=(v4f)A,r; for(int i=0;i<4;i++) r[i]=kh_rcp32(a[i]); return (v128)r; }
static inline v128 vx_RSqrtEst32Fx4(v128 A){ v4f a=(v4f)A,r; for(int i=0;i<4;i++) r[i]=kh_rsqrt32(a[i]); return (v128)r; }
static inline v128 vx_RecipEst32F0x4(v128 A){ v4f a=(v4f)A,r=a; r[0]=kh_rcp32(a[0]); return (v128)r; }
static inline v128 vx_RSqrtEst32F0x4(v128 A){ v4f a=(v4f)A,r=a; r[0]=kh_rsqrt32(a[0]); return (v128)r; }
static inline v128 vx_I32StoF32x4(v128 A){ v4i32 a=(v4i32)A; v4f r; for(int i=0;i<4;i++) r[i]=(float)a[i]; return (v128)r; }
static inline v128 vx_F32toI32Sx4(v128 A,int rm){ v4f a=(v4f)A; v4i32 r; for(int i=0;i<4;i++) r[i]=f2i32(a[i],rm); return (v128)r; }
static inline v128 vx_F32toF64x2(v128 A){ v4f a=(v4f)A; v2d r={kh_f32tof64(a[0]),kh_f32tof64(a[1])}; return (v128)r; }
static inline v128 vx_F64toF32x2(v128 A){ v2d a=(v2d)A; v4f r={kh_f64tof32(a[0]),kh_f64tof32(a[1]),0,0}; return (v128)r; }

/* ---- integer lane ops ---- */
#define KH_DEF_IARITH(NAME,SYM,W,L,T) static inline v128 vx_##NAME##W##x##L(v128 a,v128 b){ return (v128)((T)a SYM (T)b); }
#define KH_DEF_IARITH_ALL(NAME,SYM) KH_DEF_IARITH(NAME,SYM,8,16,v16u8) KH_DEF_IARITH(NAME,SYM,16,8,v8u16) KH_DEF_IARITH(NAME,SYM,32,4,v4u32) KH_DEF_IARITH(NAME,SYM,64,2,v2u64)
KH_DEF_IARITH_ALL(Add,+)
KH_DEF_IARITH_ALL(Sub,-)
KH_DEF_IARITH(Mul,*,16,8,v8u16)
KH_DEF_IARITH(Mul,*,32,4,v4u32)
KH_DEF_IARITH(Mul,*,64,2,v2u64)
#define KH_DEF_CMPI(NAME,SYM,SU,W,L,T) static inline v128 vx_Cmp##NAME##W##SU##x##L(v128 a,v128 b){ return (v128)((T)a SYM (T)b); }
#define KH_DEF_CMPI_EQ(W,L,T) static inline v128 vx_CmpEQ##W##x##L(v128 a,v128 b){ return (v128)((T)a == (T)b); }
KH_DEF_CMPI_EQ(8,16,v16u8) KH_DEF_CMPI_EQ(16,8,v8u16) KH_DEF_CMPI_EQ(32,4,v4u32) KH_DEF_CMPI_EQ(64,2,v2u64)
KH_DEF_CMPI(GT,>,S,8,16,v16i8) KH_DEF_CMPI(GT,>,S,16,8,v8i16) KH_DEF_CMPI(GT,>,S,32,4,v4i32) KH_DEF_CMPI(GT,>,S,64,2,v2i64)
KH_DEF_CMPI(GT,>,U,8,16,v16u8) KH_DEF_CMPI(GT,>,U,16,8,v8u16) KH_DEF_CMPI(GT,>,U,32,4,v4u32) KH_DEF_CMPI(GT,>,U,64,2,v2u64)
#define KH_DEF_MINMAX(NAME,SYM,SU,W,L,T) static inline v128 vx_##NAME##W##SU##x##L(v128 A,v128 B){ T a=(T)A,b=(T)B,r; for(int i=0;i<L;i++) r[i]=(a[i] SYM b[i])?a[i]:b[i]; return (v128)r; }
#define KH_DEF_MINMAX_ALL(SU,T8,T16,T32,T64) \
  KH_DEF_MINMAX(Min,<,SU,8,16,T8) KH_DEF_MINMAX(Min,<,SU,16,8,T16) KH_DEF_MINMAX(Min,<,SU,32,4,T32) KH_DEF_MINMAX(Min,<,SU,64,2,T64) \
  KH_DEF_MINMAX(Max,>,SU,8,16,T8) KH_DEF_MINMAX(Max,>,SU,16,8,T16) KH_DEF_MINMAX(Max,>,SU,32,4,T32) KH_DEF_MINMAX(Max,>,SU,64,2,T64)
KH_DEF_MINMAX_ALL(S,v16i8,v8i16,v4i32,v2i64)
KH_DEF_MINMAX_ALL(U,v16u8,v8u16,v4u32,v2u64)
#define KH_DEF_SHN(NAME,W,L,T,BODY) static inline v128 vx_##NAME##N##W##x##L(v128 A,u8 s){ T a=(T)A; BODY; return (v128)a; }
#define KH_SHL_BODY(W) if(s>=W) a=a-a; else a=a<<s
#define KH_SHR_BODY(W) if(s>=W) a=a-a; else a=a>>s
#define KH_SAR_BODY(W) if(s>=W) s=W-1; a=a>>s
KH_DEF_SHN(Shl,8,16,v16u8,KH_SHL_BODY(8))  KH_DEF_SHN(Shl,16,8,v8u16,KH_SHL_BODY(16)) KH_DEF_SHN(Shl,32,4,v4u32,KH_SHL_BODY(32)) KH_DEF_SHN(Shl,64,2,v2u64,KH_SHL_BODY(64))
KH_DEF_SHN(Shr,8,16,v16u8,KH_SHR_BODY(8))  KH_DEF_SHN(Shr,16,8,v8u16,KH_SHR_BODY(16)) KH_DEF_SHN(Shr,32,4,v4u32,KH_SHR_BODY(32)) KH_DEF_SHN(Shr,64,2,v2u64,KH_SHR_BODY(64))
KH_DEF_SHN(Sar,8,16,v16i8,KH_SAR_BODY(8))  KH_DEF_SHN(Sar,16,8,v8i16,KH_SAR_BODY(16)) KH_DEF_SHN(Sar,32,4,v4i32,KH_SAR_BODY(32)) KH_DEF_SHN(Sar,64,2,v2i64,KH_SAR_BODY(64))
/* InterleaveLO/HI (x,y): result lane 2i+1 = x[off+i], lane 2i = y[off+i] (VEX argL is the odd lane) */
#define KH_DEF_INTER(NAME,OFFEXPR,W,L,T) static inline v128 vx_Interleave##NAME##W##x##L(v128 X,v128 Y){ T l=(T)X,r=(T)Y,o; for(int i=0;i<L/2;i++){ o[2*i+1]=l[(OFFEXPR)+i]; o[2*i]=r[(OFFEXPR)+i]; } return (v128)o; }
KH_DEF_INTER(LO,0,8,16,v16u8) KH_DEF_INTER(LO,0,16,8,v8u16) KH_DEF_INTER(LO,0,32,4,v4u32) KH_DEF_INTER(LO,0,64,2,v2u64)
KH_DEF_INTER(HI,8,8,16,v16u8) KH_DEF_INTER(HI,4,16,8,v8u16) KH_DEF_INTER(HI,2,32,4,v4u32) KH_DEF_INTER(HI,1,64,2,v2u64)
/* narrowing: result low half from argR, high half from argL (VEX convention) */
static inline s16 kh_satS16toU8(s16 v){ return v<0?0:(v>255?255:v); }
static inline s16 kh_satS16toS8(s16 v){ return v<-128?-128:(v>127?127:v); }
static inline s32 kh_satS32toS16(s32 v){ return v<-32768?-32768:(v>32767?32767:v); }
static inline s32 kh_satS32toU16(s32 v){ return v<0?0:(v>65535?65535:v); }
static inline v128 vx_QNarrowBin16Sto8Ux16(v128 L,v128 R){ v8i16 l=(v8i16)L,r=(v8i16)R; v16u8 o; for(int i=0;i<8;i++){ o[i]=(u8)kh_satS16toU8(r[i]); o[8+i]=(u8)kh_satS16toU8(l[i]); } return (v128)o; }
static inline v128 vx_QNarrowBin16Sto8Sx16(v128 L,v128 R){ v8i16 l=(v8i16)L,r=(v8i16)R; v16i8 o; for(int i=0;i<8;i++){ o[i]=(s8)kh_satS16toS8(r[i]); o[8+i]=(s8)kh_satS16toS8(l[i]); } return (v128)o; }
static inline v128 vx_QNarrowBin32Sto16Sx8(v128 L,v128 R){ v4i32 l=(v4i32)L,r=(v4i32)R; v8i16 o; for(int i=0;i<4;i++){ o[i]=(s16)kh_satS32toS16(r[i]); o[4+i]=(s16)kh_satS32toS16(l[i]); } return (v128)o; }
static inline v128 vx_QNarrowBin32Sto16Ux8(v128 L,v128 R){ v4i32 l=(v4i32)L,r=(v4i32)R; v8u16 o; for(int i=0;i<4;i++){ o[i]=(u16)kh_satS32toU16(r[i]); o[4+i]=(u16)kh_satS32toU16(l[i]); } return (v128)o; }
static inline v128 vx_NarrowBin16to8x16(v128 L,v128 R){ v8u16 l=(v8u16)L,r=(v8u16)R; v16u8 o; for(int i=0;i<8;i++){ o[i]=(u8)r[i]; o[8+i]=(u8)l[i]; } return (v128)o; }
static inline v128 vx_NarrowBin32to16x8(v128 L,v128 R){ v4u32 l=(v4u32)L,r=(v4u32)R; v8u16 o; for(int i=0;i<4;i++){ o[i]=(u16)r[i]; o[4+i]=(u16)l[i]; } return (v128)o; }
static inline u16 vx_GetMSBs8x16(v128 A){ v16u8 a=(v16u8)A; u16 r=0; for(int i=0;i<16;i++) r|=(u16)((a[i]>>7)&1)<<i; return r; }
