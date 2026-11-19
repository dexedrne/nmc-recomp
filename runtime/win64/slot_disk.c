#include "slot_disk.h"
#include "slot_hash.h"
#include "settings.h"

extern const char *kh_game_image_path(void);
#include <errno.h>
#include <dirent.h>
#include <fts.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <crt_externs.h>
#else
extern char **environ;
#endif

static int file_hash(const char *path,u8 out[32]){
    FILE *f=fopen(path,"rb");
    if(!f) return 0;
    SlotHash hash; slot_hash_init(&hash);
    u8 buf[65536]; size_t n;
    while((n=fread(buf,1,sizeof buf,f))) slot_hash_add(&hash,buf,n);
    int ok=!ferror(f);
    fclose(f); slot_hash_end(out,&hash);
    return ok;
}

static int order(const FTSENT **a,const FTSENT **b){ return strcmp((*a)->fts_name,(*b)->fts_name); }

/* Files in the loose tree are immutable while playing. Include inode change
 * times so a same-length edit with a preserved mtime invalidates the slot. */
static int tree_hash(SlotHash *hash,const char *path){
    if(!path||!*path){ slot_hash_add(hash,"absent",7); return 1; }
    char *paths[]={(char *)path,NULL};
    FTS *tree=fts_open(paths,FTS_LOGICAL|FTS_NOCHDIR,order);
    if(!tree) return 0;
    size_t root=0;
    int ok=1;
    while(ok){
        errno=0;
        FTSENT *e=fts_read(tree);
        if(!e){ ok=!errno; break; }
        if(e->fts_info==FTS_DP) continue;
        if(e->fts_info!=FTS_F && e->fts_info!=FTS_D){ ok=0; break; }
        if(!e->fts_level) root=e->fts_pathlen;
        const char *name=e->fts_path+root;
        slot_hash_add(hash,name,strlen(name)+1);
        struct stat *s=e->fts_statp;
#ifdef __APPLE__
        struct timespec changed=s->st_ctimespec;
#else
        struct timespec changed=s->st_ctim;
#endif
        u64 stamp[]={s->st_dev,s->st_ino,s->st_mode,s->st_size,changed.tv_sec,changed.tv_nsec};
        slot_hash_add(hash,stamp,sizeof stamp);
    }
    return fts_close(tree)==0 && ok;
}
static int string_order(const void *a,const void *b){
    return strcmp(*(const char *const *)a,*(const char *const *)b);
}
static int profile_var(const char *s){
    if(strncmp(s,"KHNMC_",6)&&strncmp(s,"KH_",3)) return 0;
    const char *ignore[]={"KHNMC_BOOT_SLOT=","KH_EXE=","KH_ROOT=","KH_DOCS=","KH_RUN_","KH_DUMP_","KH_TMPDIR=",
        "KHNMC_TEST_SAVESTATE=","KHNMC_TEST_SETTINGS=","KHNMC_PHASELOG=","KHNMC_FILELOG=",
        "KHNMC_DATA=","KHNMC_PKGDIR=","KHNMC_INPUT_SCRIPT=","KHNMC_GFX_HASHLOG=",
        "KHNMC_GFX_DUMP","KHNMC_GFX_PIPECACHE="};
    for(unsigned i=0;i<sizeof ignore/sizeof *ignore;i++) if(!strncmp(s,ignore[i],strlen(ignore[i]))) return 0;
    return 1;
}
const char *kh_slot_identity(KhSlotHeader *h){
    static u8 build[32];
    static int have_build;
    if(!have_build){
#ifdef __APPLE__
        char path[4096]; uint32_t cap=sizeof path;
        if(_NSGetExecutablePath(path,&cap)) return "savestate-build-path";
#else
        const char *path="/proc/self/exe";
#endif
        if(!file_hash(path,build)) return "savestate-build-read";
        have_build=1;
    }
    memcpy(h->build,build,sizeof build);
    if(!file_hash(kh_game_image_path(),h->game)) return "savestate-game-read";
    SlotHash hash; slot_hash_init(&hash);
    const char *data=getenv("KHNMC_DATA");
    if(!tree_hash(&hash,data?data:"data")||!tree_hash(&hash,getenv("KHNMC_PKGDIR"))) return "savestate-data-tree-read";
    slot_hash_end(h->data,&hash);
#ifdef __APPLE__
    char **env=*_NSGetEnviron();
#else
    char **env=environ;
#endif
    size_t n=0;
    for(char **p=env;*p;p++) n+=profile_var(*p);
    char **vars=calloc(n?n:1,sizeof *vars);
    if(!vars) return "savestate-profile-memory";
    n=0;
    for(char **p=env;*p;p++) if(profile_var(*p)) vars[n++]=*p;
    qsort(vars,n,sizeof *vars,string_order);
    slot_hash_init(&hash);
    for(size_t i=0;i<n;i++) slot_hash_add(&hash,vars[i],strlen(vars[i])+1);
    free(vars);
    for(int i=0;kh_setting_live(i);i++){
        double value=kh_live_setting_value(i);
        slot_hash_add(&hash,&value,sizeof value);
    }
    const char *script=getenv("KHNMC_INPUT_SCRIPT");
    if(script&&*script){
        u8 digest[32];
        if(!file_hash(script,digest)) return "savestate-input-script-read";
        slot_hash_add(&hash,digest,sizeof digest);
    }
    slot_hash_end(h->settings,&hash);
    return NULL;
}
