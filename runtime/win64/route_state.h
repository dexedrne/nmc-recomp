/* route_state.h : the game states a scripted route can wait for (Task 27, extended by Task 29).  The readers are in
 * runtime/route_state.c, the script language that uses them (`@wait`, `@press ... until`, `@select`, `@expect`, `@goto`) in
 * runtime/null_input.c, the documentation in input/README.md. */
#pragma once
#include "khnmc_rt.h"
enum { KR_INITIAL_STATE, KR_TITLE_SCREEN, KR_TITLE_MENU, KR_TITLE_CHOICE, KR_NEWGAME_DIALOG, KR_NEWGAME_CHOICE, KR_GAME_LEVEL,
       KR_FIELD, KR_BATTLE, KR_HITS, KR_MESSAGE, KR_FRAMES,
       /* task 29: the player's status sheet, the field command menu, the map, the system menu, the item handler */
       KR_HP_BELOW_MAX, KR_MP_BELOW_MAX, KR_HP_AT_MOST, KR_MP_AT_MOST, KR_COMMAND_DEPTH, KR_COMMAND_ROW, KR_COMMAND_ID, KR_COMMAND_HAS,
       KR_WORLD, KR_AREA, KR_MENU_OPEN, KR_MISSION, KR_ENEMIES_AT_MOST, KR_ITEM_USES, KR_ITEM_DRAIN, KR_ITEM_HEAL,
       /* task 30: the place block of 0x152be0 (door and the area scripts' programs) */
       KR_DOOR, KR_MAP_PROGRAM, KR_BTL_PROGRAM, KR_EVT_PROGRAM,
       /* task 30: the pause screen (system menu 50: a mission's or an event's; rows 6 = continue, 5 = skip the event) */
       KR_PAUSE_MENU, KR_PAUSE_ID, KR_PAUSE_HAS,
       KR_NCOND };
typedef struct { const char *name; int has_arg; int lo, hi; const char *what; } KhRouteCondInfo;
extern const KhRouteCondInfo kh_route_cond_info[KR_NCOND];
int  kh_route_cond_lookup(const char *name);                       /* index into kh_route_cond_info, -1 = unknown */
/* What a step remembers from its first frame: conditions that count (`frames N`, `hits N`, `item-uses N`, `item-drain`,
 * `item-heal`) count from there. */
typedef struct { u64 frame, hits, items; } KhRouteBase;
/* Evaluated on the game thread at the end of game frame n (the 0x105e10 hook). */
int  kh_route_cond_eval(KCtx *g,int kind,int arg,u64 n,const KhRouteBase *base);
void kh_route_base_now(u64 n,KhRouteBase *out);                    /* the baseline a step takes when it starts */
/* `@select`: the cursor it moves (title-choice, newgame-choice, command-row, command-id: the row index under the cursor, -1 = not up)
 * and the row it must reach for argument `arg` (command-id: the row of the top command list that holds command `arg`, -1 = none). */
int  kh_route_cursor(KCtx *g,int kind);
int  kh_route_cursor_target(KCtx *g,int kind,int arg);
void kh_route_state_tick(KCtx *g,u64 n);                           /* once per game frame, before any condition: per-frame probes, trace */
void kh_route_state_summary(KCtx *g,u64 n,char *out,size_t cap);   /* every state at once (time-out fatal, report) */
#define KR_BATTLE_RECENT 300                                       /* `battle`: a hit computed within this many game frames */

/* ---- task 29 readers (all read-only; every pointer is checked before it is followed) ---------------------------------------- */
typedef struct { int ok; u64 obj, sheet; s32 hp, hp_max, mp, mp_max, chr; } KhPlayerSheet;
int  kh_route_player_sheet(KCtx *g,KhPlayerSheet *out);            /* the field player's status sheet; 0 when there is none */
int  kh_route_player_pos(KCtx *g,float *x,float *y,float *z,float *yaw);   /* world position and facing (radians); 0 = none */
int  kh_route_camera_yaw(KCtx *g,float *yaw);                      /* horizontal direction the field camera looks along; 0 = none */
/* the field command menu: depth of its menu stack (0 = no menu), the top list's type, row count, cursor, command under it */
typedef struct { int depth, type, rows, cursor, cmd_under; u16 cmd[8]; u8 state[8]; } KhCommandMenu;
int  kh_route_command_menu(KCtx *g,KhCommandMenu *out);
int  kh_route_world(KCtx *g);                                      /* [0x140717008], -1 when 0xff (no map) */
int  kh_route_programs(KCtx *g,int *door,int *map,int *btl,int *evt);   /* task 30: door and the map / btl / evt programs now running (0x152be0); 0 when no map */
/* task 30: the pause screen (menu 50): its rows' ids (0xffff ends the list) and the cursor; 0 when it does not take input */
typedef struct { int rows, cursor; u16 id[12]; } KhPauseMenu;
int  kh_route_pause_menu(KCtx *g,KhPauseMenu *out);
int  kh_route_area(KCtx *g);                                       /* [0x140717009], -1 when 0xff */
u64  kh_route_mission(KCtx *g);                                    /* the running mission's record (0x3ad500), 0 = none */
/* enemy objects in the object list (YS::ZAKO, ENEMY, BOSS, LARGEZAKO, LARGEBOSS) and the nearest one to (px,pz); -1 = no list */
int  kh_route_enemies(KCtx *g,float px,float pz,float *nx,float *nz,float *nd);
/* `@goto`: the stick that walks toward (tx,tz) from (x,z) with the camera looking along cam_yaw (left stick, XInput units) */
void kh_route_goto_stick(float x,float z,float tx,float tz,float cam_yaw,float scale,s16 *lx,s16 *ly);
void kh_route_map_report(KCtx *g,FILE *f,const char *what);        /* id table, arena, RSS, files, threads: one block */
