#include "slot_disk.h"
#include <sys/stat.h>
#include "settings.h"
#include "gfx_present.h"
#include "route_state.h"
#include "autosave.h"
#include <errno.h>
#include <setjmp.h>
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

extern int kh_gfx_vkd3d_on;
int kh_cut_enabled;
_Thread_local unsigned kh_cut_callback_depth;
_Atomic int kh_cut_requested;
enum CutOperation { FREEZE, CENSUS, HASH, SAVE, LOAD, SYNC_SAVE, SYNC_CHECK };
static struct Trigger {
    struct Trigger *next;
    u64 frame;
    enum CutOperation op;
    char *slot;
} *trigger_head;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t resume=PTHREAD_COND_INITIALIZER;
static struct Park { struct Park *next; KThread *thread; const char *site,*owner; void *callers[64]; int depth,stream; struct KhPendingWait *pending; u32 *rng; } *parks;
static unsigned generation, completed, missed;
static FILE *oracle; /* CPU comparison evidence only; never accepted as a playable slot. */
static int oracle_failed;
/* UI thread submits one numbered slot. Only the game thread touches the trigger list. */
static _Atomic int menu_request;
static _Atomic(const char *) menu_status;
static struct Trigger menu_trigger;
static char menu_path[4096];
static int scripted;
static struct KhSyncSlot *test_kernel;
static struct KhAudioSlot *test_audio;

struct SessionSlot {
    char *path;
    KhSlotHeader header;
    KhRecords arena;
    KhPadHistory pad;
    struct KhGpuCut *gpu;
    struct KhSyncSlot *sync;
    struct KhAudioSlot *audio;
    struct KhCrtFiles *files;
    unsigned nthreads;
    struct ThreadPoint { KThread *t; const char *site; u64 successor; } *points;
    struct SessionSlot *next;
};
static struct SessionSlot *slots;
static KhBootDocs startup_docs,disk_docs;
static KhSlotHeader disk_header;
static KhRecords disk_arena;
static KhPadHistory disk_pad;
static FILE *bootstrap;
static int opened;
static u64 load_started;

static void close_load(void){
    if(bootstrap){ fclose(bootstrap); bootstrap=NULL; }
    kh_pad_stop_bootstrap();
    kh_slot_docs_finish();
    free(disk_docs.data); disk_docs=(KhBootDocs){0};
    free(disk_pad.changes); disk_pad=(KhPadHistory){0};
    kh_records_free(&disk_arena);
    opened=0;
}
static void discard_slot(struct SessionSlot *s){
    if(!s) return;
    kh_gpu_cut_free(s->gpu);
    kh_records_free(&s->arena);
    if(s->sync) kh_sync_discard(s->sync);
    kh_audio_discard(s->audio); kh_crt_files_discard(s->files);
    free(s->pad.changes); free(s->points); free(s->path); free(s);
}
static struct SessionSlot **slot_at(const char *path){
    struct SessionSlot **p=&slots;
    while(*p&&strcmp((*p)->path,path)) p=&(*p)->next;
    return p;
}
static const char *check_identity(const KhSlotHeader *h){
    KhSlotHeader local={0};
    const char *err=kh_slot_identity(&local);
    if(!err){
        if(memcmp(local.build,h->build,32)) err="savestate-load-build";
        else if(memcmp(local.game,h->game,32)) err="savestate-load-game";
        else if(memcmp(local.data,h->data,32)) err="savestate-load-data-tree";
        else if(memcmp(local.settings,h->settings,32)) err="savestate-load-switches";
    }
    return err;
}
/* A resident slot can be large enough that reading it before parking the
 * timing thread lets that thread leave its restorable release handoff. */
