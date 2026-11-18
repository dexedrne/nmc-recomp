/* gfx_present.h : the picture on a display (IOS-GRAPHICS-PLAN.md steps 7, 8 and the gate of step 11; TASK36).
 *
 * Three pieces, all in runtime/gfx_present.c:
 *
 *  1. THE SUSPEND GATE, admission-counted (task 39; IOS-GRAPHICS-PLAN.md "Review c1/c2" item 1).  A process-wide flag plus a count of
 *     the threads that are inside a submit-capable region.  kh_gfx_gate_enter(where) waits while the gate is set, then counts the
 *     caller in under the gate's mutex; kh_gfx_gate_try_enter(where) does the same without waiting (0 = the gate is set, nothing was
 *     done: the caller takes its own way back to the gate); kh_gfx_gate_leave() counts it out after the submission call returned.
 *     kh_gfx_gate_quiesce(slices), called after kh_gfx_gate_set, waits until nothing is admitted, at most `slices` expired slices of
 *     kh_stuck_slice_ms, and returns a named result (kh_gfx_gate_result_name).  A thread admitted before the gate was set finishes
 *     its submission and holds quiesce until it leaves; a thread that arrives while it is set parks outside, holds no runtime lock,
 *     is counted per place and is not admitted.  Admissions never nest (gfx-gate-nested); a leave without one is gfx-gate-unbalanced.
 *     What is gated TODAY:
 *       ADMISSIONS (the submission happens only while admitted):
 *         - the presenter from its last check to vkd3d_release_vk_queue (submit + vkQueuePresentKHR; gfx_present.c "presenter:
 *           submit and present");
 *         - the presenter's consumption of a parked acquire (slot_consume's vkQueueSubmit), also in park_after_acquire, which
 *           parks again when the gate was set once more between its park and its admission;
 *         - the frame readback of kh_vkd3d_present (dumps, hash log, video) and the check-mode reference readback: their
 *           ExecuteCommandLists + Signal on the game's queue (gfx_vkd3d.c readback());
 *         - the back-buffer drain's ID3D12CommandQueue::Signal (ResizeBuffers / release);
 *         - (TASK41, step 11) the game's own queue calls through gfx_wrap.c w_queue: ExecuteCommandLists, Signal and Wait;
 *         - (TASK41) ID3D12Fence::Signal from the CPU (w_fence slot 10) while a queue may be blocked (fence signals held, or a queue
 *           Wait was seen): kh_gfx_gate_enter_if_may_block, the condition read under the gate's mutex with the admission;
 *         - (TASK41) the suspend drain's own Signal on every registered queue (kh_gfx_gate_enter_drain: admitted WHILE the gate is set,
 *           only on the life worker of runtime/app_life.c, after kh_gfx_gate_quiesce).
 *       PARKS (wait while set, not admission-counted):
 *         - the top of kh_vkd3d_present (the game's Present waits while the gate is set, also when nothing is read back);
 *         - the presenter before vkAcquireNextImageKHR and after every acquire time-out; its check after the marker (e) parks
 *           after the acquire (park_after_acquire);
 *         - (TASK41) the null backend's Present (null_d3d12.c), only while the app is SUSPENDED (kh_app_suspend to the resume; review
 *           A41: a resign alone is the PC's focus loss there, the game runs on): app_life.c kh_app_null_present_park, not this gate.
 *       NOT HELD: the game's CPU fence signals while no queue can be blocked (no Wait seen, no drain that failed): they submit nothing.
 *       The gipa wrapper counts every vkQueueSubmit vkd3d makes; one made while the gate is set by a thread that is not admitted is
 *       counted and logged (gfx_vkd3d.c, "OUTSIDE an admission").
 *       Every submission call of an admission is preceded by kh_gfx_gate_assert_admitted (the presenter's also when it gives the
 *       VkQueue back): a submission outside its admission is the named fatal gfx-gate-not-admitted (review Y39 n=2).
 *       The presenter's marker (d) is outside every admission: vkd3d flushes it only when it is alone in the game's queue (so never
 *       that queue's own queued work), but running it signals a fence from the CPU, which makes vkd3d flush EVERY blocked queue of
 *       the device (vkd3d 2.1 command.c 891, 746-776).  A queue whose Wait another thread's signal has just satisfied (a CPU signal that
 *       passed its check before the set, the race step 11 lists) can thus have its work submitted from the presenter thread; the
 *       step-11 test (no vkQueueSubmit through the gipa counter while the gate is set) is the guard (review Y39 n=5).
 *     The presenter does not hold its lock in (d) (review Y39 n=1): kh_present_resume and kh_present_quiesce may be called from the
 *     main thread while the gate is set and a Present waits there.
 *     The bounded waits after a submission (the presenter's marker wait (d), the drain's fence wait) count an expired slice only when
 *     the gate was clear for the whole slice (kh_gfx_gate_slices_*): time under a set gate never fires their named fatals.
 *     The iOS scene delegate (step 11) sets and clears it through runtime/app_life.c (khnmc_app.h: kh_app_will_resign_active,
 *     kh_app_suspend, kh_app_resume), with the queue drain; no stuck budget of kh_compat.h counts a slice that overlapped a set gate.
 *
 *  2. DISPLAY PACING (host clock only).  IDXGIOutput::WaitForVBlank (null_d3d12.c) waits for the next display refresh when a tick
 *     source is registered: the iOS shell's CADisplayLink calls kh_vsync_tick on every refresh; on Linux KHNMC_DISPLAY=timer60 is a
 *     stand-in (a host thread on absolute 16.667 ms deadlines) with the same interface.  Without a source the old 16.67 ms sleep stays.
 *     KHNMC_CLOCK=frames is not affected (the virtual vblank of vclock.c answers first; ticks are then counted as ignored).
 *
 *  3. THE PRESENTER (builds with the vkd3d backend).  KHNMC_GFX_PRESENT=off|headless|layer (default off).  At every Present the
 *     presented back buffer (one of the swap chain's own VkImages, gfx_vkd3d.c) is scaled into an image of a VkSwapchainKHR and
 *     presented: VK_EXT_headless_surface (Linux lavapipe, MoltenVK) or VK_EXT_metal_surface on the CAMetalLayer the shell hands over.
 *         KHNMC_GFX_PRESENT_SIZE=WxH        headless surface size (default 1920x1080)
 *         KHNMC_GFX_PRESENT_FIT=aspect|stretch   aspect = black bars (the PC look, default)
 *         KHNMC_GFX_PRESENT_CHECK=n         every n-th present: exact black bars and the picture within the tolerance of a CPU bilinear
 *                                           reference computed from an INDEPENDENT copy of the same back buffer: a D3D12 readback queued
 *                                           after the presenter's submission (0 = off, default)
 *     KHNMC_GFX_PRESENT other than off needs the vkd3d backend (KHNMC_GFX=vkd3dw / vkd3d): otherwise gfx-present-needs-backend at start-up.
 *         KHNMC_GFX_PRESENT_TOL=max,mean    the tolerance in levels (default 2,0.5)
 *
 * This header has no Vulkan or D3D12 type: the iOS shell (Objective-C) includes it. */
