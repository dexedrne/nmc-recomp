#pragma once
#include "khnmc_rt.h"
void kh_autosave_init(void);
void kh_autosave_load(KCtx *g);
u16 kh_input_buttons(void);
const char *kh_autosave_documents(void);
int kh_autosave_indicator(void);
u32 kh_save_crc(const u8 *payload);
int kh_autosave_shortcut(void);
int kh_autosave_preview(KCtx *g,int shortcut,u32 *level,int *tag);
