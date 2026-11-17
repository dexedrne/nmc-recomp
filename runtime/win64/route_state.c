/* route_state.c : the game states a scripted route waits for (Task 27; player, command menu, map, camera and item readers: Task 29).  Every condition is read from the game's own memory or from
 * a probe that already exists, on the game thread at the end of a game frame (the 0x105e10 hook in runtime/null_input.c), so that
 * a route follows what the game shows instead of how many frames the host managed to draw.  Where each one comes from (exe RVAs;
 * names from the corrected name DB where it has them, otherwise from the code that reads the value):
 *
 *   initial-state N   Initial Settings state machine: [this+0x48] at its vfn1 0x5122e0 (runtime/phase_log.c, the probe `@hold`
 *                     used since task 14), believed only between the ctor 0x5111d0 and the dtor 0x511300.  3 = the settings page
 *                     waits for input, 14 = "Begin with these settings?" waits for input (cursor on No), 4..10 = settings applied.
 *   title-screen      the title's "press a button" state: the title task 0x2b4590 is running (entered and not returned),
 *                     its layout [0x140b1d5d0] exists with sequence [layout+0xe0] 12 or 13, mode [0x140b1d5ec] = 0, no menu yet
 *                     ([0x140b1d5d8] = 0) and MENU::isRunning [0x1409006b0] = 0.  That is the branch of the title's input
 *                     handler 0x2b35a0 that opens the menu on a confirm (pad bits 0x404 of [0x140bf31a0+8]).
 *   title-menu        the title menu takes input: title task running, layout, mode 0, menu [0x140b1d5d8] != 0, MENU::isRunning 0:
 *                     0x2b35a0 then reads up / down / confirm every frame (0x2b37b0..).
 *   title-choice N    title-menu and its cursor [0x140b1d5e4] = N (0 = New Game; count [0x140b1d5f0]).
 *   newgame-dialog N  page N of the New Game menu takes input: the menu task 0x2b7420 is running (hook in phase_log.c), the title is
 *                     in mode 1, its context [0x140b1d800] has page [ctx] = N and phase [ctx+4] = 1, and the menu layout
 *                     [[0x140b1d808]+0x614] is at sequence 0x1d (the ready test of 0x2b92c0).  Page 0 = difficulty (4 rows:
 *                     0 Beginner, 1 Standard, 2 Critical, 3 Less Than Nothing; the cursor starts on GAME_SAVE::GetGameLevel
 *                     0x152350 = Standard after InitSave), page 1 = the second question (2 rows), page 2 = the final confirmation;
 *                     NewGameMenu::commit 0x2b88c0 then calls GAME_SAVE::SetGameLevel 0x152370 with the page-0 answer.
 *   newgame-choice N  a newgame-dialog page is up and the row under its cursor is N: list [0x140b1d7e0], (s16 [list] +
 *                     s16 [list+0x12]) mod u16 [list+0x16], without the modulo when that word is 0 (the exe's 0x2f7fb0).
 *   game-level N      the save's difficulty dword [0x1409abd48] = [save+0x2498] (GAME_SAVE::GetGameLevel 0x152350) is N.
 *   field             the field is up and the player has control: FIELD_COMMAND.execute (0x3f29e0, the mod DLL's replacement,
 *                     probe in phase_log.c) ran during this game frame, the current world [0x140717008] is set (not 0xff) and
 *                     no field message window waits for a button (`message` below is false).
 *   battle            a battle is in progress: BATTLE.calc_damage (0x3c12f0, probe in phase_log.c) computed a hit within the
 *                     last KR_BATTLE_RECENT game frames.  (No separate battle-mode word was identified in the time of this task.)
 *   hits N            at least N hits computed by BATTLE.calc_damage since the step started (either side's hits).
 *   message           a field message window waits for a button: one of the YS::WINDOW_BASE windows (list head [0x142a16310],
 *                     next window = object id [window+0xac], decoded through the id table at 0x142b0d720 as 0x4ad270 does) is in
 *                     state [window+0xa0] = 2 with mode [window+0x260] 1 (confirm: 0x40c advances) or 2 (a choice), no child
 *                     window [window+0x5c0] (YS::WINDOW_BASE::vfn4 0x3bce30).
 *   frames N          N game frames have passed since the step started (a delay counted in game frames, never in wall time).
 *
 * Task 29 (exe RVAs; each structure was read from the disassembly of the functions named and checked against traced runs, run-N29/):
 *   the player        YS::FIELD_COMMAND has one static instance, FC = 0x142a10620 (its ctor 0x24560 stores the vtable 0x1405c4c68,
 *                     whose slot 2 is FIELD_COMMAND.execute 0x3f29e0).  [FC+0xae0] = the field player's object (the mod's 0x3f29e0
 *                     port reads it the same way; 0x3f27d0 clears it on a reset), [obj+0x5c0] = his status sheet, one of the 80
 *                     records of 0x278 bytes in the pool 0x142a17290..0x142a23810 (allocator 0x3c04e0; character id at +0x260):
 *                     HP +0, max HP +4, floor +8 (the HP add 0x3c0860 clamps to it), MP +0x180, max MP +0x184.
 *   hp-below-max, mp-below-max, hp-at-most N, mp-at-most N   read from that sheet.
 *   position, facing  [obj+0x670] = x, y, z, 1.0 (YS::OBJ::set_pos 0x3b64d0 copies a vector there and sets +0x67c = 1.0);
 *                     [obj+0x68c] = the facing angle in radians, [obj+0x680] = its (sin, 0, cos).  y points down.
 *   camera            0x19ce40(0) returns the current camera record: (byte [0x140ac183c] ? 0x140ac1020 : 0x140ac1b50) + 0x80 *
 *                     [0x140ac1838].  Eye = record+0x48, look-at = record+0x58 (x, y, z, 1 each).  Movement is camera-relative:
 *                     with the left stick at angle a = atan2(lx, ly) (up 0, right +pi/2) the player runs along heading = camera
 *                     yaw + a, where camera yaw = atan2(at.x - eye.x, at.z - eye.z) and a heading h is the direction (sin h, cos h)
 *                     in x / z (traced in four directions, run-N29/disc1: all within 0.03 rad).  (The mod DLL's GetCameraEye /
 *                     GetCameraAt pointers, set by its worker 0x342a0, are 0x140ac1884 / 0x140ac1894, which hold no vectors in
 *                     this exe; those two syscalls are not ported.)
 *   command menu      FC is also the field command menu: lists of 0x80 bytes at FC+0x28+0x80*k (k = 0 the main list, 2 Items
 *                     (0x3f3300 fills it from the player's item slots), 3 Drive, 4 party, ...; a command record's byte +6 names the
 *                     list it opens) and a stack of list pointers at FC+0xaf0 whose top entry [FC+0x9a8] points at (depth =
 *                     (top - (FC+0xaf0)) / 8 + 1, as 0x3f2470 computes it).  A list: type +0, row count +0x10, rows of 12 bytes
 *                     from +0x14 (command id word at +2, state byte at +5: below 4 = can be chosen, 5 = the list it opens is
 *                     empty, as 0x3b0f20 sets it), cursor +0x74, command under the cursor +0x78 (0x3b1b80).  DPAD_DOWN moves the
 *                     cursor to the next row, DPAD_UP to the previous one, both wrapping (traced).
 *   command-depth N, command-row N, command-id N, command-has N   read from the top of that stack while the player exists.
 *   world N, area N   [0x140717008] and [0x140717009] (0xff while no map is up).
 *   menu-open         MENU::isRunning [0x1409006b0]: a system menu runs.  START (0x1572b0) opens the camp menu (0x2e4950(0)) only when
 *                     no mission runs; while one does it opens that mission's pause screen (0x2e4c20, menu/us/pause.2ld).
 *   enemies-at-most N the object list (head [0x142a171c8], next = the id at [obj+0xa90], as 0x3bfb70 walks it) holds at most N objects
 *                     whose class record (the id at [obj+0]) has an enemy vtable: BTLOBJ::VTABLE<YS::ZAKO / ENEMY / BOSS /
 *                     LARGEZAKO / LARGEBOSS> (0x5d2d68, 0x5c71d8, 0x5c9c40, 0x5d3ff8, 0x5d2858; names from the class DB).
 *   mission           0x3ad500 finds a mission record: the highest of the six 0x40-byte records at 0x142a114a0 whose first word
 *                     is not 0 (the first field's EH_NOKILL mission keeps record 5 set; its barricade F_BARRICADE_03 stands in
 *                     the exit to eh01).
 *   item-uses N, item-drain, item-heal   the record the mod's item handler port keeps of its last kind-0 application
 *                     (mod_difficulty.c item_stock_apply: the target sheet's HP / MP right before and right after the DLL's adds).
 * Map reports: each time world / area change to a map, `kh: map report` (id table, arena zones, RSS, files, threads) is printed
 * (kh_route_map_report); with KHNMC_IDTABLE_REPORT=1 the whole id table follows.
 *
 * KHNMC_ROUTE_TRACE=1 prints the whole state (kh_route_state_summary) whenever it changes: the tool used to find the values above.
 * KHNMC_ROUTE_TRACE=2 also prints the player's sheet and position, the camera and the command menu every 8 game frames on a map. */