#pragma once
#include <stdint.h>
#include <stdio.h>

/* ---- 1. suspend gate ---------------------------------------------------------------------------------------------------------- */
void kh_gfx_gate_set(void);                     /* from now on every gated thread parks at its next check */
void kh_gfx_gate_clear(void);                   /* wakes every parked thread */
int  kh_gfx_gate_is_set(void);
void kh_gfx_gate_hold_fence_signals(int on);    /* step 11: hold ID3D12Fence::Signal from the CPU while the gate is set */
/* admissions (task 39): every submit-capable region of the runtime */
void kh_gfx_gate_enter(const char *where);      /* wait while set (parked, counted), then admitted; `where` names the place (a string literal) */
int  kh_gfx_gate_try_enter(const char *where);  /* 1 = admitted; 0 = the gate is set: not admitted, not parked (counted as refused) */
void kh_gfx_gate_leave(void);                   /* after the submission call returned; the last one out wakes kh_gfx_gate_quiesce */
void kh_gfx_gate_assert_admitted(const char *what);   /* right before a submission call: not admitted = the named fatal gfx-gate-not-admitted */
enum { KH_GATE_QUIESCED=0, KH_GATE_NOT_QUIESCED=1, KH_GATE_QUIESCE_NOT_SET=2 };
int  kh_gfx_gate_quiesce(unsigned slices);      /* the gate set: wait until nothing is admitted, at most `slices` expired slices; KH_GATE_* */
int  kh_gfx_gate_quiesce_bounded(unsigned slices,unsigned slice_ms);   /* TASK41: the same, slices of slice_ms (0 = kh_stuck_slice_ms) */
const char *kh_gfx_gate_result_name(int r);     /* "quiesced", "suspend-not-quiesced" (logged with the places still admitted), "gate-not-set" */
int  kh_gfx_gate_admitted(void);                /* threads admitted now */
uint64_t kh_gfx_gate_parks_at(const char *prefix);   /* review A41: parks at the places whose name starts with prefix */
/* TASK41 (plan step 11) */
void kh_gfx_gate_enter_drain(const char *where);    /* the suspend drain's own Signal: admitted WHILE the gate is set (with it clear: gfx-gate-drain-unset) */
int  kh_gfx_gate_enter_if_may_block(const char *where);   /* ID3D12Fence::Signal from the CPU: an admission (waits while set) iff a queue may be
                                                         * blocked (fence signals held, or a queue Wait seen); 0 = not gated, nothing done */
int  kh_gfx_gate_thread_admitted(void);         /* the calling thread is inside an admission (the gipa vkQueueSubmit counter) */
/* a bounded wait in expired slices that does not count time under a set gate: begin, then after every poll `expired(s, N)` = 1 once N
 * slices COUNTED (the gate clear for the whole slice); s.free_ = slices not counted */
