/* host_env.c : what the HOST PROCESS gives the runtime, and the one switch that selects how an iOS app gets it (task 26).
 *
 * KH_IOS_PATHS (compile time; runtime/kh_compat.h): 1 when the target OS is iOS, 0 elsewhere, and it can be forced to 1 on Linux
 * and macOS (`make IOS_PATHS=1`) so that every line an iPhone build runs is compiled and executed on a desk machine first.
 * With KH_IOS_PATHS=0 nothing in this file changes what the program did before it existed.
 *
 *                         KH_IOS_PATHS=0 (Linux, macOS)                  KH_IOS_PATHS=1 (iOS; forced: any host)
 *  temporary files        $KH_TMPDIR, else "/tmp"                        $KH_TMPDIR, else $TMPDIR, else the named fatal `tmpdir-unset`
 *                         tmpfile()                                      an unlinked mkstemp file in that directory
 *  child processes        fork(): the selftests' fatal cases run         no reference to fork at all: every such case is NOT RUN,
 *                         in a child                                     one loud line each with the number of checks, and a total
 *  directories            the caller's: working directory, ./docs,       the app container (section 4): Documents for what the user
 *                         ./data, KH_* variables                         brings and sees, Library/Application Support for the rest
 *  arguments, environment the caller's command line and environment      + Documents/khnmc.conf (an app has no caller)
 *  exit status            the caller reads it                            + written to a status file (the conf names it)
 *  resident-memory report mach_vm_page_range_query (macOS), mincore      mincore (runtime/core.c; <mach/mach_vm.h> is not for iOS)
 *
 * system() / popen() / exec*: the runtime never used them (tools/check_ios_symbols.py refuses a KH_IOS_PATHS binary that imports
 * one of them, fork, vfork, posix_spawn, tmpfile, tmpnam or mktemp).
 *
 * 1. Temporary files.  The selftests build their fixtures in a scratch directory.  It was a hard-coded "/tmp"; an iOS app can only
 *    write inside its container.  No usable directory is a named fatal, never a silent fallback to a path that cannot be written.
 * 2. Child processes.  About forty selftest cases prove a NAMED FATAL: they run the failing call in a forked child and read its
 *    exit status and its stderr.  An iOS app has no fork(), and these cases cannot simply run in-process: the child ends in _exit()
 *    in the middle of lifted code, some after deliberately corrupting image bytes or guest structures, and the fork is what throws
 *    that state away.  So where there is no fork the case is NOT RUN and says so (kh_selftest_not_run: one LOUD line per case with
 *    the number of checks that did not execute; kh_selftest_not_run_report: the total, at the end of the selftest).  A suite's own
 *    "N checks" line counts executed checks only, so in such a build it is smaller by exactly the reported number.
 * 3. The end of the process.  The runtime ends with _exit(status) from several places (normal exit, named fatal 3, signal 4,
 *    dispatch fault 5).  A command-line caller reads the status; an app has no caller.  kh_host_exit(status) = kh_exit_hook (when
 *    set), then _exit.  The hook must be async-signal-safe (it also runs from the signal report).  No hook: plain _exit, as before.
 * 4. The app container (KH_IOS_PATHS only): kh_host_env_init(), called first thing in main().
 */
#include "khnmc_rt.h"
#include "gfx_pipecache.h"                              /* the host env selftest saves a cache file under @CACHES@ (review C43 n=6) */
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
static int file_touch(const char *p){ int fd=open(p,O_WRONLY|O_CREAT,0644); if(fd<0) return 0; close(fd); return 1; }

/* ---- 1. temporary files -------------------------------------------------------------------------------------------- */
const char *kh_tmpdir(void){
    static char dir[KH_TMP_MAX]; static pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if(!dir[0]){
        const char *e=getenv("KH_TMPDIR"), *why="KH_TMPDIR";
#if KH_IOS_PATHS
        if(!e||!*e){ e=getenv("TMPDIR"); why="TMPDIR"; }
        if(!e||!*e){ pthread_mutex_unlock(&m); kh_fatal(NULL,"tmpdir-unset: neither KH_TMPDIR nor TMPDIR is set; a KH_IOS_PATHS build writes temporary files only where the host says (an iOS app: inside its container, the app shell passes NSTemporaryDirectory())"); }
#else
        if(!e||!*e){ e="/tmp"; why="default"; }
#endif
        size_t n=strlen(e); while(n>1&&e[n-1]=='/') n--;                    /* iOS hands out ".../tmp/" */
        if(n+KH_TMP_LEAF_MAX>=sizeof dir){ pthread_mutex_unlock(&m); kh_fatal(NULL,"tmpdir-too-long: %s=%s (%zu bytes; the limit is %zu)",why,e,n,sizeof dir-KH_TMP_LEAF_MAX-1); }
        memcpy(dir,e,n); dir[n]=0;
        struct stat st; if(stat(dir,&st)||!S_ISDIR(st.st_mode)){ char bad[KH_TMP_MAX]; snprintf(bad,sizeof bad,"%s",dir); dir[0]=0; pthread_mutex_unlock(&m); kh_fatal(NULL,"tmpdir-missing: %s=%s is not a directory",why,bad); }
    }
    pthread_mutex_unlock(&m);
    return dir;
}
/* out = "<tmpdir>/<leaf>" (leaf: a file name or a mkstemp / mkdtemp template, at most KH_TMP_LEAF_MAX-1 bytes); returns out */
char *kh_tmp_path(char *out,size_t cap,const char *leaf){
    const char *d=kh_tmpdir(); size_t n=strlen(d), l=strlen(leaf);
    if(l>=KH_TMP_LEAF_MAX||n+1+l+1>cap) kh_fatal(NULL,"tmp-path-too-long: %s/%s does not fit %zu bytes",d,leaf,cap);
    memcpy(out,d,n); out[n]='/'; memcpy(out+n+1,leaf,l+1); return out;
}
/* an anonymous read-write temporary file (the capture file of a fatal case).  tmpfile() as before; with KH_IOS_PATHS, where
 * tmpfile() looks outside the container (P_tmpdir), an unlinked mkstemp file in kh_tmpdir(). */
FILE *kh_tmpfile(void){
#if KH_IOS_PATHS
    char p[KH_TMP_MAX]; kh_tmp_path(p,sizeof p,"khnmc-capture-XXXXXX");
    int fd=mkstemp(p); if(fd<0) return NULL; unlink(p);
    FILE *f=fdopen(fd,"w+"); if(!f) close(fd); return f;
#else
    return tmpfile();
#endif
}

