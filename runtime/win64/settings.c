#include "khnmc_rt.h"
#include "settings.h"
#include "pad_layout.h"
#include "xion_swap.h"
#include "kh_look.h"
#include "mod_config.h"
#include "route_state.h"
#include <math.h>
#include <zlib.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

extern int kh_run_is_selftest;
enum { LOOK, HP, MP, EXP, DROP, HUD_COMBO, COUNT };
#define BOOL_OPT(name,label,initial) {"KHNMC_" name,label,initial,"0|1",0,1,1}
#define TWEAK_OPT(name,label,initial) {"KHNMC_TWEAK_" name,label,initial,"off|on",0,1,1}
#define NUMBER(name,label,initial,min,max,step) {"KHNMC_" name,label,initial,NULL,min,max,step}
const struct kh_option kh_options[]={
    {"KHNMC_PLAYER_LOOK","Character","roxas","roxas|xion|xion-hooded",0,0,0},
    BOOL_OPT("CHEAT_INF_HP","Infinite HP","0"), BOOL_OPT("CHEAT_INF_MP","Infinite MP","0"),
    NUMBER("EXP_MULT","EXP multiplier","1",0.5,10,0.5), NUMBER("DROP_MULT","Item drop multiplier","1",0.5,10,0.5),
    BOOL_OPT("HUD_HIDE_COMBO","Hold L3 + R3 to hide HUD","0"),
    {"KHNMC_PAD_LAYOUT","Face buttons","auto","auto|positions|letters",0,0,0},
    BOOL_OPT("PAD_SWAP_AB","Swap confirm / back (A-B)","0"),
    TWEAK_OPT("DEBUG_DEBUG_ON","Enable debug commands","off"),
    TWEAK_OPT("DEBUG_SOFT_RESET","Allow debug reset to title","off"),
    NUMBER("TWEAK_DEBUG_TURBO_SPEED","Debug turbo speed","7.5",0.1,100,0.1),
    TWEAK_OPT("RETRY_RETRY","Retry battle","off"), TWEAK_OPT("SAVEPOINT_SAVEPOINT_QOL","Restore Drive at savepoints","off"),
#define KH_GROUP(key,env,label) TWEAK_OPT(#env,label,"on"),
#include "tweak_groups.h"
#undef KH_GROUP
    BOOL_OPT("XKB_MENU","Xion Knows Best difficulty menu","0"), BOOL_OPT("SCENE_ROXAS","Scene Roxas","0"),
    NUMBER("FOV_ZOOM","Portrait camera zoom","1.3",1,1.89,0.05),
    NUMBER("HUD_SCALE_CMD","Portrait command menu","1.25",0.5,3,0.05),
    NUMBER("HUD_SCALE_HP","Portrait player gauges","1.5",0.5,3,0.05),
    NUMBER("TWEAK_FORMS_DUALWIELD_FORMS","Dual-wield form IDs","0,2,3,0,5,0",0,6,1),
    NUMBER("TWEAK_COMBAT_CURSORREMAIN","Remember cursor rows","0,0,0,0,0,5,0,0,0,0,10,0,0,0,14,0,16,17,18",0,31,1),
    TWEAK_OPT("MULTIPLESHORTCUTS_MULTIPLE_SHORTCUTS","Five shortcut sets","off"),
    TWEAK_OPT("WORLDS_PATCH_GUMMISKIP","Skip Gummi routes","off"),
    TWEAK_OPT("WORLDS_HALVE_GUMMISHIP_PLANS","Halve Gummi ship plan slots","off"),
    TWEAK_OPT("WORLDS_HALVE_TINYSHIP_PLANS","Halve tiny ship plan slots","off"),
    NUMBER("TWEAK_LEVELCAP_SET_LEVEL_CAP","Maximum level","99",1,99,1),
    NUMBER("TWEAK_WORLDS_WORLDPOINT_AMOUNT","World point count","54",1,127,1),
    TWEAK_OPT("ANTIFORM_ANTIFORM_TWEAKS","Anti Form actions and pickups","off"),
    TWEAK_OPT("ANTIFORM_DISABLE_RANDOM_ANTI","Disable random Anti Form","off"),
    TWEAK_OPT("ANTIFORM_DISABLE_ABILITY_ANTI","Disable Light & Dark Anti Form","off"),
    NUMBER("TWEAK_COMBAT_BEGINNER_ENEMY_DMG","Beginner enemy damage","50",0,10000,5),
    NUMBER("TWEAK_COMBAT_BEGINNER_PARTY_DMG","Beginner party damage","150",0,10000,5),
    NUMBER("TWEAK_COMBAT_BEGINNER_AP_MULT","Beginner AP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_BEGINNER_HP_MULT","Beginner HP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_BEGINNER_MP_MULT","Beginner MP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_STANDARD_ENEMY_DMG","Standard enemy damage","100",0,10000,5),
    NUMBER("TWEAK_COMBAT_STANDARD_PARTY_DMG","Standard party damage","100",0,10000,5),
    NUMBER("TWEAK_COMBAT_STANDARD_AP_MULT","Standard AP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_STANDARD_HP_MULT","Standard HP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_STANDARD_MP_MULT","Standard MP growth","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_PROUD_ENEMY_DMG","Proud enemy damage","200",0,10000,5),
    NUMBER("TWEAK_COMBAT_PROUD_PARTY_DMG","Proud party damage","125",0,10000,5),
    NUMBER("TWEAK_COMBAT_PROUD_AP_MULT","Proud AP growth","1.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_PROUD_HP_MULT","Proud HP growth","0.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_PROUD_MP_MULT","Proud MP growth","0.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_CRIT_ENEMY_DMG","Crit enemy damage","200",0,10000,5),
    NUMBER("TWEAK_COMBAT_CRIT_PARTY_DMG","Crit party damage","125",0,10000,5),
    NUMBER("TWEAK_COMBAT_CRIT_AP_MULT","Crit AP growth","1.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_CRIT_HP_MULT","Crit HP growth","0.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_CRIT_MP_MULT","Crit MP growth","0.5",0,100,0.25),
    NUMBER("TWEAK_COMBAT_BLOW_GRAVITY","Knockback gravity","0.81632656",0,100,0.05),
    NUMBER("TWEAK_COMBAT_GLOBAL_GRAVITY","Jump gravity","0.40816328",0,100,0.05),
    NUMBER("TWEAK_COMBAT_DRIVE_ADD_MULT","Drive gained on hit","1",0,100,0.25),
    NUMBER("TWEAK_FORMS_VALOR_GROWTH","Valor Growth","0,94,95,96,97",0,65535,1),
    NUMBER("TWEAK_FORMS_WISDOM_GROWTH","Wisdom Growth","0,0,0,0,0",0,65535,1),
    NUMBER("TWEAK_FORMS_LIMIT_GROWTH","Limit Growth","0,0,0,0,0",0,65535,1),
    NUMBER("TWEAK_FORMS_MASTER_GROWTH","Master Growth","0,102,103,104,105",0,65535,1),
    NUMBER("TWEAK_FORMS_FINAL_GROWTH","Final Growth","0,106,107,108,109",0,65535,1),
    NUMBER("TWEAK_CAMERA_CAMERA_PATCHES_OUTDOORS","Camera Outdoors","1.55,420.00,380.00,250.00,520.00,0.00800000038",0,100000,0.1),
    NUMBER("TWEAK_CAMERA_CAMERA_PATCHES_INDOORS","Camera Indoors","1.05,600.00,500.00,420.00,700.00,0.004999999888",0,100000,0.1),
    NUMBER("TWEAK_WORLDS_SYMBOL_ZZ","Symbol Zz","5024,1,2,3,4",0,2147483647,1),
    NUMBER("TWEAK_WORLDS_SYMBOL_ES","Symbol Es","5001,5,6,7,8",0,2147483647,1),
    NUMBER("TWEAK_WORLDS_SYMBOL_DI","Symbol Di","0,0,0,0,0",0,2147483647,1),
    NUMBER("TWEAK_WORLDS_SYMBOL_PO","Symbol Po","0,0,0,0,0",0,2147483647,1),
    NUMBER("TWEAK_WORLDS_SYMBOL_WI","Symbol Wi","0,0,0,0,0",0,2147483647,1),
    NUMBER("TWEAK_WORLDS_SYMBOL_TR","Symbol Tr","9999,9,9,9,9",0,2147483647,1),
    NUMBER("TWEAK_COMBAT_BEGGINER_HP_MULT","Beginner HP growth (INI spelling)","1",0,100,0.25),
    NUMBER("TWEAK_COMBAT_BEGGINER_MP_MULT","Beginner MP growth (INI spelling)","1",0,100,0.25),
    TWEAK_OPT("COMBAT_SUBMENU_MEMORY","Submenu memory (unused by DLL)","on"),
    TWEAK_OPT("COMBAT_MENU_MEMORY","Menu memory (unused by DLL)","off"),
    TWEAK_OPT("COMBAT_REPOINT_AND_EXTEND_BTLOBJ_SHEET","Extend battle objects (unused by DLL)","on"),
    TWEAK_OPT("COMBAT_CHANGE_CUSTOM_MAGIC_MPDRIVE_STATUS","Magic MP/Drive status (unused by DLL)","off"),
    NUMBER("SAVE_STATE","Save state","1",1,3,1), NUMBER("LOAD_STATE","Load state","1",1,3,1),
    BOOL_OPT("DEBUG_TURBO","Debug turbo","0"),
    BOOL_OPT("DEBUG_OHKO","Debug one-hit kills","0"),
    BOOL_OPT("DEBUG_INVINCIBLE","Debug party invincibility","0"),
    NUMBER("DEBUG_WARP","Warp: world, room, door, map, battle, event","2,0,0,0,0,0",0,65535,1),
    NUMBER("DEBUG_HIDE_GROUP","Hide map group","0",0,63,1),
    NUMBER("DEBUG_SHOW_GROUP","Show map group","0",0,63,1),
    BOOL_OPT("DEBUG_RESET","Return to title (discards progress)","0"),
};
const int kh_option_count=sizeof kh_options/sizeof *kh_options;
enum { STATE_ACTIONS=2 };
enum { TURBO, OHKO, INVINCIBLE, WARP, HIDE, SHOW, RESET, DEBUG_COUNT };
static _Atomic int debug_enabled,allow_reset;
static int debug_pending=-1,debug_arg[6],turbo;
static float turbo_speed=7.5f;
static unsigned long long mp_probe;
static int test_log;
int kh_setting_action(int i){return i>=kh_option_count-DEBUG_COUNT-STATE_ACTIONS && i<kh_option_count;}
static const char *unavailable[sizeof kh_options/sizeof *kh_options];
const char *kh_setting_unavailable(int i){
    if(i<0 || i>=kh_option_count) return "Unknown option";
    int first=kh_option_count-DEBUG_COUNT-STATE_ACTIONS;
    if(i>=first && i<first+STATE_ACTIONS) return kh_cut_menu_unavailable();
    return unavailable[i];
}
int kh_setting_menu_count(void){return kh_option_count-(debug_enabled?(allow_reset?0:1):DEBUG_COUNT);}
static _Atomic double values[COUNT]={[EXP]=1,[DROP]=1};
double kh_live_setting_value(int i){ return atomic_load(&values[i]); }
int kh_setting_live(int index){return index>=0 && index<COUNT;}

