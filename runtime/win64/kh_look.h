#pragma once
#include "khnmc_rt.h"

/* Call at a quiescent player reload boundary, before requesting the new resources. */
void kh_look_set(int xion);
/* Zero for stock, -1 for an embedded-only model, face slot + 1 for an APDX or its DDS. */
enum { KH_SCENE_MODEL = -2, KH_SCENE_TEXTURES = 3 };
#define KH_SCENE_MDLX "obj/H_EX500.mdlx"
void kh_scene_pack_begin(void);
void kh_scene_pack_end(void);
int kh_scene_pack_active(void);
int kh_scene_key(const char *rel);
int kh_scene_path(const char *rel,char *host,size_t cap);
enum { KH_FILE_LOOK = 2 }; /* A selected overlay file must open successfully. */
int kh_look_enabled(void);
int kh_look_key(const char *rel);
void kh_look_path(KCtx *g,const char *rel,char *host,size_t cap);