#include "route_state.h"
#include "mod_config.h"
#include <math.h>
#include <sys/resource.h>
u64 kh_input_game_frames(void);
u64 kh_phase_field_cmds(void); u64 kh_phase_damage_hits(void); u64 kh_phase_damage_last_frame(void);
int kh_phase_initial_alive(void); int kh_phase_initial_state(void); int kh_phase_title_alive(void); int kh_phase_newgame_alive(void);
const KhRouteCondInfo kh_route_cond_info[KR_NCOND]={
    [KR_INITIAL_STATE] ={"initial-state",1,0,63,"the Initial Settings state machine is in state N"},
    [KR_TITLE_SCREEN]  ={"title-screen",0,0,0,"the title waits for a button to open its menu"},
    [KR_TITLE_MENU]    ={"title-menu",0,0,0,"the title menu takes input"},
    [KR_TITLE_CHOICE]  ={"title-choice",1,0,15,"the title menu's cursor is on row N"},
    [KR_NEWGAME_DIALOG]={"newgame-dialog",1,0,2,"page N of the New Game menu takes input (0 difficulty, 1 second question, 2 confirmation)"},
    [KR_NEWGAME_CHOICE]={"newgame-choice",1,0,15,"a New Game page takes input with its cursor on row N"},
    [KR_GAME_LEVEL]    ={"game-level",1,0,3,"the save's difficulty is N (0 Beginner, 1 Standard, 2 Critical, 3 Less Than Nothing)"},
    [KR_FIELD]         ={"field",0,0,0,"the field is up and the player has control"},
    [KR_BATTLE]        ={"battle",0,0,0,"a battle is in progress (a hit computed in the last 300 game frames)"},
    [KR_HITS]          ={"hits",1,1,100000,"N hits computed since the step started"},
    [KR_MESSAGE]       ={"message",0,0,0,"a field message window waits for a button"},
    [KR_FRAMES]        ={"frames",1,1,1000000,"N game frames since the step started"},
    [KR_HP_BELOW_MAX]  ={"hp-below-max",0,0,0,"the player's HP is below its maximum"},
    [KR_MP_BELOW_MAX]  ={"mp-below-max",0,0,0,"the player's MP is below its maximum"},
    [KR_HP_AT_MOST]    ={"hp-at-most",1,0,1000000,"the player's HP is at most N"},
    [KR_MP_AT_MOST]    ={"mp-at-most",1,0,1000000,"the player's MP is at most N"},
    [KR_COMMAND_DEPTH] ={"command-depth",1,0,16,"the field command menu's stack is N lists deep (1 = the main list, 2 = a sub-list is open, 0 = no command menu)"},
    [KR_COMMAND_ROW]   ={"command-row",1,0,7,"the top command list's cursor is on row N"},
    [KR_COMMAND_ID]    ={"command-id",1,0,65535,"the top command list's cursor is on a row of command N"},
    [KR_COMMAND_HAS]   ={"command-has",1,0,65535,"the top command list has a row of command N that can be chosen"},
    [KR_WORLD]         ={"world",1,0,254,"the current world id is N"},
    [KR_AREA]          ={"area",1,0,254,"the current area id is N"},
    [KR_MENU_OPEN]     ={"menu-open",0,0,0,"a system menu runs (MENU::isRunning)"},
    [KR_MISSION]       ={"mission",0,0,0,"a mission runs (0x3ad500 finds a mission record)"},
    [KR_ENEMIES_AT_MOST]={"enemies-at-most",1,0,1000,"at most N enemies are in the object list"},
    [KR_ITEM_USES]     ={"item-uses",1,1,1000,"N items applied by the mod's item handler since the step started"},
    [KR_ITEM_DRAIN]    ={"item-drain",0,0,0,"an item applied since the step started lowered the target's HP or MP"},
    [KR_ITEM_HEAL]     ={"item-heal",0,0,0,"an item applied since the step started raised the target's HP or MP"},
};
int kh_route_cond_lookup(const char *name){ for(int i=0;i<KR_NCOND;i++) if(!strcmp(name,kh_route_cond_info[i].name)) return i; return -1; }