static int validate(const char *name,const char *value,double *number){
    if(!name || !value || !*value || strlen(value)>1000) return -1;
    for(int i=0;i<kh_option_count;i++) if(!strcmp(name,kh_options[i].name)){
        const struct kh_option *o=&kh_options[i];
        if(o->choices){
            const char *p=o->choices;
            while(*p){
                size_t n=strcspn(p,"|");
                if(strlen(value)==n && !strncmp(p,value,n)){*number=i==LOOK?(!strcmp(value,"xion-hooded")?2:!strcmp(value,"xion")):!strcmp(value,"1");return i;}
                p+=n; if(*p) p++;
            }
            return -1;
        }
        extern int kh_fov_zoom_parse(const char *,double *),kh_hud_scale_parse(const char *,double *);
        if(!strcmp(name,"KHNMC_FOV_ZOOM")) return kh_fov_zoom_parse(value,number)?-1:i;
        if(!strncmp(name,"KHNMC_HUD_SCALE_",16)) return kh_hud_scale_parse(value,number)?-1:i;
        const char *p=value,*shape=o->initial;
        for(int field=0;;field++){
            if(*p<'0' || *p>'9') return -1;
            char *end; double n=strtod(p,&end);
            size_t len=end-p;
            if(!len || len!=strspn(p,"0123456789.") || end[-1]=='.' ||
               !isfinite(n) || n<o->minimum || n>o->maximum) return -1;
            if((o->step>=1 || strstr(name,"_DMG")) && floor(n)!=n) return -1;
            if(!strcmp(name,"KHNMC_DEBUG_WARP") && ((field==0 && (n<1 || n>18)) || (field==1 && n>63) || (field==2 && n>255))) return -1;
            if(strstr(name,"_SYMBOL_") && field && n>255) return -1;
            if(field==0) *number=n;
            shape=strchr(shape,',');
            if(!shape) return *end?-1:i;
            if(*end!=',') return -1;
            p=end+1;shape++;
        }
    }
    return -1;
}

/* Replace only this env directive. Rename within Documents leaves the old file intact on failure. */
static int save_option(const char *dir,const char *name,const char *value){
    char path[4096],tmp[4096];
    if(snprintf(path,sizeof path,"%s/khnmc.conf",dir)>=(int)sizeof path ||
       snprintf(tmp,sizeof tmp,"%s.new.XXXXXX",path)>=(int)sizeof tmp) return -1;
    FILE *in=fopen(path,"r");
    if(!in && errno!=ENOENT) return -1;
    int fd=mkstemp(tmp);
    FILE *out=fd<0?NULL:fdopen(fd,"w");
    if(!out){if(in) fclose(in); if(fd>=0){close(fd);unlink(tmp);} return -1;}
    char *line=NULL; size_t cap=0; ssize_t size; int failed=0,last='\n';
    while(in && (size=getline(&line,&cap,in))>=0){
        char *p=line+strspn(line," \t");
        if(!strncmp(p,"env",3) && (p[3]==' ' || p[3]=='\t')){
            p+=3; p+=strspn(p," \t");
            if(!strncmp(p,name,strlen(name)) && p[strlen(name)]=='=') continue;
        }
        if(fwrite(line,1,size,out)!=(size_t)size) failed=1;
        last=line[size-1];
    }
    if(in){if(ferror(in)) failed=1; fclose(in);}
    free(line);
    if(fprintf(out,"%senv %s=%s\n",last=='\n'?"":"\n",name,value)<0) failed=1;
    if(fflush(out) || fsync(fd)) failed=1;
    if(fclose(out) || failed || rename(tmp,path)){unlink(tmp);return -1;}
    return 0;
}
/* Linux routes can reuse a Documents directory across launches. The shell environment wins there. */
#if !KH_IOS_PATHS
static void load_saved_options(void){
    const char *dir=getenv("KH_DOCS");
    if(!dir || !*dir) return;
    char path[4096];
    if(snprintf(path,sizeof path,"%s/khnmc.conf",dir)>=(int)sizeof path) kh_fatal(NULL,"settings-config-path");
    FILE *in=fopen(path,"r");
    if(!in){if(errno!=ENOENT) kh_fatal(NULL,"settings-config-read: %s",path);return;}
    char *line=NULL; size_t capacity=0;
    while(getline(&line,&capacity,in)>=0){
        char *p=line+strspn(line," \t");
        if(strncmp(p,"env",3) || (p[3]!=' ' && p[3]!='\t')) continue;
        p+=3;p+=strspn(p," \t");
        char *v=strchr(p,'=');
        if(!v) continue;
        *v++=0;v[strcspn(v,"\r\n")]=0;
        for(int i=0;i<kh_option_count-DEBUG_COUNT-STATE_ACTIONS;i++)
            if(!strcmp(p,kh_options[i].name) && !getenv(p) && setenv(p,v,0)) kh_fatal(NULL,"settings-config-env: %s",p);
    }
    if(ferror(in)) kh_fatal(NULL,"settings-config-read: %s",path);
    free(line);fclose(in);
}
#endif
static _Atomic int look_request=-1;
int kh_settings_look_locked(void){return kh_xion_enabled!=0;}
int kh_settings_hooded(void){return kh_look_pack_ready(2);}
int kh_settings_look(void){
    int request=atomic_load(&look_request);
    return request>=0?request%3:kh_look_enabled();
}
/* f_321e40 creates a four-row list. f_2eb490 builds two or three
 * stock rows, ending with Title Screen even when World Map is hidden. */
