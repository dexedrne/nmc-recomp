/* data_deny.c (task 32) : a deny list for the data tree.
 *
 *   KHNMC_DATA_DENY=<file>      relative names of data files, one per line (`#` starts a comment line; case and the kind of slash do
 *                               not matter, as for the game's own requests).  A listed file behaves exactly as an absent one for
 *                               every loose-tree entry point: size (0x1063d0), the three loaders (0x106700 / 0x106790 / 0x106870)
 *                               and OpenStream (0x106da0).  Every denied request is counted; the first request for each name
 *                               prints `kh: data deny: ...` with the caller.
 *   KHNMC_DATA_DENY_FATAL=1     a request for a listed file ends the run at once in the named fatal `data-deny:` (file, kind of
 *                               request, caller).  That is how a wrong list shows itself: a silent miss would make the game walk an
 *                               unloaded table and die far away (as a missing area file did on another machine).
 * This is how a "these files can go" manifest (tools/prune_lists.py) is proved against real routes WITHOUT touching the tree.
 * Remastered textures (remastered/<file>/...) are read only when <file> itself was loaded (hd_assets.c).  A list that names such a
 * texture while its file is requested and not listed contradicts itself; an absent texture of a present file is already an error of
 * the HD layer, so that request is the named fatal `data-deny-remastered:` in both modes.
 * A list that cannot be read, names nothing, or holds a name that leaves the tree is the named fatal `data-deny-list:`. */
#include "khnmc_rt.h"
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static pthread_mutex_t dm=PTHREAD_MUTEX_INITIALIZER;
static int state;                                  /* 0 = not looked at yet, 1 = no list, 2 = a list is loaded */
static char **names; static size_t nnames;         /* sorted, lower case, '/' */
static char **rmdirs; static size_t nrmdirs;       /* the files whose remastered folder holds a listed texture */
static int fatal_mode;
static char list_path[1024];
static _Atomic u64 n_denied, n_asked;
#define SEEN_MAX 4096
static char *seen[SEEN_MAX]; static size_t nseen; static u64 n_distinct;

static void norm(const char *s,char *out,size_t cap){
    while(*s==' '||*s=='\t') s++;
    while(s[0]=='.'&&(s[1]=='/'||s[1]=='\\')) s+=2;
    while(*s=='/'||*s=='\\') s++;
    size_t n=0; for(;*s&&n+1<cap;s++) out[n++]=*s=='\\'?'/':(char)tolower((unsigned char)*s);
    while(n&&(out[n-1]=='\n'||out[n-1]=='\r'||out[n-1]==' '||out[n-1]=='\t'||out[n-1]=='/')) n--;
    out[n]=0;
}
static int cmp(const void *a,const void *b){ return strcmp(*(char*const*)a,*(char*const*)b); }
static size_t uniq(char **v,size_t n){ if(!n) return 0; size_t k=1; for(size_t i=1;i<n;i++){ if(strcmp(v[i],v[k-1])) v[k++]=v[i]; else free(v[i]); } return k; }
static void push(char ***v,size_t *n,const char *s){
    if(!(*n&(*n+1))||!*v){ size_t cap=*n?(*n+1)*2:64; char **nv=realloc(*v,cap*sizeof **v); if(!nv) kh_fatal(NULL,"data-deny-list: out of memory"); *v=nv; }
    if(!((*v)[*n]=strdup(s))) kh_fatal(NULL,"data-deny-list: out of memory");
    (*n)++;
}
static void load(KCtx *g){
    const char *p=getenv("KHNMC_DATA_DENY");
    if(!p||!*p){ state=1; return; }
    snprintf(list_path,sizeof list_path,"%s",p);
    FILE *f=fopen(p,"r"); if(!f) kh_fatal(g,"data-deny-list: cannot read KHNMC_DATA_DENY=%s (%s)",p,strerror(errno));
    char line[4200],nm[4200]; int ln=0;
    while(fgets(line,sizeof line,f)){
        ln++;
        if(!strchr(line,'\n')&&!feof(f)){ fclose(f); kh_fatal(g,"data-deny-list: %s line %d is longer than %zu bytes",p,ln,sizeof line-2); }
        norm(line,nm,sizeof nm);
        if(!nm[0]||nm[0]=='#') continue;
        if(strchr(nm,':')||!strncmp(nm,"../",3)||strstr(nm,"/../")||(strlen(nm)>=3&&!strcmp(nm+strlen(nm)-3,"/.."))||!strcmp(nm,"..")){
            fclose(f); kh_fatal(g,"data-deny-list: %s line %d names a path that is not inside the data tree: %s",p,ln,nm); }
        push(&names,&nnames,nm);
        if(!strncmp(nm,"remastered/",11)){ char d[4200]; snprintf(d,sizeof d,"%s",nm+11); char *s=strrchr(d,'/'); if(s){ *s=0; push(&rmdirs,&nrmdirs,d); } }
    }
    fclose(f);
    if(!nnames) kh_fatal(g,"data-deny-list: KHNMC_DATA_DENY=%s names no file (an empty list denies nothing: unset the variable instead)",p);
    qsort(names,nnames,sizeof *names,cmp); nnames=uniq(names,nnames);
    if(nrmdirs){ qsort(rmdirs,nrmdirs,sizeof *rmdirs,cmp); nrmdirs=uniq(rmdirs,nrmdirs); }
    const char *fm=getenv("KHNMC_DATA_DENY_FATAL"); fatal_mode=fm&&*fm&&strcmp(fm,"0");
    state=2;
    fprintf(stderr,"kh: data deny: %zu names from %s (%zu files with listed remastered textures); a request for one is %s\n",nnames,p,nrmdirs,
            fatal_mode?"the named fatal data-deny (KHNMC_DATA_DENY_FATAL)":"answered as for an absent file and counted");
}
static int listed(char **v,size_t n,const char *key){ return n&&bsearch(&key,v,n,sizeof *v,cmp)!=NULL; }

