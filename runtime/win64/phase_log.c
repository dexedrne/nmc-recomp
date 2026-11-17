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
/* Task 27: liveness of the screens the scripted route waits for (runtime/route_state.c).  A screen's own globals outlive it (the
 * title's layout pointers, the New Game dialog's context: freed but never cleared), so a state read from them is only believed while
 * the object or task that owns them is alive: InitialSetting between its ctor 0x5111d0 and dtor 0x511300, the title task 0x2b4590
 * and the New Game menu task 0x2b7420 between their entry and their return (both are task bodies that yield frame by frame on their
 * own fiber and return when the screen is done). */
static _Atomic int initial_alive, title_depth, newgame_depth;
int kh_phase_initial_alive(void){ return atomic_load(&initial_alive)>0; }
int kh_phase_title_alive(void){ return atomic_load(&title_depth)>0; }
int kh_phase_newgame_alive(void){ return atomic_load(&newgame_depth)>0; }
void kh_phase_test_set_alive(int initial,int title,int newgame){ atomic_store(&initial_alive,initial); atomic_store(&title_depth,title); atomic_store(&newgame_depth,newgame); }   /* selftest only */
void orig_f_5111d0(KCtx *); void f_5111d0(KCtx *g){ entries[1]++; atomic_store(&initial_alive,1); orig_f_5111d0(g); }
void orig_f_5122e0(KCtx *);
static _Atomic int initial_state=-1,initial_choice=-1;
int kh_phase_initial_state(void){ return atomic_load(&initial_state); }
void kh_phase_test_set_initial_state(int st){ atomic_store(&initial_state,st); }   /* selftest only */
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
void orig_f_511300(KCtx *); void f_511300(KCtx *g){ entries[3]++; atomic_store(&initial_alive,0); orig_f_511300(g); }
PHASE(13da10,4)
PHASE(13d260,5)
void kh_phase_mapevent_entry(void){ entries[6]++; } /* f_405440 itself lives in mod_hooks_a.c (Task 9) */
/* FIELD_COMMAND.execute: also a running total (the video stream starts "when the field is up") and the place of its first call */
static _Atomic u64 field_cmds_total;
u64 kh_phase_field_cmds(void){ return atomic_load(&field_cmds_total); }
u64 kh_input_game_frames(void);
/* Task 23: f_3f29e0 itself lives in mod_hooks_b.c.  On the PC the mod DLL's detour 0x306c0 REPLACES this function (it never calls
 * the trampoline), so neither the vanilla body nor the body re-lifted from the patched image (mod_f_3f29e0, Task 12) runs; the hook
 * calls this probe first. */
void kh_phase_field_cmd_entry(void){ entries[7]++;
    if(atomic_fetch_add(&field_cmds_total,1)==0){ extern _Atomic u64 gfx_presents;
        fprintf(stderr,"kh: phase: first FIELD_COMMAND.execute at present %llu, game frame %llu, virtual vblank %llu, host-ns=%llu\n",(unsigned long long)gfx_presents,(unsigned long long)kh_input_game_frames(),(unsigned long long)(kh_vclock_on?kh_vclock_vblanks():0),(unsigned long long)kh_host_ns()); } }
/* BATTLE.calc_damage: one line per call with every clock, so that a hit can be found in the picture stream and in the sound */
/* Task 27: the hits counted here (only while something is on screen) and the game frame of the last one feed the route conditions
 * `battle` and `hits N` (runtime/route_state.c). */
static _Atomic u64 damage_hits, damage_last_frame;
u64 kh_phase_damage_hits(void){ return atomic_load(&damage_hits); }
u64 kh_phase_damage_last_frame(void){ return atomic_load(&damage_last_frame); }
void kh_phase_test_set_damage(u64 hits,u64 last_frame){ atomic_store(&damage_hits,hits); atomic_store(&damage_last_frame,last_frame); }   /* selftest only */
void kh_phase_damage_entry(void){ entries[8]++; extern _Atomic u64 gfx_presents;
    if(!gfx_presents) return;                                       /* not while nothing is on screen (selftests) */
    u64 k=atomic_fetch_add(&damage_hits,1)+1; atomic_store(&damage_last_frame,kh_input_game_frames());
    /* host-ns (TASK21b): the host's monotonic clock, so that presents and game frames per second between two events can be read from
     * the log on the host clock too, where the two virtual-clock fields are 0 */
    fprintf(stderr,"kh: event damage #%llu present=%llu game-frame=%llu vblank=%llu virtual-ns=%llu host-ns=%llu\n",(unsigned long long)k,(unsigned long long)gfx_presents,(unsigned long long)kh_input_game_frames(),
            (unsigned long long)(kh_vclock_on?kh_vclock_vblanks():0),(unsigned long long)(kh_vclock_on?kh_vclock_now_ns():0),(unsigned long long)kh_host_ns()); }
void kh_phase_title_entry(void){ entries[9]++; atomic_fetch_add(&title_depth,1); } /* f_2b4590 itself lives in moose.c (Task 23: KH2Moose fast_title picks the patched or the vanilla body) */
void kh_phase_title_exit(void){ atomic_fetch_sub(&title_depth,1); }                  /* ... and calls this when the title task returns */
/* Task 27: the New Game menu task (title mode 1; the exe calls it once, from the title task at 0x2b493f): difficulty, the second
 * question and the final confirmation are pages of [0x140b1d800] inside it (runtime/route_state.c) */
void orig_f_2b7420(KCtx *); void f_2b7420(KCtx *g){ atomic_fetch_add(&newgame_depth,1); orig_f_2b7420(g); atomic_fetch_sub(&newgame_depth,1); }
void orig_f_4f94b0(KCtx *);
void f_4f94b0(KCtx *g){
    entries[10]++;
    static int previous=-1; int state=rd32(g,A0(g)+0x774);
    if(previous!=state){ extern _Atomic u64 gfx_presents;
        fprintf(stderr,"kh: SettingMenu frame=%llu state=%d\n",(unsigned long long)gfx_presents,state); previous=state;
    }
    orig_f_4f94b0(g);
}
size_t kh_phase_files_seen(void){ pthread_mutex_lock(&phase_lock); size_t n=nseen; pthread_mutex_unlock(&phase_lock); return n; }   /* task 29: distinct paths opened */
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