typedef struct { uint64_t start_ns, deadline_ns; unsigned counted, free_; } KhGateSlices;
void kh_gfx_gate_slices_begin(KhGateSlices *s);
int  kh_gfx_gate_slices_expired(KhGateSlices *s,unsigned slices);
int  kh_gfx_gate_slice_counts(uint64_t slice_start_ns);
int  kh_gfx_gate_selftest(void);                /* every --selftest (and KHNMC_SELFTEST_ONLY=gate) */
/* selftest: the next admission at a place containing `place` sets the gate inside its region and runs kh_gfx_gate_quiesce(slices) on
 * another thread; kh_gfx_gate_test_result(what) = 1 when quiesce returned `quiesced` only after that thread left (and joins it) */
void kh_gfx_gate_test_arm(const char *place,unsigned slices);
int  kh_gfx_gate_test_result(const char *what);
/* runtime-internal */
void kh_gfx_gate_park(const char *where);       /* returns at once when the gate is clear; otherwise parks (counted per place) */
int  kh_gfx_gate_park_fence_signal(void);       /* ID3D12Fence::Signal: parks only while the gate is set AND fence signals are held; 1 = parked */
void kh_gfx_gate_note_queue_wait(void);         /* ID3D12CommandQueue::Wait was called: from now on a queue may be blocked */
uint64_t kh_gfx_gate_parks(void);               /* total parks so far (all places) */
void kh_gfx_gate_report(FILE *f);

/* ---- 2. display pacing ------------------------------------------------------------------------------------------------------- */
void kh_vsync_register(void);                   /* a tick source exists from now on (call before its first tick) */
void kh_vsync_tick(uint64_t host_ns);           /* one display refresh at host_ns (CLOCK_MONOTONIC ns); any thread */
/* runtime-internal */
void kh_vsync_init(void);                       /* start-up: KHNMC_DISPLAY=timer60 starts the stand-in source */
int  kh_vsync_wait(void);                       /* WaitForVBlank on the host clock: 1 = paced by ticks (done), 0 = no source (sleep as before) */
void kh_vsync_note_virtual(void);               /* WaitForVBlank answered by the virtual vblank (KHNMC_CLOCK=frames) */
void kh_vsync_note_sleep(void);                 /* WaitForVBlank answered by the old 16.67 ms sleep */
void kh_vsync_report(FILE *f);
int  kh_vsync_selftest(void);
/* runtime-internal, every build (review V36 n=5) */
void kh_present_init(int vkd3d_backend_on);     /* start-up, after the backend is chosen: KHNMC_GFX_PRESENT other than off without it = gfx-present-needs-backend */
void kh_present_shell_report(FILE *f);          /* exit: the shell's kh_present_set_layer / _set_size calls, used or DROPPED */

/* ---- 3. presenter: what the shell calls (Apple; the layer sink) -------------------------------------------------------------- */
/* Both are counted in every build; a build without the vkd3d backend, or a run without a layer presenter, reports them as DROPPED at exit */
void kh_present_set_layer(void *ca_metal_layer,uint32_t w_px,uint32_t h_px);   /* before the game starts (KHNMC_GFX_PRESENT=layer) */
void kh_present_set_size(uint32_t w_px,uint32_t h_px);                         /* the drawable size: stored; the swapchain is rebuilt at the next present only when it changed (0x0, or the same size: counted, ignored) */
void kh_present_note_drawable(uint32_t w_px,uint32_t h_px);                    /* review A41: the drawable size alone (every app, also without the backend; no lock, no line) for KHNMC_SCREEN=layer */
/* TASK41: the shell's view, display link and test label (no lock, any thread, every build) */
int  kh_present_built(void);                     /* 1 = this binary has the vkd3d backend (the shell hands over its layer only then) */
int  kh_present_wants_ticks(void);               /* -1 = not decided yet (before start-up read KHNMC_GFX_PRESENT), 1 = a layer run: the display
                                                  * link calls kh_vsync_tick; 0 = not (the null app keeps its WaitForVBlank pacing; the link stops) */
int  kh_present_layer_size(uint32_t *w_px,uint32_t *h_px);   /* the drawable size of the last kh_present_set_layer; 0 = none (KHNMC_SCREEN=layer) */
/* TASK41: the shell's resume (through kh_app_resume) and its test label: no lock, any thread, every build (no backend: no-ops / 0) */
void kh_present_resume_all(void);                /* every presenter rebuilds only its own swapchain at its next Present (an atomic generation) */
uint64_t kh_present_presented(void);             /* swapchain presents so far (the shell hides its label after the first) */
uint64_t kh_present_rebuilds_after_resume(void);
uint64_t kh_present_resume_calls(void);          /* review A41: kh_present_resume_all calls (every build) */
uint64_t kh_present_parked_after_acquire(void);  /* review A41: Presents that parked after their acquire (they also rebuild as after-park) */
/* runtime-internal (the suspend drain's worker): every presenter's slot fences, bounded, no fatal; 1 = all complete */
int  kh_present_quiesce_all_bounded(unsigned slices,unsigned slice_ms,unsigned *npres,unsigned *left);