static int character_row=-1, picking_character;
static u16 choice_place;
static u64 character_labels[6], root_labels[4];
enum { CHANGE_LABEL=3, LOCK_LABEL, BACK_LABEL, CHARACTER_MSG=30016 };
u64 kh_save_character_text(unsigned id){
    unsigned i=(id&0x7fff)-CHARACTER_MSG;
    return i<6?character_labels[i]:0;
}
static u64 save_widget(KCtx *g,u64 menu,int row){
    return rd64(g,rd64(g,menu+0x28)+8*row);
}
void orig_f_2f8e20(KCtx *);
void f_2f8e20(KCtx *g){
    u64 sp=KH_RSP(g),menu=A0(g);
    if(rd64(g,sp)!=GS_BASE+0x2eb508 || rd32(g,sp+0x30)!=0x8a81 ||
       rd32(g,GS_BASE+0x900724)!=3){orig_f_2f8e20(g);return;}
    if(!character_labels[0]){
        const char *labels[]={"Roxas","Xion","Xion (hooded)","Change Character","Xion (locked)","Back"};
        for(int i=0;i<6;i++) character_labels[i]=kh_encode_menu_text(labels[i],i==CHANGE_LABEL);
    }
    character_row=(int)A1(g)+1;
    if(character_row<2 || character_row>3) kh_fatal(g,"save-character-row: %d",character_row);
    if(getenv("KHNMC_TEST_LOOK_DIGEST")) kh_warn("save-character built row %d",character_row);
    picking_character=0;
    KCtx ctx=*g;
    /* f_2f8e20 reuses its fifth stack argument for the tail call to f_2f8ab0. */
    u64 args[4];
    memcpy(args,hp(g,sp+0x28),sizeof args);
    orig_f_2f8e20(g);
    KH_RSP(&ctx)=sp-0x100;
    wr64(&ctx,KH_RSP(&ctx),GS_BASE+0x2eb508);
    memcpy(hp(&ctx,KH_RSP(&ctx)+0x28),args,sizeof args);
    A1(&ctx)=character_row;
    wr32(&ctx,KH_RSP(&ctx)+0x30,0x8000| (CHARACTER_MSG+(kh_settings_look_locked()?LOCK_LABEL:CHANGE_LABEL)));
    orig_f_2f8e20(&ctx);
    u64 widget=save_widget(&ctx,menu,character_row);
    kh_call(&ctx,GS_BASE+0x2f8db0,menu,character_row,0,0xffffffffu);
    kh_call(&ctx,GS_BASE+0x166ba0,widget,0,0,0);
    kh_call(&ctx,GS_BASE+0x1672c0,widget,0,0,0);
}
void orig_f_2eb620(KCtx *);
void f_2eb620(KCtx *g){
    KCtx ctx=*g;
    u64 menu=rd64(g,GS_BASE+0xbef650);
    if(character_row>=0 && menu && rd32(g,GS_BASE+0x900724)==3 &&
       !kh_call(&ctx,GS_BASE+0x2f5f30,0,0,0,0) &&
       (picking_character || kh_call(&ctx,GS_BASE+0x2f7fb0,menu,0,0,0)==character_row)){
        int label=kh_settings_look_locked()?LOCK_LABEL:CHANGE_LABEL;
        kh_call(&ctx,GS_BASE+0x2fa1d0,0x8000|(CHARACTER_MSG+label),1,0,0);
        KH_RET(g);return;
    }
    orig_f_2eb620(g);
}
void orig_f_2f9440(KCtx *);
void f_2f9440(KCtx *g){
    u64 menu=A0(g),caller=rd64(g,KH_RSP(g));
    orig_f_2f9440(g);
    if(caller!=GS_BASE+0x2e9ebf || character_row<0 || rd32(g,GS_BASE+0x900724)!=3) return;
    int row=(s32)KH_RAX(g);
    if(!picking_character && row==character_row){
        KH_RAX(g)=(u32)-1;
        if(kh_settings_look_locked()) return;
        picking_character=1;
        KCtx ctx=*g;
        for(int i=0;i<=character_row;i++){
            u64 field=save_widget(g,menu,i)+0x1f8;
            root_labels[i]=rd64(g,field);
            wr64(g,field,kh_call(&ctx,GS_BASE+0x3ad9a0,0x8000|(CHARACTER_MSG+(i==3?BACK_LABEL:i)),0,0,0));
        }
        kh_call5(&ctx,GS_BASE+0x2f8c90,menu,kh_settings_look(),1,0,2);
    }else if(picking_character && (row>=0 || row==-2 || row==-4)){
        if(row>=0 && row<3 && !kh_settings_look_locked()){
            choice_place=rd16(g,GS_BASE+0x717008);
            atomic_store(&look_request,3+row); /* save-menu requests must remain at the point */
            if(getenv("KHNMC_TEST_LOOK_DIGEST")) kh_warn("save-character chose %d",row);
        }
        for(int i=0;i<=character_row;i++) wr64(g,save_widget(g,menu,i)+0x1f8,root_labels[i]);
        picking_character=0;
        KH_RAX(g)=(u32)-1;
    }
}

