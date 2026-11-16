/* khtest_stub.c : deterministic callee stubs + test dispatch (portable C; used by difftest.py and by tests/drv.c).
 * Callees are replaced by kh_test_stub(): the Unicorn / native oracle side reproduces the same effect (tools/difftest.py). */
#include "khrt.h"
static u64 disp_n, disp_total, stub_total;
static u64 stub_n; static const u64 *stub_set; static size_t stub_cnt;
static u64 arena_lo, arena_hi;
void khtest_reset(void){ stub_n=0; disp_n=0; }
void khtest_config(const u64 *set, size_t n, u64 alo, u64 ahi){ stub_set=set; stub_cnt=n; arena_lo=alo; arena_hi=ahi; stub_n=0; }
u64 khtest_stub_count(void){ return stub_n; }
static inline u64 sm64(u64 x){ x+=0x9E3779B97F4A7C15ull; x=(x^(x>>30))*0xBF58476D1CE4E5B9ull; x=(x^(x>>27))*0x94D049BB133111EBull; return x^(x>>31); }
static inline v128 vfl(u64 h1,u64 h2){ /* four floats in [1,2) with random sign: only integer ops so both sides agree */
    v4u32 r; u64 hs[2]={h1,h2};
    for(int i=0;i<4;i++){ u64 h=hs[i>>1]>>((i&1)*32); r[i]=0x3f800000u|((u32)h&0x7fffffu)|(((u32)(h>>23)&1u)<<31); } return (v128)r; }
/* effect of a stubbed callee (mirrored in difftest.py): out-params, rax/rdx/volatile regs, xmm0-5, pop return address */
void kh_test_stub(KCtx *g, u64 va){
    u64 n=stub_n++; u64 h=sm64(va*0x100000001B3ull ^ (n*0x9E3779B97F4A7C15ull));
    u64 args[2]={KH_RCX(g),KH_RDX(g)};
    for(int i=0;i<2;i++){ u64 p=args[i]; if(p>=arena_lo && p+16<=arena_hi){ u64 w0=sm64(h+20+i), w1=sm64(h+30+i); memcpy((void*)XL(g,p),&w0,8); memcpy((void*)XL(g,p+8),&w1,8); } }
    KH_RAX(g)=sm64(h+1); KH_RDX(g)=sm64(h+2); KH_RCX(g)=sm64(h+3); KH_R8(g)=sm64(h+4); KH_R9(g)=sm64(h+5);
    KH_R64(g,KO_R10)=sm64(h+6); KH_R64(g,KO_R11)=sm64(h+7);
    for(int i=0;i<6;i++) KH_XMM(g,i)=vfl(sm64(h+40+2*i),sm64(h+41+2*i));
    KH_RET(g);
}
static int in_set(u64 t){ size_t lo=0,hi=stub_cnt; while(lo<hi){ size_t m=(lo+hi)/2; if(stub_set[m]<t) lo=m+1; else hi=m; } return lo<stub_cnt && stub_set[lo]==t; }
void kh_dispatch(KCtx *g, u64 t){ disp_n++; if(in_set(t)) kh_test_stub(g,t); else kh_fault(g,t,"dispatch-unknown"); }
void khtest_acc(void){ disp_total+=disp_n; stub_total+=stub_n; disp_n=0; }
u64 khtest_disp_total(void){ return disp_total; }
u64 khtest_stub_total(void){ return stub_total; }
