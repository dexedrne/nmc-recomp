/* Shipped KH2Tweaks function detours, derived from objdump; see docs/task9-hooks-a.md. */
#include "mod_config.h"
static _Atomic u64 hits[9], extensions[9];
void orig_f_2fc860(KCtx *); void orig_f_2fc4e0(KCtx *);
void orig_f_4014a0(KCtx *); void orig_f_3a7cb0(KCtx *); void orig_f_29efa0(KCtx *);
void orig_f_405440(KCtx *); void orig_f_3a89a0(KCtx *);
void orig_f_3d61d0(KCtx *); void orig_f_3d60c0(KCtx *);
void f_2fc860(KCtx *g){
    hits[0]++;
    u32 form=A0(g);
    KH_RAX(g)=(KH_RAX(g)&~255ull)|(form==0||form==2||form==3||form==5);
    KH_RET(g);
}
void f_2fc4e0(KCtx *g){ hits[1]++; KH_RAX(g)=0x2d; KH_RET(g); }
void kh_mod_hooks_a_dump(void){
    const char *names[]={"2fc860","2fc4e0","4014a0","3a7cb0","29efa0","405440","3a89a0","3d61d0","3d60c0"};
    for(int i=0;i<9;i++) fprintf(stderr,"kh: mod A %s calls=%llu extensions=%llu\n",names[i],(unsigned long long)hits[i],(unsigned long long)extensions[i]);
}
