/* Loose-first entry points; real packages use the untouched lifted implementation. */
#include "khnmc_rt.h"
#include "khnmc_hd.h"
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static pthread_mutex_t files_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned real_packages;
void orig_f_106000(KCtx *g);
void orig_f_1063d0(KCtx *g);
void orig_f_106700(KCtx *g);
void orig_f_106790(KCtx *g);
void orig_f_106870(KCtx *g);
int kh_data_denied(KCtx *g,const char *rel,const char *what);        /* data_deny.c (task 32): KHNMC_DATA_DENY */
void kh_data_deny_remastered(KCtx *g,const char *rel);
void kh_music_note_file(KCtx *g,const char *path,int hit,long long size);   /* music_log.c (task 32): bgm/ and vagstream/ requests */

static int real_package(const char *name){
    if(!getenv("KHNMC_PKGDIR")) return 0;
    char win[2048],host[2048]; struct stat st;
    const char *exts[]={"hed","pkg"};
    for(int i=0;i<2;i++){
        if(snprintf(win,sizeof win,"%s.%s",name,exts[i])>=(int)sizeof win ||
           !kh_winpath(win,host,sizeof host)||stat(host,&st)||!S_ISREG(st.st_mode)) return 0;
    }
    return 1;
}
static int package_fallback(const char *path){
    char normalized[2048]; size_t n=strlen(path);
    if(!real_packages||n>=sizeof normalized) return 0;
    for(size_t i=0;i<=n;i++) normalized[i]=path[i]=='\\'?'/':path[i];
    for(char *p=normalized;*p;){ size_t len=strcspn(p,"/");
        if(len==2&&!strncmp(p,"..",2)) return 0; p+=len; if(*p) p++; }
    return 1;
}
static void file_log(KCtx *g,const char *path,const char *source,int size){
    kh_phase_file(g,path); kh_music_note_file(g,path,source!=NULL,size);
    { extern void kh_warp_file_event(KCtx *,const char *,const char *); kh_warp_file_event(g,path,source); }   /* task 33: attributed to a debug warp while one is watched */
    const char *log=getenv("KHNMC_FILELOG"); FILE *lf=fopen(log?log:"filelog.txt","a");
    if(!lf) kh_fatal(g,"cannot append file log %s",log?log:"filelog.txt");
    int failed=fprintf(lf,"%s %s %d %s\n",source?"HIT":"MISS",source?source:"NONE",size,path)<0;
    if(fclose(lf)||failed) kh_fatal(g,"cannot write file log");
}

void f_106000(KCtx *g){
    u64 obj=A0(g); const char *name=gstr(g,A1(g));
    if(real_package(name)){
        orig_f_106000(g);
        if(!rd64(g,obj+8)||(s32)rd32(g,obj+0x10)<0)
            kh_fatal(g,"real package constructor failed: %s",name);
        real_packages++;
        fprintf(stderr,"kh: real package registered: %s (%u entries)\n",name,rd32(g,obj+0x14));
        return;
    }
    fprintf(stderr,"kh: empty package registered: %s\n",name);
    memset(hp(g,obj),0,0x260);
    wr64(g,obj,GS_BASE+0x585fb0);
    wr64(g,obj+8,ga_calloc(1,32)); /* non-NULL is the package-ready flag; count stays zero */
    if(!rd64(g,obj+8)) kh_fatal(g,"empty package table allocation failed");
    wr32(g,obj+0x10,-1);
    snprintf(hp(g,obj+0x20),260,"%s.pkg",name);
    SET_RAX(g,obj); KH_RET(g);
}