static int notice_selftest(KCtx *g);
int kh_settings_selftest(KCtx *g){
    char dir[KH_TMP_MAX],path[KH_TMP_MAX+16],data[200];
    kh_tmp_path(dir,sizeof dir,"khnmc-settings-XXXXXX");
    if(!mkdtemp(dir)) return 1;
    snprintf(path,sizeof path,"%s/khnmc.conf",dir);
    FILE *f=fopen(path,"w");int fail=!f;
    if(f){fputs("# keep\n env KHNMC_EXP_MULT=1\narg --selftest",f);fail|=fclose(f)!=0;}
    if(!fail) fail=save_option(dir,"KHNMC_EXP_MULT","2")!=0;
    if(!fail) fail=save_option(dir,"KHNMC_PAD_LAYOUT","auto") ||
                   save_option(dir,"KHNMC_PAD_SWAP_AB","1") || save_option(dir,"KHNMC_PAD_LAYOUT","positions");
    f=fopen(path,"r");
    if(!f) fail=1;
    else{
        size_t n=fread(data,1,sizeof data-1,f);data[n]=0;fclose(f);
        fail|=strcmp(data,"# keep\narg --selftest\nenv KHNMC_EXP_MULT=2\nenv KHNMC_PAD_SWAP_AB=1\nenv KHNMC_PAD_LAYOUT=positions\n")!=0;
    }
    double v;
    fail|=validate("KHNMC_SAVE_STATE","1",&v)<0 || v!=1;
    fail|=validate("KHNMC_LOAD_STATE","3",&v)<0 || v!=3;
    fail|=validate("KHNMC_LOAD_STATE","4",&v)>=0 || validate("KHNMC_SAVE_STATE","1.5",&v)>=0;
    fail|=!kh_setting_action(validate("KHNMC_SAVE_STATE","2",&v));
    fail|=validate("KHNMC_TWEAK_WORLDS_SYMBOL_ZZ","12,1,2,3,256",&v)>=0;
    fail|=validate("KHNMC_TWEAK_FORMS_VALOR_GROWTH","1,2",&v)>=0;
    fail|=validate("KHNMC_TWEAK_DEBUG_TURBO_SPEED","7.5",&v)<0;
    fail|=validate("KHNMC_PAD_LAYOUT","positions",&v)<0 || validate("KHNMC_PAD_LAYOUT","letters",&v)<0 ||
          validate("KHNMC_PAD_LAYOUT","auto",&v)<0 || validate("KHNMC_PAD_LAYOUT","unknown",&v)>=0;
    fail|=validate("KHNMC_PAD_SWAP_AB","1",&v)<0 || v!=1;
    fail|=kh_pad_swap_xy("auto",0,NULL) || !kh_pad_swap_xy("auto",1,NULL) ||
          kh_pad_swap_xy("auto",1,"0") || !kh_pad_swap_xy("auto",0,"1") ||
          kh_pad_swap_xy("positions",1,"1") || !kh_pad_swap_xy("letters",0,"0");
    for(int xy=0;xy<2;xy++) for(int ab=0;ab<2;ab++) for(int mask=0;mask<16;mask++){
        unsigned mapped=kh_pad_faces(mask&1,mask&2,mask&4,mask&8,xy,ab);
        unsigned recovered=kh_pad_faces(mapped&0x1000,mapped&0x2000,mapped&0x4000,mapped&0x8000,xy,ab);
        fail|=recovered!=(unsigned)(mask<<12);
    }
    fail|=kh_pad_faces(1,0,1,0,0,0)!=0x5000 || kh_pad_faces(1,0,1,0,1,0)!=0x9000 ||
          kh_pad_faces(1,0,1,0,0,1)!=0x6000;
    int state=0; u64 start=0;
    fail|=kh_stick_step(&state,&start,0x40,1)!=CHORD_IDLE;
    fail|=kh_stick_step(&state,&start,0xc0,2)!=CHORD_PRESS;
    fail|=kh_stick_step(&state,&start,0x80,600000001)!=CHORD_CLICK;
    fail|=kh_stick_step(&state,&start,0xc0,900000000)!=CHORD_PRESS;
    fail|=kh_stick_step(&state,&start,0xc0,1500000000)!=CHORD_HOLD;
    fail|=kh_stick_step(&state,&start,0,1500000001)!=CHORD_IDLE;
    fail|=LOOK!=0;
    fail|=validate("KHNMC_PLAYER_LOOK","xion",&v)!=LOOK || v!=1;
    fail|=notice_selftest(g);
    unlink(path);rmdir(dir);
    fprintf(stderr,"kh: settings persistence and vector validation: %d failures\n",fail);
    return fail;
}
static pthread_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
static int kh_save_live_option(const char *,const char *,int,double);
int kh_setting_set(const char *name,const char *value){
    double v; int i=validate(name,value,&v);
    if(i<0) return -1;
    if(kh_setting_unavailable(i)) return -2;
    if(kh_setting_action(i)){
        int debug_start=kh_option_count-DEBUG_COUNT;
        if(i<debug_start) return kh_cut_menu_request(i==debug_start-1,(int)v);
        if(i>=kh_setting_menu_count()) return -1;
        pthread_mutex_lock(&config_lock);
        if(debug_pending>=0){pthread_mutex_unlock(&config_lock);return -1;}
        for(int k=0;k<6;k++){
            char *end;debug_arg[k]=(int)strtod(value,&end);
            if(*end!=',') break;
            value=end+1;
        }
        debug_pending=i-(kh_option_count-DEBUG_COUNT);
        pthread_mutex_unlock(&config_lock);return 0;
    }
    if(i==LOOK){atomic_store(&look_request,(int)v); return 0;}
    return kh_save_live_option(name,value,i,v);
}
static int kh_save_live_option(const char *name,const char *value,int i,double v){
    const char *dir=getenv("KH_DOCS"); char docs[4096];
#if KH_IOS_PATHS
    const char *app=getenv("KH_APP_HOME");
    if(app){
        if(snprintf(docs,sizeof docs,"%s/Documents",app)>=(int)sizeof docs) return -1;
        dir=docs;
    }
#endif
    pthread_mutex_lock(&config_lock);
    int result=kh_run_is_selftest?0:dir?save_option(dir,name,value):(kh_setting_live(i)?0:-1);
    if(!result && kh_setting_live(i)) atomic_store(&values[i],v);
    pthread_mutex_unlock(&config_lock);
    return result;
}

