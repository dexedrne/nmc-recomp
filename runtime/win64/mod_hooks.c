/* Task 8: the gummi stop gate; other active detours remain pending in STATUS. */
#include "mod_config.h"
static _Atomic u64 gummi_blocks;
void orig_f_1505a0(KCtx *g);
void kh_phase_mode_entry(void);
void f_1505a0(KCtx *g){
    kh_phase_mode_entry();
    if(A0(g)==GS_BASE+KH_MOD_GUMMI_TASK){
        u64 count=atomic_fetch_add(&gummi_blocks,1)+1;
        fprintf(stderr,"kh: gummi gate blocked=%llu task=0x716c10\n",(unsigned long long)count);
        /* The caller already invoked its UI transition and ignores RAX. */
        kh_fatal(g,"gummi-gate-contract: after 24ffb0 invokes its UI transition at 25021f and requests task 716c10 at 25022d, which original transition restores the world map and clears afc0c4 bit 3 when no gummi task is created?");
    }
    orig_f_1505a0(g);
}
void kh_mod_hooks_dump(void){
    kh_mod_hooks_a_dump();
    kh_mod_params_dump();
    fprintf(stderr,"kh: mod hooks gummi-blocks=%llu\n",(unsigned long long)atomic_load(&gummi_blocks));
}
