#include "khnmc_rt.h"
#include "settings.h"
#include "mod_config.h"
#include "route_state.h"
#include <math.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

extern int kh_run_is_selftest;
enum { HP, MP, EXP, DROP, HUD_COMBO, COUNT };
#define BOOL_OPT(name,label,initial) {"KHNMC_" name,label,initial,"0|1",0,1,1}
#define TWEAK_OPT(name,label,initial) {"KHNMC_TWEAK_" name,label,initial,"off|on",0,1,1}
#define NUMBER(name,label,initial,min,max,step) {"KHNMC_" name,label,initial,NULL,min,max,step}
const struct kh_option kh_options[]={
    BOOL_OPT("CHEAT_INF_HP","Infinite HP","0"), BOOL_OPT("CHEAT_INF_MP","Infinite MP","0"),
    NUMBER("EXP_MULT","EXP multiplier","1",0.5,10,0.5), NUMBER("DROP_MULT","Item drop multiplier","1",0.5,10,0.5),
    BOOL_OPT("HUD_HIDE_COMBO","Hold L3 + R3 to hide HUD (not at save points)","0"),
    TWEAK_OPT("DEBUG_DEBUG_ON","Enable debug commands","off"),
    TWEAK_OPT("DEBUG_SOFT_RESET","Allow debug reset to title","off"),
    NUMBER("TWEAK_DEBUG_TURBO_SPEED","Debug turbo speed","7.5",0.1,100,0.1),
    TWEAK_OPT("RETRY_RETRY","Retry battle","off"), TWEAK_OPT("SAVEPOINT_SAVEPOINT_QOL","Restore Drive at savepoints","off"),
#define KH_GROUP(key,env,label) TWEAK_OPT(#env,label,"on"),
#include "tweak_groups.h"
#undef KH_GROUP
    {"KHNMC_PLAYER_LOOK","Player appearance","roxas","roxas|xion",0,0,0},
    BOOL_OPT("XION_SWAP","Xion swap","0"), BOOL_OPT("SCENE_ROXAS","Scene Roxas","0"),
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
int kh_setting_menu_count(void){return kh_option_count-(debug_enabled?(allow_reset?0:1):DEBUG_COUNT);}
static _Atomic double values[COUNT]={0,0,1,1};
int kh_setting_live(int index){return index>=0 && index<COUNT;}

static int validate(const char *name,const char *value,double *number){
    if(!name || !value || !*value || strlen(value)>1000) return -1;
    for(int i=0;i<kh_option_count;i++) if(!strcmp(name,kh_options[i].name)){
        const struct kh_option *o=&kh_options[i];
        if(o->choices){
            const char *p=o->choices;
            while(*p){
                size_t n=strcspn(p,"|");
                if(strlen(value)==n && !strncmp(p,value,n)){*number=!strcmp(value,"1");return i;}
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
static int hide_combo(int,unsigned);
int kh_settings_selftest(void){
    char dir[KH_TMP_MAX],path[KH_TMP_MAX+16],data[200];
    kh_tmp_path(dir,sizeof dir,"khnmc-settings-XXXXXX");
    if(!mkdtemp(dir)) return 1;
    snprintf(path,sizeof path,"%s/khnmc.conf",dir);
    FILE *f=fopen(path,"w");int fail=!f;
    if(f){fputs("# keep\n env KHNMC_EXP_MULT=1\narg --selftest",f);fail|=fclose(f)!=0;}
    if(!fail) fail=save_option(dir,"KHNMC_EXP_MULT","2")!=0;
    f=fopen(path,"r");
    if(!f) fail=1;
    else{
        size_t n=fread(data,1,sizeof data-1,f);data[n]=0;fclose(f);
        fail|=strcmp(data,"# keep\narg --selftest\nenv KHNMC_EXP_MULT=2\n")!=0;
    }
    double v;
    fail|=validate("KHNMC_SAVE_STATE","1",&v)<0 || v!=1;
    fail|=validate("KHNMC_LOAD_STATE","3",&v)<0 || v!=3;
    fail|=validate("KHNMC_LOAD_STATE","4",&v)>=0 || validate("KHNMC_SAVE_STATE","1.5",&v)>=0;
    fail|=!kh_setting_action(validate("KHNMC_SAVE_STATE","2",&v));
    fail|=validate("KHNMC_TWEAK_WORLDS_SYMBOL_ZZ","12,1,2,3,256",&v)>=0;
    fail|=validate("KHNMC_TWEAK_FORMS_VALOR_GROWTH","1,2",&v)>=0;
    fail|=validate("KHNMC_TWEAK_DEBUG_TURBO_SPEED","7.5",&v)<0;
    fail|=validate("KHNMC_HUD_HIDE_COMBO","1",&v)!=HUD_COMBO || !hide_combo(1,0xc0) || hide_combo(0,0xc0) || hide_combo(1,0x40) || hide_combo(1,0x80);
    unlink(path);rmdir(dir);
    fprintf(stderr,"kh: settings persistence and vector validation: %d failures\n",fail);
    return fail;
}
static pthread_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
int kh_setting_set(const char *name,const char *value){
    double v; int i=validate(name,value,&v);
    if(i<0) return -1;
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
void kh_settings_init(void){
#if !KH_IOS_PATHS
    load_saved_options();
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
    test_log=getenv("KHNMC_TEST_CHEAT_LOG") && !strcmp(getenv("KHNMC_TEST_CHEAT_LOG"),"1");
    for(int i=0;i<COUNT;i++){
        const char *name=kh_options[i].name,*v=getenv(name);
        if(v){double n; if(validate(name,v,&n)<0) kh_fatal(NULL,"setting-value: %s=%s",name,v); values[i]=n;}
    }
    for(int i=COUNT;i<kh_option_count;i++){
        const char *key=kh_options[i].name,*v=getenv(key);
        if(strncmp(key,"KHNMC_TWEAK_",12) || !v) continue;
        if(kh_options[i].choices){ extern int kh_tweak_boolean(KCtx *,const char *,int); kh_tweak_boolean(NULL,key,0); }
        else {double n; if(validate(key,v,&n)<0 || (strstr(key,"_DMG") && n!=floor(n))) kh_fatal(NULL,"tweak-value: %s=%s",key,v);}
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
static int hide_combo(int enabled,unsigned buttons){ return enabled && (buttons&0xc0)==0xc0; }
void kh_settings_frame(KCtx *g,uint64_t frame){
    while(pending && pending->frame<=frame){
        struct change *c=pending; pending=c->next;
        if(kh_setting_set(c->name,c->value)) kh_fatal(NULL,"settings-save: %s",c->name);
        fprintf(stderr,"kh: settings frame %llu %s=%s\n",c->frame,c->name,c->value);
        free(c);
    }
    if(schedule_storage && !pending){free(schedule_storage);schedule_storage=NULL;}
    extern u16 kh_input_buttons(void);
    int enabled=atomic_load(&values[HUD_COMBO])!=0;
    /* 11/19 (main session): L3 + R3 is also the Xion swap's chord at a save point (task 54): no HUD hiding while standing at one */
    extern int kh_xion_enabled; extern int kh_route_at_save_point(KCtx *);
    int at_swap_point=enabled && kh_xion_enabled && g && kh_route_at_save_point(g);
    atomic_store(&kh_hide_hud,enabled && !at_swap_point && hide_combo(enabled,kh_input_buttons()));
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
