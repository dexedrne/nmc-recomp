/* mod_static_teststub.c : callee stubs, test dispatch and block trace for tools/mod_static_difftest.py (Task 12).
 *
 * Same contract as the lifter's rt/khtest_stub.c (both the native oracle's INT3 trap and the lifted side call kh_test_stub(), so
 * the two sides always agree), with two differences:
 *  - the stub's RAX is usually a pointer into the test arena (or a small integer / zero / one of the function's own compare
 *    constants) instead of a 64-bit hash.  The edited functions dereference what their callees return; with hash results almost
 *    every test dies on the first such access in the oracle and proves nothing;
 *  - the callee's view of its ARGUMENTS (rcx rdx r8 r9, xmm0-3, the 5th and 6th stack argument) feeds the stub effect, so a side
 *    that passes a different argument gets a different effect and the test fails.  Several edits only change a call argument. */
#include "khrt.h"
static u64 disp_n, disp_total, stub_total;
static u64 stub_n; static const u64 *stub_set; static size_t stub_cnt;
static u64 arena_lo, arena_hi;
static const u64 *ivals; static u32 nivals; static u64 stack_lo, stack_hi;
void khtest_values(const u64 *v, u32 n, u64 slo, u64 shi){ ivals=v; nivals=n; stack_lo=slo; stack_hi=shi; }
void khtest_reset(void){ stub_n=0; disp_n=0; }
void khtest_config(const u64 *set, size_t n, u64 alo, u64 ahi){ stub_set=set; stub_cnt=n; arena_lo=alo; arena_hi=ahi; stub_n=0; }
u64 khtest_stub_count(void){ return stub_n; }
static inline u64 sm64(u64 x){ x+=0x9E3779B97F4A7C15ull; x=(x^(x>>30))*0xBF58476D1CE4E5B9ull; x=(x^(x>>27))*0x94D049BB133111EBull; return x^(x>>31); }
static inline v128 vfl(u64 h1,u64 h2){
    v4u32 r; u64 hs[2]={h1,h2};
    for(int i=0;i<4;i++){ u64 h=hs[i>>1]>>((i&1)*32); r[i]=0x3f800000u|((u32)h&0x7fffffu)|(((u32)(h>>23)&1u)<<31); } return (v128)r; }
void kh_test_stub(KCtx *g, u64 va){
    u64 n=stub_n++; u64 h=sm64(va*0x100000001B3ull ^ (n*0x9E3779B97F4A7C15ull));
    u64 a=KH_RCX(g)*3+KH_RDX(g)*5+KH_R8(g)*7+KH_R9(g)*11;
    for(int i=0;i<4;i++){ u64 x; memcpy(&x,&KH_XMM(g,i),8); a=a*13+x; }
    u64 sp=KH_RSP(g);
    if(sp>=stack_lo && sp+0x38<=stack_hi){ u64 s5,s6; memcpy(&s5,(const void*)XL(g,sp+0x28),8); memcpy(&s6,(const void*)XL(g,sp+0x30),8); a=a*17+s5*19+s6; }
    h=sm64(h^sm64(a));
    u64 args[2]={KH_RCX(g),KH_RDX(g)};
    for(int i=0;i<2;i++){ u64 p=args[i]; if(p>=arena_lo && p+16<=arena_hi){ u64 w0=sm64(h+20+i), w1=sm64(h+30+i); memcpy((void*)XL(g,p),&w0,8); memcpy((void*)XL(g,p+8),&w1,8); } }
    u64 r=sm64(h+1), sel=r&15, ret;
    if(sel<10) ret=arena_lo+0x1000+(((r>>8)%0x1e000ull)&~15ull);       /* object pointer */
    else if(sel<12) ret=(nivals && ((r>>20)&1)) ? ivals[(r>>24)%nivals] : (r>>8)&63;   /* small integer / boolean / a constant the function compares against */
    else if(sel<14) ret=0;                                               /* null / false */
    else ret=r;                                                          /* anything */
    KH_RAX(g)=ret; KH_RDX(g)=sm64(h+2); KH_RCX(g)=sm64(h+3); KH_R8(g)=sm64(h+4); KH_R9(g)=sm64(h+5);
    KH_R64(g,KO_R10)=sm64(h+6); KH_R64(g,KO_R11)=sm64(h+7);
    for(int i=0;i<6;i++) KH_XMM(g,i)=vfl(sm64(h+40+2*i),sm64(h+41+2*i));
    KH_RET(g);
}
static int in_set(u64 t){ size_t lo=0,hi=stub_cnt; while(lo<hi){ size_t m=(lo+hi)/2; if(stub_set[m]<t) lo=m+1; else hi=m; } return lo<stub_cnt && stub_set[lo]==t; }
void kh_dispatch(KCtx *g, u64 t){ disp_n++; if(in_set(t)) kh_test_stub(g,t); else kh_fault(g,t,"dispatch-unknown"); }
void khtest_acc(void){ disp_total+=disp_n; stub_total+=stub_n; disp_n=0; }
u64 khtest_disp_total(void){ return disp_total; }
u64 khtest_stub_total(void){ return stub_total; }

/* block trace of the instrumented variants (c_<rva>) and their entry selector (0 = function entry, else a block address).
 * Single-block mode: the variant returns as soon as control reaches a block other than the entry block (kh_stop_id = that block);
 * the oracle stops at the same place through INT3 on the other block starts. */
#define KH_COV_CAP 16384
u16 kh_cov_buf[KH_COV_CAP]; u32 kh_cov_n; u64 kh_test_entry;
int kh_single; u32 kh_single_entry_id, kh_stop_id; int kh_stopped;
int kh_cov(u32 id){
    if(kh_single && id!=kh_single_entry_id){ kh_stopped=1; kh_stop_id=id; return 1; }
    if(kh_cov_n<KH_COV_CAP) kh_cov_buf[kh_cov_n]=(u16)id; kh_cov_n++; return 0;
}