#define T_LAYOUT   (GS_BASE+0xb1d5d0ull)   /* title layout object (sequence at +0xe0) */
#define T_MENU     (GS_BASE+0xb1d5d8ull)   /* title menu object: created by 0x2b4480 when the first confirm opens the menu */
#define T_CURSOR   (GS_BASE+0xb1d5e4ull)
#define T_MODE     (GS_BASE+0xb1d5ecull)   /* 0 main menu, 1 New Game menu, 2 load, 3 other */
#define T_COUNT    (GS_BASE+0xb1d5f0ull)
#define MENU_RUN   (GS_BASE+0x9006b0ull)   /* MENU::isRunning 0x1515c0 reads this byte */
#define NG_LIST    (GS_BASE+0xb1d7e0ull)
#define NG_CTX     (GS_BASE+0xb1d800ull)
#define NG_LAYOUT  (GS_BASE+0xb1d808ull)
#define GAME_LEVEL (GS_BASE+0x9abd48ull)
#define NOW_WORLD  (GS_BASE+0x717008ull)   /* the current world / room / place / map / battle set / event (mod_hooks_b.c reads it too) */
#define WIN_HEAD   (GS_BASE+0x2a16310ull)
#define ID_TABLE   (GS_BASE+0x2b0d720ull)

static u64 fc_prev, fc_frame;              /* FIELD_COMMAND.execute calls: running total at the last tick, calls during the last frame */
static int readable(u64 a,u64 n){ return a && GA_OK(a) && GA_OK(a+n-1); }
static int title_up(KCtx *g){ u64 lay=rd64(g,T_LAYOUT); return kh_phase_title_alive() && readable(lay,0xe8) && rd8(g,MENU_RUN)==0 && (s32)rd32(g,T_MODE)==0; }
static int title_screen(KCtx *g){ if(!title_up(g) || rd64(g,T_MENU)) return 0; s32 seq=(s32)rd32(g,rd64(g,T_LAYOUT)+0xe0); return seq==12||seq==13; }
static int title_menu(KCtx *g){ return title_up(g) && rd64(g,T_MENU)!=0; }
/* the page of the New Game menu that takes input, -1 when none does */
static int newgame_page(KCtx *g){
    if(!kh_phase_newgame_alive() || !kh_phase_title_alive() || (s32)rd32(g,T_MODE)!=1) return -1;
    u64 ctx=rd64(g,NG_CTX), lay=rd64(g,NG_LAYOUT);
    if(!readable(ctx,0x10) || !readable(lay,0x618)) return -1;
    s32 page=(s32)rd32(g,ctx), phase=(s32)rd32(g,ctx+4);
    return page>=0 && page<=2 && phase==1 && (s32)rd32(g,lay+0x614)==0x1d ? page : -1;
}
static int newgame_choice(KCtx *g){
    if(newgame_page(g)<0) return -1;
    u64 list=rd64(g,NG_LIST); if(!readable(list,0x18)) return -1;
    int cnt=(u16)rd16(g,list+0x16), v=(s16)rd16(g,list)+(s16)rd16(g,list+0x12);
    return cnt ? v%cnt : v;                              /* as 0x2f7fb0: no modulo when the word at +0x16 is 0 (the confirmation page) */
}
/* field message windows: count of windows in the list, and of those that wait for a button */
static void windows(KCtx *g,int *total,int *waiting){
    *total=*waiting=0; u64 w=rd64(g,WIN_HEAD);
    for(int k=0;k<16 && readable(w,0x5c8);k++){
        (*total)++;
        s32 st=(s32)rd32(g,w+0xa0), mode=(s32)rd32(g,w+0x260);
        if(st==2 && (mode==1||mode==2) && !rd64(g,w+0x5c0)) (*waiting)++;
        u32 id=rd32(g,w+0xac); if(!id) break;
        w=rd64(g,ID_TABLE+8ull*((id&0x7fffffffu)>>25))|(id&0x1ffffffu);
    }
}
static int message(KCtx *g){ int t,wt; windows(g,&t,&wt); return wt>0; }
static int field(KCtx *g){ return fc_frame>0 && rd8(g,NOW_WORLD)!=0xff && !message(g); }
static int battle(u64 n){ u64 h=kh_phase_damage_hits(), last=kh_phase_damage_last_frame(); return h>0 && n>=last && n-last<=KR_BATTLE_RECENT; }

