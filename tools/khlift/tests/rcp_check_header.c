/* exhaustive check: kh_rcp32 / kh_rsqrt32 (khrt_simd.h, table khrt_rcp_tab.c) vs the host CPU's RCPPS / RSQRTPS, all 2^32 inputs.
 *   cc -O2 -msse4.1 -I ../rt rcp_check_header.c ../rt/khrt_rcp_tab.c -o rcp_check && ./rcp_check */
#include "khrt.h"
#include <stdio.h>
#include <immintrin.h>
int main(void){ unsigned long long b1=0,b2=0;
  for(unsigned long long x=0;x<(1ull<<32);x+=4){
    __m128i i=_mm_set_epi32(x+3,x+2,x+1,x); __m128 f=_mm_castsi128_ps(i);
    u32 a[4],b[4]; _mm_storeu_si128((__m128i*)a,_mm_castps_si128(_mm_rcp_ps(f))); _mm_storeu_si128((__m128i*)b,_mm_castps_si128(_mm_rsqrt_ps(f)));
    for(int k=0;k<4;k++){ float fx=kh_bf32((u32)(x+k)); if(kh_fb32(kh_rcp32(fx))!=a[k]) b1++; if(kh_fb32(kh_rsqrt32(fx))!=b[k]) b2++; } }
  printf("kh_rcp32 mismatches %llu, kh_rsqrt32 mismatches %llu (of 2^32)\n",b1,b2); return (b1||b2)?1:0; }
