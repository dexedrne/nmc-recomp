#include <math.h>
#include "xion_swap.h"
#include "autosave.h"
#include "kh_look.h"
#include "rewind.h"
#include "route_state.h"

#define VA(rva) (GS_BASE+(rva))
#define ACTIVE VA(0x2a105d0)
#define PARKED VA(0x2a11268)
#define FC VA(0x2a10620)
#define MICKEY VA(0x7523b8)

_Atomic int kh_xion_enabled;
int kh_xkb_menu;
static u64 messages[6];
static const char *const lines[]={
    "Standard","Critical","Xion Knows Best","Play as Xion.\nRoxas steps in when needed.",
    "Roxas steps in","Xion returns"
};
static const u16 refs[]={30000,30001,20019,30002,17205,17206,20020,30003};

u64 kh_xkb_message(int id){
    unsigned index=(id&0x7fff)-30000u;
    return kh_xkb_menu && index<4 ? messages[index] : 0;
}

static void prepare_menu(KCtx *g){
    if(!messages[0]){
        for(unsigned i=0;i<6;i++){
            const char *src=lines[i];
            messages[i]=ga_alloc(strlen(src)+1,16);
            u8 *dst=GUEST_PTR(messages[i]);
            while(*src){
                char c=*src++;
                if(c>='a'&&c<='z') *dst++=0x9a+c-'a';
                else if(c>='A'&&c<='Z') *dst++=0x2e + c-'A';
                else if(c==' ') *dst++=1;
                else if(c=='\n') *dst++=2;
                else if(c=='.') *dst++=0x4f;   /* 0x4f = '.'; 0x4e is the Japanese full stop (OpenKH InternationalSystemDecode) */
                else kh_fatal(g,"xkb-message-encoding");
            }
            *dst=0;
        }
    }
    for(unsigned i=0;i<8;i++) wr32(g,VA(0x5b870c)+4*i,0x8000|refs[i]);
}
#define MENU_INIT(r) void orig_f_##r(KCtx *); void f_##r(KCtx *g){ \
    if(kh_xkb_menu) prepare_menu(g); orig_f_##r(g); if(kh_xkb_menu) wr32(g,VA(0xb1d810),0); }
MENU_INIT(2b84f0)
MENU_INIT(2b8520)

void orig_f_2b88c0(KCtx *);
void f_2b88c0(KCtx *g){
    if(!kh_xkb_menu){orig_f_2b88c0(g);return;}
    u32 index=rd32(g,VA(0xb1d810));
    if(index>=4) kh_fatal(g,"xkb-menu-choice: %u",index);
    kh_xkb_set_mode(g,index==3);
    wr32(g,VA(0xb1d810),index==3?1:index+1);
    orig_f_2b88c0(g);
    wr32(g,VA(0xb1d810),index);
    kh_warn("xkb-choice: row %u level %u XKB %d",index,rd32(g,VA(0x9abd48)),kh_xion_enabled);
}

static int declined,save_interaction,death_pending;
static u64 xion,roxas,command_actor,command_place;
static u16 command_evt;
static u64 retiring;
static s32 sheet_gauges[5];
static s32 entry_hp,entry_mp;

/* Guest actor addresses and gauge backups belong to the same cut as the player sheets. */
void kh_rewind_xion(KhRecords *r){
    extern void kh_rewind_xkb_save(KhRecords *);
    kh_rewind_xkb_save(r);
    KH_RECORD(r,declined);
    int enabled=kh_xion_enabled;
    KH_VALUE(r,enabled);
    if(r->loading) kh_xion_enabled=enabled;
    KH_RECORD(r,messages);
    KH_RECORD(r,save_interaction);
    KH_RECORD(r,death_pending);
    KH_RECORD(r,xion);
    KH_RECORD(r,roxas);
    KH_RECORD(r,command_actor);
    KH_RECORD(r,command_place);
    KH_RECORD(r,command_evt);
    KH_RECORD(r,retiring);
    KH_RECORD(r,sheet_gauges);
    KH_RECORD(r,entry_hp);
    KH_RECORD(r,entry_mp);
}

