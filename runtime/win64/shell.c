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
/* ---- TASK41: the PC's minimise and restore, delivered on this thread (the WinForms UI thread's place) -----------------------------
 * On the PC the form's OnResize (IL method 529, rva 0x389e1c) calls BaseSimpleForm::suspend (IL 547, 0x38696c) when the window is
 * minimised and BaseSimpleForm::resume (IL 548, 0x386994) when it is restored or maximised:
 *   suspend:  if(!suspendFlg) Axa.Sound.Sys.SystemPause(true);  Axa.GX.GXDevice.suspendDevice();  suspendFlg = true;
 *   resume:   if(!forceSuspending){ Axa.GX.GXDevice.resumeDevice(); if(suspendFlg) Axa.Sound.Sys.SystemPause(false); suspendFlg = false; }
 * with the native functions SystemPause = 0x13b2d0 (-> 0x79a60 -> 0x9dc90: every sound of the sd system paused / resumed under its lock
 * 0x142b81cb0), suspendDevice = 0x127eb0 and resumeDevice = 0x125a10 (GX singleton 0x1408a0970, byte +0x308 = 1 / 0).  forceSuspending
 * is only ever set to 0 (the form's constructor).  The app shell's "background" is the PC's minimise (the game is not visible), so
 * runtime/app_life.c posts these two requests at kh_app_suspend / kh_app_resume, and this thread runs exactly the PC's sequence, on a
 * guest thread that is not one of the two game threads, like the UI thread on the PC.  Focus loss alone (OnDeactivate, IL 534) calls
 * nothing of the game: the cursor and a WinForms panel only; its effect on the game is the pad gate of 0x134ff0 (null_window.c). */
#include "khnmc_app.h"
#define SHELL_QMAX 16
static int shell_q[SHELL_QMAX], shell_qn, shell_in_loop, shell_suspend_flg; static u64 shell_posted, shell_minimised, shell_restored, shell_dropped;
/* A layout may arrive before ManageRun parks or while minimised. Keep the latest pair until the form can consume it. */
static u64 pending_size;
void kh_shell_drawable_size(u32 w,u32 h){
    pthread_mutex_lock(&sm);
    pending_size=((u64)w<<32)|h;
    pthread_cond_signal(&scv);
    pthread_mutex_unlock(&sm);
}
void kh_shell_post(int req){
    pthread_mutex_lock(&sm); shell_posted++;
    if(shell_qn<SHELL_QMAX){ shell_q[shell_qn++]=req; pthread_cond_broadcast(&scv); } else shell_dropped++;
    pthread_mutex_unlock(&sm);
    if(shell_dropped) kh_warn("shell: %llu lifecycle request(s) dropped: the queue of %d is full (ManageRun is not serving it)",(unsigned long long)shell_dropped,SHELL_QMAX);
}
int kh_shell_test_take(void){ pthread_mutex_lock(&sm); int r=0; if(shell_qn){ r=shell_q[0]; for(int i=1;i<shell_qn;i++) shell_q[i-1]=shell_q[i]; shell_qn--; } pthread_mutex_unlock(&sm); return r; }   /* selftest */
void kh_shell_counts(u64 *posted,u64 *minimised,u64 *restored,int *serving){ pthread_mutex_lock(&sm); *posted=shell_posted; *minimised=shell_minimised; *restored=shell_restored; *serving=shell_in_loop; pthread_mutex_unlock(&sm); }
/* REVIEW A41 (verification): the PC's minimise and restore are the form's OnResize (IL 529, rva 0x389e1c), not suspend / resume alone:
 *   reason = GXDevice.getResizeReason()   (0x11c900: GX+0x430, bit 0 cleared by the read; GX = 0x1408a0970, the static of 0x11c150)
 *   reason 2: when ClientSize equals the frame buffer (getFrameBufferSize 0x11c0b0 = GX+0x438 / +0x43c, floats) the whole event is
 *             ignored, else reason = 100;
 *   WindowState Minimized: suspend;  Normal / Maximized: resume;
 *   then: reason 100 -> nothing; reason bit 0 -> resizeMainPanel(frame buffer) (a WinForms panel); otherwise, on a restore only,
 *   GXDevice.resize(ClientSize.Width, ClientSize.Height) (IL_022d-IL_028f; native 0x124940 -> 0x124970 with the GX singleton).
 * 0x124970 returns at once when GX+0x428 is set, or when the size equals GX+0x438 / +0x43c and GX+0x416 is 0; otherwise it requests a
 * resize for the timing thread (under the lock GX+0x7ac8: GX+0x400 = 1, GX+0x401 = 1, the size in GX+0x408 / +0x40c, GX+0x404 = 2 when
 * IsZoomed, else 0).  The port runs that sequence for reason 0 (ClientSize = the window seam's GetClientRect answer) and logs which
 * branch 0x124970 takes.  Any other reason is logged and counted; the port then resumes without the resize (it never skips the resume:
 * a game left minimised would stay frozen). */
static u64 shell_reason_other, shell_resize_calls, shell_resize_noop;
void kh_window_client_size(int *w,int *h);
static u32 shell_resize_reason(KCtx *g,const char *when){
    u32 r=(u32)kh_call(g,GS_BASE+0x11c900,0,0,0,0);        /* Axa.GX.GXDevice.getResizeReason() */
    if(r){ pthread_mutex_lock(&sm); shell_reason_other++; pthread_mutex_unlock(&sm);
        kh_warn("shell: %s: GXDevice.getResizeReason() = %u: the PC's OnResize would %s; the port does what reason 0 does without the resize (counted)",when,r,
                r==2?"compare ClientSize with the frame buffer (equal: ignore the event, else no resize)":r==100?"not resize":(r&1)?"call resizeMainPanel (a WinForms panel)":"resize as for reason 0"); }
    return r; }
