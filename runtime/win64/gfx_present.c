/* gfx_present.c : the suspend gate, display pacing of WaitForVBlank, and the Vulkan presenter (TASK36; IOS-GRAPHICS-PLAN.md steps 7,
 * 8 and the gate of step 11).  The interface and the settings are described in gfx_present.h.
 *
 * The gate, the display pacing, the start-up check of KHNMC_GFX_PRESENT and the counting of the shell's calls are host code and are
 * compiled in every build.  The presenter needs the vkd3d backend (KH_GFX_VKD3D); without an active one KHNMC_GFX_PRESENT other than
 * off is the named fatal gfx-present-needs-backend (kh_present_init, review V36 n=5).
 *
 * THE PRESENTER, per Present (kh_present_frame, called by gfx_vkd3d.c kh_vkd3d_present AFTER the optional frame readback and before
 * null_d3d12.c advances the back-buffer index):
 *   (a) gate check (park while set; no lock held)
 *   (b) the swapchain is made lazily, or again when stale (oldSwapchain passed): B8G8R8A8_UNORM + SRGB_NONLINEAR (never _SRGB: the
 *       game's values are already display-encoded), FIFO, max(minImageCount,3) images (limited by maxImageCount unless 0), the
 *       surface's currentExtent or, when that is 0xFFFFFFFF, the host's size; usage TRANSFER_DST (+TRANSFER_SRC in check mode when
 *       reported).  The surface must be supported on the family of the game's queue.
 *   (c) the slot's fence is waited for (its acquire semaphore and command buffer are reused: a semaphore with a pending wait must
 *       not be handed to vkAcquireNextImageKHR; the plan acquired first, which would reuse it while the previous submit may still wait
 *       on it), then vkAcquireNextImageKHR with a 1-slice time-out, before any lock of vkd3d.  A time-out goes back to (a); it is
 *       counted unless the gate is set or was cleared less than one slice ago; 5 counted time-outs in a row are gfx-present-acquire.
 *   (d) vkd3d_queue_signal_on_cpu on the presenter's fence, waited for in slices (5 expired slices = gfx-present-queue-blocked; a
 *       stopped process costs only the slice it interrupts): everything the game queued for this frame has left vkd3d's op queue.
 *   (e) gate check again: if set now, the acquired image stays "acquired, not presented", the thread parks, and on the way back the
 *       slot's acquire semaphore is consumed (an empty batch that waits on it) and then destroyed and made anew, and the swapchain is
 *       rebuilt.  Otherwise our command buffer is recorded: back-buffer barrier (COLOR_ATTACHMENT_OUTPUT|TRANSFER -> TRANSFER, writes ->
 *       TRANSFER_READ, layout unchanged), swapchain image UNDEFINED -> TRANSFER_DST, a black clear, a write-after-write barrier, the
 *       aspect-fit vkCmdBlitImage (LINEAR, or NEAREST when the format cannot filter linearly), in check mode copies of the swapchain
 *       image AND of the back buffer to host buffers (`general` mode: the first back-buffer barrier also has HOST in its first scope, to
 *       chain with vkd3d's PRESENT = GENERAL at the HOST stage), in `general` back-buffer mode a trailing back-buffer barrier TRANSFER -> HOST
 *       (there PRESENT maps to the HOST stage: without it the game's next PRESENT -> RENDER_TARGET barrier would not wait for the
 *       blit), and the swapchain image to PRESENT_SRC_KHR.
 *   (f) vkd3d_acquire_vk_queue; vkQueueSubmit (waits on the acquire semaphore at TRANSFER, signals the image's render semaphore and the
 *       slot fence); vkQueuePresentKHR (FIFO; VkPresentTimesInfoGOOGLE{present number, desiredPresentTime 0} chained when
 *       VK_GOOGLE_display_timing is enabled); vkd3d_release_vk_queue.
 *   (g) SUBOPTIMAL / OUT_OF_DATE mark the swapchain stale; SURFACE_LOST (from the acquire or from the present) destroys the swapchain and
 *       the surface after queue idle, and the next Present makes both anew.  A zero-size drawable destroys the swapchain; Presents are
 *       then counted zero-size skips until it has a size.  The game's back buffers are never touched.
 * Check mode (every n-th present; review V36 n=1): kh_present_frame returns 1, and kh_vkd3d_present queues a D3D12 readback of the same
 * back buffer AFTER the presenter's submission and hands it to kh_present_check.  That reference is independent of the image the
 * presenter picked and of its barriers.  After the slot fence the bars must be exactly black, the picture must be within the tolerance
 * of a CPU bilinear reference computed from the readback with the blit's own mapping (Vulkan "scaled blit": the centre of destination
 * texel x maps to u = (x + 0.5 - dst0) * src_w / dst_w, filtered with clamp to edge), and the presenter's own copy of its source image
 * must equal the readback byte for byte.  A picture is written as present-NNNNNN.ppm next to the frame dumps (KHNMC_GFX_DUMP) when they
 * are on. */
#define _GNU_SOURCE
#ifdef KH_GFX_VKD3D
#define COBJMACROS
#ifndef WIDL_C_INLINE_WRAPPERS
#define WIDL_C_INLINE_WRAPPERS
#endif
#include <vkd3d_windows.h>
#include <vkd3d_d3d12.h>
#include <vkd3d.h>
#include <vkd3d_utils.h>
#endif
#include "khnmc_rt.h"
#include "gfx_present.h"
#include <errno.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define P_(x) ((unsigned long long)(x))
static void ns_to_ts(u64 ns,struct timespec *ts){ ts->tv_sec=(time_t)(ns/1000000000ull); ts->tv_nsec=(long)(ns%1000000000ull); }
/* the shell's presenter calls, counted in every build (section 2b; review V36 n=5) */
extern int kh_gfx_vkd3d_on;
static _Atomic u64 sh_layer_calls, sh_size_calls, sh_size_zero; static _Atomic int sh_presenter_layer; static const char *sh_env_mode="off (unset)";

/* ==== 1. the suspend gate ======================================================================================================= */
static pthread_mutex_t gate_m=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static _Atomic int gate_on, gate_hold_fences; static _Atomic u64 gate_queue_waits, gate_last_clear_ns;
static u64 gate_sets, gate_parks, gate_parked_ns, gate_fence_parks;
#define GATE_PLACES 12
static struct { const char *where; u64 n; } gate_place[GATE_PLACES]; static int gate_nplace; static u64 gate_place_more;
void kh_gfx_gate_set(void){ pthread_mutex_lock(&gate_m); if(!gate_on){ atomic_store(&gate_on,1); gate_sets++; } pthread_mutex_unlock(&gate_m); }
void kh_gfx_gate_clear(void){ pthread_mutex_lock(&gate_m); atomic_store(&gate_on,0); atomic_store(&gate_last_clear_ns,kh_host_ns()); pthread_cond_broadcast(&gate_cv); pthread_mutex_unlock(&gate_m); }
int  kh_gfx_gate_is_set(void){ return atomic_load(&gate_on); }
void kh_gfx_gate_hold_fence_signals(int on){ atomic_store(&gate_hold_fences,on?1:0); }
void kh_gfx_gate_note_queue_wait(void){ atomic_fetch_add(&gate_queue_waits,1); }
u64  kh_gfx_gate_last_clear_ns(void){ return atomic_load(&gate_last_clear_ns); }
uint64_t kh_gfx_gate_parks(void){ pthread_mutex_lock(&gate_m); u64 n=gate_parks; pthread_mutex_unlock(&gate_m); return n; }
/* caller holds gate_m and saw the gate set */
static void gate_wait_locked(const char *where){
    int i; for(i=0;i<gate_nplace;i++) if(gate_place[i].where==where||!strcmp(gate_place[i].where,where)) break;
    if(i==gate_nplace){ if(gate_nplace<GATE_PLACES){ gate_place[i].where=where; gate_place[i].n=0; gate_nplace++; } else i=-1; }
    if(i>=0) gate_place[i].n++; else gate_place_more++;
    gate_parks++; u64 t0=kh_host_ns();
    while(atomic_load(&gate_on)) pthread_cond_wait(&gate_cv,&gate_m);     /* parked: no runtime lock is held by the caller */
    gate_parked_ns+=kh_host_ns()-t0;
}
void kh_gfx_gate_park(const char *where){
    if(!atomic_load(&gate_on)) return;
    pthread_mutex_lock(&gate_m); if(atomic_load(&gate_on)) gate_wait_locked(where); pthread_mutex_unlock(&gate_m);
}
/* ID3D12Fence::Signal from the CPU: held only while a queue may be blocked (fact 11 of the plan): fence signals were put on hold
 * (step 11, a drain that did not complete) or a queue Wait was ever seen */
int kh_gfx_gate_park_fence_signal(void){
    if(!atomic_load(&gate_on)) return 0;
    if(!atomic_load(&gate_hold_fences)&&!atomic_load(&gate_queue_waits)) return 0;
    pthread_mutex_lock(&gate_m); int parked=0;
    if(atomic_load(&gate_on)){ gate_fence_parks++; gate_wait_locked("ID3D12Fence::Signal (a queue may be blocked)"); parked=1; }
    pthread_mutex_unlock(&gate_m); return parked;
}
void kh_gfx_gate_test_reset_queue_waits(void){ atomic_store(&gate_queue_waits,0); }      /* selftest only */
void kh_gfx_gate_report(FILE *f){
    pthread_mutex_lock(&gate_m);
    fprintf(f,"kh: gfx gate: set %llu time(s), %llu park(s) (%.1f ms parked in all), fence-signal parks %llu, queue Waits seen %llu, fence signals held: %s",
            P_(gate_sets),P_(gate_parks),gate_parked_ns/1e6,P_(gate_fence_parks),P_(atomic_load(&gate_queue_waits)),
            atomic_load(&gate_hold_fences)?"yes":atomic_load(&gate_queue_waits)?"yes (a queue Wait was seen)":"no");
    for(int i=0;i<gate_nplace;i++) fprintf(f,"%s %s=%llu",i?";":"; by place:",gate_place[i].where,P_(gate_place[i].n));
    if(gate_place_more) fprintf(f," +%llu at more places",P_(gate_place_more));
    fputc('\n',f); pthread_mutex_unlock(&gate_m);
}

/* ==== 2. display pacing of WaitForVBlank (host clock) ============================================================================
 * One instance for the process (vs_proc); the selftest makes its own.  Per calling THREAD the tick count it saw at its last return is
 * kept (so a second caller does not inherit the timing thread's state).  A wait:
 *   - a tick came since this thread's last return: return at once (counted late; ticks missed beyond the first are counted too), so a
 *     late timing thread never loses a whole period;
 *   - else wait for the next tick until 2 periods after the LAST tick (absolute); a tick ends it (counted on-tick);
 *   - when that passes: counted time-out, then an ABSOLUTE wait to the next point of the last tick's grid (last + k*period > now) or the
 *     next tick, whichever is first.  Never a relative sleep: a dead source still paces at the period, a late one is not compounded. */
#define VS_PERIOD_NS 16666667ull
typedef struct KhVsync { pthread_mutex_t m; pthread_cond_t cv; int registered, log; u64 ticks, last_tick_ns, period_ns;
    u64 n_wait, n_on_tick, n_late, n_missed, n_timeout, n_fb_deadline, n_fb_tick, wait_ns, n_virtual, n_sleep;
    u64 rate_n, rate_t0, rate_f0; } KhVsync;
static KhVsync vs_proc; static pthread_once_t vs_once=PTHREAD_ONCE_INIT;
static void vs_init_struct(KhVsync *v){ memset(v,0,sizeof *v); pthread_mutex_init(&v->m,NULL); kh_cond_init(&v->cv); v->period_ns=VS_PERIOD_NS; }
static void vs_proc_init(void){ vs_init_struct(&vs_proc); vs_proc.log=1; }
static __thread struct { KhVsync *v; u64 seen; } vs_tls[4];
static u64 *vs_seen(KhVsync *v){                                  /* caller holds v->m; first use by this thread: the ticks so far count as seen */
    for(int i=0;i<4;i++) if(vs_tls[i].v==v) return &vs_tls[i].seen;
    for(int i=0;i<4;i++) if(!vs_tls[i].v){ vs_tls[i].v=v; vs_tls[i].seen=v->ticks; return &vs_tls[i].seen; }
    vs_tls[3].v=v; vs_tls[3].seen=v->ticks; return &vs_tls[3].seen; }
static void vs_forget_thread(KhVsync *v){ for(int i=0;i<4;i++) if(vs_tls[i].v==v) vs_tls[i].v=NULL; }
u64 kh_input_game_frames(void);
/* every 600 host-clock WaitForVBlank calls of the process: the pacing and the game's frame rate over that stretch */
static void vs_rate(KhVsync *v,const char *src){
    if(!v->log) return; u64 now=kh_host_ns(), gf=kh_input_game_frames();
    if(!v->rate_t0){ v->rate_t0=now; v->rate_f0=gf; v->rate_n=0; return; }
    if(++v->rate_n<600) return;
    double s=(now-v->rate_t0)/1e9;
    fprintf(stderr,"kh: vsync: source=%s waits=%llu on-tick=%llu late=%llu (missed ticks %llu) time-outs=%llu (then grid deadline %llu, tick %llu) sleeps=%llu; last 600 waits: %.3f s host, %llu game frames = %.2f game frames per host second, %.2f waits per second\n",
            src,P_(v->n_wait),P_(v->n_on_tick),P_(v->n_late),P_(v->n_missed),P_(v->n_timeout),P_(v->n_fb_deadline),P_(v->n_fb_tick),P_(v->n_sleep),s,P_(gf-v->rate_f0),(gf-v->rate_f0)/s,600/s);
    v->rate_t0=now; v->rate_f0=gf; v->rate_n=0;
}
static int vs_wait(KhVsync *v){
    pthread_mutex_lock(&v->m);
    if(!v->registered){ pthread_mutex_unlock(&v->m); return 0; }
    u64 *seen=vs_seen(v), t0=kh_host_ns(); v->n_wait++;
    if(v->ticks>*seen){ v->n_late++; v->n_missed+=v->ticks-*seen-1; *seen=v->ticks; v->wait_ns+=kh_host_ns()-t0; vs_rate(v,"ticks"); pthread_mutex_unlock(&v->m); return 1; }
    u64 P=v->period_ns; struct timespec dl; ns_to_ts(v->last_tick_ns+2*P,&dl);
    while(v->ticks<=*seen) if(kh_cond_wait_until(&v->cv,&v->m,&dl)==ETIMEDOUT) break;
    if(v->ticks>*seen) v->n_on_tick++;
    else { v->n_timeout++;
        u64 now=kh_host_ns(), base=v->last_tick_ns, k=now>base?(now-base)/P+1:1; ns_to_ts(base+k*P,&dl);
        while(v->ticks<=*seen) if(kh_cond_wait_until(&v->cv,&v->m,&dl)==ETIMEDOUT) break;
        if(v->ticks>*seen) v->n_fb_tick++; else v->n_fb_deadline++; }
    *seen=v->ticks; v->wait_ns+=kh_host_ns()-t0; vs_rate(v,"ticks");
    pthread_mutex_unlock(&v->m); return 1;
}
static void vs_tick(KhVsync *v,u64 ns){ pthread_mutex_lock(&v->m); v->ticks++; v->last_tick_ns=ns; pthread_cond_broadcast(&v->cv); pthread_mutex_unlock(&v->m); }
static void vs_register(KhVsync *v){ pthread_mutex_lock(&v->m); if(!v->registered){ v->registered=1; v->last_tick_ns=kh_host_ns(); } pthread_mutex_unlock(&v->m); }
void kh_vsync_register(void){ pthread_once(&vs_once,vs_proc_init); vs_register(&vs_proc);
    fprintf(stderr,"kh: vsync: a display tick source is registered: WaitForVBlank (host clock) now waits for its ticks (period %llu ns; time-out 2 periods, then the last tick's grid)\n",P_(vs_proc.period_ns)); }
