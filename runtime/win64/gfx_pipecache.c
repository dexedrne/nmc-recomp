/* gfx_pipecache.c : the disk pipeline cache (TASK43; IOS-GRAPHICS-PLAN.md section 4 step 12).  What it is, where the file lives, the
 * file's layout and when it is written are in gfx_pipecache.h.  This file: the file image (pure), reading and writing the file, the
 * run's configuration, the load and save rules with their log lines and counters, the exit report, and the selftest (no device: the
 * driver's side is a stand-in there; the same rules on a real device are tested by the vkd3d selftest in gfx_vkd3d.c).
 * Compiled in every build: without the vkd3d backend nothing registers a provider and nothing is configured. */
#include "khnmc_rt.h"
#include "gfx_pipecache.h"
#include "gfx_present.h"
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define P_(x) ((unsigned long long)(x))
extern int kh_run_is_selftest;
const char *kh_host_caches_dir(void);                 /* host_env.c: <home>/Library/Caches/khnmc in an app-container build, else "" */

/* ---- the file image (pure) ---- */
static const uint8_t PC_MAGIC[8]={'K','H','N','M','C','P','C',0};
static const char *const pc_names[KH_PC_N]={"ok","pipecache-missing","pipecache-unreadable","pipecache-truncated","pipecache-garbage","pipecache-header-corrupt",
    "pipecache-version","pipecache-too-large","pipecache-length","pipecache-checksum","pipecache-foreign-device","pipecache-empty","pipecache-driver-refused","pipecache-load-unfinished"};
const char *kh_pc_result_name(int r){ return r>=0&&r<KH_PC_N?pc_names[r]:"pipecache-?"; }
uint64_t kh_pc_fnv(const void *p,size_t n){ const uint8_t *b=(const uint8_t*)p; uint64_t h=0xcbf29ce484222325ull; for(size_t i=0;i<n;i++){ h^=b[i]; h*=0x100000001b3ull; } return h; }
static void put32(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p,uint64_t v){ put32(p,(uint32_t)v); put32(p+4,(uint32_t)(v>>32)); }
static uint32_t get32(const uint8_t *p){ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p){ return (uint64_t)get32(p)|(uint64_t)get32(p+4)<<32; }
static void uuid_text(char out[40],const uint8_t u[16]){ for(int i=0;i<16;i++) snprintf(out+2*i,3,"%02x",u[i]); }
void kh_pc_header_make(uint8_t h[KH_PC_HEADER_BYTES],const KhPcDev *dev,const void *payload,size_t len){
    memset(h,0,KH_PC_HEADER_BYTES); memcpy(h,PC_MAGIC,8); put32(h+8,KH_PC_VERSION); put32(h+12,KH_PC_HEADER_BYTES);
    put32(h+16,dev->vendor); put32(h+20,dev->device); put32(h+24,dev->driver); memcpy(h+32,dev->uuid,16);
    put64(h+48,(uint64_t)len); put64(h+56,kh_pc_fnv(payload,len)); put64(h+64,kh_pc_fnv(h,64));
}
int kh_pc_parse(const uint8_t *file,size_t n,const KhPcDev *dev,uint64_t cap,size_t *off,size_t *len,char *why,size_t whycap){
    *off=0; *len=0; if(whycap) why[0]=0;
    size_t m=n<8?n:8;
    if(memcmp(file,PC_MAGIC,m)){ snprintf(why,whycap,"the file does not start with this runtime's magic (%zu bytes; not a pipeline cache of ours)",n); return KH_PC_GARBAGE; }
    if(n<KH_PC_HEADER_BYTES){ snprintf(why,whycap,"%zu bytes, shorter than the %u-byte header",n,KH_PC_HEADER_BYTES); return KH_PC_TRUNCATED; }
    if(get64(file+64)!=kh_pc_fnv(file,64)){ snprintf(why,whycap,"the header's own checksum does not match (stored %016llx, computed %016llx)",P_(get64(file+64)),P_(kh_pc_fnv(file,64))); return KH_PC_HEADER_CORRUPT; }
    if(get32(file+8)!=KH_PC_VERSION||get32(file+12)!=KH_PC_HEADER_BYTES){ snprintf(why,whycap,"file version %u with a %u-byte header (this runtime reads version %u, %u bytes)",get32(file+8),get32(file+12),KH_PC_VERSION,KH_PC_HEADER_BYTES); return KH_PC_VERSION_UNKNOWN; }
    uint64_t plen=get64(file+48);
    if(plen>cap){ snprintf(why,whycap,"the header says %llu payload bytes, above the cap of %llu bytes",P_(plen),P_(cap)); return KH_PC_TOO_LARGE; }
    if((uint64_t)n<KH_PC_HEADER_BYTES+plen){ snprintf(why,whycap,"the header says %llu payload bytes, the file has %llu",P_(plen),P_(n-KH_PC_HEADER_BYTES)); return KH_PC_TRUNCATED; }
    if((uint64_t)n>KH_PC_HEADER_BYTES+plen){ snprintf(why,whycap,"%llu bytes after the %llu payload bytes the header names",P_(n-KH_PC_HEADER_BYTES-plen),P_(plen)); return KH_PC_LENGTH; }
    if(!plen){ snprintf(why,whycap,"a header with no payload"); return KH_PC_EMPTY; }
    if(memcmp(file+32,dev->uuid,16)||get32(file+16)!=dev->vendor||get32(file+20)!=dev->device||get32(file+24)!=dev->driver){
        char a[40],b[40]; uuid_text(a,file+32); uuid_text(b,dev->uuid);
        snprintf(why,whycap,"written by another device: vendor 0x%x device 0x%x driver 0x%x pipelineCacheUUID %s; this device: vendor 0x%x device 0x%x driver 0x%x pipelineCacheUUID %s",
                 get32(file+16),get32(file+20),get32(file+24),a,dev->vendor,dev->device,dev->driver,b); return KH_PC_FOREIGN_DEVICE; }
    uint64_t sum=kh_pc_fnv(file+KH_PC_HEADER_BYTES,(size_t)plen);
    if(sum!=get64(file+56)){ snprintf(why,whycap,"the payload checksum does not match (stored %016llx, computed %016llx over %llu bytes)",P_(get64(file+56)),P_(sum),P_(plen)); return KH_PC_CHECKSUM; }
    *off=KH_PC_HEADER_BYTES; *len=(size_t)plen; return KH_PC_OK;
}

/* ---- files ---- */
int kh_pc_file_load(const char *path,const KhPcDev *dev,uint64_t cap,uint8_t **file,const uint8_t **payload,size_t *len,char *why,size_t whycap){
    *file=NULL; *payload=NULL; *len=0; if(whycap) why[0]=0;
    int fd=open(path,O_RDONLY);
    if(fd<0){ int e=errno; snprintf(why,whycap,"%s",strerror(e)); return e==ENOENT?KH_PC_MISSING:KH_PC_UNREADABLE; }
    struct stat st;
    if(fstat(fd,&st)){ snprintf(why,whycap,"fstat: %s",strerror(errno)); close(fd); return KH_PC_UNREADABLE; }
    if(!S_ISREG(st.st_mode)){ snprintf(why,whycap,"not a regular file"); close(fd); return KH_PC_UNREADABLE; }
    if((uint64_t)st.st_size>cap+KH_PC_HEADER_BYTES){ snprintf(why,whycap,"the file has %llu bytes, above the cap of %llu bytes (+ the %u-byte header): not read",P_(st.st_size),P_(cap),KH_PC_HEADER_BYTES); close(fd); return KH_PC_TOO_LARGE; }
    size_t n=(size_t)st.st_size; uint8_t *b=malloc(n?n:1);
    if(!b){ snprintf(why,whycap,"out of memory for %zu bytes",n); close(fd); return KH_PC_UNREADABLE; }
    size_t got=0; while(got<n){ ssize_t r=read(fd,b+got,n-got); if(r<0&&errno==EINTR) continue; if(r<=0) break; got+=(size_t)r; }
    close(fd);
    if(got!=n){ snprintf(why,whycap,"read %zu of %zu bytes",got,n); free(b); return KH_PC_UNREADABLE; }
    size_t off=0; int r=kh_pc_parse(b,n,dev,cap,&off,len,why,whycap);
    if(r!=KH_PC_OK){ free(b); return r; }
    *file=b; *payload=b+off; return KH_PC_OK;
}
/* review C43 n=6 / n=9: the directory `path` lies in, made with its parents (a cache path named under a directory nothing has made
 * yet, e.g. the conf token @CACHES@; or a caches directory the system emptied while the app ran).  1 = it is a directory now */
static int pc_mkparent(const char *path){
    char b[KH_TMP_MAX+96]; size_t n=strlen(path); if(n>=sizeof b) return 0; memcpy(b,path,n+1);
    char *sl=strrchr(b,'/'); if(!sl||sl==b) return 0; *sl=0;
    for(char *q=b+1;*q;q++) if(*q=='/'){ *q=0; mkdir(b,0755); *q='/'; }
    mkdir(b,0755); struct stat st; return !stat(b,&st)&&S_ISDIR(st.st_mode);
}
static void pc_count_dir_made(void);
static int write_all(int fd,const void *p,size_t n){ const uint8_t *b=(const uint8_t*)p; size_t done=0;
    while(done<n){ ssize_t r=write(fd,b+done,n-done); if(r<0&&errno==EINTR) continue; if(r<=0) return -1; done+=(size_t)r; } return 0; }
