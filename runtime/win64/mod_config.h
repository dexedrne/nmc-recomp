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

/* Task 9 batch A (runtime/mod_hooks_a.c).  Every guest helper the DLL calls through a signature-scanned slot is reached through
 * this table so the selftest can redirect it to a recorder; the forwards to the original functions are redirectable the same way. */
enum kh_mod_a_ext {
    KHA_ENCODE,        /* 4ad240  object pointer -> id (0 for null)                       DLL slot 826a8 */
    KHA_DECODE,        /* 4ad270  id -> object pointer (0 for 0)                          DLL slot 826b0 */
    KHA_REF_ACTOR,     /* 3eafc0  (a,b) -> object pointer                                 DLL slot 820a0 */
    KHA_SCRIPT_RUN,    /* 3e1840  (script,trigger,args,count,0)                           DLL slot 829e0 */
    KHA_SCRIPT_NEW,    /* 3e1730  (script data,0) -> script object                        DLL slot 829c0 */
    KHA_SCRIPT_RESET,  /* 3e1410                                                          DLL slot 829d0 */
    KHA_SCRIPT_FREE,   /* 3e1510                                                          DLL slot 829d8 */
    KHA_ACTOR_PRE,     /* 3b4f80                                                          DLL slot 824f8 */
    KHA_SAVE,          /* 152380  game state block                                        DLL slot 81a70 */
    KHA_SUMMON,        /* 3ddf00  (position ref, xmm1 float)                              DLL slot 82948 */
    KHA_GLOBALS,       /* 3b3e80  -> object whose +dc8 holds the vector pointer           DLL slot 82530 */
    KHA_VEC_PREP,      /* 3bde30                                                          DLL slot 82978 */
    KHA_FIND,          /* 3d74e0  (actor,id) -> entry                                     DLL slot 825d8 */
    KHA_APPLY,         /* 3d78d0  (actor,entry,vec)                                       DLL slot 825c8 */
    KHA_GATE,          /* 3ba9a0  (actor) -> AL                                           DLL slot 82500 */
    KHA_MOTION,        /* 409b00  (xmm0 double, actor in rdx)                             DLL slot 82000 */
    KHA_OP_A,          /* 3c6dc0  (this,int,xmm2,xmm3)                                    DLL slot 824d0 */
    KHA_OP_B,          /* 3c8310  (this,int,xmm2,xmm3,int)                                DLL slot 824e0 */
    KHA_SHOP_ACT,      /* 3b5fa0  (actor,actor,int,0)                                     DLL slot 82520 */
    KHA_SHOP_ARG,      /* 1518e0  (int)                                                   DLL slot 81a88 */
    KHA_ABILITY_INFO,  /* 3e39a0  (id) -> table entry                                     DLL slot 82358 */
    KHA_ABILITY_ADD,   /* 401730  (list,id)                                               DLL slot 81fe0 */
    KHA_SHOP_SEL,      /* 3e3180  (id) -> selector                                        DLL slot 82470 */
    KHA_NAME_ENTRY,    /* 3dfeb0  (selector) -> entry (+8: 32 name bytes)                 DLL slot 82560 */
    KHA_N
};
enum kh_mod_a_fwd { KHF_405440, KHF_3A89A0, KHF_3D61D0, KHF_3D60C0, KHF_3A7CB0, KHF_N };
void kh_mod_a_set_target(int ext,u64 target);      /* test only: redirect a helper (0 restores the guest function) */
void kh_mod_a_set_fwd(int fwd,KhFn fn);            /* test only: redirect a forward to the original (NULL restores) */
int kh_mod_a_counts(int hook,u64 *calls,u64 *extensions);   /* hook 0..8 in dump order; 0 when out of range */

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

/* ---- Task 16: DLL 0x31260 kind 0 (the stock item path, DLL 0x3143b..0x31618) and the fall-through of DLL 0x30f60, both in
 * runtime/mod_difficulty.c; runtime/mod_difficulty_items_test.c holds the selftest and runtime/mod_item_cases.h the reference
 * arithmetic shared with tools/native_item_oracle.c.
 * The sink replaces every outgoing guest call of the apply path by a recorded tuple (arguments the DLL leaves unset are 0);
 * NULL in normal runs. */
#define KH_ITEM_MAXCALLS 12
struct kh_item_call { u32 rva; u64 a[4]; };
struct kh_item_mock {
    int ncalls, overflow;
    struct kh_item_call c[KH_ITEM_MAXCALLS];
    u64 (*fake)(void *user, const struct kh_item_call *call);   /* value returned to the handler for a recorded call */
    void *user;
};
extern struct kh_item_mock *kh_item_mock_sink;
enum { KH_ITEM_SLOTS = 15 };                                    /* ids 1..7, 0x83, 0x112..0x117, any other id */
struct kh_item_counts {
    u64 apply[4][KH_ITEM_SLOTS];      /* kind-0 applications per difficulty and id slot (one per target of a party-wide item) */
    u64 reversed[4];                  /* of those, ids 1..7 whose command Reserve byte was 2 */
    u64 noop[4], usable[4];           /* DLL no-op apply kinds (25..29, >31); usability fall-throughs to the original */
    u64 hp_calls, mp_calls;           /* calls of exe 0x3c0c30 / 0x3c0cd0 made by the kind-0 path */
    u64 cmd2f_writes, cmd2f_two, cmd2f_zero, cmd2f_other;   /* WriteByte stores that hit +0x2f of a cmd record, by value */
};
void kh_mod_items_counts(struct kh_item_counts *out);
void kh_mod_items_reset(void);
void kh_mod_item_writebyte(KCtx *g, u64 p, u8 value);          /* observer called by the WriteByte syscall before it stores */
void kh_mod_items_dump(void);
int kh_mod_items_selftest(KCtx *g, u64 sheet);                 /* runs inside the caps fixture (character-2 sheet registry) */
u64 kh_mod_items_test_cmds(KCtx *g);                           /* synthetic sorted cmd table for exe 0x3b68b0 (test only) */