void kh_xion_init(void){
    const char *e=getenv("KHNMC_XKB_MENU");
    if(e&&*e&&strcmp(e,"0")&&strcmp(e,"1")) kh_fatal(NULL,"xkb-menu-mode: expected 0 or 1");
    kh_xkb_menu=e&&!strcmp(e,"1");
}

static int field_supported(int world,int area,int map,int btl,int evt){
    static const struct { u8 world,area; u16 map,btl,evt; } fields[] = {
#include "xion_fields.inc"
    };
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
        if(fields[i].world==world&&fields[i].area==area&&fields[i].map==map&&
           fields[i].btl==btl&&fields[i].evt==evt) return 1;
    return 0;
}

static int supported(KCtx *g,int loading){
    int map,btl,evt;
    if(!kh_route_programs(g,NULL,&map,&btl,&evt)) return 0;
    int world=kh_route_world(g),area=kh_route_area(g);
    int allowed=field_supported(world,area,map,btl,evt);
    if(!allowed&&loading&&kh_xion_enabled&&world==8&&area==9&&map==0&&btl==10&&evt==0)
        kh_warn("xion-swap-declined: mu09 0/10/0 retains Roxas: P_EX220 ptya table 28 lacks key 1");
    return allowed;
}

void kh_xion_command_done(KCtx *g,u64 controller,u64 actor){
    if(controller!=FC||rd64(g,ACTIVE)!=actor||rd64(g,FC+0xc38)!=actor) return;
    command_actor=actor;
    command_place=rd64(g,VA(0x717008));
    command_evt=rd16(g,VA(0x717010));
}

// Keep an unchanged resource exact across a round trip, including 1 HP on unlike sheets.
static s32 fraction(s32 value,s32 maximum,s32 destmax){
    if(value<=0||maximum<=0||destmax<=0) return 0;
    if(value>=maximum) return destmax;
    s32 scaled=(s32)((s64)value*destmax/maximum);
    return scaled?scaled:1;
}
static s32 carry_back(s32 value,s32 entry,s32 original,s32 maximum,s32 destmax){
    return value==entry?original:fraction(value,maximum,destmax);
}
/* Save-point recovery addresses the canonical party. Heal the playable sheet
 * so damage and recovery are ordered on the same gauge throughout the swap. */
u64 kh_xion_recovery_sheet(KCtx *g,u64 sheet){
    if(xion && sheet==rd64(g,roxas+0x5c0)) return rd64(g,xion+0x5c0);
    return sheet;
}
static s32 return_gauge(KCtx *g,u64 live,u64 parked,s32 entry){
    return carry_back(rd32(g,live),entry,rd32(g,parked),rd32(g,live+4),rd32(g,parked+4));
}
static u64 call(KCtx *g,u32 r,u64 a,u64 b){ return kh_call(g,VA(r),a,b,0,0); }

/* f_180900 follows HUD slot zero to actor+0x5c0 on its next update. */
static void gauge_owner(KCtx *g,u64 actor){
    call(g,0x168850,0,actor);
    if(call(g,0x1689a0,0,0)!=actor) kh_fatal(g,"xion-swap-hud-owner");
}

static void actor_method(KCtx *g,u64 actor,u32 slot){
    u64 desc=call(g,0x4ad270,rd32(g,actor),0);
    kh_call(g,rd64(g,rd64(g,desc)+slot),desc,actor,0,0);
}