void kh_vsync_tick(uint64_t host_ns){ pthread_once(&vs_once,vs_proc_init); vs_tick(&vs_proc,host_ns); }
int  kh_vsync_wait(void){ pthread_once(&vs_once,vs_proc_init); return vs_wait(&vs_proc); }
static _Atomic u64 vs_virtual_calls;                              /* lock-free: the frames-clock path keeps its exact behaviour */
void kh_vsync_note_virtual(void){ atomic_fetch_add(&vs_virtual_calls,1); }
void kh_vsync_note_sleep(void){ pthread_once(&vs_once,vs_proc_init); pthread_mutex_lock(&vs_proc.m); vs_proc.n_sleep++; vs_rate(&vs_proc,"sleep"); pthread_mutex_unlock(&vs_proc.m); }
/* KHNMC_DISPLAY=timer60: the Linux stand-in for CADisplayLink, a host thread on ABSOLUTE 16.667 ms deadlines */
static void *timer60_main(void *a){ (void)a; u64 base=kh_host_ns(); struct timespec dl;
    for(u64 k=1;;k++){ ns_to_ts(base+k*VS_PERIOD_NS,&dl); kh_sleep_until(&dl); kh_vsync_tick(kh_host_ns()); } return NULL; }
static const char *vs_source="none";
void kh_vsync_init(void){
    pthread_once(&vs_once,vs_proc_init);
    const char *e=getenv("KHNMC_DISPLAY"); if(!e||!*e) return;
    if(strcmp(e,"timer60")) kh_fatal(NULL,"display-source-unknown: KHNMC_DISPLAY=%s (known: timer60 = a host thread that ticks on absolute 16.667 ms deadlines, the Linux stand-in for the display link)",e);
    vs_source="timer60"; kh_vsync_register();
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    if(pthread_create(&t,&at,timer60_main,NULL)) kh_fatal(NULL,"display-source-thread: KHNMC_DISPLAY=timer60: the tick thread cannot be started");
    pthread_attr_destroy(&at);
    fprintf(stderr,"kh: vsync: KHNMC_DISPLAY=timer60: tick thread started (absolute 16.667 ms deadlines)%s\n",kh_vclock_on?"; KHNMC_CLOCK=frames answers WaitForVBlank with its virtual vblank, the ticks are counted as ignored":"");
}
void kh_vsync_report(FILE *f){
    pthread_once(&vs_once,vs_proc_init); KhVsync *v=&vs_proc; pthread_mutex_lock(&v->m);
    fprintf(f,"kh: vsync: source=%s registered=%d ticks=%llu; WaitForVBlank: tick-paced %llu (on a tick %llu, late %llu, missed ticks %llu, time-outs %llu then grid deadline %llu / tick %llu; mean wait %.3f ms), virtual vblank %llu%s, 16.67 ms sleep %llu\n",
            vs_source,v->registered,P_(v->ticks),P_(v->n_wait),P_(v->n_on_tick),P_(v->n_late),P_(v->n_missed),P_(v->n_timeout),P_(v->n_fb_deadline),P_(v->n_fb_tick),
            v->n_wait?v->wait_ns/1e6/(double)v->n_wait:0.0,P_(atomic_load(&vs_virtual_calls)),v->registered&&atomic_load(&vs_virtual_calls)?" (ticks ignored: KHNMC_CLOCK=frames)":"",P_(v->n_sleep));
    pthread_mutex_unlock(&v->m);
}
/* ---- selftest of the pacing (every --selftest; its own instance, its own threads) ---- */
typedef struct { KhVsync *v; _Atomic int state; u64 ret_ns; int rounds; } VsT;
static void *vs_test_waiter(void *a){ VsT *t=(VsT*)a; for(int i=0;i<t->rounds;i++){ atomic_store(&t->state,1); vs_wait(t->v); t->ret_ns=kh_host_ns(); atomic_store(&t->state,2+i); }
    vs_forget_thread(t->v); return NULL; }
static int vs_blocked(KhVsync *v,int want){ for(int i=0;i<4000;i++){ pthread_mutex_lock(&v->m); int ok=(int)(v->n_wait-v->n_late-v->n_on_tick-v->n_fb_deadline-v->n_fb_tick)>=want; pthread_mutex_unlock(&v->m); if(ok) return 1; struct timespec ts={0,250000}; nanosleep(&ts,0); } return 0; }
static void ms_sleep(unsigned ms){ struct timespec ts={(time_t)(ms/1000),(long)(ms%1000)*1000000L}; nanosleep(&ts,0); }
int kh_vsync_selftest(void){
    int n=0,f=0;
#define TV(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"vsync selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    KhVsync v; vs_init_struct(&v);
    TV(vs_wait(&v)==0);                                                      /* no source: the caller sleeps as before */
    vs_register(&v); v.period_ns=40000000ull;                                /* 40 ms period: the bounds below have room on a loaded host */
    /* 1. a waiter returns on a tick, not before */
    { VsT t={&v,0,0,1}; pthread_t th; pthread_create(&th,NULL,vs_test_waiter,&t); TV(vs_blocked(&v,1));
      ms_sleep(15); TV(atomic_load(&t.state)==1); u64 tk=kh_host_ns(); vs_tick(&v,tk); pthread_join(th,NULL);
      TV(atomic_load(&t.state)==2 && t.ret_ns>=tk && v.n_on_tick==1 && v.n_timeout==0); }
    /* 2. the late path: two ticks come while nobody waits; the next wait of that thread returns at once (1 missed tick counted) */
    { u64 *s; pthread_mutex_lock(&v.m); s=vs_seen(&v); *s=v.ticks; pthread_mutex_unlock(&v.m);   /* this thread: everything so far seen */
      vs_tick(&v,kh_host_ns()); vs_tick(&v,kh_host_ns()); u64 t0=kh_host_ns(); TV(vs_wait(&v)==1); u64 dt=kh_host_ns()-t0;
      TV(dt<10000000ull && v.n_late==1 && v.n_missed==1); }
    /* 3. the time-out path, with a dead source: period 100 ms, last tick at lt, the wait is called at lt + 250 ms.  Its time-out (2
     *    periods after the LAST tick, lt + 200) has passed: counted at once, then the wait ends on the last tick's grid at lt + 300.  A
     *    relative sleep of one period from the call would end at lt + 350, a time-out counted from the call at lt + 450 or later. */
    { v.period_ns=100000000ull; u64 lt=kh_host_ns(); vs_tick(&v,lt); u64 *s; pthread_mutex_lock(&v.m); s=vs_seen(&v); *s=v.ticks; pthread_mutex_unlock(&v.m);
      while(kh_host_ns()<lt+250000000ull) ms_sleep(1);
      TV(vs_wait(&v)==1); u64 end=kh_host_ns();
      TV(v.n_timeout==1 && v.n_fb_deadline==1 && end>=lt+300000000ull && end<lt+330000000ull);
      v.period_ns=40000000ull; }
    /* 4. two calling threads, each waits for ITS next tick: one tick releases both (a state shared between threads would let only the
     *    first one see a new tick; the other would end by its time-out) */
    { VsT a={&v,0,0,1}, b={&v,0,0,1}; pthread_t ta,tb; u64 w0,t0;
      pthread_mutex_lock(&v.m); w0=v.n_on_tick; t0=v.n_timeout; pthread_mutex_unlock(&v.m);
      vs_tick(&v,kh_host_ns());                                              /* a fresh last tick: the time-outs below are 2 periods away */
      pthread_create(&ta,NULL,vs_test_waiter,&a); pthread_create(&tb,NULL,vs_test_waiter,&b); TV(vs_blocked(&v,2));
      vs_tick(&v,kh_host_ns()); pthread_join(ta,NULL); pthread_join(tb,NULL);
      TV(atomic_load(&a.state)==2 && atomic_load(&b.state)==2 && v.n_on_tick==w0+2 && v.n_timeout==t0); }
    vs_forget_thread(&v); pthread_mutex_destroy(&v.m); pthread_cond_destroy(&v.cv);
    /* 5. (review V36 n=5) the shell's presenter calls are counted in every build, and a dropped one is reported at exit.
     *    kh_present_set_size(0,0) changes nothing in any build (a zero size is counted and ignored). */
    { int kh_present_shell_line(char *buf,size_t n); u64 s0=atomic_load(&sh_size_calls), z0=atomic_load(&sh_size_zero); char buf[512];
      kh_present_set_size(0,0); TV(atomic_load(&sh_size_calls)==s0+1 && atomic_load(&sh_size_zero)==z0+1);
      TV(kh_present_shell_line(buf,sizeof buf) && (strstr(buf,"DROPPED")!=NULL || atomic_load(&sh_presenter_layer)));
      atomic_store(&sh_size_calls,s0); atomic_store(&sh_size_zero,z0); }
#ifndef KH_GFX_VKD3D
    { u64 l0=atomic_load(&sh_layer_calls); kh_present_set_layer((void*)16,64,64); TV(atomic_load(&sh_layer_calls)==l0+1); atomic_store(&sh_layer_calls,l0); }
#endif
    fprintf(stderr,"kh: vsync selftest: %d checks, %d failures (tick, late path, time-out with the absolute grid, two calling threads; the shell's presenter calls counted)\n",n,f);
#undef TV
    return f;
}

/* ==== 2b. the presenter settings in every build, and the shell's calls (review V36 n=5) ==========================================
 * KHNMC_GFX_PRESENT is checked at start-up (null_d3d12.c, after the backend is chosen) whatever the backend: a value other than off
 * without an active vkd3d backend is the named fatal gfx-present-needs-backend (it used to be read only by the vkd3d backend: no
 * picture, no message, and even an invalid value passed).  Presenter settings while it is off are reported as ignored.  The shell's
 * kh_present_set_layer / kh_present_set_size are counted in every build; at exit (loader.c) a line says whether they were used or
 * DROPPED (no backend in this build, the backend not active, or no presenter with the layer sink). */
void kh_present_init(int backend_on){
    const char *e=getenv("KHNMC_GFX_PRESENT"); int m=0;
    if(e&&*e){ if(!strcmp(e,"off")) m=0; else if(!strcmp(e,"headless")) m=1; else if(!strcmp(e,"layer")) m=2;
        else kh_fatal(NULL,"gfx-present-mode: KHNMC_GFX_PRESENT=%s (off, headless = VK_EXT_headless_surface, layer = the shell's CAMetalLayer)",e); }
    if(m&&!backend_on){
#ifdef KH_GFX_VKD3D
        kh_fatal(NULL,"gfx-present-needs-backend: KHNMC_GFX_PRESENT=%s needs the vkd3d graphics backend (KHNMC_GFX=vkd3dw or vkd3d), which this run does not use (KHNMC_GFX is null or unset): there would be no picture",e);
#else
        kh_fatal(NULL,"gfx-present-needs-backend: KHNMC_GFX_PRESENT=%s needs the vkd3d graphics backend, which is not compiled into this binary (GFX_VKD3D=0, as in every IOS_PATHS=1 build): there would be no picture",e);
#endif
    }
    sh_env_mode=m==1?"headless":m==2?"layer":e&&*e?"off":"off (unset)";
    if(!m){ static const char *const ks[]={"KHNMC_GFX_PRESENT_SIZE","KHNMC_GFX_PRESENT_FIT","KHNMC_GFX_PRESENT_CHECK","KHNMC_GFX_PRESENT_TOL"};
        for(int i=0;i<4;i++){ const char *v=getenv(ks[i]); if(v&&*v) fprintf(stderr,"kh: gfx present: %s=%s IGNORED: the presenter is off (KHNMC_GFX_PRESENT=%s)\n",ks[i],v,sh_env_mode); } }
}
/* the exit line about the shell's calls; 0 = there were none (no line) */
int kh_present_shell_line(char *buf,size_t n){
    u64 l=atomic_load(&sh_layer_calls), s=atomic_load(&sh_size_calls), z=atomic_load(&sh_size_zero); if(!l&&!s) return 0;
#ifdef KH_GFX_VKD3D
    const char *why=atomic_load(&sh_presenter_layer)?"used by the presenter (layer sink)":!kh_gfx_vkd3d_on?"DROPPED: the vkd3d backend is not active in this run (KHNMC_GFX)":"DROPPED: no presenter with the layer sink was attached (KHNMC_GFX_PRESENT=";
#else
    const char *why="DROPPED: this build has no graphics backend (GFX_VKD3D=0)";
#endif
    int k=snprintf(buf,n,"kh: gfx present: the shell handed over a layer %llu time(s) and a size %llu time(s) (%llu of them 0x0, ignored): %s%s",P_(l),P_(s),P_(z),why,
                   why[strlen(why)-1]=='='?sh_env_mode:"");
    if(k>0&&why[strlen(why)-1]=='='&&(size_t)k+1<n) strcat(buf,")");
    return 1;
}
void kh_present_shell_report(FILE *f){ char buf[512]; if(kh_present_shell_line(buf,sizeof buf)) fprintf(f,"%s\n",buf); }

