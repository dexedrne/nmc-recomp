#include "khnmc_rt.h"
#include "settings.h"
#if KH_IOS_PATHS
#include "ios/touch_layout.h"
#endif
#include "frame_time.h"
#include "autosave.h"
#include "pad_layout.h"
#include "xion_swap.h"
#include "kh_look.h"
#include "title_logo.h"
#include "mod_config.h"
#include "route_state.h"
#include <math.h>
#include <zlib.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

extern int kh_run_is_selftest;
extern u64 kh_game_frames(void);
enum { LOOK, HP, MP, EXP, DROP, DRIVE, HUD_COMBO, COUNT, PAD=COUNT, SWAP_AB, STYLE, RAISE, ENEMY, SUB, LOWER, MENU_LAYOUT, LOGO, CREDIT, TITLE_STYLE, TUTORIAL, AUTO_SAVE, NAMES, DIFF_LABEL, FRAME_WORK, FRAME_LOG, TOUCH_MODE, TOUCH_SIZE, TOUCH_OPACITY, TOUCH_STYLE, TOUCH_LOOK, TOUCH_SLOTS, TOUCH_DPAD };
#define BOOL_OPT(name,label,initial) {"KHNMC_" name,label,initial,"0|1",0,1,1}
#define TWEAK_OPT(name,label,initial) {"KHNMC_TWEAK_" name,label,initial,"off|on",0,1,1}
#define NUMBER(name,label,initial,min,max,step) {"KHNMC_" name,label,initial,NULL,min,max,step}
const struct kh_option kh_options[]={
    {"KHNMC_PLAYER_LOOK","Character","roxas","roxas|xion|xion-hooded",0,0,0},
    BOOL_OPT("CHEAT_INF_HP","Infinite HP","0"), BOOL_OPT("CHEAT_INF_MP","Infinite MP","0"),
    NUMBER("EXP_MULT","EXP multiplier","1",0.5,10,0.5), NUMBER("DROP_MULT","Item drop multiplier","1",0.5,10,0.5),
    BOOL_OPT("CHEAT_INF_DRIVE","Infinite Drive","0"),
    BOOL_OPT("HUD_HIDE_COMBO","Hold L3 + R3 to hide HUD","0"),
    {"KHNMC_PAD_LAYOUT","Face buttons","auto","auto|positions|letters",0,0,0},
    BOOL_OPT("PAD_SWAP_AB","Swap confirm / back (A-B)","0"),
    NUMBER("HUD_SCALE_STYLE","Style rank size","2",0.5,3,0.05),
    NUMBER("HUD_STYLE_RAISE","Style rank raise","0.25",0,0.5,0.01),
    NUMBER("HUD_SCALE_ENEMY","Enemy bars","1.5",0.5,3,0.05),
    NUMBER("SUB_SCALE","Subtitle size","1.5",0.5,3,0.05),
    NUMBER("SUB_LOWER","Subtitle lower","0.06",0,0.5,0.01),
    {"KHNMC_MENU_LAYOUT","Portrait menu layout","portrait","portrait|band"},
    {"KHNMC_TITLE_LOGO","Title logo","auto","auto|cycle|mod|roxas|xion|xkb"},
    {"KHNMC_TITLE_CREDIT","Title credit","show","show|hide"},
    {"KHNMC_TITLE_STYLE","Title style","auto","auto|roxas|xion"},
    {"KHNMC_TUTORIAL_FIGHT","Tutorial fight","real","real|mod"},
    BOOL_OPT("AUTOSAVE","Autosave","0"),
    {"KHNMC_LOOK_NAMES","Names in text","all","all|menus"},
    BOOL_OPT("SAVE_DIFFICULTY","Save difficulty labels","1"),
    BOOL_OPT("FRAME_WORK","Frame workload log","0"),
    BOOL_OPT("FRAME_TIME","Frame time log","1"),
#if KH_IOS_PATHS
    {"KHNMC_TOUCH_MODE","Touch controls","Auto","Auto|Always|Off"},
    {"KHNMC_TOUCH_SIZE","Touch button size","M","S|M|L"},
    {"KHNMC_TOUCH_OPACITY","Touch button opacity","25","15|25|35|65|100"},
    {"KHNMC_TOUCH_STYLE","Touch style","Dynamic","Dynamic|Fixed"},
    {"KHNMC_TOUCH_LOOK","Look sensitivity (%)","100","50|75|100|125|150|200"},
    BOOL_OPT("TOUCH_SHORTCUTS","Shortcut buttons","1"),
    BOOL_OPT("TOUCH_DPAD","Show D-pad","0"),
#endif
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
/* Selftests run with the cheats at their defaults, so a player's saved cheats cannot change expected values. */
void kh_settings_cheats_default(double saved[5]){ for(int i=HP;i<=DRIVE;i++){ saved[i-HP]=values[i]; values[i]=(i==EXP||i==DROP)?1:0; } }
void kh_settings_cheats_restore(const double saved[5]){ for(int i=HP;i<=DRIVE;i++) values[i]=saved[i-HP]; }
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
static const char *option_dir(void){
#if KH_IOS_PATHS
    extern const char *kh_autosave_documents(void);
    return kh_autosave_documents();
#else
    return kh_docs;
#endif
}
static void load_saved_options(void){
    const char *dir=option_dir();
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
        for(int i=0;i<kh_option_count-DEBUG_COUNT-STATE_ACTIONS;i++) if(i!=LOOK && !strcmp(p,kh_options[i].name)){
            if(setenv(p,v,1)) kh_fatal(NULL,"settings-config-env: %s",p);
            if(test_log) fprintf(stderr,"kh: RMC restored %s=%s\n",p,v);
            break;
        }
    }
    if(ferror(in)) kh_fatal(NULL,"settings-config-read: %s",path);
    free(line);fclose(in);
}
static _Atomic int look_request=-1;
static int finishing_look=-1;
static int gauge_pending;
static u64 deferred_resources;
static u64 waiting_since;
void kh_settings_reset_look(void){
    int request=atomic_exchange(&look_request,-1);
    if(request>=0) kh_warn("player appearance: dropped %d place-changed",request%3);
    waiting_since=0;
    finishing_look=-1;
}

