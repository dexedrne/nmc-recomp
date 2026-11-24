#include "khnmc_rt.h"
#include "rmc.h"
#include "kh_look.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static KhRmcFile *files;
static size_t count;

static int by_path(const void *a,const void *b){
    return strcasecmp(((const KhRmcFile *)a)->rel,((const KhRmcFile *)b)->rel);
}
static size_t lower(const char *key){
    size_t lo=0,hi=count;
    while(lo<hi){
        size_t mid=lo+(hi-lo)/2;
        if(strcasecmp(files[mid].rel,key)<0) lo=mid+1; else hi=mid;
    }
    return lo;
}
KhRmcFile *kh_rmc_find(const char *rel){
    if(!count) return NULL;
    char key[4096]; size_t n=0;
    while(*rel){
        size_t len=strcspn(rel,"/");
        if(len && !(len==1 && *rel=='.')){
            if(n+len+2>sizeof key) return NULL;
            if(n) key[n++]='/';
            memcpy(key+n,rel,len); n+=len;
        }
        rel+=len; if(*rel) rel++;
    }
    key[n]=0;
    size_t i=lower(key);
    return i<count && !strcasecmp(files[i].rel,key)?&files[i]:NULL;
}
size_t kh_rmc_children(const char *dir,KhRmcFile **first){
    *first=NULL;
    if(!count) return 0;
    if(!*dir){ *first=files; return count; }
    char prefix[4096]; int n=snprintf(prefix,sizeof prefix,"%s/",dir);
    if(n<0 || (size_t)n>=sizeof prefix) return 0;
    size_t i=lower(prefix),end=i;
    while(end<count && !strncasecmp(files[end].rel,prefix,n)) end++;
    if(end>i) *first=files+i;
    return end-i;
}
void kh_rmc_report(KhRmcFile *file){
    if(!__atomic_exchange_n(&file->reported,1,__ATOMIC_RELAXED))
        fprintf(stderr,"kh: RMC override: %s\n",file->rel);
}

int kh_rmc_path(const char *rel,char *host,size_t cap){
    KhRmcFile *f=kh_rmc_find(rel);
    if(!f) return 0;
    if(snprintf(host,cap,"%s",f->host)>=(int)cap) kh_fatal(NULL,"rmc-path-length: %s",rel);
    kh_rmc_report(f);
    return KH_FILE_RMC;
}
int kh_data_path(const char *root,const char *rel,char *host,size_t cap){
    if(kh_rmc_path(rel,host,cap)) return KH_FILE_RMC;
    return kh_resolve_ci(root,rel,host,cap);
}

static void index_files(const char *root,int flags){
    for(size_t i=0;i<count;i++) free(files[i].host);
    free(files); files=NULL; count=0;
    if(!root || !*root) return;
    struct stat st;
    if(stat(root,&st)){
        if(errno!=ENOENT && errno!=ENOTDIR) kh_fatal(NULL,"rmc-directory: %s: %s",root,strerror(errno));
    }else if(S_ISDIR(st.st_mode)){
        char *paths[]={(char *)root,NULL};
        FTS *tree=fts_open(paths,flags|FTS_NOCHDIR,NULL);
        if(!tree) kh_fatal(NULL,"rmc-index: %s: %s",root,strerror(errno));
        FTSENT *e; size_t cap=0,base=strlen(root);
        while(base && root[base-1]=='/') base--;
        while((e=fts_read(tree))){
            if(e->fts_info==FTS_D || e->fts_info==FTS_DP) continue;
            if(e->fts_info!=FTS_F)
                kh_fatal(NULL,"rmc-index: invalid or unreadable entry %s",e->fts_path);
            if(count==cap){
                cap=cap?cap*2:32;
                files=realloc(files,cap*sizeof *files);
                if(!files) kh_fatal(NULL,"rmc-index: out of memory");
            }
            char *host=strdup(e->fts_path);
            if(!host) kh_fatal(NULL,"rmc-index: out of memory");
            const char *rel=host+base; while(*rel=='/') rel++;
            files[count++]=(KhRmcFile){host,rel,e->fts_statp->st_size,0};
        }
        if(errno || fts_close(tree)) kh_fatal(NULL,"rmc-index: %s: %s",root,strerror(errno));
        if(count) qsort(files,count,sizeof *files,by_path);
        for(size_t i=1;i<count;i++) if(!by_path(files+i-1,files+i)) kh_fatal(NULL,"rmc-case-collision: %s",files[i].rel);
    }
}

