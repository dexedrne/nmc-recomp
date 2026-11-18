/* vclock.c : KHNMC_CLOCK=frames, the guest's clocks follow a virtual 60 Hz display instead of the host (TASK18).
 *
 * Why.  The CPU Vulkan driver renders the field at a fraction of real time.  The game's own time is the display's: its timing thread
 * (0x1287d0, started by GXDevice.init) calls IDXGIOutput::WaitForVBlank once per loop, measures the loop period with
 * QueryPerformanceCounter and, once per ~1.0 measured periods of a 59.94 Hz display, releases the two frame semaphores (the device's
 * at [GXDevice+0x300] through 0xfdc70, waited for by the settings-menu loop 0x129630, and the main loop's at [0x140abc610] through
 * 0x15edb0 -> 0xfdb10, waited for by the end-of-frame task 0x15ee10; both waits go through 0xfdd90, and both releases do nothing when
 * nobody waits).  The game thread makes ONE game frame per release and never skips frames.  With host time everywhere a slow renderer
 * therefore gives slow motion, while everything that reads the host clock directly keeps real time: the XAudio2 device clock
 * (null_audio.c), the sound driver's periodic handlers (0x784c0 / 0x77fc0: elapsed milliseconds from QueryPerformanceCounter, woken
 * by winmm timers through 0xfb040), the frame tick's timeGetTime (0x12ba80).  Picture and sound drift apart by the slow-down factor.
 *
 * What.  One clock for all of them, advanced by the display:
 *   - until the first WaitForVBlank the virtual clock IS the host's monotonic clock (boot behaves as before);
 *   - from then on it only moves inside WaitForVBlank, by exactly 1/60 s per call ("virtual vblank"), stopping on the way at every
 *     instant a registered client has work (the audio mixer's 10 ms passes) until that client is done;
 *   - QueryPerformanceCounter, timeGetTime and clock() read it (core.c kh_now_ns), winmm timers wait on it (k32.c), the mixer's
 *     device clock is it (null_audio.c: 800 sample frames per virtual vblank, whatever the host does);
 *   - Sleep, wait time-outs and file times stay on the host clock: they are upper bounds and wall dates, not the game's time.
 * When a virtual vblank is released (the "gate"; every exit is counted and reported):
 *   frame-presented   the timing thread released a frame semaphore after the previous vblank (seen at the ReleaseSemaphore import),
 *                     the game thread made a game frame (0x105e10) and is back in its frame wait, and a Present happened since: the
 *                     normal case, as soon as it holds;
 *   multi-vblank      a release was seen and the game thread is back in its frame wait WITHOUT having made a game frame: the
 *                     end-of-frame task waits two or three vblanks per frame in its 30 / 20 fps modes (three call sites of 0x15ee10
 *                     in 0x15474b; seen in the field's opening seconds).  No picture is due: at once;
 *   no-release        no frame semaphore was released after the previous vblank and the game thread waits: nothing can happen
 *                     before the next vblank (the timing thread's 59.94 Hz accumulator withholds one release in ~1000 periods of a
 *                     60 Hz display).  At once; a second one in a row waits one period of HOST time, so that a state in which the
 *                     timing thread never releases cannot run the clock faster than real time;
 *   game-busy         no release and the game thread is not in a frame wait: one period passes anyway, time cannot stop.  After
 *                     VC_BUSY_BOOT_NS of host time while no frame was ever presented in step (boot), after VC_BUSY_RUN_NS once
 *                     the game has run in step: a game thread that the HOST holds up must not turn host time into game time
 *                     (seen in run v4 of TASK18: the host starved the process for 17 s, the 100 ms rule of that version let 69
 *                     vblanks pass over 5 game frames, a 1.1 s freeze in the picture stream);
 *   stall-guard       a release happened but the frame did not come back within VC_STALL_NS (a frame without a Present).
 * A virtual vblank is never released sooner than one display period of HOST time after the previous one: a display does not run faster
 * than real time, and loading (host-bound) must not take more game frames than on real hardware.  KHNMC_CLOCK_PACE=free removes that
 * floor (quick tests on the null backend: the route then runs as fast as the host allows, and loads take many more game frames).
 * The video stream (gfx_vkd3d.c) writes one picture per virtual vblank, so picture k and sound sample 800*k are the same instant by
 * construction; a vblank without a new Present repeats the previous picture, as a display does.
 * Unset / "host": every function here is a pass-through to the host clock and nothing changes. */
#include "khnmc_rt.h"
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

int kh_vclock_on;                                   /* 1 = KHNMC_CLOCK=frames (set once at start-up, or by a selftest) */
/* TASK28, KHNMC_CLOCK_SYNC=all: the clock moves past an instant only when every guest thread is idle (blocked on an object that would not
 * satisfy it, in a clock wait, parked); Sleep and wait time-outs are on the virtual clock (a clock client at the deadline); the gate's
 * host-time exits (game-busy, stall-guard) become "the guest is quiet": time passes when nothing can happen without it.  Work of the
 * guest's own threads (the sound driver's stream handler decoding, the background loader reading) therefore takes no virtual time
 * and is finished before the next instant, whatever the host's speed.  A guest that never goes quiet for VC_QUIET_GUARD_NS of host
 * time is let go (counted, the thread named): that step is not deterministic. */
int kh_vclock_sync;
#define VC_QUIET_GUARD_NS 60000000000ull               /* a field load takes 5..10 s of host time on one core: that is work, not a hang */
static u64 n_quiet_waits, n_quiet_guard, n_vsleeps, quiet_host_ns; static int n_quiet_guard_logged;
/* review U35 n=20/29: ONE guard for every wait on quiescence.  A clock client due at the current instant (the mixer, a timer, a Sleep, a
 * wait's time-out) waits for the guest to be quiet before it runs; the clock used to charge that wait as the client's own work (30
 * slices, vclock-client-stuck) before its 60 s guard could ever apply, and nothing let such a client go.  Now the client marks itself
 * (quiet_wait), the clock does not charge those slices, and the client is let go by the same guard (counted, the thread named). */
static u64 quiet_guard_ns=VC_QUIET_GUARD_NS;           /* the selftest shortens it */
static u64 n_client_quiet_guard; static int n_client_guard_logged;
static int stuck_record; static u64 n_stuck_recorded;  /* selftest only: a stuck client is counted instead of the named fatal */
#define VC_HZ 60ull
#define VC_BUSY_BOOT_NS 100000000ull
#define VC_BUSY_RUN_NS 1000000000ull
#define VC_STALL_NS 2000000000ull
#define VC_POLL_NS     200000ull
#define VC_CLIENT_WAIT_S 30
#define FRAME_SEM_VA (GS_BASE+0xabc610ull)          /* main loop: pointer to the frame semaphore object {u32 waiters; u32 pad; HANDLE} */
#define DEV_SEM_OFF 0x300ull                        /* GXDevice: pointer to its frame semaphore object (same layout) */
#define MAXCLIENTS 16
#define MAXLISTEN 4

u64 kh_host_ns(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000000000ull+(u64)ts.tv_nsec; }

static pthread_mutex_t vm=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t vcv; static pthread_once_t vonce=PTHREAD_ONCE_INIT;
static _Atomic int stepped;                         /* 0 = the virtual clock is the host clock, 1 = moved by virtual vblanks only */
static _Atomic u64 v_now;                           /* ns, valid when stepped */
static u64 v_t0, host_t0;                           /* virtual / host time when stepping began */
static _Atomic u64 n_vblank;                        /* virtual vblanks since stepping began */
static u64 n_normal, n_multi, n_norel, n_busy, n_stall, gate_host_ns, max_gate_ns, presents_seen, ticks_seen, frames_seen; static int last_how=-1;
typedef struct { int used; const char *name; u64 due; int prio; KThread *keyth; int keybase, keyseq; int quiet_wait; u64 quiet_since; } VClient;   /* keyth: prio = keybase + key(keyth) * 8 + keyseq, read when compared; quiet_wait: due, first in line, waiting for quiescence since quiet_since (host ns) */
static int cprio(int i);
static VClient clients[MAXCLIENTS];
static void (*listeners[MAXLISTEN])(KCtx *g,u64 vblank,u64 vnow_ns); static int nlisten;
extern _Atomic u64 gfx_presents;
static int gate_log;                                /* KHNMC_CLOCK_LOG=1: one line per vblank that was not released by a presented frame */
static _Atomic u64 frame_sem_h[2], frame_releases;  /* handles of the two frame semaphores (0 = unknown) ; ReleaseSemaphore calls on them */

