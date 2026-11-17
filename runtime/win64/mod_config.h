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