static void shell_restore_resize(KCtx *g,u32 reason){
    if(reason) return;                                     /* logged by shell_resize_reason */
    int cw=0,ch=0; kh_window_client_size(&cw,&ch); u64 gx=GS_BASE+0x8a0970;
    u32 m428=rd32(g,gx+0x428), a=rd32(g,gx+0x438), b=rd32(g,gx+0x43c); u8 f416=rd8(g,gx+0x416); float fw,fh; memcpy(&fw,&a,4); memcpy(&fh,&b,4);
    int noop=m428!=0||(fw==(float)cw&&fh==(float)ch&&!f416);
    kh_call(g,GS_BASE+0x124940,(u64)(u32)cw,(u64)(u32)ch,0,0);   /* Axa.GX.GXDevice.resize(ClientSize.Width, ClientSize.Height) */
    pthread_mutex_lock(&sm); shell_resize_calls++; if(noop) shell_resize_noop++; pthread_mutex_unlock(&sm);
    kh_warn("shell: restored: GXDevice.resize(%dx%d) as the PC's OnResize: %s (frame buffer %.0fx%.0f, GX+0x428 = %u, GX+0x416 = %u)",cw,ch,
            m428?"returned at once (GX+0x428 is set)":noop?"returned at once (the frame buffer already has that size)":"a resize was requested (GX+0x400 set)",fw,fh,m428,f416);
}
void kh_shell_resize_counts(u64 *calls,u64 *noop,u64 *other_reason){ pthread_mutex_lock(&sm); *calls=shell_resize_calls; *noop=shell_resize_noop; *other_reason=shell_reason_other; pthread_mutex_unlock(&sm); }
static void shell_run(KCtx *g,int req){
    if(req==KH_SHELL_MINIMISE){
        (void)shell_resize_reason(g,"minimise");          /* OnResize reads it first (and clears its bit 0) */
        int was=shell_suspend_flg;
        if(!was) kh_call(g,GS_BASE+0x13b2d0,1,0,0,0);     /* Axa.Sound.Sys.SystemPause(true) */
        kh_call(g,GS_BASE+0x127eb0,0,0,0,0);              /* Axa.GX.GXDevice.suspendDevice() */
        shell_suspend_flg=1; pthread_mutex_lock(&sm); shell_minimised++; pthread_mutex_unlock(&sm);
        kh_warn("shell: minimised, as the PC's BaseSimpleForm::suspend: %sGXDevice.suspendDevice",was?"(sound already paused) ":"SystemPause(true), ");
    } else if(req==KH_SHELL_RESTORE){
        u32 reason=shell_resize_reason(g,"restore");
        kh_call(g,GS_BASE+0x125a10,0,0,0,0);              /* Axa.GX.GXDevice.resumeDevice() */
        int was=shell_suspend_flg; if(was) kh_call(g,GS_BASE+0x13b2d0,0,0,0,0);   /* Axa.Sound.Sys.SystemPause(false) */
        shell_suspend_flg=0; pthread_mutex_lock(&sm); shell_restored++; pthread_mutex_unlock(&sm);
        kh_warn("shell: restored, as the PC's BaseSimpleForm::resume: GXDevice.resumeDevice%s",was?", SystemPause(false)":" (the sound was not paused)");
        shell_restore_resize(g,reason);                    /* then OnResize's GXDevice.resize(ClientSize) */
    } else kh_fatal(g,"shell-request-unknown: lifecycle request %d",req);
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
    kh_vclock_idle(1);                                     /* TASK28: parked, idle for the frames clock's quiescence test */
    pthread_mutex_lock(&sm); shell_in_loop=1;
    while(!exit_requested){
        if(shell_qn){ int req=shell_q[0]; for(int i=1;i<shell_qn;i++) shell_q[i-1]=shell_q[i]; shell_qn--;   /* TASK41: in order */
            pthread_mutex_unlock(&sm); kh_vclock_idle(0); shell_run(g,req); kh_vclock_idle(1); pthread_mutex_lock(&sm); continue; }
        if(pending_size&&!shell_suspend_flg){
            u64 size=pending_size; u32 w=size>>32,h=(u32)size; pending_size=0;
            pthread_mutex_unlock(&sm);
            extern int kh_window_apply_size(u32,u32);
            if(kh_window_apply_size(w,h)){
                kh_vclock_idle(0);
                kh_warn("shell: WM_SIZE SIZE_MAXIMIZED %ux%u",w,h);
                kh_call(g,GS_BASE+0x10eb40,HWND_MAIN,5,2,h<<16|w);  /* packed client rectangle, as delivered to the PC child */
                kh_vclock_idle(1);
            }
            pthread_mutex_lock(&sm); continue;
        }
        if(secs){ if(pthread_cond_timedwait(&scv,&sm,&dl)!=0){ pthread_mutex_unlock(&sm); kh_warn("shell: KH_RUN_SECONDS=%ld elapsed, ending the run",secs); kh_exit_process(0); } } else pthread_cond_wait(&scv,&sm); }
    shell_in_loop=0; pthread_mutex_unlock(&sm); kh_vclock_idle(0);
    kh_warn("shell: ExitApplication requested; ManageRun returns");
    KH_RET(g);
}
void f_579bc0(KCtx *g){                                  /* ExitApplication() */
    pthread_mutex_lock(&sm); exit_requested=1; pthread_cond_broadcast(&scv); pthread_mutex_unlock(&sm); KH_RET(g);
}
void f_579a50(KCtx *g){ KH_RAX(g)=0; KH_RET(g); }          /* Axa.EndMessageClose: quit-confirmation box, auto-accepted */
void f_579ad0(KCtx *g){ KH_RAX(g)=0; KH_RET(g); }          /* MouseEnable */