int kh_settings_hooded(void){return kh_look_pack_ready(2);}
int kh_settings_look(void){
    int request=atomic_load(&look_request);
    return request>=0?request%3:kh_look_enabled();
}
/* f_321e40 creates a four-row list. f_2eb490 builds two or three
 * stock rows, ending with Title Screen even when World Map is hidden. */
static int character_row=-1;
static _Atomic int enter_character;
static u16 choice_place;
/* A native Save reaction can open beyond the old 60-unit prototype radius. */
static struct { float x,y,z; int valid; } choice_position;
static int at_choice_point(KCtx *g){
    float x,y,z,yaw;
    if(choice_position.valid && kh_route_player_pos(g,&x,&y,&z,&yaw) &&
       fabsf(x-choice_position.x)<1 && fabsf(y-choice_position.y)<1 && fabsf(z-choice_position.z)<1) return 1;
    return kh_route_at_save_point(g);
}
static u64 character_labels[6];
enum { CHANGE_LABEL=3, CHARACTER_MSG=30016 };
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
        const char *labels[]={"Roxas","Xion","Xion (hooded)","Change Character"};
        for(int i=0;i<4;i++) character_labels[i]=kh_encode_menu_text(labels[i],i==CHANGE_LABEL);
    }
    character_row=(int)A1(g)+1;
    if(character_row<2 || character_row>3) kh_fatal(g,"save-character-row: %d",character_row);
    if(getenv("KHNMC_TEST_LOOK_DIGEST")) kh_warn("save-character built row %d",character_row);
    KCtx ctx=*g;
    /* f_2f8e20 reuses its fifth stack argument for the tail call to f_2f8ab0. */
    u64 args[4];
    memcpy(args,hp(g,sp+0x28),sizeof args);
    orig_f_2f8e20(g);
    KH_RSP(&ctx)=sp-0x100;
    wr64(&ctx,KH_RSP(&ctx),GS_BASE+0x2eb508);
    memcpy(hp(&ctx,KH_RSP(&ctx)+0x28),args,sizeof args);
    A1(&ctx)=character_row;
    wr32(&ctx,KH_RSP(&ctx)+0x30,0x8000| (CHARACTER_MSG+CHANGE_LABEL));
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
       kh_call(&ctx,GS_BASE+0x2f7fb0,menu,0,0,0)==character_row){
        kh_call(&ctx,GS_BASE+0x2fa1d0,0x8000|(CHARACTER_MSG+CHANGE_LABEL),1,0,0);
        KH_RET(g);return;
    }
    orig_f_2eb620(g);
}
void orig_f_2f9440(KCtx *);
void f_2f9440(KCtx *g){
    u64 caller=rd64(g,KH_RSP(g));
    orig_f_2f9440(g);
    if(caller!=GS_BASE+0x2e9ebf || character_row<0 || rd32(g,GS_BASE+0x900724)!=3) return;
    int row=(s32)KH_RAX(g);
    if(row==character_row){
        KH_RAX(g)=(u32)-1;
        atomic_store(&enter_character,1);
    }
}

static int menu_catalog_test(void);
static int notice_selftest(KCtx *g);
static int look_gate_selftest(KCtx *g);
static int next_multiplier(double,int);
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
    fail|=validate("KHNMC_TITLE_STYLE","auto",&v)!=TITLE_STYLE ||
          validate("KHNMC_TITLE_STYLE","roxas",&v)!=TITLE_STYLE ||
          validate("KHNMC_TITLE_STYLE","xion",&v)!=TITLE_STYLE ||
          validate("KHNMC_TITLE_STYLE","cycle",&v)>=0;
    fail|=validate("KHNMC_PLAYER_LOOK","xion",&v)!=LOOK || v!=1;
    const int presets[]={1,2,3,5,10};
    for(int i=0;i<5;i++)
        fail|=next_multiplier(presets[i],1)!=presets[(i+1)%5] ||
              next_multiplier(presets[i],0)!=presets[(i+4)%5];
    fail|=next_multiplier(2.5,1)!=3 || next_multiplier(2.5,0)!=2;
    fail|=menu_catalog_test();
    fail|=notice_selftest(g);
    fail|=look_gate_selftest(g);
    unlink(path);rmdir(dir);
    fprintf(stderr,"kh: settings persistence and vector validation: %d failures\n",fail);
    return fail;
}
static pthread_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
static int kh_save_live_option(const char *,const char *,int,double);
#if KH_IOS_PATHS
/* The app shell installs this before the game thread starts. Values come from validated catalog entries. */
void (*kh_ios_touch_changed)(KHTouchSettings);
static KHTouchSettings touch_settings;
static void touch_setting(int i,const char *v){
    switch(i){
    case TOUCH_MODE: touch_settings.mode=!strcmp(v,"Off")?2:!strcmp(v,"Always"); break;
    case TOUCH_SIZE: touch_settings.size=!strcmp(v,"L")?2:!strcmp(v,"M"); break;
    case TOUCH_OPACITY: touch_settings.opacity=atoi(v); break;
    case TOUCH_STYLE: touch_settings.fixed=!strcmp(v,"Fixed"); break;
    case TOUCH_LOOK: touch_settings.sensitivity=atoi(v); break;
    case TOUCH_SLOTS: touch_settings.shortcuts=atoi(v); break;
    case TOUCH_DPAD: touch_settings.dpad=atoi(v); break;
    }
}
static void publish_touch(void){
    if(kh_ios_touch_changed) kh_ios_touch_changed(touch_settings);
}
#endif
static _Atomic(const char *) live_choices[FRAME_LOG+1];
const char *kh_menu_choice(const char *name){
    for(int i=PAD;i<=FRAME_LOG;i++) if(!strcmp(name,kh_options[i].name)){
        const char *v=atomic_load(&live_choices[i]);
        if(v) return v;
        break;
    }
    return getenv(name);
}

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
    if(i==LOOK){
        int empty=-1;
        if(finishing_look>=0 || !atomic_compare_exchange_strong(&look_request,&empty,(int)v)) return -2;
        return 0;
    }
    return kh_save_live_option(name,value,i,v);
}
static int kh_save_live_option(const char *name,const char *value,int i,double v){
    const char *dir=option_dir();
    pthread_mutex_lock(&config_lock);
    int result=kh_run_is_selftest?0:save_option(dir,name,value);
#if KH_IOS_PATHS
    if(!result && i>=TOUCH_MODE && i<=TOUCH_DPAD){touch_setting(i,value);publish_touch();}
#endif
    if(!result && kh_setting_live(i)) atomic_store(&values[i],v);
    if(!result && ((i>=PAD && i<=FRAME_LOG) || strstr(name,"HUD_SCALE_") || !strcmp(name,"KHNMC_FOV_ZOOM"))){
        /* The render and controller threads can keep reading these immutable strings. */
        if(i==PAD || i==SWAP_AB || i==LOGO || i==TITLE_STYLE || i==NAMES || i==AUTO_SAVE){
            static const char *const choices[]={"auto","positions","letters","0","1","cycle","mod","roxas","xion","xkb","all","menus"};
            for(size_t j=0;j<sizeof choices/sizeof *choices;j++) if(!strcmp(value,choices[j])) live_choices[i]=choices[j];
        }
        extern void kh_menu_display_apply(const char *,const char *),kh_menu_tutorial(int),kh_menu_difficulty_labels(int);
        if((i>=STYLE && i<=MENU_LAYOUT) || i==CREDIT || strstr(name,"HUD_SCALE_") || !strcmp(name,"KHNMC_FOV_ZOOM")) kh_menu_display_apply(name,value);
        if(i==AUTO_SAVE) kh_autosave_init();
        if(i==TUTORIAL) kh_menu_tutorial(!strcmp(value,"real"));
        if(i==DIFF_LABEL) kh_menu_difficulty_labels((int)v);
        if(i==FRAME_WORK) kh_frame_work=(int)v;
        if(i==FRAME_LOG) kh_frame_log=(int)v;
    }
    pthread_mutex_unlock(&config_lock);
    return result;
}

