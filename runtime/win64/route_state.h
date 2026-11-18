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
       /* task 31: the running mission and what it tells the player (runtime/route_objective.c), the reaction command offered */
       KR_MISSION_ID, KR_OBJECTIVE, KR_INFORMATION, KR_REACTION, KR_REACTION_ID,
       /* a reaction command on a named object, the lock-on, a choice window's cursor, the system menu that runs */
       KR_REACTION_ON, KR_LOCK_ON, KR_MESSAGE_CHOICE, KR_MENU_ID, KR_MISSION_COUNT1, KR_MISSION_COUNT2, KR_MENU_LOCKED, KR_ITEM_SLOT, KR_MENU_PAGE,
       /* a message the running menu looked up on its current page (its rows' labels, its help texts) */
       KR_MENU_TEXT,
       /* the save's progress flags (story) and menu flags (which rows the camp menu shows) */
       KR_PROGRESS_FLAG, KR_MENU_FLAG,
       KR_NCOND };
/* has_arg: 0 none, 1 a number lo..hi, 2 a name (an object's model name; `arg` is then its index, kh_route_name_intern) */
typedef struct { const char *name; int has_arg; int lo, hi; const char *what; } KhRouteCondInfo;
int kh_route_name_intern(const char *s);                           /* -1: too long (39 characters at most) or the table (32 names) is full */
const char *kh_route_name(int index);
extern const KhRouteCondInfo kh_route_cond_info[KR_NCOND];
int  kh_route_cond_lookup(const char *name);                       /* index into kh_route_cond_info, -1 = unknown */
/* What a step remembers from its first frame: conditions that count (`frames N`, `hits N`, `item-uses N`, `item-drain`,
 * `item-heal`) count from there. */
typedef struct { u64 frame, hits, items; } KhRouteBase;
/* Evaluated on the game thread at the end of game frame n (the 0x105e10 hook).  1 = the state holds, 0 = it does not, KR_UNREADABLE =
 * the reader cannot read the state now (review of task 31: the player's sheet for hp-* / mp-*, the command menu for command-row / -id /
 * -has, the item slots for item-slot): then NEITHER the condition NOR its negation holds; an `@expect` on it is the named fatal
 * route-condition-unreadable (before, every reader failure was 0 and `@expect not X` passed when X could not be read). */
#define KR_UNREADABLE (-1)
int  kh_route_cond_eval(KCtx *g,int kind,int arg,u64 n,const KhRouteBase *base);
void kh_route_base_now(u64 n,KhRouteBase *out);                    /* the baseline a step takes when it starts */
/* `@select`: the cursor it moves (title-choice, newgame-choice, command-row, command-id: the row index under the cursor, -1 = not up)
 * and the row it must reach for argument `arg` (command-id: the row of the top command list that holds command `arg`, -1 = none). */
int  kh_route_cursor(KCtx *g,int kind);
int  kh_route_cursor_target(KCtx *g,int kind,int arg);
void kh_route_state_tick(KCtx *g,u64 n);                           /* once per game frame, before any condition: per-frame probes, trace */
void kh_route_state_summary(KCtx *g,u64 n,char *out,size_t cap);   /* every state at once (time-out fatal, report) */
/* task 33 (the debug warp's result line): what `field` is made of: FIELD_COMMAND.execute calls in the last game frame, field message
 * windows in the list and how many of them wait for a button (`field` = calls > 0, a world set, no window waiting) */
void kh_route_field_parts(KCtx *g,u64 *field_cmds,int *windows,int *waiting);
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

/* ---- task 31 readers (runtime/route_objective.c; read-only) ---------------------------------------------------------------------- */
/* the running mission: its object [0x142a0ff68], header [object+8] (id, flags, INFORMATION message, pause kind, objective message),
 * file path, the pause record of its kind (one of the six at 0x142a114a0) and the message pointer that record holds */
typedef struct { int ok; u64 mission, header, record, text; int id, flags, information, pause_kind, objective; char path[48]; } KhObjective;
int  kh_route_objective(KCtx *g,KhObjective *out);                 /* 0 when no mission runs (or its file is not loaded yet) */
/* the running mission's counters (the gauges of its HUD: up to three objects of 0x50 bytes at mission+0x160; one that is in use has
 * its layout pointer +8 set): value dword +0x38, target dword +0x24.  Returns how many are in use (0 = none, or no mission) */
int  kh_route_mission_counters(KCtx *g,int value[3],int target[3]);
size_t kh_route_objective_format(KCtx *g,u64 n,const KhObjective *o,char *out,size_t cap);   /* the `kh: objective:` line of a mission */
int  kh_route_message_id_of(KCtx *g,u64 p);                        /* the id of the message that contains the byte at p, -1 = none */
int  kh_route_message_ctl_args(int code);                          /* argument bytes of a control code (0x00..0x1f) */
void kh_route_objective_test_reset(void); void kh_route_message_test_reset(void);
void kh_route_objective_tick(KCtx *g,u64 n);                       /* once per game frame: `kh: objective:` when the mission or its texts change */
void kh_route_objective_trace(KCtx *g,u64 n);                      /* KHNMC_ROUTE_TRACE=2: reaction rows on change, named objects every 64 frames */
u64  kh_route_message(KCtx *g,int id,int *table);                  /* 0x3ad9a0's lookup: pointer to the message's bytes, 0 = in no table */
int  kh_route_message_id(KCtx *g,u64 p);                           /* the id of the message that starts at p, -1 = none */
size_t kh_route_message_text(KCtx *g,u64 p,char *out,size_t cap);  /* the message as ASCII; returns the bytes read */
int  kh_route_message_by_id(KCtx *g,int id,char *out,size_t cap);
/* INFORMATION lines shown so far (calls of 0x17b710) and the last one's message id: the id the game looked up for it (0x3ad9a0 in the
 * same game frame on the same thread gave that pointer), else the only id whose text starts there, else -1 (none, or several ids share
 * the text and the game's lookup was not seen) */