/* ---- task 29 ------------------------------------------------------------------------------------------------------------------ */
#define FC           (GS_BASE+0x2a10620ull)  /* YS::FIELD_COMMAND, the one static instance */
#define FC_PLAYER    (FC+0xae0)
#define FC_STACK_TOP (FC+0x9a8)
#define FC_STACK     (FC+0xaf0)
#define SHEET_POOL   (GS_BASE+0x2a17290ull)  /* 80 status sheets of 0x278 bytes (0x3c04e0) */
#define SHEET_END    (GS_BASE+0x2a23810ull)
#define CAM_SEL      (GS_BASE+0xac183cull)
#define CAM_IDX      (GS_BASE+0xac1838ull)
#define CAM_A        (GS_BASE+0xac1020ull)
#define CAM_B        (GS_BASE+0xac1b50ull)
static float rdf(KCtx *g,u64 a){ u32 v=rd32(g,a); float f; memcpy(&f,&v,4); return f; }
static u64 player_obj(KCtx *g){
    if(rd8(g,NOW_WORLD)==0xff) return 0;
    u64 obj=rd64(g,FC_PLAYER); if(!readable(obj,0xe00)) return 0;
    u64 sh=rd64(g,obj+0x5c0);                                   /* a player being built has no sheet yet (allocator fill 0xefaccafe...) */
    return sh>=SHEET_POOL && sh<SHEET_END && (sh-SHEET_POOL)%0x278==0 ? obj : 0;
}
int kh_route_player_sheet(KCtx *g,KhPlayerSheet *o){
    memset(o,0,sizeof *o);
    u64 obj=player_obj(g); if(!obj) return 0;
    u64 sh=rd64(g,obj+0x5c0);                                    /* player_obj accepted it: a record of the sheet pool */
    o->obj=obj; o->sheet=sh; o->hp=(s32)rd32(g,sh); o->hp_max=(s32)rd32(g,sh+4); o->mp=(s32)rd32(g,sh+0x180); o->mp_max=(s32)rd32(g,sh+0x184);
    o->chr=(s32)rd32(g,sh+0x260); o->ok=1; return 1;
}
int kh_route_player_pos(KCtx *g,float *x,float *y,float *z,float *yaw){
    u64 obj=player_obj(g); if(!obj) return 0;
    float a=rdf(g,obj+0x670),b=rdf(g,obj+0x674),c=rdf(g,obj+0x678),h=rdf(g,obj+0x68c);
    if(!isfinite(a)||!isfinite(b)||!isfinite(c)||!isfinite(h)) return 0;
    *x=a; *y=b; *z=c; if(yaw) *yaw=h; return 1;
}
int kh_route_camera_yaw(KCtx *g,float *yaw){
    if(rd8(g,NOW_WORLD)==0xff) return 0;
    s32 idx=(s32)rd32(g,CAM_IDX); if(idx<0 || idx>=16) return 0;
    u64 rec=(rd8(g,CAM_SEL)?CAM_A:CAM_B)+0x80ull*(u64)idx;
    float ex=rdf(g,rec+0x48),ez=rdf(g,rec+0x50),ax=rdf(g,rec+0x58),az=rdf(g,rec+0x60);
    if(!isfinite(ex)||!isfinite(ez)||!isfinite(ax)||!isfinite(az)) return 0;
    float dx=ax-ex,dz=az-ez; if(dx*dx+dz*dz<1.0f) return 0;                  /* looking straight down: no horizontal direction */
    *yaw=atan2f(dx,dz); return 1;
}
void kh_route_goto_stick(float x,float z,float tx,float tz,float cam_yaw,float scale,s16 *lx,s16 *ly){
    float a=atan2f(tx-x,tz-z)-cam_yaw, m=32767.0f*(scale>1.0f?1.0f:scale<0.0f?0.0f:scale);
    *lx=(s16)lrintf(sinf(a)*m); *ly=(s16)lrintf(cosf(a)*m);
}
int kh_route_command_menu(KCtx *g,KhCommandMenu *o){
    memset(o,0,sizeof *o); o->cursor=-1; o->cmd_under=-1;
    if(!player_obj(g)) return 0;
    u64 top=rd64(g,FC_STACK_TOP);
    if(top<FC_STACK || top>=FC_STACK+8*16 || (top-FC_STACK)%8) return 0;
    u64 L=rd64(g,top); if(!readable(L,0x80)) return 0;
    o->depth=(int)((top-FC_STACK)/8)+1; o->type=(int)(s32)rd32(g,L);
    int n=(int)(s32)rd32(g,L+0x10); o->rows=n<0?0:n>8?8:n;
    for(int i=0;i<o->rows;i++){ o->cmd[i]=rd16(g,L+0x14+12*(u64)i+2); o->state[i]=rd8(g,L+0x14+12*(u64)i+5); }
    int c=(int)(s32)rd32(g,L+0x74); o->cursor=c>=0&&c<o->rows?c:-1;
    o->cmd_under=o->cursor>=0?o->cmd[o->cursor]:-1;
    return o->depth;
}
int kh_route_world(KCtx *g){ u8 w=rd8(g,NOW_WORLD); return w==0xff?-1:w; }
int kh_route_area(KCtx *g){ u8 a=rd8(g,NOW_WORLD+1); return rd8(g,NOW_WORLD)==0xff||a==0xff?-1:a; }
#define MISSIONS (GS_BASE+0x2a114a0ull)    /* six mission records of 0x40 bytes; 0x3ad500 returns the highest whose first word is not 0 */
u64 kh_route_mission(KCtx *g){ for(int i=5;i>=0;i--){ u64 r=MISSIONS+0x40ull*(u64)i; if(rd16(g,r)) return r; } return 0; }
/* The object list: head [0x142a171c8], next = the object id at +0xa90 (0x3bfb70 walks it).  [obj+0] is the id of the object's
 * class record, whose first qword is the class's vtable; the class DB names the vtables (BTLOBJ::VTABLE<YS::X>). */
