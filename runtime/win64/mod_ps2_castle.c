#include "rmc.h"
/* Optional V0.20 encounter rules. Current-place fields come from f_152be0.
 * All addresses here refer to the PC image; EE addresses are never dereferenced. */
#include "khnmc_rt.h"
#include "route_state.h"
#include <string.h>

enum CastleTable { ATTACK, APPROACH, COMMAND, OBJENTRY };
struct TableEdit {
    enum CastleTable table;
    unsigned offset, width, original, value;
};
#define ATK(row, off, width, pc, ps2) {ATTACK, 8+48*(row)+(off), width, pc, ps2}
#define HEIGHT(row, pc) {APPROACH, 0x124+68*(row)+40, 4, pc, 0xc3480000u}
static const struct TableEdit edits[] = {
    {OBJENTRY,0x21b4,4,1003,3000}, {OBJENTRY,0x7a34,4,1003,3000},
    HEIGHT(0,0xc37a0000u), HEIGHT(1,0xc3af0000u), HEIGHT(32,0xc37a0000u),
    {COMMAND,8+48*59+2,1,72,0},
    ATK(2499,10,1,11,8), ATK(2499,12,2,45,100),
    ATK(2500,10,1,3,8), ATK(2500,12,2,100,100),
    ATK(2501,10,1,11,8), ATK(2501,12,2,30,100),
    ATK(2127,10,1,11,10), ATK(2127,12,2,15,0xff9c),
    ATK(2128,10,1,11,10), ATK(2128,12,2,15,0xff9c),
    ATK(621,10,1,9,10), ATK(621,12,2,30,0xfe0c),
    ATK(644,10,1,9,10), ATK(644,12,2,0xff9c,100), ATK(644,14,2,0xfff1,0),
    ATK(1587,10,1,11,10), ATK(1584,10,1,11,10),
    ATK(1943,10,1,11,10), ATK(1943,12,2,25,350),
    ATK(1944,10,1,11,10), ATK(1944,12,2,0xff9c,350),
    ATK(1945,10,1,11,10), ATK(1945,12,2,18,350),
    ATK(1946,10,1,11,10), ATK(1946,12,2,18,350),
    ATK(1947,10,1,11,10), ATK(1947,12,2,0xff9c,350),
    ATK(1948,10,1,11,10), ATK(1948,12,2,0xff9c,350)
};

static void combat_tables(KCtx *g, int here, int map, int battle, int event){
    static int previous;
    static u64 address[sizeof edits/sizeof edits[0]];
    static u32 saved[sizeof edits/sizeof edits[0]];
    int mask=0;
    if(here==0x1412 && map==74 && event==74) mask|=1<<ATTACK;
    if(here==0x0a12 && (battle==57 || battle==100) && battle==event) mask|=1<<APPROACH;
    if(here==0x1712 && map==73) mask|=1<<COMMAND;
    if(here==0x0212 && battle==10) mask|=1<<OBJENTRY;
    if(!(mask|previous)) return;
    const u64 slot[]={0x2ae57b0,0x2ae57b8,0x2a161e8,0x2a25030};
    const u32 count[]={2713,70,0,0};
    for(size_t i=0;i<sizeof edits/sizeof edits[0];i++){
        const struct TableEdit *e=edits+i;
        int bit=1<<e->table;
        if(!((mask|previous)&bit)) continue;
        u64 table=rd64(g,GS_BASE+slot[e->table]);
        if(!table || !GA_OK(table+7)) continue;
        if(count[e->table] && rd32(g,table+4)!=count[e->table])
            kh_fatal(g,"ps2-castle-table-layout: table %u count %u",e->table,rd32(g,table+4));
        if(e->table==OBJENTRY && (rd32(g,table+4)<=325 ||
           rd32(g,table+8+96*89)!=120 || rd32(g,table+8+96*325)!=442))
            kh_fatal(g,"ps2-castle-neostatus-row: expected ids 120 and 442");
        if(e->table==COMMAND && (rd32(g,table+4)<=59 || rd16(g,table+8+48*59)!=72))
            kh_fatal(g,"ps2-castle-summon-row: command 72 absent");
        u64 p=table+e->offset;
        if(!GA_OK(p+e->width-1)) kh_fatal(g,"ps2-castle-table-range: offset %u",e->offset);
        u32 current=e->width==1?rd8(g,p):e->width==2?rd16(g,p):rd32(g,p);
        u32 wanted;
        if(mask&bit){
            /* The PC controller itself changes ATKP during play. Keep the last native value. */
            if(address[i]!=p || current!=e->value) saved[i]=current;
            address[i]=p;
            wanted=e->value;
        }else{
            if(address[i]!=p || current!=e->value){ address[i]=0; continue; }
            address[i]=0;
            wanted=saved[i];
        }
        if(current==wanted) continue;
        if(e->width==1) wr8(g,p,wanted);
        else if(e->width==2) wr16(g,p,wanted);
        else wr32(g,p,wanted);
    }
    previous=mask;
}

