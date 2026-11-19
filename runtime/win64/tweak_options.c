#include "mod_config.h"
#include "rewind.h"
#include "xion_swap.h"
#include "mod_static_table.h"
#include <strings.h>

int kh_retry,kh_savepoint,kh_gummiskip;
static int shortcuts;
static int keep_inventory,keep_party,restore_party;
static unsigned char checkpoint[0x10fc0];
void kh_rewind_retry(KhRecords *r){
    if(!kh_retry) return;
    KH_VALUE(r,checkpoint);
    KH_VALUE(r,keep_inventory);
    KH_VALUE(r,keep_party);
    KH_VALUE(r,restore_party);
}

int kh_retry_rewind_selftest(void){
    KhRecords original={0}, cut={0};
    int enabled=kh_retry;
    kh_retry=1;
    kh_records_begin(&original,0); kh_rewind_retry(&original);
    memset(checkpoint,0x53,sizeof checkpoint);
    keep_inventory=1; keep_party=0; restore_party=1;
    kh_records_begin(&cut,0); kh_rewind_retry(&cut);
    memset(checkpoint,0xa7,sizeof checkpoint);
    keep_inventory=0; keep_party=1; restore_party=0;
    kh_records_begin(&cut,1); kh_rewind_retry(&cut); kh_records_end(&cut);
    int fail=keep_inventory!=1 || keep_party || restore_party!=1;
    for(size_t i=0;i<sizeof checkpoint;i++) fail|=checkpoint[i]!=0x53;
    kh_records_begin(&original,1); kh_rewind_retry(&original); kh_records_end(&original);
    kh_records_free(&original); kh_records_free(&cut); kh_retry=enabled;
    return fail;
}
#define EX(rva) (GS_BASE+(rva))
#define call(rva,a,b,c,d) kh_call(g,EX(rva),a,b,c,d)
#define RETRY_BACKUP EX(0x29fb580)

int kh_tweak_boolean(KCtx *g,const char *env,int fallback){
    const char *v=getenv(env);
    if(!v) return fallback==1;
    if(!strcasecmp(v,"on")||!strcasecmp(v,"true")||!strcmp(v,"1")) return 1;
    if(!strcasecmp(v,"off")||!strcasecmp(v,"false")||!strcmp(v,"0")) return 0;
    kh_fatal(g,"tweak-value: %s=%s (expected on or off)",env,v);
    return 0;
}

void orig_f_418f30(KCtx *g);
void f_418f30(KCtx *g){
    if(!kh_savepoint){ orig_f_418f30(g); return; }
    u64 self=A0(g),party=call(0x3e2e20,UINT32_MAX,0,0,0);
    for(int i=0;i<4;i++){
        u32 member=call(0x3e3680,party,i,0,0);
        if(!member) continue;
        u64 s=call(0x3c04c0,member,UINT32_MAX,0,0);
        if(!s) kh_fatal(g,"savepoint-party-sheet: member %u",member);
        s=kh_xion_recovery_sheet(g,s);
        if(rd32(g,s+0x268)){
            float recharge; memcpy(&recharge,hp(g,s+0x1c0),4);
            if((s32)rd32(g,s)<(s32)rd32(g,s+4) || recharge>0 ||
               (s32)rd32(g,s+0x180)<(s32)rd32(g,s+0x184) ||
               (rd8(g,s+0x1b1)<rd8(g,s+0x1b2) && rd8(g,s+0x1b0)!=100)){
                call(0x3c0c30,s,1,0,0); call(0x3c0cd0,s,1,0,0); call(0x3c0ad0,s,2,0,0);
                wr8(g,self+0xc08,1);
            }
        }else{
            call(0x3c2040,s,0,0,0); call(0x3c0860,s,rd32(g,s+4),0,0);
        }
        call(0x3c07e0,s,0,0,0);
    }
    if(rd8(g,self+0xc09)==1) wr16(g,self+0xc08,1);
    KH_RET(g);
}

void orig_f_3a0660(KCtx *g),orig_f_3fc7a0(KCtx *g),orig_f_3fcbe0(KCtx *g);
void orig_f_3fc350(KCtx *g),orig_f_3fc2d0(KCtx *g);
/* 11/19 merge with task 57: f_3a0660 is hooked once, in autosave.c, which calls this first (retry's checkpoint before the
 * Continue refresh), then the lifted body, then its own autosave write. */