#define OBJ_HEAD (GS_BASE+0x2a171c8ull)
static const u32 enemy_vt[]={0x5d2d68 /* ZAKO */,0x5c71d8 /* ENEMY */,0x5c9c40 /* BOSS */,0x5d3ff8 /* LARGEZAKO */,0x5d2858 /* LARGEBOSS */};
static u64 decode_id(KCtx *g,u32 id){ return rd64(g,ID_TABLE+8ull*((id&0x7fffffffu)>>25))|(id&0x1ffffffu); }
int kh_route_enemies(KCtx *g,float px,float pz,float *nx,float *nz,float *nd){
    if(rd8(g,NOW_WORLD)==0xff) return -1;
    u64 o=rd64(g,OBJ_HEAD); int n=0; float best=1e30f;
    for(int k=0;k<512 && o && readable(o,0xe00);k++){
        u32 cid=rd32(g,o); u64 cls=cid&0x80000000u?decode_id(g,cid):0;
        if(readable(cls,8)){ u64 vt=rd64(g,cls)-GS_BASE;
            for(size_t i=0;i<sizeof enemy_vt/sizeof *enemy_vt;i++) if(vt==enemy_vt[i]){
                float x=rdf(g,o+0x670),z=rdf(g,o+0x678); n++;
                if(isfinite(x)&&isfinite(z)){ float d=hypotf(x-px,z-pz); if(d<best){ best=d; if(nx) *nx=x; if(nz) *nz=z; } } } }
        u32 id=rd32(g,o+0xa90); if(!id) break; o=decode_id(g,id);
    }
    if(nd) *nd=best; return n;
}
static int item_cond(int kind,const KhRouteBase *b){
    struct kh_item_obs o; u64 seq=kh_mod_item_obs(&o);
    if(kind==KR_ITEM_USES) return 0;
    if(seq<=b->items) return 0;
    int down=(o.hp_call&&o.hp_after<o.hp_before)||(o.mp_call&&o.mp_after<o.mp_before);
    int up=(o.hp_call&&o.hp_after>o.hp_before)||(o.mp_call&&o.mp_after>o.mp_before);
    return kind==KR_ITEM_DRAIN?down:up;
}

