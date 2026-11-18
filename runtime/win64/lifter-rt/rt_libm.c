/* rt_libm.c : the CRT math imports as named runtime wrappers (guest ABI: Win64, floats in xmm0/xmm1, result in xmm0).
 * POLICY.md section 7: single-precision functions are evaluated in double precision and rounded once to float, so the
 * result is independent of the host's own sinf/cosf implementation (correctly rounded except for the rare double-rounding
 * case); double functions use the host libm.  These strong definitions override the weak defaults in rt_imports_default.c. */
#include "khrt.h"
void kh_trace_import(KCtx *g,const char *name);   /* runtime-v0 addition: libm imports appear in the import trace (the lifter's original rt_libm.c is untraced) */
#define MTR(g,n) kh_trace_import((g),"api_ms_win_crt_math_l1_1_0!" n)
#define F1(NAME,EXPR) void rt_imp_api_ms_win_crt_math_l1_1_0__##NAME(KCtx *g){ MTR(g,#NAME); float x; memcpy(&x,&KH_XMM(g,0),4); float r=(float)(EXPR); v128 o=VZ; memcpy(&o,&r,4); KH_XMM(g,0)=o; KH_RET(g); }
#define F2(NAME,EXPR) void rt_imp_api_ms_win_crt_math_l1_1_0__##NAME(KCtx *g){ MTR(g,#NAME); float x,y; memcpy(&x,&KH_XMM(g,0),4); memcpy(&y,&KH_XMM(g,1),4); float r=(float)(EXPR); v128 o=VZ; memcpy(&o,&r,4); KH_XMM(g,0)=o; KH_RET(g); }
#define D1(NAME,EXPR) void rt_imp_api_ms_win_crt_math_l1_1_0__##NAME(KCtx *g){ MTR(g,#NAME); double x; memcpy(&x,&KH_XMM(g,0),8); double r=(EXPR); v128 o=VZ; memcpy(&o,&r,8); KH_XMM(g,0)=o; KH_RET(g); }
#define D2(NAME,EXPR) void rt_imp_api_ms_win_crt_math_l1_1_0__##NAME(KCtx *g){ MTR(g,#NAME); double x,y; memcpy(&x,&KH_XMM(g,0),8); memcpy(&y,&KH_XMM(g,1),8); double r=(EXPR); v128 o=VZ; memcpy(&o,&r,8); KH_XMM(g,0)=o; KH_RET(g); }
/* the out-parameters of modf / modff / frexp are guest stores like any other (review Y39 n=6): ST under KH_ORDERED_MEM (an ordered
 * store when aligned), the memcpy of before otherwise.  #line keeps the plain build's line table (its object is unchanged) */
#if defined(KH_ORDERED_MEM) && KH_ORDERED_MEM
#define LIBM_OUT(T,a,src) ST(T,a,*(const T*)(src))
#else
#define LIBM_OUT(T,a,src) memcpy((void*)XL(g,a),(src),sizeof(T))
#endif
#line 12
F1(sinf,sin((double)x))   F1(cosf,cos((double)x))   F1(tanf,tan((double)x))
F1(asinf,asin((double)x)) F1(acosf,acos((double)x)) F1(atanf,atan((double)x))
F1(expf,exp((double)x))   F1(logf,log((double)x))   F1(log10f,log10((double)x))
F1(floorf,floor((double)x)) F1(ceilf,ceil((double)x)) F1(roundf,round((double)x)) F1(sqrtf,kh_sqrtf32(x))
F2(atan2f,atan2((double)x,(double)y)) F2(powf,pow((double)x,(double)y)) F2(fmodf,fmod((double)x,(double)y))
D1(sin,sin(x)) D1(cos,cos(x)) D1(atan,atan(x)) D1(acos,acos(x)) D1(exp,exp(x)) D1(log,log(x)) D1(floor,floor(x)) D1(ceil,ceil(x)) D1(sqrt,kh_sqrtf64(x))
D2(pow,pow(x,y))
void rt_imp_api_ms_win_crt_math_l1_1_0__modf(KCtx *g){ MTR(g,"modf"); double x,ip; memcpy(&x,&KH_XMM(g,0),8); double f=modf(x,&ip); LIBM_OUT(double,KH_RDX(g),&ip);       v128 o=VZ; memcpy(&o,&f,8); KH_XMM(g,0)=o; KH_RET(g); }
void rt_imp_api_ms_win_crt_math_l1_1_0__modff(KCtx *g){ MTR(g,"modff"); float x; memcpy(&x,&KH_XMM(g,0),4); double ip; float f=(float)modf((double)x,&ip); float fi=(float)ip; LIBM_OUT(float,KH_RDX(g),&fi);        v128 o=VZ; memcpy(&o,&f,4); KH_XMM(g,0)=o; KH_RET(g); }
void rt_imp_api_ms_win_crt_math_l1_1_0__frexp(KCtx *g){ MTR(g,"frexp"); double x; memcpy(&x,&KH_XMM(g,0),8); int e; double f=frexp(x,&e); s32 e32=e; LIBM_OUT(s32,KH_RDX(g),&e32);          v128 o=VZ; memcpy(&o,&f,8); KH_XMM(g,0)=o; KH_RET(g); }
void rt_imp_api_ms_win_crt_math_l1_1_0__ldexp(KCtx *g){ MTR(g,"ldexp"); double x; memcpy(&x,&KH_XMM(g,0),8); double r=ldexp(x,(int)(s32)KH_RDX(g)); v128 o=VZ; memcpy(&o,&r,8); KH_XMM(g,0)=o; KH_RET(g); }
void rt_imp_api_ms_win_crt_math_l1_1_0___isnan(KCtx *g){ MTR(g,"_isnan"); double x; memcpy(&x,&KH_XMM(g,0),8); KH_RAX(g)=(x!=x); KH_RET(g); }
void rt_imp_api_ms_win_crt_math_l1_1_0___finite(KCtx *g){ MTR(g,"_finite"); double x; memcpy(&x,&KH_XMM(g,0),8); KH_RAX(g)=isfinite(x)?1:0; KH_RET(g); }
