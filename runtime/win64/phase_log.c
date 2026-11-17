/* Entry probes selected from the corrected name DB and platform/MOVIES.md.
 * Archive aliases keep same-shard relocations and function-table entries on these hooks. */
#include "khnmc_rt.h"
static pthread_mutex_t phase_lock=PTHREAD_MUTEX_INITIALIZER;
static char **seen;
static size_t nseen,reported;
static _Atomic u64 entries[11];
static const char *names[]={"1505a0:GAME_MODE.start","5111d0:InitialSetting.ctor","5122e0:InitialSetting.vfn1","511300:InitialSetting.dtor","13da10:movie.open","13d260:movie.prepare","405440:EVENT.MapEvent","3f29e0:FIELD_COMMAND.execute","3c12f0:BATTLE.calc_damage","2b4590:title.Layout.task","4f94b0:SettingMenu.vfn1"};
#define PHASE(RVA,I) void orig_f_##RVA(KCtx *); void f_##RVA(KCtx *g){ entries[I]++; orig_f_##RVA(g); }
void kh_phase_mode_entry(void){ entries[0]++; }
PHASE(5111d0,1)
void orig_f_5122e0(KCtx *);
static _Atomic int initial_state=-1,initial_choice=-1;
/* present number at which each Initial Settings state was first observed (-1 = not yet): the scripted input's `@hold` uses it */
static _Atomic long long initial_first[64];
long long kh_phase_initial_first(int state){ if(state<0||state>=64) return -1; return atomic_load(&initial_first[state])-1; }
void kh_phase_test_set_initial_first(int state,long long present){ if(state>=0&&state<64) atomic_store(&initial_first[state],present+1); }   /* selftest only; -1 = unseen */
void f_5122e0(KCtx *g){
    entries[2]++;
    initial_state=rd32(g,A0(g)+0x48); initial_choice=rd16(g,A0(g)+0x2ce);
    static int previous=-1;
    if(previous!=initial_state){ extern _Atomic u64 gfx_presents;
        int st=initial_state; if(st>=0&&st<64){ long long z=0; atomic_compare_exchange_strong(&initial_first[st],&z,(long long)gfx_presents+1); }
        fprintf(stderr,"kh: InitialSetting frame=%llu state=%d choice=%d\n",(unsigned long long)gfx_presents,(int)initial_state,(int)initial_choice); previous=initial_state;
    }
    orig_f_5122e0(g);
}
PHASE(511300,3)
PHASE(13da10,4)
PHASE(13d260,5)
void kh_phase_mapevent_entry(void){ entries[6]++; } /* f_405440 itself lives in mod_hooks_a.c (Task 9) */
/* Task 12: 3f29e0 has two static edits (0x3f2d2e, 0x3f2d4a); the probe forwards to the body re-lifted from the patched image. */
void mod_f_3f29e0(KCtx *);
void f_3f29e0(KCtx *g){ entries[7]++; mod_f_3f29e0(g); }
void kh_phase_damage_entry(void){ entries[8]++; }
PHASE(2b4590,9)
void orig_f_4f94b0(KCtx *);
void f_4f94b0(KCtx *g){
    entries[10]++;
    static int previous=-1; int state=rd32(g,A0(g)+0x774);
    if(previous!=state){ extern _Atomic u64 gfx_presents;
        fprintf(stderr,"kh: SettingMenu frame=%llu state=%d\n",(unsigned long long)gfx_presents,state); previous=state;
    }
    orig_f_4f94b0(g);
}
void kh_phase_file(KCtx *g,const char *path){
    pthread_mutex_lock(&phase_lock);
    for(size_t i=0;i<nseen;i++) if(!strcmp(seen[i],path)){ pthread_mutex_unlock(&phase_lock); return; }
    char **next=realloc(seen,(nseen+1)*sizeof *seen);
    if(!next) kh_fatal(g,"phase log: file list allocation failed"); seen=next;
    seen[nseen]=strdup(path); if(!seen[nseen]) kh_fatal(g,"phase log: path allocation failed"); nseen++;
    pthread_mutex_unlock(&phase_lock);
}
void kh_phase_present(KCtx *g,u64 present,u64 draws,u64 clears,u64 psos){
    if(present%60) return;
    static u64 prev_draws,prev_clears,prev_psos;
    const char *path=getenv("KHNMC_PHASELOG"); if(!path) path="phase.log";
    pthread_mutex_lock(&phase_lock);
    FILE *f=fopen(path,"a"); if(!f) kh_fatal(g,"phase log: cannot append %s",path);
    fprintf(f,"present=%llu draws=%llu clears=%llu pso-binds=%llu files=[",(unsigned long long)present,(unsigned long long)(draws-prev_draws),(unsigned long long)(clears-prev_clears),(unsigned long long)(psos-prev_psos));
    for(size_t i=reported;i<nseen;i++) fprintf(f,"%s%s",i==reported?"":" | ",seen[i]);
    reported=nseen; fprintf(f,"] initial-state=%d choice=%d entries=[",(int)initial_state,(int)initial_choice);
    for(int i=0;i<11;i++) fprintf(f,"%s%s=%llu",i?" ":"",names[i],(unsigned long long)atomic_exchange(&entries[i],0));
    int failed=fprintf(f,"]\n")<0 || ferror(f);
    if(fclose(f)||failed) kh_fatal(g,"phase log: write failed %s",path);
    prev_draws=draws; prev_clears=clears; prev_psos=psos;
    pthread_mutex_unlock(&phase_lock);
}