static void handback(KCtx *g,const char *reason){
    command_actor=0;
    if(!xion) return;
    int lethal=death_pending; death_pending=0;
    if(rd64(g,ACTIVE)!=xion||rd64(g,PARKED)!=roxas)
        kh_fatal(g,"xion-swap-ownership: player changed before %s",reason);
    u64 xs=rd64(g,xion+0x5c0),rs=rd64(g,roxas+0x5c0);
    s32 hp=return_gauge(g,xs,rs,entry_hp);
    s32 mp=return_gauge(g,xs+0x180,rs+0x180,entry_mp);
    u64 pos=call(g,0x3b5b40,xion,0);
    call(g,0x3b64d0,roxas,pos);
    KH_XMM(g,1)=VZ; u32 yaw=rd32(g,xion+0x68c); memcpy(&KH_XMM(g,1),&yaw,4);
    call(g,0x3b6560,roxas,0);
    // Native release writes these six bytes back into character 4's save record.
    wr32(g,xs,sheet_gauges[0]); wr32(g,xs+4,sheet_gauges[1]); wr32(g,xs+8,sheet_gauges[2]);
    wr32(g,xs+0x180,sheet_gauges[3]); wr32(g,xs+0x184,sheet_gauges[4]);
    // Weapon and effect children must leave the native parent list before their owner retires.
    while(rd64(g,xion+0x8b8)){
        u64 attachment=rd64(g,xion+0x8b8);
        u64 child=attachment-0x7b0;
        if(call(g,0x4ad270,rd32(g,child+0x6a0),0)!=xion)
            kh_fatal(g,"xion-swap-child: attachment does not belong to the retiring actor");
        call(g,0x3da250,child,0);
        if(rd64(g,xion+0x8b8)==attachment)
            kh_fatal(g,"xion-swap-detach: native detach left the same child");
    }
    actor_method(g,roxas,0x88);
    call(g,0x4151e0,MICKEY,xion); // descriptor slot +0x38, also pops the special command list
    retiring=xion;
    call(g,0x3a8140,0,0);
    call(g,0x3bd2e0,xion,roxas);
    gauge_owner(g,roxas);
    wr32(g,rs,hp); wr32(g,rs+0x180,mp);
    call(g,0x3c2120,rs,0);
    if(rd64(g,ACTIVE)!=roxas||rd64(g,PARKED)||rd64(g,FC+0xae0)!=roxas)
        kh_fatal(g,"xion-swap-restore: controller did not return to Roxas at %s",reason);
    kh_warn("xion-swap: Roxas (%s), HP %d MP %d, save EXP %u munny %u",reason,hp,mp,rd32(g,VA(0x9acf90)),rd32(g,VA(0x9abcf0)));
    wr32(g,xion+0x120,rd32(g,xion+0x120)|(1u<<19)); // native end-of-frame deletion queue
    u64 player=roxas;
    xion=roxas=0;
    if(messages[4]) call(g,0x17b710,messages[4],0);
    if(lethal){
        u64 desc=call(g,0x4ad270,rd32(g,player),0);
        kh_call(g,rd64(g,rd64(g,desc)+0xb0),desc,player,0,0);
    }
}

static void enter(KCtx *g,const KhPlayerSheet *p){
    u64 row=call(g,0x3e2e40,0,0);
    if(!row||rd16(g,row+0x24)!=792||rd64(g,PARKED)||p->chr!=1||p->hp<=0) return;
    u64 ent=call(g,0x3dfeb0,792,0);
    if(!ent||rd16(g,ent+0x4c)!=4){ declined=1; kh_warn("xion-swap-objentry: missing sheet 4, keeping Roxas"); return; }
    u32 ready=call(g,0x3e0160,792,ent);
    if(!ready||(ready&~12u)){ kh_warn("xion-swap-resources: cache status %#x, keeping Roxas",ready); declined=1; return; }
    u64 pos=call(g,0x3b5b40,p->obj,0);
    if(!pos||!rd32(g,VA(0x2a23950))){
        kh_warn("xion-swap-preflight: missing position or free character sheet, keeping Roxas"); declined=1; return;
    }
    u64 actor=call(g,0x152430,0xe60,0);
    if(!actor){ kh_warn("xion-swap-allocation: keeping Roxas"); declined=1; return; }
    roxas=p->obj;
    call(g,0x3a80e0,0,0);
    actor_method(g,roxas,0x80); // trap_obj_hide: attachments still need their owner in the object list
    wr32(g,roxas+0x9b8,rd32(g,roxas+0x9b8)|4); // action/damage gate cleared by 3a8178 on restore
    u32 yaw=rd32(g,roxas+0x68c);
    // Sixth Win64 argument, immediately above kh_call5's fifth argument.
    wr64(g,((KH_RSP(g)-0x100)&~15ull)+0x28,yaw);
    kh_call5(g,VA(0x3a7a40),actor,ent,11,rd64(g,roxas+0xdb8),pos);
    wr64(g,actor+0xe08,0); wr8(g,actor+0xe10,0); wr64(g,actor+0xe18,0); wr64(g,actor+0xe50,0);
    wr32(g,actor,call(g,0x4ad240,MICKEY,0));
    wr32(g,actor+0x6c8,rd32(g,actor+0x6c8)|0x2000000);
    u64 sheet=rd64(g,actor+0x5c0);
    if(!sheet||rd64(g,ACTIVE)!=actor||rd64(g,FC+0xae0)!=actor||rd64(g,FC+0xc38)!=actor){
        actor_method(g,roxas,0x88);
        call(g,0x4151e0,MICKEY,actor); retiring=actor; wr32(g,actor+0x120,rd32(g,actor+0x120)|(1u<<19)); call(g,0x3a8140,0,0);
        roxas=0; declined=1; kh_warn("xion-swap-constructor: rolled back to Roxas"); return;
    }
    sheet_gauges[0]=rd32(g,sheet); sheet_gauges[1]=rd32(g,sheet+4); sheet_gauges[2]=rd32(g,sheet+8);
    sheet_gauges[3]=rd32(g,sheet+0x180); sheet_gauges[4]=rd32(g,sheet+0x184);
    if(!sheet_gauges[1]) wr32(g,sheet+4,p->hp_max);
    if(!sheet_gauges[4]) wr32(g,sheet+0x184,p->mp_max);
    entry_hp=fraction(p->hp,p->hp_max,rd32(g,sheet+4)); entry_mp=fraction(p->mp,p->mp_max,rd32(g,sheet+0x184));
    wr32(g,sheet,entry_hp); wr32(g,sheet+0x180,entry_mp);
    wr32(g,sheet+8,0); // permit genuine KO, the zero-result callback is MICKEY vfn22
    call(g,0x3bd2e0,roxas,actor);
    gauge_owner(g,actor);
    xion=actor;
    if(messages[5]) call(g,0x17b710,messages[5],0);
    kh_warn("xion-swap: Xion object 792, HP %d/%d MP %d/%d",entry_hp,rd32(g,sheet+4),entry_mp,rd32(g,sheet+0x184));
}