static u64 sem_handle(KCtx *g,u64 ptr_va){ if(!GA_OK(ptr_va)||!GA_OK(ptr_va+7)) return 0; u64 o=rd64(g,ptr_va); if(!GA_OK(o)||!GA_OK(o+15)) return 0; return rd64(g,o+8); }
static int sem_waiters(KCtx *g,u64 ptr_va){ if(!GA_OK(ptr_va)||!GA_OK(ptr_va+7)) return 0; u64 o=rd64(g,ptr_va); if(!GA_OK(o)||!GA_OK(o+15)) return 0; return (s32)rd32(g,o)>0; }
/* the gate reads the game's state through these; the selftest replaces them */
static int game_parked(void);
static int game_waiting_real(KCtx *g,u64 dev){ (void)g;(void)dev; return game_parked(); }
static int guest_waiters(KCtx *g,u64 dev){ return sem_waiters(g,FRAME_SEM_VA)||(dev&&sem_waiters(g,dev+DEV_SEM_OFF)); }
static u64 presents_real(void){ return atomic_load(&gfx_presents); }
static u64 releases_real(void){ return atomic_load(&frame_releases); }
u64 kh_input_game_frames(void);
static int (*gate_waiting)(KCtx*,u64)=game_waiting_real; static u64 (*gate_presents)(void)=presents_real, (*gate_releases)(void)=releases_real, (*gate_frames)(void)=kh_input_game_frames;
static u64 gate_busy_ns=VC_BUSY_BOOT_NS, gate_busy_run_ns=VC_BUSY_RUN_NS, gate_stall_ns=VC_STALL_NS, gate_period_ns=1000000000ull/VC_HZ;
static u64 pace_ns=1000000000ull/VC_HZ, last_release_host, paced_ns;      /* pace_ns: least host time between two releases (0 = none) */
/* kernel32!ReleaseSemaphore and WaitForSingleObject (threads.c) on the game's two frame semaphores.  "Parked" = a thread is inside a
 * frame wait and no release is on its way to it.  The game's own waiter count ([object+0]) cannot say that: it is still 1 between the
 * release and the moment the woken thread runs (lock dec at 0xfddab), and the timing thread is back in WaitForVBlank long before.
 * So releases and successful waits are counted here; "no release on its way" = their difference is at its resting value.  That value
 * is not known up front (a semaphore can start with a count, a wait can begin before the handle is known), so it is CALIBRATED: when
 * a thread has sat in a frame wait with both semaphore counts at zero and the difference unchanged for VC_QUIET_NS of host time, it
 * is parked by definition and the difference of that moment is the resting value.  Until the first calibration the game counts as
 * not parked (the game-busy exit carries boot).  The leave order (in-wait down, then taken up) keeps the test false across a wake-up. */
#define VC_QUIET_NS 50000000ull
static _Atomic u64 frame_taken; static _Atomic long frame_in_wait, frame_rest; static _Atomic int frame_rest_ok; static u64 n_calib;
static int is_frame_sem(u64 h){ return h&&(h==atomic_load(&frame_sem_h[0])||h==atomic_load(&frame_sem_h[1])); }
void kh_vclock_note_release(u64 h){ if(is_frame_sem(h)) atomic_fetch_add(&frame_releases,1); }
int kh_vclock_frame_wait_enter(u64 h){ if(!is_frame_sem(h)) return 0; atomic_fetch_add(&frame_in_wait,1); return 1; }
void kh_vclock_frame_wait_leave(int got){ atomic_fetch_sub(&frame_in_wait,1); if(got) atomic_fetch_add(&frame_taken,1); }
static long frame_diff(void){ return (long)(atomic_load(&frame_releases)-atomic_load(&frame_taken)); }
static int game_parked(void){ return atomic_load(&frame_rest_ok)&&atomic_load(&frame_in_wait)>0&&frame_diff()==atomic_load(&frame_rest)&&atomic_load(&frame_in_wait)>0; }
static int frame_sems_empty(void){ for(int i=0;i<2;i++){ u64 h=atomic_load(&frame_sem_h[i]); if(!h) continue; KWaitObj *w=h_get(h,HK_SEM); if(w&&w->count>0) return 0; } return 1; }
/* called on every gate poll with the host time; q = the caller's quiet-period state {since, difference at its start} */
static void frame_calibrate(u64 now,u64 *q_since,long *q_diff){
    long d=frame_diff();
    if(atomic_load(&frame_in_wait)>0&&frame_sems_empty()&&(!*q_since||d==*q_diff)){
        if(!*q_since){ *q_since=now; *q_diff=d; }
        else if(now-*q_since>=VC_QUIET_NS&&(!atomic_load(&frame_rest_ok)||atomic_load(&frame_rest)!=d)){ atomic_store(&frame_rest,d); atomic_store(&frame_rest_ok,1); n_calib++;
            if(gate_log) fprintf(stderr,"kh: vclock: frame wait calibrated: resting difference releases - taken = %ld (calibration %llu)\n",d,(unsigned long long)n_calib); }
    } else *q_since=0;
}

/* the condition's timed wait takes CLOCK_MONOTONIC deadlines (abs_in): kh_compat.h, because Darwin has no pthread_condattr_setclock
 * and reads a deadline given to pthread_cond_timedwait as wall-clock time (a monotonic one would time out at once) */
static void vc_once(void){ kh_cond_init(&vcv); }
static void abs_in(struct timespec *ts,u64 ns){ u64 t=kh_host_ns()+ns; ts->tv_sec=(time_t)(t/1000000000ull); ts->tv_nsec=(long)(t%1000000000ull); }
static void host_sleep_until(u64 t){ struct timespec d={(time_t)(t/1000000000ull),(long)(t%1000000000ull)}; kh_sleep_until(&d); }   /* kh_compat.h: clock_nanosleep on Linux, a nanosleep loop on Darwin */

void kh_vclock_init(void){
    pthread_once(&vonce,vc_once);
    const char *e=getenv("KHNMC_CLOCK");
    if(!e||!*e||!strcmp(e,"host")) return;
    if(strcmp(e,"frames")) kh_fatal(NULL,"clock-mode-unknown: KHNMC_CLOCK=%s (known: host = the host's clock, the default; frames = a virtual 60 Hz display drives every guest clock)",e);
    kh_vclock_on=1;
    { const char *lg=getenv("KHNMC_CLOCK_LOG"); gate_log=lg&&*lg=='1'; }
    { const char *sy=getenv("KHNMC_CLOCK_SYNC"); kh_vclock_sync=1;                     /* TASK28: deterministic by default */
      if(sy&&*sy){ if(!strcmp(sy,"all")) kh_vclock_sync=1; else if(!strcmp(sy,"timers")) kh_vclock_sync=0; else kh_fatal(NULL,"clock-sync-unknown: KHNMC_CLOCK_SYNC=%s (known: all = the clock moves only while every guest thread waits, the default; timers = only the winmm timers and the mixer are ordered clock clients)",sy); }
      if(kh_vclock_sync) fprintf(stderr,"kh: vclock: KHNMC_CLOCK_SYNC=all: the virtual clock moves only while every guest thread waits; Sleep and wait time-outs are on the virtual clock\n"); }
    const char *pc=getenv("KHNMC_CLOCK_PACE");
    if(pc&&!strcmp(pc,"free")) pace_ns=0; else if(pc&&*pc&&strcmp(pc,"realtime")) kh_fatal(NULL,"clock-pace-unknown: KHNMC_CLOCK_PACE=%s (known: realtime = never faster than a real %llu Hz display, the default; free = as fast as the host allows)",pc,VC_HZ);
    fprintf(stderr,"kh: vclock: KHNMC_CLOCK=frames: QueryPerformanceCounter / timeGetTime / winmm timers / the audio device clock follow a virtual %llu Hz display from the first WaitForVBlank on (1/%llu s per call; host time before that); pace=%s\n",VC_HZ,VC_HZ,pace_ns?"realtime (never faster than the host clock)":"free");
}
void kh_vclock_idle(int on){
    KThread *t=cur_thread();
    if(on==1&&t) atomic_store(&t->vc_state,2); else if(on==0&&t) atomic_store(&t->vc_state,0);
    if(on&&kh_vclock_sync&&!pthread_mutex_trylock(&vm)){ pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm); }   /* a waiting step re-tests */
}
int kh_vclock_deadline_add(u64 dl){ int id=kh_vclock_client_add_prio("timed-wait",dl,1<<28); kh_vclock_client_set_key(id,1<<28,cur_thread(),1); return id; }
void kh_vclock_deadline_drop(int id){ kh_vclock_client_drop(id); }
int kh_vclock_vsleep(u32 ms){
    if(!kh_vclock_sync||!atomic_load(&stepped)||!cur_thread()) return 0;
    u64 dl=kh_vclock_now_ns()+(u64)ms*1000000ull; int id=kh_vclock_client_add_prio("sleep",dl,1<<28); kh_vclock_client_set_key(id,1<<28,cur_thread(),0); n_vsleeps++;
    kh_vclock_client_wait(id,dl);                                                        /* ordered after the mixer and the timers of that instant, the guest quiet */
    kh_vclock_client_drop(id);                                                           /* running (client_wait marked it), then the clock may leave the deadline */
    return 1;
}
/* caller holds vm: the client `id` is (on=1) / is no longer (on=0) waiting for quiescence at its instant; the clock re-tests */
static void client_quiet_mark(int id,int on){
    if(on&&!clients[id].quiet_wait){ clients[id].quiet_wait=1; clients[id].quiet_since=kh_host_ns(); pthread_cond_broadcast(&vcv); }
    else if(!on&&clients[id].quiet_wait){ clients[id].quiet_wait=0; clients[id].quiet_since=0; pthread_cond_broadcast(&vcv); }
}
/* caller holds vm: a due client has waited quiet_guard_ns for quiescence; it runs anyway (counted, named): that instant is not ordered */
static void client_let_go(int id,u64 waited_ns,const char *who,u64 t){
    n_client_quiet_guard++;
    if(n_client_guard_logged++<8) fprintf(stderr,"kh: vclock: quiescence guard: clock client '%s' due at virtual %llu ns let go after %.1f s of host time waiting for thread %s; its work at that instant is not ordered\n",
                                          clients[id].name,(unsigned long long)t,waited_ns/1e9,who&&*who?who:"(unknown)");
}
/* a timed wait's deadline (threads.c kh_wait): reached, the clock's ordering for that instant respected and the guest quiet (or the
 * quiescence guard passed: review U35 n=20/29; meanwhile the clock does not charge this client as stuck) */
