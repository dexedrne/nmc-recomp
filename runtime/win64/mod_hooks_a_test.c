/* Task 9 synthetic guest-state tests through the real alias dispatch. */
#include "mod_config.h"
int kh_mod_hooks_a_selftest(KCtx *g){
    int checks=0,failures=0;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod A FAIL line %d: %s\n",__LINE__,#c); } } while(0)
    const u32 forms[]={0,1,2,3,4,5,6,7,0xffffffff,0x80000000};
    u64 sp=KH_RSP(g);
    for(unsigned i=0;i<sizeof forms/sizeof *forms;i++){
        u32 f=forms[i]; KH_RAX(g)=0x123456789abcde77ull;
        u64 r=kh_call(g,GS_BASE+0x2fc860,f,0,0,0);
        T(r==(0x123456789abcde00ull | (f==0||f==2||f==3||f==5)));
        T(KH_RSP(g)==sp);
    }
    T(kh_call(g,GS_BASE+0x2fc4e0,0,0,0,0)==0x2d);
    fprintf(stderr,"kh: mod A selftest: %d checks, %d failures\n",checks,failures);
    return failures;
#undef T
}
