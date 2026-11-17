/* route_state.h : the game states a scripted route can wait for (Task 27).  The readers are in runtime/route_state.c, the script
 * language that uses them (`@wait`, `@press ... until`, `@select`, `@expect`) in runtime/null_input.c, the documentation in
 * input/README.md. */
#pragma once
#include "khnmc_rt.h"
enum { KR_INITIAL_STATE, KR_TITLE_SCREEN, KR_TITLE_MENU, KR_TITLE_CHOICE, KR_NEWGAME_DIALOG, KR_NEWGAME_CHOICE, KR_GAME_LEVEL,
       KR_FIELD, KR_BATTLE, KR_HITS, KR_MESSAGE, KR_FRAMES, KR_NCOND };
typedef struct { const char *name; int has_arg; int lo, hi; const char *what; } KhRouteCondInfo;
extern const KhRouteCondInfo kh_route_cond_info[KR_NCOND];
int  kh_route_cond_lookup(const char *name);                       /* index into kh_route_cond_info, -1 = unknown */
/* Evaluated on the game thread at the end of game frame n (the 0x105e10 hook).  step_frame / step_hits: game frame and hit count at
 * the start of the step that asks (the conditions `frames N` and `hits N` count from there). */
int  kh_route_cond_eval(KCtx *g,int kind,int arg,u64 n,u64 step_frame,u64 step_hits);
int  kh_route_cursor(KCtx *g,int kind);                            /* the cursor a `@select` moves (title-choice, newgame-choice); -1 = not up */
void kh_route_state_tick(KCtx *g,u64 n);                           /* once per game frame, before any condition: per-frame probes, trace */
void kh_route_state_summary(KCtx *g,u64 n,char *out,size_t cap);   /* every state at once (time-out fatal, report) */
#define KR_BATTLE_RECENT 300                                       /* `battle`: a hit computed within this many game frames */
