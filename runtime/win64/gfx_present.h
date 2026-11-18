/* gfx_present.h : the picture on a display (IOS-GRAPHICS-PLAN.md steps 7, 8 and the gate of step 11; TASK36).
 *
 * Three pieces, all in runtime/gfx_present.c:
 *
 *  1. THE SUSPEND GATE.  One process-wide flag.  While it is set, no thread of the runtime submits GPU work: the checks are at the top
 *     of kh_vkd3d_present (before the frame readback for dumps, hash log and video, which submits on the game's queue), before the
 *     ResizeBuffers / back-buffer drain, in the presenter (before acquire, after every acquire time-out, and again before submit and
 *     present), and on ID3D12Fence::Signal from the CPU while a queue may be blocked (a CPU signal can make vkd3d submit queued work:
 *     command.c 1197-1205; the hold is switched on by kh_gfx_gate_hold_fence_signals, or by itself once a queue Wait was seen).  A
 *     thread that meets the set gate parks on a condition, holds no runtime lock, and is counted.  The iOS scene delegate (step 11)
 *     sets and clears it (step 11, with the queue drain and the stuck-budget exemption); today only the selftest does.
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
void kh_present_set_size(uint32_t w_px,uint32_t h_px);                         /* the drawable size changed: stored, the swapchain is rebuilt at the next present (0x0: counted, ignored) */
