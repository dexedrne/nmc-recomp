#include "khnmc_rt.h"
#include "settings.h"
#include "kh_look.h"
#include "mod_config.h"
#include "route_state.h"
#include <math.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

extern int kh_run_is_selftest;
enum { HP, MP, EXP, DROP, HUD_COMBO, LOOK, COUNT };
#define BOOL_OPT(name,label,initial) {"KHNMC_" name,label,initial,"0|1",0,1,1}
#define TWEAK_OPT(name,label,initial) {"KHNMC_TWEAK_" name,label,initial,"off|on",0,1,1}
#define NUMBER(name,label,initial,min,max,step) {"KHNMC_" name,label,initial,NULL,min,max,step}
const struct kh_option kh_options[]={
    BOOL_OPT("CHEAT_INF_HP","Infinite HP","0"), BOOL_OPT("CHEAT_INF_MP","Infinite MP","0"),
    NUMBER("EXP_MULT","EXP multiplier","1",0.5,10,0.5), NUMBER("DROP_MULT","Item drop multiplier","1",0.5,10,0.5),
    BOOL_OPT("HUD_HIDE_COMBO","Hold L3 + R3 to hide HUD","0"),
    {"KHNMC_PLAYER_LOOK","Player appearance","roxas","roxas|xion",0,0,0},
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
int kh_setting_menu_count(void){return kh_option_count-(debug_enabled?(allow_reset?0:1):DEBUG_COUNT);}
static _Atomic double values[COUNT]={0,0,1,1};
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
                if(strlen(value)==n && !strncmp(p,value,n)){*number=i==LOOK?!strcmp(value,"xion"):!strcmp(value,"1");return i;}
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
int kh_settings_look(void){
    int request=atomic_load(&look_request);
    return request>=0?request:kh_look_enabled();
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
    int state=0; u64 start=0;
    fail|=kh_stick_step(&state,&start,0x40,1)!=CHORD_IDLE;
    fail|=kh_stick_step(&state,&start,0xc0,2)!=CHORD_PRESS;
    fail|=kh_stick_step(&state,&start,0x80,600000001)!=CHORD_CLICK;
    fail|=kh_stick_step(&state,&start,0xc0,900000000)!=CHORD_PRESS;
    fail|=kh_stick_step(&state,&start,0xc0,1500000000)!=CHORD_HOLD;
    fail|=kh_stick_step(&state,&start,0,1500000001)!=CHORD_IDLE;
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
/* f_18deb0 replaces the active INFORMATION line. Keep transient feedback off
 * that widget so mission scripts retain their own message and timing. */
static const u8 look_text[][18]={
    {0x45,0xa2,0xa8,0xa7},                         /* Xion */
    {0x3f,0xa8,0xb1,0x9a,0xac},                    /* Roxas */
    {0x30,0x9a,0xa7,0x57,0xad,1,0xac,0xb0,0xa2,0xad,0x9c,0xa1,1,0xa7,0xa8,0xb0}
};
static _Atomic u64 notice; /* expiry in host ns, with message index in low two bits */
u8 kh_look_font[65536];
static const char *const notice_lines[]={"Xion","Roxas","Can't switch now"};
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
    return now<(state & ~3ull)?look_text[state & 3]:NULL;
}
const u8 *kh_look_notice(void){
    u64 state=atomic_load(&notice);
    if(!state) return NULL;
    int ready=atomic_load(&font_ready);
    if(!ready) return NULL;
    if(ready<0) kh_fatal(NULL,"look-notice-system-font-unavailable");
    u64 now=kh_host_ns();
    /* A stick click during boot may arrive before the game loads its font. */
    if(state<=3){
        u64 active=((now+2000000000ull) & ~3ull) | (state-1);
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
    for(unsigned i=0;i<3;i++){
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
static struct look_art art[30];
static u64 face_actor,face_instance,face_model,face_bar;
static u64 last_switch;
/* A frame already submitted may still reference the old instance. Retire it
 * with the actor, at the same native teardown boundary as the active one. */
static struct idle_render { u64 owner,instance; } idle[80]; /* native sheet pool */
static void free_render(KCtx *g,u64 inst){
    u64 model=rd64(g,inst+0x20),allocator=kh_call(g,GS_BASE+0x152590,0,0,0,0);
    kh_call(g,rd64(g,rd64(g,model)+0x48),model,allocator,inst,0);
}
static void end_look_actor(KCtx *g){
    if(!last_switch) return;
    if(A0(g)==face_actor) face_actor=face_instance=face_model=face_bar=0;
    for(size_t i=0;i<80;i++) if(idle[i].owner==A0(g)){
        KCtx ctx=*g;
        free_render(&ctx,idle[i].instance);
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
    u64 path=ga_strdup(name);
    int size=(s32)kh_call(g,GS_BASE+0x3a09b0,path,0,0,0);
    u64 bar=size>=16?ga_alloc(size,128):0;
    if(bar && (s32)kh_call(g,GS_BASE+0x3f9850,path,bar,0,0)!=size){
        ga_free(bar); bar=0;
    }
    ga_free(path);
    if(bar){
        wr8(g,bar+3,(rd8(g,bar+3)&63)|(rd8(g,native+3)&192));
        kh_call(g,GS_BASE+0x3a64f0,bar,0,0,0);
        kh_call(g,GS_BASE+0x3a6840,bar,0,0,0);
    }
    return bar;
}
static int bind_look(KCtx *g,KhPlayerSheet p,const char *stem,int target){
    u64 obj=p.obj,inst=rd64(g,obj+0x7b0);
    if(!inst || !rd64(g,obj+0xa88) || !rd64(g,obj+0x920) || !rd64(g,obj+0x928) ||
        rd64(g,inst+0x20)!=rd64(g,obj+0xa88)) return 0;
    struct look_art *a=art;
    while(a<art+30 && a->stem[0] && (a->mode!=target || strcmp(a->stem,stem))) ++a;
    if(a==art+30) return 0;
    snprintf(a->stem,sizeof a->stem,"%s",stem); a->mode=target;
    if(!a->body) a->body=look_bar(g,stem,"mdlx",rd64(g,obj+0x920));
    if(!a->face) a->face=look_bar(g,stem,"a.us",rd64(g,obj+0x928));
    if(!a->body || !a->face) return 0;
    u64 native=rd64(g,obj+0x920),face=a->face;
    u64 original=kh_call(g,GS_BASE+0x3a6420,native,4,0,0);
    u64 wanted=kh_call(g,GS_BASE+0x3a6420,a->body,4,0,0);
    if(!original || !wanted) return 0;
    /* All 15 X5 model members differ in length from their Roxas recipient.
     * Keep the native instance on a room/form load that already selected it. */
    if(rd32(g,original+12)==rd32(g,wanted+12)) face=rd64(g,obj+0x928);
    else wr64(g,obj+0x920,a->body);
    u64 model=kh_call(g,GS_BASE+0x3e62e0,obj+0x910,0,0,0);
    u64 tex=kh_call(g,GS_BASE+0x3e6310,obj+0x910,0,0,0);
    wr64(g,obj+0x920,native);
    if(!model || !tex || rd64(g,model)!=rd64(g,rd64(g,obj+0xa88))) return 0;
    int changed=rd64(g,obj+0xa88)!=model;
    if(changed){
        size_t slot=(p.sheet-(GS_BASE+0x2a17290))/0x278;
        if(slot>=80 || (idle[slot].owner && idle[slot].owner!=obj)) return 0;
        u64 other=idle[slot].instance;
        if(!other || rd64(g,other+0x20)!=model){
            u64 allocator=kh_call(g,GS_BASE+0x152590,0,0,0,0);
            u64 next=kh_call(g,rd64(g,rd64(g,model)+0x40),model,allocator,0,0);
            if(!next) return 0;
            kh_call(g,GS_BASE+0x1a4bb0,next,tex,0,0);
            kh_call(g,GS_BASE+0x1a4e30,next,0,0,0);
            if(other) free_render(g,other);
            other=next;
        }
        idle[slot]=(struct idle_render){obj,inst};
        inst=other;
        wr64(g,obj+0xa88,model);
        wr64(g,obj+0x7b0,inst);
        wr64(g,obj+0x7e0,tex);
    }
    face_bar=face; face_actor=obj; face_instance=inst; face_model=model;
    if(!changed) return 1;
    /* f_180900 refreshes dk::FACE separately from gauge visibility (f_16c250). */
    u64 gauge=rd64(g,GS_BASE+0xabd248);
    if(gauge && rd64(g,gauge+0x40)==obj){
        u64 widget=gauge+0x50;
        kh_call(g,GS_BASE+0x17eb30,widget,obj,rd32(g,gauge+0x20),0);
        if(!rd8(g,widget+0x304)) kh_fatal(g,"look-gauge-face-bind: %s",stem);
    }
    return 1;
}
static void look_frame(KCtx *g,int target,u64 frame){
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
    const char *line="Can't switch now";
    int notice_index=2;
    if(!cmds || windows || rd8(g,GS_BASE+0x9ba8d1) || rd8(g,GS_BASE+0x2a11428) || rd8(g,GS_BASE+0x9006b0) || rd64(g,GS_BASE+0x2a11478) ||
       rd64(g,GS_BASE+0x2ae9fa8) || rd64(g,GS_BASE+0x2a11268) || /* Roxas parked during XKB ownership */
       rd32(g,GS_BASE+0x2a10504) || rd64(g,GS_BASE+0x9ba928) ||
       (!refresh && last_switch && frame>=last_switch && frame-last_switch<60) ||
       !kh_route_player_sheet(g,&player) || player.hp<=0 ||
       !kh_route_object_name(g,player.obj,stem,sizeof stem)) goto feedback;
    snprintf(path,sizeof path,"obj/%s.mdlx",stem);
    if(kh_player_asset_key(path)!=-1) goto feedback;
    if(target){
        char face[96],host[4096];
        if(!kh_look_available(path,host,sizeof host)) {line="Xion model missing"; goto feedback;}
        snprintf(path,sizeof path,"obj/%s.a.us",stem);
        snprintf(face,sizeof face,"remastered/%s/-%d.dds",path,kh_player_asset_key(path)-1);
        if(!kh_look_available(path,host,sizeof host) || !kh_look_available(face,host,sizeof host)){
            line="Xion portrait missing"; goto feedback;
        }
    }
    KCtx ctx=*g;
    kh_look_set(target);
    if(!bind_look(&ctx,player,stem,target)){
        if(!refresh) kh_look_set(!target);
        line="Appearance resources unavailable";
        goto feedback;
    }
    if(refresh) return;
    line=target?"Xion":"Roxas";
    notice_index=target?0:1;
    if(kh_save_live_option("KHNMC_PLAYER_LOOK",target?"xion":"roxas",LOOK,target))
        kh_fatal(g,"look-settings-save");
    last_switch=frame;
feedback:
    if(refresh) return;
    kh_warn("player appearance: %s (frame %llu)",line,(unsigned long long)frame);
    show_look_notice(notice_index,frame);
}
void kh_settings_frame(KCtx *g,uint64_t frame){
    while(pending && pending->frame<=frame){
        struct change *c=pending; pending=c->next;
        if(kh_setting_set(c->name,c->value)) kh_fatal(NULL,"settings-save: %s",c->name);
        fprintf(stderr,"kh: settings frame %llu %s=%s\n",c->frame,c->name,c->value);
        free(c);
    }
    if(schedule_storage && !pending){free(schedule_storage);schedule_storage=NULL;}
    int action=kh_stick_action(frame);
    atomic_store(&kh_hide_hud,action==CHORD_HOLD && values[HUD_COMBO]!=0);
    int target=atomic_exchange(&look_request,-1);
    if(action==CHORD_CLICK) target=!kh_look_enabled();
    if(g && (target>=0 || last_switch)) look_frame(g,target,frame);
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
    KH_RECORD(r,idle);
    KH_RECORD(r,face_actor);
    KH_RECORD(r,face_instance);
    KH_RECORD(r,face_model);
    KH_RECORD(r,face_bar);
    KH_RECORD(r,last_switch);
}