int kh_vclock_deadline_due(int id,u64 dl){
    if(atomic_load(&v_now)<dl) return 0;
    pthread_mutex_lock(&vm); int ahead=0, my=cprio(id); for(int i=0;i<MAXCLIENTS;i++) if(i!=id&&clients[i].used&&clients[i].due<=dl&&cprio(i)<my){ ahead=1; break; }
    client_quiet_mark(id,!ahead); u64 q0=clients[id].quiet_since; pthread_mutex_unlock(&vm);
    if(ahead) return 0;
    char who[64]=""; int go=kh_threads_quiet(who,sizeof who);
    pthread_mutex_lock(&vm);
    if(!go){ u64 h=kh_host_ns(); if(h-q0>=quiet_guard_ns){ client_let_go(id,h-q0,who,dl); go=1; } }
    if(go) client_quiet_mark(id,0);
    pthread_mutex_unlock(&vm);
    return go;
}
u64 kh_vclock_now_ns(void){
    if(atomic_load(&stepped)) return atomic_load(&v_now);
    u64 h=kh_host_ns(); return atomic_load(&stepped)?atomic_load(&v_now):h;
}
u64 kh_vclock_vblanks(void){ return atomic_load(&n_vblank); }
/* TASK28, KHNMC_AUDIO=pull with the frames clock: the audio device paces the clock (the mixer waits for room before a pass while it holds
 * the clock), so the host-time floor is not wanted on top of it: with it the clock could fall behind the device (loads) and never catch
 * up.  Unless KHNMC_CLOCK_PACE was given explicitly, the pace becomes free.  Returns 1 when it changed the pace. */
int kh_vclock_pace_by_device(void){ const char *pc=getenv("KHNMC_CLOCK_PACE"); if(!kh_vclock_on||(pc&&*pc)) return 0; pace_ns=0; return 1; }
u64 kh_vclock_origin(void){ return atomic_load(&stepped)?v_t0:0; }   /* virtual time of the first virtual vblank (0 = not stepped yet) */
int kh_vclock_stepped(void){ return atomic_load(&stepped); }
/* review of task 28: a clock client created before the first WaitForVBlank (the audio mixer, a winmm timer) waits here before it fixes its
 * grid, so that the grid's phase against the vblanks does not depend on the host (8 of 100 one-CPU starts created the mixer before the
 * first vblank and got a host-phased grid).  The caller is NOT marked idle: with sync=all the clock cannot leave its origin before the
 * caller has registered there.  Waits at most max_ns of host time; 1 = stepping has begun. */
int kh_vclock_wait_stepped(u64 max_ns){
    pthread_once(&vonce,vc_once); struct timespec d; abs_in(&d,max_ns);
    pthread_mutex_lock(&vm);
    while(!atomic_load(&stepped)) if(kh_cond_wait_until(&vcv,&vm,&d)==ETIMEDOUT) break;
    int st=atomic_load(&stepped); pthread_mutex_unlock(&vm); return st;
}
/* block until the virtual clock reaches t (a host sleep while the virtual clock still is the host clock) */
void kh_vclock_wait_until(u64 t){
    pthread_once(&vonce,vc_once);
    if(kh_vclock_sync){ kh_vclock_idle(1); kh_vclock_wait_until_raw(t); kh_vclock_idle(0); return; }
    kh_vclock_wait_until_raw(t);
}
void kh_vclock_wait_until_raw(u64 t){
    for(;;){
        if(!atomic_load(&stepped)){ u64 h=kh_host_ns(); if(h>=t) return; host_sleep_until(t-h>50000000ull?h+50000000ull:t); continue; }   /* re-check every 50 ms: stepping may begin */
        pthread_mutex_lock(&vm);
        while(atomic_load(&stepped)&&atomic_load(&v_now)<t) pthread_cond_wait(&vcv,&vm);
        int done=atomic_load(&stepped)&&atomic_load(&v_now)>=t; pthread_mutex_unlock(&vm);
        if(done) return;
    }
}
/* a client is a thread with work at known virtual instants (the mixer's passes): the clock stops at every `due` and goes on only when
 * the client has announced a later one.  first_due = the instant of its first piece of work. */
int kh_vclock_client_add_prio(const char *name,u64 first_due,int prio){
    pthread_once(&vonce,vc_once); pthread_mutex_lock(&vm); int id=-1;
    for(int i=0;i<MAXCLIENTS;i++) if(!clients[i].used){ clients[i].used=1; clients[i].name=name; clients[i].due=first_due; clients[i].prio=prio; clients[i].keyth=NULL; clients[i].quiet_wait=0; clients[i].quiet_since=0; id=i; break; }
    pthread_mutex_unlock(&vm);
    if(id<0) kh_fatal(NULL,"vclock-clients: more than %d clock clients",MAXCLIENTS);
    return id;
}
int kh_vclock_client_add(const char *name,u64 first_due){ return kh_vclock_client_add_prio(name,first_due,0); }
/* TASK28: clients with work at the SAME instant run one after the other, in the order of their prio (smaller first): the mixer's pass
 * (prio 0) before the winmm timers (k32.c, keyed by the creating thread), then sleepers and time-outs (keyed by their thread), so a
 * timer's handler always sees the pass of its instant done, and the
 * calls it makes (stamped with that instant) go to the next pass.  A client of prio p waits here for its instant t: the clock at t
 * or later, and every used client of a smaller prio due later than t (done with t).  Before the clock steps, it is a host sleep. */
