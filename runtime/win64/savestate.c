#include "savestate.h"
#include "gfx_present.h"
#include "route_state.h"
#include "autosave.h"
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#define game_digest(data,size,out) CC_SHA256(data,size,out)
#else
#include <openssl/sha.h>
#define game_digest SHA256
#endif

int kh_cut_enabled;
_Thread_local unsigned kh_cut_callback_depth;
_Atomic int kh_cut_requested;
static u64 target;
static const char *operation,*slot;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t resume=PTHREAD_COND_INITIALIZER;
static struct Park { struct Park *next; KThread *thread; const char *site; void *callers[64]; int depth; } *parks;
static unsigned generation, completed, missed;
static FILE *oracle; /* CPU comparison evidence only; never accepted as a playable slot. */
static int oracle_failed;
static void record_bytes(const void *data,size_t size){
    if(!oracle_failed&&fwrite(data,1,size,oracle)!=size) oracle_failed=1;
}


/* Guest-visible time for this profile. Internal deadline scheduling still uses
 * vclock's original time domain. Boot precedes virtual vblank zero. */
u64 kh_cut_elapsed_ns(void){
    u64 origin=kh_vclock_origin();
    return origin?kh_vclock_now_ns()-origin:0;
}
u64 kh_cut_unix_ns(void){
    return 1704067200000000000ull+kh_cut_elapsed_ns(); /* 2024-01-01 UTC */
}
void kh_cut_init(void){
    const char *enable=getenv("KHNMC_SAVESTATE"), *trigger=getenv("KHNMC_TEST_SAVESTATE");
    if(!enable||!strcmp(enable,"0")) return;
    if(strcmp(enable,"1")) kh_fatal(NULL,"savestate-switch: expected 0 or 1");
    extern int kh_gfx_vkd3d_on;
    if(kh_gfx_vkd3d_on||!kh_vclock_sync||ga_deterministic) kh_fatal(NULL,"savestate-profile: requires null graphics and frames clock with sync=all");
    const char *audio=getenv("KHNMC_AUDIO");
    if(audio&&*audio&&strcmp(audio,"null")&&strncmp(audio,"wav:",4))
        kh_fatal(NULL,"savestate-profile: external audio device active");
    kh_cut_enabled=1;
    if(!trigger) return;
    if(strncmp(trigger,"frame:",6)||trigger[6]<'0'||trigger[6]>'9') goto invalid;
    char *end;
    errno=0;
    target=strtoull(trigger+6,&end,10);
    if(errno||!target||*end!=':') goto invalid;
    operation=end+1;
    if(!strcmp(operation,"freeze")) return;
    if(!strncmp(operation,"census:",7)&&operation[7]){ slot=operation+7; return; }
    if(strncmp(operation,"save:",5)&&strncmp(operation,"load:",5)) goto invalid;
    slot=operation+5;
    if(*slot) return;
invalid:
    kh_fatal(NULL,"savestate-trigger: expected frame:N:save:PATH, frame:N:load:PATH frame:N:freeze or frame:N:census:PATH");
}

/* Only called at predicate loops. Drop the owner's lock before acknowledging and
 * recheck the predicate under that lock on return, including after a spurious wake. */
void kh_cut_poll(const char *site,pthread_mutex_t *held){
    if(kh_cut_callback_depth||!kh_cut_loader_safe()) return;
    if(held) pthread_mutex_unlock(held);
    pthread_mutex_lock(&mutex);
    if(atomic_load(&kh_cut_requested)){
        struct Park p={parks,cur_thread(),site};
        if(!completed) p.depth=backtrace(p.callers,64);
        parks=&p;
        unsigned epoch=generation;
        while(atomic_load(&kh_cut_requested)&&epoch==generation) pthread_cond_wait(&resume,&mutex);
        struct Park **link=&parks;
        while(*link!=&p) link=&(*link)->next;
        *link=p.next;
        pthread_cond_broadcast(&resume);
    }
    pthread_mutex_unlock(&mutex);
    if(held) pthread_mutex_lock(held);
}
void kh_cut_wait(const char *site,pthread_cond_t *cv,pthread_mutex_t *m){
    if(!kh_cut_enabled){ pthread_cond_wait(cv,m); return; }
    if(atomic_load(&kh_cut_requested)&&!kh_cut_callback_depth&&kh_cut_loader_safe()){ kh_cut_poll(site,m); return; }
    struct timespec d;
    kh_mono_deadline_ms(&d,10);
    kh_cond_wait_until(cv,m,&d);
}

