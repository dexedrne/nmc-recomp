/* platform_seam_store.c : THE Steam platform seam (single file, by owner decision 2026-11-16).
 *
 * Purpose: the iOS port has no Steam.  This file is the runtime's own implementation of the 9 imports of steam_api64.dll and of the
 * handful of Steamworks interface methods the game calls.  For the owner's personal build it reports "initialised" (Steam running, app
 * installed, English, a fixed SteamID64, no controllers, no achievements) so the game's own startup flow proceeds.
 * What it is not: it does not patch, hook or modify the game executable (the image is mapped exactly as shipped); it does not analyse or
 * touch Steam's DRM; it is not part of the exe.  It exists only inside our runtime, for personal use; the original exe is never
 * redistributed.  Replacing this file with real Steamworks (or with an Epic/own-storefront backend) requires no other change.
 *
 * Facts used (docs/platform/04-STEAM-FACTS.md): SteamAPI_RestartAppIfNecessary + SteamAPI_Init are called from 0x14d610; the interface
 * context blocks are { init fn, counter, interface pointers... } and are filled by SteamInternal_ContextInit(block) which runs the init fn
 * (it calls SteamAPI_GetHSteamUser + SteamInternal_FindOrCreateUserInterface(version string)).
 */
#include "khnmc_rt.h"

#define STEAM_APPID 2552430
#define STEAM_FAKE_ID64 0x0110000100000001ull      /* universe 1 (public), type 1 (individual), instance 1, account 1 */

/* interface class ids */
static int c_user, c_friends, c_utils, c_remote, c_stats, c_input, c_apps, c_generic;
static u64 o_user, o_friends, o_utils, o_remote, o_stats, o_input, o_apps, o_generic;
static u64 str_english;
u64 kh_steam_objs[9];

static int steam_h(KCtx *g,u64 obj,int slot,const char *cls){
    (void)obj;
    if(!strcmp(cls,"steam:ISteamUser023")){ if(slot==2){ wr64(g,A1(g),STEAM_FAKE_ID64); SET_RAX(g,A1(g)); return 1; } }   /* GetSteamID: CSteamID is returned through a hidden pointer (rdx), rax = that pointer */
    else if(!strcmp(cls,"steam:ISteamApps008")){ if(slot==4){ SET_RAX(g,str_english); return 1; } }               /* GetCurrentGameLanguage */
    else if(!strcmp(cls,"steam:ISteamInput006")){ if(slot==0){ SET_RAX(g,1); return 1; } }                          /* Init(bExplicitlyCallRunFrame) -> true */
    else if(!strcmp(cls,"steam:ISteamUserStats012")){ if(slot==0){ SET_RAX(g,1); return 1; } }                      /* RequestCurrentStats -> true (no callback is delivered) */
    else if(!strcmp(cls,"steam:ISteamFriends017")){ if(slot==43){ SET_RAX(g,1); return 1; } }                       /* SetRichPresence -> true */
    return 0;                                                                                                       /* everything else: 0 / false */
}
void kh_seam_steam_init(void){
    c_user=kh_com_class("steam:ISteamUser023",64,0,steam_h);       c_friends=kh_com_class("steam:ISteamFriends017",96,0,steam_h);
    c_utils=kh_com_class("steam:ISteamUtils010",64,0,steam_h);     c_remote=kh_com_class("steam:ISteamRemoteStorage016",96,0,steam_h);
    c_stats=kh_com_class("steam:ISteamUserStats012",96,0,steam_h); c_input=kh_com_class("steam:ISteamInput006",96,0,steam_h);
    c_apps=kh_com_class("steam:ISteamApps008",48,0,steam_h);       c_generic=kh_com_class("steam:generic",64,0,steam_h);
    o_user=kh_com_obj(c_user,0); o_friends=kh_com_obj(c_friends,0); o_utils=kh_com_obj(c_utils,0); o_remote=kh_com_obj(c_remote,0);
    o_stats=kh_com_obj(c_stats,0); o_input=kh_com_obj(c_input,0); o_apps=kh_com_obj(c_apps,0); o_generic=kh_com_obj(c_generic,0);
    str_english=ga_strdup("english");
    kh_steam_objs[0]=o_user; kh_steam_objs[1]=o_friends; kh_steam_objs[2]=o_utils; kh_steam_objs[3]=o_remote; kh_steam_objs[4]=o_stats; kh_steam_objs[5]=o_input; kh_steam_objs[6]=o_apps; kh_steam_objs[7]=o_generic; kh_steam_objs[8]=str_english;
}

IMP(steam_api64,SteamAPI_RestartAppIfNecessary){ kh_cnt_steam_calls++; SET_RAX(g,0); }       /* false: no relaunch through Steam needed */
IMP(steam_api64,SteamAPI_Init){ kh_cnt_steam_calls++; SET_RAX(g,1); }                         /* true: "initialised" */
IMP(steam_api64,SteamAPI_Shutdown){ kh_cnt_steam_calls++; }
IMP(steam_api64,SteamAPI_RunCallbacks){ kh_cnt_steam_calls++; }
IMP(steam_api64,SteamAPI_RegisterCallback){ kh_cnt_steam_calls++; }
IMP(steam_api64,SteamAPI_UnregisterCallback){ kh_cnt_steam_calls++; }
IMP(steam_api64,SteamAPI_GetHSteamUser){ kh_cnt_steam_calls++; SET_RAX(g,1); }
IMP(steam_api64,SteamInternal_FindOrCreateUserInterface){ kh_cnt_steam_calls++; const char *v=gstr(g,A1(g)); u64 o;
    if(!strncmp(v,"SteamUser",9)) o=o_user; else if(!strncmp(v,"SteamFriends",12)) o=o_friends; else if(!strncmp(v,"SteamUtils",10)) o=o_utils;
    else if(!strncmp(v,"STEAMREMOTESTORAGE",18)) o=o_remote; else if(!strncmp(v,"STEAMUSERSTATS",14)) o=o_stats; else if(!strncmp(v,"SteamInput",10)) o=o_input;
    else if(!strncmp(v,"STEAMAPPS",9)) o=o_apps; else { kh_warn("steam seam: unknown interface version '%s' -> generic null interface",v); o=o_generic; }
    SET_RAX(g,o); }
IMP(steam_api64,SteamInternal_ContextInit){ kh_cnt_steam_calls++; u64 blk=A0(g); u64 ctr=rd64(g,blk+8);
    if(ctr!=1){ u64 fn=rd64(g,blk); if(fn) kh_call(g,fn,blk+16,0,0,0); wr64(g,blk+8,1); }
    SET_RAX(g,blk+16); }