static void client_wait_raw(int id,u64 t);
void kh_vclock_client_wait(int id,u64 t){
    pthread_once(&vonce,vc_once);
    if(kh_vclock_sync) kh_vclock_idle(1);
    client_wait_raw(id,t);
    if(kh_vclock_sync) kh_vclock_idle(0);
}
static void client_wait_raw(int id,u64 t){
    for(;;){
        if(!atomic_load(&stepped)){ u64 h=kh_host_ns(); if(h>=t) return; host_sleep_until(t-h>50000000ull?h+50000000ull:t); continue; }
        pthread_mutex_lock(&vm);
        for(;;){
            if(!atomic_load(&stepped)) break;
            int ahead=0; if(atomic_load(&v_now)>=t){ int my=cprio(id); for(int i=0;i<MAXCLIENTS;i++) if(i!=id&&clients[i].used&&clients[i].due<=t&&cprio(i)<my){ ahead=1; break; } }
            if(atomic_load(&v_now)>=t&&!ahead){ if(!kh_vclock_sync) break;                     /* sync=all: and nothing else of the guest is still running at t */
                client_quiet_mark(id,1);                                                       /* not this client's work: the clock does not charge it (review U35 n=20/29) */
                char who[64]=""; pthread_mutex_unlock(&vm); int q=kh_threads_quiet(who,sizeof who); pthread_mutex_lock(&vm); if(q) break;
                u64 h=kh_host_ns(); if(h-clients[id].quiet_since>=quiet_guard_ns){ client_let_go(id,h-clients[id].quiet_since,who,t); break; }
                struct timespec d2; abs_in(&d2,200000ull); kh_cond_wait_until(&vcv,&vm,&d2); continue; }
            client_quiet_mark(id,0);
            pthread_cond_wait(&vcv,&vm);
        }
        client_quiet_mark(id,0);                                                               /* runs its work now: charged from here on */
        int done=atomic_load(&stepped); pthread_mutex_unlock(&vm);
        if(done) return;
    }
}
void kh_vclock_client_set_prio(int id,int prio){ pthread_mutex_lock(&vm); clients[id].prio=prio; pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm); }
/* TASK28: same-instant order must not depend on which thread happened to create its timer or start its sleep first (a run with the
 * MSEC and VSYNC handlers' timers created in the other order swapped their ticks at a common instant: 1 title start in 100 differed).
 * The key is the guest's own thread name (SetThreadName, set long before the first tick) and start routine, never an id that thread
 * creation order assigns.  20 bits; equal keys (unnamed threads with the same start routine) fall back to the order of their ids. */
int kh_vclock_thread_key(KThread *t){
    if(!t) return 0; u32 h=2166136261u; for(const char *p=t->name;*p;p++){ if(!strncmp(t->name,"thr",3)) break; h^=(u8)*p; h*=16777619u; }
    u64 st=t->start; h^=(u32)st^(u32)(st>>32); h*=16777619u; return (int)(h&0xFFFFFu);
}
static int cprio(int i){ return clients[i].keyth?clients[i].keybase+kh_vclock_thread_key(clients[i].keyth)*8+clients[i].keyseq:clients[i].prio; }
void kh_vclock_client_set_key(int id,int base,KThread *t,int seq){ pthread_mutex_lock(&vm); clients[id].keyth=t; clients[id].keybase=base; clients[id].keyseq=seq&7; pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm); }
void kh_vclock_client_due(int id,u64 due){ pthread_mutex_lock(&vm); clients[id].due=due; pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm); }
void kh_vclock_client_drop(int id){ pthread_mutex_lock(&vm); clients[id].used=0; pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm); }
void kh_vclock_on_vblank(void (*fn)(KCtx *g,u64 vblank,u64 vnow_ns)){ for(int i=0;i<nlisten;i++) if(listeners[i]==fn) return; if(nlisten>=MAXLISTEN) kh_fatal(NULL,"vclock-listeners: more than %d vblank listeners",MAXLISTEN); listeners[nlisten++]=fn; }

/* caller holds vm.  Moves the clock to `target`; it never passes an instant at which a client has work before that work is done, and
 * returns only when everything due at `target` is done too. */
static void advance_locked(KCtx *g,u64 target){
    for(;;){
        u64 now=atomic_load(&v_now);
        unsigned stuck=0;                                                               /* slices that expired (kh_compat.h: a stopped process loses one slice, not the budget) */
        for(;;){ int behind=-1, qw=0; for(int i=0;i<MAXCLIENTS;i++) if(clients[i].used&&clients[i].due<=now){ if(behind<0) behind=i; qw|=clients[i].quiet_wait; }
            if(behind<0) break;
            /* a due client waits for the guest to be quiet (every other due client waits behind it): not stuck work, so not charged;
             * that wait has the quiescence guard (review U35 n=20/29) */
            if(qw){ struct timespec d; abs_in(&d,1000000000ull); kh_cond_wait_until(&vcv,&vm,&d); continue; }
            if(kh_cond_wait_stuck(&vcv,&vm,&stuck,VC_CLIENT_WAIT_S)==ETIMEDOUT){ if(stuck_record){ n_stuck_recorded++; break; }
                const char *nm=clients[behind].name; u64 due=clients[behind].due; pthread_mutex_unlock(&vm);
                kh_fatal(g,"vclock-client-stuck: clock client '%s' did not finish its work due at virtual time %llu ns in %d slices of waiting, about that many seconds of this process running (the clock is at %llu ns)",nm,(unsigned long long)due,VC_CLIENT_WAIT_S,(unsigned long long)now); } }
        if(now>=target) return;
        if(kh_vclock_sync){                                                             /* nothing may still be running at `now` */
            u64 q0=0; char who[64]="";
            while(!kh_threads_quiet(who,sizeof who)){
                u64 h=kh_host_ns(); if(!q0){ q0=h; n_quiet_waits++; }
                if(h-q0>=quiet_guard_ns){ n_quiet_guard++; if(n_quiet_guard_logged++<8) fprintf(stderr,"kh: vclock: quiescence guard: thread %s still running %.1f s of host time after virtual %llu ns; the clock goes on\n",who,(h-q0)/1e9,(unsigned long long)now); break; }
                struct timespec d2; abs_in(&d2,200000ull); kh_cond_wait_until(&vcv,&vm,&d2);
            }
            if(q0) quiet_host_ns+=kh_host_ns()-q0;
            int moved=0; for(int i=0;i<MAXCLIENTS;i++) if(clients[i].used&&clients[i].due<=atomic_load(&v_now)){ moved=1; break; }   /* a deadline was added meanwhile */
            if(moved) continue;
        }
        u64 stop=target; for(int i=0;i<MAXCLIENTS;i++) if(clients[i].used&&clients[i].due<stop) stop=clients[i].due;
        atomic_store(&v_now,stop); pthread_cond_broadcast(&vcv);
    }
}
static void one_period_locked(KCtx *g,u64 *k_out,u64 *vnow_out){
    u64 k=atomic_load(&n_vblank)+1; advance_locked(g,v_t0+k*1000000000ull/VC_HZ); atomic_store(&n_vblank,k); *k_out=k; *vnow_out=atomic_load(&v_now);
}
/* IDXGIOutput::WaitForVBlank in frames mode (null_d3d12.c): the gate, then one display period.  dev = the GXDevice of the calling
 * timing thread (0 when the call does not come from one of its two known call sites). */