static const char *read_resident(struct SessionSlot *s){
    const char *err=check_identity(&disk_header);
    if(err) return err;
    if(memcmp(&s->header,&disk_header,sizeof disk_header)) return "savestate-load-checksum";
    KhPadHistory check={0}; KhBootDocs initial={0};
    err=kh_slot_read(bootstrap,&disk_header,NULL,&check,&initial,NULL);
    free(check.changes); free(initial.data);
    if(!err && fseeko(bootstrap,sizeof disk_header,SEEK_SET)) err="savestate-load-seek";
    if(!err) err=kh_slot_read(bootstrap,&disk_header,&disk_arena,&disk_pad,&disk_docs,NULL);
    return err;
}
static const char *open_slot(const char *path,struct SessionSlot **out,KhSlotHeader *h,KhPadHistory *pad,KhBootDocs *docs,FILE **file){
    FILE *f=fopen(path,"rb");
    if(!f) return "savestate-load-open";
    const char *err=NULL;
    if(fread(h->magic,1,8,f)!=8) err="savestate-load-truncated";
    else if(!memcmp(h->magic,"KHCPU01\n",8)) err="savestate-load-cpu-only";
    else if(!memcmp(h->magic,"KHSES01\n",8)) err="savestate-load-session";
    else if(memcmp(h->magic,"KHSS003\n",8)) err="savestate-load-format";
    else if(fread((u8 *)h+8,1,sizeof *h-8,f)!=sizeof *h-8) err="savestate-load-truncated";
    struct SessionSlot *s=*slot_at(path);
    if(!err && s && !memcmp(s->header.key,h->key,sizeof h->key)){
        *out=s; *file=f;
        return NULL;
    }
    if(!err) err=check_identity(h);
    if(!err) err=kh_slot_read(f,h,NULL,pad,docs,NULL);
    if(!err) *file=f;
    else fclose(f);
    return err;
}
static void retain_slot(struct SessionSlot *s){
    struct SessionSlot **where=slot_at(s->path), *old=*where;
    s->next=old?old->next:NULL; *where=s; discard_slot(old);
}
static const char *publish_slot(struct SessionSlot *s){
    u64 started=kh_host_ns();
    KhSlotHeader *h=&s->header;
    memcpy(h->magic,"KHSS003\n",8);
    const char *err=kh_slot_identity(h);
    if(err) return err;
    FILE *random=fopen("/dev/urandom","rb");
    if(!random) return "savestate-session-key";
    int ok=fread(h->key,1,sizeof h->key,random)==sizeof h->key;
    fclose(random);
    if(!ok) return "savestate-session-key";
    char *tmp=NULL;
    if(asprintf(&tmp,"%s.XXXXXX",s->path)<0) return "savestate-slot-memory";
    int fd=mkstemp(tmp);
    if(fd<0){ free(tmp); return "savestate-slot-create"; }
    FILE *f=fdopen(fd,"wb");
    if(!f){ close(fd); err="savestate-slot-create"; }
    else{
        err=kh_slot_write(f,h,&s->arena,&s->pad,&startup_docs);
        if(!err && (fflush(f)||fsync(fd))) err="savestate-slot-sync";
        if(fclose(f)&&!err) err="savestate-slot-close";
    }
    if(!err&&rename(tmp,s->path)) err="savestate-slot-publish";
    if(err) unlink(tmp);
    free(tmp);
    if(err) return err;
    kh_records_free(&s->arena);
    fprintf(stderr,"kh: savestate-slot-written frame=%llu bytes=%llu publish-us=%llu\n",
            (unsigned long long)h->frame,(unsigned long long)(sizeof *h+h->bytes),
            (unsigned long long)((kh_host_ns()-started)/1000));
    retain_slot(s);
    return NULL;
}
static const char *session_check(struct SessionSlot *s,int load,unsigned workers){
    if(ga_spread_on()) return "savestate-arena-spread";
    const char *err=kh_rewind_handles_check();
    if(!err) err=kh_rewind_crt_check();
    if(!err) err=kh_rewind_fiber_check(load);
    if(err) return err;
    if(load&&s->nthreads!=workers+1) return "savestate-thread-set";
    struct Park *p=NULL;
    for(unsigned i=0;i<=workers;i++){
        KThread *t=p?p->thread:cur_thread();
        const char *site=p?p->site:"game";
        if(t->cur_fiber) return "savestate-active-fiber";
        if(load){
            unsigned j=0; while(j<s->nthreads&&s->points[j].t!=t) j++;
            if(j==s->nthreads) return "savestate-thread-set";
            if(strcmp(s->points[j].site,site)||s->points[j].successor!=rd64(t->g,KH_RSP(t->g)))
                return "savestate-worker-successor";
        }else s->points[i]=(struct ThreadPoint){t,site,rd64(t->g,KH_RSP(t->g))};
        p=i?p->next:parks;
    }
    return NULL;
}
/* Thread objects and their native contexts are process-local. TIDs order the
 * architectural records independently of pthread arrival order at the cut. */