void kh_xion_frame(KCtx *g,u16 buttons){
    if(death_pending){ KCtx ctx=*g; handback(&ctx,"KO"); return; }
    u64 actor=command_actor; command_actor=0;
    if(!actor||rd64(g,ACTIVE)!=actor||rd64(g,FC+0xae0)!=actor||rd64(g,FC+0xc38)!=actor||
       command_place!=rd64(g,VA(0x717008))||command_evt!=rd16(g,VA(0x717010))||
       rd8(g,VA(0x9006b0))||rd64(g,VA(0x2a11478))) return;
    KCtx ctx=*g; // injected calls must preserve the interrupted frame's entire register state
    if(kh_route_world(g)==2&&kh_route_area(g)==7){
        // TT07_MS101 starts its scene from an area trigger around the friends at the armor shop.
        float x,y,z,yaw;
        if(!kh_route_player_pos(g,&x,&y,&z,&yaw)||hypotf(x+1297,z+2897)<950){
            handback(&ctx,"armor shop scene approach"); return;
        }
    }
    if(xion&&(buttons&0x8000)&&kh_route_at_save_point(g)){
        save_interaction=1; handback(&ctx,"save interaction"); return;
    }
    if(save_interaction){
        if(kh_route_at_save_point(g)) return;
        save_interaction=0;
    }
    if(!declined&&!xion&&!retiring&&!rd64(g,VA(0x9ba928))&&!rd8(g,VA(0x9ba8d1))&&supported(g,0)&&!rd16(g,VA(0x9acdd4))){
        if(kh_route_mission(g)){
            KhObjective o;
            if(kh_route_world(g)!=2||kh_route_area(g)!=7||!kh_route_objective(g,&o)||o.id!=0xa7) return;
        }
        KhPlayerSheet p;
        if(kh_route_player_sheet(g,&p)&&p.obj==actor) enter(&ctx,&p);
    }
}

void orig_f_3e2eb0(KCtx *);
void f_3e2eb0(KCtx *g){
    orig_f_3e2eb0(g);
    if(kh_xion_enabled) declined=0;
    if(kh_xion_enabled&&supported(g,1)){
        KCtx ctx=*g;
        kh_call(&ctx,VA(0x3e0460),792,3000,7,0); // normal area cache lifetime, without flushing Drive group 5000
    }
}