void kh_retry_before_3a0660(KCtx *g){
    if(kh_retry && (u8)call(0x3a0540,0,0,0,0)){
        u64 save=call(0x152380,0,0,0,0);
        memcpy(checkpoint,hp(g,save),sizeof checkpoint);
    }
}
void f_3fc7a0(KCtx *g){
    if(kh_retry){
        int retry=(u32)call(0x3aba30,0,0,0,0)==2;
        u64 menu=call(0x3ad480,1,0,0,0);
        wr16(g,menu+2,retry?0x204:2);
        wr32(g,menu+12,retry?0x8ab100ff:0); wr32(g,menu+16,retry?0xd62e0100:0);
    }
    orig_f_3fc7a0(g);
}
void f_3fcbe0(KCtx *g){
    if(kh_retry){
        u32 cmd=A0(g);
        if(cmd==2) memcpy(hp(g,RETRY_BACKUP),checkpoint,sizeof checkpoint);
        if(cmd==0xff || cmd==0x100){
            u64 w=EX(0x717008),sp=KH_RSP(g),rec=sp-0x30;
            KH_RSP(g)=rec;
            wr32(g,rec,rd32(g,w)&0xffffff);
            for(int off=4;off<=8;off+=2) wr16(g,rec+off,rd8(g,w+off));
            kh_call5(g,EX(0x152990),rec,0x80000001,1,1,0);
            KH_RSP(g)=sp;
            if(cmd==0xff) keep_inventory=1; else keep_party=1;
        }
        if(cmd==0x101) wr8(g,EX(0xabac5a),1);
    }
    orig_f_3fcbe0(g);
}
/* DLL worker 32380 waits for the menu to close. Poll on the game thread so guest save writes stay ordered. */
void kh_retry_frame(KCtx *g){
    if(!restore_party || (u8)call(0x1515c0,0,0,0,0)) return;
    u64 save=call(0x152380,0,0,0,0);
    const struct {u32 offset,size;} parts[]={{0x24f0,0x1040},{0x3580,0x140},{0x36f8,8},{0xe000,0x28}};
    for(unsigned i=0;i<4;i++) memcpy(hp(g,RETRY_BACKUP+parts[i].offset),hp(g,save+parts[i].offset),parts[i].size);
    restore_party=0;
}
void f_3fc350(KCtx *g){
    if(kh_retry){
        u64 save=call(0x152380,0,0,0,0);
        if(keep_inventory) memcpy(hp(g,RETRY_BACKUP+16),hp(g,save+16),0x1c80);
        if(keep_party){call(0x2e4950,0,UINT32_MAX,0,0); restore_party=1;}
        if((u32)A0(g)==0xb9){call(0x3a1600,save+0x24f0,1,0x29,0); KH_RET(g); return;}
    }
    orig_f_3fc350(g);
}
void f_3fc2d0(KCtx *g){
    if(kh_retry) keep_inventory=keep_party=0;
    orig_f_3fc2d0(g);
}

struct kh_mod_difficulty_config kh_mod_difficulty_config[4] = {
    {50,150,1.0f,1.0f,1.0f},
    {100,100,1.0f,1.0f,1.0f},
    {200,125,1.5f,0.5f,0.5f},
    {200,125,1.5f,0.5f,0.5f},
};

#define KH_GROUP(key,env,label) int kh_##key=1;
#include "tweak_groups.h"
#undef KH_GROUP

static double configured(const char *name,double initial){
    const char *v=getenv(name);
    return v?strtod(v,NULL):initial;
}

