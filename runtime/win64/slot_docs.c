/* Keep the guest's initial save/profile files with the replay history. A
 * bootstrap must neither consume a later profile nor overwrite player saves. */
#include "slot_disk.h"
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROFILE "My Games/KINGDOM HEARTS HD 1.5+2.5 ReMIX"
#define DIRECTORY UINT64_MAX
static char overlay[PATH_MAX];
static _Atomic int overlay_active;

const char *kh_slot_documents(void){ return atomic_load(&overlay_active)?overlay:kh_docs; }
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
const char *kh_slot_docs_capture(KhBootDocs *d){
    char path[PATH_MAX];
    if(snprintf(path,sizeof path,"%s/%s",kh_docs,PROFILE)>=(int)sizeof path) return "savestate-documents-path";
    struct stat st;
    if(lstat(path,&st)) return errno==ENOENT?NULL:"savestate-documents-stat";
    if(!S_ISDIR(st.st_mode)) return "savestate-documents-type";
    char *paths[]={path,NULL};
    FTS *tree=fts_open(paths,FTS_PHYSICAL|FTS_NOCHDIR,entry_order);
    if(!tree) return "savestate-documents-open";
    const char *error=NULL;
    size_t base=strlen(path);
    while(!error){
        errno=0;
        FTSENT *e=fts_read(tree);
        if(!e){ if(errno) error="savestate-documents-read"; break; }
        if(e->fts_info==FTS_DP) continue;
        if(e->fts_info!=FTS_D && e->fts_info!=FTS_F){ error="savestate-documents-type"; break; }
        const char *name=e->fts_path+base+(e->fts_level!=0);
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
    fts_close(tree); return error;
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
    if(d->size && (snprintf(path,sizeof path,"%s/My Games",overlay)>=(int)sizeof path||mkdir(path,0700))) goto failed;
    for(size_t pos=0;pos<d->size;){
        u32 n; u64 bytes;
        if(d->size-pos<sizeof n+sizeof bytes) goto failed;
        memcpy(&n,d->data+pos,sizeof n); pos+=sizeof n;
        memcpy(&bytes,d->data+pos,sizeof bytes); pos+=sizeof bytes;
        char name[PATH_MAX];
        if(n>=sizeof name||n>d->size-pos||memchr(d->data+pos,0,n)) goto failed;
        memcpy(name,d->data+pos,n); pos+=n; name[n]=0;
        if(!safe_name(name)||snprintf(path,sizeof path,"%s/%s%s%s",overlay,PROFILE,n?"/":"",name)>=(int)sizeof path) goto failed;
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