struct change { unsigned long long frame; char *name,*value; struct change *next; };
static struct change *pending;
static char *schedule_storage;
static void init_notice_text(void);
void kh_settings_init(void){
    init_notice_text();
#if !KH_IOS_PATHS
    load_saved_options();
#endif
    /* Older phone configs use the shorter spelling. */
    const char *present=getenv("KHNMC_PRESENT");
    if(present && *present && !getenv("KHNMC_GFX_PRESENT"))
        setenv("KHNMC_GFX_PRESENT",present,0);
    static const struct { const char *key,*choices; } switches[]={
        {"KHNMC_ASPECT","pc|fill"},{"KHNMC_ASPECT_LOG","0|1|2"},
        {"KHNMC_TITLE_LAYOUT","portrait|band"},{"KHNMC_INPUT","host|script"},
        {"KHNMC_MOVIES","end|fail"},{"KHNMC_DIFFICULTY","0|1|2|3"},
        {"KHNMC_THEATER_UNLOCK","0|1"},{"KHNMC_APP_LIFE","on|off"},
        {"KHNMC_DISPLAY","timer60"},{"KHNMC_GFX","null|vkd3dw|vkd3d"},
        {"KHNMC_GFX_DRIVER","system|lavapipe"},{"KHNMC_GFX_BACKBUFFERS","own|general|committed"},
        {"KHNMC_GFX_PRESENT","off|headless|layer"},{"KHNMC_GFX_PRESENT_FIT","aspect|stretch"},
    };
    for(size_t i=0;i<sizeof switches/sizeof *switches;i++){
        const char *v=getenv(switches[i].key),*p=switches[i].choices;
        if(!v || !*v) continue;
        while(*p){
            size_t n=strcspn(p,"|");
            if(strlen(v)==n && !strncmp(p,v,n)) break;
            p+=n; if(*p) p++;
        }
        if(!*p){
            kh_warn("%s disabled: expected %s",switches[i].key,switches[i].choices);
            unsetenv(switches[i].key);
        }
    }
    for(int i=0;i<kh_option_count-DEBUG_COUNT-STATE_ACTIONS;i++){
        const char *key=kh_options[i].name,*v=getenv(key);
        if(!v) continue;
        double n;
        const char *reason=NULL;
        if(!strncmp(key,"KHNMC_TWEAK_",12) && kh_options[i].choices){
            if(strcasecmp(v,"on") && strcasecmp(v,"true") && strcmp(v,"1") &&
               strcasecmp(v,"off") && strcasecmp(v,"false") && strcmp(v,"0"))
                reason="Expected on or off";
        }else if(validate(key,v,&n)<0) reason="Invalid configured value";
        else if(i==LOOK && n && !kh_look_pack_ready((int)n)) reason="Xion asset pack incomplete";
        else if(!strcmp(key,"KHNMC_SCENE_ROXAS") && n && !kh_scene_pack_ready()) reason="Scene asset pack incomplete";
        if(!reason && (!strcmp(key,"KHNMC_FOV_ZOOM") || !strncmp(key,"KHNMC_HUD_SCALE_",16))){
            const char *aspect=getenv("KHNMC_ASPECT");
            if(!aspect || strcmp(aspect,"fill")) reason="Requires fill aspect";
        }
        if(reason){
            kh_warn("%s disabled: %s",key,reason);
            unavailable[i]=reason;
            unsetenv(key);
        }
    }
    extern int kh_tweak_boolean(KCtx *,const char *,int);
    debug_enabled=kh_tweak_boolean(NULL,"KHNMC_TWEAK_DEBUG_DEBUG_ON",0);
    allow_reset=kh_tweak_boolean(NULL,"KHNMC_TWEAK_DEBUG_SOFT_RESET",0);
    const char *speed=getenv("KHNMC_TWEAK_DEBUG_TURBO_SPEED");
    if(speed) turbo_speed=strtof(speed,NULL);
    const char *probe=getenv("KHNMC_TEST_MP_COST_FRAME");
    if(probe){
        char *end;errno=0;mp_probe=strtoull(probe,&end,10);
        if(*probe<'0'||*probe>'9'||errno||*end||!mp_probe) kh_fatal(NULL,"settings-mp-probe-frame: %s",probe);
    }
    test_log=getenv("KHNMC_TEST_CHEAT_LOG") && !strcmp(getenv("KHNMC_TEST_CHEAT_LOG"),"1");
    for(int i=0;i<COUNT;i++){
        const char *name=kh_options[i].name,*v=getenv(name);
        if(v){double n; if(validate(name,v,&n)<0) kh_fatal(NULL,"setting-value: %s=%s",name,v); values[i]=n;}
    }
    const char *spec=getenv("KHNMC_TEST_SETTINGS");
    if(!spec) return;
    schedule_storage=strdup(spec);
    char *rest=schedule_storage,*entry;
    if(!rest) kh_fatal(NULL,"settings-allocation");
    unsigned long long frame=0;
    while(rest){
        entry=rest;
        char *sep=rest;
        while((sep=strchr(sep,','))){
            if(!strncmp(sep+1,"frame:",6) || !strncmp(sep+1,"KHNMC_",6)) break;
            sep++;
        }
        rest=sep?sep+1:NULL;
        if(sep) *sep=0;
        char *number=entry;
        char *key=entry;
        if(!strncmp(entry,"frame:",6) || (*entry>='0' && *entry<='9')){
            if(!strncmp(entry,"frame:",6)) number+=6;
            char *end;
            errno=0;frame=strtoull(number,&end,10);
            if(*number<'0'||*number>'9'||errno||*end!=':'||!frame) kh_fatal(NULL,"settings-frame: %s",entry);
            key=end+1;
        }else if(!frame) kh_fatal(NULL,"settings-frame: %s",spec);
        char *val=strchr(key,'=');
        if(!val) kh_fatal(NULL,"settings-assignment: %s",key);
        *val++=0;
        double n; if(validate(key,val,&n)<0) kh_fatal(NULL,"setting-value: %s=%s",key,val);
        struct change *c=malloc(sizeof *c);
        if(!c) kh_fatal(NULL,"settings-allocation");
        struct change **p=&pending;
        while(*p && (*p)->frame<=frame) p=&(*p)->next;
        *c=(struct change){frame,key,val,*p}; *p=c;
    }
}
_Atomic int kh_hide_hud;
/* f_18deb0 replaces the active INFORMATION line. Keep transient feedback off
 * that widget so mission scripts retain their own message and timing. */
enum { NOTICE_XION, NOTICE_ROXAS, NOTICE_FIELD, NOTICE_BUSY, NOTICE_ACTOR, NOTICE_PACK, NOTICE_XKB, NOTICE_HOODED, NOTICE_COUNT };
static const char *const notice_lines[]={"Xion","Roxas","Can't switch now: outside field","Can't switch now: busy",
    "Can't switch now: player unavailable","Can't switch now: missing art","Can't switch now: XKB","Xion (hooded)"};
static u8 look_text[NOTICE_COUNT][40];
static _Atomic u64 notice;
u8 kh_look_font[65536];
static void init_notice_text(void){
    for(int i=0;i<NOTICE_COUNT;i++){
        for(size_t j=0;notice_lines[i][j];j++){
            char c=notice_lines[i][j];
            look_text[i][j]=c=='('?0x5a:c==')'?0x5b:c==' '?1:c=='\''?0x57:c==':'?0x52:c>='a'?0x9a+c-'a':0x2e + c-'A';
        }
    }
}
static _Atomic int font_ready; /* zero while loading, negative for an unsupported atlas */
void kh_look_font_loaded(const char *path,const u8 *bar,size_t n){
    /* Called under files_lock for both loose and packaged data. f_3e2470
     * uploads the first sys member as a 512-wide 4-bit system font. */
    if(atomic_load(&font_ready)) return;
    size_t len=strlen(path);
    const char suffix[]="fontimage.bar";
    if(len<sizeof suffix-1 || strcasecmp(path+len-(sizeof suffix-1),suffix)) return;
    if(n<32 || memcmp(bar,"BAR\1",4) || memcmp(bar+16,"\x24\0\0\0sys",8)) goto invalid;
    u32 offset,size;
    memcpy(&offset,bar+24,4); memcpy(&size,bar+28,4);
    if(offset>n || size>n-offset || size<sizeof kh_look_font) goto invalid;
    memcpy(kh_look_font,bar+offset,sizeof kh_look_font);
    atomic_store(&font_ready,1);
    return;
invalid:
    atomic_store(&font_ready,-1);
}
static const u8 *visible_notice(u64 state,u64 now){
    return now<(state & ~7ull)?look_text[state & 7]:NULL;
}
const u8 *kh_look_notice(void){
    u64 state=atomic_load(&notice);
    if(!state) return NULL;
    int ready=atomic_load(&font_ready);
    if(ready<=0){
        if(ready==-1 && atomic_compare_exchange_strong(&font_ready,&ready,-2))
            kh_warn("appearance notices disabled: unsupported system font");
        return NULL;
    }
    u64 now=kh_host_ns();
    /* Start the notice timer when the font is ready. */
    if(state<=NOTICE_COUNT){
        u64 active=((now+2000000000ull) & ~7ull) | (state-1);
        if(!atomic_compare_exchange_strong(&notice,&state,active)) return NULL;
        state=active;
    }
    const u8 *text=visible_notice(state,now);
    if(!text) atomic_compare_exchange_strong(&notice,&state,0);
    return text;
}
static int notice_selftest(KCtx *g){
    u64 p=ga_alloc(sizeof look_text[0],16);
    if(!p) return 1;
    int bad=0;
    init_notice_text();
    for(unsigned i=0;i<NOTICE_COUNT;i++){
        char decoded[40];
        memcpy(hp(g,p),look_text[i],sizeof look_text[i]);
        kh_route_message_text(g,p,decoded,sizeof decoded);
        bad|=strcmp(decoded,notice_lines[i])!=0 ||
             visible_notice(2000000000ull|i,1999999999ull)!=look_text[i] ||
             visible_notice(2000000000ull|i,2000000000ull)!=NULL;
    }
    ga_free(p);
    return bad || visible_notice(0,0)!=NULL;
}
static void show_look_notice(int index,u64 frame){
    atomic_store(&notice,index+1);
    fprintf(stderr,"kh: appearance notice: game frame %llu: \"%s\"\n",(unsigned long long)frame,notice_lines[index]);
}
/* A Drive change destroys the actor and its weapons. Use its model and material
 * binding operations on the live render instance, retaining the pose and sheet. */
struct look_art { char stem[40]; int mode; u64 body,face; };
static struct look_art art[48];
static u64 face_actor,face_instance,face_model,face_bar;
static u64 last_switch;
static int gauge_pending, finishing_look=-1;
/* A frame already submitted may still reference the old instance. Retire it
 * with the actor, at the same native teardown boundary as the active one. */
