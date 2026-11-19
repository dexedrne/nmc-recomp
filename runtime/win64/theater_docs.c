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

static void remove_visit(char *path){
    char *paths[]={path,NULL};
    FTS *tree=fts_open(paths,FTS_PHYSICAL|FTS_NOCHDIR,NULL);
    if(!tree) kh_fatal(NULL,"theater-documents-cleanup-open");
    FTSENT *e;
    for(;;){
        errno=0; e=fts_read(tree);
        if(!e){ if(errno) kh_fatal(NULL,"theater-documents-cleanup-read"); break; }
        if(e->fts_info==FTS_D) continue;
        if((e->fts_info==FTS_DP?rmdir(e->fts_path):unlink(e->fts_path))!=0)
            kh_fatal(NULL,"theater-documents-cleanup: %s",strerror(errno));
    }
    if(fts_close(tree)) kh_fatal(NULL,"theater-documents-cleanup-close");
}
static DIR *lock_visits(void);
void kh_theater_docs_end(void){
    pthread_mutex_lock(&visit_mutex);
    atomic_store(&enabled,0);
    if(visit_fd>=0){
        DIR *dir=lock_visits();
        remove_visit(overlay);
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
       flock(fd,LOCK_EX)) kh_fatal(NULL,"theater-documents-registry-lock");
    DIR *dir=fdopendir(fd);
    if(!dir) kh_fatal(NULL,"theater-documents-registry-open");
    return dir;
}
void kh_theater_docs_init(int create){
    char leaf[64];
    snprintf(leaf,sizeof leaf,"khnmc-theater-%lu",(unsigned long)geteuid());
    kh_tmp_path(visits,sizeof visits,leaf);
    if(create){ if(mkdir(visits,0700)&&errno!=EEXIST) kh_fatal(NULL,"theater-documents-registry-create"); }
    else if(access(visits,F_OK)&&errno==ENOENT) return;
    DIR *dir=lock_visits();
    for(;;){
        errno=0;
        struct dirent *e=readdir(dir);
        if(!e){ if(errno) kh_fatal(NULL,"theater-documents-registry-read"); break; }
        if(strncmp(e->d_name,"visit-",6) || strlen(e->d_name)!=12) continue;
        int fd=openat(dirfd(dir),e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(fd<0){ if(errno==ENOENT) continue; kh_fatal(NULL,"theater-documents-visit-open"); }
        if(!flock(fd,LOCK_EX|LOCK_NB)){
            char path[KH_TMP_MAX];
            if(snprintf(path,sizeof path,"%s/%s",visits,e->d_name)>=(int)sizeof path) kh_fatal(NULL,"theater-documents-path");
            remove_visit(path);
        }else if(errno!=EWOULDBLOCK) kh_fatal(NULL,"theater-documents-visit-lock");
        close(fd);
    }
    closedir(dir);
}

void kh_theater_docs_begin(void){
    pthread_mutex_lock(&visit_mutex);
    if(*overlay) kh_fatal(NULL,"theater-documents-busy");
    DIR *dir=lock_visits();
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
