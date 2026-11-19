#pragma once
#include "khnmc_rt.h"
extern int kh_xion_enabled, kh_xkb_menu;
u64 kh_xkb_message(int id);
void kh_xkb_load(KCtx *g);
void kh_xkb_serialized(void);
void kh_xion_reset(KCtx *g);
void kh_xion_init(void);
void kh_xion_command_done(KCtx *g,u64 controller,u64 actor);
void kh_xion_frame(KCtx *g,u16 buttons);
u64 kh_xion_progress_player(void);
u64 kh_xion_recovery_sheet(KCtx *g,u64 sheet);