static int point_order(const void *a,const void *b){
    const struct ThreadPoint *x=a,*y=b;
    return (x->t->tid>y->t->tid)-(x->t->tid<y->t->tid);
}
static void disk_cpu(struct SessionSlot *s,KhRecords *r){
    for(unsigned i=0;i<s->nthreads;i++){
        struct ThreadPoint *p=&s->points[i];
        KThread *t=p->t;
        u64 identity[]={t->tid,t->start,t->teb,t->stack_lo,t->stack_hi,p->successor};
        KH_RECORD(r,identity);
        u32 *rng=kh_rewind_rng();
        if(t!=cur_thread()){
            struct Park *park=parks;
            while(park->thread!=t) park=park->next;
            rng=park->rng;
        }
        KH_RECORD(r,t->g->st); KH_RECORD(r,t->g->aux);
        KH_RECORD(r,t->lasterr); KH_RECORD(r,t->last_error); KH_RECORD(r,*rng);
    }
    kh_rewind_arena(r);
    kh_rewind_fibers(r); kh_rewind_handles(r); kh_rewind_crt(r);
    kh_rewind_gfx(r); kh_rewind_aspect(r); kh_rewind_input(r); kh_rewind_shell(r); kh_rewind_route(r); kh_rewind_phase(r);
    kh_rewind_warp(r); kh_rewind_xion(r); kh_rewind_retry(r); kh_rewind_hd(r); kh_rewind_mod(r); kh_rewind_maps(r);
    kh_rewind_ids(r); kh_rewind_files(r); kh_rewind_cutscenes(r);
}
static void disk_owners(struct SessionSlot *s,KhRecords *r){
    kh_disk_sync(r,s->sync);
    size_t sync=r->position;
    kh_disk_files(r,s->files);
    size_t files=r->position;
    kh_disk_audio(r,s->audio);
    fprintf(stderr,"kh: savestate-owner-records sync-end=%zu files-end=%zu audio-end=%zu\n",
            sync,files,r->position);
}
const char *kh_cut_menu_status(void){ return atomic_exchange(&menu_status,NULL); }
int kh_cut_menu_request(int load,int slot){
    if(slot<1||slot>3) return -1;
    if(!kh_cut_enabled||scripted) return -2;
    int empty=0;
    return atomic_compare_exchange_strong(&menu_request,&empty,load?slot+3:slot)?0:-1;
}
static void finish_menu(const char *message){
    fprintf(stderr,"kh: savestate-menu: %s (%s)\n",message,menu_path);
    trigger_head=NULL;
    completed=missed=0;
    atomic_store(&menu_status,message);
    atomic_store(&menu_request,0);
}
static void take_menu_request(u64 frame){
    int request=atomic_load(&menu_request);
    if(!request) return;
    const char *docs=".";
#if KH_IOS_PATHS
    static char directory[4096];
    if(snprintf(directory,sizeof directory,"%s/savestates",kh_autosave_documents())>=(int)sizeof directory ||
       (request<=3 && mkdir(directory,0700) && errno!=EEXIST)){
        finish_menu("savestate-slot-directory");
        return;
    }
    docs=directory;
#endif
    if(!docs||!*docs||snprintf(menu_path,sizeof menu_path,"%s/slot%d.khss",docs,(request-1)%3+1)>=(int)sizeof menu_path){
        finish_menu("savestate-slot-path: Documents directory unavailable");
        return;
    }
    menu_trigger=(struct Trigger){NULL,frame,request>3?LOAD:SAVE,menu_path};
    trigger_head=&menu_trigger;
}

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

    extern int kh_gfx_wrap_on;
    if((kh_gfx_vkd3d_on&&!kh_gfx_wrap_on)||!kh_vclock_sync||ga_deterministic) kh_fatal(NULL,"savestate-profile: requires null or wrapped graphics and frames clock with sync=all");
    const char *audio=getenv("KHNMC_AUDIO");
    if(audio&&*audio&&strcmp(audio,"null")&&strncmp(audio,"wav:",4))
        kh_fatal(NULL,"savestate-profile: external audio device active");
    kh_cut_reserve_boot_object();
    kh_cut_enabled=1;
    scripted=trigger!=NULL;
    const char *boot=getenv("KHNMC_BOOT_SLOT");
    if(boot&&*boot){
        if(scripted || boot[0]<'1'||boot[0]>'3'||boot[1]) kh_fatal(NULL,"savestate-boot-slot: expected 1..3 without a scripted slot trigger");
        if(kh_gfx_vkd3d_on) kh_fatal(NULL,"savestate-cold-gpu-objects");
        if(kh_cut_menu_request(1,boot[0]-'0')) kh_fatal(NULL,"savestate-boot-slot-request");
        take_menu_request(1);
        if(!trigger_head) kh_fatal(NULL,"savestate-boot-slot-path");
        struct SessionSlot *resident=NULL;
        load_started=kh_host_ns();
        const char *err=open_slot(menu_path,&resident,&disk_header,&disk_pad,&disk_docs,&bootstrap);
        if(!err) err=kh_slot_docs_begin(&disk_docs);
        if(err) kh_fatal(NULL,"%s: %s",err,menu_path);
        if(!kh_pad_bootstrap(&disk_pad)) kh_fatal(NULL,"savestate-cold-input-prefix");
        startup_docs=disk_docs; disk_docs=(KhBootDocs){0};
        trigger_head->frame=disk_header.frame; opened=1;
        fprintf(stderr,"kh: savestate-cold-bootstrap request=boot target=%llu\n",(unsigned long long)disk_header.frame);
    }else{
        const char *err=kh_slot_docs_capture(&startup_docs);
        if(err) kh_fatal(NULL,"%s",err);
    }
    if(!trigger) return;
    char *storage=strdup(trigger), *spec=storage;
    if(!spec) kh_fatal(NULL,"savestate-trigger-memory");
    struct Trigger **tail=&trigger_head;
    u64 previous=0;
    int after_load=0;
    while(spec){
        char *next=strstr(spec,",frame:");
        if(next) *next++=0;
        if(strncmp(spec,"frame:",6)||spec[6]<'0'||spec[6]>'9') goto invalid;
        char *end;
        errno=0;
        u64 frame=strtoull(spec+6,&end,10);
        if(errno||(!frame||frame<previous||(frame==previous&&!after_load))||*end!=':') goto invalid;
        previous=frame;
        const char *operation=end+1;
        struct Trigger *item=calloc(1,sizeof *item);
        if(!item) kh_fatal(NULL,"savestate-trigger-memory");
        if(!strcmp(operation,"freeze")) item->op=FREEZE;
        else if(!strcmp(operation,"hash")) item->op=HASH;
        else if(!strcmp(operation,"sync-save")) item->op=SYNC_SAVE;
        else if(!strcmp(operation,"sync-check")) item->op=SYNC_CHECK;
        else if(!strncmp(operation,"census:",7)&&operation[7]) item->op=CENSUS;
        else if(!strncmp(operation,"save:",5)&&operation[5]) item->op=SAVE;
        else if(!strncmp(operation,"load:",5)&&operation[5]) item->op=LOAD;
        else goto invalid;
        after_load=item->op==LOAD;
        item->frame=frame;
        if(item->op==CENSUS||item->op==SAVE||item->op==LOAD){
            item->slot=strdup(strchr(operation,':')+1);
            if(!item->slot) kh_fatal(NULL,"savestate-trigger-memory");
        }
        *tail=item;
        tail=&item->next;
        spec=next;
    }
    free(storage);
    return;