static struct idle_render { u64 owner,instance[3]; } idle[80]; /* native sheet pool */
static void free_render(KCtx *g,u64 inst){
    u64 model=rd64(g,inst+0x20),allocator=kh_call(g,GS_BASE+0x152590,0,0,0,0);
    kh_call(g,rd64(g,rd64(g,model)+0x48),model,allocator,inst,0);
}
static void end_look_actor(KCtx *g){
    if(!last_switch) return;
    if(A0(g)==face_actor) face_actor=face_instance=face_model=face_bar=0;
    for(size_t i=0;i<80;i++) if(idle[i].owner==A0(g)){
        KCtx ctx=*g;
        for(int j=0;j<3;j++) if(idle[i].instance[j]) free_render(&ctx,idle[i].instance[j]);
        idle[i]=(struct idle_render){0};
        break;
    }
}
void orig_f_3b43c0(KCtx *); void f_3b43c0(KCtx *g){ end_look_actor(g); orig_f_3b43c0(g); }
void orig_f_3b4ec0(KCtx *); void f_3b4ec0(KCtx *g){ end_look_actor(g); orig_f_3b4ec0(g); }
void orig_f_3b57b0(KCtx *);
void f_3b57b0(KCtx *g){
    u64 actor=A0(g);
    if(!face_bar || actor!=face_actor || rd64(g,actor+0x7b0)!=face_instance || rd64(g,actor+0xa88)!=face_model){
        orig_f_3b57b0(g); return;
    }
    u64 native=rd64(g,actor+0x928);
    wr64(g,actor+0x928,face_bar);
    orig_f_3b57b0(g);
    wr64(g,actor+0x928,native);
}
static u64 look_bar(KCtx *g,const char *stem,const char *ext,u64 native){
    char name[72];
    int n=snprintf(name,sizeof name,"obj/%s.%s",stem,ext);
    if(n<0 || (size_t)n>=sizeof name) return 0;
    char host[4096];
    if(kh_look_enabled() && !kh_look_available(name,host,sizeof host)) return 0;
    u64 path=ga_strdup(name);
    int size=(s32)kh_call(g,GS_BASE+0x3a09b0,path,0,0,0);
    u64 bar=size>=16?ga_alloc(size,128):0;
    if(bar && (s32)kh_call(g,GS_BASE+0x3f9850,path,bar,0,0)!=size){
        ga_free(bar); bar=0;
    }
    ga_free(path);
    if(bar){
        if(getenv("KHNMC_TEST_LOOK_DIGEST"))
            fprintf(stderr,"kh: look-resource mode=%d path=%s crc=%08lx bar=%llx\n",kh_look_enabled(),name,
                    (unsigned long)crc32(0,hp(g,bar+16),size-16),(unsigned long long)bar);
        wr8(g,bar+3,(rd8(g,bar+3)&63)|(rd8(g,native+3)&192));
        kh_call(g,GS_BASE+0x3a64f0,bar,0,0,0);
        kh_call(g,GS_BASE+0x3a6840,bar,0,0,0);
    }
    return bar;
}
static struct look_art *look_art(KCtx *g,const char *stem,int target,u64 body,u64 face){
    struct look_art *a=art;
    while(a<art+48 && a->stem[0] && (a->mode!=target || strcmp(a->stem,stem))) ++a;
    if(a==art+48) kh_fatal(g,"look-art-cache-full: %s",stem);
    snprintf(a->stem,sizeof a->stem,"%s",stem); a->mode=target;
    if(!a->body) a->body=look_bar(g,stem,"mdlx",body);
    if(face && !a->face) a->face=look_bar(g,stem,"a.us",face);
    return a->body && (!face || a->face)?a:NULL;
}

/* The native cache owns +0x10/+0x18 and releases them by pointer at f_3e61e0.
 * Keep those references until teardown while all consumers see the selected BARs. */
