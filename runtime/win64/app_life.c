#include "savestate.h"
/* app_life.c : the app's life cycle (TASK41; IOS-GRAPHICS-PLAN.md section 4 step 11, docs/IOS-SHELL.md "Suspend and resume").  The API
 * and what each call does are in khnmc_app.h.  This file holds:
 *   1. the focus flag the window seam reads (null_window.c GetForegroundWindow);
 *   2. the LIFE WORKER: one host thread (made at the first suspend or resume, never otherwise) that runs the suspend and resume steps
 *      in the order the shell asked for them.  The shell's thread only posts a request and waits for it in slices, at most its bound;
 *      it never calls a vkd3d queue function and never takes a lock that is held across a Vulkan or vkd3d call;
 *   3. the Linux test triggers KHNMC_TEST_GATE and KHNMC_TEST_SUSPEND;
 *   4. the step-12 call point (TASK43: the disk pipeline cache's save, runtime/gfx_pipecache.c), the exit report and the selftest.
 *
 * THE PC'S FOCUS LOSS (TASK41 item 2; evidence from the exe and the IL of the mixed-mode image, scratch/S4-il-boot-path/il/):
 *   - The native window procedure 0x10eb40 handles WM_DESTROY (2) and WM_SIZE (5) only; WM_ACTIVATE, WM_ACTIVATEAPP, WM_KILLFOCUS and
 *     every other message go to DefWindowProcA (0x10eb5a-0x10eb68).  The WinForms form: OnActivated (IL 533) hides panel1;
 *     OnDeactivate (IL 534) un-clips and shows the cursor and shows panel1; WndProc (IL 524) handles WM_SIZING (0x214) and
 *     WM_DEVICECHANGE (0x219: DXInput.deviceChanged) only.  So focus loss opens no pause menu, mutes nothing and stops no clock.
 *   - Its one effect on the game: the input update 0x105810 writes every PADDATA neutral each frame (buttons 0, sticks 0x80) and then
 *     calls 0x134ff0 per pad, which reads the pad only when GetForegroundWindow() is the game's window [0x1408bbcb8]
 *     (0x13501c-0x135025).  Without the focus the game runs on and sees neutral pads.  kh_app_will_resign_active does exactly that.
 *   - MINIMISE is different (the PC's equivalent of the app leaving the screen): the form's OnResize (IL 529, WindowState 1) calls
 *     BaseSimpleForm::suspend (IL 547): SystemPause(true) (0x13b2d0 -> 0x79a60 -> 0x9dc90: every sound of the sd system paused) and
 *     GXDevice.suspendDevice (0x127eb0: GX+0x308 = 1; the native WM_SIZE SIZE_MINIMIZED at 0x10ebb6 sets the same byte).  The timing
 *     thread 0x1287d0 then signals its queue and waits for the GPU (0x128e10-0x128ead), and loops Sleep(10) while GX+0x308 is set
 *     (0x128ec0-0x128efb; SteamAPI_RunCallbacks every 61 rounds): it releases no frame, the game is frozen.  Restore: resume (IL 548):
 *     resumeDevice (0x125a10: GX+0x308 = 0), SystemPause(false).  kh_app_suspend / kh_app_resume deliver those two on the game's UI
 *     thread (runtime/shell.c), so a trip to the background pauses and resumes the game's sounds and its timing as on the PC. */
#include "khnmc_rt.h"
#include "khnmc_app.h"
#include "khnmc_audio_out.h"
#include "gfx_present.h"
#include "gfx_pipecache.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>

#define P_(x) ((unsigned long long)(x))
extern int kh_gfx_vkd3d_on;
extern int kh_run_is_selftest;
extern _Atomic u64 gfx_presents;
int kh_app_game_presented(void){ return atomic_load(&gfx_presents)>0; }
uint64_t kh_app_host_ns(void){ return kh_host_ns(); }

/* ---- 1. focus ---- */
static _Atomic int life_inactive, life_used;
static _Atomic u64 c_fg_other;
int  kh_app_window_active(void){ return !atomic_load_explicit(&life_inactive,memory_order_relaxed); }
void kh_app_note_foreground_query(void){ atomic_fetch_add_explicit(&c_fg_other,1,memory_order_relaxed); }

/* ---- 2. the life worker ---- */
#define LIFE_SLICE_MS 50u                         /* every bound of this file is counted in slices of this length (a stop costs one) */
#define LIFE_DRAIN_MARGIN_MS 10u                  /* a suspend's drain starts only with at least this much of its caller's bound left (review A41) */
enum { RQ_SUSPEND=1, RQ_RESUME=2, RQ_AU_BEGAN=3, RQ_AU_ENDED=4 };
enum { ST_ACTIVE, ST_RESIGNED, ST_SUSPENDED };
/* deadline_ns: the earliest host time at which the caller can give up waiting (its post time + its bound in whole slices; a slice ends
 * only after LIFE_SLICE_MS of real time).  Before it the caller is certainly still inside its call (review A41, verification). */
typedef struct { int kind; unsigned bound_ms; u64 seq, epoch, deadline_ns; } LifeReq;
#define LQ 16
static pthread_mutex_t lm=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t l_cv, l_done_cv, l_susp_cv; static pthread_once_t l_once=PTHREAD_ONCE_INIT;
static LifeReq lq[LQ]; static int lqn, l_started, l_busy; static u64 l_seq, l_done; static int l_res[LQ];
/* REVIEW A41 (verification): l_epoch moves, and the gate is set, under lm in every resign and suspend; the resume clears the gate under lm only
 * when l_epoch still is the one it was posted with.  So a resume still running on the worker (its audio start can take long: a Bluetooth
 * route) never clears a gate that a later resign or suspend set. */
static _Atomic u64 l_epoch;
static _Atomic int l_state;                       /* ST_*: what the shell said last, in the order of its calls (set under lm) */
static _Atomic int l_suspended;                   /* under lm: from kh_app_suspend to the resume that clears the gate (the null Present parks only then) */
static int cut_committing;
static u64 cut_life_seq; /* guarded by lm; one field cut owns the gate */
int kh_cut_gate_begin(u64 *epoch){
    pthread_mutex_lock(&lm);
    int ok=!l_busy&&!lqn&&atomic_load(&l_state)==ST_ACTIVE&&kh_app_window_active()&&!kh_gfx_gate_is_set();
    if(ok){ cut_life_seq=l_seq; *epoch=atomic_load(&l_epoch); kh_gfx_gate_set(); }
    pthread_mutex_unlock(&lm);
    return ok;
}
/* Keep a lifecycle transition from invalidating a load after it mutates guest RAM. */
int kh_cut_gate_commit(u64 epoch){
    pthread_mutex_lock(&lm);
    int ok=atomic_load(&l_epoch)==epoch&&cut_life_seq==l_seq&&!cut_committing;
    if(ok) cut_committing=1;
    pthread_mutex_unlock(&lm);
    return ok;
}
void kh_cut_gate_committed_end(u64 epoch){
    pthread_mutex_lock(&lm);
    cut_committing=0;
    if(epoch==atomic_load(&l_epoch)&&atomic_load(&l_state)==ST_ACTIVE) kh_gfx_gate_clear();
    if(l_started) pthread_cond_broadcast(&l_cv);
    pthread_mutex_unlock(&lm);
}
int kh_cut_gate_end(u64 epoch){
    pthread_mutex_lock(&lm);
    int ok=atomic_load(&l_epoch)==epoch;
    if(ok) kh_gfx_gate_clear(); /* resign/suspend take ownership by incrementing l_epoch */
    ok=ok&&cut_life_seq==l_seq;
    pthread_mutex_unlock(&lm);
    return ok;
}
static int (*au_start)(void)=kh_audio_out_start; static void (*au_stop)(void)=kh_audio_out_stop; static int (*au_running)(void)=kh_audio_ring_started;
/* worker only.  au_pending: the device was running when a suspend or an audio-session interruption stopped it, and it has not been started
 * again since (review A41, verification: it is only ever SET by a stop and cleared by a start that worked, so a second suspend before
 * a real resume cannot lose it).  au_interrupted: an interruption began and has not ended. */
static int au_pending, au_interrupted, shell_minimised;
static int st_frames_clock=-1;                    /* selftest: stands for kh_vclock_on (-1 = the real one) */
/* counters (worker or lm) */
static _Atomic u64 c_resign, c_suspend, c_resume, c_q_ok, c_q_not, c_drains, c_drain_q, c_drain_ok, c_drain_late, c_drain_failed, c_drain_none,
    c_hold, c_slots_late, c_pipecache, c_au_stop, c_au_none, c_au_start, c_au_fail, c_pads, c_pads_held, c_min_posted, c_res_posted,
    c_bound_exp, c_unpaired, c_superseded, c_gate_by_suspend, c_present_resume, c_worker_ns_max, c_min_skipped,
    c_superseded_late, c_caller_gone, c_au_deferred, c_int_began, c_int_ended, c_int_stop, c_int_start, c_null_parks, c_null_park_ns;

void kh_app_set_audio(int (*start)(void),void (*stop)(void)){ pthread_mutex_lock(&lm); au_start=start?start:kh_audio_out_start; au_stop=stop?stop:kh_audio_out_stop; pthread_mutex_unlock(&lm); }
static void l_init(void){ kh_cond_init(&l_cv); kh_cond_init(&l_done_cv); kh_cond_init(&l_susp_cv); }
static unsigned slices_of(unsigned ms){ unsigned n=ms/LIFE_SLICE_MS; return n?n:1; }
/* caller holds lm: the suspended state ends (the null Present goes on) */
static void l_unsuspend_locked(void){ if(atomic_load(&l_suspended)){ atomic_store(&l_suspended,0); pthread_cond_broadcast(&l_susp_cv); } }

