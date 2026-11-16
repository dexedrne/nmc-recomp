/* Loose-first entry points; real packages use the untouched lifted implementation. */
#include "khnmc_rt.h"
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
    kh_phase_file(g,path);
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
static FILE *open_loose(KCtx *g,const char *path,int *size){
    char rel[2048],host[4096]; FILE *f=NULL; struct stat st;
    const char *root=getenv("KHNMC_DATA"); if(!root) root="data";
    if(relative(g,path,rel,sizeof rel)&&strlen(root)+strlen(rel)+2<sizeof host &&
       kh_resolve_ci(root,rel,host,sizeof host)){
        f=fopen(host,"rb");
        if(f&&(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||st.st_size>INT_MAX)){ fclose(f); f=NULL; }
        if(f) *size=(int)st.st_size;
    }
    return f;
}
static int register_resource(KCtx *g,u64 path,int n,u64 dst){
    /* shortcut: loose HD sidecars retain the previous policy; wire separate sidecar hooks when needed. */
    wr32(g,GS_BASE+0x79cfd4,0);
    return (s32)kh_call(g,GS_BASE+0x12f070,path,n,dst,0)!=-1;
}
void f_1063d0(KCtx *g){
    pthread_mutex_lock(&files_lock);
    int n=0; FILE *f=open_loose(g,gstr(g,A1(g)),&n);
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
static void load(KCtx *g,int bounded,int allocate){
    u64 path=A1(g),dst=A2(g),sizeptr=A2(g); int hd=bounded?A4(g):A3(g);
    s32 oldsize=allocate?(s32)rd32(g,sizeptr):0;
    pthread_mutex_lock(&files_lock);
    int n=0; FILE *f=open_loose(g,gstr(g,path),&n); s64 result=allocate||bounded?0:-1;
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
        dst=ga_alloc(n,16); if(!dst){ wr32(g,sizeptr,-2); goto close; } }
    if(fread(hp(g,dst),1,n,f)!=(size_t)n){
        if(allocate){ ga_free(dst); wr32(g,sizeptr,-2); } else result=-2;
        goto close;
    }
    result=allocate?dst:n;
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
    char dir[]="/tmp/khnmc-files-XXXXXX",path[256]; int bad=0,checks=0;
    const char *oldroot=getenv("KHNMC_DATA"),*oldlog=getenv("KHNMC_FILELOG");
    char *root=oldroot?strdup(oldroot):NULL,*log=oldlog?strdup(oldlog):NULL;
    if(!mkdtemp(dir)) return 1;
    snprintf(path,sizeof path,"%s/MiXeD.bin",dir); FILE *f=fopen(path,"wb");
    if(!f) return 1; fwrite("test",1,4,f); fclose(f);
    setenv("KHNMC_DATA",dir,1); char lp[256]; snprintf(lp,sizeof lp,"%s/log",dir); setenv("KHNMC_FILELOG",lp,1);
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
    char image[256],mapped[2048],expected[256];
    snprintf(image,sizeof image,"%s/Image",dir); mkdir(image,0700);
    snprintf(expected,sizeof expected,"%s/Image/SettingMenu.hed",dir);
    f=fopen(expected,"wb"); if(!f) return 1; fclose(f);
    setenv("KHNMC_PKGDIR",dir,1);
    TEST(kh_winpath("C:\\hd28\\STEAM\\Image\\settingmenu.HED",mapped,sizeof mapped)&&!strcmp(mapped,expected));
    TEST(kh_winpath("C:\\KH2\\image/SettingMenu.hed",mapped,sizeof mapped)&&!strcmp(mapped,expected));
    TEST(!kh_winpath("C:/hd28/STEAM/Image/../SettingMenu.hed",mapped,sizeof mapped));
    TEST(!real_package("C:/KH2/Image/SettingMenu")); /* HED alone must not select the original constructor. */
    char pkgpath[256]; snprintf(pkgpath,sizeof pkgpath,"%s/Image/SettingMenu.pkg",dir);
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
    fprintf(stderr,"kh: file selftest: %d checks, %d failures\n",checks,bad); return bad;
}
