#pragma once
#include "khnmc_rt.h"

extern int kh_cut_enabled;
extern _Thread_local unsigned kh_cut_callback_depth;
extern _Atomic int kh_cut_requested;
void kh_cut_rebuild_fibers(void);
void kh_cut_stream_steps(KCtx *g,int pc);
int kh_cut_on_stream(void);
void kh_cut_stream_loop(KCtx *g);
void kh_cut_game_loop(KCtx *g,KhFn entry);
void kh_game_frame_step(KCtx *g);
u64 kh_cut_elapsed_ns(void);
u64 kh_cut_unix_ns(void);
void kh_cut_reserve_boot_object(void);
void kh_cut_init(void);
void kh_cut_frame(KCtx *g,u64 frame);
void kh_cut_poll(const char *site,pthread_mutex_t *held);
void kh_cut_wait(const char *site,pthread_cond_t *cv,pthread_mutex_t *m);
int kh_cut_threads(KThread **parked,unsigned count,int report,unsigned *cs_waiter);
int kh_cut_gate_begin(u64 *epoch);
int kh_cut_gate_commit(u64 epoch);
void kh_cut_gate_committed_end(void);
int kh_cut_gate_end(u64 epoch);
int kh_cut_loader_safe(void);

enum KhCutWait { KH_CUT_SLEEP=1, KH_CUT_HANDLE, KH_CUT_RELEASE, KH_CUT_SHELL, KH_CUT_TIMER, KH_CUT_MIXER };
void kh_cut_worker_loop(KCtx *g,u64 target);
void kh_cut_transfer_wait(KCtx *g,enum KhCutWait kind,KhFn body);
extern _Thread_local enum KhCutWait kh_cut_wait_owner;
void kh_cut_loader_relock(KCtx *g);
u64 kh_vclock_timer_wait(int id,u64 due);

/* In-process records. The barrier owns the live wait frames until thaw. */
struct KhPendingWait {
    KWaitObj *object;
    u64 handle, generation;
    int blocked, handoff, frame_wait, deadline_client;
    u32 timeout_ms;
    u64 deadline;
};
extern _Thread_local struct KhPendingWait *kh_cut_pending;
struct KhSyncSlot;
struct KhSyncSlot *kh_sync_capture(struct KhPendingWait **waits,KThread **threads,unsigned n);
const char *kh_sync_restore(struct KhSyncSlot *,struct KhPendingWait **waits,KThread **threads,unsigned n);
void kh_sync_discard(struct KhSyncSlot *);

struct KhTimerSlot;
struct KhTimerSlot *kh_timer_capture(void);
const char *kh_timer_validate(struct KhTimerSlot *);
void kh_timer_restore(struct KhTimerSlot *);
u64 kh_clock_client_deadline(int id);
struct KhCsSlot;
struct KhCsSlot *kh_cs_capture(void);
const char *kh_cs_validate(struct KhCsSlot *);
void kh_cs_discard(struct KhCsSlot *);
void kh_cs_restore(struct KhCsSlot *);
struct KhClockSlot;
struct KhClockSlot *kh_clock_capture(void);
const char *kh_clock_validate(struct KhClockSlot *);
void kh_clock_restore(struct KhClockSlot *);
u64 kh_cut_mix_wait(int client,u64 due);
unsigned kh_cut_audio_graph(unsigned *buffers,unsigned *commands);
struct KhAudioSlot;
struct KhAudioSlot *kh_audio_capture(void);
void kh_audio_restore(struct KhAudioSlot *);
void kh_audio_discard(struct KhAudioSlot *);
struct KhStreamSlot;
struct KhStreamSlot *kh_stream_capture(void);
void kh_stream_restore(struct KhStreamSlot *);
void kh_stream_discard(struct KhStreamSlot *);