/* REVIEW A41 (verification): the null backend's Present parks only while the app is SUSPENDED (sceneDidEnterBackground to the resume), never at
 * a resign alone.  A resign (Control Center, a call banner) is the PC's focus loss: the game runs on and reads neutral pads (null_window.c);
 * the null app has no GPU work to hold.  The vkd3d backend keeps the plan's gate at the resign ("Review c1/c2" item 2). */
int  kh_app_suspended(void){ return atomic_load_explicit(&l_suspended,memory_order_relaxed); }
void kh_app_null_present_park(void){
    if(__builtin_expect(!atomic_load_explicit(&l_suspended,memory_order_relaxed),1)) return;
    pthread_once(&l_once,l_init);
    pthread_mutex_lock(&lm); u64 t0=kh_host_ns(); int parked=0;
    while(atomic_load(&l_suspended)){ parked=1; pthread_cond_wait(&l_susp_cv,&lm); }     /* lm is never held across a slow step */
    if(parked){ atomic_fetch_add(&c_null_parks,1); atomic_fetch_add(&c_null_park_ns,kh_host_ns()-t0); }
    pthread_mutex_unlock(&lm);
}
uint64_t kh_app_null_parks(void){ return atomic_load(&c_null_parks); }
void kh_app_test_suspended(int on){ pthread_once(&l_once,l_init); pthread_mutex_lock(&lm); if(on) atomic_store(&l_suspended,1); else l_unsuspend_locked(); pthread_mutex_unlock(&lm); }   /* selftest */

/* the device's start after a suspend or an interruption (worker).  A start that fails while an interruption is still going on is not a
 * failure (the session cannot be activated during a call): it is tried again when the interruption ends.  Otherwise a failure is loud and
 * counted, and au_pending stays set: the next resume or interruption end tries again. */
static void au_restart(const char *when,int *res){
    pthread_mutex_lock(&lm); int (*start)(void)=au_start; pthread_mutex_unlock(&lm);
    int rc=start();
    if(!rc){ au_pending=0; atomic_fetch_add(&c_au_start,1); return; }
    if(au_interrupted){ atomic_fetch_add(&c_au_deferred,1);
        fprintf(stderr,"kh: app: %s: the audio device did not start (result %d) while an audio-session interruption is going on: it is started again when the interruption ends (counted)\n",when,rc); return; }
    if(res) *res|=KH_APP_AUDIO_FAILED; atomic_fetch_add(&c_au_fail,1);
    fprintf(stderr,"kh: app: %s: audio-restart-failed: the audio device did NOT start again (result %d): no sound; the mixer keeps its fallback clock; tried again at the next resume (counted)\n",when,rc);
}
/* the audio part of a stop (suspend, interruption): 1 = the device was running and is stopped now */
static int au_stop_now(void){
    pthread_mutex_lock(&lm); int (*running)(void)=au_running; void (*stop)(void)=au_stop; pthread_mutex_unlock(&lm);
    int now=running(); if(now) stop(); au_pending|=now; return now;
}

static int do_suspend(const LifeReq *rq){
    unsigned bound_ms=rq->bound_ms; int res=0; atomic_fetch_add(&c_suspend,1);
    if(!kh_gfx_gate_is_set()){ atomic_fetch_add(&c_gate_by_suspend,1); kh_gfx_gate_set(); }          /* step 1 (normally done by the resign already) */
    atomic_store(&life_inactive,1);
    /* step 2: nothing admitted at the gate any more (a thread inside a submission finishes it; one that arrives parks outside) */
    u64 p0=kh_gfx_gate_parks(); int q=kh_gfx_gate_quiesce_bounded(slices_of(bound_ms*3/10),LIFE_SLICE_MS);
    KhDrainResult dr; memset(&dr,0,sizeof dr); unsigned np=0, left=0; int slots_ok=1;
    if(q!=KH_GATE_QUIESCED){
        /* "Review c1/c2" item 2: still admitted at the deadline: log suspend-not-quiesced, skip the drain steps; a queue may still get
         * work, so CPU fence signals are held from now on (step 4) */
        res|=KH_APP_NOT_QUIESCED; atomic_fetch_add(&c_q_not,1); kh_gfx_gate_hold_fence_signals(1); atomic_fetch_add(&c_hold,1);
        fprintf(stderr,"kh: app: suspend: suspend-not-quiesced: a submission was still admitted after %u ms; the queue drain, the slot fences and the pipeline-cache save are skipped; CPU fence signals held (counted)\n",slices_of(bound_ms*3/10)*LIFE_SLICE_MS);
    } else if(kh_host_ns()+(u64)LIFE_DRAIN_MARGIN_MS*1000000ull>=rq->deadline_ns){
        /* REVIEW A41 (verification): this suspend waited behind another step (a resume's slow audio start) until its caller's bound had passed
         * or had less than LIFE_DRAIN_MARGIN_MS left: sceneDidEnterBackground may have returned, and Metal refuses GPU work from an app in the
         * background.  No drain Signal is made after the caller's return: the drain steps are skipped as for suspend-not-quiesced */
        res|=KH_APP_CALLER_GONE; atomic_fetch_add(&c_caller_gone,1); kh_gfx_gate_hold_fence_signals(1); atomic_fetch_add(&c_hold,1);
        fprintf(stderr,"kh: app: suspend: suspend-caller-gone: the caller's bound (%u ms) had passed, or had less than %u ms left, when the queue drain would start (the request waited behind another step): the queue drain, the slot fences and the pipeline-cache save are skipped; CPU fence signals held (counted)\n",bound_ms,LIFE_DRAIN_MARGIN_MS);
    } else {
        atomic_fetch_add(&c_q_ok,1);
        /* step 3: every registered queue: a runtime fence Signal (the drain's own admission) and a bounded GetCompletedValue poll */
        int ok=kh_gfx_queues_drain(slices_of(bound_ms*3/10),LIFE_SLICE_MS,&dr); atomic_fetch_add(&c_drains,1);
        atomic_fetch_add(&c_drain_q,dr.queues); atomic_fetch_add(&c_drain_ok,dr.drained); atomic_fetch_add(&c_drain_late,dr.late); atomic_fetch_add(&c_drain_failed,dr.failed);
        if(!dr.queues) atomic_fetch_add(&c_drain_none,1);
        if(!ok) res|=KH_APP_DRAIN_LATE;
        /* step 4: a queue may hold ops that vkd3d would submit from the next thread that signals a fence (plan fact 11) */
        if(!ok||dr.waits){ kh_gfx_gate_hold_fence_signals(1); atomic_fetch_add(&c_hold,1); }
        /* step 5: the presenters' submissions done */
        slots_ok=kh_present_quiesce_all_bounded(slices_of(bound_ms*2/10),LIFE_SLICE_MS,&np,&left);
        if(!slots_ok){ res|=KH_APP_SLOTS_LATE; atomic_fetch_add(&c_slots_late,1); }
        /* step 6 (TASK43, plan step 12): the disk pipeline cache is written here, while the game is parked: the gate is set, nothing is
         * admitted, the queues are drained.  Nothing unless a cache file is configured (gfx_pipecache.c: the rule, the bound, the report) */
        atomic_fetch_add(&c_pipecache,1); kh_gfx_pipecache_save_point(rq->deadline_ns);
    }
    /* the PC's minimise (BaseSimpleForm::suspend on the UI thread): the game pauses its sounds and its timing thread.  NOT under
     * KHNMC_CLOCK=frames (a Linux test clock, never on the phone): there the virtual clock moves only inside WaitForVBlank, which the
     * timing thread stops calling while minimised; its Sleep(10) loop (0x128ec0) waits on that same virtual clock, so the game would
     * never come back (seen in the first null-route test of TASK41: frozen at the present of the suspend).  Skipped loudly, counted. */
    if(st_frames_clock>=0?st_frames_clock:kh_vclock_on){ atomic_fetch_add(&c_min_skipped,1); shell_minimised=0;
        fprintf(stderr,"kh: app: suspend: the PC's minimise is NOT delivered under KHNMC_CLOCK=frames (the game's minimised Sleep loop would stop the virtual clock for good); the gate, the drain, audio and pads are as on the phone (counted)\n"); }
    else { kh_shell_post(KH_SHELL_MINIMISE); shell_minimised=1; atomic_fetch_add(&c_min_posted,1); }
    /* step 7: audio stop, every pad neutral */
    int au_now=au_stop_now(); if(au_now) atomic_fetch_add(&c_au_stop,1); else atomic_fetch_add(&c_au_none,1);
    unsigned held=kh_input_host_pad_neutral(); atomic_fetch_add(&c_pads,1); if(held) atomic_fetch_add(&c_pads_held,1);
    fprintf(stderr,"kh: app: suspend: gate %s (%llu park(s) since its quiesce began), %u queue(s) registered: %u drained, %u late, %u failed (%u slice(s)); presenters %u, slots still pending %u; "
                   "audio %s; pads neutral%s; the PC's minimise %s; result 0x%x\n",
            q==KH_GATE_QUIESCED?"quiesced":"NOT quiesced",P_(kh_gfx_gate_parks()-p0),dr.queues,dr.drained,dr.late,dr.failed,dr.slices,np,left,
            au_now?"stopped":au_pending?"already stopped (its restart is pending)":"was not running (nothing to stop)",held?" (something was held)":"",shell_minimised?"posted":"NOT delivered (KHNMC_CLOCK=frames)",res);
    return res;
}
static int do_resume(const LifeReq *rq){
    int res=0, restored=0; atomic_fetch_add(&c_resume,1);
    if(atomic_load(&l_epoch)!=rq->epoch){                 /* the shell resigned or suspended again after it asked for this resume */
        atomic_fetch_add(&c_superseded,1);
        fprintf(stderr,"kh: app: resume superseded by a later resign / suspend: the gate stays set (counted)\n"); return 0; }
    int was=atomic_load(&l_state)==ST_ACTIVE&&!kh_gfx_gate_is_set()&&kh_app_window_active();
    if(was){ res|=KH_APP_NOT_SUSPENDED; atomic_fetch_add(&c_unpaired,1); }
    kh_gfx_gate_hold_fence_signals(0);                    /* decided again at the next suspend (a queue Wait seen keeps signals held by itself) */
    kh_present_resume_all(); atomic_fetch_add(&c_present_resume,1);     /* only OUR swapchains; the game sees nothing */
    if(shell_minimised){ kh_shell_post(KH_SHELL_RESTORE); shell_minimised=0; restored=1; atomic_fetch_add(&c_res_posted,1); }
    int au_tried=au_pending; if(au_pending) au_restart("resume",&res);
    /* REVIEW A41 (verification): the steps above can take long (the audio start).  The gate and the focus go back only if no resign or suspend
     * came meanwhile: checked and done under lm, where every resign and suspend moves l_epoch and sets the gate */
    pthread_mutex_lock(&lm);
    int late=atomic_load(&l_epoch)!=rq->epoch;
    if(!late){ atomic_store(&life_inactive,0); l_unsuspend_locked(); kh_gfx_gate_clear(); }
    pthread_mutex_unlock(&lm);
    if(late){ atomic_fetch_add(&c_superseded_late,1);
        fprintf(stderr,"kh: app: resume superseded DURING its steps by a later resign / suspend: the gate that one set stays set, the focus stays away (counted)\n"); return res; }
    fprintf(stderr,"kh: app: resume: presenters marked stale, %s, audio %s, focus back, gate cleared; result 0x%x\n",
            (res&KH_APP_NOT_SUSPENDED)?"nothing was suspended (counted)":restored?"the PC's restore posted":"no restore (it was a resign only)",
            !au_tried?"as before (not stopped)":au_pending?"NOT restarted (pending)":"restarted",res);
    return res;
}
/* REVIEW A41 (verification): audio-session interruptions run here too, so the main thread never takes the CoreAudio lock or activates the
 * session (AVAudioSession setActive: is synchronous; the worker may hold kh_ca_m inside a slow start) */