void kh_rmc_init(const char *root){
    index_files(root,FTS_LOGICAL);
    if(root && *root) fprintf(stderr,"kh: RMC overlay: %zu files in %s\n",count,root);
}

static void check_pack_dir(const char *path){
    struct stat st;
    if(lstat(path,&st)==0){
        if(!S_ISDIR(st.st_mode)) kh_fatal(NULL,"rmc-sync-directory: %s",path);
    }else if(errno!=ENOENT) kh_fatal(NULL,"rmc-sync-directory: %s: %s",path,strerror(errno));
}

/* The uploader writes the complete NUL-delimited inventory last. Only the
 * physical walk produces deletion targets. A look's index is temporary. */
static int sync_inventory(const char *docs,const char *look,const char *selected){
    char manifest[4096],root[4096],parent[4096];
    if(look){
        if(snprintf(parent,sizeof parent,"%s/looks",docs)>=(int)sizeof parent ||
           snprintf(root,sizeof root,"%s/%s",parent,look)>=(int)sizeof root ||
           snprintf(manifest,sizeof manifest,"%s/looks-%s.pending",docs,look)>=(int)sizeof manifest)
            kh_fatal(NULL,"rmc-sync-path-length");
    }else if(snprintf(manifest,sizeof manifest,"%s/rmc-pack.pending",docs)>=(int)sizeof manifest ||
             snprintf(root,sizeof root,"%s/rmc",docs)>=(int)sizeof root) kh_fatal(NULL,"rmc-sync-path-length");
    int fd=open(manifest,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0){
        if(errno==ENOENT) return 0;
        kh_fatal(NULL,"rmc-manifest-open: %s: %s",manifest,strerror(errno));
    }
    struct stat st;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode)) kh_fatal(NULL,"rmc-manifest-type: %s",manifest);
    FILE *in=fdopen(fd,"rb");
    if(!in) kh_fatal(NULL,"rmc-manifest-open: %s: %s",manifest,strerror(errno));
    if(look) check_pack_dir(parent);
    check_pack_dir(root);
    index_files(root,FTS_PHYSICAL);
    char *line=NULL; size_t cap=0; ssize_t n;
    while((n=getdelim(&line,&cap,0,in))>0){
        if(line[n-1]!=0) break;
        if(!strcmp(line,"/")) break;
        KhRmcFile *f=kh_rmc_find(line);
        if(!f || f->reported) kh_fatal(NULL,"rmc-manifest-missing-or-duplicate: %s",line);
        f->reported=1;
    }
    if(n!=2 || memcmp(line,"/",2) || fgetc(in)!=EOF || ferror(in) || fclose(in))
        kh_fatal(NULL,"rmc-manifest-incomplete: %s",manifest);
    free(line);
    size_t kept=0;
    for(size_t i=0;i<count;i++){
        if(files[i].reported){
            files[i].reported=0;
            files[kept++]=files[i];
        }else{
            if(unlink(files[i].host)) kh_fatal(NULL,"rmc-prune: %s: %s",files[i].host,strerror(errno));
            free(files[i].host);
        }
    }
    if(look) fprintf(stderr,"kh: look sync: %s: removed %zu stale files; kept %zu\n",look,count-kept,kept);
    else fprintf(stderr,"kh: RMC sync: removed %zu stale files\n",count-kept);
    count=kept;
    if(unlink(manifest)) kh_fatal(NULL,"rmc-manifest-finish: %s",strerror(errno));
    if(selected && !strcmp(selected,root)){
        fprintf(stderr,"kh: RMC overlay: %zu files in %s\n",count,root);
        return 1;
    }
    kh_rmc_init(NULL);
    return 0;
}

int kh_rmc_sync(const char *docs,const char *selected){
    static const char *const looks[]={"xion","xion-hooded","axel","roxas-hd"};
    for(size_t i=0;i<sizeof looks/sizeof *looks;i++) sync_inventory(docs,looks[i],NULL);
    return sync_inventory(docs,NULL,selected);
}