/* ---- 2. child processes --------------------------------------------------------------------------------------------- */
static int not_run_cases, not_run_checks;
int kh_fork_ok(void){ return !KH_IOS_PATHS; }
pid_t kh_fork(void){
#if KH_IOS_PATHS
    errno=ENOSYS; return -1;                /* no reference to fork in this build; callers ask kh_fork_ok() first */
#else
    return fork();
#endif
}
void kh_selftest_not_run(const char *suite,const char *what,int checks){
    not_run_cases++; not_run_checks+=checks;
    fprintf(stderr,"kh: selftest: NOT RUN (KH_IOS_PATHS: no child process): %s: %s: %d check%s not executed\n",suite,what,checks,checks==1?"":"s");
}
int kh_selftest_not_run_report(FILE *f){
    if(not_run_checks||not_run_cases)
        fprintf(f,"kh: selftest: NOT RUN: %d checks in %d fatal-path cases were not executed (they need fork(), see the NOT RUN lines above); every suite line above counts executed checks only\n",not_run_checks,not_run_cases);
    return not_run_checks;
}

/* ---- 3. the end of the process -------------------------------------------------------------------------------------- */
void (*kh_exit_hook)(int status);
void kh_host_exit(int status){
    void (*h)(int)=kh_exit_hook; if(h) h(status);
    _exit(status);
}

/* ---- 4. the app container (KH_IOS_PATHS) ---------------------------------------------------------------------------- */
#if !KH_IOS_PATHS
void kh_host_env_init(int *argc,char ***argv){ (void)argc; (void)argv; }
int kh_host_env_selftest(void){ return 0; }
const char *kh_autosave_documents(void){ return kh_docs; }
const char *kh_host_caches_dir(void){ return ""; }               /* TASK43: no app container: no default cache file (KHNMC_GFX_PIPECACHE names one) */
#else
/* Layout, all inside the app's home (the container on iOS; KH_APP_HOME names it, and it is REQUIRED when the switch is forced on a
 * desk machine, so that a test never writes into a person's real home):
 *   <home>/Documents/                         what the user brings and sees (Files app / Finder file sharing)
 *       khnmc.conf                            optional: the app's "command line" (below)
 *       kh2fm.exe                             KH_EXE default
 *       data/                                 KHNMC_DATA default: the loose data tree
 *       pkgdir/                               KHNMC_PKGDIR default, only when the directory exists
 *       Saves/                                KH_DOCS default: the game's own Documents folder (saves, its config); created
 *   <home>/Library/Application Support/khnmc/
 *       root/                                 KH_ROOT default: the game's working directory; created, and the process chdir()s there
 *   temporary files                           kh_tmpdir(): $KH_TMPDIR, else $TMPDIR (iOS sets it to the container's tmp/)
 * Every default yields to a variable that is already set, in the environment or by the conf.
 *
 * Documents/khnmc.conf: one directive per line, `#` starts a comment line, blank lines are skipped:
 *   env NAME=VALUE     set a variable (overrides the process environment: the file is authoritative, an app has no other caller)
 *   arg TEXT           append one command-line argument
 *   log NAME           stdout and stderr go to this file (truncated at start); relative names are inside Documents
 *   status NAME        "running" is written there at start and "exit N" right before the process ends; relative: inside Documents
 *   logcap SIZE        (TASK43) the log file of the `log` line before it is size-bounded: SIZE bytes (N, NK or NM; 4096 .. 1024M) per file.
 *                      When the file reaches it, it becomes NAME.1 (replacing an older NAME.1) and a new NAME starts: at most two files,
 *                      2 x SIZE plus what is written in one check period (section 5 below).  Without the line the log grows as before.
 * In VALUE, TEXT and NAME: @DOCS@ = <home>/Documents, @SUPPORT@ = <home>/Library/Application Support/khnmc, @HOME@, @TMP@,
 * @CACHES@ = <home>/Library/Caches/khnmc (TASK43: what may be lost, e.g. the disk pipeline cache; not in the backup, not in the Files view).
 * Anything else is the named fatal `host-conf` with the file and line: a typo must not become a silently different run. */
#define HE_MAXARG 64
static char he_home[KH_TMP_MAX], he_docs[KH_TMP_MAX+32], he_support[KH_TMP_MAX+64], he_status[KH_TMP_MAX+320], he_caches[KH_TMP_MAX+64];
const char *kh_autosave_documents(void){ return *he_docs ? he_docs : kh_docs; }
static void he_mkdirs(const char *path);
/* TASK43: <home>/Library/Caches/khnmc, made at its first use ("" when it cannot be made: the caller then has no default file there) */
const char *kh_host_caches_dir(void){
    static int made; if(!he_caches[0]) return "";
    if(!made){ he_mkdirs(he_caches); struct stat st; if(stat(he_caches,&st)||!S_ISDIR(st.st_mode)){ fprintf(stderr,"kh: host dirs: the caches directory %s cannot be created (%s): nothing is kept there\n",he_caches,strerror(errno)); return ""; } made=1; }
    return he_caches; }
static int he_logcap_parse(const char *a,uint64_t *bytes);
static void he_logcap_start(const char *path,uint64_t cap);
static char he_log[KH_TMP_MAX+420];
static char *he_argv[HE_MAXARG+2]; static int he_argc;