static void encounter_stats(KCtx *g,int here,int battle){
    static u64 sheet, owner;
    static u16 original[3], last[3];
    int approach=here==0x0212 && battle>=10 && battle<=13;
    int finale=here==0x0312 && battle==65;
    if(!approach && !finale && !sheet) return;
    KhPlayerSheet player;
    if(!kh_route_player_sheet(g,&player) || player.obj!=owner || player.sheet!=sheet){
        sheet=owner=0;
        if(!(approach||finale) || !player.ok) return;
        sheet=player.sheet; owner=player.obj;
        for(int i=0;i<3;i++) original[i]=last[i]=rd16(g,sheet+0x188+2*i);
    }
    for(int i=0;i<3;i++){
        u64 p=sheet+0x188+2*i;
        u16 cur=rd16(g,p);
        if(cur!=last[i]) original[i]=cur;
        u16 next=finale?(i==2?255:100):(approach&&i==2)?40:original[i];
        if(cur!=next) wr16(g,p,next);
        last[i]=next;
    }
    if(!approach&&!finale) sheet=owner=0;
}

static u64 final_actor(KCtx *g,int index,const char *model){
    const u64 sheet=GS_BASE+0x2a17290+0x278u*index;
    u32 id=rd32(g,sheet+0x268);
    if(!(id&0x80000000u)) return 0;
    u64 obj=rd64(g,GS_BASE+0x2b0d720+8*((id>>25)&63))|(id&0x1ffffffu);
    char name[40];
    if(!kh_route_object_name(g,obj,name,sizeof name) || strcmp(model,name) ||
       rd64(g,obj+0x5c0)!=sheet || rd32(g,obj+4)!=id) return 0;
    return sheet;
}

void kh_ps2_castle_frame(KCtx *g){
    static int enabled=-1, one_hp;
    if(enabled<0){
        extern int kh_tweak_boolean(KCtx *,const char *,int);
        enabled=kh_tweak_boolean(g,"KHNMC_PS2_CASTLE",kh_rmc_find("rmc-package-1") != NULL);
        one_hp=kh_tweak_boolean(g,"KHNMC_PS2_FINAL_ONE_HP",enabled);
    }
    if(!enabled && !one_hp) return;
    u64 place=GS_BASE+0x717008;
    int here=rd16(g,place), map=rd16(g,place+4), battle=rd16(g,place+6), event=rd16(g,place+8);
    if(enabled && here==0x0e12 && !map && !battle && !event){
        wr16(g,place+4,58);
        wr16(g,place+6,58);
        wr16(g,place+8,58);
    }
    if(enabled){
        combat_tables(g,here,map,battle,event);
        encounter_stats(g,here,battle);
    }
    if(here!=0x1412 || (map!=74 && map!=98)) return;
    const char *name=map==74?"B_EX170_LAST":"B_EX170_LAST_LV99";
    u64 primary=final_actor(g,77,name), clone=final_actor(g,76,name);
    if(enabled && clone) wr32(g,clone+0x1a8,0x00323232u);
    if(primary){
        if(enabled) wr32(g,primary+0x1a8,0x00323264u);
        if(one_hp && rd32(g,primary)==1){
            wr32(g,primary,0);
            fprintf(stderr,"kh: ps2-castle: %s sheet 77 reached one HP; applied V0.20 finish\n",name);
        }
    }
}