u64  kh_route_information_shown(int *last_id);
const char *kh_route_information_line(void);                       /* the last `kh: information:` line (without the newline), for the selftest */
void kh_route_information_note(KCtx *g,u64 p);                     /* what the 0x17b710 hook does before it passes the call on */
void kh_route_message_lookup_note(int id,u64 p);                   /* what the 0x3ad9a0 hook records: this thread's last lookup (id -> p) */
int  kh_route_message_ids(KCtx *g,u64 p,int *ids,int max);         /* every id whose text starts at p (deduplicated texts share one) */
u64  kh_route_objective_lines(const char **last);                  /* `kh: objective:` mission lines printed so far and the last one */
/* the reaction command the field command menu offers now: the entry the reaction handler 0x408520 keeps at FC+0xb40 (command word
 * +2, state byte +5: below 4 = the reaction button answers it, target object id +8); offered = a command is there, usable = offered
 * and the field command's update 0x3b2340 would answer it in the next frame as far as memory says (closed = why not, NULL = usable):
 * state below 4, the pad record [FC+0xae8] set, FC+0x24 bits 0 and 1 clear, [0x142a10500] = 0 (0x3a57f0), and the entry fresh: the
 * reaction handler 0x408520 only picks one when 0x39a8c0 is true and [FC+0xb3c] bit 0 is set, otherwise it returns and the entry
 * stays as it was.  Not read: the player class's virtual +0x78 (code), which 0x3b2340 also asks. */
typedef struct { int offered, usable, cmd, state; u32 target_id; const char *closed; } KhReaction;
int  kh_route_reaction(KCtx *g,KhReaction *out);                   /* returns usable */
int  kh_route_reaction_target(KCtx *g,const KhReaction *r,char *out,size_t cap);   /* the model name of the object the offered command belongs to */
int  kh_route_item_slots(KCtx *g,int item[8],int *armor_slots,int *accessory_slots);   /* the field player's item slots (returns their number, -1 = unreadable: no player, no equipment record) */
int  kh_route_menu_page(KCtx *g);                                  /* the camp menu's page that takes input (0 = its main list), -1 = none */
int  kh_route_camp_menu_closed(KCtx *g,char *why,size_t cap);      /* 0 = START would open the camp menu; else 1 and the first reason found */
int  kh_route_menu_locked(KCtx *g);
/* the save's progress flag N (N = world << 10 | index; exe 0x39afc0 / 0x39a980: bit N&31 of the dword 0x1409ab540 + 4*(((N&0x3ff)>>5) +
 * 8*(N>>10))) and menu flag N (exe 0x3e38f0: bit N&31 of the dword 0x1409abc40 + 4*(N>>5)); 1 set, 0 clear, -1 = out of range */
int  kh_route_progress_flag(KCtx *g,int flag);
int  kh_route_menu_flag(KCtx *g,int flag);
int  kh_route_lock_on(KCtx *g,u32 *target_id);                     /* the player is locked on to a target (his target record [player+0xdc8] is of kind 2) */
int  kh_route_command_name(KCtx *g,int cmd,char *out,size_t cap);  /* a command's name from the game's message data */
/* the field message window that waits for a button (the first one of the list): mode 1 = a page to confirm, 2 = a choice (rows,
 * cursor; takes = its input phase [w+0x408] is 1); text = the message's first byte, page = the first byte of the page shown */
typedef struct { u64 w, text, page; int mode, rows, cursor, takes; } KhMessageWindow;
int  kh_route_message_window(KCtx *g,KhMessageWindow *out);
size_t kh_route_message_page(KCtx *g,u64 p,char *out,size_t cap);  /* one page of a message (up to the page break 0x10) as ASCII */
int  kh_route_object_name(KCtx *g,u64 obj,char *out,size_t cap);   /* an object's objentry model name */
int  kh_route_objects_named(KCtx *g,const char *name,float px,float pz,float *nx,float *nz,float *nd);   /* count, nearest; -1 = no map */
/* the texts a system menu looks up (0x3ad9a0 hook): `kh: menu text:` once per message id and (menu id, camp menu page) while a menu runs */
void kh_route_menu_text_note(KCtx *g,int id,u64 p);
u64  kh_route_menu_text_lines(const char **last);                  /* lines printed so far and the last one */
int  kh_route_menu_text_seen(KCtx *g,int id);                      /* a menu runs and looked message `id` up on the page it shows now */
void kh_route_menu_text_test_reset(void);
