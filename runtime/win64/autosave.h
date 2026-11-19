#pragma once
#include "khnmc_rt.h"
void kh_autosave_init(void);
void kh_autosave_load(KCtx *g);
u16 kh_input_buttons(void);
const char *kh_autosave_documents(void);
