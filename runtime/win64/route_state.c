/* route_state.c : the game states a scripted route waits for (Task 27).  Every condition is read from the game's own memory or from
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
 * KHNMC_ROUTE_TRACE=1 prints the whole state (kh_route_state_summary) whenever it changes: the tool used to find the values above. */
#include "route_state.h"
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
int kh_route_cursor(KCtx *g,int kind){
    if(kind==KR_TITLE_CHOICE) return title_menu(g)?(int)(s32)rd32(g,T_CURSOR):-1;
    if(kind==KR_NEWGAME_CHOICE) return newgame_choice(g);
    return -1;
}
int kh_route_cond_eval(KCtx *g,int kind,int arg,u64 n,u64 step_frame,u64 step_hits){
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
    case KR_HITS:           return kh_phase_damage_hits()-step_hits>=(u64)arg;
    case KR_MESSAGE:        return message(g);
    case KR_FRAMES:         return n-step_frame>=(u64)arg;
    }
    kh_fatal(g,"route-condition-unknown: condition kind %d",kind);
}
void kh_route_state_summary(KCtx *g,u64 n,char *out,size_t cap){
    int t,wt; windows(g,&t,&wt); u64 hits=kh_phase_damage_hits(); char last[24]="-";
    if(hits) snprintf(last,sizeof last,"%llu",(unsigned long long)kh_phase_damage_last_frame());
    snprintf(out,cap,"initial-state=%d%s title=%s mode=%d menu=%s cursor=%d screen=%d newgame=%s page=%d choice=%d game-level=%d world=%02x room=%02x "
             "field-cmds-this-frame=%llu field=%d windows=%d waiting=%d hits=%llu last-hit-frame=%s battle=%d",
             kh_phase_initial_state(),kh_phase_initial_alive()?"":"(gone)",kh_phase_title_alive()?"running":"not-running",(int)(s32)rd32(g,T_MODE),
             rd64(g,T_MENU)?"up":"none",(int)(s32)rd32(g,T_CURSOR),title_screen(g),kh_phase_newgame_alive()?"running":"not-running",newgame_page(g),newgame_choice(g),
             (int)(s32)rd32(g,GAME_LEVEL),rd8(g,NOW_WORLD),rd8(g,NOW_WORLD+1),(unsigned long long)fc_frame,field(g),t,wt,(unsigned long long)hits,last,battle(n));
}
static int trace=-1; static char trace_prev[600];
void kh_route_state_tick(KCtx *g,u64 n){
    u64 fc=kh_phase_field_cmds(); fc_frame=fc-fc_prev; fc_prev=fc;
    if(trace<0){ const char *e=getenv("KHNMC_ROUTE_TRACE"); trace=e&&!strcmp(e,"1"); }
    if(!trace) return;
    char cur[600]; kh_route_state_summary(g,n,cur,sizeof cur);
    char *h=strstr(cur," hits="); if(h) *h=0;      /* the hit counters change with every hit: only the screens decide a trace line */
    if(strcmp(cur,trace_prev)){ kh_route_state_summary(g,n,trace_prev,sizeof trace_prev); fprintf(stderr,"kh: route trace game-frame=%llu %s\n",(unsigned long long)n,trace_prev); strcpy(trace_prev,cur); }
}
void kh_route_state_test_reset(void){ fc_prev=kh_phase_field_cmds(); fc_frame=0; }     /* selftest only */
void kh_route_state_test_set_field_cmds(u64 per_frame){ fc_frame=per_frame; }          /* selftest only */
