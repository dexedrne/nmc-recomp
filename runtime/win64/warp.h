/* warp.h : the debug warp of the scripted controller (task 33; runtime/warp.c).  A TEST TOOL: a result after a warp says "this room
 * loads from this state", not "the game reaches it". */
#pragma once
#include "khnmc_rt.h"
#define KH_WARP_WORLDS 19
#define KH_WARP_ROOMS 64
#define KH_WARP_LIST_MAX 4096
extern const char *const kh_warp_world_names[KH_WARP_WORLDS];
typedef struct { int world, room, door; int prog[3]; unsigned line; char text[160]; } KhWarpTarget;   /* prog: map, btl, evt; 0xffff = the save's */
int  kh_warp_world_lookup(const char *s);                          /* "mu" or 8 -> 8; -1 = unknown */
/* WORLD ROOM [door N] [map N] [btl N] [evt N] (+ [settle S] [timeout T] when options) from words[*i]; 0 + *why on error */
int  kh_warp_parse(char **w,int n,int *i,KhWarpTarget *t,int options,u64 *settle,u64 *timeout,const char **why);
int  kh_warp_list_load(const char *path,KhWarpTarget **out,size_t *count,char *why,size_t cap);   /* 0 + why (with the line) on error */
void kh_warp_record(const KhWarpTarget *t,u8 rec[10]);             /* the place record handed to 0x152990 */
void kh_warp_describe(const KhWarpTarget *t,char *out,size_t cap);
int  kh_warp_is_probe(const char *rel);                            /* NNcommon.bdx / NNeffect.bar: the numbered probes of every run */
/* the game's side, replaceable by the selftest */
typedef struct {
    int  (*cond)(KCtx *g,int kind,u64 n);                           /* KR_FIELD, KR_MENU_OPEN, KR_MESSAGE */
    int  (*jump_busy)(KCtx *g);                                    /* a jump task exists or a jump runs */
    int  (*place)(KCtx *g,int *world,int *room);                   /* 0 = no place (world 0xff) */
    void (*request)(KCtx *g,const u8 rec[10]);                     /* exe 0x152990(record, 1, 0, 0, 0) */
    /* what `field` is made of, for the result line: FIELD_COMMAND calls in the last frame, message windows, of them waiting; NULL = not
     * known (the line prints -1) */
    void (*parts)(KCtx *g,u64 *field_cmds,int *windows,int *waiting);
} KhWarpProbe;
void kh_warp_set_probe(const KhWarpProbe *p);                      /* NULL = the real game */
void kh_warp_begin(KCtx *g,int index,const KhWarpTarget *t,u64 n);
int  kh_warp_tick(KCtx *g,u64 n,u64 settle,u64 timeout,char *what,size_t cap);   /* 0 going, 1 field reached, -1 failed (what) */
int  kh_warp_active(void);
u64  kh_warp_count(void);                                          /* debug warps requested in this run */
void kh_warp_counts(u64 *requested,u64 *field,u64 *jumped,u64 *failed);   /* jumped: the room's own program jumped on (JUMPED-ON) */
void kh_warp_reset_counts(void);                                   /* kh_input_load: the counts are the loaded route's */
void kh_warp_file_event(KCtx *g,const char *path,const char *source);   /* runtime/file_layer.c: every file the game asks for */
#define KH_WARP_SETTLE_DEFAULT 60