int kh_pc_file_save(const char *path,const KhPcDev *dev,const void *payload,size_t len,char *why,size_t whycap){
    char tmp[KH_TMP_MAX+96]; if(whycap) why[0]=0;
    if(snprintf(tmp,sizeof tmp,"%s.tmp",path)>=(int)sizeof tmp){ snprintf(why,whycap,"the path is too long"); return -1; }
    uint8_t h[KH_PC_HEADER_BYTES]; kh_pc_header_make(h,dev,payload,len);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644), e=errno;
    if(fd<0&&e==ENOENT&&pc_mkparent(path)){                 /* the directory is not there: made once, said, counted */
        fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); e=errno;
        if(fd>=0){ pc_count_dir_made(); fprintf(stderr,"kh: gfx pipeline cache: the directory of %s was not there: it was made (counted)\n",path); } }
    if(fd<0){ snprintf(why,whycap,"cannot create the temporary file %s (%s)",tmp,strerror(e)); return -1; }
    if(write_all(fd,h,sizeof h)||write_all(fd,payload,len)){ snprintf(why,whycap,"writing %s failed (%s)",tmp,strerror(errno)); close(fd); unlink(tmp); return -1; }
    if(fsync(fd)){ snprintf(why,whycap,"fsync of %s failed (%s)",tmp,strerror(errno)); close(fd); unlink(tmp); return -1; }
    if(close(fd)){ snprintf(why,whycap,"closing %s failed (%s)",tmp,strerror(errno)); unlink(tmp); return -1; }
    if(rename(tmp,path)){ snprintf(why,whycap,"renaming %s onto %s failed (%s)",tmp,path,strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

/* ---- configuration, state, counters ---- */
typedef struct { int on; char path[KH_TMP_MAX+64]; uint64_t cap; const char *src; } PcConf;
static PcConf pc, pc_run;                               /* what is in force; the run's own (the selftests put another in force for a while) */
static pthread_mutex_t pm=PTHREAD_MUTEX_INITIALIZER;   /* pc, the counters, the disk state */
static pthread_mutex_t ps=PTHREAD_MUTEX_INITIALIZER;   /* one save at a time */
static KhPcCounters cnt; static uint64_t c_points, c_points_off;
static int disk_valid; static size_t disk_len; static uint64_t disk_sum; static KhPcDev disk_dev;      /* the payload this process last loaded or wrote */
/* review C43 n=9: what the FILE looked like when this process last loaded or wrote it.  A save point trusts disk_valid only while the
 * file still looks like that (a file the system removed while the app ran is written again); and after a load that could not READ the
 * file (not a judgement of its content) the file is not replaced by a run that made no pipeline */
typedef struct { int have; dev_t dev; ino_t ino; off_t size; time_t mtime; } PcFileId;
static PcFileId disk_id; static int load_keep, pc_test_load_fault; static uint64_t c_kept_said;
/* 1 = *id is the file's identity; 0 = no such file; -1 = it cannot be looked at */
static int pc_file_id(const char *path,PcFileId *id){ struct stat st; memset(id,0,sizeof *id); if(stat(path,&st)) return errno==ENOENT?0:-1;
    id->have=1; id->dev=st.st_dev; id->ino=st.st_ino; id->size=st.st_size; id->mtime=st.st_mtime; return 1; }
static int pc_id_same(const PcFileId *a,const PcFileId *b){ return a->have&&b->have&&a->dev==b->dev&&a->ino==b->ino&&a->size==b->size&&a->mtime==b->mtime; }
static void pc_count_dir_made(void){ pthread_mutex_lock(&pm); cnt.dirs_made++; pthread_mutex_unlock(&pm); }
static void pc_marker_remove(const char *path){ char mk[KH_TMP_MAX+96]; if(snprintf(mk,sizeof mk,"%s.loading",path)<(int)sizeof mk) unlink(mk); }
static const KhPcProvider *prov; static _Atomic int pc_exiting; static int pc_backend;
static uint64_t save_ns_max, fetch_ns_max;

/* "n" MiB, 1..1024 -> bytes; 0 = malformed or out of range */
static uint64_t pc_cap_parse(const char *s){
    if(!s||*s<'0'||*s>'9') return 0; char *e; unsigned long long v=strtoull(s,&e,10); if(*e||v<1||v>1024) return 0; return (uint64_t)v<<20; }
/* KHNMC_GFX_PIPECACHE_RESET=1: the file (and a temporary file a killed save left) is removed at start-up, so this run is a first run
 * (the phone's run 1 of a run 1 / run 2 comparison: tools/ios/mac/device_run.sh PIPECACHE=fresh).  1 = removed, 0 = there was none,
 * -1 = it is there and could not be removed (why) */
static int pc_reset(const char *path,char *why,size_t whycap){
    char tmp[KH_TMP_MAX+96]; if(snprintf(tmp,sizeof tmp,"%s.tmp",path)<(int)sizeof tmp) unlink(tmp);
    pc_marker_remove(path);                                   /* a load marker belongs to the file that goes */
    if(!unlink(path)) return 1; if(errno==ENOENT) return 0; snprintf(why,whycap,"%s",strerror(errno)); return -1; }
/* review C43 n=3: WHICH file a run uses, as one decision with no side effect but the lazy caches() call (kh_pc_init acts on it):
 *   KHNMC_GFX_PIPECACHE=<path>  that file;  =off  none;  unset or empty: the app container's <caches>/pipe.bin when the run has the vkd3d
 *   backend and caches() names a directory (it is asked only then: the directory is made only for a run that can use it), else none.
 *   A named file without the backend does nothing (said); a --selftest run never uses the configured file (said). */
enum { PCI_NONE=0, PCI_OFF, PCI_NO_BACKEND, PCI_SELFTEST, PCI_ON, PCI_PATH_LONG };
static int pc_decide(const char *env,const char *(*caches)(void),int backend,int selftest,PcConf *c){
    int off=env&&!strcmp(env,"off"), named=env&&*env&&!off; c->on=0; c->path[0]=0; c->src="";
    if(named){ if(snprintf(c->path,sizeof c->path,"%s",env)>=(int)sizeof c->path){ c->path[0]=0; return PCI_PATH_LONG; } c->src="KHNMC_GFX_PIPECACHE"; }
    else if(!off&&backend){ const char *d=caches();          /* app-container builds only ("" elsewhere: unset = no cache) */
        if(d&&*d){ if(snprintf(c->path,sizeof c->path,"%s/pipe.bin",d)>=(int)sizeof c->path){ c->path[0]=0; return PCI_PATH_LONG; } c->src="the app container's default"; } }
    if(!c->path[0]) return off&&backend?PCI_OFF:PCI_NONE;
    if(!backend) return PCI_NO_BACKEND;                       /* the null backend makes no pipeline: an explicit request is said to do nothing */
    if(selftest) return PCI_SELFTEST;
    c->on=1; return PCI_ON;
}
void kh_pc_init(int vkd3d_backend_on){
    const char *e=getenv("KHNMC_GFX_PIPECACHE"), *m=getenv("KHNMC_GFX_PIPECACHE_MAX_MB");
    PcConf c; memset(&c,0,sizeof c); c.cap=KH_PC_DEFAULT_CAP; pc_backend=vkd3d_backend_on;
    if(m&&*m){ c.cap=pc_cap_parse(m); if(!c.cap){ kh_warn("pipeline cache disabled: invalid capacity"); return; } }
    int k=pc_decide(e,kh_host_caches_dir,vkd3d_backend_on,kh_run_is_selftest,&c);
    if(k==PCI_PATH_LONG){ kh_warn("pipeline cache disabled: path too long"); return; }
    if(k==PCI_OFF) fprintf(stderr,"kh: gfx pipeline cache: KHNMC_GFX_PIPECACHE=off: no disk pipeline cache in this run\n");
    if(k==PCI_NO_BACKEND) fprintf(stderr,"kh: gfx pipeline cache: KHNMC_GFX_PIPECACHE=%s is set, but this run has no vkd3d backend (KHNMC_GFX): no pipeline cache is loaded or saved\n",c.path);
    if(k==PCI_SELFTEST) fprintf(stderr,"kh: gfx pipeline cache: %s (%s) is NOT used in a --selftest run (the selftest's own cache tests write into the scratch directory)\n",c.path,c.src);
    if(k!=PCI_ON) return;
    pthread_mutex_lock(&pm); pc=pc_run=c; pthread_mutex_unlock(&pm);
    fprintf(stderr,"kh: gfx pipeline cache: file %s (%s), cap %llu MiB: read when vkd3d creates its VkPipelineCache, written only while the game is parked at the suspend gate or at exit\n",c.path,c.src,P_(c.cap>>20));
    { const char *rs=getenv("KHNMC_GFX_PIPECACHE_RESET");
      if(rs&&*rs){ if(strcmp(rs,"1")){ kh_warn("pipeline cache reset disabled: expected 1"); return; }
          char why[256]; int r=pc_reset(c.path,why,sizeof why);
          if(r<0) fprintf(stderr,"kh: gfx pipeline cache: pipecache-reset-failed: KHNMC_GFX_PIPECACHE_RESET=1, but %s could NOT be removed (%s): this run reads it as it is\n",c.path,why);
          else fprintf(stderr,"kh: gfx pipeline cache: KHNMC_GFX_PIPECACHE_RESET=1: %s: this run starts with an empty cache and writes a new file\n",r?"the file was removed":"there was no file"); } }
}
int kh_pc_active(void){ pthread_mutex_lock(&pm); int r=pc.on; pthread_mutex_unlock(&pm); return r; }
const char *kh_pc_path(void){ return pc.path; }
uint64_t kh_pc_cap(void){ pthread_mutex_lock(&pm); uint64_t c=pc.cap; pthread_mutex_unlock(&pm); return c; }
void kh_pc_set_provider(const KhPcProvider *p){ pthread_mutex_lock(&pm); prov=p; pthread_mutex_unlock(&pm); }
void kh_pc_test_config(const char *path,uint64_t cap){
    pthread_mutex_lock(&pm);
    if(path){ memset(&pc,0,sizeof pc); pc.on=1; snprintf(pc.path,sizeof pc.path,"%s",path); pc.cap=cap?cap:KH_PC_DEFAULT_CAP; pc.src="selftest"; } else pc=pc_run;
    disk_valid=0; load_keep=0; memset(&disk_id,0,sizeof disk_id); pthread_mutex_unlock(&pm);
}
void kh_pc_test_exiting(int on){ atomic_store(&pc_exiting,on); }
uint64_t kh_pc_save_points(void){ pthread_mutex_lock(&pm); uint64_t n=c_points; pthread_mutex_unlock(&pm); return n; }
void kh_pc_counters(KhPcCounters *c){ pthread_mutex_lock(&pm); *c=cnt; pthread_mutex_unlock(&pm); }

/* ---- load ---- */
int kh_pc_load(const KhPcDev *dev,uint8_t **file,const uint8_t **payload,size_t *len){
    *file=NULL; *payload=NULL; *len=0;
    pthread_mutex_lock(&pm); PcConf c=pc; int fault=pc_test_load_fault; pc_test_load_fault=0; pthread_mutex_unlock(&pm);
    if(!c.on) return KH_PC_MISSING;
    char why[640], mk[KH_TMP_MAX+96]; int r; struct stat ms, fs; snprintf(mk,sizeof mk,"%s.loading",c.path);
    /* review C43 n=12: a marker of the load guard is still there: the last time this file's bytes were handed to the driver, the call never
     * came back (a crash, a hang, or the process was ended inside vkCreatePipelineCache).  The file goes; this run is a first run */
    if(!stat(mk,&ms)){
        if(stat(c.path,&fs)){ unlink(mk);                    /* the marker without its file: nothing to guard against */
            fprintf(stderr,"kh: gfx pipeline cache: a load marker %s without its cache file was removed\n",mk); }
        else { int gone=!unlink(c.path), e=errno; if(gone) unlink(mk);
            pthread_mutex_lock(&pm); cnt.load_by[KH_PC_LOAD_UNFINISHED]++; cnt.load_rejected++; load_keep=0; pthread_mutex_unlock(&pm);
            if(gone) fprintf(stderr,"kh: gfx pipeline cache: %s: %s: the last launch handed this file's %lld bytes to the Vulkan driver and never came back from that call (the marker %s was still there: a crash, a hang or a kill inside vkCreatePipelineCache): the file was REMOVED, an EMPTY cache is used and the run goes on (counted; the next save writes a new file)\n",
                             c.path,kh_pc_result_name(KH_PC_LOAD_UNFINISHED),(long long)fs.st_size,mk);
            else fprintf(stderr,"kh: gfx pipeline cache: %s: %s: the last launch never came back from the driver with this file's %lld bytes (the marker %s was still there), and the file could NOT be removed (%s): it is NOT loaded, an EMPTY cache is used and the run goes on (counted; the marker stays)\n",
                         c.path,kh_pc_result_name(KH_PC_LOAD_UNFINISHED),(long long)fs.st_size,mk,strerror(e));
            return KH_PC_LOAD_UNFINISHED; } }
    if(fault){ r=fault; snprintf(why,sizeof why,"selftest: an injected read failure"); }
    else r=kh_pc_file_load(c.path,dev,c.cap,file,payload,len,why,sizeof why);
    int keep=r==KH_PC_UNREADABLE||r==KH_PC_TOO_LARGE;        /* the file was not read: nothing is known against its content (review C43 n=9) */
    pthread_mutex_lock(&pm); if(r>=0&&r<KH_PC_N) cnt.load_by[r]++; if(r==KH_PC_MISSING) cnt.load_missing++; else if(r!=KH_PC_OK) cnt.load_rejected++; load_keep=keep; pthread_mutex_unlock(&pm);
    if(r==KH_PC_MISSING) fprintf(stderr,"kh: gfx pipeline cache: %s: no file yet (%s): an empty cache is used (a first run)\n",c.path,kh_pc_result_name(r));
    else if(r!=KH_PC_OK) fprintf(stderr,"kh: gfx pipeline cache: %s: %s: %s: an EMPTY cache is used and the run goes on (counted; %s)\n",c.path,kh_pc_result_name(r),why,
                                 keep?"the file was not read: it is left as it is unless this run creates pipelines, then the next save replaces it":"the file is replaced at the next save");
    if(r==KH_PC_OK){                                         /* the load guard: the marker stays until the driver's call has returned (kh_pc_note_*) */
        int fd=open(mk,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd>=0) close(fd);
        else { int e=errno; pthread_mutex_lock(&pm); cnt.marker_failed++; pthread_mutex_unlock(&pm);
            fprintf(stderr,"kh: gfx pipeline cache: pipecache-marker-failed: the load marker %s cannot be written (%s): the bytes are handed to the driver WITHOUT the guard against a launch that ends inside that call (counted)\n",mk,strerror(e)); } }
    return r;
}
void kh_pc_note_loaded(size_t len,uint64_t payload_sum,const KhPcDev *dev,uint64_t driver_ns){
    char u[40]; uuid_text(u,dev->uuid);
    pthread_mutex_lock(&pm); const char *path=pc.path; pc_marker_remove(path);
    cnt.load_ok++; cnt.load_bytes+=len; disk_valid=1; disk_len=len; disk_sum=payload_sum; disk_dev=*dev; pc_file_id(path,&disk_id); load_keep=0; pthread_mutex_unlock(&pm);
    fprintf(stderr,"kh: gfx pipeline cache: loaded %zu bytes from %s (payload checksum %016llx, written by this device: vendor 0x%x device 0x%x driver 0x%x pipelineCacheUUID %s); the driver took %.1f ms to build its VkPipelineCache from them\n",
            len,path,P_(payload_sum),dev->vendor,dev->device,dev->driver,u,driver_ns/1e6);
}
void kh_pc_note_driver_refused(size_t len,int vk_result){
    pthread_mutex_lock(&pm); const char *path=pc.path; pc_marker_remove(path);
    cnt.driver_refused++; cnt.load_by[KH_PC_DRIVER_REFUSED]++; cnt.load_by[KH_PC_OK]--; cnt.load_rejected++; load_keep=1; pthread_mutex_unlock(&pm);
    fprintf(stderr,"kh: gfx pipeline cache: %s: %s: the Vulkan driver refused the %zu bytes (vkCreatePipelineCache: VkResult %d) although the file's own checks passed: an EMPTY cache is used and the run goes on (counted; the file is left as it is unless this run creates pipelines, then the next save replaces it)\n",
            path,kh_pc_result_name(KH_PC_DRIVER_REFUSED),len,vk_result);
}
void kh_pc_note_probe_skipped(void){ pthread_mutex_lock(&pm); cnt.probes++; pthread_mutex_unlock(&pm); }

/* ---- save ---- */
/* THE RULE: a save happens only while the game is parked at the suspend gate (the gate set, nobody admitted, the caller not admitted)
 * or while the process is ending.  1 = allowed; 0 = not (why says what was found) */
static int save_allowed(int at,char *why,size_t cap){
    if(at==KH_PC_AT_EXIT){ if(atomic_load(&pc_exiting)) return 1; snprintf(why,cap,"an at-exit save while the process is not ending"); return 0; }
    if(at==KH_PC_AT_PARKED){ int set=kh_gfx_gate_is_set(), adm=kh_gfx_gate_admitted(), me=kh_gfx_gate_thread_admitted();
        if(set&&!adm) return 1;                               /* a caller that is itself admitted is one of the admitted */
        snprintf(why,cap,"the suspend gate is %s, %d thread(s) admitted%s",set?"set":"CLEAR",adm,me?" (the calling thread is one of them)":""); return 0; }
    snprintf(why,cap,"an unknown save point %d",at); return 0;
}
int kh_gfx_pipecache_save(int at,uint64_t deadline_ns){
    pthread_mutex_lock(&pm); PcConf c=pc; const KhPcProvider *p=prov; pthread_mutex_unlock(&pm);
    const char *when=at==KH_PC_AT_EXIT?"at exit":"while parked";
    if(!c.on){ pthread_mutex_lock(&pm); cnt.save_off++; pthread_mutex_unlock(&pm); return KH_PCS_OFF; }
    char why[640];
    if(!save_allowed(at,why,sizeof why)){
        pthread_mutex_lock(&pm); uint64_t k=cnt.save_refused++; pthread_mutex_unlock(&pm);
        if(k<8) fprintf(stderr,"kh: gfx pipeline cache: pipecache-save-refused: a save was attempted outside the suspend gate and outside the process exit (%s): NOTHING is written (counted; the file is only written while the game is parked or at exit)\n",why);
        return KH_PCS_REFUSED; }
    if(pthread_mutex_trylock(&ps)){ pthread_mutex_lock(&pm); cnt.save_busy++; pthread_mutex_unlock(&pm);
        fprintf(stderr,"kh: gfx pipeline cache: a save %s found another save still running: skipped (counted)\n",when); return KH_PCS_BUSY; }
    int res=KH_PCS_NOTHING;
    if(!p){ pthread_mutex_lock(&pm); uint64_t k=cnt.save_nothing++; pthread_mutex_unlock(&pm);
        if(!k) fprintf(stderr,"kh: gfx pipeline cache: a save %s, but no Vulkan device has registered a pipeline cache: nothing to save (counted)\n",when);
        goto out; }
    int d=p->dirty();
    pthread_mutex_lock(&pm); int have=disk_valid, keep=load_keep; PcFileId was=disk_id; pthread_mutex_unlock(&pm);
    if(d<0){ pthread_mutex_lock(&pm); uint64_t k=cnt.save_nothing++; pthread_mutex_unlock(&pm);
        if(!k) fprintf(stderr,"kh: gfx pipeline cache: a save %s, but no VkPipelineCache of a game device is alive: nothing to save (counted)\n",when);
        goto out; }
    if(have){                                                /* review C43 n=9: what this process remembers of the file counts only while the file is still that file */
        PcFileId now; int k=pc_file_id(c.path,&now);
        if(k!=1||!pc_id_same(&now,&was)){
            pthread_mutex_lock(&pm); disk_valid=0; if(k==0) cnt.disk_gone++; else cnt.disk_changed++; pthread_mutex_unlock(&pm); have=0;
            if(k==0) fprintf(stderr,"kh: gfx pipeline cache: %s: pipecache-file-gone: the file this run loaded or wrote is no longer there (removed while the game ran; the system may empty its caches directory): it is written again %s (counted)\n",c.path,when);
            else fprintf(stderr,"kh: gfx pipeline cache: %s: pipecache-file-changed: the file is not the one this run loaded or wrote (%s): it is written again %s (counted)\n",c.path,k<0?"it cannot be looked at":"another size, time or file",when); } }
    if(d==0&&!have&&keep){                                   /* review C43 n=9: the file could not be READ at the load and this run made no pipeline: an empty cache must not replace it */
        pthread_mutex_lock(&pm); cnt.save_kept++; uint64_t k=c_kept_said++; pthread_mutex_unlock(&pm); res=KH_PCS_KEPT;
        if(!k) fprintf(stderr,"kh: gfx pipeline cache: a save %s: %s could not be read or was refused at the load (see that line) and this run created no pipeline through the cache: the file is LEFT AS IT IS (counted)\n",when,c.path);
        goto out; }
    if(d==0&&have){ pthread_mutex_lock(&pm); cnt.save_unchanged_clean++; pthread_mutex_unlock(&pm); res=KH_PCS_UNCHANGED; goto out; }   /* no pipeline since the last load or save: the driver is not asked */
    if(deadline_ns&&kh_host_ns()+(uint64_t)KH_PC_SAVE_MARGIN_MS*1000000ull>=deadline_ns){
        pthread_mutex_lock(&pm); cnt.save_late++; pthread_mutex_unlock(&pm);
        fprintf(stderr,"kh: gfx pipeline cache: a save %s is skipped: less than %u ms of the caller's bound are left (counted; the next save point writes it)\n",when,KH_PC_SAVE_MARGIN_MS);
        res=KH_PCS_LATE; goto out; }
    { void *data=NULL; size_t len=0; KhPcDev dev; memset(&dev,0,sizeof dev); why[0]=0;
      uint64_t t0=kh_host_ns(); int fr=p->fetch(c.cap,deadline_ns,&data,&len,&dev,why,sizeof why); uint64_t t1=kh_host_ns();
      if(t1-t0>fetch_ns_max) fetch_ns_max=t1-t0;
      if(fr==KH_PCF_NONE){ pthread_mutex_lock(&pm); cnt.save_nothing++; pthread_mutex_unlock(&pm);
          fprintf(stderr,"kh: gfx pipeline cache: a save %s: the driver has nothing to write (%s) (counted)\n",when,why[0]?why:"an empty cache"); goto out; }
      if(fr==KH_PCF_TOO_LARGE){ pthread_mutex_lock(&pm); cnt.save_over_cap++; pthread_mutex_unlock(&pm); res=KH_PCS_OVER_CAP;
          fprintf(stderr,"kh: gfx pipeline cache: pipecache-over-cap: the driver's cache data is %zu bytes, above the cap of %llu bytes (KHNMC_GFX_PIPECACHE_MAX_MB): REFUSED, nothing is written, %s keeps what it had (counted)\n",len,P_(c.cap),c.path);
          goto out; }
      if(fr==KH_PCF_BUSY){ pthread_mutex_lock(&pm); cnt.save_busy++; pthread_mutex_unlock(&pm); res=KH_PCS_BUSY;
          fprintf(stderr,"kh: gfx pipeline cache: a save %s is skipped: %s (counted; the next save point writes it)\n",when,why); goto out; }
      if(fr!=KH_PCF_OK){ pthread_mutex_lock(&pm); cnt.save_failed++; pthread_mutex_unlock(&pm); res=KH_PCS_FAILED;
          fprintf(stderr,"kh: gfx pipeline cache: pipecache-save-failed: the driver's cache data could not be fetched %s: %s (counted; %s keeps what it had)\n",when,why,c.path); goto out; }
      if((uint64_t)len>c.cap){                               /* the fetch refuses above the cap without fetching; bytes that arrive above it all the same are not written either */
          pthread_mutex_lock(&pm); cnt.save_over_cap++; pthread_mutex_unlock(&pm); res=KH_PCS_OVER_CAP; free(data); p->done(0);
          fprintf(stderr,"kh: gfx pipeline cache: pipecache-over-cap: the driver's cache data is %zu bytes, above the cap of %llu bytes (KHNMC_GFX_PIPECACHE_MAX_MB): REFUSED, nothing is written, %s keeps what it had (counted)\n",len,P_(c.cap),c.path);
          goto out; }
      uint64_t sum=kh_pc_fnv(data,len);
      pthread_mutex_lock(&pm); int same=disk_valid&&disk_len==len&&disk_sum==sum&&!memcmp(&disk_dev,&dev,sizeof dev); pthread_mutex_unlock(&pm);
      if(same){ pthread_mutex_lock(&pm); cnt.save_unchanged_same++; pthread_mutex_unlock(&pm); res=KH_PCS_UNCHANGED; free(data); p->done(1);
          fprintf(stderr,"kh: gfx pipeline cache: a save %s: the driver's %zu bytes are the ones already in %s (checksum %016llx): not written again\n",when,len,c.path,P_(sum)); goto out; }
      if(deadline_ns&&kh_host_ns()+(uint64_t)KH_PC_SAVE_MARGIN_MS*1000000ull>=deadline_ns){        /* review C43 n=11: the fetch used up the caller's bound: the file is not written now */
          pthread_mutex_lock(&pm); cnt.save_late++; cnt.save_late_fetch++; pthread_mutex_unlock(&pm); res=KH_PCS_LATE; free(data); p->done(0);
          fprintf(stderr,"kh: gfx pipeline cache: a save %s is skipped after the driver's data was fetched (%.1f ms): less than %u ms of the caller's bound are left for the file (counted; the next save point writes it)\n",when,(t1-t0)/1e6,KH_PC_SAVE_MARGIN_MS);
          goto out; }
      uint64_t w0=kh_host_ns(); int wr=kh_pc_file_save(c.path,&dev,data,len,why,sizeof why); uint64_t w1=kh_host_ns(); free(data);
      if(w1-w0>save_ns_max) save_ns_max=w1-w0;
      if(wr){ pthread_mutex_lock(&pm); cnt.save_failed++; pthread_mutex_unlock(&pm); res=KH_PCS_FAILED; p->done(0);
          fprintf(stderr,"kh: gfx pipeline cache: pipecache-save-failed: %s (counted; the file itself is as it was)\n",why); goto out; }
      pthread_mutex_lock(&pm); cnt.save_written++; cnt.save_bytes+=len; disk_valid=1; disk_len=len; disk_sum=sum; disk_dev=dev; pc_file_id(c.path,&disk_id); load_keep=0; pthread_mutex_unlock(&pm);
      p->done(1); res=KH_PCS_WRITTEN;
      fprintf(stderr,"kh: gfx pipeline cache: saved %zu bytes to %s %s (payload checksum %016llx; the driver's data in %.1f ms, the file in %.1f ms: temporary file, fsync, rename)\n",
              len,c.path,when,P_(sum),(t1-t0)/1e6,(w1-w0)/1e6); }
out:
    pthread_mutex_unlock(&ps); return res;
}
void kh_gfx_pipecache_save_point(uint64_t deadline_ns){
    pthread_mutex_lock(&pm); c_points++; int on=pc.on; if(!on) c_points_off++; pthread_mutex_unlock(&pm);
    if(on) kh_gfx_pipecache_save(KH_PC_AT_PARKED,deadline_ns);
}
void kh_gfx_pipecache_exit(void){
    if(!kh_pc_active()) return;
    atomic_store(&pc_exiting,1); kh_gfx_pipecache_save(KH_PC_AT_EXIT,0);
}
void kh_pc_report(FILE *f){
    pthread_mutex_lock(&pm); PcConf c=pc_run; KhPcCounters n=cnt; uint64_t pts=c_points; pthread_mutex_unlock(&pm);
    if(!c.on) return;                                     /* nothing configured for this run: no line */
    fprintf(f,"kh: gfx pipeline cache: file %s, cap %llu MiB; loads: ok %llu (%llu bytes), no file %llu, rejected %llu (unreadable %llu, truncated %llu, garbage %llu, header %llu, version %llu, too large %llu, length %llu, checksum %llu, foreign device %llu, empty %llu, driver refused %llu, load unfinished %llu), probe devices given nothing %llu, load markers not written %llu; "
              "saves: written %llu (%llu bytes), unchanged %llu (no new pipeline %llu, same bytes %llu), REFUSED outside the gate %llu, over the cap %llu, nothing to save %llu, skipped busy %llu, skipped late %llu (of them after the fetch %llu), kept after an unread file %llu, FAILED %llu; "
              "the file found gone %llu, changed %llu (written again), directories made %llu; suspend save points %llu; slowest fetch %.1f ms, slowest file write %.1f ms\n",
            c.path,P_(c.cap>>20),P_(n.load_ok),P_(n.load_bytes),P_(n.load_missing),P_(n.load_rejected),P_(n.load_by[KH_PC_UNREADABLE]),P_(n.load_by[KH_PC_TRUNCATED]),P_(n.load_by[KH_PC_GARBAGE]),P_(n.load_by[KH_PC_HEADER_CORRUPT]),
            P_(n.load_by[KH_PC_VERSION_UNKNOWN]),P_(n.load_by[KH_PC_TOO_LARGE]),P_(n.load_by[KH_PC_LENGTH]),P_(n.load_by[KH_PC_CHECKSUM]),P_(n.load_by[KH_PC_FOREIGN_DEVICE]),P_(n.load_by[KH_PC_EMPTY]),P_(n.driver_refused),P_(n.load_by[KH_PC_LOAD_UNFINISHED]),P_(n.probes),P_(n.marker_failed),
            P_(n.save_written),P_(n.save_bytes),P_(n.save_unchanged_clean+n.save_unchanged_same),P_(n.save_unchanged_clean),P_(n.save_unchanged_same),P_(n.save_refused),P_(n.save_over_cap),P_(n.save_nothing),P_(n.save_busy),P_(n.save_late),P_(n.save_late_fetch),P_(n.save_kept),P_(n.save_failed),
            P_(n.disk_gone),P_(n.disk_changed),P_(n.dirs_made),P_(pts),fetch_ns_max/1e6,save_ns_max/1e6);
}

/* ---- selftest (no device): the file image, the files, and the save rule with a stand-in for the driver ---- */
static struct { int dirty, fetch_res, fetch_calls, done_calls, done_committed, ignore_cap, use_up_deadline; uint64_t last_deadline; uint8_t bytes[4096]; size_t len; KhPcDev dev; } sp;
static int sp_dirty(void){ return sp.dirty; }
static int sp_fetch(uint64_t cap,uint64_t deadline_ns,void **data,size_t *len,KhPcDev *dev,char *why,size_t whycap){
    sp.fetch_calls++; *dev=sp.dev; *len=sp.len; sp.last_deadline=deadline_ns;
    if(sp.use_up_deadline&&deadline_ns){                     /* a slow driver: it comes back when less than the margin of the caller's bound is left */
        uint64_t until=deadline_ns-(uint64_t)(KH_PC_SAVE_MARGIN_MS/2)*1000000ull;
        while(kh_host_ns()<until){ struct timespec ts={0,2000000}; nanosleep(&ts,0); } }
    if(sp.fetch_res!=KH_PCF_OK){ snprintf(why,whycap,"selftest stand-in"); return sp.fetch_res; }
    if(sp.len>cap&&!sp.ignore_cap) return KH_PCF_TOO_LARGE;
    *data=malloc(sp.len?sp.len:1); if(!*data) return KH_PCF_FAILED; memcpy(*data,sp.bytes,sp.len); return KH_PCF_OK; }
static void sp_done(int committed){ sp.done_calls++; sp.done_committed+=committed; if(committed) sp.dirty=0; }
static const KhPcProvider sp_prov={sp_dirty,sp_fetch,sp_done};
typedef struct { _Atomic int state, go, res; } PcAdm;
static void *pc_adm(void *a){ PcAdm *t=(PcAdm*)a; kh_gfx_gate_enter("selftest: pipecache render thread"); atomic_store(&t->state,2);
    while(atomic_load(&t->go)==0){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
    if(atomic_load(&t->go)==2){ atomic_store(&t->res,kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)); atomic_store(&t->state,3); while(atomic_load(&t->go)==2){ struct timespec ts={0,1000000}; nanosleep(&ts,0); } }
    kh_gfx_gate_leave(); atomic_store(&t->state,4); return NULL; }
static void *pc_render_save(void *a){ *(int*)a=kh_gfx_pipecache_save(KH_PC_AT_PARKED,0); return NULL; }
static int file_is(const char *path,const void *want,size_t n){ FILE *f=fopen(path,"rb"); if(!f) return 0; uint8_t b[8192]; size_t g=fread(b,1,sizeof b,f); fclose(f); return g==n&&!memcmp(b,want,n); }
static int file_put(const char *path,const void *b,size_t n){ FILE *f=fopen(path,"wb"); if(!f) return 0; int ok=fwrite(b,1,n,f)==n; return fclose(f)==0&&ok; }
static int exists(const char *p){ struct stat st; return !stat(p,&st); }
static int st_caches_calls; static const char *st_caches_dir="";
static const char *st_caches(void){ st_caches_calls++; return st_caches_dir; }
int kh_pc_selftest(void){
    int n=0,f=0;
#define TP(c) do{ n++; if(!(c)){ f++; fprintf(stderr,"gfx pipecache selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    KhPcDev A; memset(&A,0,sizeof A); for(int i=0;i<16;i++) A.uuid[i]=(uint8_t)(0xa0+i); A.vendor=0x10005; A.device=0; A.driver=0x06400004;
    uint8_t pay[300]; for(unsigned i=0;i<sizeof pay;i++) pay[i]=(uint8_t)(i*7+3);
    uint8_t img[KH_PC_HEADER_BYTES+sizeof pay+8]; kh_pc_header_make(img,&A,pay,sizeof pay); memcpy(img+KH_PC_HEADER_BYTES,pay,sizeof pay);
    size_t N=KH_PC_HEADER_BYTES+sizeof pay, off=9,len=9; char why[640]; uint64_t cap=1u<<20;
    /* 1. the image: a round trip, and every way it can be wrong, each by its own name */
    TP(kh_pc_parse(img,N,&A,cap,&off,&len,why,sizeof why)==KH_PC_OK && off==KH_PC_HEADER_BYTES && len==sizeof pay && !why[0]);
    TP(kh_pc_parse(img,0,&A,cap,&off,&len,why,sizeof why)==KH_PC_TRUNCATED && len==0);
    TP(kh_pc_parse(img,5,&A,cap,&off,&len,why,sizeof why)==KH_PC_TRUNCATED);
    TP(kh_pc_parse(img,KH_PC_HEADER_BYTES-1,&A,cap,&off,&len,why,sizeof why)==KH_PC_TRUNCATED);
    TP(kh_pc_parse(img,N-1,&A,cap,&off,&len,why,sizeof why)==KH_PC_TRUNCATED && strstr(why,"299")!=NULL);
    TP(kh_pc_parse(img,KH_PC_HEADER_BYTES,&A,cap,&off,&len,why,sizeof why)==KH_PC_TRUNCATED);
    { uint8_t g[400]; for(unsigned i=0;i<sizeof g;i++) g[i]=(uint8_t)(i*31+11); TP(kh_pc_parse(g,sizeof g,&A,cap,&off,&len,why,sizeof why)==KH_PC_GARBAGE); TP(kh_pc_parse(g,3,&A,cap,&off,&len,why,sizeof why)==KH_PC_GARBAGE);
      uint8_t z[200]; memset(z,0,sizeof z); TP(kh_pc_parse(z,sizeof z,&A,cap,&off,&len,why,sizeof why)==KH_PC_GARBAGE); }
    { uint8_t b[sizeof img]; memcpy(b,img,N); b[17]^=1; TP(kh_pc_parse(b,N,&A,cap,&off,&len,why,sizeof why)==KH_PC_HEADER_CORRUPT);     /* a flipped bit in the vendor field */
      memcpy(b,img,N); b[50]^=4; TP(kh_pc_parse(b,N,&A,cap,&off,&len,why,sizeof why)==KH_PC_HEADER_CORRUPT);                            /* in the payload length */
      memcpy(b,img,N); b[8]=2; put64(b+64,kh_pc_fnv(b,64)); TP(kh_pc_parse(b,N,&A,cap,&off,&len,why,sizeof why)==KH_PC_VERSION_UNKNOWN);  /* a later version, consistent in itself */
      memcpy(b,img,N); b[KH_PC_HEADER_BYTES+100]^=0x80; TP(kh_pc_parse(b,N,&A,cap,&off,&len,why,sizeof why)==KH_PC_CHECKSUM && len==0);
      memcpy(b,img,N); b[N]=0; TP(kh_pc_parse(b,N+1,&A,cap,&off,&len,why,sizeof why)==KH_PC_LENGTH); }
    TP(kh_pc_parse(img,N,&A,sizeof pay-1,&off,&len,why,sizeof why)==KH_PC_TOO_LARGE); TP(kh_pc_parse(img,N,&A,sizeof pay,&off,&len,why,sizeof why)==KH_PC_OK);
    { uint8_t e[KH_PC_HEADER_BYTES]; kh_pc_header_make(e,&A,pay,0); TP(kh_pc_parse(e,sizeof e,&A,cap,&off,&len,why,sizeof why)==KH_PC_EMPTY); }
    { KhPcDev B=A; B.uuid[15]^=1; TP(kh_pc_parse(img,N,&B,cap,&off,&len,why,sizeof why)==KH_PC_FOREIGN_DEVICE && strstr(why,"another device")!=NULL);
      B=A; B.vendor=0x106b; TP(kh_pc_parse(img,N,&B,cap,&off,&len,why,sizeof why)==KH_PC_FOREIGN_DEVICE);
      B=A; B.device=7; TP(kh_pc_parse(img,N,&B,cap,&off,&len,why,sizeof why)==KH_PC_FOREIGN_DEVICE);
      B=A; B.driver++; TP(kh_pc_parse(img,N,&B,cap,&off,&len,why,sizeof why)==KH_PC_FOREIGN_DEVICE && len==0); }
    TP(pc_cap_parse("64")==64ull<<20 && pc_cap_parse("1")==1ull<<20 && pc_cap_parse("1024")==1024ull<<20 && !pc_cap_parse("0") && !pc_cap_parse("1025") && !pc_cap_parse("8M") && !pc_cap_parse("") && !pc_cap_parse("-1") && !pc_cap_parse(NULL));
    for(int i=0;i<KH_PC_N;i++) TP(pc_names[i]!=NULL && (i==0||!strncmp(kh_pc_result_name(i),"pipecache-",10)));
    /* 2. the files */
    char dir[KH_TMP_MAX], path[KH_TMP_MAX+32], tmp[KH_TMP_MAX+40], mark[KH_TMP_MAX+48]; kh_tmp_path(dir,sizeof dir,"khnmc-pipecache-XXXXXX");
    if(!mkdtemp(dir)){ fprintf(stderr,"gfx pipecache selftest FAIL: mkdtemp(%s): %s\n",dir,strerror(errno)); return f+1; }
    snprintf(path,sizeof path,"%s/pipe.bin",dir); snprintf(tmp,sizeof tmp,"%s.tmp",path); snprintf(mark,sizeof mark,"%s.loading",path);
    uint8_t *file=NULL; const uint8_t *pl=NULL;
    TP(kh_pc_file_load(path,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_MISSING && !file);
    TP(kh_pc_file_load(dir,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_UNREADABLE && !file);
    TP(kh_pc_file_save(path,&A,pay,sizeof pay,why,sizeof why)==0 && !exists(tmp) && file_is(path,img,N));
    TP(kh_pc_file_load(path,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_OK && file && len==sizeof pay && !memcmp(pl,pay,sizeof pay)); free(file); file=NULL;
    TP(kh_pc_file_load(path,&A,100,&file,&pl,&len,why,sizeof why)==KH_PC_TOO_LARGE && !file && strstr(why,"not read")!=NULL);
    /* review C43 n=6 / n=9: a directory that is not there is made (with its parents), said and counted; one that cannot be made is the named failure */
    { char deep[KH_TMP_MAX+48], d1[KH_TMP_MAX+32], d2[KH_TMP_MAX+40], under[KH_TMP_MAX+48]; snprintf(d1,sizeof d1,"%s/none",dir); snprintf(d2,sizeof d2,"%s/deep",d1); snprintf(deep,sizeof deep,"%s/pipe.bin",d2);
      KhPcCounters k0,k1; kh_pc_counters(&k0); TP(!exists(d1) && kh_pc_file_save(deep,&A,pay,sizeof pay,why,sizeof why)==0 && file_is(deep,img,N)); kh_pc_counters(&k1); TP(k1.dirs_made==k0.dirs_made+1);
      TP(kh_pc_file_save(deep,&A,pay,sizeof pay,why,sizeof why)==0); kh_pc_counters(&k1); TP(k1.dirs_made==k0.dirs_made+1);       /* it is there now: nothing made, nothing counted */
      TP(unlink(deep)==0 && rmdir(d2)==0 && rmdir(d1)==0);
      snprintf(under,sizeof under,"%s/x/pipe.bin",path);         /* under a regular file: no directory can be made there */
      TP(file_is(path,img,N) && kh_pc_file_save(under,&A,pay,sizeof pay,why,sizeof why)==-1 && strstr(why,"cannot create the temporary file")!=NULL && file_is(path,img,N)); }
    { TP(mkdir(tmp,0755)==0);                                  /* the temporary name is taken by a directory: the save fails by name and the file is as it was */
      uint8_t p2[10]={1,2,3,4,5,6,7,8,9,10}; TP(kh_pc_file_save(path,&A,p2,sizeof p2,why,sizeof why)==-1 && file_is(path,img,N)); TP(rmdir(tmp)==0); }
    TP(file_put(path,img,N-40) && kh_pc_file_load(path,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_TRUNCATED && !file);
    { uint8_t g[500]; for(unsigned i=0;i<sizeof g;i++) g[i]=(uint8_t)(i*13+5); TP(file_put(path,g,sizeof g) && kh_pc_file_load(path,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_GARBAGE && !file); }
    TP(file_put(path,img,0) && kh_pc_file_load(path,&A,cap,&file,&pl,&len,why,sizeof why)==KH_PC_TRUNCATED);
    /* KHNMC_GFX_PIPECACHE_RESET: the file and a leftover temporary file go; nothing there is not an error; what cannot be removed is said */
    TP(file_put(tmp,img,10) && file_put(mark,img,1) && pc_reset(path,why,sizeof why)==1 && !exists(path) && !exists(tmp) && !exists(mark)); TP(pc_reset(path,why,sizeof why)==0);
    TP(mkdir(path,0755)==0 && pc_reset(path,why,sizeof why)==-1 && why[0] && rmdir(path)==0);
    unlink(path);
    /* 3. the rules, with a stand-in for the driver.  The run's own configuration, counters and provider are put back at the end */
    pthread_mutex_lock(&pm); KhPcCounters cnt0=cnt; const KhPcProvider *prov0=prov; int dv0=disk_valid; size_t dl0=disk_len; uint64_t ds0=disk_sum, pts0=c_points, pto0=c_points_off, fm0=fetch_ns_max, sm0=save_ns_max, ks0=c_kept_said; KhPcDev dd0=disk_dev; PcFileId di0=disk_id; int lk0=load_keep; pthread_mutex_unlock(&pm);
    int ex0=atomic_load(&pc_exiting); atomic_store(&pc_exiting,0);
    int gate0=kh_gfx_gate_is_set(); TP(!gate0); if(gate0) kh_gfx_gate_clear();
    kh_pc_test_config(path,1000); kh_pc_set_provider(&sp_prov); memset(&sp,0,sizeof sp); sp.dev=A; sp.len=sizeof pay; memcpy(sp.bytes,pay,sizeof pay); sp.dirty=1;
    KhPcCounters c0,c1;
    /* 3a. THE SAVE ONLY WHILE PARKED OR AT EXIT.  A render thread (any thread, the gate clear) that tries to save: refused, nothing written */
    { int r=-1; pthread_t th; kh_pc_counters(&c0); TP(pthread_create(&th,NULL,pc_render_save,&r)==0); pthread_join(th,NULL); kh_pc_counters(&c1);
      TP(r==KH_PCS_REFUSED && !exists(path) && !exists(tmp) && c1.save_refused==c0.save_refused+1 && sp.fetch_calls==0); }
    /*     the gate set but a thread still inside a submission (admitted): refused for another thread AND for that thread itself */
    { PcAdm a; memset(&a,0,sizeof a); pthread_t th; TP(pthread_create(&th,NULL,pc_adm,&a)==0); for(int i=0;i<5000&&atomic_load(&a.state)<2;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      kh_gfx_gate_set(); kh_pc_counters(&c0);
      TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_REFUSED && !exists(path));
      atomic_store(&a.go,2); for(int i=0;i<5000&&atomic_load(&a.state)<3;i++){ struct timespec ts={0,1000000}; nanosleep(&ts,0); }
      kh_pc_counters(&c1); TP(atomic_load(&a.res)==KH_PCS_REFUSED && !exists(path) && c1.save_refused==c0.save_refused+2 && sp.fetch_calls==0);
      atomic_store(&a.go,1); pthread_join(th,NULL);
      /* 3b. parked: the gate set, nobody admitted: written through the temporary file; the file is ours and holds the driver's bytes */
      TP(kh_gfx_gate_admitted()==0); kh_pc_counters(&c0);
      TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,img,N) && !exists(tmp) && sp.fetch_calls==1 && sp.done_calls==1 && sp.done_committed==1);
      kh_pc_counters(&c1); TP(c1.save_written==c0.save_written+1 && c1.save_bytes==c0.save_bytes+sizeof pay);
      /* 3c. no pipeline created since: the driver is not asked, nothing is written */
      struct stat s0,s1; TP(!stat(path,&s0)); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED && sp.fetch_calls==1); TP(!stat(path,&s1) && s0.st_ino==s1.st_ino);
      /*     pipelines created, but the driver's bytes are the ones on disk: fetched, not written again (the same inode) */
      sp.dirty=1; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED && sp.fetch_calls==2 && sp.done_calls==2 && sp.done_committed==2 && sp.dirty==0); TP(!stat(path,&s1) && s0.st_ino==s1.st_ino);
      kh_pc_counters(&c1); TP(c1.save_unchanged_clean==c0.save_unchanged_clean+1 && c1.save_unchanged_same==c0.save_unchanged_same+1);
      /*     new bytes: the file is replaced */
      sp.dirty=1; sp.bytes[10]^=0xff; uint8_t img2[sizeof img]; kh_pc_header_make(img2,&A,sp.bytes,sp.len); memcpy(img2+KH_PC_HEADER_BYTES,sp.bytes,sp.len);
      TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,img2,N) && !exists(tmp));
      /* 3d. THE CAP: data above it is refused, said so, and the file keeps what it had */
      sp.dirty=1; sp.len=1001; kh_pc_counters(&c0); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_OVER_CAP && file_is(path,img2,N) && !exists(tmp)); kh_pc_counters(&c1); TP(c1.save_over_cap==c0.save_over_cap+1 && sp.dirty==1);
      /*     a driver side that hands over bytes above the cap all the same: still refused, the exclusion ended without a commit */
      { int dc=sp.done_calls, dm=sp.done_committed; sp.ignore_cap=1; kh_pc_counters(&c0); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_OVER_CAP && file_is(path,img2,N) && !exists(tmp));
        kh_pc_counters(&c1); TP(c1.save_over_cap==c0.save_over_cap+1 && sp.done_calls==dc+1 && sp.done_committed==dm && sp.dirty==1); sp.ignore_cap=0; }
      sp.len=1000; { uint8_t big[KH_PC_HEADER_BYTES+1000]; kh_pc_header_make(big,&A,sp.bytes,1000); memcpy(big+KH_PC_HEADER_BYTES,sp.bytes,1000);
        TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,big,sizeof big)); }           /* exactly the cap is allowed */
      sp.len=sizeof pay;
      /* 3e. the driver's side says busy / failed / nothing: named results, the file is as it was, the exclusion is not left held */
      { struct stat t0,t1; TP(!stat(path,&t0)); int dc=sp.done_calls;
        sp.dirty=1; sp.fetch_res=KH_PCF_BUSY; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_BUSY);
        sp.fetch_res=KH_PCF_FAILED; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_FAILED);
        sp.fetch_res=KH_PCF_NONE; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_NOTHING);
        sp.dirty=-1; sp.fetch_res=KH_PCF_OK; int fc=sp.fetch_calls; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_NOTHING && sp.fetch_calls==fc);
        TP(!stat(path,&t1) && t0.st_ino==t1.st_ino && sp.done_calls==dc); }
      /*     a write that fails (the temporary name is a directory): FAILED by name, done(0), the file as it was, still dirty */
      { struct stat t0,t1; TP(!stat(path,&t0)); TP(mkdir(tmp,0755)==0); sp.dirty=1; sp.bytes[20]^=0x55; int dc=sp.done_calls, dm=sp.done_committed;
        TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_FAILED && sp.done_calls==dc+1 && sp.done_committed==dm && sp.dirty==1); TP(!stat(path,&t1) && t0.st_ino==t1.st_ino); TP(rmdir(tmp)==0); }
      /* 3f. a suspend with too little of its caller's bound left does not start the save */
      { int fc=sp.fetch_calls; kh_pc_counters(&c0); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,kh_host_ns()+10000000ull)==KH_PCS_LATE && sp.fetch_calls==fc); kh_pc_counters(&c1); TP(c1.save_late==c0.save_late+1);
        { uint64_t dl=kh_host_ns()+60000000000ull; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,dl)==KH_PCS_WRITTEN && sp.last_deadline==dl); }   /* the driver's side is told the bound */
        /* review C43 n=11: a save that STARTS in time and whose fetch uses the bound up does not write the file after it: skipped late, the
         * exclusion ended without a commit, the file as it was, still dirty; without a bound the same slow fetch is written */
        { struct stat t0,t1; TP(!stat(path,&t0)); int dc=sp.done_calls, dm=sp.done_committed; fc=sp.fetch_calls; sp.dirty=1; sp.bytes[40]^=0x21; sp.use_up_deadline=1; kh_pc_counters(&c0);
          uint64_t dl=kh_host_ns()+400000000ull; int r=kh_gfx_pipecache_save(KH_PC_AT_PARKED,dl); uint64_t end=kh_host_ns(); kh_pc_counters(&c1);
          TP(r==KH_PCS_LATE && sp.fetch_calls==fc+1 && sp.last_deadline==dl && c1.save_late==c0.save_late+1 && c1.save_late_fetch==c0.save_late_fetch+1 && c1.save_written==c0.save_written);
          TP(sp.done_calls==dc+1 && sp.done_committed==dm && sp.dirty==1 && !stat(path,&t1) && t0.st_ino==t1.st_ino && !exists(tmp)); TP(end<dl+2000000000ull);
          TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && sp.last_deadline==0); sp.use_up_deadline=0; } }
      /* 3j. review C43 n=9 (i): THE FILE IS GONE UNDER US (the system emptied its caches directory, or rm): what the process remembers
       *     does not count; the file is written again, by name, with no new pipeline and with the same bytes; a file that is ANOTHER
       *     file now (size, time, inode) likewise; the directory gone too: made again */
      { uint8_t cur[KH_PC_HEADER_BYTES+sizeof pay]; kh_pc_header_make(cur,&A,sp.bytes,sp.len); memcpy(cur+KH_PC_HEADER_BYTES,sp.bytes,sp.len);
        TP(file_is(path,cur,N) && sp.dirty==0 && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED);         /* the state before: clean, the file there */
        kh_pc_counters(&c0); TP(unlink(path)==0 && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,cur,N)); kh_pc_counters(&c1); TP(c1.disk_gone==c0.disk_gone+1 && c1.save_unchanged_clean==c0.save_unchanged_clean);
        sp.dirty=1; TP(unlink(path)==0 && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,cur,N)); kh_pc_counters(&c1); TP(c1.disk_gone==c0.disk_gone+2 && c1.save_unchanged_same==c0.save_unchanged_same);
        TP(file_put(path,img,N-5) && sp.dirty==0 && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,cur,N)); kh_pc_counters(&c1); TP(c1.disk_changed==c0.disk_changed+1);
        TP(unlink(path)==0 && rmdir(dir)==0 && kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,cur,N)); kh_pc_counters(&c1); TP(c1.dirs_made==c0.dirs_made+1 && c1.disk_gone==c0.disk_gone+3);
        TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_UNCHANGED);
        /* 3k. review C43 n=9 (ii): a load that could not READ the file says nothing against its content: a run that made no pipeline does
         *     not replace it with an empty cache (KEPT, counted); a run that made pipelines does; the same after a driver's refusal; a
         *     file that was read and found WRONG is replaced as before */
        uint8_t *lf=NULL; const uint8_t *lp=NULL; size_t ll=0; int fc=sp.fetch_calls;
        kh_pc_test_config(path,1000); pthread_mutex_lock(&pm); pc_test_load_fault=KH_PC_UNREADABLE; pthread_mutex_unlock(&pm); kh_pc_counters(&c0);
        TP(kh_pc_load(&A,&lf,&lp,&ll)==KH_PC_UNREADABLE && !lf && file_is(path,cur,N) && !exists(mark));
        sp.dirty=0; sp.bytes[50]^=0x33; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_KEPT && file_is(path,cur,N) && sp.fetch_calls==fc); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_KEPT);
        kh_pc_counters(&c1); TP(c1.save_kept==c0.save_kept+2 && c1.save_written==c0.save_written && c1.load_by[KH_PC_UNREADABLE]==c0.load_by[KH_PC_UNREADABLE]+1);
        sp.dirty=1; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && !file_is(path,cur,N)); kh_pc_header_make(cur,&A,sp.bytes,sp.len); memcpy(cur+KH_PC_HEADER_BYTES,sp.bytes,sp.len); TP(file_is(path,cur,N));
        kh_pc_test_config(path,1000); TP(kh_pc_load(&A,&lf,&lp,&ll)==KH_PC_OK && lf && exists(mark)); free(lf); lf=NULL; kh_pc_note_driver_refused(ll,-3); TP(!exists(mark));
        sp.dirty=0; sp.bytes[51]^=0x44; kh_pc_counters(&c0); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_KEPT && file_is(path,cur,N)); kh_pc_counters(&c1); TP(c1.save_kept==c0.save_kept+1);
        kh_pc_test_config(path,1000); TP(file_put(path,img,N-9) && kh_pc_load(&A,&lf,&lp,&ll)==KH_PC_TRUNCATED && !lf);
        kh_pc_header_make(cur,&A,sp.bytes,sp.len); memcpy(cur+KH_PC_HEADER_BYTES,sp.bytes,sp.len); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && file_is(path,cur,N)); }
      kh_gfx_gate_clear(); }
    /* 3g. at exit: only while the process is ending (then also with the gate clear) */
    { sp.dirty=1; sp.bytes[30]^=0x11; kh_pc_counters(&c0); TP(kh_gfx_pipecache_save(KH_PC_AT_EXIT,0)==KH_PCS_REFUSED); kh_pc_counters(&c1); TP(c1.save_refused==c0.save_refused+1);
      kh_pc_test_exiting(1); TP(kh_gfx_pipecache_save(KH_PC_AT_EXIT,0)==KH_PCS_WRITTEN); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_REFUSED); kh_pc_test_exiting(0);
      TP(kh_gfx_pipecache_save(7,0)==KH_PCS_REFUSED); }
    /* 3h. the load side through the configuration: ours; another device's; truncated; garbage: each by name, counted, no bytes handed over */
    { kh_pc_counters(&c0); TP(kh_pc_load(&A,&file,&pl,&len)==KH_PC_OK && file && len==sizeof pay && !memcmp(pl,sp.bytes,len));
      /* review C43 n=12, THE LOAD GUARD: while the bytes are with the driver the marker is there; it goes when the driver's call is back */
      TP(exists(mark)); kh_pc_note_loaded(len,kh_pc_fnv(pl,len),&A,0); TP(!exists(mark)); free(file); file=NULL;
      /*   a marker found at a load: the last load never came back.  Named, counted, the file REMOVED, nothing handed over; then a first run */
      { KhPcCounters g0,g1; kh_pc_counters(&g0); TP(file_put(mark,pay,1) && exists(path) && kh_pc_load(&A,&file,&pl,&len)==KH_PC_LOAD_UNFINISHED && !file && !len && !exists(path) && !exists(mark));
        kh_pc_counters(&g1); TP(g1.load_by[KH_PC_LOAD_UNFINISHED]==g0.load_by[KH_PC_LOAD_UNFINISHED]+1 && g1.load_rejected==g0.load_rejected+1 && g1.load_ok==g0.load_ok);
        TP(kh_pc_load(&A,&file,&pl,&len)==KH_PC_MISSING);
        TP(file_put(mark,pay,1) && kh_pc_load(&A,&file,&pl,&len)==KH_PC_MISSING && !exists(mark));                 /* a marker without its file: removed, a first run */
        kh_pc_counters(&g1); TP(g1.load_by[KH_PC_LOAD_UNFINISHED]==g0.load_by[KH_PC_LOAD_UNFINISHED]+1 && g1.load_missing==g0.load_missing+2);
        kh_gfx_gate_set(); sp.dirty=1; TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_WRITTEN && exists(path)); kh_gfx_gate_clear(); }   /* the next save writes a new file */
      KhPcDev B=A; B.uuid[0]^=0xff; TP(kh_pc_load(&B,&file,&pl,&len)==KH_PC_FOREIGN_DEVICE && !file && !len);
      { FILE *fp=fopen(path,"rb"); uint8_t b[2048]; size_t g=fp?fread(b,1,sizeof b,fp):0; if(fp) fclose(fp); TP(g>100 && file_put(path,b,g-17)); }
      TP(kh_pc_load(&A,&file,&pl,&len)==KH_PC_TRUNCATED && !file);
      { uint8_t g[333]; for(unsigned i=0;i<sizeof g;i++) g[i]=(uint8_t)(255-i); TP(file_put(path,g,sizeof g)); } TP(kh_pc_load(&A,&file,&pl,&len)==KH_PC_GARBAGE && !file);
      unlink(path); TP(kh_pc_load(&A,&file,&pl,&len)==KH_PC_MISSING);
      kh_pc_counters(&c1); TP(c1.load_by[KH_PC_OK]==c0.load_by[KH_PC_OK]+1 && c1.load_by[KH_PC_FOREIGN_DEVICE]==c0.load_by[KH_PC_FOREIGN_DEVICE]+1 && c1.load_by[KH_PC_TRUNCATED]==c0.load_by[KH_PC_TRUNCATED]+1
         && c1.load_by[KH_PC_GARBAGE]==c0.load_by[KH_PC_GARBAGE]+1 && c1.load_missing==c0.load_missing+3 && c1.load_rejected==c0.load_rejected+4 && c1.load_by[KH_PC_LOAD_UNFINISHED]==c0.load_by[KH_PC_LOAD_UNFINISHED]+1);
      /* a driver that refuses bytes our checks passed: counted as rejected by its own name */
      kh_pc_counters(&c0); kh_pc_note_driver_refused(123,-3); kh_pc_counters(&c1); TP(c1.driver_refused==c0.driver_refused+1 && c1.load_by[KH_PC_DRIVER_REFUSED]==c0.load_by[KH_PC_DRIVER_REFUSED]+1 && c1.load_rejected==c0.load_rejected+1); }
    /* 3i. nothing configured: every save point does nothing (the state a run with nothing set is in) */
    { PcConf keep=pc_run; pthread_mutex_lock(&pm); memset(&pc_run,0,sizeof pc_run); pthread_mutex_unlock(&pm); kh_pc_test_config(NULL,0);
      int fc=sp.fetch_calls; sp.dirty=1; kh_gfx_gate_set(); TP(kh_gfx_pipecache_save(KH_PC_AT_PARKED,0)==KH_PCS_OFF); kh_gfx_pipecache_save_point(0); kh_gfx_gate_clear();
      kh_gfx_pipecache_exit(); TP(sp.fetch_calls==fc && !exists(path) && !atomic_load(&pc_exiting) && !kh_pc_active());
      pthread_mutex_lock(&pm); pc_run=keep; pthread_mutex_unlock(&pm); }
    /* 3l. review C43 n=3: WHICH file a run uses (kh_pc_init acts on this decision) */
    { PcConf d; char lng[sizeof d.path+8]; memset(lng,'p',sizeof lng-1); lng[sizeof lng-1]=0; st_caches_calls=0; st_caches_dir="/app/Library/Caches/khnmc";
      TP(pc_decide(NULL,st_caches,1,0,&d)==PCI_ON && d.on && !strcmp(d.path,"/app/Library/Caches/khnmc/pipe.bin") && strstr(d.src,"default") && st_caches_calls==1);   /* the app: on by default */
      TP(pc_decide("",st_caches,1,0,&d)==PCI_ON && !strcmp(d.path,"/app/Library/Caches/khnmc/pipe.bin"));                                 /* empty = unset */
      st_caches_calls=0; TP(pc_decide(NULL,st_caches,0,0,&d)==PCI_NONE && !d.on && !d.path[0] && st_caches_calls==0);                       /* no backend: nothing, the directory is not even asked for */
      TP(pc_decide("off",st_caches,1,0,&d)==PCI_OFF && !d.on && !d.path[0] && st_caches_calls==0); TP(pc_decide("off",st_caches,0,0,&d)==PCI_NONE && !d.on);
      TP(pc_decide("/x/y.bin",st_caches,1,0,&d)==PCI_ON && d.on && !strcmp(d.path,"/x/y.bin") && !strcmp(d.src,"KHNMC_GFX_PIPECACHE") && st_caches_calls==0);   /* a named file wins over the default */
      TP(pc_decide("/x/y.bin",st_caches,0,0,&d)==PCI_NO_BACKEND && !d.on && !strcmp(d.path,"/x/y.bin"));
      TP(pc_decide("/x/y.bin",st_caches,1,1,&d)==PCI_SELFTEST && !d.on); TP(pc_decide(NULL,st_caches,1,1,&d)==PCI_SELFTEST && !d.on);         /* a --selftest run never uses the configured file */
      TP(pc_decide(lng,st_caches,1,0,&d)==PCI_PATH_LONG && !d.on && !d.path[0]);
      st_caches_dir=""; st_caches_calls=0; TP(pc_decide(NULL,st_caches,1,0,&d)==PCI_NONE && !d.on && st_caches_calls==1);                   /* not an app container: unset = no cache */
      st_caches_dir=lng; TP(pc_decide(NULL,st_caches,1,0,&d)==PCI_PATH_LONG && !d.on); st_caches_dir=""; }
    unlink(path); unlink(mark); TP(!exists(tmp)); TP(rmdir(dir)==0);
    kh_pc_test_config(NULL,0); atomic_store(&pc_exiting,ex0);
    pthread_mutex_lock(&pm); cnt=cnt0; prov=prov0; disk_valid=dv0; disk_len=dl0; disk_sum=ds0; disk_dev=dd0; disk_id=di0; load_keep=lk0; c_kept_said=ks0; pc_test_load_fault=0; c_points=pts0; c_points_off=pto0; fetch_ns_max=fm0; save_ns_max=sm0; pthread_mutex_unlock(&pm);
    TP(!kh_gfx_gate_is_set() && kh_gfx_gate_admitted()==0);
    fprintf(stderr,"kh: gfx pipecache selftest: %d checks, %d failures (the file image and each named rejection: truncated, garbage, header, version, length, checksum, too large, another device; the temporary file and rename; a missing directory made; the save only while parked at the gate or at exit, refused from a render thread; the cap; unchanged data not rewritten; busy / failed / late, also after the fetch; the file gone or changed under us; an unread file kept; the load guard; which file a run uses)\n",n,f);
#undef TP
    return f;
}