static void he_mkdirs(const char *path){                       /* mkdir -p; the caller checks the result with stat */
    char b[KH_TMP_MAX+400]; size_t n=strlen(path); if(n>=sizeof b) return; memcpy(b,path,n+1);
    for(char *p=b+1;*p;p++) if(*p=='/'){ *p=0; mkdir(b,0755); *p='/'; }
    mkdir(b,0755);
}
static int he_isdir(const char *p){ struct stat st; return !stat(p,&st)&&S_ISDIR(st.st_mode); }
/* expand the four tokens; returns 0 when the result does not fit */
static int he_expand(const char *in,char *out,size_t cap){
    size_t o=0;
    while(*in){
        const char *rep=NULL; size_t skip=0;
        if(*in=='@'){
            if(!strncmp(in,"@DOCS@",6)){ rep=he_docs; skip=6; } else if(!strncmp(in,"@SUPPORT@",9)){ rep=he_support; skip=9; }
            else if(!strncmp(in,"@HOME@",6)){ rep=he_home; skip=6; } else if(!strncmp(in,"@TMP@",5)){ rep=kh_tmpdir(); skip=5; }
            else if(!strncmp(in,"@CACHES@",8)){ rep=he_caches; skip=8; }
        }
        if(rep){ size_t l=strlen(rep); if(o+l>=cap) return 0; memcpy(out+o,rep,l); o+=l; in+=skip; }
        else { if(o+1>=cap) return 0; out[o++]=*in++; }
    }
    out[o]=0; return 1;
}
/* a NAME of `log` / `status`: expanded; relative = inside Documents */
static int he_file(const char *name,char *out,size_t cap){
    char e[KH_TMP_MAX+300]; if(!he_expand(name,e,sizeof e)||!e[0]) return 0;
    return (e[0]=='/'?snprintf(out,cap,"%s",e):snprintf(out,cap,"%s/%s",he_docs,e))<(int)cap;
}
static void he_status_hook(int status){                         /* async-signal-safe: open, write, close */
    char b[24]; int n=0; unsigned v=(unsigned)(status<0?-status:status); char d[12]; int k=0;
    memcpy(b,"exit ",5); n=5; if(status<0) b[n++]='-';
    do{ d[k++]=(char)('0'+v%10); v/=10; }while(v); while(k) b[n++]=d[--k]; b[n++]='\n';
    int fd=open(he_status,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){ if(write(fd,b,(size_t)n)<0){} close(fd); }
}
/* one conf file.  apply=0: syntax check only (the selftest); returns the number of directives; *err gets the first problem */
static int he_conf(const char *path,int apply,char *err,size_t errcap){
    FILE *f=fopen(path,"r"); if(!f){ snprintf(err,errcap,"cannot read %s",path); return -1; }
    char line[2048], val[KH_TMP_MAX+400]; int ln=0, n=0, have_log=0; err[0]=0;
    while(fgets(line,sizeof line,f)){
        ln++; size_t l=strlen(line);
        if(l==sizeof line-1&&line[l-1]!='\n'){ snprintf(err,errcap,"%s:%d: line longer than %zu bytes",path,ln,sizeof line-2); break; }
        while(l&&(line[l-1]=='\n'||line[l-1]=='\r'||line[l-1]==' '||line[l-1]=='\t')) line[--l]=0;
        char *p=line; while(*p==' '||*p=='\t') p++;
        if(!*p||*p=='#') continue;
        char *a=p; while(*a&&*a!=' '&&*a!='\t') a++; size_t kw=(size_t)(a-p); while(*a==' '||*a=='\t') a++;
        if(!*a){ snprintf(err,errcap,"%s:%d: directive without a value: %s",path,ln,p); break; }
        if(kw==3&&!strncmp(p,"env",3)){
            char *eq=strchr(a,'='); if(!eq||eq==a||memchr(a,' ',(size_t)(eq-a))){ snprintf(err,errcap,"%s:%d: env needs NAME=VALUE: %s",path,ln,a); break; }
            if(!he_expand(eq+1,val,sizeof val)){ snprintf(err,errcap,"%s:%d: value too long after expansion",path,ln); break; }
            if(apply){ *eq=0; setenv(a,val,1); fprintf(stderr,"kh: host conf: env %s=%s\n",a,val); *eq='='; }
        } else if(kw==3&&!strncmp(p,"arg",3)){
            if(!he_expand(a,val,sizeof val)){ snprintf(err,errcap,"%s:%d: argument too long after expansion",path,ln); break; }
            if(apply){ if(he_argc>=HE_MAXARG){ snprintf(err,errcap,"%s:%d: more than %d arguments",path,ln,HE_MAXARG); break; }
                       he_argv[he_argc++]=strdup(val); fprintf(stderr,"kh: host conf: arg %s\n",val); }
        } else if(kw==3&&!strncmp(p,"log",3)){
            if(!he_file(a,val,sizeof val)){ snprintf(err,errcap,"%s:%d: bad log file name",path,ln); break; }
            if(apply){ fflush(stdout); fflush(stderr);
                       if(!freopen(val,"w",stderr)){ snprintf(err,errcap,"%s:%d: cannot write the log file %s",path,ln,val); break; }
                       setvbuf(stderr,NULL,_IOLBF,0); if(dup2(fileno(stderr),STDOUT_FILENO)<0){} fprintf(stderr,"kh: host conf: log %s\n",val);
                       snprintf(he_log,sizeof he_log,"%s",val); }
            have_log=1;
        } else if(kw==6&&!strncmp(p,"logcap",6)){                      /* TASK43: the size cap of that log file (section 5) */
            uint64_t cap=0;
            if(!have_log){ snprintf(err,errcap,"%s:%d: logcap needs a log directive before it",path,ln); break; }
            if(!he_logcap_parse(a,&cap)){ snprintf(err,errcap,"%s:%d: logcap needs a size of 4096 bytes to 1024M (N, NK or NM): %s",path,ln,a); break; }
            if(apply){ fprintf(stderr,"kh: host conf: logcap %llu bytes\n",(unsigned long long)cap); he_logcap_start(he_log,cap); }
        } else if(kw==6&&!strncmp(p,"status",6)){
            if(!he_file(a,val,sizeof val)||strlen(val)>=sizeof he_status){ snprintf(err,errcap,"%s:%d: bad status file name",path,ln); break; }
            if(apply){ memcpy(he_status,val,strlen(val)+1);
                       int fd=open(he_status,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){ snprintf(err,errcap,"%s:%d: cannot write the status file %s",path,ln,val); he_status[0]=0; break; }
                       if(write(fd,"running\n",8)<0){} close(fd); kh_exit_hook=he_status_hook; fprintf(stderr,"kh: host conf: status %s\n",val); }
        } else { snprintf(err,errcap,"%s:%d: unknown directive: %s",path,ln,p); break; }
        n++;
    }
    fclose(f); return err[0]?-1:n;
}
static void he_default(const char *name,const char *value,int only_if_dir){
    const char *cur=getenv(name);
    if(cur&&*cur){ fprintf(stderr,"kh: host dirs: %s=%s (set by the caller or the conf)\n",name,cur); return; }
    if(only_if_dir&&!he_isdir(value)){ fprintf(stderr,"kh: host dirs: %s stays unset (%s is not there)\n",name,value); return; }
    setenv(name,value,1); fprintf(stderr,"kh: host dirs: %s=%s (app container default)\n",name,value);
}
void kh_host_env_init(int *argc,char ***argv){
    const char *h=getenv("KH_APP_HOME"), *why="KH_APP_HOME";
#if KH_IOS
    if(!h||!*h){ h=getenv("HOME"); why="HOME"; }                /* the container; the app shell passes NSHomeDirectory() as KH_APP_HOME */
    if(!h||!*h) kh_fatal(NULL,"app-home-unset: neither KH_APP_HOME nor HOME is set");
#else
    if(!h||!*h) kh_fatal(NULL,"app-home-unset: KH_APP_HOME is not set.  This build has KH_IOS_PATHS forced on a host that is not iOS: it takes its directories from an app container, and it must be told which directory plays that part (it never uses your real home)");
#endif
    size_t n=strlen(h); while(n>1&&h[n-1]=='/') n--;
    if(n>=sizeof he_home-1) kh_fatal(NULL,"app-home-too-long: %s=%s",why,h);
    memcpy(he_home,h,n); he_home[n]=0;
    if(!he_isdir(he_home)) kh_fatal(NULL,"app-home-missing: %s=%s is not a directory",why,he_home);
    snprintf(he_docs,sizeof he_docs,"%s/Documents",he_home);
    snprintf(he_support,sizeof he_support,"%s/Library/Application Support/khnmc",he_home);
    snprintf(he_caches,sizeof he_caches,"%s/Library/Caches/khnmc",he_home);                 /* TASK43: made at its first use (kh_host_caches_dir) */
    char root[KH_TMP_MAX+96], saves[KH_TMP_MAX+64], p[KH_TMP_MAX+96], err[KH_TMP_MAX+600];
    snprintf(root,sizeof root,"%s/root",he_support); snprintf(saves,sizeof saves,"%s/Saves",he_docs);
    he_mkdirs(he_docs); he_mkdirs(root);
    if(!he_isdir(he_docs)||!he_isdir(root)) kh_fatal(NULL,"app-home-not-writable: cannot create %s and %s",he_docs,root);
    /* the conf first: its `log` moves every later line into the log file, its `env` lines win over the defaults below */
    he_argc=0; for(int i=0;i<*argc&&he_argc<HE_MAXARG;i++) he_argv[he_argc++]=(*argv)[i];
    snprintf(p,sizeof p,"%s/khnmc.conf",he_docs);
    if(!access(p,F_OK)){
        int nd=he_conf(p,1,err,sizeof err);
        if(nd<0) kh_fatal(NULL,"host-conf: %s",err);
        fprintf(stderr,"kh: host conf: %s: %d directives\n",p,nd);
    } else fprintf(stderr,"kh: host conf: no %s (arguments and variables are the caller's)\n",p);
    he_argv[he_argc]=NULL; *argc=he_argc; *argv=he_argv;
    fprintf(stderr,"kh: host dirs: KH_IOS_PATHS build%s: home %s (%s)\n",KH_IOS?"":" (FORCED on a host that is not iOS)",he_home,why);
    snprintf(p,sizeof p,"%s/kh2fm.exe",he_docs); he_default("KH_EXE",p,0);
    he_default("KH_ROOT",root,0); he_default("KH_DOCS",saves,0);
    snprintf(p,sizeof p,"%s/data",he_docs); he_default("KHNMC_DATA",p,0);
    snprintf(p,sizeof p,"%s/looks/xion",he_docs); he_default("KHNMC_LOOK_DIR",p,0);
    snprintf(p,sizeof p,"%s/pkgdir",he_docs); he_default("KHNMC_PKGDIR",p,1);
    fprintf(stderr,"kh: host dirs: temporary files in %s\n",kh_tmpdir());
    const char *wd=getenv("KH_ROOT"); he_mkdirs(wd);
    if(chdir(wd)) kh_fatal(NULL,"app-home-not-writable: cannot enter the working directory %s",wd);
    /* the game's Documents folder: the game creates only its own sub-folders in it, so it must exist (a test run gives a fresh
     * one under @TMP@ through khnmc.conf: tools/ios/mac/device_run.sh route; the game's config there changes how it boots) */
    const char *gd=getenv("KH_DOCS"); struct stat gst; he_mkdirs(gd);
    if(stat(gd,&gst)||!S_ISDIR(gst.st_mode)) kh_fatal(NULL,"app-docs-not-writable: cannot create the game's Documents folder %s",gd);
    if(access(gd,W_OK)) kh_fatal(NULL,"app-docs-not-writable: the game's Documents folder %s exists but cannot be written (%s)",gd,strerror(errno));
    fprintf(stderr,"kh: host dirs: working directory %s, game Documents %s\n",wd,gd);
}