/* 1 = `rel` (tree-relative name) is on the list: the caller must answer as for an absent file.  `what` names the kind of request. */
int kh_data_denied(KCtx *g,const char *rel,const char *what){
    char nm[2100]; int first=0;
    pthread_mutex_lock(&dm);
    if(!state) load(g);
    if(state!=2){ pthread_mutex_unlock(&dm); return 0; }
    n_asked++;
    norm(rel,nm,sizeof nm);
    if(!listed(names,nnames,nm)){ pthread_mutex_unlock(&dm); return 0; }
    n_denied++;
    { const char *k=nm; if(!nseen||!bsearch(&k,seen,nseen,sizeof *seen,cmp)){ first=1; n_distinct++;
          if(nseen<SEEN_MAX&&(seen[nseen]=strdup(nm))){ nseen++; qsort(seen,nseen,sizeof *seen,cmp); } } }
    int fm=fatal_mode; char lp[1024]; snprintf(lp,sizeof lp,"%s",list_path);
    pthread_mutex_unlock(&dm);
    u64 ra=g&&GA_OK(KH_RSP(g))?rd64(g,KH_RSP(g)):0;
    if(first||fm) fprintf(stderr,"kh: data deny: %s request for %s is denied (caller rva 0x%llx)\n",what,nm,(unsigned long long)(ra?ra-GS_BASE:0));
    if(fm) kh_fatal(g,"data-deny: the game asked for %s (%s request, caller rva 0x%llx), which the deny list %s names: the list is wrong, or this build must not load that file",
                    nm,what,(unsigned long long)(ra?ra-GS_BASE:0),lp);
    return 1;
}
/* the HD layer is about to read the remastered folder of `rel`: a listed texture below it contradicts the list */
void kh_data_deny_remastered(KCtx *g,const char *rel){
    char nm[2100];
    pthread_mutex_lock(&dm);
    if(!state) load(g);
    int hit=0; if(state==2&&nrmdirs){ norm(rel,nm,sizeof nm); hit=listed(rmdirs,nrmdirs,nm); }
    char lp[1024]; snprintf(lp,sizeof lp,"%s",list_path);
    pthread_mutex_unlock(&dm);
    if(hit){ n_denied++; kh_fatal(g,"data-deny-remastered: the game loaded %s, and the deny list %s names textures of remastered/%s but not the file itself: "
                                   "an absent texture of a present file is an error of the HD layer, the list contradicts itself",nm,lp,nm); }
}
u64 kh_data_deny_count(void){ return n_denied; }
void kh_data_deny_report(FILE *f){
    pthread_mutex_lock(&dm);
    if(state==2) fprintf(f,"kh: data deny: list %s (%zu names): %llu requests checked, %llu denied (%llu distinct names)%s\n",list_path,nnames,(unsigned long long)n_asked,
                         (unsigned long long)n_denied,(unsigned long long)n_distinct,n_denied?"":": zero denied requests");
    pthread_mutex_unlock(&dm);
}
/* selftest only: forget the list, so that the next request reads the environment again */
void kh_data_deny_test_reset(void){
    pthread_mutex_lock(&dm);
    for(size_t i=0;i<nnames;i++) free(names[i]); free(names); names=NULL; nnames=0;
    for(size_t i=0;i<nrmdirs;i++) free(rmdirs[i]); free(rmdirs); rmdirs=NULL; nrmdirs=0;
    for(size_t i=0;i<nseen;i++) free(seen[i]); nseen=0; n_distinct=0; n_denied=0; n_asked=0; state=0; fatal_mode=0; list_path[0]=0;
    pthread_mutex_unlock(&dm);
}

