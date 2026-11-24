/* khnmc_app.h : the app's life cycle between a platform shell and the runtime (TASK41; IOS-GRAPHICS-PLAN.md section 4 step 11,
 * docs/IOS-SHELL.md "Suspend and resume").  runtime/app_life.c implements it; runtime/ios/ios_main.m (the scene delegate) and the
 * Linux test triggers (KHNMC_TEST_GATE, KHNMC_TEST_SUSPEND, below) call it.  No runtime type here: Objective-C includes this header.
 *
 * THE SHELL'S CALLS (any thread that is not a guest thread; on iOS the main thread).  None of them blocks longer than its bound, and none
 * of them calls a vkd3d queue function itself ("Review c1/c2" item 2: the main thread could block without bound on vkd3d's queue mutex
 * while a presenter holds it inside MoltenVK, which may be waiting for the main thread):
 *   kh_app_will_resign_active()  sceneWillResignActive (Control Center, a call banner, the start of every trip to the background): the
 *                                suspend gate is set and the game's window loses the focus, then it returns at once.  GPU work is still
 *                                allowed at this point; what is in flight finishes; with the vkd3d backend the game's submitting threads
 *                                park at their next submission (the plan's gate, "Review c1/c2" item 2: the game stops while Control
 *                                Center is down, its sound plays on; NOT the PC's behaviour, an open owner decision in implementation notes).
 *                                The null backend does not park at a resign (review A41, verification): there it is exactly the PC's focus loss.
 *                                Focus loss as on the PC: GetForegroundWindow() is no longer the game's window, so the game's
 *                                own input update reads neutral pads (exe 0x105810 -> 0x134ff0; see app_life.c "the PC's focus loss").
 *   kh_app_suspend(bound_ms)     sceneDidEnterBackground: the plan's "Leaving" steps 2-7 on the life worker thread (gate quiesced; on every
 *                                registered command queue a runtime fence Signal + a bounded GetCompletedValue poll; fence signals held
 *                                when a drain did not complete or a queue Wait was seen; the presenters' slot fences; the pipeline-cache
 *                                save point (step 12: a named stub); audio stop; every pad neutral) and the PC's minimise (the form's
 *                                BaseSimpleForm::suspend, delivered on the game's UI thread: runtime/shell.c).  It waits for the worker
 *                                at most bound_ms, counted in slices (a stopped process costs one slice).  Every bound that expires is
 *                                logged and counted, and the gate stays set.  Returns 0 when every step completed, else KH_APP_* bits.
 *                                A suspend that starts on the worker after its caller's bound has passed (it waited behind a slow resume)
 *                                makes no drain Signal (suspend-caller-gone, counted).  The null backend's Present parks from here to the
 *                                resume that clears the gate.
 *   kh_app_resume(bound_ms)      sceneDidBecomeActive: "Coming back" on the same worker, in order after any suspend still running: every
 *                                presenter rebuilds only its own swapchain at its next Present (a slot "acquired, not presented" gets a
 *                                new acquire semaphore there), the PC's restore (BaseSimpleForm::resume), audio restarted (when it ran at
 *                                the suspend; the result is checked, a failure is loud and counted), the focus back, the gate cleared.
 *                                The shell passes the layer's size first (kh_present_set_size) and reads its controllers' CURRENT state
 *                                after (a button that came up during the suspension sent no event).  Waits at most bound_ms.  A resign or
 *                                suspend that comes while it still runs on the worker wins: the gate and the focus stay away (counted).
 *   kh_app_audio_interruption(b) AVAudioSessionInterruptionNotification began (1) / ended (0): posted to the same worker, returns at once
 *                                (the main thread never activates the session or takes the CoreAudio lock).  Began: the device is stopped
 *                                when it runs; ended: started again unless the app is suspended (the resume does it then).
 *   kh_app_set_audio(start,stop) the device's start / stop (iOS: kh_coreaudio_start / kh_coreaudio_stop; set before the first suspend).
 *                                Without it: kh_audio_out_start / kh_audio_out_stop (the pull ring's device-neutral half).
 * Without any of these calls nothing changes: no thread, no gate, no report line. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* kh_app_suspend / kh_app_resume results (0 = every step done in its bound) */
#define KH_APP_BOUND_EXPIRED   0x01u   /* the worker had not finished when the caller's bound expired (it goes on; the gate stays set) */
#define KH_APP_NOT_QUIESCED    0x02u   /* suspend-not-quiesced: a submission was still admitted at the end of its bound; drain steps skipped */
#define KH_APP_DRAIN_LATE      0x04u   /* a registered queue did not reach its drain fence in the bound (fence signals now held) */
#define KH_APP_SLOTS_LATE      0x08u   /* a presenter's slot fence did not complete in the bound */
#define KH_APP_AUDIO_FAILED    0x10u   /* resume: the audio device did not start again (loud; the mixer keeps its fallback clock) */
#define KH_APP_NOT_SUSPENDED   0x20u   /* resume without a suspend or a resign before it (counted; the gate is cleared anyway) */
#define KH_APP_CALLER_GONE     0x40u   /* suspend: it reached the drain after its caller's bound had passed: drain steps skipped (no Signal) */

int kh_app_game_presented(void);
void kh_app_will_resign_active(void);
int  kh_app_suspend(unsigned bound_ms);
int  kh_app_resume(unsigned bound_ms);
void kh_app_set_audio(int (*start)(void),void (*stop)(void));
void kh_app_audio_interruption(int began);
uint64_t kh_app_host_ns(void);                  /* the runtime's monotonic clock (CLOCK_MONOTONIC ns): the display link's tick time */
#define KH_APP_SUSPEND_BOUND_MS 2000u           /* what the iOS shell passes (iOS ends a background task after ~5 s) */
#define KH_APP_RESUME_BOUND_MS   500u