static void do_audio_interruption(int began){
    if(began){ atomic_fetch_add(&c_int_began,1); au_interrupted=1; int now=au_stop_now(); if(now) atomic_fetch_add(&c_int_stop,1);
        fprintf(stderr,"kh: app: audio-session interruption began: the audio device %s\n",now?"is stopped (started again when it ends, or at the resume)":au_pending?"was already stopped (its restart is pending)":"was not running");
        return; }
    atomic_fetch_add(&c_int_ended,1); au_interrupted=0;
    if(au_pending&&!atomic_load(&l_suspended)){ u64 s0=atomic_load(&c_au_start); au_restart("audio-session interruption ended",NULL);
        if(atomic_load(&c_au_start)>s0){ atomic_fetch_add(&c_int_start,1); fprintf(stderr,"kh: app: audio-session interruption ended: the audio device started again\n"); } }
    else fprintf(stderr,"kh: app: audio-session interruption ended: %s\n",au_pending?"the app is suspended: the device starts at the resume":"nothing to restart");
}
static void *life_worker(void *a){
    (void)a; pthread_mutex_lock(&lm);
    for(;;){
        while(!lqn||cut_committing) pthread_cond_wait(&l_cv,&lm);
        LifeReq rq=lq[0]; for(int i=1;i<lqn;i++) lq[i-1]=lq[i]; lqn--; l_busy=1;
        pthread_mutex_unlock(&lm);
        u64 t0=kh_host_ns(); int r=0;
        if(rq.kind==RQ_SUSPEND) r=do_suspend(&rq); else if(rq.kind==RQ_RESUME) r=do_resume(&rq); else do_audio_interruption(rq.kind==RQ_AU_BEGAN);
        u64 dt=kh_host_ns()-t0;
        if(dt>atomic_load(&c_worker_ns_max)) atomic_store(&c_worker_ns_max,dt);
        pthread_mutex_lock(&lm); l_res[rq.seq%LQ]=r; l_done=rq.seq; l_busy=0; pthread_cond_broadcast(&l_done_cv);
    }
    return NULL;
}
/* caller holds lm: the worker exists, the request is queued; its sequence number */
static u64 l_post_locked(int kind,unsigned bound_ms){
    if(!l_started){ pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
        if(pthread_create(&th,&at,life_worker,NULL)){ pthread_mutex_unlock(&lm); kh_fatal(NULL,"app-life-worker: the life worker thread cannot be started"); }
        pthread_attr_destroy(&at); l_started=1; }
    if(lqn>=LQ){ pthread_mutex_unlock(&lm); kh_fatal(NULL,"app-life-queue-full: %d life-cycle requests wait for the worker (it has not finished one for %d calls)",LQ,LQ); }
    u64 my=++l_seq; LifeReq *q=&lq[lqn++]; q->kind=kind; q->bound_ms=bound_ms; q->seq=my; q->epoch=atomic_load(&l_epoch);
    q->deadline_ns=kh_host_ns()+(u64)slices_of(bound_ms)*LIFE_SLICE_MS*1000000ull; pthread_cond_broadcast(&l_cv);
    return my;
}
/* post a request and wait for it, at most bound_ms in slices of LIFE_SLICE_MS (no other lock held; the worker never waits for us) */
static int post_wait(int kind,unsigned bound_ms,const char *what){
    pthread_once(&l_once,l_init); atomic_store(&life_used,1);
    pthread_mutex_lock(&lm);
    if(kind==RQ_RESUME) atomic_store(&l_state,ST_ACTIVE);  /* review A41 (verification): in call order, not after the wait (a later call on another thread wins) */
    u64 my=l_post_locked(kind,bound_ms);
    unsigned want=slices_of(bound_ms), expired=0;
    while(l_done<my){ struct timespec dl; kh_mono_deadline_ms(&dl,LIFE_SLICE_MS);
        if(kh_cond_wait_until(&l_done_cv,&lm,&dl)==ETIMEDOUT&&l_done<my&&++expired>=want) break; }
    int r=l_done>=my?l_res[my%LQ]:KH_APP_BOUND_EXPIRED; int queued=lqn, busy=l_busy;
    pthread_mutex_unlock(&lm);
    if(r==KH_APP_BOUND_EXPIRED){ atomic_fetch_add(&c_bound_exp,1);
        fprintf(stderr,"kh: app: %s: BOUND EXPIRED: the life worker had not finished after %u slice(s) of %u ms (%s, %d request(s) queued); it goes on, the gate stays set until a resume completes (counted)\n",
                what,want,LIFE_SLICE_MS,busy?"it is inside a step":"idle",queued); }
    return r;
}
/* `env KHNMC_APP_LIFE=off` (khnmc.conf): the shell's calls are counted and do nothing (a switch for the owner if the life cycle misbehaves
 * on the phone; the app then behaves as before TASK41 when it leaves the screen) */
static _Atomic u64 c_off;
static int life_off(void){ const char *e=getenv("KHNMC_APP_LIFE"); if(e&&!strcmp(e,"off")){ atomic_store(&life_used,1); if(!atomic_fetch_add(&c_off,1)) fprintf(stderr,"kh: app: KHNMC_APP_LIFE=off: the shell's life-cycle calls are ignored (counted)\n"); return 1; }
    if(e&&*e&&strcmp(e,"on")) kh_fatal(NULL,"app-life-value: KHNMC_APP_LIFE=%s (on, the default, or off)",e); return 0; }
void kh_app_will_resign_active(void){
    if(life_off()) return;
    atomic_store(&life_used,1); atomic_fetch_add(&c_resign,1);
    pthread_mutex_lock(&lm);                               /* review A41 (verification): with the resume's check, under lm (held only briefly anywhere) */
    atomic_fetch_add(&l_epoch,1); kh_gfx_gate_set(); atomic_store(&life_inactive,1);
    if(atomic_load(&l_state)==ST_ACTIVE) atomic_store(&l_state,ST_RESIGNED);
    pthread_mutex_unlock(&lm);
}
int kh_app_suspend(unsigned bound_ms){
    if(life_off()) return 0;
    pthread_once(&l_once,l_init);
    pthread_mutex_lock(&lm);                               /* at once, on the caller's thread (no wait for the worker) */
    atomic_fetch_add(&l_epoch,1); atomic_store(&l_state,ST_SUSPENDED);
    kh_gfx_gate_set(); atomic_store(&life_inactive,1); atomic_store(&l_suspended,1);
    pthread_mutex_unlock(&lm);
    return post_wait(RQ_SUSPEND,bound_ms,"suspend");
}
int kh_app_resume(unsigned bound_ms){
    if(life_off()) return 0;
    return post_wait(RQ_RESUME,bound_ms,"resume");
}
/* review A41 (verification): an audio-session interruption began (1) or ended (0).  Posted to the life worker; returns at once (never waits) */
void kh_app_audio_interruption(int began){
    if(life_off()) return;
    pthread_once(&l_once,l_init); atomic_store(&life_used,1);
    pthread_mutex_lock(&lm); l_post_locked(began?RQ_AU_BEGAN:RQ_AU_ENDED,0); pthread_mutex_unlock(&lm);
}

