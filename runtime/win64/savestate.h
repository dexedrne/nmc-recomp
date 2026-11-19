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
int kh_cut_gate_end(u64 epoch);
int kh_cut_loader_safe(void);

enum KhCutWait { KH_CUT_SLEEP=1, KH_CUT_HANDLE, KH_CUT_RELEASE, KH_CUT_SHELL, KH_CUT_TIMER, KH_CUT_MIXER };
void kh_cut_worker_loop(KCtx *g,u64 target);
void kh_cut_transfer_wait(KCtx *g,enum KhCutWait kind,KhFn body);
extern _Thread_local enum KhCutWait kh_cut_wait_owner;
void kh_cut_loader_relock(KCtx *g);
u64 kh_vclock_timer_wait(int id,u64 due);