static u64 held_resources, deferred_resources;
static void forget_deferred(KCtx *g,u64 resource){
    u64 prev=0;
    for(u64 e=deferred_resources;e;prev=e,e=rd64(g,e)){
        if(rd64(g,e+8)!=resource) continue;
        if(prev) wr64(g,prev,rd64(g,e)); else deferred_resources=rd64(g,e);
        ga_free(e);return;
    }
}
static void publish_art(KCtx *g,u64 resource,u64 body,u64 face){
    u64 entry=held_resources;
    while(entry && rd64(g,entry+8)!=resource) entry=rd64(g,entry);
    if(!entry){
        if(rd64(g,resource+16)==body && rd64(g,resource+24)==face) return;
        entry=ga_alloc(32,16);
        if(!entry) kh_fatal(g,"look-resource-allocation");
        wr64(g,entry,held_resources); wr64(g,entry+8,resource);
        wr64(g,entry+16,rd64(g,resource+16)); wr64(g,entry+24,rd64(g,resource+24));
        held_resources=entry;
    }
    wr64(g,resource+16,body); wr64(g,resource+24,face);
}
void orig_f_3e61e0(KCtx *);
void f_3e61e0(KCtx *g){
    u64 prior=0,resource=A0(g);
    forget_deferred(g,resource);
    for(u64 e=held_resources;e;prior=e,e=rd64(g,e)) if(rd64(g,e+8)==resource){
        wr64(g,resource+16,rd64(g,e+16)); wr64(g,resource+24,rd64(g,e+24));
        if(prior) wr64(g,prior,rd64(g,e)); else held_resources=rd64(g,e);
        ga_free(e); break;
    }
    orig_f_3e61e0(g);
}
void orig_f_3e6120(KCtx *);
static int bind_resource(KCtx *g,u64 resource){
    if(!last_switch && !kh_scene_pack_active()) return 1;
    u64 row=rd64(g,resource+8),body=rd64(g,resource+16),face=rd64(g,resource+24);
    if(!row || !body) return 1;
    char stem[33],path[64];
    memcpy(stem,hp(g,row+8),32); stem[32]=0;
    snprintf(path,sizeof path,"obj/%s.mdlx",stem);
    int high=kh_scene_pack_active() && !strcmp(path,KH_SCENE_MDLX);
    if(!high && (!last_switch || kh_player_asset_key(path)!=-1)) return 1;
    KCtx ctx=*g;
    struct look_art *a=look_art(&ctx,stem,kh_look_enabled(),body,high?0:face);
    if(!a) return 0;
    publish_art(g,resource,a->body,high?face:(face?a->face:0));
    return 1;
}
void f_3e6120(KCtx *g){
    u64 resource=A0(g);
    orig_f_3e6120(g);
    if(bind_resource(g,resource)) return;
    for(u64 e=deferred_resources;e;e=rd64(g,e)) if(rd64(g,e+8)==resource) return;
    u64 entry=ga_alloc(16,16);
    if(!entry) kh_fatal(g,"look-resource-retry-allocation");
    wr64(g,entry,deferred_resources); wr64(g,entry+8,resource);
    deferred_resources=entry;
    kh_warn("look-resource-bind deferred");
}
static int bind_look(KCtx *g,KhPlayerSheet p,const char *stem,int target){
    u64 obj=p.obj,inst=rd64(g,obj+0x7b0);
    if(!inst || !rd64(g,obj+0xa88) || !rd64(g,obj+0x920) || !rd64(g,obj+0x928) ||
        rd64(g,inst+0x20)!=rd64(g,obj+0xa88)) return 0;
    struct look_art *a=look_art(g,stem,target,rd64(g,obj+0x920),rd64(g,obj+0x928));
    if(!a) return 0;
    u64 native=rd64(g,obj+0x920),face=a->face;
    u64 original=kh_call(g,GS_BASE+0x3a6420,native,4,0,0);
    u64 wanted=kh_call(g,GS_BASE+0x3a6420,a->body,4,0,0);
    if(!original || !wanted) return 0;
    /* Hooded and unhooded rigs can have the same byte length. */
    u64 selected_body=native;
    if(original==wanted) face=rd64(g,obj+0x928);
    else { selected_body=a->body; wr64(g,obj+0x920,selected_body); }
    u64 model=kh_call(g,GS_BASE+0x3e62e0,obj+0x910,0,0,0);
    u64 tex=kh_call(g,GS_BASE+0x3e6310,obj+0x910,0,0,0);
    wr64(g,obj+0x920,native);
    if(!model || !tex || rd64(g,model)!=rd64(g,rd64(g,obj+0xa88))) return 0;
    int changed=rd64(g,obj+0xa88)!=model;
    if(changed){
        size_t slot=(p.sheet-(GS_BASE+0x2a17290))/0x278;
        if(slot>=80 || (idle[slot].owner && idle[slot].owner!=obj)) return 0;
        u64 *spare=idle[slot].instance;
        int j=0;
        while(j<3 && spare[j] && rd64(g,spare[j]+0x20)!=model) j++;
        if(j==3) return 0;
        u64 other=spare[j];
        if(!other){
            u64 allocator=kh_call(g,GS_BASE+0x152590,0,0,0,0);
            u64 next=kh_call(g,rd64(g,rd64(g,model)+0x40),model,allocator,0,0);
            if(!next) return 0;
            kh_call(g,GS_BASE+0x1a4bb0,next,tex,0,0);
            kh_call(g,GS_BASE+0x1a4e30,next,0,0,0);
            other=next;
        }
        idle[slot].owner=obj; spare[j]=inst;
        inst=other;
        wr64(g,obj+0xa88,model);
        wr64(g,obj+0x7b0,inst);
        wr64(g,obj+0x7e0,tex);
    }
    publish_art(g,obj+0x910,selected_body,face);
    face_bar=face; face_actor=obj; face_instance=inst; face_model=model;
    gauge_pending=1;
    return 1;
}
static void retry_gauge(KCtx *g){
    if(!gauge_pending) return;
    u64 gauge=rd64(g,GS_BASE+0xabd248);
    if(!face_actor){gauge_pending=0;return;}
    if(!gauge || rd64(g,gauge+0x40)!=face_actor){gauge_pending=0;return;}
    KCtx ctx=*g;
    kh_call(&ctx,GS_BASE+0x17eb30,gauge+0x50,face_actor,rd32(g,gauge+0x20),0);
    gauge_pending=!rd8(g,gauge+0x50+0x304);
}
static void look_frame(KCtx *g,int request,u64 frame){
    int target=request%3;
    int previous=kh_look_enabled();
    int refresh=target<0;
    if(refresh){
        KhPlayerSheet active;
        if(!kh_route_player_sheet(g,&active) ||
           (active.obj==face_actor && rd64(g,active.obj+0x7b0)==face_instance &&
            rd64(g,active.obj+0xa88)==face_model)) return;
        target=kh_look_enabled();
    }else if(target==kh_look_enabled()) return;
    KhPlayerSheet player;
    char stem[40],path[64];
    u64 cmds; int windows,waiting;
    kh_route_field_parts(g,&cmds,&windows,&waiting);
    int notice_index=NOTICE_FIELD;
    if(kh_xion_enabled && !refresh){notice_index=NOTICE_XKB; goto feedback;}
    if(!cmds) goto feedback;
    notice_index=NOTICE_BUSY;
    if(windows || rd64(g,GS_BASE+0x2a244d8) || rd8(g,GS_BASE+0x9ba8d1) || rd8(g,GS_BASE+0x2a11428) || rd8(g,GS_BASE+0x9006b0) || rd64(g,GS_BASE+0x2a11478) ||
       rd64(g,GS_BASE+0x2ae9fa8) || rd64(g,GS_BASE+0x2a11268) || /* Roxas parked during XKB ownership */
       rd32(g,GS_BASE+0x2a10504) || rd64(g,GS_BASE+0x9ba928) || kh_scene_pack_active()) goto feedback;
    notice_index=NOTICE_ACTOR;
    if(!kh_route_player_sheet(g,&player) || player.hp<=0 ||
       !kh_route_object_name(g,player.obj,stem,sizeof stem)) goto feedback;
    snprintf(path,sizeof path,"obj/%s.mdlx",stem);
    if(kh_player_asset_key(path)!=-1) goto feedback;
    if(target){
        static int validated[3];
        if(!validated[target]){
            if(!kh_look_switch_ready(target)){notice_index=NOTICE_PACK;goto feedback;}
            validated[target]=1;
        }
        char face[96],host[4096];
        if(!kh_look_asset(target,path,host,sizeof host)) {notice_index=NOTICE_PACK; goto feedback;}
        snprintf(path,sizeof path,"obj/%s.a.us",stem);
        snprintf(face,sizeof face,"remastered/%s/-%d.dds",path,kh_player_asset_key(path)-1);
        if(!kh_look_asset(target,path,host,sizeof host) || !kh_look_asset(target,face,host,sizeof host)){
            notice_index=NOTICE_PACK; goto feedback;
        }
    }
    KCtx ctx=*g;
    kh_loading_look=target;
    int bound=bind_look(&ctx,player,stem,target);
    kh_loading_look=-1;
    if(!bound){
        notice_index=NOTICE_PACK;
        goto feedback;
    }
    if(refresh) return;
    kh_look_set(&ctx,target);
    retry_gauge(&ctx);
    if((previous!=0)!=(target!=0)) kh_look_voice_bind(&ctx);
    finishing_look=target;
    last_switch=frame;
    if(getenv("KHNMC_TEST_LOOK_DIGEST")){
        u64 name=kh_call(&ctx,GS_BASE+0x3ad9a0,0x851f,0,0,0);
        const char *text=gstr(&ctx,name);
        fprintf(stderr,"kh: look-commit frame=%llu mode=%d stem=%s body=%llx face=%llx name=%08lx gauge=%d\n",
                (unsigned long long)frame,target,stem,(unsigned long long)rd64(g,player.obj+0x920),
                (unsigned long long)face_bar,(unsigned long)crc32(0,(const u8 *)text,strlen(text)),!gauge_pending);
    }
    return;
feedback:
    if(refresh) return;
    static int last_feedback=-1;
    if(notice_index>=NOTICE_FIELD && notice_index<NOTICE_XKB){
        int empty=-1;
        atomic_compare_exchange_strong(&look_request,&empty,request);
        if(last_feedback==notice_index) return;
    }
    last_feedback=notice_index;
    kh_warn("player appearance: %s (frame %llu)",notice_lines[notice_index],(unsigned long long)frame);
    show_look_notice(notice_index,frame);
}
void kh_settings_frame(KCtx *g,uint64_t frame){
    int voice_busy=kh_look_voice_retry(g);
    if(g){
        retry_gauge(g);
        for(u64 e=deferred_resources,next;e;e=next){
            next=rd64(g,e);
            u64 resource=rd64(g,e+8);
            if(bind_resource(g,resource)) forget_deferred(g,resource);
        }
    }
    if(finishing_look>=0 && !voice_busy && !gauge_pending && !deferred_resources){
        int target=finishing_look;
        finishing_look=-1;
        if(!kh_settings_look_locked()){
            const char *value=target==2?"xion-hooded":target?"xion":"roxas";
            if(kh_save_live_option("KHNMC_PLAYER_LOOK",value,LOOK,target))
                kh_warn("appearance changed for this session: unable to save setting");
            int id=target==2?NOTICE_HOODED:target?NOTICE_XION:NOTICE_ROXAS;
            show_look_notice(id,frame);
        }
    }
    while(pending && pending->frame<=frame){
        struct change *c=pending; pending=c->next;
        if(kh_setting_set(c->name,c->value)) kh_fatal(NULL,"settings-save: %s",c->name);
        fprintf(stderr,"kh: settings frame %llu %s=%s\n",c->frame,c->name,c->value);
        free(c);
    }
    if(schedule_storage && !pending){free(schedule_storage);schedule_storage=NULL;}
    int action=kh_stick_action(frame);
    atomic_store(&kh_hide_hud,action==CHORD_HOLD && values[HUD_COMBO]!=0);
    if(g && !rd8(g,GS_BASE+0x9006b0)){
        character_row=-1;
        picking_character=0;
    }
    if(g && !voice_busy && !gauge_pending && !deferred_resources){
        int request=atomic_load(&look_request);
        if(request>=3){
            if(rd8(g,GS_BASE+0x9006b0) || kh_route_cond_eval(g,KR_BATTLE,0,frame,NULL)) request=-1;
            else if(rd16(g,GS_BASE+0x717008)!=choice_place || !kh_route_at_save_point(g) || kh_settings_look_locked()){
                atomic_compare_exchange_strong(&look_request,&request,-1);
                request=-1;
            }
        }
        if(request>=0 && !atomic_compare_exchange_strong(&look_request,&request,-1)) request=-1;
        if(request>=0 || last_switch) look_frame(g,request,frame);
    }
    if(mp_probe && frame==mp_probe){
        KhPlayerSheet p;
        if(!kh_route_player_sheet(g,&p) || p.mp<2) kh_fatal(g,"settings-mp-probe: no active player with MP");
        kh_call(g,GS_BASE+0x3d3cf0,p.obj,(u32)-1,0,0);
        fprintf(stderr,"kh: test MP cost frame=%llu before=%d after=%d\n",(unsigned long long)frame,p.mp,(s32)rd32(g,p.sheet+0x180));
    }
    if(!debug_enabled) return;
    pthread_mutex_lock(&config_lock);
    int cmd=debug_pending,arg[6];memcpy(arg,debug_arg,sizeof arg);debug_pending=-1;
    pthread_mutex_unlock(&config_lock);
    static s32 damage[2][4];static int active[2];
    if(cmd==TURBO){
        turbo=arg[0];float speed=turbo?turbo_speed:1.0f;memcpy(hp(g,GS_BASE+0x717424),&speed,4);
    }
    if(turbo) memcpy(hp(g,GS_BASE+0x717424),&turbo_speed,4);
    if(cmd==OHKO || cmd==INVINCIBLE){
        int type=cmd-OHKO;
        if(active[type]!=arg[0]) for(int i=0;i<4;i++){
            s32 *d=type?&kh_mod_difficulty_config[i].enemy_damage:&kh_mod_difficulty_config[i].party_damage;
            if(arg[0]){damage[type][i]=*d;*d=type?0:9999;}else *d=damage[type][i];
        }
        active[type]=arg[0];
    }
    if(cmd==WARP || cmd==HIDE || cmd==SHOW || (cmd==RESET && arg[0])){
        KhPlayerSheet p;
        if(!kh_route_player_sheet(g,&p)){fprintf(stderr,"kh: debug command requires an active field player\n");return;}
        if(cmd==RESET) wr8(g,GS_BASE+0xabac5a,1);
        else if(cmd==HIDE || cmd==SHOW) kh_call(g,GS_BASE+(cmd==HIDE?0x1629b0:0x162a00),arg[0],0,0,0);
        else{
            u64 place=ga_calloc(1,10);
            if(!place) kh_fatal(g,"debug-warp-allocation");
            for(int i=0;i<3;i++){wr8(g,place+i,arg[i]);wr16(g,place+4+2*i,arg[i+3]);}
            kh_call5(g,GS_BASE+0x152990,place,0,0,0,0);ga_free(place);
        }
    }
}

