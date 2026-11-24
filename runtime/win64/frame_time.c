#include "frame_time.h"
#include "kh_look.h"
#include "route_state.h"

_Atomic u64 kh_display_ticks, kh_present_ns, kh_acquire_ns, kh_slot_wait_ns;
extern _Atomic u64 gfx_draws;

_Atomic int kh_frame_work,kh_frame_log=1;
_Atomic u64 kh_vertices, kh_blend_vertices, kh_texture_uploads, kh_file_reads, kh_bg_wait_ns;
void kh_frame_time_init(void){
    const char *v=getenv("KHNMC_FRAME_WORK");
    if(v && strcmp(v,"0") && strcmp(v,"1")) kh_fatal(NULL,"frame-work-value: expected 0 or 1");
    const char *log=getenv("KHNMC_FRAME_TIME");
    kh_frame_log=!log || !strcmp(log,"1");
    kh_frame_work=v && *v=='1';
}

typedef struct {
    u64 frames, wall, cpu, present, acquire, slot_wait, worst, missed, late, draws;
    u64 hist[257]; /* upper bounds in milliseconds, last bucket uses the observed max */
} FrameStats;

static void emit(const char *kind,int world,int area,int look,const FrameStats *s,int display){
    if(!kh_frame_log || !s->frames) return;
    unsigned i=0;
    u64 cumulative=s->hist[0], rank=s->frames-s->frames/20;
    while(cumulative<rank && i<256) cumulative+=s->hist[++i];
    double ms=s->frames*1e6;
    fprintf(stderr,"kh: frame-time %s room=%02x:%02x look=%s frames=%llu wall_ms=%.3f p95_le_ms=%.3f worst_ms=%.3f cpu_ms=%.3f present_ms=%.3f acquire_ms=%.3f slot_wait_ms=%.3f missed=%llu source=%s draws_per_frame=%.2f\n",
        kind,world&255,area&255,look==2?"xion-hooded":look==3?"axel":look==4?"roxas-hd":look?"xion":"roxas",(unsigned long long)s->frames,
        s->wall/ms,i==256?s->worst/1e6:(double)i,s->worst/1e6,s->cpu/ms,s->present/ms,s->acquire/ms,s->slot_wait/ms,
        (unsigned long long)s->missed,display?"display-link":"60Hz-estimate",(double)s->draws/s->frames);
}

void kh_frame_time(KCtx *g,u64 frame){
    /* End-of-frame hook. A thread switch starts a fresh baseline for the CPU clock. */
    static _Thread_local struct {
        u64 wall,cpu,present,acquire,slot_wait,ticks,draws;
        int world,area,look;
        FrameStats room,second;
    } p;
    struct timespec ts;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)) kh_fatal(g,"frame-time-thread-clock");
    u64 cpu=(u64)ts.tv_sec*1000000000+ts.tv_nsec,wall=kh_host_ns();
    u64 ticks=atomic_load_explicit(&kh_display_ticks,memory_order_relaxed);
    u64 present=atomic_load_explicit(&kh_present_ns,memory_order_relaxed);
    u64 acquire=atomic_load_explicit(&kh_acquire_ns,memory_order_relaxed);
    u64 slot_wait=atomic_load_explicit(&kh_slot_wait_ns,memory_order_relaxed);
    u64 draws=atomic_load_explicit(&gfx_draws,memory_order_relaxed);
    int world=kh_route_world(g),area=kh_route_area(g),look=kh_look_enabled();
    if(kh_frame_work && p.wall){
        u64 live,allocs; ga_stats(&live,&allocs);
        fprintf(stderr,"kh: frame-work frame=%llu room=%02x:%02x end_ns=%llu cpu_ns=%llu draws=%llu vertices=%llu null_blend_vertices=%llu file_reads=%llu uploads=%llu allocs=%llu bg_wait_ns=%llu\n",
            (unsigned long long)frame,p.world&255,p.area&255,(unsigned long long)wall,(unsigned long long)(cpu-p.cpu),
            (unsigned long long)draws,(unsigned long long)kh_vertices,(unsigned long long)kh_blend_vertices,(unsigned long long)kh_file_reads,
            (unsigned long long)kh_texture_uploads,(unsigned long long)allocs,(unsigned long long)kh_bg_wait_ns);
    }
    if(p.wall){
        u64 dt=wall-p.wall;
        u64 periods=ticks?ticks-p.ticks:(dt+8333333)/16666667;
        u64 bucket=(dt+999999)/1000000;
        FrameStats *both[]={&p.second,&p.room};
        for(unsigned i=0;i<2;i++){
            FrameStats *s=both[i];
            s->frames++; s->wall+=dt; s->cpu+=cpu-p.cpu;
            s->hist[bucket<256?bucket:256]++; if(dt>s->worst) s->worst=dt;
            s->present+=present-p.present; s->acquire+=acquire-p.acquire; s->slot_wait+=slot_wait-p.slot_wait;
            s->draws+=draws>=p.draws?draws-p.draws:0;
            s->missed+=periods>1?periods-1:0; s->late+=dt>16666667;
        }
        int change=world!=p.world || area!=p.area || look!=p.look;
        if(p.second.wall>=1000000000 || change){
            if(p.second.late && p.second.wall>=1000000000) emit("late",p.world,p.area,p.look,&p.second,ticks!=0);
            memset(&p.second,0,sizeof p.second);
        }
        if(change){
            emit("room",p.world,p.area,p.look,&p.room,ticks!=0);
            memset(&p.room,0,sizeof p.room);
        }
    }
    p.world=world; p.area=area; p.look=look; p.wall=wall; p.cpu=cpu;
    p.present=present; p.acquire=acquire; p.slot_wait=slot_wait; p.ticks=ticks; p.draws=draws;
}