static int normalize(const char *s,char *out,size_t cap){
    size_t n=strlen(s); if(n>=cap) return 0;
    for(size_t i=0;i<=n;i++) out[i]=s[i]=='\\'?'/':s[i];
    return 1;
}
static int relative(KCtx *g,const char *path,char *rel,size_t cap){
    char base[2048];
    if(!normalize(path,rel,cap)||!normalize(gstr(g,GS_BASE+0x2bd4340),base,sizeof base)) return 0;
    size_t n=strlen(base);
    if(n&&!strncasecmp(rel,base,n)&&rel[n]=='/') memmove(rel,rel+n+1,strlen(rel+n+1)+1);
    /* Reject absolute and parent paths before using the shared Windows resolver. */
    if(rel[0]=='/'||strchr(rel,':')) return 0;
    for(char *p=rel;*p;){ size_t n=strcspn(p,"/"); if(n>=512||(n==2&&!strncmp(p,"..",2))) return 0; p+=n; if(*p) p++; }
    return 1;
}
/* `what` names the kind of request for the deny list: a listed file is answered exactly as an absent one (data_deny.c) */
static FILE *open_loose(KCtx *g,const char *path,int *size,const char *what){
    char rel[2048],host[4096]; FILE *f=NULL; struct stat st;
    const char *root=getenv("KHNMC_DATA"); if(!root) root="data";
    if(relative(g,path,rel,sizeof rel)&&!kh_data_denied(g,rel,what)&&strlen(root)+strlen(rel)+2<sizeof host &&
       kh_resolve_ci(root,rel,host,sizeof host)){
        f=fopen(host,"rb");
        if(f&&(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||st.st_size>INT_MAX)){ fclose(f); f=NULL; }
        if(f) *size=(int)st.st_size;
    }
    return f;
}
/* For the stream-fd layer (stream_layer.c): the same tree-relative mapping and the same file log as the loaders. */
int kh_file_loose_path(KCtx *g,const char *path,char *host,size_t cap){
    char rel[2048]; const char *root=getenv("KHNMC_DATA"); if(!root) root="data";
    return relative(g,path,rel,sizeof rel)&&!kh_data_denied(g,rel,"stream")&&strlen(root)+strlen(rel)+2<cap&&kh_resolve_ci(root,rel,host,cap);
}
void kh_file_log_entry(KCtx *g,const char *path,const char *source,long long size){ file_log(g,path,source,size>INT_MAX?INT_MAX:(int)size); }
int kh_file_package_fallback(const char *path){ return package_fallback(path); }
static int register_resource(KCtx *g,u64 path,int n,u64 dst){
    /* HD sidecars: the lifted ResourceEntry constructor runs inside SetResourceItem on this thread and pulls GetRemasteredCount/Entry/Asset (hd_assets.c). */
    char rel[2048]; const char *root=getenv("KHNMC_DATA"); if(!root) root="data";
    if(!relative(g,gstr(g,path),rel,sizeof rel)) kh_fatal(g,"hd assets: cannot derive a tree-relative path from %s",gstr(g,path));
    kh_data_deny_remastered(g,rel);                    /* a deny list that names textures of this file but not the file: named fatal */
    wr32(g,GS_BASE+0x79cfd4,0);
    HdTable *prev=kh_hd_begin(g,root,rel,(const unsigned char*)hp(g,dst),n);
    int ok=(s32)kh_call(g,GS_BASE+0x12f070,path,n,dst,0)!=-1;
    kh_hd_end(prev);
    return ok;
}
void f_1063d0(KCtx *g){
    pthread_mutex_lock(&files_lock);
    int n=0; FILE *f=open_loose(g,gstr(g,A1(g)),&n,"size");
    if(f){ fclose(f); file_log(g,gstr(g,A1(g)),"LOOSE",n); }
    else if(package_fallback(gstr(g,A1(g)))){
        const char *path=gstr(g,A1(g)); orig_f_1063d0(g);
        n=(s32)KH_RAX(g); file_log(g,path,n>0?"PACKAGE":NULL,n);
        pthread_mutex_unlock(&files_lock); return;
    } else { char host[2048]; struct stat st; kh_winpath(gstr(g,A1(g)),host,sizeof host);
        if(!stat(host,&st)&&S_ISREG(st.st_mode)&&st.st_size<=INT_MAX) n=st.st_size;
        file_log(g,gstr(g,A1(g)),n>0?"LOOSE":NULL,n); }
    pthread_mutex_unlock(&files_lock); SET_RAX(g,n); KH_RET(g);
}
/* Task 31: which loaded files the game still holds.  Every file read with allocation (0x106870: the buffer is the game's to free)
 * and every HD asset handed out by GetRemasteredAsset (0x106460, hd_assets.c: the `file` source of the arena is these two) is noted
 * (buffer, size, path, game frame); the report asks the arena whether that buffer is still the live `file` block of that
 * size (ga_describe) and forgets the notes of buffers that are gone.  Read-only for the game; `kh: file buffers` lines at every
 * map report and at exit (the largest ones by name).  What the id zone's growth through Twilight Town is made of. */
