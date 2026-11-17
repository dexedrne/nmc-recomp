/* Effective shipped INI values. "begginer_*" is ignored by the DLL. */
#pragma once
#include "khnmc_rt.h"
static const struct kh_mod_difficulty_config {
    s32 enemy_damage, party_damage;
    float ap, hp, mp;
} kh_mod_difficulty_config[4] = {
    {50,150,1.0f,1.0f,1.0f},
    {100,100,1.0f,1.0f,1.0f},
    {200,125,1.5f,0.5f,0.5f},
    {200,125,1.5f,0.5f,0.5f},
};

/* GAME_MODE descriptor, not a DLL option. */
#define KH_MOD_GUMMI_TASK 0x716c10u

void kh_mod_hooks_a_dump(void);
int kh_mod_hooks_a_selftest(KCtx *g);

/* ---- Task 11: DLL 0x2df50 -> exe 0x40d810 (range-parameter dispatcher, kinds 54..83) and DLL 0x2ec30 -> exe 0x40dd70
 * (frame-op dispatcher, ops 50..80).  Implemented in mod_params.c; mod_params_test.c holds the selftest.
 * The call sink lets a test replace every outgoing guest call and the forward to the original by a recorded, canonicalised
 * entry (the native DLL oracle in tools/native_params_oracle.c records the same tuples); NULL in normal runs. */
#include "mod_params_call.h"
struct kh_mp_mock {
    int ncalls, overflow;
    struct kh_mp_call c[KH_MP_MAXCALLS];
    u64 (*fake)(void *user, const struct kh_mp_call *call);   /* value returned to the handler for a recorded call */
    void *user;
};
extern struct kh_mp_mock *kh_mp_mock_sink;
void kh_mod_params_dump(void);
int kh_mod_params_selftest(KCtx *g);
/* The DLL's two std::unordered_map<actor*,int> objects (kinds 64 and 65): test access and reset. */
size_t kh_mod_params_map_size(int which);                     /* 0 = minimum HP (64), 1 = resistance byte (65) */
int kh_mod_params_map_get(int which, u64 actor, s32 *saved);  /* 1 if present */
void kh_mod_params_map_clear(void);