void orig_f_3c0860(KCtx *g),orig_f_3d3cf0(KCtx *g),orig_f_3ecf30(KCtx *g),orig_f_3dc170(KCtx *g);
void kh_drop_roll(KCtx *g);
static int controlled(KCtx *g){return A0(g) && A0(g)==rd64(g,GS_BASE+0x2a105d0);}
static int player_sheet(KCtx *g){
    u64 actor=rd64(g,GS_BASE+0x2a105d0);
    return actor && rd64(g,actor+0x5c0)==A0(g);
}
void f_3c0860(KCtx *g){
    if((values[HP] || test_log) && (s32)A1(g)<0 && !(u32)A2(g) && player_sheet(g)){
        if(test_log) fprintf(stderr,"kh: cheat HP before=%u delta=%d protected=%d\n",rd32(g,A0(g)),(s32)A1(g),!!values[HP]);
        if(values[HP]) A1(g)=0;
    }
    orig_f_3c0860(g);
}
void orig_f_3c20a0(KCtx *g);
void f_3c20a0(KCtx *g){
    if(values[MP] && player_sheet(g)){KH_RET(g);return;}
    orig_f_3c20a0(g);
}
void f_3d3cf0(KCtx *g){
    if((values[MP] || test_log) && (s32)A1(g)<0 && controlled(g)){
        if(test_log) fprintf(stderr,"kh: cheat MP delta=%d protected=%d\n",(s32)A1(g),!!values[MP]);
        if(values[MP]) A1(g)=0;
    }
    orig_f_3d3cf0(g);
}
static s32 scaled_award(s32 amount){
    double m=values[EXP];
    return m!=1 && amount>0?(s32)fmin(9999999,amount*m):amount;
}
void orig_f_17ca70(KCtx *);
void f_17ca70(KCtx *g){
    if(values[EXP]!=1) A1(g)=(u32)scaled_award((s32)A1(g));
    orig_f_17ca70(g);
}
void f_3ecf30(KCtx *g){
    s32 original=(s32)A0(g);
    if(values[EXP]!=1) A0(g)=(u32)scaled_award(original);
    if(test_log) fprintf(stderr,"kh: cheat EXP award=%d scaled=%d\n",original,(s32)A0(g));
    if(rd8(g,GS_BASE+0x3ed0bc)!=99){extern void kh_level_award(KCtx *); kh_level_award(g);}
    else orig_f_3ecf30(g);
    if(test_log) fprintf(stderr,"kh: cheat EXP save-total=%u\n",rd32(g,GS_BASE+0x9acf90));
}
float kh_drop_chance(float chance){return fmin(100.0,chance*values[DROP]);}
float kh_drop_roll_test(float chance,float roll,u16 item){
    float adjusted=values[DROP]==1?chance:kh_drop_chance(chance);
    if(test_log && item) fprintf(stderr,"kh: cheat DROP item=%u chance=%.4g scaled=%.4g rolled=%.6g won=%d\n",item,chance,adjusted,roll,adjusted>roll);
    return adjusted;
}
void f_3dc170(KCtx *g){
    if(values[DROP]==1 && !test_log) orig_f_3dc170(g);
    else kh_drop_roll(g);
}

#include "rewind.h"
void kh_rewind_look(KhRecords *r){
    u64 legacy_notice=0; /* former guest message buffer slot */
    KH_RECORD(r,legacy_notice);
    if(r->loading) atomic_store(&notice,0);
    KH_RECORD(r,art);
    KH_RECORD(r,held_resources);
    KH_RECORD(r,deferred_resources);
    KH_RECORD(r,character_labels);
    KH_RECORD(r,root_labels);
    KH_RECORD(r,character_row);
    KH_RECORD(r,choice_place);
    KH_RECORD(r,picking_character);
    KH_RECORD(r,gauge_pending);
    KH_RECORD(r,finishing_look);
    int request=atomic_load(&look_request);
    KH_RECORD(r,request);
    if(r->loading) atomic_store(&look_request,request);
    KH_RECORD(r,idle);
    KH_RECORD(r,face_actor);
    KH_RECORD(r,face_instance);
    KH_RECORD(r,face_model);
    KH_RECORD(r,face_bar);
    KH_RECORD(r,last_switch);
}