/* ---- selftest: a synthetic tree, the real entry points through the dispatch table, the fatal cases in forked children --------- */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"data deny selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static void put(const char *dir,const char *rel,const char *text){
    char p[KH_TMP_MAX+300]; snprintf(p,sizeof p,"%s/%s",dir,rel); FILE *f=fopen(p,"wb"); if(!f) kh_fatal(NULL,"data deny selftest: cannot write %s",p); fputs(text,f); fclose(f);
}
/* runs fn(g,arg) in a child with stderr captured; returns the exit status (or -1) and the output */
static int in_child(KCtx *g,void (*fn)(KCtx*,u64),u64 arg,char *out,size_t cap){
    FILE *cap_f=kh_tmpfile(); if(!cap_f) kh_fatal(g,"data deny selftest: tmpfile failed");
    pid_t c=kh_fork(); if(c<0) kh_fatal(g,"data deny selftest: fork failed");
    if(!c){ if(dup2(fileno(cap_f),STDERR_FILENO)<0) _exit(98); fn(g,arg); _exit(99); }
    int st=0; if(waitpid(c,&st,0)!=c){ fclose(cap_f); return -1; }
    rewind(cap_f); size_t n=fread(out,1,cap-1,cap_f); out[n]=0; fclose(cap_f);
    return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
static void child_load(KCtx *g,u64 name){ u64 dst=ga_alloc(64,16); kh_call(g,GS_BASE+0x106700,0,name,dst,0); }
static void child_size(KCtx *g,u64 name){ kh_call(g,GS_BASE+0x1063d0,0,name,0,0); }
static void child_stream(KCtx *g,u64 name){ kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,name,0,0); }
static void child_rm(KCtx *g,u64 name){ kh_data_deny_remastered(g,gstr(g,name)); }
int kh_data_deny_selftest(KCtx *g){
    checks=bad=0;
    char dir[KH_TMP_MAX],p[KH_TMP_MAX+300],lst[KH_TMP_MAX+300],flog[KH_TMP_MAX+300]; kh_tmp_path(dir,sizeof dir,"khnmc-deny-XXXXXX");
    const char *keys[]={"KHNMC_DATA","KHNMC_FILELOG","KHNMC_DATA_DENY","KHNMC_DATA_DENY_FATAL"}; char *old[4];
    for(int i=0;i<4;i++){ const char *v=getenv(keys[i]); old[i]=v?strdup(v):NULL; }
    if(!mkdtemp(dir)) return 1;
    snprintf(p,sizeof p,"%s/Sub",dir); mkdir(p,0700); snprintf(p,sizeof p,"%s/VagStream",dir); mkdir(p,0700);
    put(dir,"keep.bin","keep"); put(dir,"Sub/Gone.bin","gone!"); put(dir,"Sub/stay.bin","stay"); put(dir,"VagStream/Song.win32.scd","0123456789"); put(dir,"VagStream/Other.win32.scd","abc");
    snprintf(lst,sizeof lst,"%s/deny.list",dir); snprintf(flog,sizeof flog,"%s/files.log",dir);
    put(dir,"deny.list","# a comment line\n\nSUB\\gone.BIN\r\n./vagstream/song.win32.scd\nremastered/sub/stay.bin/-0.dds\nnot/in/the/tree.bin\n");
    setenv("KHNMC_DATA",dir,1); setenv("KHNMC_FILELOG",flog,1); unsetenv("KHNMC_DATA_DENY"); unsetenv("KHNMC_DATA_DENY_FATAL");
    u64 keep=ga_strdup("keep.bin"),gone=ga_strdup("sub/gone.bin"),gone2=ga_strdup("SUB\\GONE.bin"),stay=ga_strdup("sub/stay.bin"),missing=ga_strdup("sub/never.bin"),
        song=ga_strdup("vagstream/song.win32.scd"),other=ga_strdup("vagstream/other.win32.scd"),dst=ga_alloc(64,16),sz=ga_alloc(16,16);
    /* without a list: everything is served, nothing is counted */
    kh_data_deny_test_reset();
    T(kh_call(g,GS_BASE+0x1063d0,0,gone,0,0)==5); T(kh_call(g,GS_BASE+0x106700,0,gone,dst,0)==5&&!memcmp(hp(g,dst),"gone!",5)); T(kh_data_deny_count()==0);
    { int fd=(s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,song,0,0); T(fd>0); if(fd>0) kh_call(g,GS_BASE+0x106b40,GS_BASE+0x79cfc0,(u64)fd,0,0); }
    /* what an ABSENT file gives at each entry point: the reference for "exactly as absent" */
    s32 abs_size=(s32)kh_call(g,GS_BASE+0x1063d0,0,missing,0,0), abs_load=(s32)kh_call(g,GS_BASE+0x106700,0,missing,dst,0);
    u64 abs_bound=kh_call5(g,GS_BASE+0x106790,0,missing,dst,64,0); wr32(g,sz,123); u64 abs_alloc=kh_call5(g,GS_BASE+0x106870,0,missing,sz,0,0); u32 abs_sz=rd32(g,sz);
    s32 abs_stream=(s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,missing,0,0);
    T(abs_size==0&&abs_load==-1&&abs_bound==0&&abs_alloc==0&&abs_sz==123&&abs_stream==-1);
    /* with the list: the listed names answer exactly like that, in any spelling; the others are untouched */
    setenv("KHNMC_DATA_DENY",lst,1); kh_data_deny_test_reset();
    T((s32)kh_call(g,GS_BASE+0x1063d0,0,gone,0,0)==abs_size); T(kh_data_deny_count()==1);
    memset(hp(g,dst),0x55,64); T((s32)kh_call(g,GS_BASE+0x106700,0,gone,dst,0)==abs_load); T(rd8(g,dst)==0x55);                      /* nothing was read into the buffer */
    T(kh_call5(g,GS_BASE+0x106790,0,gone2,dst,64,0)==abs_bound); T(rd8(g,dst)==0x55);
    wr32(g,sz,123); T(kh_call5(g,GS_BASE+0x106870,0,gone,sz,0,0)==abs_alloc&&rd32(g,sz)==abs_sz);
    wr32(g,sz,(u32)-1); kh_call5(g,GS_BASE+0x106870,0,gone,sz,0,0); { u32 denied_probe=rd32(g,sz); wr32(g,sz,(u32)-1); kh_call5(g,GS_BASE+0x106870,0,missing,sz,0,0); T(denied_probe==rd32(g,sz)); }   /* the size probe too */
    T((s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,song,0,0)==abs_stream);
    T(kh_data_deny_count()==6);                                                                                                      /* every denied request is counted */
    T(kh_call(g,GS_BASE+0x1063d0,0,keep,0,0)==4); T(kh_call(g,GS_BASE+0x106700,0,stay,dst,0)==4&&!memcmp(hp(g,dst),"stay",4));
    { int fd=(s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,other,0,0); T(fd>0); if(fd>0) kh_call(g,GS_BASE+0x106b40,GS_BASE+0x79cfc0,(u64)fd,0,0); }
    T((s32)kh_call(g,GS_BASE+0x106700,0,missing,dst,0)==abs_load); T(kh_data_deny_count()==6);                                         /* a plain miss is not a denied request */
    { FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){ kh_data_deny_report(rf); rewind(rf); char b[600]; size_t n=fread(b,1,sizeof b-1,rf); b[n]=0; fclose(rf);
          T(strstr(b,"6 denied (2 distinct names)")!=NULL); T(strstr(b,"(4 names)")!=NULL); } }
    { FILE *lf=fopen(flog,"r"); char line[4096]; int miss_gone=0,hit_gone=0; while(lf&&fgets(line,sizeof line,lf)){ if(strstr(line,"gone.bin")||strstr(line,"GONE.bin")){ if(!strncmp(line,"MISS NONE",9)) miss_gone++; else hit_gone++; } } if(lf) fclose(lf);
      T(miss_gone>=5&&hit_gone==2); }                                                 /* the file log shows the denied requests as misses (the 2 hits are from before the list) */
    T(access(p,F_OK)==0); snprintf(p,sizeof p,"%s/Sub/Gone.bin",dir); T(access(p,R_OK)==0);                                          /* the tree itself was not touched */
    if(!kh_fork_ok()) kh_selftest_not_run("data deny","the named fatals data-deny, data-deny-remastered and data-deny-list (5 children)",16);
    else {
        char out[16384]; int rc;
        setenv("KHNMC_DATA_DENY_FATAL","1",1); kh_data_deny_test_reset();
        rc=in_child(g,child_load,gone,out,sizeof out); T(rc==3); T(strstr(out,"FATAL: data-deny: the game asked for sub/gone.bin (load request")!=NULL); T(strstr(out,"caller rva 0x")!=NULL); T(strstr(out,"deny.list")!=NULL);
        rc=in_child(g,child_size,gone2,out,sizeof out); T(rc==3); T(strstr(out,"data-deny: the game asked for sub/gone.bin (size request")!=NULL);
        rc=in_child(g,child_stream,song,out,sizeof out); T(rc==3); T(strstr(out,"data-deny: the game asked for vagstream/song.win32.scd (stream request")!=NULL);
        rc=in_child(g,child_load,keep,out,sizeof out); T(rc==99); T(strstr(out,"kh: FATAL:")==NULL);                                 /* an unlisted file does not end the run */
        rc=in_child(g,child_rm,stay,out,sizeof out); T(rc==3); T(strstr(out,"data-deny-remastered: the game loaded sub/stay.bin")!=NULL);
        setenv("KHNMC_DATA_DENY_FATAL","0",1); kh_data_deny_test_reset();
        rc=in_child(g,child_load,gone,out,sizeof out); T(rc==99);                                                                     /* FATAL=0 is the counting mode */
        snprintf(p,sizeof p,"%s/no-such.list",dir); setenv("KHNMC_DATA_DENY",p,1); kh_data_deny_test_reset();
        rc=in_child(g,child_load,keep,out,sizeof out); T(rc==3); T(strstr(out,"data-deny-list: cannot read KHNMC_DATA_DENY=")!=NULL);
        put(dir,"empty.list","# nothing\n\n"); snprintf(p,sizeof p,"%s/empty.list",dir); setenv("KHNMC_DATA_DENY",p,1); kh_data_deny_test_reset();
        rc=in_child(g,child_load,keep,out,sizeof out); T(rc==3); T(strstr(out,"data-deny-list:")!=NULL&&strstr(out,"names no file")!=NULL);
        put(dir,"escape.list","ok.bin\n../outside.bin\n"); snprintf(p,sizeof p,"%s/escape.list",dir); setenv("KHNMC_DATA_DENY",p,1); kh_data_deny_test_reset();
        rc=in_child(g,child_load,keep,out,sizeof out); T(rc==3); T(strstr(out,"line 2 names a path that is not inside the data tree")!=NULL);
    }
    for(int i=0;i<4;i++){ if(old[i]) setenv(keys[i],old[i],1); else unsetenv(keys[i]); free(old[i]); }
    kh_data_deny_test_reset();
    ga_free(keep); ga_free(gone); ga_free(gone2); ga_free(stay); ga_free(missing); ga_free(song); ga_free(other); ga_free(dst); ga_free(sz);
    const char *rm[]={"keep.bin","Sub/Gone.bin","Sub/stay.bin","VagStream/Song.win32.scd","VagStream/Other.win32.scd","deny.list","files.log","empty.list","escape.list"};
    for(size_t i=0;i<sizeof rm/sizeof *rm;i++){ snprintf(p,sizeof p,"%s/%s",dir,rm[i]); unlink(p); }
    snprintf(p,sizeof p,"%s/Sub",dir); rmdir(p); snprintf(p,sizeof p,"%s/VagStream",dir); rmdir(p); rmdir(dir);
    fprintf(stderr,"kh: data deny selftest: %d checks, %d failures\n",checks,bad); return bad;
}
