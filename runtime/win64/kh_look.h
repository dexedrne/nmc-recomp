#pragma once
#include "khnmc_rt.h"

/* The player appearances.  Each look past Roxas is an optional pack; an absent pack is never offered.
 * (Savestate records store these numbers: keep the first three as they are.) */
enum { KH_LOOK_ROXAS, KH_LOOK_XION, KH_LOOK_HOODED, KH_LOOK_AXEL, KH_LOOK_ROXAS_HD, KH_LOOKS };
/* The campaign modes of kh_xkb_set_mode: 0 an ordinary campaign, 1 Xion Knows Best / Xion Always Cries, 2 Axel Won't Cry. */
enum { KH_MODE_NONE, KH_MODE_XION, KH_MODE_AXEL };
/* Call at a quiescent player reload boundary, before requesting the new resources. */
void kh_look_set(KCtx *g,int look);
void kh_select_saved_look(KCtx *g,int look);
void kh_restore_saved_look(KCtx *g);
/* 0 Roxas's own battle voice, 1 Xion's (both costumes share it), 2 Axel's. */
static inline int kh_look_voice_set(int look){ return look==KH_LOOK_AXEL?2:look==KH_LOOK_XION || look==KH_LOOK_HOODED; }
static inline int kh_look_is_xion(int look){ return look==KH_LOOK_XION || look==KH_LOOK_HOODED; }
/* The number of looks the pickers cycle through: Roxas, Xion, Xion (hooded), then Axel only while its pack is complete. */
int kh_look_count(void);
/* Zero for stock, -1 for an embedded-only model, face slot + 1 for an APDX or its DDS.  KH_LOOK_STOCK_HD: the pack's motion file
 * with the stock HD table (a pack whose face sheet is only at a form's legacy slot, hd_assets.c keys[]). */
enum { KH_SCENE_MODEL = -2, KH_SCENE_TEXTURES = 3, KH_LOOK_STOCK_HD = -4, KH_LOOK_WEAPON = -5, KH_LOOK_COMPANION = -6 };
/* lane HM: NMC's Holo-Mission companion Xion (obj/N_PO010_BTL.mdlx and obj/N_PO010_RTN.mdlx, one model on her own 62-bone rig)
 * wears the Xion pack's HD body whenever that pack holds <name>.mdlx and remastered/obj/<name>.mdlx/-0.dds, whatever the player's
 * look or campaign mode: the companion and the playable Xion match.  Her motions (.mset), voice/effects (.a.us), AI program and
 * collision (inside the BAR, kept by the pack file) and her keyblade object N_PO010_SIT_RTN stay NMC's.  1 when rel (canonical:
 * the .mdlx or a leaf of its remastered folder) is served from the Xion pack (KH_LOOK_COMPANION in kh_look_key). */
int kh_companion_asset(const char *rel,char *host,size_t cap);
/* selftest probe (hd_assets.c): the HD-table key and costume a file would get now */
int kh_hd_look_probe(const char *rel,int *costume);
/* The chakram set of the Axel look (file_layer.c).  kh_loading_weapon: set while the settings layer reads a chakram model; only
 * then does a W_EX010*.mdlx request map to the set (KH_LOOK_WEAPON).  The game's own reads always get the keyblades. */
extern _Thread_local int kh_loading_weapon;
int kh_axel_weapon_asset(const char *rel,char *host,size_t cap);
int kh_axel_weapon_ready(void);
int kh_axel_weapon_chakrams(void);
#define KH_SCENE_MDLX "obj/H_EX500.mdlx"
void kh_scene_pack_begin(void);
void kh_scene_pack_end(void);
int kh_scene_pack_active(void);
int kh_scene_key(const char *rel);
/* lane CSI: high Sora in a Drive form inside an event (obj/H_EX500_{BTLF,MAGF,KH1F,TRIF,ULTF}.mdlx, the same 284-bone rig as
 * H_EX500, only member 7 differs) shows the look's H_EX500.  1 when rel (a canonical tree path: the .mdlx or a leaf of its
 * remastered folder) is one of them; with out, the H_EX500 equivalent is written there. */
int kh_scene_form_alias(const char *rel,char *out,size_t cap);
/* lane CSI (gated by KHNMC_SCENE_LOOK, default 0): an event that casts high Sora (572 / 264 / 200) opens the scene pack when a
 * look is on with KHNMC_SCENE_LOOK=1 (any campaign: the look's H_EX500), or in the Xion / Axel modes / KHNMC_SCENE_ROXAS=1 (the
 * scene-roxas overlay; events_stub.c). */
int kh_event_high_scene(KCtx *g);
/* lane scene-off: KHNMC_SCENE_LOOK=0 (default) keeps the look out of the story scenes (Roxas plays); round 9 item 0: in every
 * campaign mode (ordinary, Xion Always Cries / Xion Knows Best, Axel Won't Cry).  Only KHNMC_SCENE_LOOK=1 lets it follow.
 * kh_scene_wear: the look the scenes wear (kh_look_enabled() with the setting, else 0 = Roxas). */
int kh_scene_look(void);
int kh_scene_wear(void);
const char *kh_scene_root(void);
const char *kh_look_root(void);
int kh_scene_path(const char *rel,char *host,size_t cap);
enum { KH_FILE_LOOK = 2, KH_FILE_RMC = 3 }; /* A selected overlay file must open successfully. */
int kh_look_enabled(void);
int kh_look_asset(int look,const char *rel,char *host,size_t cap);
int kh_look_pack_ready(int look);
/* The look's pack holds the HUD face sheet of this player stem (at its slot or its legacy slot). */
int kh_look_face_ready(int look,const char *stem);
int kh_look_switch_ready(int target);
int kh_scene_pack_ready(void);
int kh_player_asset_key(const char *canonical);
int kh_player_voice(const char *rel);
void kh_look_voice_request(KCtx *g);
void kh_look_voice_bind(KCtx *g);
int kh_look_voice_retry(KCtx *g);
extern _Thread_local int kh_loading_look;
int kh_look_key(const char *rel);
int kh_look_available(const char *rel,char *host,size_t cap);
void kh_look_path(KCtx *g,const char *rel,char *host,size_t cap);

int kh_look_message_id(KCtx *g,u64 text);
u64 kh_save_character_text(unsigned id);
u64 kh_look_name(KCtx *g,int id,u64 original);