static void report_callers(unsigned tid,void **callers,int depth){
    fprintf(stderr,"kh: savestate-callers tid=%u count=%d",tid,depth);
    for(int i=0;i<depth;i++){
        Dl_info d={0};
        if(dladdr(callers[i],&d)&&d.dli_sname)
            fprintf(stderr," %s+%zx",d.dli_sname,(size_t)((uintptr_t)callers[i]-(uintptr_t)d.dli_saddr));
        else if(d.dli_fbase)
            fprintf(stderr," image+%zx",(size_t)((uintptr_t)callers[i]-(uintptr_t)d.dli_fbase));
        else fprintf(stderr," unknown");
    }
    fputc('\n',stderr);
}
static void report_extent(KCtx *g,u64 address,u64 size){
    if(!size) return;
    fprintf(stderr,"kh: savestate-extent guest=%llx bytes=%llu\n",(unsigned long long)address,(unsigned long long)size);
    while(size){
        size_t bytes=size>(1u<<20)?1u<<20:(size_t)size;
        unsigned char digest[32];
        game_digest(hp(g,address),bytes,digest);
        fprintf(stderr,"kh: savestate-chunk %llx %zu ",(unsigned long long)address,bytes);
        for(unsigned i=0;i<32;i++) fprintf(stderr,"%02x",digest[i]);
        fputc('\n',stderr);
        if(oracle){
            static const unsigned char zeros[65536];
            for(size_t i=0;i<bytes;i+=sizeof zeros){
                u64 at=address+i;
                u32 n=bytes-i>sizeof zeros?sizeof zeros:(u32)(bytes-i);
                const void *src=hp(g,at);
                u32 stored=memcmp(src,zeros,n)?n:0;
                record_bytes(&at,sizeof at);
                record_bytes(&n,sizeof n);
                record_bytes(&stored,sizeof stored);
                if(stored) record_bytes(src,n);
            }
        }
        address+=bytes;
        size-=bytes;
    }
}
void kh_cut_frame(KCtx *g,u64 frame){
    extern _Atomic u64 gfx_presents;
    extern size_t kh_phase_files_seen(void);
    unsigned char digest[32];
    char hex[65];
    /* f_152380 returns GAME_SAVE. This digest covers the complete save block. */
    game_digest(hp(g,GS_BASE+0x9a98b0),0x10fc0,digest);
    for(unsigned i=0;i<sizeof digest;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
    fprintf(stderr,"kh: savestate-frame %llu present=%llu return=%llx world=%d area=%d buttons=%x paths=%zu game-save=%s\n",
            (unsigned long long)frame,(unsigned long long)atomic_load(&gfx_presents),
            (unsigned long long)rd64(g,KH_RSP(g)),kh_route_world(g),kh_route_area(g),
            kh_input_buttons(),kh_phase_files_seen(),hex);
    if(!target||frame<target) return;
    int census=!strncmp(operation,"census:",7);
    unsigned wanted=!strcmp(operation,"freeze")?20:1;
    if(completed==wanted) return;
    if(slot&&!strncmp(operation,"load:",5)){
        FILE *f=fopen(slot,"rb");
        if(!f) kh_fatal(g,"savestate-load-open: %s: %s",slot,strerror(errno));
        char header[8];
        size_t size=fread(header,1,sizeof header,f);
        int failed=ferror(f);
        fclose(f);
        if(failed) kh_fatal(g,"savestate-load-read: %s",slot);
        if(size<sizeof header) kh_fatal(g,"savestate-load-truncated: %s",slot);
        if(!memcmp(header,"KHCPU01\n",8))
            kh_fatal(g,"savestate-load-cpu-only: %s has no host or execution continuations",slot);
        kh_fatal(g,"savestate-load-format: unrecognized checkpoint %s",slot);
    }
    KThread *threads[4096];
    u64 start=kh_host_ns(),epoch;
    pthread_mutex_lock(&mutex);
    generation++;
    atomic_store(&kh_cut_requested,1);
    extern void kh_cut_wake_shell(void);
    kh_cut_wake_shell();
    unsigned n;
    int missing;
    for(;;){
        n=0;
        for(struct Park *p=parks;p;p=p->next){
            if(n==sizeof threads/sizeof threads[0]) kh_fatal(g,"savestate-registry-overflow");
            threads[n++]=p->thread;
        }
        int expired=kh_host_ns()-start>=1000000000ull;
        missing=kh_cut_threads(threads,n,expired);
        if(!missing||expired) break;
        pthread_mutex_unlock(&mutex);
        struct timespec nap={0,1000000};
        nanosleep(&nap,NULL);
        pthread_mutex_lock(&mutex);
    }
    int gate_ok=0;
    char *tmp=NULL;
    int fd=-1;
    if(!missing&&kh_cut_gate_begin(&epoch)){
        gate_ok=kh_gfx_gate_quiesce_bounded(4,10)==KH_GATE_QUIESCED;
        if(!completed&&gate_ok){
            void *callers[64];
            int depth=backtrace(callers,64);
            report_callers(cur_thread()->tid,callers,depth);
            for(struct Park *p=parks;p;p=p->next){
                report_callers(p->thread->tid,p->callers,p->depth);
                fprintf(stderr,"kh: savestate-park tid=%u start=%llx at=%s\n",p->thread->tid,(unsigned long long)p->thread->start,p->site);
            }
            extern void kh_cut_arena_census(KCtx *,void (*)(KCtx *,u64,u64));
            if(census){
                if(asprintf(&tmp,"%s.XXXXXX",slot)<0) tmp=NULL;
                if(tmp) fd=mkstemp(tmp);
                if(fd>=0) oracle=fdopen(fd,"wb");
                if(!oracle){ if(fd>=0) close(fd); oracle_failed=1; }
                else record_bytes("KHCPU01\n",8);
            }
            kh_cut_arena_census(g,report_extent);
            if(oracle){
                record_bytes("KHCPUEND",8);
                if(fflush(oracle)||fsync(fd)) oracle_failed=1;
                if(fclose(oracle)) oracle_failed=1;
                oracle=NULL;
            }
            kh_cut_fiber_report();
            kh_route_map_report(g,stderr,"savestate candidate");
        }
        gate_ok=kh_cut_gate_end(epoch)&&gate_ok;
    }
    atomic_store(&kh_cut_requested,0);
    pthread_cond_broadcast(&resume);
    while(parks) pthread_cond_wait(&resume,&mutex);
    pthread_mutex_unlock(&mutex);
    fprintf(stderr,"kh: savestate-rendezvous frame=%llu epoch=%u workers=%u missing=%d gate=%d elapsed-us=%llu\n",
            (unsigned long long)frame,generation,n,missing,gate_ok,(unsigned long long)((kh_host_ns()-start)/1000));
    if(census&&tmp&&fd>=0){
        if(!oracle_failed&&gate_ok&&!missing&&rename(tmp,slot)) oracle_failed=1;
        if(oracle_failed||!gate_ok||missing) unlink(tmp);
    }
    free(tmp);
    if(oracle_failed) kh_fatal(g,"savestate-census-io: could not publish CPU comparison file");
    if(missing||!gate_ok){
        if(++missed==4) kh_fatal(g,"savestate-cut-refused: four successive incomplete rendezvous; last frame %llu",(unsigned long long)frame);
        kh_warn("savestate-cut-deferred: frame %llu, missing %d, gate %d",(unsigned long long)frame,missing,gate_ok);
        return;
    }
    completed++;
    missed=0;
    if(slot&&!census) kh_fatal(g,"savestate-save-unsupported: native caller continuations unavailable for %s",slot);
}