/* ---- 3. test triggers ---- */
static int t_kind, t_at=-1; static u64 t_frame; static unsigned t_ms; static _Atomic int t_state; static _Atomic u64 t_at_present, t_r1, t_r2, t_held_ns;
/* "frame:N:ms" -> 0 ok; 1 malformed; 2 out of range */
static int test_parse(const char *s,u64 *n,unsigned *ms){
    if(!s||strncmp(s,"frame:",6)) return 1; s+=6; char *e; if(*s<'0'||*s>'9') return 1;
    unsigned long long a=strtoull(s,&e,10); if(*e!=':'||e[1]<'0'||e[1]>'9') return 1; unsigned long long b=strtoull(e+1,&e,10); if(*e) return 1;
    if(a<1||a>100000000ull||b>3600000ull) return 2; *n=a; *ms=(unsigned)b; return 0; }
/* REVIEW A41 (verification): both triggers ASSERT what they are meant to show; a check that fails is the named fatal app-test-gate-failed /
 * app-test-suspend-failed (the route then FAILs).  The gate trigger runs a stuck-budget waiter behind the parked game: a host thread that
 * waits, with kh_cond_wait_stuck and the runtime's longest stuck budget (30 slices: bgload.c BG_LOCK_WAIT_MAX_S, vclock.c
 * VC_CLIENT_WAIT_S), for the game to make two more Presents; with the gate held longer than that budget it fires unless slices under the
 * gate are not counted (kh_compat.h). */
#define T_WAIT_BUDGET 30u
extern void kh_gfx_submit_counts(u64 *total,u64 *set_admitted,u64 *set_outside);   /* gfx_vkd3d.c (gipa wrapper; 0 without the backend) */
extern int kh_gfx_wrap_on;
static pthread_mutex_t tw_m=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t tw_cv; static u64 tw_want; static _Atomic int tw_rc=-1;
static void *test_gate_waiter(void *a){
    (void)a; unsigned ex=0; int rc=0; pthread_mutex_lock(&tw_m);
    while(atomic_load(&gfx_presents)<tw_want){ rc=kh_cond_wait_stuck(&tw_cv,&tw_m,&ex,T_WAIT_BUDGET); if(rc==ETIMEDOUT) break; }
    pthread_mutex_unlock(&tw_m); atomic_store(&tw_rc,rc); return NULL; }
static void test_sleep_ms(unsigned ms){ struct timespec ts={(time_t)(ms/1000),(long)(ms%1000)*1000000L}; while(nanosleep(&ts,&ts)==-1&&errno==EINTR){} }
/* TASK43 (TASK43.md item 6): ONE suspend / resume cycle as the shell's main thread makes it, with its checks: the KHNMC_TEST_SUSPEND
 * trigger and the presenter selftest (gfx_present.c) both run exactly this.
 *   at = -1                      the cycle starts wherever the game happens to be (the trigger without KHNMC_TEST_SUSPEND_AT);
 *   at = KH_TP_BEFORE_ACQUIRE    a Present is first stopped at the top of the presenter, before its vkAcquireNextImageKHR; the resign
 *                                (the gate) comes while it stands there; then it goes on and parks BEFORE the acquire;
 *   at = KH_TP_AFTER_ACQUIRE     the same right after a successful acquire: the Present then finds the gate at its check (e) and parks
 *                                AFTER the acquire (park_after_acquire: on the way back the acquired image is consumed by an admitted
 *                                submit, its semaphore is made anew, the swapchain is rebuilt as after-park).
 * Both interleavings are correct paths.  The trigger used to fail the second one ("a Present parked after its acquire": seen once under
 * load at task 41's merge) because it could not tell the park's rebuild from the resume's.  Now it verifies each: in both, a presenter
 * must have SEEN the resume (kh_present_resume_seen: the generation kh_present_resume_all moves, read at a Present; that is what the
 * a12b mutant removes) and a swapchain rebuild must have followed; after an acquire, also every parked acquire consumed and its
 * semaphore made anew.  Returns the number of failed checks (their text in why). */