/* selftest of this section (exists only in a KH_IOS_PATHS build): expansion, the conf syntax, the status hook */
int kh_host_env_selftest(void){
    int checks=0, fails=0;
#define T(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"host env selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    char out[KH_TMP_MAX+400], want[KH_TMP_MAX+400], err[KH_TMP_MAX+600], dir[KH_TMP_MAX], p[KH_TMP_MAX+64];
    T(he_isdir(he_docs)); T(he_isdir(he_support)); T(getenv("KH_ROOT")&&he_isdir(getenv("KH_ROOT")));
    { char cwd[KH_TMP_MAX+400]; char real[KH_TMP_MAX+400]; T(getcwd(cwd,sizeof cwd)&&realpath(getenv("KH_ROOT"),real)&&!strcmp(cwd,real)); }
    T(he_expand("a@DOCS@/b",out,sizeof out)); snprintf(want,sizeof want,"a%s/b",he_docs); T(!strcmp(out,want));
    T(he_expand("@SUPPORT@|@HOME@|@TMP@",out,sizeof out)); snprintf(want,sizeof want,"%s|%s|%s",he_support,he_home,kh_tmpdir()); T(!strcmp(out,want));
    T(he_expand("@NOPE@ x@y @DOCS",out,sizeof out)&&!strcmp(out,"@NOPE@ x@y @DOCS"));            /* unknown tokens stay text */
    T(he_expand("@CACHES@/pipe.bin",out,sizeof out)); snprintf(want,sizeof want,"%s/Library/Caches/khnmc/pipe.bin",he_home); T(!strcmp(out,want));   /* TASK43 */
    T(strstr(he_caches,"/Documents")==NULL && !strncmp(he_caches,he_home,strlen(he_home)));         /* the caches directory is never inside Documents */
    { const char *cd=kh_host_caches_dir(); T(!strcmp(cd,he_caches) && he_isdir(he_caches)); }
    /* review C43 n=6: a pipeline-cache file NAMED under @CACHES@ (what the token is for), in a directory nothing has made: the save makes it */
    { char nm[KH_TMP_MAX+400], d1[KH_TMP_MAX+400], d2[KH_TMP_MAX+400], why[700]; KhPcDev dv; memset(&dv,0,sizeof dv); static const unsigned char pay[5]={1,2,3,4,5};
      snprintf(want,sizeof want,"@CACHES@/st-named-%ld/sub/x.bin",(long)getpid()); T(he_expand(want,nm,sizeof nm));
      snprintf(d1,sizeof d1,"%s/st-named-%ld",he_caches,(long)getpid()); snprintf(d2,sizeof d2,"%s/sub",d1);
      T(!he_isdir(d1) && kh_pc_file_save(nm,&dv,pay,sizeof pay,why,sizeof why)==0 && he_isdir(d2) && !access(nm,R_OK));
      unlink(nm); rmdir(d2); T(rmdir(d1)==0); }
    T(!he_expand("@DOCS@",out,4));                                                                 /* does not fit: refused, not cut */
    T(he_file("x.log",out,sizeof out)); snprintf(want,sizeof want,"%s/x.log",he_docs); T(!strcmp(out,want));
    T(he_file("@TMP@/x.log",out,sizeof out)); snprintf(want,sizeof want,"%s/x.log",kh_tmpdir()); T(!strcmp(out,want));
    kh_tmp_path(dir,sizeof dir,"khnmc-hostenv-XXXXXX"); T(mkdtemp(dir)!=NULL);
    T(!strncmp(dir,kh_tmpdir(),strlen(kh_tmpdir()))&&dir[strlen(kh_tmpdir())]=='/');                            /* fixtures follow the host's directory */
    snprintf(p,sizeof p,"%s/c.conf",dir);
    static const struct { const char *text; int n; const char *err; } cases[]={
        {"# comment\n\n  env A=1\nenv B=@DOCS@/x y\narg --selftest\nlog run.log\nstatus run.status\n",5,NULL},
        {"env A=1\r\narg x\r\n",2,NULL},                                    /* a file edited on Windows */
        {"",0,NULL},
        {"env A\n",-1,":1: env needs NAME=VALUE"},
        {"env =1\n",-1,":1: env needs NAME=VALUE"},
        {"env A B=1\n",-1,":1: env needs NAME=VALUE"},
        {"arg ok\nenvv A=1\n",-1,":2: unknown directive: envv A=1"},
        {"arg\n",-1,":1: directive without a value"},
        {"log\n",-1,":1: directive without a value"},
        {"KH_EXE=/x\n",-1,":1: directive without a value"},                 /* a bare assignment is not a directive */
        {"set A=1\n",-1,":1: unknown directive"},
        {"log a.log\nlogcap 16M\n",2,NULL},                                  /* TASK43 */
        {"log a.log\nlogcap 4096\nstatus s\n",3,NULL},
        {"logcap 16M\n",-1,":1: logcap needs a log directive before it"},
        {"log a.log\nlogcap 12\n",-1,":2: logcap needs a size"},
        {"log a.log\nlogcap 2G\n",-1,":2: logcap needs a size"},
        {"log a.log\nlogcap\n",-1,":2: directive without a value"},
    };
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++){
        FILE *f=fopen(p,"w"); T(f!=NULL); if(!f) continue; fputs(cases[i].text,f); fclose(f);
        int n=he_conf(p,0,err,sizeof err); T(n==cases[i].n);
        if(cases[i].err) T(strstr(err,cases[i].err)!=NULL); else T(!err[0]);
    }
    { FILE *f=fopen(p,"w"); T(f!=NULL); if(f){ fputs("arg ",f); for(int i=0;i<3000;i++) fputc('x',f); fputc('\n',f); fclose(f); T(he_conf(p,0,err,sizeof err)==-1&&strstr(err,"line longer than")); } }
    unlink(p); snprintf(p,sizeof p,"%s/none.conf",dir); T(he_conf(p,0,err,sizeof err)==-1&&strstr(err,"cannot read"));
    /* the status hook writes "exit N" (the run's own status file, when the conf named one, is put back) */
    { char keep[sizeof he_status]; memcpy(keep,he_status,sizeof keep); snprintf(he_status,sizeof he_status,"%s/st",dir);
      static const struct { int st; const char *want; } sc[]={{0,"exit 0\n"},{3,"exit 3\n"},{124,"exit 124\n"},{-7,"exit -7\n"}};
      for(unsigned i=0;i<4;i++){ he_status_hook(sc[i].st); char b[32]={0}; FILE *f=fopen(he_status,"r"); T(f!=NULL); if(f){ size_t r=fread(b,1,sizeof b-1,f); b[r]=0; fclose(f); } T(!strcmp(b,sc[i].want)); }
      unlink(he_status); memcpy(he_status,keep,sizeof keep); }
    /* the anonymous capture file is a real read-write file in the host's directory, and there is no child process here */
    { FILE *f=kh_tmpfile(); T(f!=NULL); if(f){ T(fputs("abc",f)>=0); rewind(f); char b[8]={0}; T(fread(b,1,3,f)==3&&!strcmp(b,"abc")); fclose(f); } }
    T(!kh_fork_ok()); { errno=0; T(kh_fork()==-1&&errno==ENOSYS); }
    T(rmdir(dir)==0);                                                                              /* nothing of the fixtures is left */
