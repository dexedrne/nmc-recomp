#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>
static uint32_t hw_rcp(uint32_t x){ __m128 v=_mm_rcp_ps(_mm_castsi128_ps(_mm_set1_epi32(x))); return _mm_cvtsi128_si32(_mm_castps_si128(v)); }
static uint32_t hw_rsq(uint32_t x){ __m128 v=_mm_rsqrt_ps(_mm_castsi128_ps(_mm_set1_epi32(x))); return _mm_cvtsi128_si32(_mm_castps_si128(v)); }
static uint32_t TR[4096], TS0[4096], TS1[4096];     /* mantissa(23b) tables: rcp, rsqrt even exp, rsqrt odd exp */
static int ER[4096], ES0[4096], ES1[4096];          /* result exponent FIELD at the reference input exponent */
static uint32_t model_rcp(uint32_t x){
  uint32_t s=x&0x80000000u, e=(x>>23)&0xff, m=x&0x7fffff, t=m>>11;
  if(e==0) return s|0x7f800000u;                     /* zero/denormal -> inf */
  if(e==255){ if(m) return x|0x400000u; return s; }  /* NaN -> qnan ; inf -> 0 */
  int re=ER[t]-((int)e-127);
  if(re>=255) return s|0x7f800000u; if(re<=0) return s;   /* overflow -> inf, underflow -> 0 (flush) */
  return s|((uint32_t)re<<23)|TR[t];
}
static uint32_t model_rsq(uint32_t x){
  uint32_t s=x&0x80000000u, e=(x>>23)&0xff, m=x&0x7fffff, t=m>>11;
  if(e==0) return s|0x7f800000u;
  if(e==255){ if(m) return x|0x400000u; return s?0xffc00000u:0; }
  if(s) return 0xffc00000u;
  int odd=e&1; int ref=odd?127:128; /* reference exponents used to build tables */
  int re=(odd?ES1[t]:ES0[t]) - (((int)e-ref)>>1);
  if(re>=255||re<=0) return 0; /* should not happen for normals */
  return ((uint32_t)re<<23)|(odd?TS1[t]:TS0[t]);
}
int main(){
  for(int t=0;t<4096;t++){
    uint32_t x=0x3f800000u|(t<<11); uint32_t r=hw_rcp(x); TR[t]=r&0x7fffff; ER[t]=(r>>23)&0xff;
    uint32_t y0=0x40000000u|(t<<11); uint32_t r0=hw_rsq(y0); TS0[t]=r0&0x7fffff; ES0[t]=(r0>>23)&0xff;   /* e=128 (value in [2,4)) */
    uint32_t y1=0x3f800000u|(t<<11); uint32_t r1=hw_rsq(y1); TS1[t]=r1&0x7fffff; ES1[t]=(r1>>23)&0xff;   /* e=127 odd */
  }
  /* NOTE: in model_rsq, odd uses reference exponent 127 (value [1,2)) and even uses 128 -> fix ref below */
  uint64_t bad1=0,bad2=0; uint32_t ex1[8]={0},ex2[8]={0}; int n1=0,n2=0;
  for(uint64_t x=0;x<(1ull<<32);x++){
    uint32_t xx=(uint32_t)x;
    uint32_t a=hw_rcp(xx), b=model_rcp(xx);
    if(a!=b){ if(n1<8) ex1[n1++]=xx; bad1++; }
    uint32_t c=hw_rsq(xx), d=model_rsq(xx);
    if(c!=d){ if(n2<8) ex2[n2++]=xx; bad2++; }
  }
  printf("rcp model mismatches %llu ; rsq model mismatches %llu\n",(unsigned long long)bad1,(unsigned long long)bad2);
  for(int i=0;i<n1;i++) printf("rcp ex x=%08x hw=%08x model=%08x\n",ex1[i],hw_rcp(ex1[i]),model_rcp(ex1[i]));
  for(int i=0;i<n2;i++) printf("rsq ex x=%08x hw=%08x model=%08x\n",ex2[i],hw_rsq(ex2[i]),model_rsq(ex2[i]));
  return 0; }