#ifdef KH_GFX_VKD3D
/* ==== 3. the Vulkan presenter ================================================================================================== */
#include "gfx_vkd3d_int.h"
static pthread_mutex_t pm_g=PTHREAD_MUTEX_INITIALIZER;
/* ---- Vulkan functions per device (core: also for the own back buffers of gfx_vkd3d.c; WSI: only when presenting) ---- */
#define VK_INST_FNS(X) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) X(GetDeviceProcAddr) X(EnumerateDeviceExtensionProperties)
#define VK_INST_WSI(X) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) X(DestroySurfaceKHR)
#define VK_DEV_FNS(X) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdBlitImage) X(CmdClearColorImage) X(CmdCopyImageToBuffer) X(CreateSemaphore) X(DestroySemaphore) \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(GetFenceStatus) X(QueueSubmit) X(QueueWaitIdle)
#define VK_DEV_WSI(X) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)
#define VK_FIELD(n) PFN_vk##n n;
struct KhVk { VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkPhysicalDeviceMemoryProperties mp;
    VK_INST_FNS(VK_FIELD) VK_INST_WSI(VK_FIELD) VK_DEV_FNS(VK_FIELD) VK_DEV_WSI(VK_FIELD) PFN_vkCreateHeadlessSurfaceEXT CreateHeadlessSurfaceEXT;
#ifdef VK_USE_PLATFORM_METAL_EXT
    PFN_vkCreateMetalSurfaceEXT CreateMetalSurfaceEXT;
#endif
};
static KhVk vk_tab[8]; static int n_vk_tab;
const KhVk *kh_vk_for(ID3D12Device *d){
    VkDevice vd=vkd3d_get_vk_device(d);
    pthread_mutex_lock(&pm_g); for(int i=0;i<n_vk_tab;i++) if(vk_tab[i].dev==vd){ pthread_mutex_unlock(&pm_g); return &vk_tab[i]; }
    if(n_vk_tab>=8){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-vk-table: more than 8 vkd3d devices"); }
    KhVk *v=&vk_tab[n_vk_tab]; memset(v,0,sizeof *v);
    PFN_vkGetInstanceProcAddr gipa=kh_gfxc_vk_gipa_real();
    v->dev=vd; v->pd=vkd3d_get_vk_physical_device(d); v->inst=vkd3d_instance_get_vk_instance(vkd3d_instance_from_device(d));
#define VLI(n) v->n=(PFN_vk##n)gipa(v->inst,"vk" #n); if(!v->n){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-vk-proc: the Vulkan instance has no vk" #n); }
#define VLIO(n) v->n=(PFN_vk##n)gipa(v->inst,"vk" #n);
    VK_INST_FNS(VLI) VK_INST_WSI(VLIO)
    v->CreateHeadlessSurfaceEXT=(PFN_vkCreateHeadlessSurfaceEXT)gipa(v->inst,"vkCreateHeadlessSurfaceEXT");
#ifdef VK_USE_PLATFORM_METAL_EXT
    v->CreateMetalSurfaceEXT=(PFN_vkCreateMetalSurfaceEXT)gipa(v->inst,"vkCreateMetalSurfaceEXT");
#endif
#define VLD(n) v->n=(PFN_vk##n)v->GetDeviceProcAddr(vd,"vk" #n); if(!v->n){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-vk-proc: the Vulkan device has no vk" #n); }
#define VLDO(n) v->n=(PFN_vk##n)v->GetDeviceProcAddr(vd,"vk" #n);
    VK_DEV_FNS(VLD) VK_DEV_WSI(VLDO)
#undef VLI
#undef VLIO
#undef VLD
#undef VLDO
    v->GetPhysicalDeviceMemoryProperties(v->pd,&v->mp);
    n_vk_tab++; pthread_mutex_unlock(&pm_g); return v;
}
VkResult kh_vk_create_image(const KhVk *v,const VkImageCreateInfo *ci,VkImage *out){ return v->CreateImage(v->dev,ci,NULL,out); }
void     kh_vk_image_memreq(const KhVk *v,VkImage im,VkMemoryRequirements *mr){ v->GetImageMemoryRequirements(v->dev,im,mr); }
VkResult kh_vk_alloc(const KhVk *v,const VkMemoryAllocateInfo *ai,VkDeviceMemory *out){ return v->AllocateMemory(v->dev,ai,NULL,out); }
VkResult kh_vk_bind_image(const KhVk *v,VkImage im,VkDeviceMemory m){ return v->BindImageMemory(v->dev,im,m,0); }
void     kh_vk_destroy_image(const KhVk *v,VkImage im){ v->DestroyImage(v->dev,im,NULL); }
void     kh_vk_free(const KhVk *v,VkDeviceMemory m){ v->FreeMemory(v->dev,m,NULL); }
/* the first memory type of `bits` that has all of `want` (vkd3d's own rule for a DEFAULT heap: the first DEVICE_LOCAL one) */
int kh_vk_memtype(const KhVk *v,u32 bits,VkMemoryPropertyFlags want){
    for(u32 i=0;i<v->mp.memoryTypeCount;i++) if((bits&(1u<<i))&&(v->mp.memoryTypes[i].propertyFlags&want)==want) return (int)i; return -1; }
/* live Vulkan objects made by this runtime (presenter + own back buffers): every create and destroy is counted */
static const char *const vko_names[VKO_N]={"surface","swapchain","semaphore","fence","command pool","buffer","memory","image"};
_Atomic long long kh_vko_live[VKO_N]; static _Atomic u64 vko_made[VKO_N];
void kh_vko_note(int kind,int made){ if(made){ atomic_fetch_add(&kh_vko_live[kind],1); atomic_fetch_add(&vko_made[kind],1); } else atomic_fetch_sub(&kh_vko_live[kind],1); }
static u64 n_vko_made_images(void){ return atomic_load(&vko_made[VKO_IMAGE]); }
long long kh_vko_live_total(void){ long long t=0; for(int k=0;k<VKO_N;k++) t+=atomic_load(&kh_vko_live[k]); return t; }
#define VKC(expr,what) do{ VkResult r_=(expr); if(r_!=VK_SUCCESS) kh_fatal(NULL,"gfx-present-vk: %s failed: VkResult %d",what,(int)r_); }while(0)

/* ---- settings ---- */
static int pr_mode=-1, pr_fit, pr_check; static u32 pr_w=1920, pr_h=1080; static double pr_tol_max=2.0, pr_tol_mean=0.5;
static _Atomic u32 pr_gen; static void *pr_layer;           /* the shell's CAMetalLayer (Apple), and a generation that makes swapchains stale */
static const char *const mode_names[]={"off","headless","layer"};
static int pr_env_mode, pr_env_fit; static u32 pr_env_w, pr_env_h;      /* what the environment said (a selftest override goes back to it) */
static void settings(void){
    pthread_mutex_lock(&pm_g); if(pr_mode>=0){ pthread_mutex_unlock(&pm_g); return; }
    const char *e=getenv("KHNMC_GFX_PRESENT"); int m=0;
    if(e&&*e){ if(!strcmp(e,"off")) m=0; else if(!strcmp(e,"headless")) m=1; else if(!strcmp(e,"layer")) m=2;
        else { pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-mode: KHNMC_GFX_PRESENT=%s (off, headless = VK_EXT_headless_surface, layer = the shell's CAMetalLayer)",e); } }
#ifndef VK_USE_PLATFORM_METAL_EXT
    if(m==2){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-layer-unavailable: KHNMC_GFX_PRESENT=layer needs VK_EXT_metal_surface and a CAMetalLayer from the iOS/macOS shell; this build has no Metal surface (use headless)"); }
#endif
    e=getenv("KHNMC_GFX_PRESENT_SIZE");
    if(e&&*e){ unsigned w=0,h=0; char x=0; if(sscanf(e,"%u%c%u",&w,&x,&h)!=3||(x!='x'&&x!='X')||!w||!h||w>16384||h>16384){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-size: KHNMC_GFX_PRESENT_SIZE=%s (WxH, 1..16384)",e); } pr_w=w; pr_h=h; }
    e=getenv("KHNMC_GFX_PRESENT_FIT"); pr_fit=0;
    if(e&&*e){ if(!strcmp(e,"aspect")) pr_fit=0; else if(!strcmp(e,"stretch")) pr_fit=1; else { pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-fit: KHNMC_GFX_PRESENT_FIT=%s (aspect = black bars, stretch)",e); } }
    e=getenv("KHNMC_GFX_PRESENT_CHECK"); pr_check=0;
    if(e&&*e){ char *end; long v=strtol(e,&end,10); if(*end||v<0){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-check: KHNMC_GFX_PRESENT_CHECK=%s (n = every n-th present, 0 = off)",e); } pr_check=(int)v; }
    e=getenv("KHNMC_GFX_PRESENT_TOL");
    if(e&&*e){ double a,b; char c; if(sscanf(e,"%lf,%lf%c",&a,&b,&c)!=2||a<0||b<0){ pthread_mutex_unlock(&pm_g); kh_fatal(NULL,"gfx-present-tol: KHNMC_GFX_PRESENT_TOL=%s (max,mean in levels)",e); } pr_tol_max=a; pr_tol_mean=b; }
    pr_mode=m; pr_env_mode=m; pr_env_fit=pr_fit; pr_env_w=pr_w; pr_env_h=pr_h; pthread_mutex_unlock(&pm_g);
    if(m) fprintf(stderr,"kh: gfx present: KHNMC_GFX_PRESENT=%s%s, fit %s, check %s%s (tolerance max %.1f / mean %.2f levels)\n",mode_names[m],m==1?"":" (the shell's layer)",pr_fit?"stretch":"aspect",pr_check?"every ":"off",pr_check?"n-th present":"",pr_tol_max,pr_tol_mean);
}
int kh_present_mode(void){ settings(); return pr_mode; }
void kh_present_set_layer(void *layer,uint32_t w,uint32_t h){ atomic_fetch_add(&sh_layer_calls,1); pthread_mutex_lock(&pm_g); pr_layer=layer; if(w&&h){ pr_w=w; pr_h=h; } atomic_fetch_add(&pr_gen,1); pthread_mutex_unlock(&pm_g); }
/* a 0x0 size is counted and ignored (the swapchain keeps its size; a zero-size drawable is what the surface reports, see swapchain_build) */
void kh_present_set_size(uint32_t w,uint32_t h){ atomic_fetch_add(&sh_size_calls,1); if(!w||!h){ atomic_fetch_add(&sh_size_zero,1); return; } pthread_mutex_lock(&pm_g); pr_w=w; pr_h=h; atomic_fetch_add(&pr_gen,1); pthread_mutex_unlock(&pm_g); }

/* ---- the presenter object ---- */
#define PSLOTS 3
#define PMAXIMG 8
enum { RC_FIRST, RC_SIZE, RC_OUTDATED, RC_SUBOPTIMAL, RC_SURFACE, RC_RESUME, RC_N };
static const char *const rc_names[RC_N]={"first","size","out-of-date","suboptimal","surface-lost","after-park"};
typedef struct { VkCommandBuffer cb; VkFence fence; VkSemaphore acq; int pending, acquired, img, check; } PSlot;
struct KhPresenter { pthread_mutex_t m; const KhVk *vk; ID3D12Device *dev; ID3D12CommandQueue *queue; ID3D12Fence *fence; u64 fence_val; u32 family;
    int mode, fit, check, general; u32 gen; VkSurfaceKHR surface; VkSwapchainKHR sc; VkExtent2D ext; u32 nimg; VkImage simg[PMAXIMG]; VkSemaphore rsem[PMAXIMG];
    int stale, stale_reason, sc_src_ok, linear_ok; VkCommandPool pool; PSlot slot[PSLOTS]; u64 frames; KhPresentBuffers bb; VkFormat fmt;
    VkBuffer hb[2]; VkDeviceMemory hm[2]; u8 *hp[2]; u64 hsz[2]; int counted_timeouts; int busy;
    int made;                                       /* a swapchain was made once: later builds are counted as rebuilds by their reason */
    int chk_pending, chk_slot; u64 chk_n; u32 chk[8];   /* a check present waiting for its reference (kh_present_check): sw sh dx dy dw dh bw bh */
    int lchk_valid; u32 lchk[8]; };                 /* the last checked present (selftest: its copy against a CPU-made picture) */
/* counters, all presenters */
static _Atomic u64 c_frames, c_submits, c_presents, c_rebuild[RC_N], c_to_counted, c_to_free, c_parked_acq, c_sem_remade, c_consumed, c_slot_waits,
    c_cpu_spins, c_check_ok, c_check_fail, c_check_skipped, c_check_bars_bad, c_check_src_bad, c_sc_suboptimal, c_sc_outdated, c_sc_surface_lost;
/* selftest injections (review V36 n=3: the headless surface never loses itself and never has a zero size) */
static VkResult test_inject_present, test_inject_acquire; static int test_inject_zero;
static u64 st_cost_n, st_cost_sum, st_cost_max, st_acq_sum, st_acq_max, st_hold_sum, st_hold_max; static double st_dmax, st_dmean_max;
#define HBUCK 2000
static u64 cost_hist[HBUCK+1];                               /* present cost (d)-(f) in 10-us buckets, last = 20 ms and more */
static void (*test_after_acquire)(KhPresenter *);
static int  test_force_check=-1;                             /* selftest: check every present (1) */
static u64 present_number_last;

static VkSemaphore mk_sem(const KhVk *v){ VkSemaphoreCreateInfo ci; memset(&ci,0,sizeof ci); ci.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; VkSemaphore s; VKC(v->CreateSemaphore(v->dev,&ci,NULL,&s),"vkCreateSemaphore"); kh_vko_note(VKO_SEM,1); return s; }
static void del_sem(const KhVk *v,VkSemaphore *s){ if(*s){ v->DestroySemaphore(v->dev,*s,NULL); kh_vko_note(VKO_SEM,0); *s=VK_NULL_HANDLE; } }
/* wait for a slot's fence in slices (a stopped process costs one slice); 5 expired slices are a named fatal */
static void slot_wait(KhPresenter *p,PSlot *s,const char *why){
    if(!s->pending) return; unsigned expired=0; atomic_fetch_add(&c_slot_waits,1);
    for(;;){ VkResult r=p->vk->WaitForFences(p->vk->dev,1,&s->fence,VK_TRUE,(u64)kh_stuck_slice_ms*1000000ull);
        if(r==VK_SUCCESS) break;
        if(r!=VK_TIMEOUT) kh_fatal(NULL,"gfx-present-vk: vkWaitForFences (%s) failed: VkResult %d",why,(int)r);
        if(++expired>=5) kh_fatal(NULL,"gfx-present-slot-timeout: the presenter's submission did not complete within 5 slices of %u ms (%s)",kh_stuck_slice_ms,why); }
    s->pending=0;
}
static void hostbuf(KhPresenter *p,int k,u64 size){
    const KhVk *v=p->vk; if(p->hsz[k]>=size) return;
    if(p->hb[k]){ v->UnmapMemory(v->dev,p->hm[k]); v->DestroyBuffer(v->dev,p->hb[k],NULL); kh_vko_note(VKO_BUF,0); v->FreeMemory(v->dev,p->hm[k],NULL); kh_vko_note(VKO_MEM,0); p->hb[k]=VK_NULL_HANDLE; p->hsz[k]=0; }
    VkBufferCreateInfo bi; memset(&bi,0,sizeof bi); bi.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size=size; bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKC(v->CreateBuffer(v->dev,&bi,NULL,&p->hb[k]),"vkCreateBuffer(check copy)"); kh_vko_note(VKO_BUF,1);
    VkMemoryRequirements mr; v->GetBufferMemoryRequirements(v->dev,p->hb[k],&mr);
    int t=kh_vk_memtype(v,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); if(t<0) kh_fatal(NULL,"gfx-present-vk: no host-visible coherent memory type for the check copy");
    VkMemoryAllocateInfo ma; memset(&ma,0,sizeof ma); ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ma.allocationSize=mr.size; ma.memoryTypeIndex=(u32)t;
    VKC(v->AllocateMemory(v->dev,&ma,NULL,&p->hm[k]),"vkAllocateMemory(check copy)"); kh_vko_note(VKO_MEM,1);
    VKC(v->BindBufferMemory(v->dev,p->hb[k],p->hm[k],0),"vkBindBufferMemory(check copy)");
    VKC(v->MapMemory(v->dev,p->hm[k],0,VK_WHOLE_SIZE,0,(void**)&p->hp[k]),"vkMapMemory(check copy)"); p->hsz[k]=size;
}
static void surface_make(KhPresenter *p){
    const KhVk *v=p->vk;
    if(p->mode==1){
        if(!v->CreateHeadlessSurfaceEXT||!kh_gfxc_pres_ext.headless) kh_fatal(NULL,"gfx-present-no-headless: KHNMC_GFX_PRESENT=headless but VK_EXT_headless_surface is not enabled on the instance (presentation extensions line)");
        VkHeadlessSurfaceCreateInfoEXT hs; memset(&hs,0,sizeof hs); hs.sType=VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        VKC(v->CreateHeadlessSurfaceEXT(v->inst,&hs,NULL,&p->surface),"vkCreateHeadlessSurfaceEXT");
    } else {
#ifdef VK_USE_PLATFORM_METAL_EXT
        if(!pr_layer) kh_fatal(NULL,"gfx-present-no-layer: KHNMC_GFX_PRESENT=layer but the shell has not called kh_present_set_layer");
        if(!v->CreateMetalSurfaceEXT||!kh_gfxc_pres_ext.metal) kh_fatal(NULL,"gfx-present-no-metal-surface: VK_EXT_metal_surface is not enabled on the instance");
        VkMetalSurfaceCreateInfoEXT ms; memset(&ms,0,sizeof ms); ms.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT; ms.pLayer=(const CAMetalLayer*)pr_layer;
        VKC(v->CreateMetalSurfaceEXT(v->inst,&ms,NULL,&p->surface),"vkCreateMetalSurfaceEXT");
#else
        kh_fatal(NULL,"gfx-present-layer-unavailable: no Metal surface in this build");
#endif
    }
    kh_vko_note(VKO_SURFACE,1);
    VkBool32 sup=0; VKC(v->GetPhysicalDeviceSurfaceSupportKHR(v->pd,p->family,p->surface,&sup),"vkGetPhysicalDeviceSurfaceSupportKHR");
    if(!sup) kh_fatal(NULL,"gfx-present-surface-unsupported: the surface cannot be presented from queue family %u (the game's queue)",p->family);
}
/* wait until our earlier presents and submissions are done (holding the queue briefly): before an old swapchain is destroyed */
static void queue_idle(KhPresenter *p){ VkQueue q=vkd3d_acquire_vk_queue(p->queue); VkResult r=p->vk->QueueWaitIdle(q); vkd3d_release_vk_queue(p->queue); if(r!=VK_SUCCESS) kh_fatal(NULL,"gfx-present-vk: vkQueueWaitIdle failed: VkResult %d",(int)r); }
static void swapchain_destroy(KhPresenter *p,VkSwapchainKHR sc,u32 nimg,VkSemaphore *rsem){
    for(u32 i=0;i<nimg;i++) del_sem(p->vk,&rsem[i]);
    if(sc){ p->vk->DestroySwapchainKHR(p->vk->dev,sc,NULL); kh_vko_note(VKO_SWAPCHAIN,0); }
}
/* the surface is lost (vkAcquireNextImageKHR or vkQueuePresentKHR said SURFACE_LOST): the swapchain and the surface are destroyed after
 * our work is done, and the next Present makes both anew (review V36 n=3: the present path used to keep the lost surface) */
static void surface_drop(KhPresenter *p){
    const KhVk *v=p->vk; atomic_fetch_add(&c_sc_surface_lost,1); queue_idle(p);
    swapchain_destroy(p,p->sc,p->nimg,p->rsem); p->sc=VK_NULL_HANDLE; p->nimg=0;
    if(p->surface){ v->DestroySurfaceKHR(v->inst,p->surface,NULL); kh_vko_note(VKO_SURFACE,0); p->surface=VK_NULL_HANDLE; }
    p->stale=1; p->stale_reason=RC_SURFACE;
}
static void swapchain_build(KhPresenter *p){
    const KhVk *v=p->vk; int reason=p->made?p->stale_reason:RC_FIRST;     /* after a dropped swapchain (surface lost, zero size) too */
    if(!v->CreateSwapchainKHR||!v->GetPhysicalDeviceSurfaceCapabilitiesKHR) kh_fatal(NULL,"gfx-present-no-swapchain: KHNMC_GFX_PRESENT=%s but VK_KHR_swapchain / VK_KHR_surface are not enabled (presentation extensions line)",mode_names[p->mode]);
    if(!p->surface) surface_make(p);
    VkSurfaceCapabilitiesKHR caps; VKC(v->GetPhysicalDeviceSurfaceCapabilitiesKHR(v->pd,p->surface,&caps),"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    u32 nf=0; VKC(v->GetPhysicalDeviceSurfaceFormatsKHR(v->pd,p->surface,&nf,NULL),"vkGetPhysicalDeviceSurfaceFormatsKHR"); VkSurfaceFormatKHR fm[64]; if(nf>64) nf=64;
    VkResult fr=v->GetPhysicalDeviceSurfaceFormatsKHR(v->pd,p->surface,&nf,fm); if(fr!=VK_SUCCESS&&fr!=VK_INCOMPLETE) kh_fatal(NULL,"gfx-present-vk: vkGetPhysicalDeviceSurfaceFormatsKHR: VkResult %d",(int)fr);
    int have=0; for(u32 i=0;i<nf;i++) if(fm[i].format==VK_FORMAT_B8G8R8A8_UNORM&&fm[i].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) have=1;
    if(!have) kh_fatal(NULL,"gfx-present-format: the surface offers no B8G8R8A8_UNORM / SRGB_NONLINEAR format (%u formats)",nf);
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) kh_fatal(NULL,"gfx-present-no-transfer-dst: the surface's images do not support TRANSFER_DST (usage 0x%x): the draw path of risk R6 is not implemented",caps.supportedUsageFlags);
    pthread_mutex_lock(&pm_g); u32 want_w=pr_w, want_h=pr_h; p->gen=atomic_load(&pr_gen); pthread_mutex_unlock(&pm_g);
    VkExtent2D ex=caps.currentExtent; if(ex.width==0xFFFFFFFFu){ ex.width=want_w; ex.height=want_h; }
    if(ex.width<caps.minImageExtent.width) ex.width=caps.minImageExtent.width; if(ex.height<caps.minImageExtent.height) ex.height=caps.minImageExtent.height;
    if(caps.maxImageExtent.width&&ex.width>caps.maxImageExtent.width) ex.width=caps.maxImageExtent.width; if(caps.maxImageExtent.height&&ex.height>caps.maxImageExtent.height) ex.height=caps.maxImageExtent.height;
    if(test_inject_zero) ex.width=ex.height=0;
    /* a zero-size drawable (minimised): no swapchain until it has a size.  An existing one is destroyed (review V36 n=3: it used to stay,
     * and the presenter acquired from and presented into it; an OUT_OF_DATE acquire there was an uncounted spin) */
    if(!ex.width||!ex.height){ if(p->sc){ queue_idle(p); swapchain_destroy(p,p->sc,p->nimg,p->rsem); p->sc=VK_NULL_HANDLE; p->nimg=0; p->stale_reason=RC_SIZE; } p->stale=1; return; }
    u32 cnt=caps.minImageCount<3?3:caps.minImageCount; if(caps.maxImageCount&&cnt>caps.maxImageCount) cnt=caps.maxImageCount;
    int check=p->check>0||test_force_check>0; p->sc_src_ok=check&&(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkSwapchainCreateInfoKHR ci; memset(&ci,0,sizeof ci); ci.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR; ci.surface=p->surface; ci.minImageCount=cnt;
    ci.imageFormat=VK_FORMAT_B8G8R8A8_UNORM; ci.imageColorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; ci.imageExtent=ex; ci.imageArrayLayers=1;
    ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|(p->sc_src_ok?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0); ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform=caps.currentTransform; ci.compositeAlpha=(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)?VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR:(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha&-caps.supportedCompositeAlpha);
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR; ci.clipped=VK_TRUE; ci.oldSwapchain=p->sc;
    VkSwapchainKHR nsc; VKC(v->CreateSwapchainKHR(v->dev,&ci,NULL,&nsc),"vkCreateSwapchainKHR"); kh_vko_note(VKO_SWAPCHAIN,1);
    if(p->sc){ queue_idle(p); swapchain_destroy(p,p->sc,p->nimg,p->rsem); }
    p->sc=nsc; p->ext=ex; u32 n=0; VKC(v->GetSwapchainImagesKHR(v->dev,nsc,&n,NULL),"vkGetSwapchainImagesKHR");
    if(n>PMAXIMG) kh_fatal(NULL,"gfx-present-images: the swapchain has %u images (at most %d handled)",n,PMAXIMG);
    VKC(v->GetSwapchainImagesKHR(v->dev,nsc,&n,p->simg),"vkGetSwapchainImagesKHR"); p->nimg=n;
    for(u32 i=0;i<n;i++) p->rsem[i]=mk_sem(v);
    p->stale=0; p->made=1; atomic_fetch_add(&c_rebuild[reason],1);
    fprintf(stderr,"kh: gfx present: swapchain %s (%s): %ux%u, %u images (asked %u; caps min %u max %u, usage 0x%x, current extent %ux%u), FIFO, B8G8R8A8_UNORM/SRGB_NONLINEAR, check copies %s\n",
            reason==RC_FIRST?"made":"rebuilt",rc_names[reason],ex.width,ex.height,n,cnt,caps.minImageCount,caps.maxImageCount,caps.supportedUsageFlags,caps.currentExtent.width,caps.currentExtent.height,
            check?(p->sc_src_ok?"on":"SKIPPED (TRANSFER_SRC not reported)"):"off");
}
KhPresenter *kh_present_attach(ID3D12Device *dev,ID3D12CommandQueue *q,const KhPresentBuffers *bb){
    settings(); if(!pr_mode) return NULL;
    KhPresenter *p=calloc(1,sizeof *p); if(!p) kh_fatal(NULL,"gfx-present: out of memory");
    pthread_mutex_init(&p->m,NULL); p->vk=kh_vk_for(dev); p->dev=dev; p->queue=q; ID3D12Device_AddRef(dev); ID3D12CommandQueue_AddRef(q);
    p->mode=pr_mode; p->fit=pr_fit; p->check=pr_check; p->family=vkd3d_get_vk_queue_family_index(q); p->bb=*bb; p->general=bb->general; p->stale=1; p->stale_reason=RC_FIRST;
    if(p->mode==2) atomic_store(&sh_presenter_layer,1);
    p->fmt=vkd3d_get_vk_format(DXGI_FORMAT_B8G8R8A8_UNORM);
    VkFormatProperties fp; p->vk->GetPhysicalDeviceFormatProperties(p->vk->pd,p->fmt,&fp); p->linear_ok=!!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    HRESULT hr=ID3D12Device_CreateFence(dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&p->fence); if(FAILED(hr)) kh_fatal(NULL,"gfx-present: CreateFence failed hr=0x%08x",(unsigned)hr);
    const KhVk *v=p->vk;
    VkCommandPoolCreateInfo pc; memset(&pc,0,sizeof pc); pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex=p->family;
    VKC(v->CreateCommandPool(v->dev,&pc,NULL,&p->pool),"vkCreateCommandPool"); kh_vko_note(VKO_POOL,1);
    for(int i=0;i<PSLOTS;i++){ VkCommandBufferAllocateInfo ai; memset(&ai,0,sizeof ai); ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool=p->pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
        VKC(v->AllocateCommandBuffers(v->dev,&ai,&p->slot[i].cb),"vkAllocateCommandBuffers");
        VkFenceCreateInfo fi; memset(&fi,0,sizeof fi); fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; VKC(v->CreateFence(v->dev,&fi,NULL,&p->slot[i].fence),"vkCreateFence"); kh_vko_note(VKO_FENCE,1);
        p->slot[i].acq=mk_sem(v); }
    fprintf(stderr,"kh: gfx present: presenter attached: %s, back buffers %u x %ux%u (%s), queue family %u, blit filter %s, display timing %s\n",mode_names[p->mode],bb->n,bb->w,bb->h,
            bb->general?"own images, PRESENT = GENERAL":"own images, PRESENT = TRANSFER_SRC",p->family,p->linear_ok?"LINEAR":"NEAREST (no linear filtering for the format)",kh_gfxc_pres_ext.display_timing?"chained":"not enabled");
    return p;
}
/* an acquired image that was never presented (the gate was set after the acquire): its semaphore has a pending signal.  An empty batch
 * that waits on it consumes the signal (the gate is clear here), then the semaphore is destroyed and made anew, as the plan asks */
static void slot_consume(KhPresenter *p,PSlot *s){
    if(!s->acquired) return; const KhVk *v=p->vk;
    VkPipelineStageFlags ws=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; VkSubmitInfo si; memset(&si,0,sizeof si); si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.waitSemaphoreCount=1; si.pWaitSemaphores=&s->acq; si.pWaitDstStageMask=&ws;
    VKC(v->ResetFences(v->dev,1,&s->fence),"vkResetFences");
    VkQueue q=vkd3d_acquire_vk_queue(p->queue); VkResult r=v->QueueSubmit(q,1,&si,s->fence); vkd3d_release_vk_queue(p->queue);
    if(r!=VK_SUCCESS) kh_fatal(NULL,"gfx-present-vk: vkQueueSubmit (consuming a parked acquire) failed: VkResult %d",(int)r);
    s->pending=1; slot_wait(p,s,"consuming a parked acquire"); atomic_fetch_add(&c_consumed,1);
    del_sem(v,&s->acq); s->acq=mk_sem(v); atomic_fetch_add(&c_sem_remade,1); s->acquired=0;
}
/* the destination rectangle of the picture inside the swapchain image */
static void fit_rect(const KhPresenter *p,u32 bw,u32 bh,u32 sw,u32 sh,u32 *dx,u32 *dy,u32 *dw,u32 *dh){
    if(p->fit==1){ *dx=*dy=0; *dw=sw; *dh=sh; return; }
    if((u64)sw*bh>(u64)sh*bw){ *dh=sh; *dw=(u32)(((u64)sh*bw+bh/2)/bh); if(*dw>sw) *dw=sw; *dy=0; *dx=(sw-*dw)/2; }
    else { *dw=sw; *dh=(u32)(((u64)sw*bh+bw/2)/bw); if(*dh>sh) *dh=sh; *dx=0; *dy=(sh-*dh)/2; }
}
static void barrier1(const KhVk *v,VkCommandBuffer cb,VkImage img,VkImageLayout from,VkImageLayout to,VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){
    VkImageMemoryBarrier b; memset(&b,0,sizeof b); b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.srcAccessMask=sa; b.dstAccessMask=da; b.oldLayout=from; b.newLayout=to;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=img; b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount=1; b.subresourceRange.layerCount=1;
    v->CmdPipelineBarrier(cb,ss,ds,0,0,NULL,0,NULL,1,&b);
}
/* check mode: the swapchain copy (sw x sh) against a CPU bilinear reference from the picture `bb` (bw x bh, rows `bp` bytes apart), same
 * mapping as the blit.  Since review V36 n=1 `bb` is an INDEPENDENT copy: the D3D12 readback of the presented back buffer that
 * kh_vkd3d_present queues after the presenter's submission (or, in the selftest, a picture made on the CPU) */
typedef struct { u64 bars_bad, samples; double dmax, dsum; } CheckRes;
static double unorm_lin(const u8 *src,u32 bw,u32 bh,u32 bp,double u,double vv,int c){
    double x=u-0.5, y=vv-0.5; double fx=floor(x), fy=floor(y); double ax=x-fx, ay=y-fy; long x0=(long)fx, y0=(long)fy, x1=x0+1, y1=y0+1;
    if(x0<0) x0=0; if(y0<0) y0=0; if(x1<0) x1=0; if(y1<0) y1=0; if(x0>(long)bw-1) x0=bw-1; if(x1>(long)bw-1) x1=bw-1; if(y0>(long)bh-1) y0=bh-1; if(y1>(long)bh-1) y1=bh-1;
    double t00=src[(u64)y0*bp+4ull*x0+c], t10=src[(u64)y0*bp+4ull*x1+c], t01=src[(u64)y1*bp+4ull*x0+c], t11=src[(u64)y1*bp+4ull*x1+c];
    return (1-ay)*((1-ax)*t00+ax*t10)+ay*((1-ax)*t01+ax*t11); }
static CheckRes check_compare(const u8 *sc,u32 sw,u32 sh,const u8 *bb,u32 bw,u32 bh,u32 bp,u32 dx,u32 dy,u32 dw,u32 dh,int linear){
    CheckRes r; memset(&r,0,sizeof r);
    for(u32 y=0;y<sh;y++){ const u8 *q=sc+4ull*sw*y; for(u32 x=0;x<sw;x++){ if(x>=dx&&x<dx+dw&&y>=dy&&y<dy+dh) continue; if(q[4*x]|q[4*x+1]|q[4*x+2]) r.bars_bad++; } }
    /* sample points: a grid of about 64 x 36 over the picture, always including its first and last row and column */
    u32 stx=dw>64?dw/64:1, sty=dh>36?dh/36:1;
    for(u32 yy=0;;){
        for(u32 xx=0;;){
            double u=(xx+0.5)*(double)bw/dw, vv=(yy+0.5)*(double)bh/dh; const u8 *q=sc+4ull*((u64)(dy+yy)*sw+dx+xx);
            for(int c=0;c<3;c++){ double ref=linear?unorm_lin(bb,bw,bh,bp,u,vv,c):(double)bb[(u64)(u32)vv*bp+4ull*(u32)u+c]; double d=fabs(q[c]-floor(ref+0.5)); if(d>r.dmax) r.dmax=d; r.dsum+=d; }
            r.samples++;
            if(xx==dw-1) break; xx=xx+stx<dw-1?xx+stx:dw-1; }
        if(yy==dh-1) break; yy=yy+sty<dh-1?yy+sty:dh-1; }
    return r;
}
static void check_ppm(const u8 *sc,u32 sw,u32 sh,u64 n){
    const char *dir=kh_gfxc_dump_dir(); if(!dir||!*dir) return;
    char path[1200]; snprintf(path,sizeof path,"%s/present-%06llu.ppm",dir,P_(n)); FILE *f=fopen(path,"wb");
    if(!f) kh_fatal(NULL,"gfx-present-check: cannot create %s (%s)",path,strerror(errno));
    fprintf(f,"P6\n%u %u\n255\n",sw,sh); u8 *row=malloc(3ull*sw); if(!row) kh_fatal(NULL,"gfx-present-check: out of memory");
    for(u32 y=0;y<sh;y++){ const u8 *q=sc+4ull*sw*y; for(u32 x=0;x<sw;x++){ row[3*x]=q[4*x+2]; row[3*x+1]=q[4*x+1]; row[3*x+2]=q[4*x]; } if(fwrite(row,3,sw,f)!=sw) kh_fatal(NULL,"gfx-present-check: short write to %s",path); }
    free(row); if(fclose(f)) kh_fatal(NULL,"gfx-present-check: closing %s failed",path);
}
static double tol_max_now(void){ return pr_tol_max; }
static _Atomic u64 c_zero_size;
static u32 last_rect[6];                                             /* the last checked present: surface w, h and the picture's x, y, w, h */
void kh_present_test_last_rect(u32 out[6]){ pthread_mutex_lock(&pm_g); memcpy(out,last_rect,sizeof last_rect); pthread_mutex_unlock(&pm_g); }
static int rect_is(u32 sw,u32 sh,u32 dx,u32 dy,u32 dw,u32 dh){ u32 r[6]; kh_present_test_last_rect(r); return r[0]==sw&&r[1]==sh&&r[2]==dx&&r[3]==dy&&r[4]==dw&&r[5]==dh; }
/* the gate was found set after a successful acquire (caller holds p->m): the image stays "acquired, not presented"; the thread parks
 * WITHOUT the presenter's lock; on the way back the acquire is consumed, its semaphore made anew, and the swapchain marked stale.
 * The caller starts the Present again from (a). */
static void park_after_acquire(KhPresenter *p,PSlot *s,const char *where){
    atomic_fetch_add(&c_parked_acq,1); p->busy=0; pthread_mutex_unlock(&p->m);
    kh_gfx_gate_park(where);
    pthread_mutex_lock(&p->m); slot_consume(p,s); p->stale=1; p->stale_reason=RC_RESUME; pthread_mutex_unlock(&p->m);
}
/* ---- one Present ----
 * Returns 1 when this present made check copies: the caller (kh_vkd3d_present) then queues an independent D3D12 readback of the same
 * back buffer AFTER this submission and hands it to kh_present_check (review V36 n=1: the reference used to be a copy in the presenter's
 * own command buffer, from the same image it had picked, so a wrong or stale source image passed every check). */
int kh_present_frame(KCtx *g,KhPresenter *p,u32 idx,u64 n){
    (void)g; if(!p) return 0; const KhVk *v=p->vk;
    if(idx>=p->bb.n) kh_fatal(NULL,"gfx-present: back buffer %u of %u",idx,p->bb.n);
    atomic_fetch_add(&c_frames,1); present_number_last=n;
    PSlot *s; u32 ii=0; int subopt=0; u64 t_acq=0;
    for(;;){
        kh_gfx_gate_park("presenter: before acquire");                                       /* (a) */
        pthread_mutex_lock(&p->m);
        if(p->busy) kh_fatal(NULL,p->chk_pending?"gfx-present-check-orphan: kh_present_frame entered while the check of present %llu still waits for its reference (kh_present_check was not called)"
                                                :"gfx-present-reentry: kh_present_frame entered while another Present of the same swap chain is inside it (present %llu)",P_(p->chk_n));
        if(kh_gfx_gate_is_set()){ pthread_mutex_unlock(&p->m); continue; }
        if(atomic_load(&pr_gen)!=p->gen&&p->sc){ p->stale=1; p->stale_reason=RC_SIZE; }
        if(!p->sc||p->stale) swapchain_build(p);                                              /* (b) */
        if(!p->sc){ pthread_mutex_unlock(&p->m); atomic_fetch_add(&c_zero_size,1); return 0; }   /* zero-size drawable: nothing to present (counted) */
        s=&p->slot[p->frames%PSLOTS];
        slot_wait(p,s,"slot reuse");                                                          /* (c) the slot's semaphore and command buffer are free again */
        if(s->acquired) slot_consume(p,s);
        u64 t0=kh_host_ns(); VkResult r;
        if(test_inject_acquire){ r=test_inject_acquire; test_inject_acquire=0; }             /* selftest: a result the headless surface never gives */
        else r=v->AcquireNextImageKHR(v->dev,p->sc,(u64)kh_stuck_slice_ms*1000000ull,s->acq,VK_NULL_HANDLE,&ii);
        t_acq=kh_host_ns()-t0;
        if(r==VK_TIMEOUT||r==VK_NOT_READY){
            int free_=kh_gfx_gate_is_set()||kh_host_ns()-kh_gfx_gate_last_clear_ns()<(u64)kh_stuck_slice_ms*1000000ull;
            if(free_) atomic_fetch_add(&c_to_free,1); else { atomic_fetch_add(&c_to_counted,1); if(++p->counted_timeouts>=5) kh_fatal(NULL,"gfx-present-acquire: vkAcquireNextImageKHR timed out %d times in a row (slices of %u ms) with the gate clear",p->counted_timeouts,kh_stuck_slice_ms); }
            pthread_mutex_unlock(&p->m); continue; }
        if(r==VK_ERROR_OUT_OF_DATE_KHR){ atomic_fetch_add(&c_sc_outdated,1); p->stale=1; p->stale_reason=RC_OUTDATED; pthread_mutex_unlock(&p->m); continue; }
        if(r==VK_ERROR_SURFACE_LOST_KHR){ surface_drop(p); pthread_mutex_unlock(&p->m); continue; }
        if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR) kh_fatal(NULL,"gfx-present-vk: vkAcquireNextImageKHR failed: VkResult %d",(int)r);
        if(ii>=p->nimg) kh_fatal(NULL,"gfx-present-vk: vkAcquireNextImageKHR gave image %u of %u",ii,p->nimg);
        subopt=r==VK_SUBOPTIMAL_KHR; p->counted_timeouts=0; s->acquired=1; s->img=(int)ii;
        if(test_after_acquire) test_after_acquire(p);
        /* (d) everything the game queued for this frame has left vkd3d's op queue */
        u64 td=kh_host_ns(); HRESULT hr=vkd3d_queue_signal_on_cpu(p->queue,p->fence,++p->fence_val); if(FAILED(hr)) kh_fatal(NULL,"gfx-present: vkd3d_queue_signal_on_cpu failed hr=0x%08x",(unsigned)hr);
        { unsigned expired=0; u64 dl=kh_host_ns()+(u64)kh_stuck_slice_ms*1000000ull;
          while(ID3D12Fence_GetCompletedValue(p->fence)<p->fence_val){ atomic_fetch_add(&c_cpu_spins,1); struct timespec ts={0,50000}; nanosleep(&ts,0);
              u64 now=kh_host_ns(); if(now>=dl){ if(++expired>=5) kh_fatal(NULL,"gfx-present-queue-blocked: vkd3d did not hand the frame's work to Vulkan within 5 slices of %u ms (a queue waits on a fence nobody signals?)",kh_stuck_slice_ms); dl=now+(u64)kh_stuck_slice_ms*1000000ull; } } }
        /* (e) the gate again: if it is set now, this acquire stays "acquired, not presented" and the thread parks */
        if(kh_gfx_gate_is_set()){ park_after_acquire(p,s,"presenter: after acquire (e)"); continue; }
        p->busy=1;
        int check=(test_force_check>0||(p->check>0&&n%(u64)p->check==0)), do_copy=check&&p->sc_src_ok;
        if(check&&!p->sc_src_ok) atomic_fetch_add(&c_check_skipped,1);
        u32 bw=p->bb.w, bh=p->bb.h, sw=p->ext.width, sh=p->ext.height, dx,dy,dw,dh; fit_rect(p,bw,bh,sw,sh,&dx,&dy,&dw,&dh);
        if(do_copy){ hostbuf(p,0,4ull*sw*sh); hostbuf(p,1,4ull*bw*bh); }
        VkImage bbi=(VkImage)(uintptr_t)p->bb.image[idx], si=p->simg[ii]; VkImageLayout bl=p->general?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VKC(v->ResetCommandBuffer(s->cb,0),"vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi; memset(&bi,0,sizeof bi); bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VKC(v->BeginCommandBuffer(s->cb,&bi),"vkBeginCommandBuffer");
        /* the back-buffer barrier.  In `general` mode vkd3d maps the game's RENDER_TARGET -> PRESENT barrier to COLOR_ATTACHMENT_OPTIMAL ->
         * GENERAL with the HOST stage as its second scope (command.c 2287-2290): HOST in our first scope chains with it, so its layout
         * transition is ordered before the blit (review V36 n=4; a validation-layer item for step 9: lavapipe and MoltenVK ignore layouts) */
        VkPipelineStageFlags bss=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT; VkAccessFlags bsa=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        if(p->general){ bss|=VK_PIPELINE_STAGE_HOST_BIT; bsa|=VK_ACCESS_HOST_WRITE_BIT; }
        barrier1(v,s->cb,bbi,bl,bl,bsa,VK_ACCESS_TRANSFER_READ_BIT,bss,VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier1(v,s->cb,si,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black; memset(&black,0,sizeof black); black.float32[3]=1.0f; VkImageSubresourceRange rr={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        v->CmdClearColorImage(s->cb,si,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&rr);
        barrier1(v,s->cb,si,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blt; memset(&blt,0,sizeof blt); blt.srcSubresource.aspectMask=blt.dstSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; blt.srcSubresource.layerCount=blt.dstSubresource.layerCount=1;
        blt.srcOffsets[1].x=(int32_t)bw; blt.srcOffsets[1].y=(int32_t)bh; blt.srcOffsets[1].z=1;
        blt.dstOffsets[0].x=(int32_t)dx; blt.dstOffsets[0].y=(int32_t)dy; blt.dstOffsets[1].x=(int32_t)(dx+dw); blt.dstOffsets[1].y=(int32_t)(dy+dh); blt.dstOffsets[1].z=1;
        v->CmdBlitImage(s->cb,bbi,bl,si,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blt,p->linear_ok?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
        VkImageLayout last=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; VkAccessFlags lasta=VK_ACCESS_TRANSFER_WRITE_BIT;
        if(do_copy){ barrier1(v,s->cb,si,last,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c; memset(&c,0,sizeof c); c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount=1; c.imageExtent.width=sw; c.imageExtent.height=sh; c.imageExtent.depth=1;
            v->CmdCopyImageToBuffer(s->cb,si,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,p->hb[0],1,&c);
            c.imageExtent.width=bw; c.imageExtent.height=bh; v->CmdCopyImageToBuffer(s->cb,bbi,bl,p->hb[1],1,&c);
            VkMemoryBarrier hb; memset(&hb,0,sizeof hb); hb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER; hb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            v->CmdPipelineBarrier(s->cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hb,0,NULL,0,NULL);
            last=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; lasta=VK_ACCESS_TRANSFER_READ_BIT; }
        if(p->general) barrier1(v,s->cb,bbi,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_HOST_READ_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        barrier1(v,s->cb,si,last,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,lasta,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VKC(v->EndCommandBuffer(s->cb),"vkEndCommandBuffer");
        /* (f) submit and present on the borrowed queue */
        VkPipelineStageFlags ws=VK_PIPELINE_STAGE_TRANSFER_BIT; VkSubmitInfo sub; memset(&sub,0,sizeof sub); sub.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; sub.waitSemaphoreCount=1; sub.pWaitSemaphores=&s->acq; sub.pWaitDstStageMask=&ws;
        sub.commandBufferCount=1; sub.pCommandBuffers=&s->cb; sub.signalSemaphoreCount=1; sub.pSignalSemaphores=&p->rsem[ii];
        VkPresentInfoKHR pi; memset(&pi,0,sizeof pi); pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR; pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&p->rsem[ii]; pi.swapchainCount=1; pi.pSwapchains=&p->sc; pi.pImageIndices=&ii;
        VkPresentTimeGOOGLE pt={(u32)n,0}; VkPresentTimesInfoGOOGLE pti; memset(&pti,0,sizeof pti); pti.sType=VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE; pti.swapchainCount=1; pti.pTimes=&pt;
        if(kh_gfxc_pres_ext.display_timing) pi.pNext=&pti;
        /* the last look before the queue: a gate set while the command buffer was recorded parks here too (the recording is dropped) */
        if(kh_gfx_gate_is_set()){ park_after_acquire(p,s,"presenter: before submit and present"); continue; }
        VKC(v->ResetFences(v->dev,1,&s->fence),"vkResetFences");
        u64 th=kh_host_ns(); VkQueue vq=vkd3d_acquire_vk_queue(p->queue); VkResult sr=v->QueueSubmit(vq,1,&sub,s->fence); VkResult pr=sr==VK_SUCCESS?v->QueuePresentKHR(vq,&pi):VK_ERROR_UNKNOWN; vkd3d_release_vk_queue(p->queue);
        u64 te=kh_host_ns(), hold=te-th, cost=te-td;
        if(sr!=VK_SUCCESS) kh_fatal(NULL,"gfx-present-vk: vkQueueSubmit failed: VkResult %d",(int)sr);
        s->pending=1; s->acquired=0; atomic_fetch_add(&c_submits,1); atomic_fetch_add(&c_presents,1);
        if(test_inject_present&&pr==VK_SUCCESS){ pr=test_inject_present; test_inject_present=0; }   /* selftest: a result the headless surface never gives */
        /* (g) */
        if(pr==VK_SUBOPTIMAL_KHR||subopt){ atomic_fetch_add(&c_sc_suboptimal,1); p->stale=1; p->stale_reason=RC_SUBOPTIMAL; }
        else if(pr==VK_ERROR_OUT_OF_DATE_KHR){ atomic_fetch_add(&c_sc_outdated,1); p->stale=1; p->stale_reason=RC_OUTDATED; }
        else if(pr==VK_ERROR_SURFACE_LOST_KHR) surface_drop(p);              /* review V36 n=3: the surface is made anew at the next present */
        else if(pr!=VK_SUCCESS) kh_fatal(NULL,"gfx-present-vk: vkQueuePresentKHR failed: VkResult %d",(int)pr);
        pthread_mutex_lock(&pm_g); st_cost_n++; st_cost_sum+=cost; if(cost>st_cost_max) st_cost_max=cost; st_acq_sum+=t_acq; if(t_acq>st_acq_max) st_acq_max=t_acq;
        st_hold_sum+=hold; if(hold>st_hold_max) st_hold_max=hold; u64 b=cost/10000; cost_hist[b>HBUCK?HBUCK:b]++; pthread_mutex_unlock(&pm_g);
        p->frames++;                                                        /* the report line comes with the backend's every 300 presents (null_d3d12.c) */
        if(do_copy){ p->chk_pending=1; p->chk_slot=(int)(s-p->slot); p->chk_n=n;     /* stays busy until kh_present_check has its reference */
            u32 c8[8]={sw,sh,dx,dy,dw,dh,bw,bh}; memcpy(p->chk,c8,sizeof c8); }
        else p->busy=0;
        pthread_mutex_unlock(&p->m);
        return do_copy;
    }
}
/* the check of the present just made (kh_present_frame returned 1), against `ref`: the D3D12 readback of the same back buffer that
 * kh_vkd3d_present queued AFTER the presenter's submission (rows `rp` bytes apart).  Independent of the image the presenter picked and of
 * its barriers: a wrong back buffer, or one read before the game's work landed, differs from it.  Also compared, byte for byte: the
 * presenter's own copy of its source image (made in its command buffer next to the blit) against the same reference. */
void kh_present_check(KhPresenter *p,u64 n,const u8 *ref,u32 rp){
    if(!p) return; pthread_mutex_lock(&p->m);
    if(!p->chk_pending||p->chk_n!=n){ int pend=p->chk_pending; u64 pn=p->chk_n; pthread_mutex_unlock(&p->m);
        kh_fatal(NULL,"gfx-present-check-state: kh_present_check for present %llu, but %s (present %llu)",P_(n),pend?"the pending check is another present's":"no check is pending",P_(pn)); }
    slot_wait(p,&p->slot[p->chk_slot],"check copy");
    u32 sw=p->chk[0],sh=p->chk[1],dx=p->chk[2],dy=p->chk[3],dw=p->chk[4],dh=p->chk[5],bw=p->chk[6],bh=p->chk[7];
    pthread_mutex_lock(&pm_g); last_rect[0]=sw; last_rect[1]=sh; last_rect[2]=dx; last_rect[3]=dy; last_rect[4]=dw; last_rect[5]=dh; pthread_mutex_unlock(&pm_g);
    memcpy(p->lchk,p->chk,sizeof p->chk); p->lchk_valid=1;
    CheckRes cr=check_compare(p->hp[0],sw,sh,ref,bw,bh,rp,dx,dy,dw,dh,p->linear_ok); double mean=cr.samples?cr.dsum/(3.0*cr.samples):0;
    u64 src_bad=0; for(u32 y=0;y<bh;y++){ const u8 *a=p->hp[1]+4ull*bw*y, *b=ref+(u64)rp*y; if(memcmp(a,b,4ull*bw)) for(u32 x=0;x<bw;x++) if(memcmp(a+4ull*x,b+4ull*x,4)) src_bad++; }
    int ok=!cr.bars_bad&&!src_bad&&cr.dmax<=tol_max_now()+1e-9&&mean<=pr_tol_mean+1e-9;
    pthread_mutex_lock(&pm_g); if(cr.dmax>st_dmax) st_dmax=cr.dmax; if(mean>st_dmean_max) st_dmean_max=mean; pthread_mutex_unlock(&pm_g);
    if(ok) atomic_fetch_add(&c_check_ok,1); else { atomic_fetch_add(&c_check_fail,1); if(cr.bars_bad) atomic_fetch_add(&c_check_bars_bad,1); if(src_bad) atomic_fetch_add(&c_check_src_bad,1);
        fprintf(stderr,"kh: gfx present: CHECK FAILED at present %llu: %llu non-black bar pixels, picture largest difference %.2f, mean %.3f over %llu sample points against the D3D12 readback (tolerance %.1f / %.2f; %ux%u -> [%u,%u %ux%u] of %ux%u); the presenter's source copy differs from the readback in %llu pixels\n",
                P_(n),P_(cr.bars_bad),cr.dmax,mean,P_(cr.samples),tol_max_now(),pr_tol_mean,bw,bh,dx,dy,dw,dh,sw,sh,P_(src_bad)); }
    check_ppm(p->hp[0],sw,sh,n);
    p->chk_pending=0; p->busy=0; pthread_mutex_unlock(&p->m);
}
/* every submission of ours is complete (the back buffers may be destroyed afterwards) */
void kh_present_quiesce(KhPresenter *p){ if(!p) return; pthread_mutex_lock(&p->m); for(int i=0;i<PSLOTS;i++) slot_wait(p,&p->slot[i],"quiesce"); pthread_mutex_unlock(&p->m); }
void kh_present_resize(KhPresenter *p,const KhPresentBuffers *bb){ if(!p) return; pthread_mutex_lock(&p->m); p->bb=*bb; p->general=bb->general; pthread_mutex_unlock(&p->m); }
void kh_present_resume(KhPresenter *p){ if(!p) return; pthread_mutex_lock(&p->m); p->stale=1; p->stale_reason=RC_RESUME; pthread_mutex_unlock(&p->m); }
void kh_present_teardown(KhPresenter *p){
    if(!p) return; const KhVk *v=p->vk; pthread_mutex_lock(&p->m);
    for(int i=0;i<PSLOTS;i++){ slot_wait(p,&p->slot[i],"teardown"); slot_consume(p,&p->slot[i]); }
    queue_idle(p); swapchain_destroy(p,p->sc,p->nimg,p->rsem); p->sc=VK_NULL_HANDLE;
    for(int i=0;i<PSLOTS;i++){ del_sem(v,&p->slot[i].acq); v->DestroyFence(v->dev,p->slot[i].fence,NULL); kh_vko_note(VKO_FENCE,0); }
    v->DestroyCommandPool(v->dev,p->pool,NULL); kh_vko_note(VKO_POOL,0);
    for(int k=0;k<2;k++) if(p->hb[k]){ v->UnmapMemory(v->dev,p->hm[k]); v->DestroyBuffer(v->dev,p->hb[k],NULL); kh_vko_note(VKO_BUF,0); v->FreeMemory(v->dev,p->hm[k],NULL); kh_vko_note(VKO_MEM,0); }
    if(p->surface){ v->DestroySurfaceKHR(v->inst,p->surface,NULL); kh_vko_note(VKO_SURFACE,0); }
    ID3D12Fence_Release(p->fence); ID3D12CommandQueue_Release(p->queue); ID3D12Device_Release(p->dev);
    pthread_mutex_unlock(&p->m); pthread_mutex_destroy(&p->m); free(p);
}
static u64 hist_pct(double q){ u64 tot=0; for(int i=0;i<=HBUCK;i++) tot+=cost_hist[i]; if(!tot) return 0; u64 want=(u64)(q*tot+0.5), acc=0; if(!want) want=1;
    for(int i=0;i<=HBUCK;i++){ acc+=cost_hist[i]; if(acc>=want) return (u64)i*10+10; } return (u64)HBUCK*10+10; }
/* a percentile in us as text: the last bucket holds everything from 20 ms on, so a percentile there is "beyond 20 ms", not "<= 20.01"
 * (review V36 follow-up: a lavapipe route without the hash log reported p50 <=20.01 ms at a mean of 46 ms) */
static void pct_fmt(char *b,size_t n,u64 us){ if(us>(u64)HBUCK*10) snprintf(b,n,">%.0f ms (past the histogram)",HBUCK*10/1000.0); else snprintf(b,n,"<=%.2f ms",us/1000.0); }
void kh_present_report(FILE *f){
    settings(); if(!pr_mode&&!c_frames) return;
    pthread_mutex_lock(&pm_g); char p50[48], p99[48]; pct_fmt(p50,sizeof p50,hist_pct(0.5)); pct_fmt(p99,sizeof p99,hist_pct(0.99));
    fprintf(f,"kh: gfx present: mode=%s frames=%llu submits=%llu presents=%llu; swapchains made %llu, rebuilt %llu (size %llu, out-of-date %llu, suboptimal %llu, surface-lost %llu, after-park %llu); "
              "present cost (d)-(f) mean %.3f ms p50 %s p99 %s max %.3f ms; acquire mean %.3f ms max %.3f ms; queue mutex held mean %.3f ms max %.3f ms; "
              "acquire time-outs counted %llu / not counted %llu; parked after acquire %llu (semaphores made anew %llu, consumed %llu); signal_on_cpu waits that spun %llu; "
              "check (against the D3D12 readback): ok %llu failed %llu (bars %llu, source copy %llu) skipped %llu, largest difference %.2f, largest mean %.3f; surface lost %llu; zero-size drawable skips %llu; live Vulkan objects of the runtime %lld\n",
            mode_names[pr_mode<0?0:pr_mode],P_(c_frames),P_(c_submits),P_(c_presents),P_(c_rebuild[RC_FIRST]),P_(c_rebuild[RC_SIZE]+c_rebuild[RC_OUTDATED]+c_rebuild[RC_SUBOPTIMAL]+c_rebuild[RC_SURFACE]+c_rebuild[RC_RESUME]),
            P_(c_rebuild[RC_SIZE]),P_(c_rebuild[RC_OUTDATED]),P_(c_rebuild[RC_SUBOPTIMAL]),P_(c_rebuild[RC_SURFACE]),P_(c_rebuild[RC_RESUME]),
            st_cost_n?st_cost_sum/1e6/(double)st_cost_n:0.0,p50,p99,st_cost_max/1e6,st_cost_n?st_acq_sum/1e6/(double)st_cost_n:0.0,st_acq_max/1e6,
            st_cost_n?st_hold_sum/1e6/(double)st_cost_n:0.0,st_hold_max/1e6,P_(c_to_counted),P_(c_to_free),P_(c_parked_acq),P_(c_sem_remade),P_(c_consumed),P_(c_cpu_spins),
            P_(c_check_ok),P_(c_check_fail),P_(c_check_bars_bad),P_(c_check_src_bad),P_(c_check_skipped),st_dmax,st_dmean_max,P_(c_sc_surface_lost),P_(c_zero_size),kh_vko_live_total());
    pthread_mutex_unlock(&pm_g);
}
/* selftest access (gfx_present_test.c-style hooks kept here: the counters are static) */
void kh_present_test_force_check(int on){ test_force_check=on?1:-1; }
void kh_present_test_after_acquire(void (*fn)(KhPresenter *)){ test_after_acquire=fn; }
void kh_present_test_override(int mode,int fit,u32 w,u32 h){ settings(); pthread_mutex_lock(&pm_g);
    if(mode<0){ pr_mode=pr_env_mode; pr_fit=pr_env_fit; pr_w=pr_env_w; pr_h=pr_env_h; } else { pr_mode=mode; pr_fit=fit; if(w&&h){ pr_w=w; pr_h=h; } }
    atomic_fetch_add(&pr_gen,1); pthread_mutex_unlock(&pm_g); }
void kh_present_test_counters(u64 *frames,u64 *submits,u64 *presents,u64 *rebuilds,u64 *check_ok,u64 *check_fail,u64 *parked,u64 *remade){
    *frames=c_frames; *submits=c_submits; *presents=c_presents; u64 r=0; for(int i=0;i<RC_N;i++) r+=c_rebuild[i]; *rebuilds=r; *check_ok=c_check_ok; *check_fail=c_check_fail; *parked=c_parked_acq; *remade=c_sem_remade; }
void kh_present_test_extent(KhPresenter *p,u32 *w,u32 *h,u32 *nimg){ pthread_mutex_lock(&p->m); *w=p->ext.width; *h=p->ext.height; *nimg=p->nimg; pthread_mutex_unlock(&p->m); }
void kh_present_test_cost(u64 *n,u64 *sum,u64 *max){ pthread_mutex_lock(&pm_g); *n=st_cost_n; *sum=st_cost_sum; *max=st_cost_max; pthread_mutex_unlock(&pm_g); }
double kh_present_test_dmax(void){ return st_dmax; }
void kh_present_test_tol(double mx,double mean){ pr_tol_max=mx; pr_tol_mean=mean; }

/* ---- a Vulkan read-back of one of OUR images (selftest of plan step 6: the D3D12 back buffer IS our VkImage) ---------------------- */
int kh_vk_test_read_image(ID3D12Device *dev,ID3D12CommandQueue *q,u64 image,u32 w,u32 h,int general,u8 *out){
    const KhVk *v=kh_vk_for(dev); KhPresenter tmp; memset(&tmp,0,sizeof tmp); tmp.vk=v; tmp.queue=q; u32 fam=vkd3d_get_vk_queue_family_index(q);
    ID3D12Fence *fe=NULL; if(FAILED(ID3D12Device_CreateFence(dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fe))) return 1;
    if(FAILED(vkd3d_queue_signal_on_cpu(q,fe,1))){ ID3D12Fence_Release(fe); return 2; }
    for(int i=0;i<200000&&ID3D12Fence_GetCompletedValue(fe)<1;i++){ struct timespec ts={0,50000}; nanosleep(&ts,0); }
    int bad=ID3D12Fence_GetCompletedValue(fe)<1; ID3D12Fence_Release(fe); if(bad) return 3;
    VkCommandPool pool; VkCommandPoolCreateInfo pc; memset(&pc,0,sizeof pc); pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pc.queueFamilyIndex=fam;
    VKC(v->CreateCommandPool(v->dev,&pc,NULL,&pool),"vkCreateCommandPool(test)"); kh_vko_note(VKO_POOL,1);
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai; memset(&ai,0,sizeof ai); ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
    VKC(v->AllocateCommandBuffers(v->dev,&ai,&cb),"vkAllocateCommandBuffers(test)");
    hostbuf(&tmp,0,4ull*w*h);
    VkCommandBufferBeginInfo bi; memset(&bi,0,sizeof bi); bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; VKC(v->BeginCommandBuffer(cb,&bi),"vkBeginCommandBuffer(test)");
    VkImageLayout l=general?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; VkImage im=(VkImage)(uintptr_t)image;
    barrier1(v,cb,im,l,l,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|(general?VK_ACCESS_HOST_WRITE_BIT:0),VK_ACCESS_TRANSFER_READ_BIT,
             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|(general?VK_PIPELINE_STAGE_HOST_BIT:0),VK_PIPELINE_STAGE_TRANSFER_BIT);   /* general: chains with PRESENT's HOST stage (review V36 n=4) */
    VkBufferImageCopy c; memset(&c,0,sizeof c); c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount=1; c.imageExtent.width=w; c.imageExtent.height=h; c.imageExtent.depth=1;
    v->CmdCopyImageToBuffer(cb,im,l,tmp.hb[0],1,&c);
    VkMemoryBarrier hb; memset(&hb,0,sizeof hb); hb.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER; hb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    v->CmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hb,0,NULL,0,NULL);
    VKC(v->EndCommandBuffer(cb),"vkEndCommandBuffer(test)");
    VkFence vf; VkFenceCreateInfo fi; memset(&fi,0,sizeof fi); fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; VKC(v->CreateFence(v->dev,&fi,NULL,&vf),"vkCreateFence(test)"); kh_vko_note(VKO_FENCE,1);
    VkSubmitInfo si; memset(&si,0,sizeof si); si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    VkQueue vq=vkd3d_acquire_vk_queue(q); VkResult r=v->QueueSubmit(vq,1,&si,vf); vkd3d_release_vk_queue(q);
    if(r==VK_SUCCESS) r=v->WaitForFences(v->dev,1,&vf,VK_TRUE,30000000000ull);
    if(r==VK_SUCCESS) memcpy(out,tmp.hp[0],4ull*w*h);
    v->DestroyFence(v->dev,vf,NULL); kh_vko_note(VKO_FENCE,0); v->DestroyCommandPool(v->dev,pool,NULL); kh_vko_note(VKO_POOL,0);
    v->UnmapMemory(v->dev,tmp.hm[0]); v->DestroyBuffer(v->dev,tmp.hb[0],NULL); kh_vko_note(VKO_BUF,0); v->FreeMemory(v->dev,tmp.hm[0],NULL); kh_vko_note(VKO_MEM,0);
    return r==VK_SUCCESS?0:4;
}

/* ---- the presenter selftest (plan step 7), part of the vkd3d device selftest when the headless surface exists ----------------------
 * Pictures are drawn on the HOST side into the swap chain's real back buffers (si.buf) on the swap chain's real queue, then presented
 * through the guest ABI (IDXGISwapChain::Present: null_d3d12.c -> kh_vkd3d_present -> the presenter), with check mode on for every
 * present, a 200x100 headless surface and aspect fit. */
typedef struct { KhSwapInfo si; ID3D12DescriptorHeap *rtv; ID3D12CommandAllocator *al; ID3D12GraphicsCommandList *cl; ID3D12Fence *fe; u64 fv;
    ID3D12RootSignature *rs; ID3D12PipelineState *pso; } PT;
static int pt_init(PT *t,KCtx *g,u64 sc){
    memset(t,0,sizeof *t); if(!kh_vkd3d_swap_test_info(g,sc,&t->si)) return 0; ID3D12Device *d=t->si.dev;
    D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd,0,sizeof hd); hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    if(FAILED(ID3D12Device_CreateDescriptorHeap(d,&hd,&IID_ID3D12DescriptorHeap,(void**)&t->rtv))) return 0;
    if(FAILED(ID3D12Device_CreateCommandAllocator(d,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&t->al))) return 0;
    if(FAILED(ID3D12Device_CreateCommandList(d,0,D3D12_COMMAND_LIST_TYPE_DIRECT,t->al,NULL,&IID_ID3D12GraphicsCommandList,(void**)&t->cl))) return 0;
    ID3D12GraphicsCommandList_Close(t->cl);
    if(FAILED(ID3D12Device_CreateFence(d,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&t->fe))) return 0;
    /* a patterned picture: a full-screen triangle from SV_VertexID, colours that change every few pixels (non-flat for the bilinear check) */
    static const char vs[]="float4 main(uint id : SV_VertexID) : SV_Position { float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }";
    static const char ps[]="float4 main(float4 p : SV_Position) : SV_Target { return float4(frac(p.x / 7.0), frac(p.y / 5.0), frac((p.x + p.y) / 23.0), 1.0); }";
    ID3DBlob *vb=NULL,*pb=NULL,*eb=NULL,*sb=NULL;
    if(D3DCompile(vs,sizeof vs-1,"present_vs",NULL,NULL,"main","vs_4_0",0,0,&vb,&eb)!=S_OK||D3DCompile(ps,sizeof ps-1,"present_ps",NULL,NULL,"main","ps_4_0",0,0,&pb,&eb)!=S_OK){
        if(eb) fprintf(stderr,"gfx present selftest: HLSL: %s\n",(const char*)ID3D10Blob_GetBufferPointer(eb)); return 0; }
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd,0,sizeof rd);
    if(FAILED(vkd3d_serialize_root_signature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sb,NULL))) return 0;
    if(FAILED(ID3D12Device_CreateRootSignature(d,0,ID3D10Blob_GetBufferPointer(sb),ID3D10Blob_GetBufferSize(sb),&IID_ID3D12RootSignature,(void**)&t->rs))) return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd,0,sizeof pd); pd.pRootSignature=t->rs; pd.VS.pShaderBytecode=ID3D10Blob_GetBufferPointer(vb); pd.VS.BytecodeLength=ID3D10Blob_GetBufferSize(vb);
    pd.PS.pShaderBytecode=ID3D10Blob_GetBufferPointer(pb); pd.PS.BytecodeLength=ID3D10Blob_GetBufferSize(pb); pd.BlendState.RenderTarget[0].RenderTargetWriteMask=0xf; pd.SampleMask=0xffffffffu;
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable=TRUE; pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count=1;
    HRESULT hr=ID3D12Device_CreateGraphicsPipelineState(d,&pd,&IID_ID3D12PipelineState,(void**)&t->pso);
    ID3D10Blob_Release(vb); ID3D10Blob_Release(pb); ID3D10Blob_Release(sb); if(eb) ID3D10Blob_Release(eb);
    return SUCCEEDED(hr);
}
static void pt_free(PT *t){ if(t->pso) ID3D12PipelineState_Release(t->pso); if(t->rs) ID3D12RootSignature_Release(t->rs); if(t->fe) ID3D12Fence_Release(t->fe);
    if(t->cl) ID3D12GraphicsCommandList_Release(t->cl); if(t->al) ID3D12CommandAllocator_Release(t->al); if(t->rtv) ID3D12DescriptorHeap_Release(t->rtv); memset(t,0,sizeof *t); }
static void pt_bar(ID3D12GraphicsCommandList *l,ID3D12Resource *r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER x; memset(&x,0,sizeof x); x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; ID3D12GraphicsCommandList_ResourceBarrier(l,1,&x); }
/* one frame into the current back buffer: `draws` patterned full-screen draws, then (clear>=0) a clear to colour c; executed on the queue */
static int pt_frame(PT *t,KCtx *g,u64 sc,int draws,int clear,const float c[4]){
    u32 i=(u32)GC(VT(sc,36),sc); if(i>=3) return 0; ID3D12Resource *rt=t->si.buf[i];
    if(t->fv&&ID3D12Fence_GetCompletedValue(t->fe)<t->fv) for(int k=0;k<600000&&ID3D12Fence_GetCompletedValue(t->fe)<t->fv;k++){ struct timespec ts={0,100000}; nanosleep(&ts,0); }
    if(FAILED(ID3D12CommandAllocator_Reset(t->al))||FAILED(ID3D12GraphicsCommandList_Reset(t->cl,t->al,NULL))) return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h=ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->rtv); ID3D12Device_CreateRenderTargetView(t->si.dev,rt,NULL,h);
    pt_bar(t->cl,rt,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_OMSetRenderTargets(t->cl,1,&h,FALSE,NULL);
    D3D12_VIEWPORT vp={0,0,(float)t->si.w,(float)t->si.h,0,1}; D3D12_RECT rc={0,0,(LONG)t->si.w,(LONG)t->si.h};
    ID3D12GraphicsCommandList_RSSetViewports(t->cl,1,&vp); ID3D12GraphicsCommandList_RSSetScissorRects(t->cl,1,&rc);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(t->cl,t->rs); ID3D12GraphicsCommandList_SetPipelineState(t->cl,t->pso); ID3D12GraphicsCommandList_IASetPrimitiveTopology(t->cl,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(int k=0;k<draws;k++) ID3D12GraphicsCommandList_DrawInstanced(t->cl,3,1,0,0);
    if(clear>=0) ID3D12GraphicsCommandList_ClearRenderTargetView(t->cl,h,c,0,NULL);
    pt_bar(t->cl,rt,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(FAILED(ID3D12GraphicsCommandList_Close(t->cl))) return 0;
    ID3D12CommandList *ls[1]={(ID3D12CommandList*)t->cl}; ID3D12CommandQueue_ExecuteCommandLists(t->si.queue,1,ls);
    if(FAILED(ID3D12CommandQueue_Signal(t->si.queue,t->fe,++t->fv))) return 0;
    return (u32)GC(VT(sc,8),sc,1,0)==0;                                         /* IDXGISwapChain::Present through the guest ABI */
}
static u64 mk_swap(KCtx *g,u64 gm,u64 fct,u64 q,u32 w,u32 h){
    u64 d=gm; memset(hp(g,d),0,48); wr32(g,d,w); wr32(g,d+4,h); wr32(g,d+8,0x57); wr32(g,d+16,1); wr32(g,d+24,0x30); wr32(g,d+28,3); wr32(g,d+36,3); wr32(g,d+44,0x42);
    wr64(g,gm+64,0); if((u32)GC(VT(fct,15),fct,q,0xAB0001,d,0,0,gm+64)) return 0; return rd64(g,gm+64); }
typedef struct { unsigned hold_ms; } GateHold;
static void *gate_clearer(void *a){ GateHold *h=(GateHold*)a; struct timespec ts={(time_t)(h->hold_ms/1000),(long)(h->hold_ms%1000)*1000000L}; nanosleep(&ts,0); kh_gfx_gate_clear(); return NULL; }
static pthread_t gate_thread; static GateHold gate_hold;
static void hook_set_gate(KhPresenter *p){ (void)p; kh_present_test_after_acquire(NULL); kh_gfx_gate_set(); gate_hold.hold_ms=60; pthread_create(&gate_thread,NULL,gate_clearer,&gate_hold); }
/* review V36 n=1: pictures made on the CPU, independent of every GPU copy.  A flat colour (float -> UNORM, round to nearest) and the
 * selftest's pattern evaluated as the pixel shader does in float: R = frac(px/7), G = frac(py/5), B = frac((px+py)/23) at the pixel
 * centre (px = x + 0.5).  BGRA rows, 4*w bytes. */
static u8 un8(float v){ if(!(v>0.0f)) return 0; if(v>=1.0f) return 255; return (u8)floorf(v*255.0f+0.5f); }
static u8 *cpu_flat(u32 w,u32 h,const float c[4]){ u8 *b=malloc(4ull*w*h); if(!b) kh_fatal(NULL,"gfx present selftest: out of memory");
    for(u64 i=0;i<(u64)w*h;i++){ b[4*i]=un8(c[2]); b[4*i+1]=un8(c[1]); b[4*i+2]=un8(c[0]); b[4*i+3]=un8(c[3]); } return b; }
static u8 *cpu_pattern(u32 w,u32 h){ u8 *b=malloc(4ull*w*h); if(!b) kh_fatal(NULL,"gfx present selftest: out of memory");
    for(u32 y=0;y<h;y++) for(u32 x=0;x<w;x++){ float px=(float)x+0.5f, py=(float)y+0.5f, r=px/7.0f, gg=py/5.0f, bl=(px+py)/23.0f; u8 *q=b+4ull*((u64)y*w+x);
        q[2]=un8(r-floorf(r)); q[1]=un8(gg-floorf(gg)); q[0]=un8(bl-floorf(bl)); q[3]=255; } return b; }
static double cpu_dmax;                                              /* largest difference against a CPU-made picture (the suite line) */
/* the last checked present's swapchain copy against a CPU-made picture of the back buffer (bw x bh).  Each call consumes that present: a
 * present that made no check copy cannot pass on an older one. */
static int pt_cpu_check(KhPresenter *p,const u8 *ref,u32 bw,u32 bh,double tmax,double tmean,const char *what,int k){
    pthread_mutex_lock(&p->m); int ok=0;
    if(!p->lchk_valid||p->lchk[6]!=bw||p->lchk[7]!=bh) fprintf(stderr,"gfx present selftest: %s %d: no checked present of a %ux%u back buffer to compare\n",what,k,bw,bh);
    else { CheckRes cr=check_compare(p->hp[0],p->lchk[0],p->lchk[1],ref,bw,bh,4*bw,p->lchk[2],p->lchk[3],p->lchk[4],p->lchk[5],p->linear_ok); double mean=cr.samples?cr.dsum/(3.0*cr.samples):0;
        ok=!cr.bars_bad&&cr.dmax<=tmax+1e-9&&mean<=tmean+1e-9; if(cr.dmax>cpu_dmax) cpu_dmax=cr.dmax;
        if(!ok) fprintf(stderr,"gfx present selftest: %s %d: the presented picture against the CPU-made one: largest difference %.2f, mean %.3f (tolerance %.1f / %.2f), %llu bar pixels not black\n",what,k,cr.dmax,mean,tmax,tmean,P_(cr.bars_bad)); }
    p->lchk_valid=0; pthread_mutex_unlock(&p->m); return ok; }
int kh_present_selftest(KCtx *g,u64 m,u64 dev,u64 q,u64 fct){
    (void)m;(void)dev;
    if(!kh_gfxc_pres_ext.headless||!kh_gfxc_pres_ext.swapchain||!kh_gfxc_pres_ext.surface){
        fprintf(stderr,"kh: gfx present selftest: SKIPPED (VK_KHR_surface / VK_EXT_headless_surface / VK_KHR_swapchain not all enabled: surface=%d headless=%d swapchain=%d)\n",
                kh_gfxc_pres_ext.surface,kh_gfxc_pres_ext.headless,kh_gfxc_pres_ext.swapchain); return 0; }
    int n=0,f=0;
#define TS(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"gfx present selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    long long live0[VKO_N]; for(int k=0;k<VKO_N;k++) live0[k]=atomic_load(&kh_vko_live[k]);
    u64 F0,S0,P0,R0,OK0,BAD0,PK0,RM0,Z0=atomic_load(&c_zero_size); kh_present_test_counters(&F0,&S0,&P0,&R0,&OK0,&BAD0,&PK0,&RM0);
    u64 shs0=atomic_load(&sh_size_calls);                                  /* the size calls below are the test's, not the shell's */
    double tm=pr_tol_max, tn=pr_tol_mean; u64 gm=ga_calloc(1,256); PT t; memset(&t,0,sizeof t);
    kh_present_test_override(1,0,200,100); kh_present_test_force_check(1); kh_vkd3d_test_bbmode(0);
    /* A: 96x64 own images (3:2) into 200x100 (2:1): pillarbox 150x100 at x = 25 */
    u64 sc=mk_swap(g,gm,fct,q,96,64); TS(sc!=0); if(!sc) goto out;
    TS(pt_init(&t,g,sc)); if(!t.pso) goto out;
    TS(t.si.pres!=NULL && t.si.bbmode==0);
    /* Every present below is checked twice: by check mode itself, against the D3D12 readback of the same back buffer queued after the
     * presenter's submission, and here against a picture made on the CPU (the clear colour, or the pattern evaluated as the shader does) */
    u8 *pat96=cpu_pattern(96,64), *pat128=cpu_pattern(128,72);
    /* 1. three flat clear colours, STRICT (every sample within 1 level, bars exactly black), each one also against its CPU colour */
    kh_present_test_tol(1.0,0.34);
    { static const float cols[3][4]={{0.25f,0.5f,0.75f,1},{1,0,0.5f,1},{0,1,0.25f,1}}; u64 ok0,bad0,x;
      kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
      for(int k=0;k<3;k++){ TS(pt_frame(&t,g,sc,0,0,cols[k])); u8 *fl=cpu_flat(96,64,cols[k]); TS(pt_cpu_check(t.si.pres,fl,96,64,1.0,0.34,"clear colour",k)); free(fl); }
      u64 ok1,bad1; kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x); TS(ok1==ok0+3 && bad1==bad0);
      u32 w,h,ni; kh_present_test_extent(t.si.pres,&w,&h,&ni); TS(w==200&&h==100&&ni>=3);
      TS(rect_is(200,100,25,0,150,100)); }                                  /* expected independently: 96x64 at height 100 is 150 wide, centred */
    /* 2. a real picture at a non-integer scale (96x64 -> 150x100, x1.5625) against the bilinear reference, default tolerance; and
     *    against the bilinear scaling of the CPU-evaluated pattern */
    kh_present_test_tol(tm,tn);
    { u64 ok0,bad0,x; kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
      for(int k=0;k<3;k++){ TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(pt_cpu_check(t.si.pres,pat96,96,64,tm,tn,"patterned picture",k)); }
      u64 ok1,bad1; kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x); TS(ok1==ok0+3 && bad1==bad0); TS(rect_is(200,100,25,0,150,100));
      fprintf(stderr,"kh: gfx present selftest: patterned picture 96x64 -> 150x100: largest difference so far %.2f levels against the D3D12 readback, %.2f against the CPU-evaluated pattern\n",kh_present_test_dmax(),cpu_dmax); }
    /* 3. ResizeBuffers in the middle of the run: 128x72 (16:9) -> letterbox 178x100 at x = 11 */
    { u64 im0=n_vko_made_images(); TS((u32)GC(VT(sc,13),sc,3,128,72,0x57,0x42)==0); TS(n_vko_made_images()==im0+3);
      pt_free(&t); TS(pt_init(&t,g,sc)); TS(t.si.w==128&&t.si.h==72);
      u64 ok0,bad0,x; kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
      for(int k=0;k<3;k++){ TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"after ResizeBuffers",k)); }
      u64 ok1,bad1; kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x); TS(ok1==ok0+3 && bad1==bad0);
      TS(rect_is(200,100,11,0,178,100)); }                                  /* 128x72 at height 100: 177.8 -> 178 wide, x = 11 */
    /* 4. a forced rebuild by a size change (the headless surface never reports SUBOPTIMAL): 160x120 */
    { u64 x,r0,r1,ok0,ok1,bad0,bad1; kh_present_test_counters(&x,&x,&x,&r0,&ok0,&bad0,&x,&x);
      kh_present_set_size(160,120); for(int k=0;k<2;k++){ TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"after a size change",k)); }
      kh_present_test_counters(&x,&x,&x,&r1,&ok1,&bad1,&x,&x); u32 w,h,ni; kh_present_test_extent(t.si.pres,&w,&h,&ni);
      TS(r1==r0+1 && w==160 && h==120 && ok1==ok0+2 && bad1==bad0); TS(rect_is(160,120,0,15,160,90)); }   /* 16:9 into 4:3: letterbox 160x90 at y = 15 */
    /* 5. the gate set right after a successful acquire, held 60 ms, cleared: the acquire is consumed, its semaphore made anew, the
     *    swapchain rebuilt, and the present goes on (and passes its check) */
    { u64 x,r0,r1,pk0,pk1,rm0,rm1,ok0,ok1,bad0,bad1,gp0=kh_gfx_gate_parks(); kh_present_test_counters(&x,&x,&x,&r0,&ok0,&bad0,&pk0,&rm0);
      kh_present_test_after_acquire(hook_set_gate); u64 t0=kh_host_ns(); TS(pt_frame(&t,g,sc,1,-1,NULL)); u64 dt=kh_host_ns()-t0; pthread_join(gate_thread,NULL);
      TS(pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"after a park",0));
      kh_present_test_counters(&x,&x,&x,&r1,&ok1,&bad1,&pk1,&rm1);
      TS(pk1==pk0+1 && rm1==rm0+1 && r1==r0+1 && ok1==ok0+1 && bad1==bad0 && dt>=50000000ull && kh_gfx_gate_parks()==gp0+1 && !kh_gfx_gate_is_set()); }
    /* 5b. (review V36 n=3) SURFACE_LOST from vkQueuePresentKHR (injected once after a real present: the headless surface never loses
     *     itself): the swapchain and the surface are dropped at once and made anew at the next present, counted as a surface-lost rebuild
     *     (before, the lost surface was kept and the next build asked it for its capabilities: the named fatal gfx-present-vk) */
    { u64 sm0=atomic_load(&vko_made[VKO_SURFACE]), rs0=atomic_load(&c_rebuild[RC_SURFACE]), x,ok0,ok1,bad0,bad1; long long sl0=atomic_load(&kh_vko_live[VKO_SURFACE]);
      kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
      test_inject_present=VK_ERROR_SURFACE_LOST_KHR; TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(test_inject_present==0 && pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"present before the lost surface",0));
      TS(atomic_load(&kh_vko_live[VKO_SURFACE])==sl0-1);                    /* dropped at once */
      TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"present on the new surface",0));
      kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x);
      TS(atomic_load(&vko_made[VKO_SURFACE])==sm0+1 && atomic_load(&c_rebuild[RC_SURFACE])==rs0+1 && atomic_load(&kh_vko_live[VKO_SURFACE])==sl0 && ok1==ok0+2 && bad1==bad0); }
    /* 5c. SURFACE_LOST from vkAcquireNextImageKHR (injected instead of the call): the surface is made anew in the same Present, and the
     *     rebuild is counted as surface-lost (it used to be counted as a first swapchain: the swapchain pointer was gone) */
    { u64 sm0=atomic_load(&vko_made[VKO_SURFACE]), rs0=atomic_load(&c_rebuild[RC_SURFACE]), x,ok0,ok1,bad0,bad1; long long sl0=atomic_load(&kh_vko_live[VKO_SURFACE]);
      kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
      test_inject_acquire=VK_ERROR_SURFACE_LOST_KHR; TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(test_inject_acquire==0 && pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"acquire after the lost surface",0));
      kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x);
      TS(atomic_load(&vko_made[VKO_SURFACE])==sm0+1 && atomic_load(&c_rebuild[RC_SURFACE])==rs0+1 && atomic_load(&kh_vko_live[VKO_SURFACE])==sl0 && ok1==ok0+1 && bad1==bad0); }
    /* 5d. a zero-size drawable (injected into the extent of a rebuild that a size change asks for): the old swapchain is destroyed, the
     *     Presents are counted zero-size skips and present nothing (before, the old swapchain was kept and presented into); with a size
     *     again the swapchain is made anew, counted as a size rebuild */
    { u64 z0=atomic_load(&c_zero_size), pr0=atomic_load(&c_presents), rz0=atomic_load(&c_rebuild[RC_SIZE]); long long sw0=atomic_load(&kh_vko_live[VKO_SWAPCHAIN]);
      test_inject_zero=1; kh_present_set_size(160,120);                     /* the same size: a new generation, so the next Present rebuilds */
      for(int k=0;k<2;k++) TS(pt_frame(&t,g,sc,1,-1,NULL));
      TS(atomic_load(&c_zero_size)==z0+2 && atomic_load(&c_presents)==pr0 && atomic_load(&kh_vko_live[VKO_SWAPCHAIN])==sw0-1);
      TS(!pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"(expected: nothing presented)",0));
      test_inject_zero=0; TS(pt_frame(&t,g,sc,1,-1,NULL)); TS(pt_cpu_check(t.si.pres,pat128,128,72,tm,tn,"with a size again",0));
      TS(atomic_load(&c_presents)==pr0+1 && atomic_load(&kh_vko_live[VKO_SWAPCHAIN])==sw0 && atomic_load(&c_rebuild[RC_SIZE])==rz0+1 && rect_is(160,120,0,15,160,90)); }
    /* 6. B's sync stress, 100 times: 200 full-screen draws, a clear to a final colour, a present: check mode sees only the final
     *    colour (strict), and so does the comparison with the CPU colour.  Own images (PRESENT = TRANSFER_SRC), then
     *    KHNMC_GFX_BACKBUFFERS=general (PRESENT = GENERAL, trailing barrier) */
    for(int mode=0;mode<2;mode++){
        kh_vkd3d_test_bbmode(mode); u64 s2=mk_swap(g,gm,fct,q,320,180); TS(s2!=0); if(!s2) break; PT u; TS(pt_init(&u,g,s2)); TS(u.si.bbmode==mode);
        kh_present_test_tol(1.0,0.34); u64 x,ok0,ok1,bad0,bad1; kh_present_test_counters(&x,&x,&x,&x,&ok0,&bad0,&x,&x);
        static const float fin[2][4]={{0.0f,0.5f,1.0f,1},{1.0f,0.25f,0.0f,1}}; u8 *ff[2]={cpu_flat(320,180,fin[0]),cpu_flat(320,180,fin[1])}; int cpu_bad=0;
        for(int k=0;k<100;k++){ TS(pt_frame(&u,g,s2,200,0,fin[k&1])); if(!pt_cpu_check(u.si.pres,ff[k&1],320,180,1.0,0.34,mode?"sync stress (general)":"sync stress (own)",k)) cpu_bad++; }
        free(ff[0]); free(ff[1]); TS(cpu_bad==0);
        kh_present_test_counters(&x,&x,&x,&x,&ok1,&bad1,&x,&x); TS(ok1==ok0+100 && bad1==bad0); TS(rect_is(160,120,0,15,160,90));
        fprintf(stderr,"kh: gfx present selftest: sync stress (%s back buffers): 100 frames of 200 draws + clear, %llu checks passed, %llu failed; against the CPU colour %d failed\n",mode?"general":"own",P_(ok1-ok0),P_(bad1-bad0),cpu_bad);
        kh_present_test_tol(tm,tn); pt_free(&u); kh_vkd3d_swap_test_release(g,s2); }
    kh_vkd3d_test_bbmode(0); free(pat96); free(pat128);
    { char b1[48], b2[48]; pct_fmt(b1,sizeof b1,5000); pct_fmt(b2,sizeof b2,(u64)HBUCK*10+10); TS(!strcmp(b1,"<=5.00 ms") && b2[0]=='>'); }   /* the cost line's percentiles */
    /* 7. presents equal submissions; Present calls that reached the presenter equal presents plus zero-size skips (5d) */
    { u64 F,S,P,R,OK,BAD,PK,RM; kh_present_test_counters(&F,&S,&P,&R,&OK,&BAD,&PK,&RM); u64 Z=atomic_load(&c_zero_size)-Z0;
      TS(S-S0==P-P0 && P-P0+Z==F-F0 && Z==2 && OK-OK0==P-P0 && BAD==BAD0);
      fprintf(stderr,"kh: gfx present selftest: %llu Present calls, %llu submissions, %llu presents, %llu zero-size skips, %llu swapchain (re)builds, %llu checks ok; largest difference against a CPU-made picture %.2f levels\n",P_(F-F0),P_(S-S0),P_(P-P0),P_(Z),P_(R-R0),P_(OK-OK0),cpu_dmax); }
out:
    pt_free(&t); if(sc) kh_vkd3d_swap_test_release(g,sc);
    /* 8. nothing is left: every Vulkan object the presenter and the own back buffers made for these swap chains is destroyed */
    for(int k=0;k<VKO_N;k++){ long long l=atomic_load(&kh_vko_live[k]); if(l!=live0[k]) fprintf(stderr,"gfx present selftest: %s: %lld live, %lld before\n",vko_names[k],l,live0[k]); TS(l==live0[k]); }
    /* optional measurement (KHNMC_GFX_PRESENT_BENCH=n): n presents of a 1920x1080 own back buffer into the KHNMC_GFX_PRESENT_SIZE
     * surface (default 2556x1179), check off: host cost (d)-(f) and the full latency (Present call until the blit's fence) */
    { const char *e=getenv("KHNMC_GFX_PRESENT_BENCH"); int bn=e&&*e?atoi(e):0;
      if(bn>1){ const char *sz=getenv("KHNMC_GFX_PRESENT_SIZE"); u32 bw=2556,bh=1179; if(sz&&*sz) sscanf(sz,"%ux%u",&bw,&bh);
          kh_present_test_force_check(0); double with_mean=0,with_max=0,without_mean=0,host_mean=0,host_max=0; int ok=1;
          for(int pass=0;pass<2&&ok;pass++){                                    /* pass 0: with the presenter, pass 1: the same frames without it */
              kh_present_test_override(pass?0:1,0,bw,bh); u64 s3=mk_swap(g,gm,fct,q,1920,1080); PT b; memset(&b,0,sizeof b);
              ok=s3&&pt_init(&b,g,s3)&&(pass?b.si.pres==NULL:b.si.pres!=NULL); TS(ok);
              u64 cn0,cs0,cm0,cn1,cs1,cm1; kh_present_test_cost(&cn0,&cs0,&cm0); double sum=0,mx=0; static const float c0[4]={0.2f,0.4f,0.6f,1};
              for(int k=0;ok&&k<bn;k++){ u64 a=kh_host_ns(); ok=pt_frame(&b,g,s3,1,-1,c0);
                  while(ID3D12Fence_GetCompletedValue(b.fe)<b.fv){ struct timespec ts={0,50000}; nanosleep(&ts,0); }      /* the frame's own work */
                  if(b.si.pres) kh_present_quiesce(b.si.pres);                                                             /* and the blit */
                  double d=(kh_host_ns()-a)/1e6; if(k){ sum+=d; if(d>mx) mx=d; } }
              kh_present_test_cost(&cn1,&cs1,&cm1);
              if(!pass){ with_mean=sum/(bn-1); with_max=mx; host_mean=cn1>cn0?(cs1-cs0)/1e6/(double)(cn1-cn0):0; host_max=cm1/1e6; } else without_mean=sum/(bn-1);
              pt_free(&b); if(s3) kh_vkd3d_swap_test_release(g,s3); }
          fprintf(stderr,"kh: gfx present bench: %d frames of a 1920x1080 back buffer, presented into %ux%u (headless, aspect, check off; LP_NUM_THREADS=%s): "
                         "host cost (d)-(f) mean %.3f ms max %.3f ms; one frame (1 draw + Present, waited for) with the presenter mean %.2f ms max %.2f ms, without %.2f ms: the present adds %.2f ms\n",
                  bn,bw,bh,getenv("LP_NUM_THREADS")?getenv("LP_NUM_THREADS"):"(runtime default 4)",host_mean,host_max,with_mean,with_max,without_mean,with_mean-without_mean); }
      else fprintf(stderr,"kh: gfx present bench: not run (KHNMC_GFX_PRESENT_BENCH=n with n >= 2 runs it)\n"); }
    kh_present_test_override(-1,0,0,0); kh_present_test_force_check(0); kh_present_test_after_acquire(NULL); kh_vkd3d_test_bbmode(-1); kh_present_test_tol(tm,tn); ga_free(gm);
    test_inject_present=test_inject_acquire=0; test_inject_zero=0; atomic_store(&sh_size_calls,shs0);
    fprintf(stderr,"kh: gfx present selftest: %d checks, %d failures (headless surface: every present against the D3D12 readback AND a CPU-made picture: clear colours, patterned picture, ResizeBuffers, size change, gate after acquire, surface lost at present and at acquire, zero-size drawable, sync stress own + general, presents = submissions, nothing left)\n",n,f);
#undef TS
    return f;
}
#else
/* no graphics backend in this build (review V36 n=5): the shell's calls are counted, the first one says so, the exit line reports them */
void kh_present_set_layer(void *l,uint32_t w,uint32_t h){
    if(!atomic_fetch_add(&sh_layer_calls,1)) fprintf(stderr,"kh: gfx present: kh_present_set_layer(%p, %ux%u) DROPPED: this build has no graphics backend (GFX_VKD3D=0): no picture (counted, reported at exit)\n",l,w,h); }
void kh_present_set_size(uint32_t w,uint32_t h){
    if(!w||!h) atomic_fetch_add(&sh_size_zero,1);
    if(!atomic_fetch_add(&sh_size_calls,1)) fprintf(stderr,"kh: gfx present: kh_present_set_size(%ux%u) DROPPED: this build has no graphics backend (GFX_VKD3D=0) (counted, reported at exit)\n",w,h); }
#endif