#undef T
    fprintf(stderr,"kh: host env selftest: %d checks, %d failures (KH_IOS_PATHS: %s; home %s)\n",checks,fails,KH_IOS?"iOS":"forced on a host that is not iOS",he_home);
    return fails;
}
#endif

/* ---- 5. the log file's size cap (TASK43; every build has the code, only the conf's `logcap` line of an app-container build uses it) ----
 * In play mode the app's log is written for hours (one line per presented frame alone is about 30 MB per hour).  `logcap SIZE` bounds it:
 * a watcher thread looks at the file's size every KH_LOGCAP_PERIOD_MS; at SIZE or more the file is renamed to NAME.1 (an older NAME.1 is
 * replaced: one generation is kept), a new NAME is opened and put behind the SAME descriptors (dup2 onto stderr's and stdout's), so every
 * writer of the process goes on without knowing.  The last line of the old file and the first line of the new one name the rotation
 * (`kh: host log: log-rotated`), with its number.  If the rename or the new file fails, the file is emptied in place instead
 * (`log-rotate-failed`, counted): the bound holds either way.  On disk: at most 2 x SIZE plus what one period writes. */
#define KH_LOGCAP_PERIOD_MS 2000u
typedef struct { char path[KH_TMP_MAX+420]; uint64_t cap; int fd_a, fd_b; FILE *stream; uint64_t rotations, failed, bytes_out, recreated, unread, split, healed; } KhLogCap;
static int he_logcap_parse(const char *a,uint64_t *bytes){
    if(!a||*a<'0'||*a>'9') return 0; char *e; unsigned long long v=strtoull(a,&e,10);
    if(*e=='K'&&!e[1]){ if(v>(1ull<<20)) return 0; v<<=10; } else if(*e=='M'&&!e[1]){ if(v>1024) return 0; v<<=20; } else if(*e) return 0;
    if(v<4096||v>(1ull<<30)) return 0; *bytes=v; return 1; }
