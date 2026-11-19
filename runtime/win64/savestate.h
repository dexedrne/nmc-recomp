#pragma once
#include "khnmc_rt.h"

extern int kh_cut_enabled;
extern _Thread_local unsigned kh_cut_callback_depth;
extern _Atomic int kh_cut_requested;
void kh_cut_rebuild_fibers(void);
void kh_cut_game_loop(KCtx *g,KhFn entry);
void kh_game_frame_step(KCtx *g);
u64 kh_cut_elapsed_ns(void);
u64 kh_cut_unix_ns(void);
void kh_cut_reserve_boot_object(void);
void kh_cut_init(void);
void kh_cut_frame(KCtx *g,u64 frame);
void kh_cut_poll(const char *site,pthread_mutex_t *held);
void kh_cut_wait(const char *site,pthread_cond_t *cv,pthread_mutex_t *m);
int kh_cut_threads(KThread **parked,unsigned count,int report);
int kh_cut_gate_begin(u64 *epoch);
int kh_cut_gate_end(u64 epoch);
int kh_cut_loader_safe(void);