void kh_tweak_static_options(KCtx *g,u64 nmc){
    struct group {const char *key,*env;int *on;} groups[]={
#define KH_GROUP(key,env,label) {#key,"KHNMC_TWEAK_" #env,&kh_##key},
#include "tweak_groups.h"
#undef KH_GROUP
    };
    for(unsigned j=0;j<sizeof groups/sizeof *groups;j++){
        struct group *group=&groups[j];
        *group->on=kh_tweak_boolean(g,group->env,1);
        if(*group->on) continue;
        for(int i=0;i<KH_MOD_STATIC_NEDITS;i++){
            const struct kh_mod_static_edit *e=&kh_mod_static_edits[i];
            if(!strcmp(e->group,group->key)) memcpy(hp(g,EX(e->rva)),e->old,e->len);
        }
    }
    /* Arrays are validated at startup, including component count and integer widths. */
    const char *forms[]={"VALOR","WISDOM","LIMIT","MASTER","FINAL"};
    for(int i=0;i<5;i++){
        char key[80];snprintf(key,sizeof key,"KHNMC_TWEAK_FORMS_%s_GROWTH",forms[i]);
        const char *p=getenv(key);
        if(p) for(int k=0;k<5;k++){
            char *end; wr16(g,EX(0x5c9030)+10*i+2*k,strtod(p,&end)); p=end+1;
        }
    }
    const char *camera[]={"OUTDOORS","INDOORS"};
    for(int i=0;i<2;i++){
        char key[80];snprintf(key,sizeof key,"KHNMC_TWEAK_CAMERA_CAMERA_PATCHES_%s",camera[i]);
        const char *p=getenv(key);
        if(p) for(int k=0;k<6;k++){
            char *end; float f=strtof(p,&end);
            memcpy(hp(g,EX(0x718c00)+24*i+4*k),&f,4);
            /* CRT initializer f_3e40 copies this source over the first indoor component. */
            if(i==1 && k==0) memcpy(hp(g,EX(0x718bdc)),&f,4);
            p=end+1;
        }
    }
    const char *symbols[]={"ZZ","ES","DI","PO","WI","TR"};
    for(int i=0;i<6;i++){
        char key[80];snprintf(key,sizeof key,"KHNMC_TWEAK_WORLDS_SYMBOL_%s",symbols[i]);
        const char *p=getenv(key);
        if(p) for(int k=0;k<5;k++){
            char *end; long num=strtod(p,&end); u64 row=nmc+0x70+i*8;
            if(k==0) wr32(g,row,num); else wr8(g,row+3+k,num);
            p=end+1;
        }
    }
    const char *gravity_keys[]={"BLOW_GRAVITY","GLOBAL_GRAVITY"};
    u64 gravity_va[]={nmc,EX(0x5ce184)};
    for(int i=0;i<2;i++){
        char name[80]; snprintf(name,sizeof name,"KHNMC_TWEAK_COMBAT_%s",gravity_keys[i]);
        if(getenv(name)){float value=configured(name,0); memcpy(hp(g,gravity_va[i]),&value,4);}
    }
    if(getenv("KHNMC_TWEAK_LEVELCAP_SET_LEVEL_CAP")){
        int cap=configured("KHNMC_TWEAK_LEVELCAP_SET_LEVEL_CAP",99);
        wr8(g,EX(0x3a14bc),cap); wr8(g,EX(0x3ed0bc),cap);
    }
    if(getenv("KHNMC_TWEAK_WORLDS_WORLDPOINT_AMOUNT"))
        wr8(g,EX(0x3f205e),configured("KHNMC_TWEAK_WORLDS_WORLDPOINT_AMOUNT",54));
    if(kh_tweak_boolean(g,"KHNMC_TWEAK_ANTIFORM_ANTIFORM_TWEAKS",0)){
        const u32 pcs[]={0x3a5719,0x3c0b97,0x3dbf6f,0x3f2ed2,0x404d06,0x4050fb,0x40870a};
        for(unsigned i=0;i<7;i++) wr8(g,EX(pcs[i]),99);
    }
    if(kh_tweak_boolean(g,"KHNMC_TWEAK_ANTIFORM_DISABLE_ABILITY_ANTI",0)) wr8(g,EX(0x421e04),0);
    if(kh_tweak_boolean(g,"KHNMC_TWEAK_ANTIFORM_DISABLE_RANDOM_ANTI",0)){
        wr8(g,EX(0x421e16),0x45); wr16(g,EX(0x421e17),0xc06b); wr8(g,EX(0x421e19),0);
    }
    if(kh_tweak_boolean(g,"KHNMC_TWEAK_WORLDS_HALVE_GUMMISHIP_PLANS",0)) wr8(g,EX(0x27a84c),9);
    if(kh_tweak_boolean(g,"KHNMC_TWEAK_WORLDS_HALVE_TINYSHIP_PLANS",0)) wr8(g,EX(0x27a8dc),9);
    const char *forms_option=getenv("KHNMC_TWEAK_FORMS_DUALWIELD_FORMS");
    if(forms_option){
        extern u32 kh_dual_forms[6]; extern u64 kh_mod_dual_slots_va;
        const int slots[]={5,4,3,1,2,0};
        for(int i=0;i<6;i++){
            char *end;kh_dual_forms[i]=strtod(forms_option,&end);
            wr32(g,kh_mod_dual_slots_va+4*slots[i],kh_dual_forms[i]); forms_option=end+1;
        }
    }
    const char *cursor=getenv("KHNMC_TWEAK_COMBAT_CURSORREMAIN");
    if(cursor){
        u32 mask=0;
        for(int i=0;i<19;i++){char *end;unsigned bit=strtod(cursor,&end); if(bit) mask|=1u<<bit;cursor=end+1;}
        wr32(g,EX(0x3b1788),mask?mask:0x7c420);
    }
    shortcuts=kh_tweak_boolean(g,"KHNMC_TWEAK_MULTIPLESHORTCUTS_MULTIPLE_SHORTCUTS",0);
    const char *levels[]={"BEGINNER","STANDARD","PROUD","CRIT"};
    for(int i=0;i<4;i++){
        struct kh_mod_difficulty_config *d=&kh_mod_difficulty_config[i];
        char name[96];
#define READ_FIELD(key,field) snprintf(name,sizeof name,"KHNMC_TWEAK_COMBAT_%s_" key,levels[i]); d->field=configured(name,d->field)
        READ_FIELD("ENEMY_DMG",enemy_damage); READ_FIELD("PARTY_DMG",party_damage);
        READ_FIELD("AP_MULT",ap);
        if(i==0){
            d->hp=configured("KHNMC_TWEAK_COMBAT_BEGGINER_HP_MULT",d->hp);
            d->mp=configured("KHNMC_TWEAK_COMBAT_BEGGINER_MP_MULT",d->mp);
        }
        READ_FIELD("HP_MULT",hp); READ_FIELD("MP_MULT",mp);
#undef READ_FIELD
    }
}

