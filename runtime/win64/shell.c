/* shell.c : the native shell that replaces the CLR/WinForms ManageRun path (G1-GATE.md section 2 "Boot and IL").
 *
 * The original exe is a mixed-mode image: axaAppMain calls the .nep unit 0x579bf0 = ManageRun(app), which runs a WinForms message loop
 * (BaseSimpleForm).  OnLoad calls Axa.DXInput.init(hwnd) (native 0x1335a0); OnShown calls Axa.GX.GXDevice.init(hwnd, app, 0) (native 0x11d070),
 * which creates the native child window and starts the two game threads (0x127ed0 and 0x129110).  Thread 0x127ed0 calls ExitApplication
 * (.nep 0x579bc0) when its loop ends.  Shutdown: ExitApplication -> Form.Close -> DXInput.exit + GXDevice.exit -> Application.Run returns.
 * Here those IL units are host functions: ManageRun performs DXInput.init and GXDevice.init and then parks until ExitApplication is called
 * (or the optional KH_RUN_SECONDS time limit of a headless run expires).  EndMessageClose and MouseEnable are no-ops.
 * These four symbols replace the generated abort stubs (tools/gen_nep.sh removes them from nep_stubs.c). */
#include "khnmc_rt.h"
#include <time.h>
#include <unistd.h>

#define HWND_MAIN 0xAB0001ull
static pthread_mutex_t sm=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t scv=PTHREAD_COND_INITIALIZER; static int exit_requested;
extern void kh_exit_process(int);
void f_1335a0(KCtx *g);   /* Axa.DXInput.init(hwnd) */
void f_11d070(KCtx *g);   /* Axa.GX.GXDevice.init(hwnd, app) */

/* oracle state dump: CRC-32 of every 4 KiB page of .data/.bss (+.shared) and of the non-zero guest-heap pages (stacks excluded) at the moment
 * ManageRun is entered; tools/oracle_unicorn.py computes the same from its emulation of the original bytes. */
static u32 crc_tab[256];
static u32 crc32_page(const u8 *p){ if(!crc_tab[1]) for(u32 i=0;i<256;i++){ u32 c=i; for(int k=0;k<8;k++) c=(c&1)?0xEDB88320u^(c>>1):c>>1; crc_tab[i]=c; } u32 c=0xFFFFFFFFu; for(int i=0;i<4096;i++) c=crc_tab[(c^p[i])&0xff]^(c>>8); return c^0xFFFFFFFFu; }
extern u64 ga_bump_ptr(void); extern u64 ga_arena_lo(void);
static void state_dump(KCtx *g,const char *path){
    FILE *f=fopen(path,"w"); if(!f) return; KThread *t=cur_thread();
    fprintf(f,"stack %llx %llx\n",(unsigned long long)t->stack_lo,(unsigned long long)t->stack_hi);
    u64 ranges[3][2]={{GS_BASE+0x713000,GS_BASE+0x2b83000},{GS_BASE+0x2bd4000,GS_BASE+0x2bd5000},{ga_arena_lo()&~0xfffull,(ga_bump_ptr()+0xfff)&~0xfffull}};
    char hp_[1200]; snprintf(hp_,sizeof hp_,"%s.heap",path); FILE *hb=fopen(hp_,"wb");
    for(int r=0;r<3;r++) for(u64 a=ranges[r][0];a<ranges[r][1];a+=4096){ if(r==2&&a>=t->stack_lo&&a<t->stack_hi) continue; fprintf(f,"%llx %08x\n",(unsigned long long)a,crc32_page((const u8*)XL(g,a)));
        if(r==2&&hb){ u64 aa=a; fwrite(&aa,8,1,hb); fwrite((const void*)XL(g,a),1,4096,hb); } }
    if(hb) fclose(hb);
    fclose(f); kh_warn("shell: state dump written to %s",path);
}
void f_579bf0(KCtx *g){                                  /* ManageRun(app) */
    u64 app=KH_RCX(g);
    if(getenv("KH_STATE_DUMP")) state_dump(g,getenv("KH_STATE_DUMP"));
    kh_warn("shell: ManageRun(app=0x%llx) -> native shell: DXInput.init, GXDevice.init",(unsigned long long)app);
    kh_call(g,GS_BASE+0x1335a0,HWND_MAIN,0,0,0);          /* BaseSimpleForm.OnLoad */
    kh_input_attach(g);
    kh_warn("shell: DXInput.init returned; calling GXDevice.init (0x11d070)");
    kh_call(g,GS_BASE+0x11d070,HWND_MAIN,app,0,0);        /* BaseSimpleForm.OnShown */
    kh_warn("shell: GXDevice.init returned; parking until ExitApplication");
    const char *lim=getenv("KH_RUN_SECONDS"); long secs=lim?atol(lim):0; struct timespec dl; clock_gettime(CLOCK_REALTIME,&dl); dl.tv_sec+=secs;
    pthread_mutex_lock(&sm);
    while(!exit_requested){ if(secs){ if(pthread_cond_timedwait(&scv,&sm,&dl)!=0){ pthread_mutex_unlock(&sm); kh_warn("shell: KH_RUN_SECONDS=%ld elapsed, ending the run",secs); kh_exit_process(0); } } else pthread_cond_wait(&scv,&sm); }
    pthread_mutex_unlock(&sm);
    kh_warn("shell: ExitApplication requested; ManageRun returns");
    KH_RET(g);
}
void f_579bc0(KCtx *g){                                  /* ExitApplication() */
    pthread_mutex_lock(&sm); exit_requested=1; pthread_cond_broadcast(&scv); pthread_mutex_unlock(&sm); KH_RET(g);
}
void f_579a50(KCtx *g){ KH_RAX(g)=0; KH_RET(g); }          /* Axa.EndMessageClose: quit-confirmation box, auto-accepted */
void f_579ad0(KCtx *g){ KH_RAX(g)=0; KH_RET(g); }          /* MouseEnable */
