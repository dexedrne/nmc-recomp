/* Keep the guest's initial save/profile files with the replay history. A
 * bootstrap must neither consume a later profile nor overwrite player saves. */
#include "slot_disk.h"
#include "autosave.h"
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <sys/file.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROFILE "My Games/KINGDOM HEARTS HD 1.5+2.5 ReMIX"
#define DIRECTORY UINT64_MAX
#define NEWEST (UINT64_MAX-1)
static char overlay[PATH_MAX];
static _Atomic int overlay_active;

const char *kh_slot_documents(void){ return atomic_load(&overlay_active)?overlay:kh_docs; }
const char *kh_slot_autosave_documents(void){
    return atomic_load(&overlay_active)?overlay:kh_autosave_documents();
}
static int rotation_slot(const char *name){
    if(strncmp(name,"KH2autosave-",12)||name[12]<'1'||name[12]>'9') return 0;
    const char *p=name+13;
    while(isdigit((unsigned char)*p)) p++;
    return !strcmp(p,".dat");
}
static int entry_order(const FTSENT **a,const FTSENT **b){ return strcmp((*a)->fts_name,(*b)->fts_name); }

static int append(KhBootDocs *d,const void *p,size_t n){
    if(n>KH_SLOT_DOCS_MAX-d->size) return 0;
    if(d->capacity<d->size+n){
        size_t cap=(d->size+n)*2;
        if(cap>KH_SLOT_DOCS_MAX) cap=KH_SLOT_DOCS_MAX;
        void *q=realloc(d->data,cap);
        if(!q) return 0;
        d->data=q; d->capacity=cap;
    }
    memcpy(d->data+d->size,p,n); d->size+=n; return 1;
}
static const char *capture_tree(KhBootDocs *d,const char *root,const char *sub,int rotation){
    char path[PATH_MAX];
    if(snprintf(path,sizeof path,"%s/%s",root,sub)>=(int)sizeof path) return "savestate-documents-path";
    struct stat st;
    if(lstat(path,&st)) return errno==ENOENT?NULL:"savestate-documents-stat";
    if(!S_ISDIR(st.st_mode)) return "savestate-documents-type";
    int lock=-1;
    if(rotation){
        lock=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        if(lock<0) return "savestate-autosave-open";
        if(flock(lock,LOCK_SH)){ close(lock); return "savestate-autosave-lock"; }
    }
    char *paths[]={path,NULL};
    FTS *tree=fts_open(paths,FTS_PHYSICAL|FTS_NOCHDIR,entry_order);
    if(!tree){ if(lock>=0) close(lock); return "savestate-documents-open"; }
    const char *error=NULL;
    size_t base=strlen(root)+1;
    while(!error){
        errno=0;
        FTSENT *e=fts_read(tree);
        if(!e){ if(errno) error="savestate-documents-read"; break; }
        if(e->fts_info==FTS_DP) continue;
        const char *name=e->fts_path+base;
        int marker=rotation && !strcmp(e->fts_name,"newest");
        if(rotation && e->fts_level>0 && !marker && !rotation_slot(e->fts_name)){
            if(e->fts_info==FTS_D) fts_set(tree,e,FTS_SKIP);
            continue;
        }
        if(marker){
            char target[40];
            ssize_t k=readlink(e->fts_path,target,sizeof target);
            if(k<=0 || k>=sizeof target){ error="savestate-autosave-marker"; break; }
            target[k]=0;
            u32 n=strlen(name),length=k; u64 type=NEWEST;
            if(!rotation_slot(target)){ error="savestate-autosave-marker"; break; }
            if(!append(d,&n,sizeof n)||!append(d,&type,sizeof type)||!append(d,name,n)||
               !append(d,&length,sizeof length)||!append(d,target,k)) error="savestate-documents-size";
            continue;
        }
        if(rotation && e->fts_level && e->fts_info!=FTS_F){ error="savestate-autosave-type"; break; }
        if(e->fts_info!=FTS_D && e->fts_info!=FTS_F){ error="savestate-documents-type"; break; }
        u32 n=strlen(name);
        u64 bytes=e->fts_info==FTS_D?DIRECTORY:(u64)e->fts_statp->st_size;
        if(!append(d,&n,sizeof n)||!append(d,&bytes,sizeof bytes)||!append(d,name,n)){
            error="savestate-documents-size"; break;
        }
        if(bytes==DIRECTORY) continue;
        FILE *f=fopen(e->fts_path,"rb");
        if(!f){ error="savestate-documents-open"; break; }
        u8 buf[32768];
        while(bytes){
            size_t k=bytes<sizeof buf?bytes:sizeof buf;
            if(fread(buf,1,k,f)!=k){ error="savestate-documents-read"; break; }
            if(!append(d,buf,k)){ error="savestate-documents-size"; break; }
            bytes-=k;
        }
        if(fgetc(f)!=EOF||ferror(f)) error="savestate-documents-changed";
        fclose(f);
    }
    fts_close(tree); if(lock>=0) close(lock); return error;
}
const char *kh_slot_docs_capture(KhBootDocs *d){
    const char *error=capture_tree(d,kh_docs,PROFILE,0);
    const char *enabled=getenv("KHNMC_AUTOSAVE");
    if(!error && enabled && !strcmp(enabled,"1"))
        error=capture_tree(d,kh_autosave_documents(),"autosave",1);
    return error;
}
static int safe_name(const char *p){
    if(!*p) return 1; /* the profile root */
    for(;;){
        size_t n=strcspn(p,"/");
        if(!n||(n==1&&*p=='.')||(n==2&&!memcmp(p,"..",2))||memchr(p,'\\',n)) return 0;
        if(!p[n]) return 1;
        p+=n+1;
    }
}
void kh_slot_docs_finish(void){
    atomic_store(&overlay_active,0);
    if(!*overlay) return;
    char *paths[]={overlay,NULL}; FTS *tree=fts_open(paths,FTS_PHYSICAL|FTS_NOCHDIR,NULL);
    if(tree){
        FTSENT *e;
        while((e=fts_read(tree))){
            if(e->fts_info==FTS_DP){ if(rmdir(e->fts_path)) kh_warn("savestate-overlay-cleanup: %s",e->fts_path); }
            else if(e->fts_info!=FTS_D && unlink(e->fts_path)) kh_warn("savestate-overlay-cleanup: %s",e->fts_path);
        }
        fts_close(tree);
    }
    *overlay=0;
}
const char *kh_slot_docs_begin(const KhBootDocs *d){
    if(*overlay) return "savestate-overlay-busy";
    char path[PATH_MAX];
    if(snprintf(path,sizeof path,"%s/.khss-bootstrap-XXXXXX",kh_root)>=(int)sizeof path||
       !mkdtemp(path)) return "savestate-overlay-create";
    strcpy(overlay,path);
    for(size_t pos=0;pos<d->size;){
        u32 n; u64 bytes;
        if(d->size-pos<sizeof n+sizeof bytes) goto failed;
        memcpy(&n,d->data+pos,sizeof n); pos+=sizeof n;
        memcpy(&bytes,d->data+pos,sizeof bytes); pos+=sizeof bytes;
        char name[PATH_MAX];
        if(n>=sizeof name||n>d->size-pos||memchr(d->data+pos,0,n)) goto failed;
        memcpy(name,d->data+pos,n); pos+=n; name[n]=0;
        if(!strcmp(name,PROFILE)){
            if(bytes!=DIRECTORY || snprintf(path,sizeof path,"%s/My Games",overlay)>=(int)sizeof path ||
               mkdir(path,0700)) goto failed;
        }
        int profile=!strcmp(name,PROFILE)||!strncmp(name,PROFILE "/",sizeof PROFILE);
        int slot=!strncmp(name,"autosave/",9)&&rotation_slot(name+9);
        int marker=!strcmp(name,"autosave/newest");
        if(!(profile||slot||marker||(!strcmp(name,"autosave")&&bytes==DIRECTORY))||
           !safe_name(name)||snprintf(path,sizeof path,"%s/%s",overlay,name)>=(int)sizeof path) goto failed;
        if(marker){
            u32 length; char target[40];
            if(bytes!=NEWEST||d->size-pos<sizeof length) goto failed;
            memcpy(&length,d->data+pos,sizeof length); pos+=sizeof length;
            if(length>=sizeof target||length>d->size-pos||memchr(d->data+pos,0,length)) goto failed;
            memcpy(target,d->data+pos,length); pos+=length; target[length]=0;
            if(!rotation_slot(target)||symlink(target,path)) goto failed;
            continue;
        }
        if(slot && bytes==DIRECTORY) goto failed;
        if(bytes==DIRECTORY){ if(mkdir(path,0700)) goto failed; continue; }
        if(!n||bytes>d->size-pos) goto failed;
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd<0) goto failed;
        FILE *f=fdopen(fd,"wb");
        if(!f){ close(fd); goto failed; }
        int ok=fwrite(d->data+pos,1,bytes,f)==bytes;
        if(fclose(f)||!ok) goto failed;
        pos+=bytes;
    }
    atomic_store(&overlay_active,1); return NULL;
failed:
    kh_slot_docs_finish(); return "savestate-overlay-profile";
}