#define BOUNDARY(r,why,reset) void orig_f_##r(KCtx *); void f_##r(KCtx *g){ \
    if(kh_xion_enabled){ KCtx ctx=*g; handback(&ctx,why); if(reset) declined=1; } orig_f_##r(g); }
BOUNDARY(3ac350,"event",0)
BOUNDARY(3a24c0,"area program",0)
BOUNDARY(152990,"area departure",0)
void orig_f_2ed110(KCtx *);
void f_2ed110(KCtx *g){
    if(kh_xion_enabled){ KCtx ctx=*g; handback(&ctx,"save"); }
    u64 dst=A0(g)?A0(g):rd64(g,VA(0xbee360));
    orig_f_2ed110(g);
    if(kh_look_enabled()) kh_xion_save_portrait(g,dst);
    kh_xkb_serialized();
}
void kh_xion_reset(KCtx *g){
    KCtx ctx=*g; handback(&ctx,"session reset"); kh_xkb_set_mode(g,0); declined=save_interaction=0;
}
void orig_f_2ed0e0(KCtx *);
void f_2ed0e0(KCtx *g){
    if(kh_xion_enabled) kh_xion_reset(g);
    int menu_load=rd64(g,KH_RSP(g))==VA(0x2e59b6);
    orig_f_2ed0e0(g);
    if(menu_load){
        if(kh_xkb_menu) kh_xkb_load(g);
        kh_autosave_load(g);
        if(kh_xion_enabled) prepare_menu(g);
    }
    if(kh_xion_enabled) kh_warn("xion-swap: loaded Roxas, save EXP %u munny %u",rd32(g,VA(0x9acf90)),rd32(g,VA(0x9abcf0)));
}
BOUNDARY(2e4950,"system menu",0)
void orig_f_3ad540(KCtx *);
void f_3ad540(KCtx *g){
    if(xion&&rd64(g,KH_RSP(g))==VA(0x157304)){ KCtx ctx=*g; handback(&ctx,"START"); }
    orig_f_3ad540(g);
}