struct change { unsigned long long frame; char *name,*value; struct change *next; };
static struct change *pending;
static char *schedule_storage;
static void init_notice_text(void);
void kh_settings_init(void){
    init_notice_text();
    test_log=getenv("KHNMC_TEST_CHEAT_LOG") && !strcmp(getenv("KHNMC_TEST_CHEAT_LOG"),"1");
    load_saved_options();
    char title_pack[4096];
    kh_title_logo_path(4,"2ld",title_pack,sizeof title_pack);
    if(access(title_pack,R_OK)) unavailable[TITLE_STYLE]="Title style pack missing";
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
        if(!v || i==LOOK) continue;
        double n;
        const char *reason=NULL;
        if(!strncmp(key,"KHNMC_TWEAK_",12) && kh_options[i].choices){
            if(strcasecmp(v,"on") && strcasecmp(v,"true") && strcmp(v,"1") &&
               strcasecmp(v,"off") && strcasecmp(v,"false") && strcmp(v,"0"))
                reason="Expected on or off";
        }else if(validate(key,v,&n)<0) reason="Invalid configured value";
        else if(!strcmp(key,"KHNMC_SCENE_ROXAS") && n && !kh_scene_pack_ready()) reason="Scene asset pack incomplete";
        if(!reason && (!strcmp(key,"KHNMC_FOV_ZOOM") || !strcmp(key,"KHNMC_HUD_SCALE_CMD") || !strcmp(key,"KHNMC_HUD_SCALE_HP"))){
            const char *aspect=getenv("KHNMC_ASPECT");
            if(!aspect || strcmp(aspect,"fill")) reason="Requires fill aspect";
        }
        if(reason){
            kh_warn("%s disabled: %s",key,reason);
            unavailable[i]=reason;
            unsetenv(key);
        }
    }
#if KH_IOS_PATHS
    for(int i=TOUCH_MODE;i<=TOUCH_DPAD;i++) touch_setting(i,getenv(kh_options[i].name)?:kh_options[i].initial);
    publish_touch();
