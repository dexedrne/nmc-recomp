/* Native title code reopens its profile by Windows path during area changes.
 * Give it a private copy for the lifetime of a Theater cut. */
#include "slot_disk.h"
#include <errno.h>
#include <fts.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static char overlay[KH_TMP_MAX];
static _Atomic int enabled;

const char *kh_theater_overlay(void){ return atomic_load(&enabled)?overlay:NULL; }
static pthread_mutex_t visit_mutex=PTHREAD_MUTEX_INITIALIZER;
static int visit_fd=-1;
static char visits[KH_TMP_MAX];

static int remove_visit(char *path){
    char *paths[]={path,NULL};
    FTS *tree=fts_open(paths,FTS_PHYSICAL|FTS_NOCHDIR,NULL);
    if(!tree) return 0;
    int ok=1;
    FTSENT *e;
    for(;;){
        errno=0; e=fts_read(tree);
        if(!e){ if(errno) ok=0; break; }
        if(e->fts_info==FTS_D) continue;
        if((e->fts_info==FTS_DP?rmdir(e->fts_path):unlink(e->fts_path))!=0)
            ok=0;
    }
    return !fts_close(tree) && ok;
}
static DIR *lock_visits(void);
void kh_theater_docs_end(void){
    pthread_mutex_lock(&visit_mutex);
    atomic_store(&enabled,0);
    if(visit_fd>=0){
        DIR *dir=lock_visits();
        if(!dir) kh_fatal(NULL,"theater-documents-registry-lock");
        if(!remove_visit(overlay)) kh_fatal(NULL,"theater-documents-cleanup");
        close(visit_fd); visit_fd=-1; *overlay=0;
        closedir(dir);
    }
    pthread_mutex_unlock(&visit_mutex);
}
/* Serialize discovery and publication. Each live visit holds its directory
 * lock until removal. The kernel drops the lock even after an abrupt exit. */
static DIR *lock_visits(void){
    int fd=open(visits,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    struct stat st;
    if(fd<0 || fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077) ||
       flock(fd,LOCK_EX)){ if(fd>=0) close(fd); return NULL; }
    DIR *dir=fdopendir(fd);
    if(!dir) close(fd);
    return dir;
}
int kh_theater_docs_init(int create){
    char leaf[64];
    snprintf(leaf,sizeof leaf,"khnmc-theater-%lu",(unsigned long)geteuid());
    kh_tmp_path(visits,sizeof visits,leaf);
    if(create){ if(mkdir(visits,0700)&&errno!=EEXIST){ kh_warn("Theater disabled: cannot create private directory"); return 0; } }
    else if(access(visits,F_OK)&&errno==ENOENT) return 1;
    DIR *dir=lock_visits();
    if(!dir){ kh_warn("Theater disabled: private directory unavailable"); return 0; }
    int ok=1;
    for(;;){
        errno=0;
        struct dirent *e=readdir(dir);
        if(!e){ if(errno) ok=0; break; }
        if(strncmp(e->d_name,"visit-",6) || strlen(e->d_name)!=12) continue;
        int fd=openat(dirfd(dir),e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(fd<0){ if(errno==ENOENT) continue; ok=0; break; }
        if(!flock(fd,LOCK_EX|LOCK_NB)){
            char path[KH_TMP_MAX];
            if(snprintf(path,sizeof path,"%s/%s",visits,e->d_name)>=(int)sizeof path || !remove_visit(path)) ok=0;
        }else if(errno!=EWOULDBLOCK) ok=0;
        close(fd);
    }
    closedir(dir);
    if(!ok) kh_warn("Theater disabled: stale visit directory unavailable");
    return ok;
}

void kh_theater_docs_begin(void){
    pthread_mutex_lock(&visit_mutex);
    if(*overlay) kh_fatal(NULL,"theater-documents-busy");
    DIR *dir=lock_visits();
    if(!dir) kh_fatal(NULL,"theater-documents-registry-lock");
    if(snprintf(overlay,sizeof overlay,"%s/visit-XXXXXX",visits)>=(int)sizeof overlay) kh_fatal(NULL,"theater-documents-path");
    if(!mkdtemp(overlay)) kh_fatal(NULL,"theater-documents-create: %s",strerror(errno));
    visit_fd=open(overlay,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(visit_fd<0||flock(visit_fd,LOCK_EX)) kh_fatal(NULL,"theater-documents-visit-lock");
    closedir(dir);
    const char *err=kh_slot_docs_snapshot(overlay);
    if(err) kh_fatal(NULL,"theater-documents-snapshot: %s",err);
    atomic_store(&enabled,1);
    pthread_mutex_unlock(&visit_mutex);
}