invalid:
    kh_fatal(NULL,"savestate-trigger: expected increasing frame:N:save:SLOT, frame:N:load:SLOT, frame:N:freeze or frame:N:census:PATH");
}

/* Only called at predicate loops. Drop the owner's lock before acknowledging and
 * recheck the predicate under that lock on return, including after a spurious wake. */
void kh_cut_poll(const char *site,pthread_mutex_t *held){
    if(kh_cut_callback_depth||!kh_cut_loader_safe()) return;
    if(held) pthread_mutex_unlock(held);
    pthread_mutex_lock(&mutex);
    if(atomic_load(&kh_cut_requested)){
        struct Park p={parks,cur_thread(),site};
        p.rng=kh_rewind_rng(); p.pending=kh_cut_pending;
        p.stream=kh_cut_on_stream();
        static const struct { const char *site,*name; } waits[]={
            [KH_CUT_SLEEP]={"clock-client","sleeper"}, [KH_CUT_HANDLE]={"handle-wait","handle"},
            [KH_CUT_RELEASE]={"wake-handoff","handoff"}, [KH_CUT_SHELL]={"shell-loop","shell"},
            [KH_CUT_TIMER]={"clock-client","timer"}, [KH_CUT_MIXER]={"clock-deadline","mixer"}
        };
        if(kh_cut_wait_owner&&!strcmp(waits[kh_cut_wait_owner].site,site))
            p.owner=waits[kh_cut_wait_owner].name;
        if(!strcmp(site,"thread-start")) p.owner="unstarted";
        if(p.stream&&(!strcmp(site,"stream-idle")||!strcmp(site,"stream-loop-top")||
                      !strcmp(site,"stream-next-request"))) p.owner="stream";
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
/* Enter the barrier from the outer game-thread dispatcher. The frame call
 * has not sampled input yet. No lifted caller survives the jump. */
u64 kh_game_frames(void);
static _Thread_local jmp_buf *game_dispatch;
static _Thread_local u64 game_entry_sp;
static _Thread_local int at_dispatch;
void kh_resume_154a1b(KCtx *),kh_resume_14fdfe(KCtx *),kh_resume_14ecde(KCtx *);
void kh_resume_121f09(KCtx *),kh_resume_127ed9(KCtx *);

void kh_cut_game_loop(KCtx *g,KhFn entry){
    jmp_buf dispatch;
    game_entry_sp=KH_RSP(g);
    game_dispatch=&dispatch;
    if(setjmp(dispatch)==0) entry(g);
    else{
        at_dispatch=1;
        kh_cut_frame(g,kh_game_frames()+1);
        at_dispatch=0;
        kh_game_frame_step(g);
        kh_resume_154a1b(g);
        kh_resume_14fdfe(g);
        kh_resume_14ecde(g);
        kh_resume_121f09(g);
        kh_resume_127ed9(g);
    }
    game_dispatch=NULL;
}
static int game_chain_known(KCtx *g){
    u64 sp=KH_RSP(g);
    return game_dispatch&&g==cur_thread()->g&&game_entry_sp>=sp&&game_entry_sp-sp==0x190&&
        rd64(g,sp)==GS_BASE+0x154a1b&&rd64(g,sp+0x40)==GS_BASE+0x14fdfe&&
        rd64(g,sp+0x90)==GS_BASE+0x14ecde&&rd64(g,sp+0xe0)==GS_BASE+0x121f09&&
        rd64(g,sp+0x160)==GS_BASE+0x127ed9&&rd64(g,game_entry_sp)==0x7ff7ffff0000ull;
}
void kh_cut_frame(KCtx *g,u64 frame){
    extern _Atomic u64 gfx_presents;
    extern size_t kh_phase_files_seen(void);
    if(!trigger_head) take_menu_request(frame);
    int due=trigger_head&&frame>=trigger_head->frame;
    if(due&&!at_dispatch&&game_chain_known(g)) longjmp(*game_dispatch,1);
    unsigned char digest[32];
    char hex[65];
    /* f_152380 returns GAME_SAVE. This digest covers the complete save block. */
    game_digest(hp(g,GS_BASE+0x9a98b0),0x10fc0,digest);
    for(unsigned i=0;i<sizeof digest;i++) snprintf(hex+i*2,3,"%02x",digest[i]);
    fprintf(stderr,"kh: savestate-frame %llu present=%llu return=%llx world=%d area=%d buttons=%x paths=%zu game-save=%s\n",
            (unsigned long long)frame,(unsigned long long)atomic_load(&gfx_presents),
            (unsigned long long)rd64(g,KH_RSP(g)),kh_route_world(g),kh_route_area(g),
            kh_input_buttons(),kh_phase_files_seen(),hex);
    if(!due) return;
    fprintf(stderr,"kh: savestate-game-dispatch frame=%llu rebuilt=%d\n",(unsigned long long)frame,at_dispatch);
    int census=trigger_head->op==CENSUS;
    const char *slot=trigger_head->slot;

    int load=trigger_head->op==LOAD;
    struct SessionSlot *session=NULL,*candidate=NULL;
    int cold=0;
    if(load){
        const char *err=NULL;
        if(!opened){
            load_started=kh_host_ns();
            err=open_slot(slot,&session,&disk_header,&disk_pad,&disk_docs,&bootstrap);
            if(!err && !session){
                if(kh_gfx_vkd3d_on) err="savestate-cold-gpu-objects";
                else if(disk_docs.size!=startup_docs.size ||
                   (disk_docs.size&&memcmp(disk_docs.data,startup_docs.data,disk_docs.size))) err="savestate-cold-startup-documents";
                else if(!kh_pad_bootstrap(&disk_pad)) err="savestate-cold-input-prefix";
                else if(frame>disk_header.frame) err="savestate-cold-bootstrap-past-cut";
                else if(frame<disk_header.frame){
                    KhBootDocs current={0};
                    err=kh_slot_docs_capture(&current);
                    if(!err) err=kh_slot_docs_begin(&current);
                    free(current.data);
                    if(!err){
                        trigger_head->frame=disk_header.frame;
                        opened=1;
                        fprintf(stderr,"kh: savestate-cold-bootstrap request=%llu target=%llu\n",
                            (unsigned long long)frame,(unsigned long long)disk_header.frame);
                        return;
                    }
                }
            }
        }
        if(err){
            close_load();
            if(trigger_head==&menu_trigger){ finish_menu(err); return; }
            kh_fatal(g,"%s: %s",err,slot);
        }
        cold=!session;
    }
    KThread *threads[4096];
    u64 start=kh_host_ns(),epoch;
    pthread_mutex_lock(&mutex);
    generation++;
    atomic_store(&kh_cut_requested,1);
    extern void kh_cut_wake_shell(void);
    kh_cut_wake_shell();
    unsigned n,cs_waiter=0;
    int missing;
    for(;;){
        n=0;
        for(struct Park *p=parks;p;p=p->next){
            if(n==sizeof threads/sizeof threads[0]) kh_fatal(g,"savestate-registry-overflow");
            threads[n++]=p->thread;
        }
        int expired=kh_host_ns()-start>=1000000000ull;
        missing=kh_cut_threads(threads,n,expired,&cs_waiter);
        if(!missing||cs_waiter||expired) break;
        pthread_mutex_unlock(&mutex);
        struct timespec nap={0,1000000};
        nanosleep(&nap,NULL);
        pthread_mutex_lock(&mutex);
    }
    int sync_test=trigger_head->op==SYNC_SAVE||trigger_head->op==SYNC_CHECK;
    int save=trigger_head->op==SAVE||load||sync_test;
    const char *sync_error=NULL;
    unsigned blocker_tid=0;
    u64 blocker_start=0,blocker_return=0;
    const char *blocker_site=NULL;
    struct KhPendingWait *waits[4096];
    KThread *wait_threads[4096];
    unsigned wait_count=0,voices=0,buffers=0,commands=0;
    int gate_ok=0,stream_busy=0,committing=0;
    char *tmp=NULL;
    int fd=-1;
    if(!missing&&kh_cut_gate_begin(&epoch)){
        gate_ok=kh_gfx_gate_quiesce_bounded(4,10)==KH_GATE_QUIESCED;
        if(gate_ok&&!completed){
            void *callers[64];
            int depth=backtrace(callers,64);
            report_callers(cur_thread()->tid,callers,depth);
            for(struct Park *p=parks;p;p=p->next){
                report_callers(p->thread->tid,p->callers,p->depth);
                fprintf(stderr,"kh: savestate-park tid=%u start=%llx at=%s\n",p->thread->tid,(unsigned long long)p->thread->start,p->site);
                fprintf(stderr,"kh: savestate-wait tid=%u owner=%s\n",p->thread->tid,p->owner?p->owner:"unresolved");
            }
        }
        if(save&&gate_ok){
            /* The stream dispatcher owns only the guest successor and suspend predicate. */
            for(struct Park *p=parks;p;p=p->next){
                if(p->stream){
                    KThread *t=p->thread;
                    if(!strcmp(p->site,"stream-loop-top")||!strcmp(p->site,"stream-next-request")) continue;
                    int idle=0;
                    if(!strcmp(p->site,"stream-idle")&&!pthread_mutex_trylock(&t->m)){
                        idle=t->suspended==1&&rd32(g,GS_BASE+0x713390)==0;
                        pthread_mutex_unlock(&t->m);
                    }
                    if(idle){
                        fprintf(stderr,"kh: savestate-stream-idle tid=%u successor=feade\n",t->tid);
                        continue;
                    }
                    stream_busy=1;
                }
                if(!p->stream&&p->owner){
                    KThread *t=p->thread;
                    if(!strcmp(p->site,"handle-wait")||!strcmp(p->site,"wake-handoff")){
                        if(!p->pending){ blocker_tid=t->tid; blocker_site="unrecorded handle wait";
                            blocker_start=t->start; blocker_return=rd64(t->g,KH_RSP(t->g)); }
                        else { waits[wait_count]=p->pending; wait_threads[wait_count++]=t; }
                    }
                    continue;
                }
                if(blocker_site&&strcmp(p->site,"self-suspend")) continue;
                blocker_tid=p->thread->tid;
                blocker_start=p->thread->start;
                blocker_site=p->site;
                KCtx *ctx=p->thread->g;
                blocker_return=rd64(ctx,KH_RSP(ctx));
            }
        }
        if(save&&gate_ok&&!stream_busy&&!blocker_site&&at_dispatch){
            if(!sync_test){
                if(!load||cold){
                    candidate=calloc(1,sizeof *candidate);
                    if(!candidate||!(candidate->points=calloc(n+1,sizeof *candidate->points))||
                       !(candidate->path=strdup(slot))) kh_fatal(g,"savestate-slot-memory");
                    candidate->nthreads=n+1; candidate->header.frame=frame; session=candidate;
                }
                if(load&&!cold) sync_error=read_resident(session);
                if(!sync_error) sync_error=session_check(session,load&&!cold,n);
                if(!sync_error&&(!load||cold)) qsort(session->points,session->nthreads,sizeof *session->points,point_order);
                if(!sync_error&&(load||kh_gfx_vkd3d_on)){
                    committing=kh_cut_gate_commit(epoch);
                    if(!committing) sync_error="savestate-lifecycle-changed";
                    else if(load&&getenv("KHNMC_TEST_SAVESTATE_HASH")){
                        extern void kh_cut_arena_census(KCtx *,void (*)(KCtx *,u64,u64));
                        kh_cut_arena_census(g,report_extent);
                    }
                }
                if(!sync_error) sync_error=kh_gpu_cut_check(&session->gpu,load);
                if(!sync_error&&(!load||cold)){
                    session->sync=kh_sync_capture(waits,wait_threads,wait_count);
                    session->audio=kh_audio_capture();
                    session->files=kh_crt_files_capture();
                    kh_pad_capture(&session->pad);
                    kh_records_begin(&session->arena,0);
                    disk_owners(session,&session->arena);
                }
                if(!sync_error&&cold){
                    if(frame!=disk_header.frame||!kh_pad_caught_up()) sync_error="savestate-cold-pad-position";
                    else if(fseeko(bootstrap,sizeof disk_header,SEEK_SET)) sync_error="savestate-load-seek";
                    else{
                        disk_cpu(session,&session->arena);
                        kh_records_end(&session->arena);
                        KhPadHistory checked={0}; KhBootDocs checked_docs={0};
                        sync_error=kh_slot_read(bootstrap,&disk_header,&disk_arena,&checked,&checked_docs,&session->arena);
                        free(checked.changes); free(checked_docs.data);
                    }
                    if(!sync_error){
                        kh_records_free(&session->arena);
                        session->header=disk_header;
                    }
                }
                if(!sync_error&&load){
                    kh_records_begin(&disk_arena,1);
                    disk_owners(session,&disk_arena);
                    sync_error=kh_sync_restore(session->sync,waits,wait_threads,wait_count);
                }
                if(!sync_error){
                    KhRecords *cpu=load?&disk_arena:&session->arena;
                    kh_gpu_cut_apply(session->gpu,load);
                    disk_cpu(session,cpu);
                    kh_records_end(cpu);
                    if(load){ kh_pad_restore(&disk_pad); kh_audio_restore(session->audio); kh_crt_files_restore(session->files); kh_slot_docs_finish(); }
                    voices=kh_cut_audio_graph(&buffers,&commands);
                    fprintf(stderr,"kh: savestate-%s at=%llu resumed-frame=%llu sample=%llu voices=%u buffers=%u commands=%u\n",
                        load?"loaded":"captured",(unsigned long long)frame,(unsigned long long)(kh_game_frames()+1),
                        (unsigned long long)kh_audio_output_frame(),voices,buffers,commands);
                }
            }else{
            struct KhAudioSlot *audio=kh_audio_capture();
            voices=kh_cut_audio_graph(&buffers,&commands);
            struct KhSyncSlot *kernel=kh_sync_capture(waits,wait_threads,wait_count);
            fprintf(stderr,"kh: savestate-sync-captured waiters=%u\n",wait_count);
            if(trigger_head->op==SYNC_SAVE){
                if(test_kernel) kh_sync_discard(test_kernel);
                test_kernel=kernel;
                kh_audio_discard(test_audio); test_audio=audio;
            }else{
                if(trigger_head->op==SYNC_CHECK){
                    sync_error=test_kernel?kh_sync_restore(test_kernel,waits,wait_threads,wait_count):"savestate-sync-test-empty";
                    if(!sync_error){
                        kh_audio_restore(test_audio);
                        unsigned aq,ac,av=kh_cut_audio_graph(&aq,&ac);
                        fprintf(stderr,"kh: savestate-mixer-restored voices=%u buffers=%u commands=%u\n",av,aq,ac);
                        kh_audio_restore(audio);
                        const char *rollback=kh_sync_restore(kernel,waits,wait_threads,wait_count);
                        if(rollback) kh_fatal(g,"savestate-sync-test-rollback: %s",rollback);
                        fprintf(stderr,"kh: savestate-sync-test restored both cuts at frame %llu\n",(unsigned long long)frame);
                    }
                }
                kh_sync_discard(kernel); kh_audio_discard(audio);
            }
            }
        }
        if(!save&&!completed&&gate_ok){
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
            kh_route_map_report(g,stderr,"savestate candidate");
        }
        if(gate_ok&&!save&&trigger_head->op!=HASH) kh_cut_rebuild_fibers();
        if(committing) kh_cut_gate_committed_end(epoch);
        else gate_ok=kh_cut_gate_end(epoch)&&gate_ok;
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
    if(stream_busy||missing||!gate_ok){
        discard_slot(candidate);
        if(load) close_load();
        if(cold){
            if(trigger_head==&menu_trigger){ finish_menu("savestate-cold-cut-missed"); return; }
            kh_fatal(g,"savestate-cold-cut-missed: frame %llu",(unsigned long long)frame);
        }
        if(cs_waiter) kh_warn("savestate-critical-section: tid=%u still acquiring at frame %llu",cs_waiter,(unsigned long long)frame);
        if(++missed==4){
            const char *reason=cs_waiter?"savestate-critical-section-timeout":stream_busy?"savestate-stream-drain-timeout":"savestate-cut-refused";
            if(trigger_head==&menu_trigger){ finish_menu(reason); return; }
            kh_fatal(g,"%s: four successive incomplete cuts; last frame %llu",reason,(unsigned long long)frame);
        }
        kh_warn("savestate-cut-deferred: frame %llu, missing %d, gate %d, stream-busy %d",
                (unsigned long long)frame,missing,gate_ok,stream_busy);
        return;
    }
    completed++;
    missed=0;
    if(load){
        if(!sync_error) fprintf(stderr,"kh: savestate-load-time cold=%d request-through-restore-us=%llu\n",cold,
                (unsigned long long)((kh_host_ns()-load_started)/1000));
        close_load();
    }
    if(candidate){
        if(!sync_error && cold){ kh_records_free(&candidate->arena); retain_slot(candidate); }
        else if(!sync_error) sync_error=publish_slot(candidate);
        if(sync_error) discard_slot(candidate);
    }
    if(sync_error&&trigger_head==&menu_trigger){ finish_menu(sync_error); return; }
    if(sync_error) kh_fatal(g,"%s: checkpoint at frame %llu",sync_error,(unsigned long long)frame);
    if(save&&(blocker_site||!at_dispatch)){
        const char *error;
        char detail[256];
        if(!at_dispatch){
            error="savestate-game-continuation";
            snprintf(detail,sizeof detail,"unsupported frame caller %llx",(unsigned long long)rd64(g,KH_RSP(g)));
        }else{
            error="savestate-worker-continuation";
            snprintf(detail,sizeof detail,"tid=%u start=%llx wait=%s guest-return=%llx",blocker_tid,
                     (unsigned long long)blocker_start,blocker_site,(unsigned long long)blocker_return);
        }
        if(trigger_head==&menu_trigger){
            kh_warn("%s: %s slot=%s",error,detail,slot);
            finish_menu(error);
            return;
        }
        kh_fatal(g,"%s: %s slot=%s",error,detail,slot?slot:"sync-test");
    }
    if(trigger_head==&menu_trigger){ finish_menu(load?"State loaded":"State saved"); return; }
    if(completed==(trigger_head->op==FREEZE?20:1)){
        struct Trigger *done=trigger_head;
        trigger_head=done->next;
        free(done->slot);
        free(done);
        completed=0;
    }
}

/* Unit fixture: take the same predicate-loop barrier as a field save. The only
 * running guest worker in this fixture is the thread passed by the test. */
int kh_sync_test_cut(KThread *worker,struct KhSyncSlot **slot,int load){
    struct timespec nap={0,1000000};
    pthread_mutex_lock(&mutex);
    generation++;
    atomic_store(&kh_cut_requested,1);
    u64 start=kh_host_ns();
    struct Park *p=NULL;
    while(kh_host_ns()-start<3000000000ull){
        for(p=parks;p&&p->thread!=worker;p=p->next){}
        if(p&&p->pending) break;
        pthread_mutex_unlock(&mutex);
        nanosleep(&nap,NULL);
        pthread_mutex_lock(&mutex);
    }
    int ok=p&&p->pending;
    if(ok){
        if(load) ok=kh_sync_restore(*slot,&p->pending,&p->thread,1)==NULL;
        else *slot=kh_sync_capture(&p->pending,&p->thread,1);
    }
    atomic_store(&kh_cut_requested,0);
    pthread_cond_broadcast(&resume);
    while(parks) pthread_cond_wait(&resume,&mutex);
    pthread_mutex_unlock(&mutex);
    return ok;
}