void orig_f_3f1e20(KCtx *);
void f_3f1e20(KCtx *g){
    if(kh_gummiskip){
        /* This version's table accessor returns save+0x3fec+4*index. */
        u64 save=call(0x152380,0,0,0,0);
        for(int i=0;i<19;i++) wr8(g,save+0x3fef+4*i,0);
        wr32(g,save+0x4158,0x02020202);wr32(g,save+0x415c,0x02020202);
    }
    orig_f_3f1e20(g);
}

/* DLL worker 2a210. Poll at the input frame, including on iOS. */
void kh_shortcut_frame(KCtx *g){
    static unsigned held;
    if(!shortcuts || rd8(g,EX(0x7435d0))!=10) return;
    u64 save=call(0x152380,0,0,0,0),pad=call(0x39b760,0,0,0,0);
    if(!pad) return;
    unsigned page=rd8(g,save+0xe000),menu=rd8(g,EX(0x7435d4)),keys=rd8(g,pad+8)&6;
    if(page>4) kh_fatal(g,"shortcut-page: save page %u exceeds 4",page);
    if(keys && !held && (menu==25 || menu==26)){
        unsigned next=page;
        if((keys&2) && page<4) next=page+1;
        if((keys&4) && page>0) next=page-1;
        if(next!=page){
            wr64(g,save+0xe001+page*8,rd64(g,save+0x36f8));
            page=next;wr8(g,save+0xe000,page);
            wr64(g,save+0x36f8,rd64(g,save+0xe001+page*8));
            call(0x1dd170,1,0,0,0);
            u64 selection=rd64(g,EX(0xbee660));
            if(menu==26 || (selection && !rd16(g,selection))) call(0x35baa0,0,0,0,0);
        }
    }
    held=keys;
    if(!keys) wr64(g,save+0xe001+page*8,rd64(g,save+0x36f8));
    u64 row=call(0x3ad9a0,0x451,0,0,0);
    if(row){static const u8 icons[]={0xee,0xf1,0xef,0xf0,0xe3};wr8(g,row+7,0x91+page);wr8(g,row+16,icons[page]);}
}