int kh_route_cursor(KCtx *g,int kind){
    if(kind==KR_TITLE_CHOICE) return title_menu(g)?(int)(s32)rd32(g,T_CURSOR):-1;
    if(kind==KR_NEWGAME_CHOICE) return newgame_choice(g);
    if(kind==KR_COMMAND_ROW || kind==KR_COMMAND_ID){ KhCommandMenu m; return kh_route_command_menu(g,&m)?m.cursor:-1; }
    return -1;
}
int kh_route_cursor_target(KCtx *g,int kind,int arg){
    if(kind!=KR_COMMAND_ID) return arg;
    KhCommandMenu m; if(!kh_route_command_menu(g,&m)) return -1;
    for(int i=0;i<m.rows;i++) if(m.cmd[i]==arg) return i;
    return -1;
}
void kh_route_base_now(u64 n,KhRouteBase *b){ b->frame=n; b->hits=kh_phase_damage_hits(); b->items=kh_mod_item_obs(NULL); }
int kh_route_cond_eval(KCtx *g,int kind,int arg,u64 n,const KhRouteBase *b){
    KhPlayerSheet ps; KhCommandMenu cm;
    switch(kind){
    case KR_INITIAL_STATE:  return kh_phase_initial_alive() && kh_phase_initial_state()==arg;
    case KR_TITLE_SCREEN:   return title_screen(g);
    case KR_TITLE_MENU:     return title_menu(g);
    case KR_TITLE_CHOICE:   return title_menu(g) && (s32)rd32(g,T_CURSOR)==arg;
    case KR_NEWGAME_DIALOG: return newgame_page(g)==arg;
    case KR_NEWGAME_CHOICE: return newgame_choice(g)==arg;
    case KR_GAME_LEVEL:     return (s32)rd32(g,GAME_LEVEL)==arg;
    case KR_FIELD:          return field(g);
    case KR_BATTLE:         return battle(n);
    case KR_HITS:           return kh_phase_damage_hits()-b->hits>=(u64)arg;
    case KR_MESSAGE:        return message(g);
    case KR_FRAMES:         return n-b->frame>=(u64)arg;
    case KR_HP_BELOW_MAX:   return kh_route_player_sheet(g,&ps) && ps.hp<ps.hp_max;
    case KR_MP_BELOW_MAX:   return kh_route_player_sheet(g,&ps) && ps.mp<ps.mp_max;
    case KR_HP_AT_MOST:     return kh_route_player_sheet(g,&ps) && ps.hp<=arg;
    case KR_MP_AT_MOST:     return kh_route_player_sheet(g,&ps) && ps.mp<=arg;
    case KR_COMMAND_DEPTH:  return kh_route_command_menu(g,&cm)==arg;
    case KR_COMMAND_ROW:    return kh_route_command_menu(g,&cm) && cm.cursor==arg;
    case KR_COMMAND_ID:     return kh_route_command_menu(g,&cm) && cm.cmd_under==arg;
    case KR_COMMAND_HAS:    if(!kh_route_command_menu(g,&cm)) return 0; for(int i=0;i<cm.rows;i++) if(cm.cmd[i]==arg && cm.state[i]<4) return 1; return 0;
    case KR_WORLD:          return kh_route_world(g)==arg;
    case KR_AREA:           return kh_route_area(g)==arg;
    case KR_MENU_OPEN:      return rd8(g,MENU_RUN)!=0;
    case KR_MISSION:        return kh_route_mission(g)!=0;
    case KR_ENEMIES_AT_MOST:{ int e=kh_route_enemies(g,0,0,NULL,NULL,NULL); return e>=0 && e<=arg; }
    case KR_ITEM_USES:      return kh_mod_item_obs(NULL)-b->items>=(u64)arg;
    case KR_ITEM_DRAIN: case KR_ITEM_HEAL: return item_cond(kind,b);
    }
    kh_fatal(g,"route-condition-unknown: condition kind %d",kind);
}
void kh_route_state_summary(KCtx *g,u64 n,char *out,size_t cap){
    int t,wt; windows(g,&t,&wt); u64 hits=kh_phase_damage_hits(); char last[24]="-";
    if(hits) snprintf(last,sizeof last,"%llu",(unsigned long long)kh_phase_damage_last_frame());
    size_t o=(size_t)snprintf(out,cap,"initial-state=%d%s title=%s mode=%d menu=%s cursor=%d screen=%d newgame=%s page=%d choice=%d game-level=%d world=%02x room=%02x "
             "field-cmds-this-frame=%llu field=%d windows=%d waiting=%d menu-open=%d",
             kh_phase_initial_state(),kh_phase_initial_alive()?"":"(gone)",kh_phase_title_alive()?"running":"not-running",(int)(s32)rd32(g,T_MODE),
             rd64(g,T_MENU)?"up":"none",(int)(s32)rd32(g,T_CURSOR),title_screen(g),kh_phase_newgame_alive()?"running":"not-running",newgame_page(g),newgame_choice(g),
             (int)(s32)rd32(g,GAME_LEVEL),rd8(g,NOW_WORLD),rd8(g,NOW_WORLD+1),(unsigned long long)fc_frame,field(g),t,wt,rd8(g,MENU_RUN));
    if(o>=cap) return;
    KhPlayerSheet ps; KhCommandMenu cm;
    if(kh_route_player_sheet(g,&ps)) o+=(size_t)snprintf(out+o,cap-o," player=char%d hp=%d/%d mp=%d/%d",(int)ps.chr,(int)ps.hp,(int)ps.hp_max,(int)ps.mp,(int)ps.mp_max);
    else o+=(size_t)snprintf(out+o,cap-o," player=none");
    if(o>=cap) return;
    if(kh_route_command_menu(g,&cm)){ o+=(size_t)snprintf(out+o,cap-o," command-menu=depth%d/type%d/row%d/cmd%d rows=",cm.depth,cm.type,cm.cursor,cm.cmd_under);
        for(int i=0;i<cm.rows && o<cap;i++) o+=(size_t)snprintf(out+o,cap-o,"%s%u%s",i?",":"",(unsigned)cm.cmd[i],cm.state[i]<4?"":"(off)"); }
    else o+=(size_t)snprintf(out+o,cap-o," command-menu=none");
    if(o>=cap) return;
    o+=(size_t)snprintf(out+o,cap-o," mission=%s enemies=%d",kh_route_mission(g)?"yes":"no",kh_route_enemies(g,0,0,NULL,NULL,NULL));
    if(o>=cap) return;
    snprintf(out+o,cap-o," hits=%llu last-hit-frame=%s battle=%d items=%llu",(unsigned long long)hits,last,battle(n),(unsigned long long)kh_mod_item_obs(NULL));
}