typedef struct { u64 dst, frame; int n; char path[100]; } LiveFile;
static LiveFile *livef; static size_t nlivef, caplivef; static u64 livef_loads, livef_bytes;
/* its own lock, taken last: GetRemasteredAsset runs inside a load that already holds files_lock on the same thread (the first
 * version took files_lock here and both final route runs hung at boot, run-Q31/final-camp-d3: 0 presents, ended by the watchdog) */
static pthread_mutex_t live_lock=PTHREAD_MUTEX_INITIALIZER;
u64 kh_game_frames(void); int ga_describe(u64 a,char *out,size_t cap);
static void live_note(u64 dst,int n,const char *path){
    pthread_mutex_lock(&live_lock);
    size_t i=0; for(;i<nlivef;i++) if(livef[i].dst==dst) break;    /* a buffer handed out again replaces its old note */
    if(i==nlivef){ if(nlivef==caplivef){ size_t nc=caplivef?caplivef*2:256; LiveFile *nl=realloc(livef,nc*sizeof *nl); if(!nl){ pthread_mutex_unlock(&live_lock); return; } livef=nl; caplivef=nc; } nlivef++; }
    size_t l=strlen(path); livef[i].dst=dst; livef[i].n=n; livef[i].frame=kh_game_frames(); snprintf(livef[i].path,sizeof livef[i].path,"%s",l>=sizeof livef[i].path?path+l-(sizeof livef[i].path-1):path);
    livef_loads++; livef_bytes+=(u64)n;
    pthread_mutex_unlock(&live_lock);
}
void kh_file_live_note(u64 dst,u64 n,const char *what){ live_note(dst,n>INT_MAX?INT_MAX:(int)n,what); }   /* hd_assets.c: the HD asset buffers */
static int live_cmp(const void *a,const void *b){ const LiveFile *x=a,*y=b; return x->n<y->n?1:x->n>y->n?-1:0; }
/* the kind of a held buffer, for the totals: its directory (obj, map, menu, event, effect, ...) and whether it is an HD asset (an HD
 * asset's note is `<archive>#<index>:<name>`, a plain file's note its path) */