static void lc_line(int fd,const char *fmt,...){ char b[KH_TMP_MAX*2+900]; va_list ap; va_start(ap,fmt); int n=vsnprintf(b,sizeof b,fmt,ap); va_end(ap);
    if(n>(int)sizeof b-1) n=(int)sizeof b-1; if(n>0&&write(fd,b,(size_t)n)<0){} }
/* review C43 n=10: the size of the log cannot be read: counted, said the first three times, the cap is not checked this time */
static int lc_unread(KhLogCap *lc,int e){ if(lc->unread++<3) lc_line(lc->fd_a,"kh: host log: log-size-unread: the size of %s cannot be read (fstat: %s): the cap is not checked this time (counted)\n",lc->path,strerror(e)); return -2; }
/* 1 = rotated (or NAME, which was not there, was made anew: counted apart); 0 = below the cap; -1 = the rotation failed and the file
 * was emptied in place; -2 = the file's size cannot be read (counted).
 * THE ORDER (review C43 n=10): the new file is opened FIRST, under NAME.new.  If that fails nothing has been renamed: NAME is still the
 * log (emptied in place), and NAME.1 still holds the generation before.  Then NAME -> NAME.1, NAME.new -> NAME, and both descriptors
 * onto the new file; a step that fails puts the old name back.  A NAME that is not there at a rotation (removed, or lost by a failure)
 * is made anew instead of failing for good.  A second descriptor that could not be moved is moved at the next check. */
static int kh_log_rotate_if_over(KhLogCap *lc){
    struct stat st, sb; if(fstat(lc->fd_a,&st)) return lc_unread(lc,errno);
    if(lc->fd_b>=0&&!fstat(lc->fd_b,&sb)&&(sb.st_dev!=st.st_dev||sb.st_ino!=st.st_ino)){        /* the second descriptor writes into another file: a rotation could not move it */
        if(dup2(lc->fd_a,lc->fd_b)>=0){ lc->healed++; lc_line(lc->fd_a,"kh: host log: the log's second descriptor was moved to %s now (a rotation before could not move it: counted)\n",lc->path); }
        else { int e2=errno; lc->split++; if(ftruncate(lc->fd_b,0)){} if(lseek(lc->fd_b,0,SEEK_SET)<0){}
            lc_line(lc->fd_a,"kh: host log: log-descriptor-split: the log's second descriptor still cannot be moved to %s (dup2: %s): the file it writes into was emptied in place (counted)\n",lc->path,strerror(e2)); } }
    if((uint64_t)st.st_size<lc->cap) return 0;
    char old[sizeof lc->path+8], nw[sizeof lc->path+8]; snprintf(old,sizeof old,"%s.1",lc->path); snprintf(nw,sizeof nw,"%s.new",lc->path);
    if(lc->stream){ flockfile(lc->stream); fflush(lc->stream); }
    if(fstat(lc->fd_a,&st)){ int e2=errno; if(lc->stream) funlockfile(lc->stream); return lc_unread(lc,e2); }
    uint64_t was=(uint64_t)st.st_size, k=lc->rotations+lc->failed+lc->recreated+1; int res=1, nfd=-1, e=0, lost=0; const char *step="";
    lc_line(lc->fd_a,"kh: host log: log-rotated: %s reached %llu bytes (cap %llu bytes per file): it becomes %s, replacing an older one, and a new %s starts (rotation %llu)\n",
            lc->path,(unsigned long long)was,(unsigned long long)lc->cap,old,lc->path,(unsigned long long)k);
    if((nfd=open(nw,O_WRONLY|O_CREAT|O_TRUNC,0644))<0){ e=errno; step="open of the new file"; res=-1; }
    else if(rename(lc->path,old)){ e=errno; if(e==ENOENT) lost=1; else { step="rename"; res=-1; } }      /* ENOENT: NAME is not there: it is made anew below */
    if(res==1&&rename(nw,lc->path)){ e=errno; step="rename of the new file"; res=-1; if(!lost&&rename(old,lc->path)){} }   /* the old name goes back */
    if(res==1&&dup2(nfd,lc->fd_a)<0){ e=errno; step="dup2"; res=-1; if(!lost&&rename(old,lc->path)){} }                      /* NAME is again the file the descriptors write into */
    if(res==1&&lc->fd_b>=0&&dup2(nfd,lc->fd_b)<0){ int e2=errno; lc->split++;
        lc_line(nfd,"kh: host log: log-descriptor-split: the log's second descriptor could not be moved to the new %s (dup2: %s): it still writes into %s and is moved at the next check (counted)\n",lc->path,strerror(e2),old); }
    if(nfd>=0) close(nfd);
    if(res!=1) unlink(nw);
    if(res==1&&!lost){ lc->rotations++; lc->bytes_out+=was;
        lc_line(lc->fd_a,"kh: host log: log-rotated: this file continues %s (rotation %llu: the %llu bytes before it are in %s; an older %s was replaced; cap %llu bytes per file, looked at every %u ms)\n",
                lc->path,(unsigned long long)k,(unsigned long long)was,old,old,(unsigned long long)lc->cap,KH_LOGCAP_PERIOD_MS); }
    else if(res==1){ lc->recreated++; lc->bytes_out+=was;
        lc_line(lc->fd_a,"kh: host log: log-recreated: %s was NOT THERE when its %llu bytes were to become %s (removed, or lost by a rotation that failed): those bytes are not kept, this new %s starts (rotation %llu, counted; cap %llu bytes per file)\n",
                lc->path,(unsigned long long)was,old,lc->path,(unsigned long long)k,(unsigned long long)lc->cap); }
    else { lc->failed++; lc->bytes_out+=was;                    /* the bound still holds: the file behind the descriptors is emptied in place */
        if(ftruncate(lc->fd_a,0)){} if(lseek(lc->fd_a,0,SEEK_SET)<0){}
        lc_line(lc->fd_a,"kh: host log: log-rotate-failed: %s of %s failed (%s) at %llu bytes (cap %llu): the log was EMPTIED IN PLACE instead, its %llu bytes are lost (rotation %llu, counted)\n",
                step,lc->path,strerror(e),(unsigned long long)was,(unsigned long long)lc->cap,(unsigned long long)was,(unsigned long long)k); }
    if(lc->stream) funlockfile(lc->stream);
    return res;
}
/* review C43 n=5: NAME.1 of an EARLIER launch (the `log` line truncates NAME only) goes when the cap takes effect, so the two files a
 * session leaves are that session's.  1 = removed (*bytes); 0 = there was none; -1 = it is there and cannot be removed (errno) */