/* ---- map reports (task 29): what a map change costs ---------------------------------------------------------------------------- */
int kh_idtable_slots(int *most); void kh_idtable_report_now(FILE *f,const char *when); int ga_zone_get(int zi,u64 *lo,u64 *hi,u64 *bump,u64 *commit,u64 *live);
void kh_threads_counts(u64 *created,u64 *ended,u64 *released); size_t kh_phase_files_seen(void);
static void rss_mib(long *now,long *peak){
    *now=*peak=-1;
#ifdef __linux__
    FILE *f=fopen("/proc/self/status","r"); if(!f) return; char l[256];
    while(fgets(l,sizeof l,f)){ if(!strncmp(l,"VmRSS:",6)) *now=strtol(l+6,0,10)/1024; else if(!strncmp(l,"VmHWM:",6)) *peak=strtol(l+6,0,10)/1024; }
    fclose(f);
#else
    struct rusage ru; if(!getrusage(RUSAGE_SELF,&ru)) *peak=(long)(ru.ru_maxrss>>20);      /* bytes on Darwin */
#endif
}
void kh_route_map_report(KCtx *g,FILE *f,const char *what){
    int most=0, used=kh_idtable_slots(&most); u64 lo=0,bump=0,live=0,flo=0,fbump=0,flive=0;
    ga_zone_get(0,&lo,NULL,&bump,NULL,&live); ga_zone_get(1,&flo,NULL,&fbump,NULL,&flive);
    long now,peak; rss_mib(&now,&peak); u64 tc=0,te=0,tr=0; kh_threads_counts(&tc,&te,&tr);
    fprintf(f,"kh: map report (%s): game frame %llu, world %02x area %02x; id table %d of 64 slots (most ever %d); id zone high-water %llu MiB, in use %llu MiB; "
              "far zone high-water %llu MiB, in use %llu MiB; RSS %ld MiB (peak %ld MiB); files opened %zu (distinct paths); threads created %llu, ended %llu (stacks released %llu)\n",
            what,(unsigned long long)kh_input_game_frames(),rd8(g,NOW_WORLD),rd8(g,NOW_WORLD+1),used,most,(unsigned long long)((bump-lo)>>20),(unsigned long long)(live>>20),
            (unsigned long long)((fbump-flo)>>20),(unsigned long long)(flive>>20),now,peak,kh_phase_files_seen(),(unsigned long long)tc,(unsigned long long)te,(unsigned long long)tr);
    const char *e=getenv("KHNMC_IDTABLE_REPORT"); if(e&&*e&&*e!='0') kh_idtable_report_now(f,what);
}

/* What the player carries (KHNMC_ROUTE_TRACE=2, printed when it changes): his item slots and the bag's restore items.
 *   item slots  the equipment record of his sheet (id [sheet+0x250]; 0x3d74c0 decodes it, 0x3c04e0 sets it from 0x3a14e0 =
 *               0x1409abda0 + 0x114 * (0x3e60c0(char) - 1)): slot count byte +0x12, the slots' words +0x34 (current) and +0x44 (the
 *               registered item), as 0x3f3300 reads them to build the Items list (FC+0x128).
 *   the bag     item table [0x142a25370] (count +4, records of 0x18 bytes from +8: id word +0, command word +0x10, stock index
 *               +0x12; 0x3e39a0 looks ids up), stock byte save [0x1409a98b0 + 0x3580 + index] (0x3c4720). */