static void live_kind(const char *path,char *out,size_t cap){
    const char *s=strchr(path,'/'); size_t l=s?(size_t)(s-path):0; if(l==0||l>12){ snprintf(out,cap,"other"); }
    else snprintf(out,cap,"%.*s",(int)l,path);
    if(cap>8 && strlen(out)+3<cap) strcat(out,strchr(path,'#')?"/hd":"/sd");
}
/* top > 0: the `top` largest buffers by name; top < 0: every held buffer (KHNMC_IDTABLE_REPORT at exit) */
void kh_file_live_report(FILE *f,int top){
    pthread_mutex_lock(&live_lock);
    size_t k=0; u64 bytes=0;
    for(size_t i=0;i<nlivef;i++){ char d[240], want[80]; ga_describe(livef[i].dst,d,sizeof d);
        snprintf(want,sizeof want,"file block 0x%llx (%llu bytes) +0x0",(unsigned long long)livef[i].dst,(unsigned long long)(u64)livef[i].n);
        if(strstr(d,want) && !strstr(d,"FREED")){ bytes+=(u64)livef[i].n; livef[k++]=livef[i]; } }
    nlivef=k;
    /* task 31: the held bytes by kind (at most 16 kinds; more go to "other/*") */
    char kinds[16][24]; u64 kb[16]={0}; int kn=0, nk=0;
    for(size_t i=0;i<k;i++){ char kd[24]; live_kind(livef[i].path,kd,sizeof kd); int j=0; for(;j<nk;j++) if(!strcmp(kinds[j],kd)) break;
        if(j==nk){ if(nk<16){ snprintf(kinds[nk],sizeof kinds[nk],"%s",kd); nk++; } else { j=15; snprintf(kinds[15],sizeof kinds[15],"other/*"); } }
        kb[j]+=(u64)livef[i].n; }
    char by[600]; size_t bo=0; by[0]=0;
    for(int j=0;j<nk && bo<sizeof by;j++){ bo+=(size_t)snprintf(by+bo,sizeof by-bo,"%s%s %.1f",kn++?", ":"",kinds[j],(double)kb[j]/1048576.0); }
    fprintf(f,"kh: file buffers: %zu of the %llu file and HD-asset buffers handed to the game are still held by it: %llu MiB of %llu MiB handed out (MiB by kind: %s)\n",
            k,(unsigned long long)livef_loads,(unsigned long long)(bytes>>20),(unsigned long long)(livef_bytes>>20),nk?by:"none");
    if(top!=0 && k){ LiveFile *c=malloc(k*sizeof *c); if(c){ memcpy(c,livef,k*sizeof *c); qsort(c,k,sizeof *c,live_cmp);
        for(size_t i=0;i<k && (top<0 || (int)i<top);i++) fprintf(f,"kh: file buffers:   %8.1f MiB held since game frame %llu  %s\n",(double)c[i].n/1048576.0,(unsigned long long)c[i].frame,c[i].path);
        free(c); } }
    pthread_mutex_unlock(&live_lock);
}
static void load(KCtx *g,int bounded,int allocate){
    u64 path=A1(g),dst=A2(g),sizeptr=A2(g); int hd=bounded?A4(g):A3(g);
    s32 oldsize=allocate?(s32)rd32(g,sizeptr):0;
    pthread_mutex_lock(&files_lock);
    int n=0; FILE *f=open_loose(g,gstr(g,path),&n,"load"); s64 result=allocate||bounded?0:-1;
    if(!f){
        if(package_fallback(gstr(g,path))){
            if(allocate) orig_f_106870(g); else if(bounded) orig_f_106790(g); else orig_f_106700(g);
            int size=allocate?(s32)rd32(g,sizeptr):(s32)KH_RAX(g);
            int hit=allocate?(KH_RAX(g)!=0||(oldsize==-1&&size>=0)):(size>0);
            file_log(g,gstr(g,path),hit?"PACKAGE":NULL,size);
            pthread_mutex_unlock(&files_lock); return;
        }
        file_log(g,gstr(g,path),NULL,0); goto done;
    }
    if(bounded&&(s32)A3(g)<n){ result=-6; goto close; }
    if(allocate){ s32 old=rd32(g,sizeptr); wr32(g,sizeptr,n); if(old==-1) goto close;
        dst=ga_alloc_src(n,16,GA_SRC_FILE); if(!dst){ wr32(g,sizeptr,-2); goto close; } }
    if(fread(hp(g,dst),1,n,f)!=(size_t)n){
        if(allocate){ ga_free(dst); wr32(g,sizeptr,-2); } else result=-2;
        goto close;
    }
    result=allocate?dst:n;
    if(allocate) live_note(dst,n,gstr(g,path));
    if(hd&&n&&!register_resource(g,path,n,dst)){ if(allocate) ga_free(dst); result=0; }
close:
    fclose(f);
    file_log(g,gstr(g,path),"LOOSE",n);
done:
    pthread_mutex_unlock(&files_lock); SET_RAX(g,allocate?result:(u32)result); KH_RET(g);
}
void f_106700(KCtx *g){ load(g,0,0); }
void f_106790(KCtx *g){ load(g,1,0); }
void f_106870(KCtx *g){ load(g,0,1); }