int kh_app_test_suspend_cycle(int at,unsigned ms,char *why,size_t whycap,uint64_t *r1o,uint64_t *r2o){
    int k=0, nf=0; if(whycap) why[0]=0; u64 t0=kh_host_ns();
#define TFAIL(c,...) do{ if(!(c)){ nf++; if(k<(int)whycap-200){ int w_=snprintf(why+k,whycap-(size_t)k,__VA_ARGS__); if(w_>0) k+=w_; } } }while(0)
    const char *atn=at<0?"wherever the game is":at==KH_TP_AFTER_ACQUIRE?"FORCED after the presenter's acquire":"FORCED before the presenter's acquire";
    fprintf(stderr,"kh: app: TEST KHNMC_TEST_SUSPEND: present %llu: resign, suspend (bound %u ms), %u ms in the background, resume, from a host thread that stands for the shell's main thread (%s)\n",
            P_(atomic_load(&gfx_presents)),KH_APP_SUSPEND_BOUND_MS,ms,atn);
    u64 pres0=kh_present_presented(), rb0=kh_present_rebuilds_after_resume(), pk0=kh_present_parked_after_acquire(), rc0=kh_present_resume_calls(),
        rs0=kh_present_resume_seen(), cn0=kh_present_consumed(), sr0=kh_present_sems_remade();
    u64 dq0=atomic_load(&c_drain_q), dok0=atomic_load(&c_drain_ok), pads0=atomic_load(&c_pads), aus0=atomic_load(&c_au_stop), aua0=atomic_load(&c_au_start), np0=atomic_load(&c_null_parks);
    int reached=1;
    if(at>=0){ kh_present_test_point_arm(at); reached=kh_present_test_point_wait(120000);
        if(!reached){ kh_present_test_point_arm(-1); TFAIL(0,"no Present reached the presenter's test point within 120 s (the forced interleavings need a presenter: KHNMC_GFX_PRESENT=headless); "); } }
    kh_app_will_resign_active();                                       /* the gate: a Present stopped at the point sees it as soon as it goes on */
    if(at>=0&&reached) kh_present_test_point_release();
    int r1=kh_app_suspend(KH_APP_SUSPEND_BOUND_MS); if(r1o) *r1o=(u64)r1;
    test_sleep_ms(ms);
    u64 at_resume=atomic_load(&gfx_presents);
    int r2=kh_app_resume(KH_APP_RESUME_BOUND_MS); if(r2o) *r2o=(u64)r2;
    fprintf(stderr,"kh: app: TEST KHNMC_TEST_SUSPEND: suspend result 0x%x, resume result 0x%x, %.1f ms from the resign to the end of the resume (the game was at present %llu)\n",
            r1,r2,(kh_host_ns()-t0)/1e6,P_(at_resume));
    /* the swapchain-stale path: with a presenter, the first Presents after the resume rebuild OUR swapchain */
    int with_pres=pres0>0||at>=0; u64 want=at_resume+2;
    for(unsigned w=0;w<6000&&atomic_load(&gfx_presents)<want;w++){ struct timespec ts={0,10000000}; nanosleep(&ts,0); }
    unsigned allowed=(kh_gfx_vkd3d_on&&!kh_gfx_wrap_on)?KH_APP_DRAIN_LATE:0;    /* the trampoline bridge registers no queue (by design, loud) */
    u64 pk=kh_present_parked_after_acquire()-pk0, rb=kh_present_rebuilds_after_resume()-rb0, rs=kh_present_resume_seen()-rs0, cn=kh_present_consumed()-cn0, sr=kh_present_sems_remade()-sr0;
    TFAIL((((unsigned)r1)&~allowed)==0,"suspend result 0x%x; ",r1);
    TFAIL(r2==0,"resume result 0x%x; ",r2);
    TFAIL(atomic_load(&c_pads)>pads0,"the pads were not set neutral; ");
    TFAIL(kh_present_resume_calls()>rc0,"the presenters were not told to rebuild (kh_present_resume_all); ");
    if(atomic_load(&c_au_stop)>aus0) TFAIL(atomic_load(&c_au_start)>aua0,"the audio stopped at the suspend did not start again; ");
    if(kh_gfx_vkd3d_on&&kh_gfx_wrap_on) TFAIL(atomic_load(&c_drain_q)>dq0&&atomic_load(&c_drain_ok)-dok0==atomic_load(&c_drain_q)-dq0,"the queue drain did not reach every registered queue; ");
    if(!kh_gfx_vkd3d_on) TFAIL(atomic_load(&c_null_parks)>np0,"the null backend's Present did not park while suspended; ");
    TFAIL(atomic_load(&gfx_presents)>=want,"the game did not present twice after the resume; ");
    if(with_pres){
        TFAIL(rs>0,"no presenter saw the resume at a Present (kh_present_resume_all did not reach it); ");
        TFAIL(rb>0,"no swapchain rebuild after the resume (the stale path); ");
        /* a Present that parked after its acquire is a correct path: verified by its own counters instead of failed */
        if(pk){ TFAIL(cn>=pk,"%llu Present(s) parked after their acquire, but only %llu parked acquire(s) were consumed; ",P_(pk),P_(cn));
                TFAIL(sr>=pk,"%llu Present(s) parked after their acquire, but only %llu acquire semaphore(s) were made anew; ",P_(pk),P_(sr)); }
        if(at==KH_TP_AFTER_ACQUIRE) TFAIL(pk>=1,"the suspend was forced after an acquire, but no Present parked after its acquire; ");
        if(at==KH_TP_BEFORE_ACQUIRE) TFAIL(pk==0,"the suspend was forced before the acquire, but %llu Present(s) parked after an acquire; ",P_(pk)); }
    fprintf(stderr,"kh: app: TEST KHNMC_TEST_SUSPEND: checks (%s): presenter %s, saw the resume +%llu, rebuilds after resume / park +%llu, parked after acquire +%llu (acquires consumed +%llu, semaphores made anew +%llu), queues drained %llu of %llu, pads neutral +%llu, audio stopped +%llu / started +%llu, null Present parks +%llu: %s\n",
            atn,with_pres?"yes":"none",P_(rs),P_(rb),P_(pk),P_(cn),P_(sr),P_(atomic_load(&c_drain_ok)-dok0),P_(atomic_load(&c_drain_q)-dq0),
            P_(atomic_load(&c_pads)-pads0),P_(atomic_load(&c_au_stop)-aus0),P_(atomic_load(&c_au_start)-aua0),P_(atomic_load(&c_null_parks)-np0),nf?"FAILED":"ok");
#undef TFAIL
    return nf;
}
int kh_app_test_used(int set){ int was=atomic_load(&life_used); if(set>=0) atomic_store(&life_used,set); return was; }   /* selftests: the report's "was used" flag, saved and put back */
static void *test_main(void *a){
    (void)a; while(atomic_load(&gfx_presents)<t_frame){ struct timespec ts={0,2000000}; nanosleep(&ts,0); }
    atomic_store(&t_at_present,atomic_load(&gfx_presents)); atomic_store(&t_state,1);
    u64 t0=kh_host_ns(); char why[400]; int k=0; why[0]=0;
#define TFAIL(c,...) do{ if(!(c)&&k<(int)sizeof why-120) k+=snprintf(why+k,sizeof why-(size_t)k,__VA_ARGS__); }while(0)
    if(t_kind==1){
        fprintf(stderr,"kh: app: TEST KHNMC_TEST_GATE: present %llu: the suspend gate is set and held %u ms of host time; a stuck-budget waiter (%u slices) waits for two more Presents\n",P_(atomic_load(&t_at_present)),t_ms,T_WAIT_BUDGET);
        u64 s0,a0,o0; kh_gfx_submit_counts(&s0,&a0,&o0); u64 gq0=kh_gfx_gate_parks_at("game queue:"), gp0=kh_gfx_gate_parks();
        unsigned long long fr0=__atomic_load_n(&kh_stuck_gate_free,__ATOMIC_RELAXED);
        kh_gfx_gate_set(); tw_want=atomic_load(&gfx_presents)+2; pthread_t tw; int tw_ok=pthread_create(&tw,NULL,test_gate_waiter,NULL)==0;
        test_sleep_ms(t_ms);
        u64 s1,a1,o1; kh_gfx_submit_counts(&s1,&a1,&o1); u64 gq1=kh_gfx_gate_parks_at("game queue:"), gp1=kh_gfx_gate_parks(), at_clear=atomic_load(&gfx_presents);
        kh_gfx_gate_clear(); atomic_store(&t_held_ns,kh_host_ns()-t0);
        fprintf(stderr,"kh: app: TEST KHNMC_TEST_GATE: gate cleared after %.1f ms (the game was at present %llu)\n",(kh_host_ns()-t0)/1e6,P_(at_clear));
        if(tw_ok) pthread_join(tw,NULL);
        unsigned long long fr1=__atomic_load_n(&kh_stuck_gate_free,__ATOMIC_RELAXED);
        TFAIL(tw_ok,"the waiter thread did not start; ");
        TFAIL(atomic_load(&tw_rc)!=ETIMEDOUT,"a %u-slice stuck budget waiting for the parked game EXPIRED (slices under the gate were counted); ",T_WAIT_BUDGET);
        TFAIL(fr1>fr0,"no stuck-budget slice was left uncharged under the gate; ");
        if(kh_gfx_vkd3d_on){
            TFAIL(at_clear<=tw_want-1,"the game made %llu Present(s) while the gate was set (at most the one in progress may finish); ",P_(at_clear-(tw_want-2)));
            TFAIL(o1==o0,"%llu vkQueueSubmit(s) by a thread that was NOT admitted while the gate was set; ",P_(o1-o0));
            TFAIL(gp1>gp0,"nothing parked at the gate; ");
            if(kh_gfx_wrap_on) TFAIL(gq1>gq0,"no game queue call (ExecuteCommandLists / Signal / Wait) parked at the gate; "); }
        fprintf(stderr,"kh: app: TEST KHNMC_TEST_GATE: checks: waiter %s, stuck-budget slices not charged under the gate +%llu, parks +%llu (game queue +%llu), vkQueueSubmit under the gate: admitted +%llu, outside +%llu: %s\n",
                atomic_load(&tw_rc)==ETIMEDOUT?"EXPIRED":"saw the game come back",fr1-fr0,P_(gp1-gp0),P_(gq1-gq0),P_(a1-a0),P_(o1-o0),k?"FAILED":"ok");
        if(!kh_gfx_vkd3d_on) fprintf(stderr,"kh: app: TEST KHNMC_TEST_GATE: null backend: the game does not park at a gate set alone (a resign = the PC's focus loss); only the waiter was checked\n");
        if(k) kh_fatal(NULL,"app-test-gate-failed: KHNMC_TEST_GATE=frame:%llu:%u: %s",P_(t_frame),t_ms,why);
    } else {
        char sw[700]; u64 r1=0,r2=0; int nf=kh_app_test_suspend_cycle(t_at,t_ms,sw,sizeof sw,&r1,&r2);
        atomic_store(&t_r1,r1); atomic_store(&t_r2,r2); atomic_store(&t_held_ns,kh_host_ns()-t0);
        if(nf) kh_fatal(NULL,"app-test-suspend-failed: KHNMC_TEST_SUSPEND=frame:%llu:%u%s%s: %s",P_(t_frame),t_ms,t_at>=0?" KHNMC_TEST_SUSPEND_AT=":"",t_at<0?"":t_at==KH_TP_AFTER_ACQUIRE?"after-acquire":"before-acquire",sw);
    }
#undef TFAIL
    atomic_store(&t_state,2); return NULL;
}
void kh_app_test_init(void){
    const char *g=getenv("KHNMC_TEST_GATE"), *s=getenv("KHNMC_TEST_SUSPEND"); if(g&&!*g) g=NULL; if(s&&!*s) s=NULL;
    if(!g&&!s) return;
    if(g&&s) kh_fatal(NULL,"app-test-both: KHNMC_TEST_GATE=%s and KHNMC_TEST_SUSPEND=%s are both set: one test trigger per run",g,s);
    if(kh_run_is_selftest) kh_fatal(NULL,"app-test-in-selftest: %s is set for a --selftest run (the selftest has its own suspend tests)",g?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND");
    const char *v=g?g:s; int pr=test_parse(v,&t_frame,&t_ms);
    if(pr==1) kh_fatal(NULL,"app-test-value: %s=%s: expected frame:N:ms (N = the Present at which it starts, ms = how long the gate / the background lasts)",g?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND",v);
    if(pr==2) kh_fatal(NULL,"app-test-range: %s=%s: N 1..100000000, ms 0..3600000",g?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND",v);
    t_kind=g?1:2; atomic_store(&life_used,1); kh_cond_init(&tw_cv);
    { const char *at=getenv("KHNMC_TEST_SUSPEND_AT");            /* TASK43: force the interleaving (before-acquire / after-acquire); unset = wherever the game is */
      if(at&&*at){ if(!s) kh_fatal(NULL,"app-test-at-needs-suspend: KHNMC_TEST_SUSPEND_AT=%s without KHNMC_TEST_SUSPEND",at);
          if(!strcmp(at,"before-acquire")) t_at=KH_TP_BEFORE_ACQUIRE; else if(!strcmp(at,"after-acquire")) t_at=KH_TP_AFTER_ACQUIRE;
          else kh_fatal(NULL,"app-test-at-value: KHNMC_TEST_SUSPEND_AT=%s (before-acquire or after-acquire: where in the presenter's Present the suspend is forced)",at);
          if(!kh_gfx_vkd3d_on) kh_fatal(NULL,"app-test-at-needs-presenter: KHNMC_TEST_SUSPEND_AT=%s needs the vkd3d backend with a presenter (KHNMC_GFX=vkd3dw KHNMC_GFX_PRESENT=headless)",at); } }
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th,&at,test_main,NULL)) kh_fatal(NULL,"app-test-thread: the test trigger thread cannot be started");
    pthread_attr_destroy(&at);
    fprintf(stderr,"kh: app: TEST %s=%s armed (Present %llu, %u ms)\n",g?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND",v,P_(t_frame),t_ms);
}

/* ---- 4. report ---- */
void kh_app_report(FILE *f){
    if(!atomic_load(&life_used)) return;
    u64 sp=0,sm=0,sr=0,rz=0,rzn=0,rzo=0; int serving=0; kh_shell_counts(&sp,&sm,&sr,&serving); kh_shell_resize_counts(&rz,&rzn,&rzo);
    if(atomic_load(&c_off)) fprintf(f,"kh: app: KHNMC_APP_LIFE=off: %llu life-cycle call(s) of the shell ignored\n",P_(c_off));
    fprintf(f,"kh: app: life cycle (TASK41): resigns %llu, suspends %llu (quiesced %llu, suspend-not-quiesced %llu, suspend-caller-gone %llu, gate set by the suspend itself %llu; queue drains %llu over %llu queue(s): drained %llu, late %llu, failed %llu, none registered %llu; "
              "CPU fence signals held %llu time(s); slot fences late %llu; pipeline-cache save points %llu), resumes %llu (nothing suspended %llu, superseded %llu before / %llu during their steps; presenter rebuilds asked %llu, made after resume %llu, presents %llu), "
              "caller bounds expired %llu, worker step max %.1f ms; audio: stopped %llu, not running %llu, restarted %llu, restart failed %llu, deferred to an interruption's end %llu; "
              "audio-session interruptions began %llu (device stopped %llu) / ended %llu (device restarted %llu); pads set neutral %llu (something held %llu); "
              "null-backend Present parks while suspended %llu (%.1f ms); "
              "GetForegroundWindow answered another window %llu time(s); the PC's minimise / restore posted %llu / %llu, served %llu / %llu (UI thread %s), minimise not delivered under KHNMC_CLOCK=frames %llu; the restore's GXDevice.resize (OnResize) called %llu (returned at once %llu), other resize reasons %llu",
            P_(c_resign),P_(c_suspend),P_(c_q_ok),P_(c_q_not),P_(c_caller_gone),P_(c_gate_by_suspend),P_(c_drains),P_(c_drain_q),P_(c_drain_ok),P_(c_drain_late),P_(c_drain_failed),P_(c_drain_none),
            P_(c_hold),P_(c_slots_late),P_(c_pipecache),P_(c_resume),P_(c_unpaired),P_(c_superseded),P_(c_superseded_late),P_(c_present_resume),P_(kh_present_rebuilds_after_resume()),P_(kh_present_presented()),
            P_(c_bound_exp),atomic_load(&c_worker_ns_max)/1e6,P_(c_au_stop),P_(c_au_none),P_(c_au_start),P_(c_au_fail),P_(c_au_deferred),
            P_(c_int_began),P_(c_int_stop),P_(c_int_ended),P_(c_int_start),P_(c_pads),P_(c_pads_held),
            P_(c_null_parks),atomic_load(&c_null_park_ns)/1e6,
            P_(c_fg_other),P_(c_min_posted),P_(c_res_posted),P_(sm),P_(sr),serving?"serving":"not serving",P_(c_min_skipped),P_(rz),P_(rzn),P_(rzo));
    if(t_kind&&t_at>=0) fprintf(f,"; KHNMC_TEST_SUSPEND_AT=%s",t_at==KH_TP_AFTER_ACQUIRE?"after-acquire":"before-acquire");
    if(t_kind) fprintf(f,"; test %s frame:%llu:%u: %s at present %llu, suspend 0x%llx resume 0x%llx, %.1f ms",t_kind==1?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND",P_(t_frame),t_ms,
                       atomic_load(&t_state)==2?"done":atomic_load(&t_state)==1?"STARTED, NOT FINISHED":"NEVER STARTED (the run ended before that present)",
                       P_(atomic_load(&t_at_present)),P_(atomic_load(&t_r1)),P_(atomic_load(&t_r2)),atomic_load(&t_held_ns)/1e6);
    fputc('\n',f);
    if(t_kind&&atomic_load(&t_state)!=2) fprintf(f,"kh: app: WARNING: test-trigger-unfinished: the %s trigger did not finish in this run (counted)\n",t_kind==1?"KHNMC_TEST_GATE":"KHNMC_TEST_SUSPEND");
    if(!kh_gfx_vkd3d_on) kh_gfx_gate_report(f);           /* the vkd3d backend prints it with its own report */
}

/* ---- 5. selftest (every build: no device; the vkd3d parts of step 11 are tested in gfx_wrap.c / gfx_present.c) ---- */
static _Atomic int st_starts, st_stops, st_start_rc, st_stop_sleep_ms, st_start_sleep_ms, st_running;
/* the stand-in device follows its own start and stop (review A41, verification: a running() that never changed hid the lost restart) */
static int st_start(void){ atomic_fetch_add(&st_starts,1); int ms=atomic_load(&st_start_sleep_ms); if(ms){ struct timespec ts={0,(long)ms*1000000L}; nanosleep(&ts,0); }
    int rc=atomic_load(&st_start_rc); if(!rc) atomic_store(&st_running,1); return rc; }
static void st_stop(void){ atomic_fetch_add(&st_stops,1); int ms=atomic_load(&st_stop_sleep_ms); if(ms){ struct timespec ts={0,(long)ms*1000000L}; nanosleep(&ts,0); } atomic_store(&st_running,0); }
static int st_running_fn(void){ return atomic_load(&st_running); }
/* the worker has nothing queued and is not inside a step (at most ms) */
static int st_wait_idle(unsigned ms){
    pthread_mutex_lock(&lm); u64 t0=kh_host_ns();
    while((lqn||l_busy)&&kh_host_ns()-t0<(u64)ms*1000000ull){ struct timespec dl; kh_mono_deadline_ms(&dl,10); kh_cond_wait_until(&l_done_cv,&lm,&dl); }
    int idle=!lqn&&!l_busy; pthread_mutex_unlock(&lm); return idle; }
typedef struct { _Atomic int state, go; } StAdm;
static void *st_adm(void *a){ StAdm *t=(StAdm*)a; kh_gfx_gate_enter("selftest: app life admitted"); atomic_store(&t->state,2);
    while(!atomic_load(&t->go)){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } kh_gfx_gate_leave(); atomic_store(&t->state,3); return NULL; }
static void *st_go_later(void *a){ StAdm *t=(StAdm*)a; struct timespec ts={0,100000000}; nanosleep(&ts,0); atomic_store(&t->go,1); return NULL; }
typedef struct { int kind, slices; unsigned hold_slices; _Atomic int rc; u64 ns; } StStuck;
static pthread_mutex_t st_m=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t st_cv, st_cv_rt=PTHREAD_COND_INITIALIZER; static kh_tmutex st_tm=KH_TMUTEX_INITIALIZER;
static _Atomic int st_tm_state;                   /* the holder of st_tm: 1 holds it, 2 = let go */
static void *st_tm_holder(void *a){ (void)a; kh_tmutex_lock_stuck(&st_tm,1000); atomic_store(&st_tm_state,1); while(atomic_load(&st_tm_state)!=2){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } kh_tmutex_unlock(&st_tm); return NULL; }
void kh_input_host_pad_set(u16 buttons,u8 lt,u8 rt,s16 lx,s16 ly,s16 rx,s16 ry);
static void *st_gate_hold(void *a){ unsigned n=*(unsigned*)a; struct timespec ts={0,(long)n*kh_stuck_slice_ms*1000000L}; nanosleep(&ts,0); kh_gfx_gate_clear(); return NULL; }
/* one stuck-budget wait of `slices` slices while the gate is set for hold slices: returns the host ns it took (ETIMEDOUT required) */
static u64 st_stuck_once(int kind,unsigned slices,unsigned hold,int *rc){
    u64 t0=kh_host_ns(); unsigned ex=0; *rc=0; pthread_t th=0; int held=hold>0;
    if(held){ kh_gfx_gate_set(); pthread_create(&th,NULL,st_gate_hold,&hold); }
    if(kind==0){ pthread_mutex_lock(&st_m); while(!*rc) *rc=kh_cond_wait_stuck(&st_cv,&st_m,&ex,slices); pthread_mutex_unlock(&st_m); }
    else if(kind==1){ pthread_mutex_lock(&st_m); while(!*rc) *rc=kh_cond_wait_stuck_rt(&st_cv_rt,&st_m,&ex,slices); pthread_mutex_unlock(&st_m); }
    else { *rc=kh_tmutex_lock_stuck(&st_tm,slices); if(!*rc) kh_tmutex_unlock(&st_tm); }
    u64 dt=kh_host_ns()-t0; if(held) pthread_join(th,NULL); return dt; }
int kh_app_life_selftest(void){
    int n=0,f=0;
#define TA(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"app life selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    /* every case starts with the gate clear and nothing admitted (a case that left it set fails here, and the next one still runs
     * instead of parking its own threads forever) */
#define TCLEAN() do{ int set_=kh_gfx_gate_is_set(); TA(!set_); if(set_) kh_gfx_gate_clear(); }while(0)
    pthread_once(&l_once,l_init); kh_cond_init(&st_cv);
    int (*s_start)(void)=au_start; void (*s_stop)(void)=au_stop; int (*s_run)(void)=au_running; int used0=atomic_load(&life_used);
    au_start=st_start; au_stop=st_stop; au_running=st_running_fn; atomic_store(&st_running,1);
    TA(kh_app_window_active() && !kh_gfx_gate_is_set());
    /* the trampoline bridge (KHNMC_GFX=vkd3d, Linux identity builds only) registers no queue: every suspend there reports
     * KH_APP_DRAIN_LATE by design (loud, counted); this selftest then accepts that one bit */
    unsigned st_mask=0; { KhDrainResult dr; kh_gfx_gate_set(); int ok=kh_gfx_queues_drain(1,10,&dr); kh_gfx_gate_clear();
      if(!ok&&!dr.registered_bridge&&!dr.queues) st_mask=KH_APP_DRAIN_LATE; }
#define SUSP_OK(x) ((((unsigned)(x))&~st_mask)==0)
    /* 1. the resign: gate set and focus lost at once, nothing else */
    { u64 s0=atomic_load(&c_suspend); kh_app_will_resign_active(); TA(kh_gfx_gate_is_set() && !kh_app_window_active() && atomic_load(&c_suspend)==s0 && atomic_load(&st_stops)==0); }
    /* 2. the suspend: a thread admitted when it starts holds the quiesce until it leaves (100 ms later); then the drain steps, the
     *    pipeline-cache point, the PC's minimise, audio stop, pads neutral.  The gate stays set, the focus stays away */
    { StAdm a; memset(&a,0,sizeof a); kh_gfx_gate_clear(); pthread_t ta,tg; pthread_create(&ta,NULL,st_adm,&a); for(int i=0;i<2000&&atomic_load(&a.state)<2;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      kh_app_will_resign_active(); pthread_create(&tg,NULL,st_go_later,&a);
      kh_input_host_pad_set(0x1000,200,0,9000,0,0,0);
      u64 pc0=atomic_load(&c_pipecache), q0=atomic_load(&c_q_ok), d0=atomic_load(&c_drains), sp0=kh_pc_save_points(); u64 t0=kh_host_ns();
      int r=kh_app_suspend(3000); u64 dt=kh_host_ns()-t0; pthread_join(tg,NULL); pthread_join(ta,NULL);
      TA(SUSP_OK(r) && atomic_load(&a.state)==3 && dt>=80000000ull && atomic_load(&c_q_ok)==q0+1 && atomic_load(&c_drains)==d0+1 && atomic_load(&c_pipecache)==pc0+1);
      TA(kh_pc_save_points()==sp0+1);                                      /* TASK43: the disk pipeline cache's save point itself was reached (gfx_pipecache.c), once */
      u16 b=1,l=1; s16 lx=1; extern void kh_input_host_pad_get(u16*,u16*,s16*); kh_input_host_pad_get(&b,&l,&lx);
      TA(b==0 && l==0 && lx==0 && atomic_load(&st_stops)==1 && atomic_load(&st_starts)==0);
      TA(kh_shell_test_take()==KH_SHELL_MINIMISE && kh_shell_test_take()==0);
      TA(kh_gfx_gate_is_set() && !kh_app_window_active());
      /* 3. the resume: presenters marked, the PC's restore, audio restarted, focus back, gate cleared */
      u64 pr0=atomic_load(&c_present_resume), rc0=kh_present_resume_calls(); r=kh_app_resume(3000);
      TA(r==0 && atomic_load(&st_starts)==1 && atomic_load(&c_present_resume)==pr0+1 && kh_shell_test_take()==KH_SHELL_RESTORE && !kh_gfx_gate_is_set() && kh_app_window_active());
      TA(kh_present_resume_calls()==rc0+1); }                              /* review A41 (verification): the call itself, not only its counter here */
    TCLEAN();
    /* 4. a device that does not start again: loud, counted, the result says so; the restart stays pending and the next resume tries
     *    again (review A41); and audio not running at the suspend, with nothing pending, is not restarted */
    { atomic_store(&st_start_rc,-1); u64 af0=atomic_load(&c_au_fail); kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); int r=kh_app_resume(2000);
      TA(r==KH_APP_AUDIO_FAILED && atomic_load(&c_au_fail)==af0+1 && !kh_gfx_gate_is_set()); atomic_store(&st_start_rc,0);
      { int s0=atomic_load(&st_stops), a0=atomic_load(&st_starts); kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); TA(kh_app_resume(2000)==0);
        TA(atomic_load(&st_stops)==s0 && atomic_load(&st_starts)==a0+1 && atomic_load(&st_running)==1); }
      atomic_store(&st_running,0); int s1=atomic_load(&st_stops), a1=atomic_load(&st_starts); kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); TA(kh_app_resume(2000)==0);
      TA(atomic_load(&st_stops)==s1 && atomic_load(&st_starts)==a1); atomic_store(&st_running,1);
      while(kh_shell_test_take()){} }
    TCLEAN();
    /* 5. a submission that stays admitted past the bound: suspend-not-quiesced, the drain steps skipped, fence signals held, the gate
     *    stays set; the resume (after the thread left) clears it */
    { StAdm a; memset(&a,0,sizeof a); pthread_t ta; pthread_create(&ta,NULL,st_adm,&a); for(int i=0;i<2000&&atomic_load(&a.state)<2;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      u64 d0=atomic_load(&c_drains), h0=atomic_load(&c_hold), pc0=atomic_load(&c_pipecache), sp0=kh_pc_save_points(); kh_app_will_resign_active(); int r=kh_app_suspend(500);
      TA((r&KH_APP_NOT_QUIESCED) && atomic_load(&c_drains)==d0 && atomic_load(&c_hold)==h0+1 && atomic_load(&c_pipecache)==pc0 && kh_gfx_gate_is_set() && kh_gfx_gate_admitted()==1);
      TA(kh_pc_save_points()==sp0);                                        /* TASK43: not quiesced: no save point (a thread is still inside a submission) */
      atomic_store(&a.go,1); pthread_join(ta,NULL); TA(kh_app_resume(2000)==0 && !kh_gfx_gate_is_set()); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 6. the caller's bound: a worker step that takes longer (an audio stop of 400 ms) does not hold the caller beyond its bound (100 ms):
     *    BOUND_EXPIRED, counted, the gate set; the resume then runs after the suspend, in order, and clears it */
    { atomic_store(&st_stop_sleep_ms,400); u64 b0=atomic_load(&c_bound_exp); kh_app_will_resign_active(); u64 t0=kh_host_ns(); int r=kh_app_suspend(100); u64 dt=kh_host_ns()-t0;
      TA(r==KH_APP_BOUND_EXPIRED && dt<350000000ull && atomic_load(&c_bound_exp)==b0+1 && kh_gfx_gate_is_set());
      r=kh_app_resume(3000); TA(r==0 && !kh_gfx_gate_is_set() && kh_app_window_active()); atomic_store(&st_stop_sleep_ms,0); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 7. a resign alone (Control Center): no audio call, no PC message, the resume clears the gate; a resume with nothing before it is
     *    counted as such; a resume posted before a later resign does not clear the gate that resign set */
    { int s1=atomic_load(&st_stops), a1=atomic_load(&st_starts); kh_app_will_resign_active(); TA(kh_app_resume(2000)==0 && !kh_gfx_gate_is_set());
      TA(atomic_load(&st_stops)==s1 && atomic_load(&st_starts)==a1 && kh_shell_test_take()==0);
      u64 u0=atomic_load(&c_unpaired); TA(kh_app_resume(2000)==KH_APP_NOT_SUSPENDED && atomic_load(&c_unpaired)==u0+1);
      atomic_store(&st_stop_sleep_ms,300); kh_app_will_resign_active(); TA(kh_app_suspend(50)==KH_APP_BOUND_EXPIRED); u64 sp0=atomic_load(&c_superseded);
      kh_app_resume(50); kh_app_will_resign_active();
      /* review A41 (verification): a second suspend before a real resume finds the device already stopped: its restart stays pending,
       * and the resume after it starts the device (it used to be forgotten: no sound for the rest of the session) */
      int a2=atomic_load(&st_starts); TA(SUSP_OK(kh_app_suspend(2000)));
      TA(kh_app_resume(3000)==0); TA(atomic_load(&c_superseded)==sp0+1 && !kh_gfx_gate_is_set() && atomic_load(&st_starts)==a2+1 && atomic_load(&st_running)==1);
      atomic_store(&st_stop_sleep_ms,0); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 7b. review A41 (verification): a resign that arrives while a resume is inside its slow audio start (setActive on a Bluetooth route): that
     *     resume must not clear the gate the resign set (superseded DURING its steps, counted); the next resume clears it */
    { kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); atomic_store(&st_start_sleep_ms,300); u64 sl0=atomic_load(&c_superseded_late); int a0=atomic_load(&st_starts);
      TA(kh_app_resume(50)==KH_APP_BOUND_EXPIRED);                        /* the worker is inside the 300 ms start */
      kh_app_will_resign_active();                                        /* Control Center pulled down meanwhile */
      TA(st_wait_idle(3000));
      TA(kh_gfx_gate_is_set() && !kh_app_window_active() && kh_app_suspended() && atomic_load(&c_superseded_late)==sl0+1 && atomic_load(&st_starts)==a0+1);
      atomic_store(&st_start_sleep_ms,0);
      TA(kh_app_resume(3000)==0 && !kh_gfx_gate_is_set() && kh_app_window_active() && !kh_app_suspended() && atomic_load(&st_starts)==a0+1); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 7c. review A41 (verification): a suspend that waits behind a slow resume until its caller's bound has passed makes no drain Signal after
     *     its caller returned: suspend-caller-gone, the drain steps skipped, CPU fence signals held, the gate set; the next resume clears it */
    { kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); atomic_store(&st_start_sleep_ms,400);
      TA(kh_app_resume(50)==KH_APP_BOUND_EXPIRED);
      u64 d0=atomic_load(&c_drains), g0=atomic_load(&c_caller_gone), h0=atomic_load(&c_hold), pc0=atomic_load(&c_pipecache);
      kh_app_will_resign_active(); TA(kh_app_suspend(100)==KH_APP_BOUND_EXPIRED);
      TA(st_wait_idle(3000));
      TA(atomic_load(&c_drains)==d0 && atomic_load(&c_caller_gone)==g0+1 && atomic_load(&c_hold)==h0+1 && atomic_load(&c_pipecache)==pc0 && kh_gfx_gate_is_set());
      atomic_store(&st_start_sleep_ms,0); TA(kh_app_resume(3000)==0 && !kh_gfx_gate_is_set()); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 7d. review A41 (verification): audio-session interruptions run on the life worker (the shell only posts them): began stops the device,
     *     ended starts it again; a resume whose start fails while the interruption goes on is deferred (not a failure), its end starts it */
    { int a0=atomic_load(&st_starts), s0=atomic_load(&st_stops); atomic_store(&st_running,1);
      kh_app_audio_interruption(1); TA(st_wait_idle(3000)); TA(atomic_load(&st_stops)==s0+1 && atomic_load(&st_running)==0);
      kh_app_audio_interruption(0); TA(st_wait_idle(3000)); TA(atomic_load(&st_starts)==a0+1 && atomic_load(&st_running)==1);
      u64 f0=atomic_load(&c_au_fail), df0=atomic_load(&c_au_deferred);
      kh_app_audio_interruption(1); kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000))); atomic_store(&st_start_rc,-1);
      TA(kh_app_resume(2000)==0 && !kh_gfx_gate_is_set() && atomic_load(&c_au_fail)==f0 && atomic_load(&c_au_deferred)==df0+1 && atomic_load(&st_running)==0);
      atomic_store(&st_start_rc,0); kh_app_audio_interruption(0); TA(st_wait_idle(3000));
      TA(atomic_load(&st_running)==1 && atomic_load(&st_starts)==a0+3 && atomic_load(&st_stops)==s0+2); while(kh_shell_test_take()){} }
    TCLEAN();
    /* 8. NO STUCK BUDGET COUNTS TIME UNDER THE GATE (kh_compat.h): each helper waits 5 slices of 20 ms for nothing; with the gate held 8
     *    slices first, ETIMEDOUT comes only after 5 COUNTED slices after the clear (>= 12 slices); with the gate clear after 5 */
    { unsigned sl0=kh_stuck_slice_ms; pthread_t th; atomic_store(&st_tm_state,0); pthread_create(&th,NULL,st_tm_holder,NULL);   /* another thread holds st_tm: the lock waits time out */
      for(int i=0;i<5000&&atomic_load(&st_tm_state)!=1;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      kh_stuck_slice_ms=20;
      for(int k=0;k<3;k++){ int rc=0; unsigned long long fr0=__atomic_load_n(&kh_stuck_gate_free,__ATOMIC_RELAXED);
          u64 dt=st_stuck_once(k,5,8,&rc); TA(rc==ETIMEDOUT && dt>=12ull*20000000ull && __atomic_load_n(&kh_stuck_gate_free,__ATOMIC_RELAXED)>=fr0+5 && !kh_gfx_gate_is_set());
          dt=st_stuck_once(k,5,0,&rc); TA(rc==ETIMEDOUT && dt>=4ull*20000000ull && dt<12ull*20000000ull); }
      kh_stuck_slice_ms=sl0; atomic_store(&st_tm_state,2); pthread_join(th,NULL); }
    TCLEAN();
    /* 9b. the PC's focus loss (null_window.c): GetForegroundWindow is the game's window while active, another one (0) from the resign
     *     to the resume, counted */
    { extern void rt_imp_user32__GetForegroundWindow(KCtx*); KCtx *g=kh_new_ctx(); u64 stk=ga_calloc(1,4096), q0=atomic_load(&c_fg_other), r[3];
      for(int k=0;k<3;k++){ if(k==1) kh_app_will_resign_active(); if(k==2) kh_app_resume(2000);
          KH_RSP(g)=stk+2048; wr64(g,stk+2048,0); rt_imp_user32__GetForegroundWindow(g); r[k]=KH_RAX(g); }
      TA(r[0]==0xAB0001ull && r[1]==0 && r[2]==0xAB0001ull && atomic_load(&c_fg_other)==q0+1); ga_free(stk); free(g); }
    /* 9c. KHNMC_SCREEN=layer (null_window.c): the game's screen is the drawable size the shell handed over before the start; without one
     *     it is the named fatal screen-layer-unset */
    { extern void kh_present_test_layer_size(uint32_t,uint32_t); extern void kh_screen_test_state(const int*,int*); extern void kh_screen_config(const char*,const char*);
      extern int kh_screen_size(int*,int*); int old[8]; uint32_t lw0=0,lh0=0; kh_present_layer_size(&lw0,&lh0);
      kh_screen_test_state(NULL,old); kh_present_test_layer_size(2670,1878); kh_screen_config("layer",NULL); int w=0,h=0, set=kh_screen_size(&w,&h);
      TA(set==1 && w==2670 && h==1878); kh_screen_test_state(old,NULL);
      /* review A41 (verification): the shell's size call of EVERY app (also without the graphics backend) is what KHNMC_SCREEN=layer reads */
      { uint32_t a=0,b=0; kh_present_test_layer_size(0,0); kh_present_note_drawable(2000,1000); TA(kh_present_layer_size(&a,&b)==1 && a==2000 && b==1000); }
      if(kh_fork_ok()){ FILE *tf=kh_tmpfile(); if(tf){ pid_t c=kh_fork(); if(c==0){ dup2(fileno(tf),2); kh_present_test_layer_size(0,0); kh_screen_config("layer",NULL); _exit(99); }
          int stt=0; if(c>0) waitpid(c,&stt,0); char out[2048]; rewind(tf); size_t k=fread(out,1,sizeof out-1,tf); out[k]=0; fclose(tf);
          TA(c>0 && WIFEXITED(stt) && WEXITSTATUS(stt)==3 && strstr(out,"screen-layer-unset")!=NULL); } }
      else kh_selftest_not_run("app life","KHNMC_SCREEN=layer without a layer size (a named fatal in a child process)",1);
      kh_present_test_layer_size(lw0,lh0); }
    /* 9d. KHNMC_CLOCK=frames: the PC's minimise is not delivered (it would stop the virtual clock for good), counted */
    { u64 k0=atomic_load(&c_min_skipped); st_frames_clock=1; kh_app_will_resign_active(); TA(SUSP_OK(kh_app_suspend(2000)));
      st_frames_clock=-1; TA(kh_shell_test_take()==0 && atomic_load(&c_min_skipped)==k0+1); TA(kh_app_resume(2000)==0 && kh_shell_test_take()==0 && !kh_gfx_gate_is_set()); }
    /* A claimed cut permits the UI to post a suspend but defers the worker drain. */
    { u64 epoch, drains=atomic_load(&c_drains);
      TA(kh_cut_gate_begin(&epoch)); TA(kh_cut_gate_commit(epoch));
      int locked=pthread_mutex_trylock(&lm)==0;
      TA(locked);
      if(locked){
          pthread_mutex_unlock(&lm);
          kh_app_will_resign_active();
          TA(kh_app_suspend(50)==KH_APP_BOUND_EXPIRED);
          TA(atomic_load(&c_drains)==drains);
      }
      kh_cut_gate_committed_end(epoch);
      if(locked){ TA(kh_gfx_gate_is_set()); TA(st_wait_idle(3000)); TA(kh_app_resume(2000)==0); }
    }
    /* 9. the test trigger's value */
    { u64 a=0; unsigned b=0; TA(test_parse("frame:120:35000",&a,&b)==0 && a==120 && b==35000); TA(test_parse("frame:0:5",&a,&b)==2 && test_parse("frame:5",&a,&b)==1 && test_parse("frames:1:2",&a,&b)==1 && test_parse("frame:1:2x",&a,&b)==1); }
    TA(st_wait_idle(3000)); au_pending=0; au_interrupted=0;                 /* the worker is idle: its audio state back to none */
    au_start=s_start; au_stop=s_stop; au_running=s_run; atomic_store(&life_used,used0);
    TA(!kh_gfx_gate_is_set() && kh_app_window_active() && kh_gfx_gate_admitted()==0);
    fprintf(stderr,"kh: app life selftest: %d checks, %d failures (resign; suspend: quiesce, drain, slot fences, step-12 point, the PC's minimise, audio stop, pads neutral; resume; audio restart failure; suspend-not-quiesced; the caller's bound; resign alone, unpaired and superseded resumes (before and during their steps); a suspend whose caller is gone; audio restart kept pending; interruptions on the worker; stuck budgets do not count time under the gate; the PC's focus loss; KHNMC_SCREEN=layer)\n",n,f);
#undef SUSP_OK
#undef TCLEAN
#undef TA
    return f;
}