static void trace_carry(KCtx *g){
    static char prev[600]; char cur[600]; size_t o=0; KhPlayerSheet ps;
    if(!kh_route_player_sheet(g,&ps)) return;
    u32 rid=rd32(g,ps.sheet+0x250); u64 rec=rid&0x80000000u?decode_id(g,rid):0;
    o+=(size_t)snprintf(cur+o,sizeof cur-o,"char %d equipment record %llx",(int)ps.chr,(unsigned long long)rec);
    if(readable(rec,0x114)){ o+=(size_t)snprintf(cur+o,sizeof cur-o," item slots %d: current",(int)(s8)rd8(g,rec+0x12));
        for(int j=0;j<8;j++) o+=(size_t)snprintf(cur+o,sizeof cur-o," %x",rd16(g,rec+0x34+2*(u64)j));
        o+=(size_t)snprintf(cur+o,sizeof cur-o," registered"); for(int j=0;j<8;j++) o+=(size_t)snprintf(cur+o,sizeof cur-o," %x",rd16(g,rec+0x44+2*(u64)j)); }
    u64 it=rd64(g,GS_BASE+0x2a25370ull);
    if(readable(it,8)){ int cnt=(int)rd32(g,it+4); o+=(size_t)snprintf(cur+o,sizeof cur-o,"; bag:");
        for(int i=0;i<cnt && i<1000 && o<sizeof cur-40;i++){ u64 r=it+8+0x18*(u64)i; u16 id=rd16(g,r);
            if(id>=1&&id<=7) o+=(size_t)snprintf(cur+o,sizeof cur-o," item%u(cmd %u)=%u",id,rd16(g,r+0x10),rd8(g,GS_BASE+0x9a98b0ull+0x3580+rd16(g,r+0x12))); } }
    if(strcmp(cur,prev)){ fprintf(stderr,"kh: route carry %s\n",cur); strcpy(prev,cur); }
}
static int trace=-1; static char trace_prev[1200]; static int map_prev=-1;
static void trace2(KCtx *g,u64 n){                         /* KHNMC_ROUTE_TRACE=2: the readers' raw values every 8 game frames */
    if(rd8(g,MENU_RUN)){                                     /* a system menu: the cursor / row-id pointers the mod's 0x31dd70 port reads */
        static char prev[300]; char cur[300]; size_t o=0; u64 cp=rd64(g,GS_BASE+0xbef528ull), ip=rd64(g,GS_BASE+0xbef538ull);
        o+=(size_t)snprintf(cur+o,sizeof cur-o,"menu 0x1409006b4=%d 0x140900724=%d cursor*=%llx",(int)rd32(g,GS_BASE+0x9006b4ull),(int)rd32(g,GS_BASE+0x900724ull),(unsigned long long)cp);
        if(readable(cp,2)) o+=(size_t)snprintf(cur+o,sizeof cur-o," cursor=%u",rd16(g,cp));
        if(readable(ip,0x20)){ o+=(size_t)snprintf(cur+o,sizeof cur-o," ids:"); for(int i=0;i<12 && o<sizeof cur-8;i++) o+=(size_t)snprintf(cur+o,sizeof cur-o," %x",rd16(g,ip+2*(u64)i)); }
        if(strcmp(cur,prev)){ fprintf(stderr,"kh: route probe game-frame=%llu %s\n",(unsigned long long)n,cur); strcpy(prev,cur); } }
    if(n%8 || rd8(g,NOW_WORLD)==0xff) return;
    KhPlayerSheet ps; float x=0,y=0,z=0,h=0,cy=0; int p=kh_route_player_pos(g,&x,&y,&z,&h), c=kh_route_camera_yaw(g,&cy);
    kh_route_player_sheet(g,&ps);
    u64 bl=rd64(g,GS_BASE+0x2a0d628ull); char blname[40]="-"; if(readable(bl,0x30)){ memcpy(blname,(void*)XL(g,bl+8),39); blname[39]=0; for(char *q=blname;*q;q++) if(*q<32||*q>126) *q='?'; }
    fprintf(stderr,"kh: route probe game-frame=%llu world=%02x area=%02x player=%llx sheet=%llx char=%d hp=%d/%d mp=%d/%d pos=%s%.1f,%.1f,%.1f facing=%.3f camera-yaw=%s%.3f fc24=%x list628=%llx(%s type %d)\n",
            (unsigned long long)n,rd8(g,NOW_WORLD),rd8(g,NOW_WORLD+1),(unsigned long long)ps.obj,(unsigned long long)ps.sheet,(int)ps.chr,(int)ps.hp,(int)ps.hp_max,(int)ps.mp,(int)ps.mp_max,
            p?"":"(none)",x,y,z,h,c?"":"(none)",cy,rd32(g,FC+0x24),(unsigned long long)bl,blname,readable(bl,0x30)?(int)rd32(g,bl+4):-1);
}
static void trace_objs(KCtx *g,u64 n){                     /* KHNMC_ROUTE_TRACE=2: the object list and the running mission every 64 game frames */
    if(n%64 || rd8(g,NOW_WORLD)==0xff) return;
    u64 o=rd64(g,GS_BASE+0x2a171c8ull); float px=0,py=0,pz=0,ph=0; kh_route_player_pos(g,&px,&py,&pz,&ph);
    char buf[4000]; size_t b=(size_t)snprintf(buf,sizeof buf,"kh: route objs game-frame=%llu:",(unsigned long long)n); int k=0;
    u64 mr=0; for(int i=5;i>=0;i--){ u64 r=GS_BASE+0x2a114a0ull+0x40ull*(u64)i; if(rd16(g,r)){ mr=r; break; } }
    if(mr){ b+=(size_t)snprintf(buf+b,sizeof buf-b," mission@%llx:",(unsigned long long)mr); for(int i=0;i<16;i++) b+=(size_t)snprintf(buf+b,sizeof buf-b," %x",rd32(g,mr+4*(u64)i)); }
    while(o && readable(o,0xe00) && k<64 && b<sizeof buf-200){
        u64 vt=rd64(g,o); float x=rdf(g,o+0x670),z=rdf(g,o+0x678);
        b+=(size_t)snprintf(buf+b,sizeof buf-b," [%llx vt%llx %.0f,%.0f d%.0f]",(unsigned long long)o,(unsigned long long)(vt-GS_BASE),x,z,hypotf(x-px,z-pz));
        u32 id=rd32(g,o+0xa90); if(!id) break; o=rd64(g,ID_TABLE+8ull*((id&0x7fffffffu)>>25))|(id&0x1ffffffu); k++; }
    fprintf(stderr,"%s (%d objects)\n",buf,k+1);
}
void kh_route_state_tick(KCtx *g,u64 n){
    u64 fc=kh_phase_field_cmds(); fc_frame=fc-fc_prev; fc_prev=fc;
    { int w=kh_route_world(g), a=kh_route_area(g), m=w>=0&&a>=0?(w<<8|a):-1;
      if(m>=0 && m!=map_prev){ char what[64]; if(map_prev<0) snprintf(what,sizeof what,"map %02x:%02x entered",w,a);
          else snprintf(what,sizeof what,"map %02x:%02x -> %02x:%02x",map_prev>>8,map_prev&0xff,w,a);
          kh_route_map_report(g,stderr,what); map_prev=m; } }
    if(trace<0){ const char *e=getenv("KHNMC_ROUTE_TRACE"); trace=e&&!strcmp(e,"1")?1:e&&!strcmp(e,"2")?2:0; }
    if(!trace) return;
    if(trace==2){ trace2(g,n); trace_objs(g,n); trace_carry(g); }
    char cur[1200]; kh_route_state_summary(g,n,cur,sizeof cur);
    char *h=strstr(cur," hits="); if(h) *h=0;      /* the hit counters change with every hit: only the screens decide a trace line */
    { char *p=strstr(cur," player="); char *q=p?strstr(p," command-menu="):NULL; if(p&&q) memmove(p,q,strlen(q)+1); }   /* nor HP / MP */
    if(strcmp(cur,trace_prev)){ char full[1200]; kh_route_state_summary(g,n,full,sizeof full); fprintf(stderr,"kh: route trace game-frame=%llu %s\n",(unsigned long long)n,full); strcpy(trace_prev,cur); }
}
void kh_route_state_test_reset(void){ fc_prev=kh_phase_field_cmds(); fc_frame=0; }     /* selftest only */
void kh_route_state_test_set_field_cmds(u64 per_frame){ fc_frame=per_frame; }          /* selftest only */