void kh_vclock_vblank(KCtx *g,u64 dev){
    pthread_once(&vonce,vc_once);
    if(kh_vclock_sync) kh_vclock_idle(1);                                              /* the timing thread moves the clock: it is not "running" */
    kh_vclock_vblank_raw(g,dev);
    if(kh_vclock_sync) kh_vclock_idle(0);
}
static u64 n_quiet_rel;
void kh_vclock_vblank_raw(KCtx *g,u64 dev){
    u64 t_enter=kh_host_ns();
    pthread_mutex_lock(&vm);
    if(!atomic_load(&stepped)){ v_t0=host_t0=t_enter; atomic_store(&v_now,v_t0); atomic_store(&stepped,1); presents_seen=gate_presents(); ticks_seen=gate_releases(); frames_seen=gate_frames(); pthread_cond_broadcast(&vcv);
        fprintf(stderr,"kh: vclock: first WaitForVBlank: the virtual clock leaves the host clock at %llu ns and now moves 1/%llu s per virtual vblank\n",(unsigned long long)v_t0,VC_HZ); }
    int prev=last_how;
    pthread_mutex_unlock(&vm);
    atomic_store(&frame_sem_h[0],sem_handle(g,FRAME_SEM_VA)); if(dev) atomic_store(&frame_sem_h[1],sem_handle(g,dev+DEV_SEM_OFF));
    int how, w=0, released=0; u64 p=0, gf=0, q_since=0; long q_diff=0;
    for(;;){
        u64 now=kh_host_ns(); frame_calibrate(now,&q_since,&q_diff); released=gate_releases()!=ticks_seen; w=gate_waiting(g,dev); p=gate_presents(); gf=gate_frames();
        if(released){ if(w&&gf==frames_seen){ how=4; break; } if(w&&p>presents_seen){ how=0; break; }
            if(kh_vclock_sync){ if(kh_threads_quiet(NULL,0)){ how=5; break; } if(now-t_enter>=VC_QUIET_GUARD_NS){ how=3; break; } }
            else if(now-t_enter>=gate_stall_ns){ how=3; break; } }
        else { if(w&&(prev!=1||now-t_enter>=gate_period_ns)){ how=1; break; }
            if(!w&&kh_vclock_sync){ if(kh_threads_quiet(NULL,0)){ how=5; break; } if(now-t_enter>=VC_QUIET_GUARD_NS){ how=2; break; } }
            else if(!w&&now-t_enter>=(n_normal?gate_busy_run_ns:gate_busy_ns)){ how=2; break; } }
        struct timespec ts={0,(long)VC_POLL_NS}; nanosleep(&ts,0);
    }
    if(gate_log&&how){ static const char *const hn[6]={"frame-presented","no-release","game-busy","stall-guard","multi-vblank","quiet"};
        fprintf(stderr,"kh: vclock: vblank %llu released by %s after %.1f ms: game-parked=%d (guest waiter counts: main=%d device=%d) release-seen=%d presents=%llu (+%llu since the last vblank) game-frames=%llu (+%llu) [in-wait=%ld taken=%llu releases=%llu seen=%llu rest=%ld%s]\n",(unsigned long long)atomic_load(&n_vblank)+1,hn[how],(kh_host_ns()-t_enter)/1e6,
                w,sem_waiters(g,FRAME_SEM_VA),dev?sem_waiters(g,dev+DEV_SEM_OFF):-1,released,(unsigned long long)p,(unsigned long long)(p-presents_seen),(unsigned long long)gf,(unsigned long long)(gf-frames_seen),atomic_load(&frame_in_wait),(unsigned long long)atomic_load(&frame_taken),(unsigned long long)atomic_load(&frame_releases),(unsigned long long)ticks_seen,atomic_load(&frame_rest),atomic_load(&frame_rest_ok)?"":" (not calibrated)"); }
    if(pace_ns&&last_release_host){ u64 now=kh_host_ns(), floor_t=last_release_host+pace_ns; if(now<floor_t){ host_sleep_until(floor_t); paced_ns+=floor_t-now; } }
    last_release_host=kh_host_ns();
    u64 waited=last_release_host-t_enter, k, vnow;
    pthread_mutex_lock(&vm);
    presents_seen=gate_presents(); ticks_seen=gate_releases(); frames_seen=gate_frames(); gate_host_ns+=waited; if(waited>max_gate_ns) max_gate_ns=waited; last_how=how;
    if(how==0) n_normal++; else if(how==1) n_norel++; else if(how==2) n_busy++; else if(how==4) n_multi++; else if(how==5) n_quiet_rel++; else n_stall++;
    one_period_locked(g,&k,&vnow);
    pthread_mutex_unlock(&vm);
    for(int i=0;i<nlisten;i++) listeners[i](g,k,vnow);
}
void kh_vclock_report(FILE *f){
    if(!kh_vclock_on) return;
    pthread_mutex_lock(&vm); u64 k=atomic_load(&n_vblank); int st=atomic_load(&stepped); u64 host=st?kh_host_ns()-host_t0:0, virt=st?atomic_load(&v_now)-v_t0:0;
    fprintf(f,"kh: vclock: mode=frames %llu Hz stepping=%s virtual-vblanks=%llu released by: frame-presented=%llu multi-vblank=%llu no-release=%llu game-busy=%llu stall-guard=%llu; frame-semaphore releases=%llu taken=%llu frame-wait calibrations=%llu; virtual time %.3f s in %.1f s of host time (x%.3f of real time); host time inside the gate %.1f s (longest %.1f ms), of which %.1f s held back by pace=%s\n",
            VC_HZ,st?"yes":"NEVER BEGAN (no WaitForVBlank call)",(unsigned long long)k,(unsigned long long)n_normal,(unsigned long long)n_multi,(unsigned long long)n_norel,(unsigned long long)n_busy,(unsigned long long)n_stall,
            (unsigned long long)atomic_load(&frame_releases),(unsigned long long)atomic_load(&frame_taken),(unsigned long long)n_calib,virt/1e9,host/1e9,host?(double)virt/(double)host:0.0,gate_host_ns/1e9,max_gate_ns/1e6,paced_ns/1e9,pace_ns?"realtime":"free");
    pthread_mutex_unlock(&vm);
    extern void kh_timer_frames_report(FILE*); kh_timer_frames_report(f);
    if(kh_vclock_sync){ extern u64 kh_threads_vwaits(void); extern u64 kh_threads_wake_waits(u64*); extern u64 kh_threads_rel_retests(void); u64 wg=0, ww=kh_threads_wake_waits(&wg);
        fprintf(f,"kh: vclock: KHNMC_CLOCK_SYNC=all: vblanks released because the guest was quiet=%llu; clock steps that waited for running threads=%llu (%.1f s of host time), let go by the %.0f s guard=%llu; due clock clients let go by the same guard=%llu; Sleep calls on the virtual clock=%llu, waits with a virtual time-out=%llu; wake-ups after which the waker waited for the woken thread=%llu, let go by the 2 s guard=%llu; quiescence tests not quiet because a thread was released during their pass=%llu\n",
        (unsigned long long)n_quiet_rel,(unsigned long long)n_quiet_waits,quiet_host_ns/1e9,quiet_guard_ns/1e9,(unsigned long long)n_quiet_guard,(unsigned long long)n_client_quiet_guard,(unsigned long long)n_vsleeps,(unsigned long long)kh_threads_vwaits(),(unsigned long long)ww,(unsigned long long)wg,(unsigned long long)kh_threads_rel_retests()); }
}

/* ---- test hooks for the selftests of this and other modules (audio clock, timers, video): a stepped clock that only the test moves */
static int sv_on, sv_stepped; static u64 sv_now, sv_t0, sv_host_t0, sv_vbl;
void kh_vclock_test_begin(u64 start_ns){
    pthread_once(&vonce,vc_once); pthread_mutex_lock(&vm);
    sv_on=kh_vclock_on; sv_now=atomic_load(&v_now); sv_t0=v_t0; sv_host_t0=host_t0; sv_vbl=atomic_load(&n_vblank); sv_stepped=atomic_load(&stepped);
    kh_vclock_on=1; v_t0=start_ns; host_t0=kh_host_ns(); atomic_store(&v_now,start_ns); atomic_store(&n_vblank,0); atomic_store(&stepped,1);
    pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm);
}
void kh_vclock_test_vblank(KCtx *g){                 /* one display period without the gate, listeners included */
    u64 k,vnow; pthread_mutex_lock(&vm); one_period_locked(g,&k,&vnow); pthread_mutex_unlock(&vm);
    for(int i=0;i<nlisten;i++) listeners[i](g,k,vnow);
}
void kh_vclock_test_end(void){
    pthread_mutex_lock(&vm); kh_vclock_on=sv_on; v_t0=sv_t0; host_t0=sv_host_t0; atomic_store(&v_now,sv_now); atomic_store(&n_vblank,sv_vbl); atomic_store(&stepped,sv_stepped);
    pthread_cond_broadcast(&vcv); pthread_mutex_unlock(&vm);
}
/* review U35 n=20/29 (threads.c kh_sync_vclock_selftest): the quiescence guard (returns the previous value), the stuck recorder (on = a
 * stuck client is counted instead of the named fatal; returns the count so far), the clients let go by the guard, a client's mark */
u64 kh_vclock_test_quiet_guard(u64 ns){ pthread_mutex_lock(&vm); u64 o=quiet_guard_ns; quiet_guard_ns=ns; pthread_mutex_unlock(&vm); return o; }
u64 kh_vclock_test_stuck_record(int on){ pthread_mutex_lock(&vm); stuck_record=on; u64 n=n_stuck_recorded; pthread_mutex_unlock(&vm); return n; }
u64 kh_vclock_client_guard_count(void){ pthread_mutex_lock(&vm); u64 n=n_client_quiet_guard; pthread_mutex_unlock(&vm); return n; }
int kh_vclock_test_client_quiet(int id){ pthread_mutex_lock(&vm); int q=clients[id].used&&clients[id].quiet_wait; pthread_mutex_unlock(&vm); return q; }