/* Fixtures are synthetic and local; this also exercises hooks through the dispatch table. */
int kh_file_selftest(KCtx *g){
    char dir[KH_TMP_MAX],path[KH_TMP_MAX+256]; int bad=0,checks=0; kh_tmp_path(dir,sizeof dir,"khnmc-files-XXXXXX");   /* host_env.c: /tmp, or the app container on iOS */
    const char *oldroot=getenv("KHNMC_DATA"),*oldlog=getenv("KHNMC_FILELOG");
    char *root=oldroot?strdup(oldroot):NULL,*log=oldlog?strdup(oldlog):NULL;
    if(!mkdtemp(dir)) return 1;
    snprintf(path,sizeof path,"%s/MiXeD.bin",dir); FILE *f=fopen(path,"wb");
    if(!f) return 1; fwrite("test",1,4,f); fclose(f);
    setenv("KHNMC_DATA",dir,1); char lp[KH_TMP_MAX+256]; snprintf(lp,sizeof lp,"%s/log",dir); setenv("KHNMC_FILELOG",lp,1);
    u64 name=ga_strdup("mixed.bin"),missing=ga_strdup("missing"),dst=ga_alloc(16,16),sz=ga_alloc(16,16);
#define TEST(c) do { checks++; if(!(c)){ fprintf(stderr,"file selftest FAIL: %s\n",#c); bad++; } } while(0)
    TEST(kh_call(g,GS_BASE+0x1063d0,0,name,0,0)==4);
    char prefixed[2048]; snprintf(prefixed,sizeof prefixed,"%s\\.\\MIXED.BIN",gstr(g,GS_BASE+0x2bd4340));
    u64 windows=ga_strdup(prefixed); TEST(kh_call(g,GS_BASE+0x1063d0,0,windows,0,0)==4); ga_free(windows);
    u64 parent=ga_strdup("../MiXeD.bin"); TEST((s32)kh_call(g,GS_BASE+0x106700,0,parent,dst,0)==-1); ga_free(parent);
    wr32(g,sz,123); TEST(kh_call5(g,GS_BASE+0x106870,0,missing,sz,0,0)==0&&rd32(g,sz)==123);
    TEST((s32)kh_call(g,GS_BASE+0x106700,0,missing,dst,0)==-1);
    TEST(kh_call5(g,GS_BASE+0x106790,0,missing,dst,0,0)==0);
    TEST((s32)kh_call5(g,GS_BASE+0x106790,0,name,dst,3,0)==-6);
    TEST(kh_call(g,GS_BASE+0x106700,0,name,dst,0)==4&&!memcmp(hp(g,dst),"test",4));
    TEST(kh_call5(g,GS_BASE+0x106790,0,name,dst,4,0)==4);
    wr32(g,sz,-1); TEST(kh_call5(g,GS_BASE+0x106870,0,name,sz,0,0)==0&&rd32(g,sz)==4);
    wr32(g,sz,0); u64 p=kh_call5(g,GS_BASE+0x106870,0,name,sz,0,0); TEST(p&&!(p&15)&&rd32(g,sz)==4&&!memcmp(hp(g,p),"test",4)); ga_free(p);
    u64 obj=ga_alloc(0x260,16); TEST(kh_call(g,GS_BASE+0x106000,obj,name,0,0)==obj&&rd64(g,obj+8)&&rd32(g,obj+0x14)==0); ga_free(rd64(g,obj+8)); ga_free(obj);
    const char *oldpkg=getenv("KHNMC_PKGDIR"); char *pkg=oldpkg?strdup(oldpkg):NULL;
    char image[KH_TMP_MAX+256],mapped[2048],expected[KH_TMP_MAX+256];
    snprintf(image,sizeof image,"%s/Image",dir); mkdir(image,0700);
    snprintf(expected,sizeof expected,"%s/Image/SettingMenu.hed",dir);
    f=fopen(expected,"wb"); if(!f) return 1; fclose(f);
    setenv("KHNMC_PKGDIR",dir,1);
    TEST(kh_winpath("C:\\hd28\\STEAM\\Image\\settingmenu.HED",mapped,sizeof mapped)&&!strcmp(mapped,expected));
    TEST(kh_winpath("C:\\KH2\\image/SettingMenu.hed",mapped,sizeof mapped)&&!strcmp(mapped,expected));
    TEST(!kh_winpath("C:/hd28/STEAM/Image/../SettingMenu.hed",mapped,sizeof mapped));
    TEST(!real_package("C:/KH2/Image/SettingMenu")); /* HED alone must not select the original constructor. */
    char pkgpath[KH_TMP_MAX+256]; snprintf(pkgpath,sizeof pkgpath,"%s/Image/SettingMenu.pkg",dir);
    f=fopen(pkgpath,"wb"); if(!f) return 1; fclose(f);
    TEST(real_package("C:/KH2/Image/SettingMenu"));
    unlink(pkgpath);
    TEST(!real_package("C:/KH2/Image/SettingMenu"));
    TEST(!package_fallback("C:/hd28/STEAM/SettingMenu/WIN/uk/font/debug.inf"));
    unsigned saved_packages=real_packages; real_packages=1;
    TEST(package_fallback("C:/hd28/STEAM/SettingMenu/WIN/uk/font/debug.inf"));
    TEST(!package_fallback("../mixed.bin")&&!package_fallback("WIN\\..\\mixed.bin"));
    /* No engine registry exists in selftest: invoking fallback here would hang, so loose must win. */
    TEST(kh_call(g,GS_BASE+0x1063d0,0,name,0,0)==4);
    TEST(kh_call(g,GS_BASE+0x106700,0,name,dst,0)==4&&!memcmp(hp(g,dst),"test",4));
    TEST(kh_call5(g,GS_BASE+0x106790,0,name,dst,4,0)==4);
    wr32(g,sz,0); p=kh_call5(g,GS_BASE+0x106870,0,name,sz,0,0);
    TEST(p&&rd32(g,sz)==4&&!memcmp(hp(g,p),"test",4)); if(p) ga_free(p);
    real_packages=saved_packages;
    if(pkg) setenv("KHNMC_PKGDIR",pkg,1); else unsetenv("KHNMC_PKGDIR"); free(pkg);
    unlink(expected); rmdir(image);
    ga_free(name); ga_free(missing); ga_free(dst); ga_free(sz);
    if(root) setenv("KHNMC_DATA",root,1); else unsetenv("KHNMC_DATA");
    if(log) setenv("KHNMC_FILELOG",log,1); else unsetenv("KHNMC_FILELOG");
    free(root); free(log); unlink(path); unlink(lp); rmdir(dir);
    /* task 31: the held-buffer report: a noted `file` block is listed while it lives and forgotten when it is freed; a note taken
     * while files_lock is held does not wait for it (an asset is handed out inside a load: a regression here hangs this test) */
    { u64 b1=ga_alloc_src(3u<<20,16,GA_SRC_FILE), b2=ga_alloc_src(5000,16,GA_SRC_FILE); char l[4096]; size_t nr; FILE *tf;
      pthread_mutex_lock(&files_lock); kh_file_live_note(b1,3u<<20,"selftest/big.asset"); pthread_mutex_unlock(&files_lock); kh_file_live_note(b2,5000,"selftest/small.asset");
      tf=kh_tmpfile(); TEST(tf!=NULL);
      if(tf){ kh_file_live_report(tf,40); rewind(tf); nr=fread(l,1,sizeof l-1,tf); l[nr]=0; fclose(tf);
          TEST(strstr(l,"kh: file buffers: ") && strstr(l,"3.0 MiB held since game frame") && strstr(l,"selftest/big.asset") && strstr(l,"selftest/small.asset"));
          TEST(strstr(l,"(MiB by kind: selftest/sd 3.0)") != NULL);                                       /* the totals by kind */
          { u64 b3=ga_alloc_src(1u<<20,16,GA_SRC_FILE); kh_file_live_note(b3,1u<<20,"obj/T31.mdlx#0:-0.dds"); FILE *t3=kh_tmpfile(); TEST(t3!=NULL);
            if(t3){ kh_file_live_report(t3,1); rewind(t3); nr=fread(l,1,sizeof l-1,t3); l[nr]=0; fclose(t3);
                TEST(strstr(l,"(MiB by kind: selftest/sd 3.0, obj/hd 1.0)") && strstr(l,"selftest/big.asset") && !strstr(l,"obj/T31.mdlx"));   /* top 1: the largest only */
                t3=kh_tmpfile(); if(t3){ kh_file_live_report(t3,-1); rewind(t3); nr=fread(l,1,sizeof l-1,t3); l[nr]=0; fclose(t3);
                    TEST(strstr(l,"selftest/big.asset") && strstr(l,"selftest/small.asset") && strstr(l,"obj/T31.mdlx#0:-0.dds")); }   /* -1: all */
                t3=kh_tmpfile(); if(t3){ kh_file_live_report(t3,0); rewind(t3); nr=fread(l,1,sizeof l-1,t3); l[nr]=0; fclose(t3); TEST(!strstr(l,"held since")); } }   /* 0: the summary only */
            ga_free(b3); }
          ga_free(b1); tf=kh_tmpfile(); if(tf){ kh_file_live_report(tf,40); rewind(tf); nr=fread(l,1,sizeof l-1,tf); l[nr]=0; fclose(tf);
              TEST(!strstr(l,"selftest/big.asset") && strstr(l,"selftest/small.asset")); }
          ga_free(b2); tf=kh_tmpfile(); if(tf){ kh_file_live_report(tf,40); rewind(tf); nr=fread(l,1,sizeof l-1,tf); l[nr]=0; fclose(tf); TEST(!strstr(l,"selftest/small.asset")); } }
      else { ga_free(b1); ga_free(b2); } }
    fprintf(stderr,"kh: file selftest: %d checks, %d failures\n",checks,bad); return bad;
}