void orig_f_414e30(KCtx *);
void f_414e30(KCtx *g){
    if(!xion||A1(g)!=xion){ orig_f_414e30(g); return; }
    call(g,0x3a8e70,xion,0); call(g,0x3db140,xion,1); KH_RET(g);
}
#define UPDATE(r,shared) void orig_f_##r(KCtx *); void f_##r(KCtx *g){ \
    if(!xion||A1(g)!=xion){ orig_f_##r(g); return; } call(g,shared,xion,0); KH_RET(g); }
UPDATE(414eb0,0x3a92c0)
UPDATE(4150e0,0x3a89a0)

#define RETURN(r) void orig_f_##r(KCtx *); void f_##r(KCtx *g){ \
    if(!xion||A0(g)!=xion){ orig_f_##r(g); return; } declined=1; handback(g,"rescue command"); KH_RET(g); }
RETURN(416030)
RETURN(416100)

void orig_f_415380(KCtx *);
void f_415380(KCtx *g){
    if(!xion||A1(g)!=xion){ orig_f_415380(g); return; }
    // Finish the damage callback and its callers before detaching their live victim.
    declined=1; death_pending=1; KH_RET(g);
}

void orig_f_4151e0(KCtx *);
void f_4151e0(KCtx *g){
    // The deletion queue can dispatch slot 7 again after the explicit controller handback.
    if(retiring&&A1(g)==retiring){ KH_RET(g); return; }
    orig_f_4151e0(g);
}

void orig_f_414dd0(KCtx *);
void f_414dd0(KCtx *g){
    u64 actor=A1(g);
    if(!retiring||actor!=retiring){ orig_f_414dd0(g); return; }
    retiring=0;
    // Neutral instance owns no rescue effects, field/menu bits or partner respawn.
    call(g,0x2e2d60,actor,0); call(g,0x152570,actor,0); KH_RET(g);
}

int kh_xion_selftest(KCtx *g){
    int failures=0;
    u64 live=ga_alloc(8,8), parked=ga_alloc(8,8);
    wr32(g,live,2); wr32(g,live+4,20); wr32(g,parked,7); wr32(g,parked+4,60);
    failures+=return_gauge(g,live,parked,2)!=7;
    wr32(g,parked,60); /* save point healed the canonical sheet */
    failures+=return_gauge(g,live,parked,2)!=60;
    u64 saved_xion=xion,saved_roxas=roxas;
    xion=ga_alloc(0x5c8,8); roxas=ga_alloc(0x5c8,8);
    wr64(g,xion+0x5c0,live); wr64(g,roxas+0x5c0,parked);
    wr32(g,parked,7); wr32(g,live,1); /* Xion took damage since the swap */
    u64 healed=kh_xion_recovery_sheet(g,parked);
    wr32(g,healed,rd32(g,healed+4));
    failures+=return_gauge(g,live,parked,2)!=60;
    ga_free(roxas); ga_free(xion); xion=saved_xion; roxas=saved_roxas;
    ga_free(parked); ga_free(live);
    failures+=fraction(0,20,60)!=0;
    failures+=fraction(7,20,60)!=21;
    failures+=fraction(1,60,20)!=1;
    failures+=fraction(65,60,20)!=20;
    failures+=fraction(7,0,20)!=0;
    failures+=carry_back(2,2,7,20,60)!=7;
    failures+=carry_back(1,2,7,20,60)!=3;
    failures+=carry_back(0,2,7,20,60)!=0;
    fprintf(stderr,"kh: xion-swap fraction selftest: 11 checks, %d failures\n",failures);
    int fields=0;
    fields+=!field_supported(2,2,1,0,0)||!field_supported(4,8,0,1,0)||!field_supported(2,7,95,95,95);
    fields+=!field_supported(16,1,5,1,0);
    fields+=field_supported(16,1,5,1,0xffff)||field_supported(15,0,0,0,0);
    fields+=field_supported(2,7,95,95,96);
    fields+=field_supported(8,9,0,10,0);  // P_EX220 lacks the ptya row used in this battle
    fields+=field_supported(5,7,0,0,0);   // chest interaction
    fields+=field_supported(7,5,0,0,0);   // carpet traversal
    fields+=field_supported(1,0,0,0,0);   // ending transit
    fprintf(stderr,"kh: xion field classification: %d failures\n",fields);
    int prior=kh_xion_enabled;
    for(int mode=0;mode<2;mode++){
        KhRecords cut={0};
        kh_xion_enabled=mode;
        kh_records_begin(&cut,0); kh_rewind_xion(&cut); kh_records_end(&cut);
        kh_xion_enabled=!mode;
        kh_records_begin(&cut,1); kh_rewind_xion(&cut); kh_records_end(&cut);
        failures+=kh_xion_enabled!=mode;
        kh_records_free(&cut);
    }
    kh_xion_enabled=prior;
    extern int kh_xkb_save_selftest(KCtx *);
    return failures+fields+kh_xkb_save_selftest(g);
}

u64 kh_xion_progress_player(void){ return xion?roxas:0; }

// The native picker has already checked the orb, proximity and recipient before this call.
void orig_f_3dca50(KCtx *);
void f_3dca50(KCtx *g){
    if(!xion||A1(g)!=xion){ orig_f_3dca50(g); return; }
    unsigned kind=rd8(g,A2(g)+0x14);
    orig_f_3dca50(g);
    kh_warn("xion-swap: collected native prize type %u, save munny %u",kind,rd32(g,VA(0x9abcf0)));
}

void orig_f_3ed110(KCtx *);
void f_3ed110(KCtx *g){
    if(!xion){ orig_f_3ed110(g); return; }
    int award=0;
    if(!(u8)call(g,0x3a3c40,0,0)){
        u32 leader=call(g,0x3e2e60,0,0);
        if((u8)call(g,0x3ba720,roxas,0)&&call(g,0x3b5b20,roxas,0)==leader){
            u64 abilities=call(g,0x3d7470,roxas,0);
            award=(rd32(g,abilities+12)&(1u<<20))==0;
        }
    }
    SET_RAX(g,award); KH_RET(g);
}

void orig_f_3d5c70(KCtx *);
void f_3d5c70(KCtx *g){
    if(xion&&(u32)A0(g)==1){ SET_RAX(g,roxas); KH_RET(g); return; }
    orig_f_3d5c70(g);
}
