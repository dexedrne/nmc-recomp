#pragma once
#include "khnmc_rt.h"

/* Call at a quiescent player reload boundary, before requesting the new resources. */
void kh_look_set(KCtx *g,int xion);
/* Zero for stock, -1 for an embedded-only model, face slot + 1 for an APDX or its DDS. */
enum { KH_SCENE_MODEL = -2, KH_SCENE_TEXTURES = 3 };
#define KH_SCENE_MDLX "obj/H_EX500.mdlx"
void kh_scene_pack_begin(void);
void kh_scene_pack_end(void);
int kh_scene_pack_active(void);
int kh_scene_key(const char *rel);
const char *kh_scene_root(void);
const char *kh_look_root(void);
int kh_scene_path(const char *rel,char *host,size_t cap);
enum { KH_FILE_LOOK = 2, KH_FILE_RMC = 3 }; /* A selected overlay file must open successfully. */
int kh_look_enabled(void);
int kh_look_asset(int look,const char *rel,char *host,size_t cap);
int kh_look_pack_ready(int look);
int kh_scene_pack_ready(void);
int kh_player_asset_key(const char *canonical);
int kh_player_voice(const char *rel);
void kh_look_voice_request(KCtx *g);
void kh_look_voice_bind(KCtx *g);
void kh_look_voice_retry(KCtx *g);   /* a voice bank that was not ready at a switch: retried each frame for ~10 s */
int kh_look_key(const char *rel);
int kh_look_available(const char *rel,char *host,size_t cap);
void kh_look_path(KCtx *g,const char *rel,char *host,size_t cap);

int kh_look_message_id(KCtx *g,u64 text);
u64 kh_look_name(KCtx *g,int id,u64 original);