/* ---- runtime side ------------------------------------------------------------------------------------------------------------------ */
int  kh_app_window_active(void);                /* 0 from kh_app_will_resign_active / kh_app_suspend to the end of kh_app_resume (null_window.c) */
void kh_app_note_foreground_query(void);        /* GetForegroundWindow answered "another window" (counted) */
void kh_app_test_init(void);                    /* start-up (null_d3d12.c): KHNMC_TEST_GATE / KHNMC_TEST_SUSPEND (below) */
void kh_app_report(FILE *f);                    /* exit: one line when anything of this header was used, else nothing */
int  kh_app_life_selftest(void);
/* step 12's call point (the gate is set, nothing admitted): TASK43, kh_gfx_pipecache_save_point(deadline_ns) of runtime/gfx_pipecache.h */
int  kh_app_test_suspend_cycle(int at,unsigned ms,char *why,size_t whycap,uint64_t *r1,uint64_t *r2);   /* TASK43: one resign / suspend / resume
                                                 * cycle with the trigger's checks; at = -1, or a presenter test point (gfx_present.h KH_TP_*);
                                                 * returns the number of failed checks */
int  kh_app_test_used(int set);                 /* selftests: the "life cycle was used" flag of the exit report: returns it; set >= 0 stores it */
int  kh_app_suspended(void);                    /* 1 from kh_app_suspend to the resume that clears the gate */
void kh_app_null_present_park(void);            /* the null backend's Present: waits while suspended (never at a resign alone), counted */
uint64_t kh_app_null_parks(void);
void kh_app_test_suspended(int on);             /* selftest: the suspended state alone */

/* the queue drain of "Leaving" step 3 (runtime/gfx_wrap.c; called on the life worker only, after kh_gfx_gate_quiesce) */
typedef struct {
    int registered_bridge;                      /* 1 = the wrapped bridge (KHNMC_GFX=vkd3dw) registers its queues; 0 = no registry */
    unsigned queues, signalled, drained, late, failed;   /* registered queues; drain Signals made; reached in the bound; not; no fence / Signal failed */
    unsigned slices;                            /* expired slices of the poll */
    uint64_t waits;                             /* queue Wait calls seen on the registered queues so far */
} KhDrainResult;
int  kh_gfx_queues_drain(unsigned slices,unsigned slice_ms,KhDrainResult *r);   /* 1 = every registered queue drained (also: none) */

/* the PC's minimise / restore on the game's UI thread (runtime/shell.c: ManageRun serves them) */
#define KH_SHELL_MINIMISE 1
#define KH_SHELL_RESTORE  2
void kh_shell_post(int req);
void kh_shell_counts(uint64_t *posted,uint64_t *minimised,uint64_t *restored,int *serving);
void kh_shell_resize_counts(uint64_t *calls,uint64_t *noop,uint64_t *other_reason);   /* review A41: the restore's GXDevice.resize (OnResize) */
int  kh_shell_test_take(void);                  /* selftest: the oldest request not yet served (0 = none), removed */

/* the host pad (runtime/null_input.c) */
unsigned kh_input_host_pad_neutral(void);       /* every button up, sticks and triggers centred, latched taps dropped; returns what was held */

/* Linux test triggers (every build; a named fatal in a --selftest run):
 *   KHNMC_TEST_GATE=frame:N:ms      at the N-th Present a host thread sets the suspend gate, holds it ms of host time, clears it
 *   KHNMC_TEST_SUSPEND=frame:N:ms   at the N-th Present a host thread (standing for the shell's main thread) calls the same
 *                                   kh_app_will_resign_active, kh_app_suspend(KH_APP_SUSPEND_BOUND_MS), waits ms, kh_app_resume(...)
 *   KHNMC_TEST_SUSPEND_AT=before-acquire | after-acquire   (TASK43; with KHNMC_TEST_SUSPEND, needs a presenter) the suspend is FORCED
 *                                   at that point of the presenter's Present: a Present is stopped there, the resign comes, it goes on.
 *                                   Both interleavings are correct paths and each is verified (after an acquire: the parked acquire is
 *                                   consumed, its semaphore made anew, the swapchain rebuilt); without the variable the suspend comes
 *                                   wherever the game is, and whichever interleaving happens is verified the same way.
 * Both are reported at exit (kh_app_report), and both ASSERT what they show (review A41): the gate trigger runs a stuck-budget waiter
 * (kh_cond_wait_stuck, 30 slices) behind the parked game and requires that it saw the game come back, that slices under the gate were not
 * charged, and with vkd3d that nothing was submitted outside an admission and (wrapped bridge) a game queue call parked; the suspend
 * trigger requires results 0, pads neutral, the presenters told to rebuild, audio restarted when it was stopped, every registered queue
 * drained, the null Present parked (null backend), and with a presenter that a Present saw the resume and a swapchain rebuild followed
 * (TASK43: a Present that parked after its acquire no longer fails the test; its own counters are checked instead).  A failed
 * check is the named fatal app-test-gate-failed / app-test-suspend-failed. */

/* review fix #10 (runtime/aspect.c): the iOS shell's current safe-area insets, per mille of the screen's height (clamped 0..200), re-read
 * at every size / orientation / inset change; the portrait camp stack follows on the next frame. */
void kh_safe_area_set(double top_pm,double bottom_pm);

#ifdef __cplusplus
}
#endif