static int lc_clear_old(const char *path,long long *bytes){
    char old[KH_TMP_MAX+440]; struct stat st; *bytes=0; if(snprintf(old,sizeof old,"%s.1",path)>=(int)sizeof old){ errno=ENAMETOOLONG; return -1; }
    if(lstat(old,&st)) return errno==ENOENT?0:-1;
    *bytes=(long long)st.st_size; return unlink(old)?-1:1; }
static KhLogCap he_lc; static int he_lc_on;
static void *he_logcap_watch(void *a){ (void)a;
    for(;;){ struct timespec ts={(time_t)(KH_LOGCAP_PERIOD_MS/1000u),(long)(KH_LOGCAP_PERIOD_MS%1000u)*1000000L}; while(nanosleep(&ts,&ts)==-1&&errno==EINTR){}
        kh_log_rotate_if_over(&he_lc); }
    return NULL; }
static void he_logcap_start(const char *path,uint64_t cap){
    if(he_lc_on){ he_lc.cap=cap; return; }                       /* a second logcap line: the later size */
    memset(&he_lc,0,sizeof he_lc); snprintf(he_lc.path,sizeof he_lc.path,"%s",path); he_lc.cap=cap; he_lc.fd_a=fileno(stderr); he_lc.fd_b=STDOUT_FILENO; he_lc.stream=stderr;
    { long long ob=0; int k=lc_clear_old(path,&ob), e=errno;                    /* review C43 n=5 */
      if(k>0) fprintf(stderr,"kh: host log: %s.1 of an earlier launch (%lld bytes) was removed: the log files of this launch are this launch's\n",path,ob);
      else if(k<0) fprintf(stderr,"kh: host log: log-old-generation: %s.1 of an earlier launch cannot be removed (%s): until this launch's first rotation replaces it, it is NOT this launch's log\n",path,strerror(e)); }
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    if(pthread_create(&th,&at,he_logcap_watch,NULL)) kh_fatal(NULL,"host-logcap-thread: the log watcher thread cannot be started (logcap %llu)",(unsigned long long)cap);
    pthread_attr_destroy(&at); he_lc_on=1;
}
void kh_host_log_report(FILE *f){
    if(!he_lc_on) return;
    fprintf(f,"kh: host log: %s: cap %llu bytes per file (logcap), looked at every %u ms: %llu rotation(s) to %s.1, %llu failed (emptied in place), %llu time(s) made anew because it was not there, %llu size check(s) not possible, second descriptor: %llu not moved, %llu moved later; %llu bytes moved out of the current file\n",
            he_lc.path,(unsigned long long)he_lc.cap,KH_LOGCAP_PERIOD_MS,(unsigned long long)he_lc.rotations,he_lc.path,(unsigned long long)he_lc.failed,(unsigned long long)he_lc.recreated,(unsigned long long)he_lc.unread,
            (unsigned long long)he_lc.split,(unsigned long long)he_lc.healed,(unsigned long long)he_lc.bytes_out);
}
static long long lc_size(const char *p){ struct stat st; return stat(p,&st)?-1:(long long)st.st_size; }
static int lc_fill(int fd,size_t n,char c){ char b[256]; memset(b,c,sizeof b); b[255]='\n'; while(n){ size_t k=n<sizeof b?n:sizeof b; if(write(fd,b,k)!=(ssize_t)k) return 0; n-=k; } return 1; }
static int lc_has(const char *p,const char *needle,char first){ FILE *f=fopen(p,"rb"); if(!f) return 0; static char b[16384]; size_t g=fread(b,1,sizeof b-1,f); b[g]=0; fclose(f); return g>0&&(!first||b[0]==first)&&strstr(b,needle)!=NULL; }
int kh_host_log_selftest(void){
    int checks=0, fails=0;
#define T(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"host log selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    char dir[KH_TMP_MAX], p[KH_TMP_MAX+32], p1[KH_TMP_MAX+40], inner[KH_TMP_MAX+48], pn[KH_TMP_MAX+40], q[KH_TMP_MAX+32]; kh_tmp_path(dir,sizeof dir,"khnmc-hostlog-XXXXXX");
    if(!mkdtemp(dir)){ fprintf(stderr,"host log selftest FAIL: mkdtemp(%s): %s\n",dir,strerror(errno)); return 1; }
    snprintf(p,sizeof p,"%s/t.log",dir); snprintf(p1,sizeof p1,"%s.1",p); snprintf(inner,sizeof inner,"%s/x",p1); snprintf(pn,sizeof pn,"%s.new",p); snprintf(q,sizeof q,"%s/q.log",dir);
    uint64_t b=0; T(he_logcap_parse("4096",&b)&&b==4096); T(he_logcap_parse("16M",&b)&&b==16ull<<20); T(he_logcap_parse("64K",&b)&&b==65536); T(he_logcap_parse("1024M",&b)&&b==1ull<<30);
    T(!he_logcap_parse("4095",&b)&&!he_logcap_parse("1025M",&b)&&!he_logcap_parse("2G",&b)&&!he_logcap_parse("",&b)&&!he_logcap_parse("M",&b)&&!he_logcap_parse("16MB",&b)&&!he_logcap_parse("-1",&b)&&!he_logcap_parse(NULL,&b));
    int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644), fd2=fd>=0?dup(fd):-1; T(fd>=0&&fd2>=0);
    if(fd>=0&&fd2>=0){
        KhLogCap lc; memset(&lc,0,sizeof lc); snprintf(lc.path,sizeof lc.path,"%s",p); lc.cap=5000; lc.fd_a=fd; lc.fd_b=fd2; lc.stream=NULL;
        /* below the cap: nothing happens */
        T(lc_fill(fd,3000,'a') && kh_log_rotate_if_over(&lc)==0 && lc_size(p)==3000 && lc_size(p1)<0 && lc.rotations==0);
        T(lc_fill(fd2,1999,'a') && kh_log_rotate_if_over(&lc)==0 && lc_size(p)==4999);                         /* one byte below */
        /* at the cap: the old content is NAME.1 (with the line that names the rotation), NAME is new and small, BOTH descriptors write into it */
        T(lc_fill(fd,1,'a') && kh_log_rotate_if_over(&lc)==1 && lc.rotations==1 && lc.failed==0);
        T(lc_size(p1)>5000 && lc_has(p1,"log-rotated: ",'a') && lc_size(p)>0 && lc_size(p)<600 && lc_has(p,"log-rotated: this file continues",'k'));
        { long long s0=lc_size(p); T(write(fd,"X\n",2)==2 && write(fd2,"Y\n",2)==2 && lc_size(p)==s0+4 && lc_has(p,"X\nY\n",'k') && lc_size(p1)>5000); }
        T(kh_log_rotate_if_over(&lc)==0);
        /* a second rotation replaces NAME.1 (one generation is kept): its content is the second file's, not the first's */
        T(lc_fill(fd2,6000,'b') && kh_log_rotate_if_over(&lc)==1 && lc.rotations==2 && lc_has(p1,"X\nY\n",'k') && lc_has(p1,"bbbb",'k') && lc_size(p)<600 && lc_has(p,"(rotation 2:",'k'));
        /* the rename cannot be made (NAME.1 is a directory that is not empty): the file is emptied in place, by name, and the bound holds */
        T(unlink(p1)==0 && mkdir(p1,0755)==0 && file_touch(inner));
        T(lc_fill(fd,7000,'c') && lc_size(p)>=7000 && kh_log_rotate_if_over(&lc)==-1 && lc.failed==1 && lc.rotations==2 && lc_size(p)>0 && lc_size(p)<700 && lc_has(p,"log-rotate-failed: rename",'k'));
        T(write(fd2,"Z\n",2)==2 && lc_has(p,"Z\n",'k'));                                                    /* the descriptors still write into the file */
        T(lc.bytes_out>=5000+6000+7000 && lc_size(pn)<0);                                                    /* the new file's temporary name is not left behind */
        unlink(inner); rmdir(p1);
        /* review C43 n=10: THE NEW FILE CANNOT BE OPENED (its temporary name is a directory): nothing was renamed.  NAME is still the log,
         * emptied in place, by name; NAME.1 still holds the generation before, untouched; both descriptors still write into NAME */
        { int o=open(p1,O_WRONLY|O_CREAT|O_TRUNC,0644); T(o>=0 && write(o,"OLDGEN\n",7)==7); if(o>=0) close(o); }
        T(mkdir(pn,0755)==0 && lc_fill(fd,6000,'d') && kh_log_rotate_if_over(&lc)==-1 && lc.failed==2 && lc.rotations==2);
        T(lc_size(p)>0 && lc_size(p)<700 && lc_has(p,"log-rotate-failed: open of the new file",'k') && lc_size(p1)==7 && lc_has(p1,"OLDGEN\n",'O'));
        { long long s0=lc_size(p); T(write(fd,"V\n",2)==2 && write(fd2,"W\n",2)==2 && lc_size(p)==s0+4 && lc_size(p1)==7); }
        T(rmdir(pn)==0 && lc_fill(fd2,6000,'e') && kh_log_rotate_if_over(&lc)==1 && lc.rotations==3 && lc_has(p1,"V\nW\n",'k') && lc_has(p1,"eeee",'k') && lc_size(p)<600 && lc_size(pn)<0);   /* and the next rotation works */
        /* NAME IS NOT THERE at a rotation (removed, or lost): it is made anew, by name, counted apart; the rotation after it is a normal one */
        T(unlink(p)==0 && lc_fill(fd,6000,'g') && lc_size(p)<0 && kh_log_rotate_if_over(&lc)==1 && lc.recreated==1 && lc.rotations==3 && lc.failed==2);
        T(lc_size(p)>0 && lc_size(p)<700 && lc_has(p,"log-recreated: ",'k') && lc_has(p1,"eeee",'k') && lc_size(pn)<0);
        { long long s0=lc_size(p); T(write(fd,"R\n",2)==2 && write(fd2,"S\n",2)==2 && lc_size(p)==s0+4); }
        T(lc_fill(fd,6000,'h') && kh_log_rotate_if_over(&lc)==1 && lc.rotations==4 && lc_has(p1,"R\nS\n",'k') && lc_has(p1,"hhhh",'k'));
        /* a second descriptor that writes into ANOTHER file (a rotation could not move it) is moved at the next check, counted */
        { int fq=open(q,O_WRONLY|O_CREAT|O_TRUNC,0644); T(fq>=0);
          if(fq>=0){ KhLogCap l2=lc; l2.fd_b=fq; l2.healed=0; long long s0=lc_size(p); T(write(fq,"lost\n",5)==5 && lc_size(q)==5);
              T(kh_log_rotate_if_over(&l2)==0 && l2.healed==1 && lc_size(p)>s0 && lc_has(p,"second descriptor was moved",'k'));
              s0=lc_size(p); T(write(fq,"T\n",2)==2 && lc_size(p)==s0+2 && lc_size(q)==5); T(kh_log_rotate_if_over(&l2)==0 && l2.healed==1);
              close(fq); unlink(q); } }
        /* a log whose size cannot be read: counted, never silent */
        { KhLogCap l3; memset(&l3,0,sizeof l3); snprintf(l3.path,sizeof l3.path,"%s",p); l3.cap=5000; l3.fd_a=-1; l3.fd_b=-1; T(kh_log_rotate_if_over(&l3)==-2 && l3.unread==1 && kh_log_rotate_if_over(&l3)==-2 && l3.unread==2); }
    }
    if(fd>=0) close(fd); if(fd2>=0) close(fd2);
    /* review C43 n=5: NAME.1 of an earlier launch goes when the cap takes effect; none there is not an error; one that cannot be removed is said */
    { long long ob=-1; unlink(p1); int o=open(p1,O_WRONLY|O_CREAT|O_TRUNC,0644); T(o>=0 && write(o,"earlier launch\n",15)==15); if(o>=0) close(o);
      T(lc_clear_old(p,&ob)==1 && ob==15 && lc_size(p1)<0); T(lc_clear_old(p,&ob)==0 && ob==0);
      T(mkdir(p1,0755)==0 && file_touch(inner) && lc_clear_old(p,&ob)==-1 && lc_size(p1)>=0); unlink(inner); rmdir(p1); }
    unlink(p); unlink(p1); T(rmdir(dir)==0);
#undef T
    fprintf(stderr,"kh: host log selftest: %d checks, %d failures (logcap sizes; below the cap nothing; at the cap the file becomes NAME.1 and both descriptors write into a new NAME; one generation kept; a rename that fails empties the file in place, by name; a new file that cannot be opened leaves NAME and NAME.1 as they are; a NAME that is gone is made anew; a second descriptor left behind is moved; an unreadable size is counted; NAME.1 of an earlier launch is removed)\n",checks,fails);
    return fails;
}