#endif
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
    for(int i=HP;i<COUNT;i++){
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
enum { NOTICE_XION, NOTICE_ROXAS, NOTICE_FIELD, NOTICE_BUSY, NOTICE_ACTOR, NOTICE_PACK, NOTICE_HOODED, NOTICE_COUNT };
static const char *const notice_lines[]={"Xion","Roxas","Can't switch now: outside field","Can't switch now: busy",
    "Can't switch now: player unavailable","Can't switch now: missing art","Xion (hooded)"};
static u8 look_text[NOTICE_COUNT][40];
static _Atomic u64 notice;
u8 kh_look_font[65536];
static void encode_system_text(u8 *out,const char *text){
    while(*text){
        char c=*text++;
        if(c>='a' && c<='z') *out++=0x9a+c-'a';
        else if(c>='A' && c<='Z') *out++=0x2e + c-'A';
        else if(c>='0' && c<='9') *out++=0x21+c-'0';
        else switch(c){
            case ' ': *out++=1; break;
            case '\n': *out++=2; break;
            case '\'': *out++=0x57; break;
            case ':': *out++=0x52; break;
            case '(': *out++=0x5a; break;
            case ')': *out++=0x5b; break;
            case '/': *out++=0x4b; break;
            case '>': *out++=0x65; break;
            case '-': *out++=0x2c; break;
            case '+': *out++=0x2b; break;
            case '_': *out++=0x2c; break;
            case ',': *out++=0x4d; break;
            case '&': *out++=0x86; break;
            case '.': *out++=0x4e; break;
            default: *out++=0x49; break; /* localized controller element or vendor name */
        }
    }
    *out=0;
}
static void init_notice_text(void){
    for(int i=0;i<NOTICE_COUNT;i++) encode_system_text(look_text[i],notice_lines[i]);
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
/* Pad polling owns the cursor. The render thread reads it and the live options. */
static int next_multiplier(double current,int right){
    static const int steps[]={1,2,3,5,10};
    if(right){
        for(int i=0;i<5;i++) if(steps[i]>current) return steps[i];
        return 1;
    }
    for(int i=4;i>=0;i--) if(steps[i]<current) return steps[i];
    return 10;
}
/* camp.2ld's first IMGZ picture contains the stock framed window and pointing hand. */
static u8 camp_indices[65536],camp_clut[64];
static _Atomic int camp_ready;
static void load_camp_art(KCtx *g){
    if(camp_ready) return;
    KCtx ctx=*g;g=&ctx;
    u64 key=ga_strdup("menu/us/camp.2ld");
    int size=(int)kh_call(g,GS_BASE+0x3a09b0,key,0,0,0);
    if(size<32 || size>16*1024*1024) kh_fatal(g,"settings-camp-layout-size: %d",size);
    u64 bar=ga_alloc(size,16);
    if(!bar || (int)kh_call(g,GS_BASE+0x3f9850,key,bar,0,0)!=size || rd32(g,bar)!=0x01524142)
        kh_fatal(g,"settings-camp-layout-read");
    u32 count=rd32(g,bar+4);
    for(unsigned j=0;j<count && j<((unsigned)size-16)/16;j++){
        u64 entry=bar+16+16*j;
        if(rd32(g,entry)!=29) continue;
        u32 off=rd32(g,entry+8),len=rd32(g,entry+12);
        if(off>(unsigned)size || len>(unsigned)size-off || len<24) break;
        u64 imgz=bar+off;
        u32 index=rd32(g,imgz+8);
        if(rd32(g,imgz)!=0x5a474d49 || !rd32(g,imgz+12) || index>len-8) break;
        u32 pic=rd32(g,imgz+index),bytes=rd32(g,imgz+index+4);
        if(pic>len || bytes>len-pic || bytes<64) break;
        u64 im=imgz+pic;
        u32 pixels=rd32(g,im+8),pal=rd32(g,im+16);
        if(rd32(g,im)!=0x44474d49 || rd32(g,im+28)!=0x01000200 || rd32(g,im+12)!=65536 ||
           rd32(g,im+20)!=64 || bytes<65536 || pixels>bytes-65536 || pal>bytes-64 || (rd32(g,im+60)&4)) break;
        memcpy(camp_indices,hp(g,im+pixels),sizeof camp_indices);
        memcpy(camp_clut,hp(g,im+pal),sizeof camp_clut);
        camp_ready=1;break;
    }
    ga_free(key);ga_free(bar);
    if(!camp_ready) kh_fatal(g,"settings-camp-atlas-format");
}
uint32_t kh_menu_texel(unsigned x,unsigned y,uint32_t bg){
    unsigned ix=(camp_indices[y*256+x/2]>>(4*(x&1)))&15;
    const u8 *c=camp_clut+4*ix;
    unsigned alpha=c[3]>128?128:c[3],color=0xff000000;
    for(unsigned i=0;i<3;i++) color|=((c[2-i]*alpha+((bg>>(8*i))&255)*(128-alpha))/128)<<(8*i);
    return color;
}
enum { ROOT, CHARACTER, CHEATS, CONTROLLER, DISPLAY, GAMEPLAY, ABOUT, ADVANCED, INPUT_TEST };
static const char *const pages[]={"Roxas May Cry","Character","Cheats","Controller","Display","Gameplay","About","Advanced","Input test"};
static _Atomic int title_screen,menu_open;
static int page,row,component,advanced_row,direct_character,preview_look,first_section;
enum { VALUE_CAP=1001 }; /* validate() caps persisted strings at 1000 bytes. */
static char chosen[sizeof kh_options/sizeof *kh_options][VALUE_CAP];
static const char *feedback;
static pthread_mutex_t panel_lock=PTHREAD_MUTEX_INITIALIZER;
#if KH_IOS_PATHS
static _Atomic int touch_device;
#endif
static char input_device[48],input_element[48];
void kh_menu_input_device(const char *device,const char *element){
    pthread_mutex_lock(&panel_lock);
#if KH_IOS_PATHS
    touch_device=device && !strcmp(device,"Touch controls");
#endif
    snprintf(input_device,sizeof input_device,"%.45s",device?:"");
    snprintf(input_element,sizeof input_element,"%.45s",element?:"");
    pthread_mutex_unlock(&panel_lock);
}
unsigned short kh_menu_map_buttons(unsigned short buttons,int nintendo){
    const char *layout=kh_menu_choice("KHNMC_PAD_LAYOUT")?:"auto";
    const char *ab=kh_menu_choice("KHNMC_PAD_SWAP_AB");
    int xy=nintendo>=0 && kh_pad_swap_xy(layout,nintendo,getenv("KHNMC_PAD_SWAP_XY"));
    int swap=(nintendo>=0 && !strcmp(layout,"letters")) ^ (ab && *ab=='1');
    return (buttons&0xfff) | kh_pad_faces(buttons&0x1000,buttons&0x2000,buttons&0x4000,buttons&0x8000,xy,swap);
}
static u8 panel_text[KH_MENU_TEXT_SIZE];

void kh_menu_title(int screen){
    int previous=atomic_exchange(&title_screen,screen);
    if(test_log && screen==KH_TITLE_SETTINGS && screen!=previous) fprintf(stderr,"kh: title GAME SETTINGS entered\n");
}
int kh_menu_active(void){return menu_open;}
int kh_title_settings_key(void){return title_screen && !menu_open;}
enum { INPUT_ROW=-1, ADVANCED_ROW=-2 };
static int section_rows(int section,int *ids){
    int n=0;
    for(int i=0;i<kh_setting_menu_count();i++){
        int group=ADVANCED;
        if(i==LOOK) group=CHARACTER;
        else if(i>=HP && i<=DRIVE) group=CHEATS;
#if KH_IOS_PATHS
        else if(i>=TOUCH_MODE && i<=TOUCH_DPAD) group=CONTROLLER;
#endif
        else if(i==PAD || i==SWAP_AB) group=CONTROLLER;
        else if(i==HUD_COMBO || (i>=STYLE && i<=TITLE_STYLE) || (i==FRAME_WORK || i==FRAME_LOG) ||
                strstr(kh_options[i].name,"HUD_SCALE_") || !strcmp(kh_options[i].name,"KHNMC_FOV_ZOOM")) group=DISPLAY;
        else if(i>=TUTORIAL && i<=DIFF_LABEL) group=GAMEPLAY;
        if(group==section && i!=TITLE_STYLE) ids[n++]=i;
    }
    if(section==DISPLAY) ids[n++]=TITLE_STYLE;
    if(section==CONTROLLER) ids[n++]=INPUT_ROW;
    if(section==GAMEPLAY) ids[n++]=ADVANCED_ROW;
    return n;
}
static int menu_catalog_test(void){
    int ids[sizeof kh_options/sizeof *kh_options+1],seen[sizeof kh_options/sizeof *kh_options]={0};
    for(int section=CHARACTER;section<=ADVANCED;section++){
        int n=section_rows(section,ids);
        if(!n && section!=ABOUT) return 1;
        for(int j=0;j<n;j++) if(ids[j]>=0) seen[ids[j]]++;
    }
    for(int i=0;i<kh_setting_menu_count();i++){
        u8 encoded[128];
        encode_system_text(encoded,kh_options[i].label);
        double value;
        if(seen[i]!=1 || validate(kh_options[i].name,kh_options[i].initial,&value)!=i) return 1;
    }
#if KH_IOS_PATHS
    int n=section_rows(CONTROLLER,ids);
    if(n!=10 || ids[9]!=INPUT_ROW) return 1;
    KHTouchSettings saved=touch_settings;
    for(int i=TOUCH_MODE;i<=TOUCH_DPAD;i++){
        if(ids[2+i-TOUCH_MODE]!=i) return 1;
        touch_setting(i,kh_options[i].initial);
    }
    int defaults=touch_settings.opacity==25 && !touch_settings.fixed && touch_settings.sensitivity==100 &&
                 touch_settings.shortcuts && !touch_settings.dpad;
    touch_setting(TOUCH_STYLE,"Fixed"); touch_setting(TOUCH_LOOK,"150");
    touch_setting(TOUCH_SLOTS,"0"); touch_setting(TOUCH_DPAD,"1");
    touch_setting(TOUCH_MODE,"Always"); touch_setting(TOUCH_SIZE,"L"); touch_setting(TOUCH_OPACITY,"35");
    int ok=defaults && touch_settings.mode==1 && touch_settings.size==2 && touch_settings.opacity==35 &&
           touch_settings.fixed && touch_settings.sensitivity==150 && !touch_settings.shortcuts && touch_settings.dpad;
    touch_setting(TOUCH_MODE,"Off"); touch_setting(TOUCH_SIZE,"S");
    ok=ok && touch_settings.mode==2 && touch_settings.size==0;
    touch_settings=saved;
    const char *layout=atomic_exchange(&live_choices[PAD],"letters");
    const char *ab=atomic_exchange(&live_choices[SWAP_AB],"0");
    ok=ok && kh_menu_map_buttons(0x5050,-1)==0x5050 && kh_menu_map_buttons(0x5050,1)==0xa050;
    live_choices[SWAP_AB]="1";
    ok=ok && kh_menu_map_buttons(0x5050,-1)==0x6050;
    live_choices[PAD]=layout; live_choices[SWAP_AB]=ab;
    if(!ok) return 1;
#endif
    return 0;
}
static void option_text(int i,char *out,size_t cap){
    if(i==LOOK){
        static const char *const names[]={"roxas","xion","xion-hooded"};
        snprintf(out,cap,"%s",names[direct_character?preview_look:kh_settings_look()]);
    }else if(i==TITLE_STYLE) snprintf(out,cap,"%s",kh_menu_choice(kh_options[i].name)?:"auto");
    else if(kh_setting_live(i)) snprintf(out,cap,"%g",kh_live_setting_value(i));
    else snprintf(out,cap,"%s",chosen[i][0]?chosen[i]:getenv(kh_options[i].name)?:kh_options[i].initial);
}
static const char *field_blocked(KCtx *g,int native_menu){
    if(!g) return "Outside field";
    if(rd8(g,GS_BASE+0x9006b0) && !native_menu) return "Game menu open";
    if(rd64(g,GS_BASE+0x2a11268)) return "Special player active";
    if(rd64(g,GS_BASE+0x9ba928) || rd8(g,GS_BASE+0x9ba8d1) ||
       rd64(g,GS_BASE+0x2a11478) || kh_scene_pack_active()) return "Cutscene active";
    if(rd64(g,GS_BASE+0x2a244d8) || rd8(g,GS_BASE+0x2a11428) ||
       rd64(g,GS_BASE+0x2ae9fa8) || rd32(g,GS_BASE+0x2a10504)) return "Field transition";
    u64 cmds; int windows,waiting;
    kh_route_field_parts(g,&cmds,&windows,&waiting);
    if(!native_menu && (!cmds || windows)) return "Waiting for field";
    return NULL;
}
static const char *look_blocked(KCtx *g,int native_menu){
    const char *blocked=field_blocked(g,native_menu);
    if(blocked) return blocked;
    if(rd16(g,GS_BASE+0x9acdd4)) return "Drive Form active";
    /* f_3abb20 supplies the native field/battle state to the music and command menus. */
    if(rd32(g,GS_BASE+0x2a11404)) return "Battle active";
    KhPlayerSheet player;
    char stem[40],path[64];
    if(!kh_route_player_sheet(g,&player) || player.hp<=0 ||
       !kh_route_object_name(g,player.obj,stem,sizeof stem)) return "Player unavailable";
    snprintf(path,sizeof path,"obj/%s.mdlx",stem);
    if(kh_player_asset_key(path)!=-1) return "Special player active";
    /* Forced forms such as the opening P_EX100_BTLF can leave the saved form word zero. */
    size_t len=strlen(stem);
    return len && stem[len-1]=='F'?"Drive Form active":NULL;
}
static const char *picker_reason(KCtx *g,int save){
    if(atomic_load(&look_request)>=0 || finishing_look>=0 || gauge_pending || deferred_resources)
        return "Character still loading";
    return look_blocked(g,save);
}
static int look_gate_selftest(KCtx *g){
    static const struct { u32 address; const char *reason; int native_menu; } cases[]={
        {0x9006b0,"Game menu open",0}, {0x9ba8d1,"Cutscene active",1},
        {0x2a11428,"Field transition",1}, {0x9acdd4,"Drive Form active",1},
        {0x2a11404,"Battle active",1}
    };
    int fail=0;
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++){
        u64 addr=GS_BASE+cases[i].address;
        u8 previous=rd8(g,addr);
        wr8(g,addr,1);
        const char *reason=picker_reason(g,cases[i].native_menu);
        fail|=!reason || strcmp(reason,cases[i].reason)!=0;
        wr8(g,addr,previous);
    }
    int previous=finishing_look;
    finishing_look=2;
    fail|=strcmp(picker_reason(g,0),"Character still loading")!=0 ||
        kh_setting_set("KHNMC_PLAYER_LOOK","roxas")!=-2 || atomic_load(&look_request)!=-1;
    finishing_look=previous;
    return fail;
}
static void select_look(KCtx *g,int target,int save){
    if(save){
        float yaw;
        choice_place=rd16(g,GS_BASE+0x717008);
        choice_position.valid=kh_route_player_pos(g,&choice_position.x,&choice_position.y,&choice_position.z,&yaw);
    }
    atomic_store(&look_request,target+(save?3:0));
    kh_warn("%s-character chose %d",save?"save":"field",target);
}
static const char *timing(int i,const char *blocked){
    if(i<0) return "A to open";
    if(i==LOOK) return blocked?:direct_character?"A to confirm   B to cancel":"Left / Right to switch";
    const char *reason=kh_setting_unavailable(i);
    if(reason) return reason;
    if(kh_setting_action(i)) return "A to queue on close";
#if KH_IOS_PATHS
    if(i>=TOUCH_MODE && i<=TOUCH_DPAD) return "Applies immediately";
#endif
    if(i==TITLE_STYLE) return "Auto follows Character";
    if(i==LOGO) return "Next title load";
    if(i==NAMES) return "Next text lookup";
    if(i==TUTORIAL) return "Next tutorial fight";
    if(i==DIFF_LABEL) return "Next save list";
    if(i==HUD_COMBO || i<=FRAME_LOG || strstr(kh_options[i].name,"HUD_SCALE_") || !strcmp(kh_options[i].name,"KHNMC_FOV_ZOOM")) return "Applies immediately";
    return "Applies on next launch";
}
static void edit_row(KCtx *g,int i,int direction,const char *blocked){
    const struct kh_option *o=&kh_options[i];
    if(kh_setting_unavailable(i) || (i==LOOK && blocked)) return;
    char old[VALUE_CAP],value[VALUE_CAP];option_text(i,old,sizeof old);
    if(o->choices){
        char list[128];snprintf(list,sizeof list,"%s",o->choices);
        char *rest=list,*v,*choices[16];int count=0,index=0;
        while((v=strsep(&rest,"|"))){if(!strcmp(old,v)) index=count;choices[count++]=v;}
        snprintf(value,sizeof value,"%s",choices[(index+direction+count)%count]);
    }else if(i==EXP || i==DROP) snprintf(value,sizeof value,"%d",next_multiplier(atof(old),direction>0));
    else{
        char *field=old;
        for(int j=0;j<component;j++){field=strchr(field,',');if(!field) return;field++;}
        char *end;double number=strtod(field,&end);
        snprintf(value,sizeof value,"%.*s%.8f",(int)(field-old),old,fmin(o->maximum,fmax(o->minimum,number+direction*o->step)));
        char *tail=value+strlen(value);while(tail>value && tail[-1]=='0') tail--;
        if(tail>value && tail[-1]=='.') tail--;
        if(strlen(end)>=sizeof value-(size_t)(tail-value)){feedback="Value too long";return;}
        strcpy(tail,end);
    }
    if(i==LOOK){
        int target=!strcmp(value,"roxas")?0:!strcmp(value,"xion")?1:2;
        if(target && !kh_look_pack_ready(target)){feedback="Missing character pack";return;}
        select_look(g,target,0);
    }else if(!strcmp(o->name,"KHNMC_SCENE_ROXAS") && !strcmp(value,"1") && !kh_scene_pack_ready()){
        feedback="Scene asset pack incomplete"; return;
    }else if(!kh_setting_action(i) && kh_setting_set(o->name,value)){
        kh_warn("settings-save: %s",o->name); feedback="Could not save option";return;
    }
    snprintf(chosen[i],sizeof chosen[i],"%s",value);
    if(test_log) fprintf(stderr,"kh: RMC %s=%s\n",o->name,value);
}
int kh_menu_pad(struct KCtx *g,unsigned buttons,int lt,int rt,int lx,int ly,int rx,int ry){
#if KH_IOS_PATHS
    if(menu_open && page!=INPUT_TEST && touch_device){
        if(abs(lx)>abs(ly)) buttons|=lx>16000?8:lx< -16000?4:0;
        else buttons|=ly>16000?1:ly< -16000?2:0;
    }
#endif
    static unsigned previous;static int releasing;
    unsigned down=buttons & ~previous;previous=buttons;
    if(releasing){releasing=buttons!=0;return releasing;}
    int chord=(buttons&0x50)==0x50 && (down&0x50);
    if(!menu_open){
        if(title_screen==KH_TITLE_LIST && !unavailable[TITLE_STYLE] && (buttons&0x100) && !(buttons&~0x100)){
            if(down&0x100){
                const char *style=kh_menu_choice("KHNMC_TITLE_STYLE");
                int xion=style && strcmp(style,"auto")?!strcmp(style,"xion"):kh_look_enabled()!=0;
                if(kh_setting_set("KHNMC_TITLE_STYLE",xion?"roxas":"xion")) kh_fatal(g,"title-style-save");
            }
            return 1;
        }
        direct_character=atomic_exchange(&enter_character,0);
        if(!direct_character && !chord && !(title_screen && (down&0x200))) return 0;
        if(!direct_character && !title_screen && !rd8(g,GS_BASE+0x9006b0)){
            if(!kh_route_cond_eval(g,KR_FIELD,0,kh_game_frames(),NULL)) return 0;
            if(rd64(g,GS_BASE+0x9ba928) || rd8(g,GS_BASE+0x9ba8d1)) return 0;
        }
        first_section=title_screen?CHEATS:CHARACTER;
        preview_look=kh_settings_look();
        load_camp_art(g);
        page=direct_character?CHARACTER:ROOT;row=component=0;feedback=NULL;menu_open=1;down=0;chord=0;
    }
    int ids[sizeof kh_options/sizeof *kh_options+1];
    int n=page==ROOT?ABOUT-first_section+1:section_rows(page,ids);
    int i=n && page!=ROOT?ids[row]:-1;
    const char *blocked=page==CHARACTER?picker_reason(g,direct_character):NULL;
    if(down && page!=INPUT_TEST){
        KCtx ctx=*g;
        kh_call(&ctx,GS_BASE+0x2eb740,(down&0x2000)?3:(down&0x1000)?2:1,1,0,0);
    }
    int before=page;
    if(down) feedback=NULL;
    if(page==INPUT_TEST){
        if(chord){page=CONTROLLER;row=section_rows(CONTROLLER,ids)-1;}
    }else if(title_screen && page==CHEATS && (down&0x200)){
        menu_open=0;releasing=1;return 1;
    }else if(chord || (down&0x2000)){
        if(page==ROOT || chord || direct_character){menu_open=0;releasing=1;return 1;}
        if(page==ADVANCED){page=GAMEPLAY;row=advanced_row;}
        else {row=page-first_section;page=ROOT;}
        component=0;
    }else if(direct_character && (down&15) && !blocked){
        preview_look=(preview_look+((down&5)?2:1))%3;
    }else if(n && (down&3)){row=(row+n+((down&1)?-1:1))%n;component=0;}
    else if(down&0x1000){
        if(direct_character){
            if(blocked) feedback=blocked;
            else if(preview_look && !kh_look_pack_ready(preview_look)) feedback="Missing character pack";
            else select_look(g,preview_look,1);
            if(!feedback){menu_open=0;releasing=1;return 1;}
        }
        if(page==ROOT){page=row+first_section;row=0;}
        else if(i==ADVANCED_ROW){advanced_row=row;page=ADVANCED;row=0;}
        else if(i==INPUT_ROW && page==CONTROLLER) page=INPUT_TEST;
        else if(i>=0 && kh_setting_action(i)){
            char value[VALUE_CAP];option_text(i,value,sizeof value);
            feedback=kh_setting_set(kh_options[i].name,value)?"Command unavailable":"Command queued";
        }
    }else if(page!=ROOT && i>=0 && (down&12)) edit_row(g,i,(down&8)?1:-1,blocked);
    else if(page==ADVANCED && i>=0 && (down&0x300)){
        int fields=1;for(const char *p=kh_options[i].initial;*p;p++) fields+=*p==',';
        component=(component+fields+((down&0x200)?1:-1))%fields;
    }
    if(test_log && before!=page) fprintf(stderr,"kh: RMC section %s frame=%llu\n",pages[page],(unsigned long long)kh_game_frames());
    char buf[KH_MENU_TEXT_SIZE];int used=snprintf(buf,sizeof buf,"%s\n",pages[page]);
    if(page==ABOUT) snprintf(buf+used,sizeof buf-used,"Version 0.1.0\nBuild T87  %s\nB to return",__DATE__);
    else if(page==INPUT_TEST){
        pthread_mutex_lock(&panel_lock);
        if(input_device[0]) used+=snprintf(buf+used,sizeof buf-used,"%s\n",input_device);
        if(input_element[0]) used+=snprintf(buf+used,sizeof buf-used,"Raw: %s\n",input_element);
        pthread_mutex_unlock(&panel_lock);
        static const char *const names[]={"Up","Down","Left","Right","Options","Create","L3","R3","L1","R1","","","Cross","Circle","Square","Triangle"};
        for(int bit=0;bit<16;bit++) if(buttons&(1u<<bit))
            used+=snprintf(buf+used,sizeof buf-used,"%s ",names[bit]);
        snprintf(buf+used,sizeof buf-used,"\nLT %d  RT %d\nLeft %d / %d\nRight %d / %d\nL3 and Options to return",lt,rt,lx,ly,rx,ry);
    }
    else{
        n=page==ROOT?ABOUT-first_section+1:section_rows(page,ids);
        int first=row<5?0:row-4;
        for(int j=first;j<n && j<first+6;j++){
            int opt=page==ROOT?-1:ids[j];
            const char *label=page==ROOT?pages[j+first_section]:opt==INPUT_ROW?"Input test":opt==ADVANCED_ROW?"Advanced":kh_options[opt].label;
            char value[VALUE_CAP]="";
            if(opt>=0) option_text(opt,value,sizeof value);
            if(opt==TITLE_STYLE){
                const char *style=!strcmp(value,"auto")?"Auto (follow Character)":!strcmp(value,"xion")?"Xion":"Roxas";
                snprintf(value,sizeof value,"%s",style);
            }
            used+=snprintf(buf+used,sizeof buf-used,"%c %.42s%s%.32s\n",j==row?'>':' ',label,opt>=0?": ":"",value);
        }
        const char *hint=page==ROOT?"A to open   B to close":timing(ids[row],blocked);
        if(page==ADVANCED && strchr(kh_options[ids[row]].initial,',')){
            char value[VALUE_CAP];option_text(ids[row],value,sizeof value);
            char *part=value;
            for(int k=0;k<component && part;k++) {part=strchr(part,',');if(part) part++;}
            if(part) used+=snprintf(buf+used,sizeof buf-used,"L1/R1 value %d: %.*s\n",component+1,(int)fmin(32,strcspn(part,",")),part);
        }
        if(page==ADVANCED && before!=page) feedback=kh_cut_menu_status();
        snprintf(buf+used,sizeof buf-used,"%s",feedback?:hint);
    }
    pthread_mutex_lock(&panel_lock);encode_system_text(panel_text,buf);pthread_mutex_unlock(&panel_lock);
    return 1;
}
int kh_menu_text(uint8_t text[KH_MENU_TEXT_SIZE]){
    if(!menu_open && !title_screen) return 0;
    int ready=atomic_load(&font_ready);
    if(ready<0) kh_fatal(NULL,"settings-menu-font: unsupported system font");
    if(!ready) return 0;
    if(!menu_open){encode_system_text(text,"R1/RB  Roxas May Cry");return 1;}
    pthread_mutex_lock(&panel_lock);memcpy(text,panel_text,sizeof panel_text);pthread_mutex_unlock(&panel_lock);
    return text[0]?2:0;
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
static u64 held_resources;
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
/* The camp model allocator needs room for Xion's extra mesh groups. */
static u64 camp_preview_pool;
void orig_f_19c3a0(KCtx *);
void f_19c3a0(KCtx *g){
    if(A0(g)==GS_BASE+0x938730 && A1(g)==0x68000 && kh_look_enabled()){
        if(!camp_preview_pool) camp_preview_pool=ga_alloc(2*1024*1024,128);
        if(!camp_preview_pool) kh_fatal(g,"camp-preview-heap");
        A0(g)=camp_preview_pool;
        A1(g)=2*1024*1024;
    }
    orig_f_19c3a0(g);
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
    if(refresh && field_blocked(g,0)) return;
    int notice_index=NOTICE_ACTOR;
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
    kh_warn("player appearance: dropped %d %s",target,notice_lines[notice_index]);
    show_look_notice(notice_index,frame);
}
static void refill_drive(KCtx *g,u64 actor){
    u64 sheet=actor?rd64(g,actor+0x5c0):0;
    if(!sheet) return;
    u8 max=rd8(g,sheet+0x1b2);
    if(max){
        wr8(g,sheet+0x1b1,max);
        wr8(g,sheet+0x1b0,100);
    }
    /* f_3c2740 initializes mode 2 for forms. Mode 3 spends the summon timer. */
    if(rd8(g,sheet+0x1af)==2) wr32(g,sheet+0x1b4,rd32(g,sheet+0x1b8));
}
void kh_settings_frame(KCtx *g,uint64_t frame){
    int voice_busy=kh_look_voice_retry(g);
    if(g){
        if(values[DRIVE]){
            refill_drive(g,rd64(g,GS_BASE+0x2a105d0));
            refill_drive(g,kh_xion_progress_player());
        }
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
        int id=target==2?NOTICE_HOODED:target?NOTICE_XION:NOTICE_ROXAS;
        show_look_notice(id,frame);
    }
    while(pending && pending->frame<=frame){
        struct change *c=pending; pending=c->next;
        int result=kh_setting_set(c->name,c->value);
        if(result==-2 && !strcmp(c->name,kh_options[LOOK].name)) kh_warn("player appearance: ignored while loading");
        else if(result) kh_fatal(NULL,"settings-save: %s",c->name);
        fprintf(stderr,"kh: settings frame %llu %s=%s\n",c->frame,c->name,c->value);
        free(c);
    }
    if(schedule_storage && !pending){free(schedule_storage);schedule_storage=NULL;}
    int action=kh_stick_action(frame);
    atomic_store(&kh_hide_hud,action==CHORD_HOLD && values[HUD_COMBO]!=0);
    if(g && !rd8(g,GS_BASE+0x9006b0)) character_row=-1;
    if(g && !voice_busy && !gauge_pending && !deferred_resources){
        int request=atomic_load(&look_request);
        if(request>=0){
            int save=request>=3;
            const char *drop=NULL;
            if(save && rd16(g,GS_BASE+0x717008)!=choice_place) drop="place-changed";
            else if(save && (menu_open || rd8(g,GS_BASE+0x9006b0))) request=-1;
            else if(save && !at_choice_point(g)) drop="not-save-point";
            else {
                const char *why=look_blocked(g,0);
                if(why && save){
                    if(!waiting_since || frame<waiting_since) waiting_since=frame;
                    if(frame-waiting_since<=KR_BATTLE_RECENT) request=-1;
                    else drop=!strcmp(why,"Battle active")?"battle-timeout":why;
                }else drop=why;
            }
            if(drop){kh_warn("player appearance: dropped %d %s",request%3,drop);
                atomic_store(&look_request,-1);request=-1;waiting_since=0;}
            if(request>=0) waiting_since=0;
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
    if(!debug_enabled || kh_menu_active()) return;
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
    /* Older snapshots stored an untagged guest message pointer here. */
    u64 legacy_notice=camp_preview_pool?camp_preview_pool|(1ull<<63):0;
    KH_RECORD(r,legacy_notice);
    if(r->loading) camp_preview_pool=(legacy_notice>>63)?legacy_notice & ~(1ull<<63):0;
    if(r->loading){notice=0;menu_open=0;enter_character=0;}
    KH_RECORD(r,art);
    KH_RECORD(r,held_resources);
    KH_RECORD(r,deferred_resources);
    KH_RECORD(r,character_labels);
    u64 legacy_root_labels[4]={0};
    if(!r->loading) memcpy(legacy_root_labels,&choice_position,sizeof choice_position);
    KH_RECORD(r,legacy_root_labels);
    if(r->loading){
        memcpy(&choice_position,legacy_root_labels,sizeof choice_position);
        choice_position.valid=choice_position.valid==1;
        waiting_since=0;
    }
    KH_RECORD(r,character_row);
    KH_RECORD(r,choice_place);
    int legacy_picker=0;
    KH_RECORD(r,legacy_picker);
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