/* ---- selftest ------------------------------------------------------------------------------------------------------------- */
static int st_w; static _Atomic u64 st_p, st_r, st_f;
static u64 st_frames(void){ return atomic_load(&st_f); }
static int st_waiting(KCtx *g,u64 dev){ (void)g;(void)dev; return st_w; }
static u64 st_presents(void){ return atomic_load(&st_p); }
static u64 st_releases(void){ return atomic_load(&st_r); }
typedef struct { int id; u64 t0, period; _Atomic u64 passes; _Atomic int quit; _Atomic u64 max_lag; } StClient;
static void st_client_thread(KCtx *g){ StClient *c=(StClient*)(uintptr_t)A0(g);
    for(u64 n=0;;n++){ u64 due=c->t0+n*c->period; if(n) kh_vclock_client_due(c->id,due); kh_vclock_wait_until(due); if(atomic_load(&c->quit)) break;
        u64 now=kh_vclock_now_ns(); if(now-due>atomic_load(&c->max_lag)) atomic_store(&c->max_lag,now-due);     /* the clock must not have run past a client instant */
        atomic_fetch_add(&c->passes,1); }
    kh_vclock_client_drop(c->id); KH_RET(g); }
typedef struct { u64 until; _Atomic int done; _Atomic u64 woke_at; } StSleeper;
static void st_sleeper_thread(KCtx *g){ StSleeper *s=(StSleeper*)(uintptr_t)A0(g); kh_vclock_wait_until(s->until); atomic_store(&s->woke_at,kh_vclock_now_ns()); atomic_store(&s->done,1); KH_RET(g); }
static void st_settle(_Atomic int *flag,int want){ for(int i=0;i<3000&&atomic_load(flag)!=want;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } }
static _Atomic int st_tn; static u64 st_tat[8];
static void st_anchor_cb(KCtx *g){ int n=atomic_fetch_add(&st_tn,1); if(n<8) st_tat[n]=kh_vclock_now_ns(); KH_RET(g); }   /* section 9: the virtual instant of each tick */
static u64 st_imp(KCtx *g,void (*f)(KCtx*),u64 a0){ u64 sp=KH_RSP(g),rc=KH_RCX(g); KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=a0; f(g); u64 r=KH_RAX(g); KH_RSP(g)=sp; KH_RCX(g)=rc; return r; }
int kh_vclock_selftest(KCtx *g){
    int checks=0,fails=0;
#define VCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"vclock selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    extern void rt_imp_kernel32__QueryPerformanceCounter(KCtx*), rt_imp_winmm__timeGetTime(KCtx*), rt_imp_winmm__timeSetEvent(KCtx*), rt_imp_winmm__timeKillEvent(KCtx*);
    extern void rt_imp_kernel32__ReleaseSemaphore(KCtx*), rt_imp_kernel32__CreateSemaphoreA(KCtx*), rt_imp_kernel32__CloseHandle(KCtx*);
    extern u64 kh_mk_event_h(int manual,int initial);
    u64 m=ga_calloc(1,64);
    /* 1. host mode: the guest clock is the host clock, waits are host sleeps */
    { int on=kh_vclock_on, st=atomic_load(&stepped); kh_vclock_on=0; u64 a=kh_host_ns(), b=kh_now_ns(), c=kh_host_ns(); VCHECK(a<=b&&b<=c); kh_vclock_on=on;
      if(!st){ a=kh_host_ns(); kh_vclock_wait_until(a+3000000ull); b=kh_host_ns(); VCHECK(b-a>=3000000ull&&b-a<2000000000ull); VCHECK(kh_vclock_now_ns()>=b); } }
    /* 2. stepped mode: nothing moves between vblanks, a vblank is exactly 1/60 s, QueryPerformanceCounter and timeGetTime follow */
    const u64 T0=5000000000000ull; kh_vclock_test_begin(T0);
    VCHECK(kh_vclock_now_ns()==T0&&kh_now_ns()==T0&&kh_qpc()==T0/100);
    { struct timespec ts={0,5000000}; nanosleep(&ts,0); } VCHECK(kh_now_ns()==T0);                       /* 5 ms of host time: the guest clock stands still */
    st_imp(g,rt_imp_kernel32__QueryPerformanceCounter,m); VCHECK(rd64(g,m)==T0/100);
    u32 tg0=(u32)st_imp(g,rt_imp_winmm__timeGetTime,0);
    kh_vclock_test_vblank(g); VCHECK(kh_now_ns()==T0+16666666ull&&kh_vclock_vblanks()==1);
    kh_vclock_test_vblank(g); kh_vclock_test_vblank(g); VCHECK(kh_now_ns()==T0+50000000ull&&kh_qpc()==(T0+50000000ull)/100);      /* 3 vblanks = 50 ms exactly: no drift */
    VCHECK((u32)st_imp(g,rt_imp_winmm__timeGetTime,0)-tg0==50);
    st_imp(g,rt_imp_kernel32__QueryPerformanceCounter,m); VCHECK(rd64(g,m)==(T0+50000000ull)/100);
    for(int i=0;i<57;i++) kh_vclock_test_vblank(g); VCHECK(kh_now_ns()==T0+1000000000ull&&kh_vclock_vblanks()==60);              /* 60 vblanks = 1 s exactly */
    /* 3. a waiter sleeps through host time and wakes when the clock gets there, not before */
    { static StSleeper s; s.until=T0+1000000000ull+40000000ull; atomic_store(&s.done,0); kh_spawn_hostfn_thread(g,"vclock_selftest_sleeper",st_sleeper_thread,(u64)(uintptr_t)&s);
      { struct timespec ts={0,20000000}; nanosleep(&ts,0); } VCHECK(atomic_load(&s.done)==0);
      kh_vclock_test_vblank(g); kh_vclock_test_vblank(g); { struct timespec ts={0,10000000}; nanosleep(&ts,0); } VCHECK(atomic_load(&s.done)==0);      /* +33.3 ms: not yet */
      kh_vclock_test_vblank(g); st_settle(&s.done,1); VCHECK(atomic_load(&s.done)==1&&atomic_load(&s.woke_at)>=s.until); }
    /* 4. a client with a pass every 10 ms: the clock stops at each pass and waits for it, so 600 vblanks = 10 s = exactly 1000 passes
     *    after the first, each started at its own instant (lag 0) */
    { static StClient c; c.t0=kh_vclock_now_ns(); c.period=10000000ull; atomic_store(&c.passes,0); atomic_store(&c.quit,0); atomic_store(&c.max_lag,0); c.id=kh_vclock_client_add("vclock-selftest-client",c.t0);
      u64 start=c.t0; kh_spawn_hostfn_thread(g,"vclock_selftest_client",st_client_thread,(u64)(uintptr_t)&c);
      for(int i=0;i<3;i++) kh_vclock_test_vblank(g); VCHECK(atomic_load(&c.passes)==6);                    /* 50 ms: passes at 0,10,20,30,40,50, all done when the vblank returns */
      for(int i=0;i<597;i++) kh_vclock_test_vblank(g);
      VCHECK(kh_vclock_now_ns()==start+10000000000ull); VCHECK(atomic_load(&c.passes)==1001); VCHECK(atomic_load(&c.max_lag)==0);
      atomic_store(&c.quit,1); kh_vclock_test_vblank(g); for(int i=0;i<3000&&clients[c.id].used;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } VCHECK(!clients[c.id].used); }
    /* 5. winmm timers wait on the virtual clock: a 5 ms periodic timer that sets an event does not fire while the clock stands, fires
     *    when a vblank passes its instant, and a one-shot fires once */
    { u64 ev=kh_mk_event_h(0,0); u64 sp=KH_RSP(g), sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
      KH_RSP(g)=((sp-0x100)&~15ull); wr64(g,KH_RSP(g)+0x20,0x11); KH_RSP(g)-=8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull);                 /* fuEvent = TIME_PERIODIC | TIME_CALLBACK_EVENT_SET */
      KH_RCX(g)=5; KH_RDX(g)=1; KH_R8(g)=ev; KH_R9(g)=0; rt_imp_winmm__timeSetEvent(g); u32 id=(u32)KH_RAX(g); KH_RSP(g)=sp; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3];
      VCHECK(id!=0); VCHECK(kh_wait(g,ev,40)==258);                                                       /* 40 ms of host time, 0 ms of guest time: no tick (WAIT_TIMEOUT) */
      kh_vclock_test_vblank(g); VCHECK(kh_wait(g,ev,2000)==0);                                           /* +16.7 ms: the timer's instant has passed */
      VCHECK(kh_wait(g,ev,40)==258);                                                                      /* and it waits for the clock again */
      kh_vclock_test_vblank(g); VCHECK(kh_wait(g,ev,2000)==0);
      VCHECK((u32)st_imp(g,rt_imp_winmm__timeKillEvent,id)==0); kh_vclock_test_vblank(g); { struct timespec ts={0,20000000}; nanosleep(&ts,0); } kh_wait(g,ev,0);
      kh_vclock_test_vblank(g); VCHECK(kh_wait(g,ev,40)==258);                                           /* killed: silent */
      h_close(ev); }
    /* 6. the gate: which exit releases a vblank, and how long it holds it */
    { int (*sw)(KCtx*,u64)=gate_waiting; u64 (*spr)(void)=gate_presents, (*sre)(void)=gate_releases, (*sfr)(void)=gate_frames; u64 sb=gate_busy_ns, sbr=gate_busy_run_ns, ss=gate_stall_ns, sp=gate_period_ns;
      u64 space=pace_ns, slr=last_release_host, spd=paced_ns; pace_ns=0;
      gate_waiting=st_waiting; gate_presents=st_presents; gate_releases=st_releases; gate_frames=st_frames; gate_busy_ns=60000000ull; gate_busy_run_ns=60000000ull; gate_stall_ns=250000000ull; gate_period_ns=30000000ull;
      pthread_mutex_lock(&vm); u64 n0=n_normal,r0=n_norel,b0=n_busy,s0=n_stall,m0=n_multi; presents_seen=0; ticks_seen=0; frames_seen=0; int lh=last_how; last_how=-1; pthread_mutex_unlock(&vm); atomic_store(&st_p,0); atomic_store(&st_r,0); atomic_store(&st_f,0);
      u64 k0=kh_vclock_vblanks(), h, d;
      st_w=1; atomic_store(&st_r,1); atomic_store(&st_f,1); atomic_store(&st_p,1); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;   /* release seen, game frame made and presented, game waits: at once */
      VCHECK(n_normal==n0+1&&d<20000000ull);
      h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;                                           /* no release, game waits: at once the first time */
      VCHECK(n_norel==r0+1&&d<20000000ull);
      h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;                                           /* ... and one host period the second time in a row */
      VCHECK(n_norel==r0+2&&d>=30000000ull&&d<150000000ull);
      st_w=0; h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;                                  /* no release, game thread busy: after the busy time */
      VCHECK(n_busy==b0+1&&d>=60000000ull&&d<250000000ull);
      st_w=0; atomic_store(&st_r,2); atomic_store(&st_f,2); atomic_store(&st_p,2); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;   /* release, frame and present, but the game is not back in its wait: held until the guard */
      VCHECK(n_stall==s0+1&&n_normal==n0+1&&d>=250000000ull);
      st_w=1; atomic_store(&st_r,3); atomic_store(&st_f,3); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;   /* release, game frame made, game waits, but no Present yet: held */
      VCHECK(n_stall==s0+2&&d>=250000000ull);
      atomic_store(&st_r,4); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;                    /* release, game waits again WITHOUT a game frame (30 fps mode): at once, no picture due */
      VCHECK(n_multi==m0+1&&d<20000000ull);
      atomic_store(&st_r,5); atomic_store(&st_f,4); atomic_store(&st_p,3); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h; VCHECK(n_normal==n0+2&&d<20000000ull);
      pace_ns=40000000ull;                                                                                /* pace: a release that is due at once still keeps one (here 40 ms) host period from the previous one */
      atomic_store(&st_r,6); atomic_store(&st_f,5); atomic_store(&st_p,4); kh_vclock_vblank(g,0); atomic_store(&st_r,7); atomic_store(&st_f,6); atomic_store(&st_p,5); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h;
      VCHECK(n_normal==n0+4&&d>=39000000ull&&d<200000000ull);
      pace_ns=0; atomic_store(&st_r,8); atomic_store(&st_f,7); atomic_store(&st_p,6); h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h; VCHECK(n_normal==n0+5&&d<20000000ull);    /* free: at once */
      VCHECK(kh_vclock_vblanks()==k0+11&&kh_vclock_now_ns()==T0+kh_vclock_vblanks()*1000000000ull/60ull); /* eleven more exact periods, whatever the exit */
      /* game-busy: the short time only while no frame was ever presented in step (boot); once the game has run in step, the long one */
      gate_busy_ns=20000000ull; gate_busy_run_ns=90000000ull; st_w=0;
      pthread_mutex_lock(&vm); u64 keep_normal=n_normal; n_normal=0; pthread_mutex_unlock(&vm);
      h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h; VCHECK(n_busy==b0+2&&d>=20000000ull&&d<80000000ull);
      pthread_mutex_lock(&vm); n_normal=keep_normal; pthread_mutex_unlock(&vm);
      h=kh_host_ns(); kh_vclock_vblank(g,0); d=kh_host_ns()-h; VCHECK(n_busy==b0+3&&d>=90000000ull&&d<300000000ull);
      pace_ns=space; last_release_host=slr; paced_ns=spd;
      gate_waiting=sw; gate_presents=spr; gate_releases=sre; gate_frames=sfr; gate_busy_ns=sb; gate_busy_run_ns=sbr; gate_stall_ns=ss; gate_period_ns=sp;
      pthread_mutex_lock(&vm); n_normal=n0; n_norel=r0; n_busy=b0; n_stall=s0; n_multi=m0; gate_host_ns=0; max_gate_ns=0; last_how=lh; presents_seen=gate_presents(); ticks_seen=gate_releases(); frames_seen=gate_frames(); pthread_mutex_unlock(&vm); }
    /* 7. the game-state readers: semaphore objects {waiters, pad, handle} behind the pointer at 0x140abc610 and at device+0x300, and
     *    the ReleaseSemaphore import counting releases of exactly those two handles */
    { u64 saved=rd64(g,FRAME_SEM_VA), o=ga_calloc(1,16), dev=ga_calloc(1,0x400), o2=ga_calloc(1,16), sh[4]={atomic_load(&frame_sem_h[0]),atomic_load(&frame_sem_h[1]),atomic_load(&frame_releases),0};
      wr64(g,FRAME_SEM_VA,0); VCHECK(guest_waiters(g,0)==0&&sem_handle(g,FRAME_SEM_VA)==0);
      wr64(g,FRAME_SEM_VA,o); VCHECK(guest_waiters(g,0)==0); wr32(g,o,1); VCHECK(guest_waiters(g,0)==1); wr32(g,o,0); VCHECK(guest_waiters(g,dev)==0);
      wr64(g,dev+DEV_SEM_OFF,o2); wr32(g,o2,1); VCHECK(guest_waiters(g,dev)==1&&guest_waiters(g,0)==0); wr32(g,o2,0);
      wr64(g,FRAME_SEM_VA,0x10); VCHECK(guest_waiters(g,0)==0);                                           /* garbage pointer: not waiting, no fault */
      wr64(g,FRAME_SEM_VA,o);
      u64 sp=KH_RSP(g), sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)}; u64 hs[3];
      for(int i=0;i<3;i++){ KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=0; KH_RDX(g)=0; KH_R8(g)=8; KH_R9(g)=0; rt_imp_kernel32__CreateSemaphoreA(g); hs[i]=KH_RAX(g); }
      KH_RSP(g)=sp; VCHECK(hs[0]&&hs[1]&&hs[2]);
      wr64(g,o+8,hs[0]); wr64(g,o2+8,hs[1]); VCHECK(sem_handle(g,FRAME_SEM_VA)==hs[0]&&sem_handle(g,dev+DEV_SEM_OFF)==hs[1]);
      atomic_store(&frame_sem_h[0],hs[0]); atomic_store(&frame_sem_h[1],hs[1]); atomic_store(&frame_releases,0);
      u64 st0=atomic_load(&frame_taken); long sw0=atomic_load(&frame_in_wait), sr0=atomic_load(&frame_rest); int sk0=atomic_load(&frame_rest_ok); u64 sc0=n_calib;
      atomic_store(&frame_taken,0); atomic_store(&frame_in_wait,0); atomic_store(&frame_rest,0); atomic_store(&frame_rest_ok,1);
      VCHECK(game_parked()==0);                                                                           /* nobody in a frame wait */
      for(int i=0;i<3;i++){ KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=hs[i]; KH_RDX(g)=1; KH_R8(g)=0; rt_imp_kernel32__ReleaseSemaphore(g); VCHECK((u32)KH_RAX(g)==1);
          VCHECK(atomic_load(&frame_releases)==(u64)(i<2?i+1:2)); }                                       /* the third semaphore is not a frame semaphore */
      /* the waits through the import: a wait that takes a release counts it, one that times out does not, the third handle counts nothing */
      extern void rt_imp_kernel32__WaitForSingleObject(KCtx*);
      for(int i=0;i<3;i++){ KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=hs[i]; KH_RDX(g)=0; rt_imp_kernel32__WaitForSingleObject(g); VCHECK((u32)KH_RAX(g)==0);
          VCHECK(atomic_load(&frame_taken)==(u64)(i<2?i+1:2)&&atomic_load(&frame_in_wait)==0); }
      KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=hs[0]; KH_RDX(g)=5; rt_imp_kernel32__WaitForSingleObject(g); VCHECK((u32)KH_RAX(g)==258&&atomic_load(&frame_taken)==2&&atomic_load(&frame_in_wait)==0);
      KH_RSP(g)=sp; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3];
      /* parked = in a frame wait with every release taken; a release in flight (not yet taken) is NOT parked even though the thread still sits in the wait */
      VCHECK(kh_vclock_frame_wait_enter(hs[2])==0&&game_parked()==0);
      VCHECK(kh_vclock_frame_wait_enter(hs[0])==1&&game_parked()==1);
      atomic_fetch_add(&frame_releases,1); VCHECK(game_parked()==0);                                      /* released, the waiter has not run yet */
      kh_vclock_frame_wait_leave(1); VCHECK(game_parked()==0&&atomic_load(&frame_taken)==atomic_load(&frame_releases));   /* took it, runs its frame */
      VCHECK(kh_vclock_frame_wait_enter(hs[1])==1&&game_parked()==1); kh_vclock_frame_wait_leave(0); VCHECK(game_parked()==0);
      /* calibration: an unknown resting difference (here: a wait that returned on the semaphore's initial count, never released by us)
       * is learnt from a thread that sits in the wait with empty semaphores for the quiet time, and only then */
      atomic_store(&frame_rest_ok,0); atomic_fetch_add(&frame_taken,1);                                    /* taken is now one ahead: difference -1 */
      VCHECK(kh_vclock_frame_wait_enter(hs[0])==1&&game_parked()==0);                                     /* in the wait, but nothing is calibrated */
      { u64 qs=0; long qd=0, t=1000; frame_calibrate(t,&qs,&qd); VCHECK(qs==1000&&qd==-1&&!atomic_load(&frame_rest_ok));
        frame_calibrate(t+VC_QUIET_NS-1,&qs,&qd); VCHECK(!atomic_load(&frame_rest_ok));                    /* not quiet long enough */
        atomic_fetch_add(&frame_releases,1); frame_calibrate(t+VC_QUIET_NS,&qs,&qd); VCHECK(qs==0&&!atomic_load(&frame_rest_ok));   /* the difference moved: the quiet period starts over */
        atomic_fetch_sub(&frame_releases,1);
        frame_calibrate(t,&qs,&qd); frame_calibrate(t+VC_QUIET_NS,&qs,&qd); VCHECK(atomic_load(&frame_rest_ok)&&atomic_load(&frame_rest)==-1&&game_parked()==1);
        atomic_fetch_add(&frame_releases,1); VCHECK(game_parked()==0);                                    /* a release on its way */
        kh_vclock_frame_wait_leave(1); VCHECK(game_parked()==0); VCHECK(kh_vclock_frame_wait_enter(hs[0])==1&&game_parked()==1); kh_vclock_frame_wait_leave(0);
        /* a pending count on a frame semaphore is not quiet */
        atomic_store(&frame_rest_ok,0); KH_RSP(g)=((sp-0x100)&~15ull)-8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull); KH_RCX(g)=hs[1]; KH_RDX(g)=1; KH_R8(g)=0; rt_imp_kernel32__ReleaseSemaphore(g); KH_RSP(g)=sp;
        VCHECK(frame_sems_empty()==0); VCHECK(kh_vclock_frame_wait_enter(hs[0])==1); qs=0; frame_calibrate(t,&qs,&qd); frame_calibrate(t+VC_QUIET_NS,&qs,&qd); VCHECK(!atomic_load(&frame_rest_ok));
        kh_vclock_frame_wait_leave(0); KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3]; }
      atomic_store(&frame_taken,st0); atomic_store(&frame_in_wait,sw0); atomic_store(&frame_rest,sr0); atomic_store(&frame_rest_ok,sk0); n_calib=sc0;
      for(int i=0;i<3;i++) h_close(hs[i]);
      atomic_store(&frame_sem_h[0],sh[0]); atomic_store(&frame_sem_h[1],sh[1]); atomic_store(&frame_releases,sh[2]);
      wr64(g,FRAME_SEM_VA,saved); ga_free(o); ga_free(o2); ga_free(dev); }
    kh_vclock_test_end(); ga_free(m);
    /* 9. review of task 28: a winmm timer created BEFORE the first vblank (the clock still the host's) does not tick on the host, and once
     *    the clock steps its grid starts at the origin (ticks at origin + 5, 10, 15 ms), not at the host instant of its creation */
    if(atomic_load(&stepped)) VCHECK(!"9: the clock is already stepped (KHNMC_CLOCK=frames selftest?)");
    else { extern u64 kh_timer_anchor_counts(u64*); int on0=kh_vclock_on; kh_vclock_on=1; atomic_store(&st_tn,0); u64 an0=kh_timer_anchor_counts(NULL);
      u64 sp=KH_RSP(g), sv[4]={KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)};
      KH_RSP(g)=((sp-0x100)&~15ull); wr64(g,KH_RSP(g)+0x20,0x01); KH_RSP(g)-=8; wr64(g,KH_RSP(g),0x7ff7ffff0000ull);                 /* fuEvent = TIME_PERIODIC | TIME_CALLBACK_FUNCTION */
      KH_RCX(g)=5; KH_RDX(g)=1; KH_R8(g)=kh_host_fn("vclock_selftest_anchor_cb",st_anchor_cb); KH_R9(g)=0; rt_imp_winmm__timeSetEvent(g); u32 id=(u32)KH_RAX(g);
      KH_RSP(g)=sp; KH_RCX(g)=sv[0]; KH_RDX(g)=sv[1]; KH_R8(g)=sv[2]; KH_R9(g)=sv[3]; VCHECK(id!=0);
      { struct timespec ts={0,40000000}; nanosleep(&ts,0); } VCHECK(atomic_load(&st_tn)==0);                  /* 40 ms of host time before the first vblank: no tick */
      const u64 T1=7000000000000ull; kh_vclock_test_begin(T1);
      for(int i=0;i<3000&&kh_timer_anchor_counts(NULL)==an0;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      VCHECK(kh_timer_anchor_counts(NULL)==an0+1);                                                          /* registered at the origin */
      kh_vclock_test_vblank(g);
      VCHECK(atomic_load(&st_tn)==3&&st_tat[0]==T1+5000000ull&&st_tat[1]==T1+10000000ull&&st_tat[2]==T1+15000000ull);
      if(atomic_load(&st_tn)!=3||st_tat[0]!=T1+5000000ull) fprintf(stderr,"vclock selftest: anchored timer: %d ticks, first at origin %+lld ns\n",atomic_load(&st_tn),(long long)(st_tat[0]-T1));
      VCHECK((u32)st_imp(g,rt_imp_winmm__timeKillEvent,id)==0); kh_vclock_test_vblank(g); { struct timespec ts={0,20000000}; nanosleep(&ts,0); }
      kh_vclock_test_end(); kh_vclock_on=on0; }
    /* 8. KHNMC_CLOCK values */
    if(!kh_fork_ok()) kh_selftest_not_run("vclock","KHNMC_CLOCK=bogus and frames with KHNMC_CLOCK_PACE=warp (named fatals at start-up)",2);   /* iOS: no fork (host_env.c) */
    else { pid_t pid=kh_fork(); if(pid==0){ setenv("KHNMC_CLOCK","bogus",1); if(!freopen("/dev/null","w",stderr)){} kh_vclock_init(); _exit(0); }
      int stt=0; VCHECK(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)!=0);             /* unknown mode: named fatal, not a silent host clock */
      pid=kh_fork(); if(pid==0){ setenv("KHNMC_CLOCK","frames",1); setenv("KHNMC_CLOCK_PACE","warp",1); if(!freopen("/dev/null","w",stderr)){} kh_vclock_init(); _exit(0); }
      stt=0; VCHECK(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)!=0); }
    fprintf(stderr,"kh: vclock selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}
