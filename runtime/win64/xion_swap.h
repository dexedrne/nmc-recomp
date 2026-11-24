#pragma once
#include "khnmc_rt.h"
#include <stdatomic.h>
extern _Atomic int kh_xion_enabled;
extern _Atomic int kh_axel_enabled;   /* the "Axel Won't Cry" campaign: Critical rules, the Axel look as the mode default, no actor swap */
/* 0 none, 1 a Xion mode, 2 Axel Won't Cry (KH_MODE_* in kh_look.h) */
static inline int kh_campaign_mode(void){ return kh_axel_enabled?2:kh_xion_enabled?1:0; }
extern int kh_xkb_menu;
void kh_xkb_set_mode(KCtx *g,int mode);
u64 kh_xkb_message(int id);
void kh_xkb_load(KCtx *g);
void kh_xkb_serialized(void);
void kh_xion_reset(KCtx *g);
void kh_xion_init(void);
void kh_xion_command_done(KCtx *g,u64 controller,u64 actor);
void kh_xion_frame(KCtx *g,u16 buttons);
u64 kh_xion_progress_player(void);
u64 kh_xion_recovery_sheet(KCtx *g,u64 sheet);
void kh_xion_save_portrait(KCtx *g,u64 buffer);
void kh_save_difficulty_init(void);
u64 kh_encode_menu_text(const char *src,int compact);
/* A save tag is the saved difficulty of a campaign-mode save (2, 3; 0 = none) plus this bit for Axel Won't Cry (always with 2). */
enum { KH_SAVE_TAG_AXEL=4 };
