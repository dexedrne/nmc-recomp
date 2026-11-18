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
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

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
 * In VALUE, TEXT and NAME: @DOCS@ = <home>/Documents, @SUPPORT@ = <home>/Library/Application Support/khnmc, @HOME@, @TMP@.
 * Anything else is the named fatal `host-conf` with the file and line: a typo must not become a silently different run. */
#define HE_MAXARG 64
static char he_home[KH_TMP_MAX], he_docs[KH_TMP_MAX+32], he_support[KH_TMP_MAX+64], he_status[KH_TMP_MAX+320];
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
    char line[2048], val[KH_TMP_MAX+400]; int ln=0, n=0; err[0]=0;
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
                       setvbuf(stderr,NULL,_IOLBF,0); if(dup2(fileno(stderr),STDOUT_FILENO)<0){} fprintf(stderr,"kh: host conf: log %s\n",val); }
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
    snprintf(p,sizeof p,"%s/pkgdir",he_docs); he_default("KHNMC_PKGDIR",p,1);
    fprintf(stderr,"kh: host dirs: temporary files in %s\n",kh_tmpdir());
    const char *wd=getenv("KH_ROOT"); he_mkdirs(wd);
    if(chdir(wd)) kh_fatal(NULL,"app-home-not-writable: cannot enter the working directory %s",wd);
    /* the game's Documents folder: the game creates only its own sub-folders in it, so it must exist (a test run gives a fresh
     * one under @TMP@ through khnmc.conf: tools/ios/mac/device_run.sh route; the game's config there changes how it boots) */
    const char *gd=getenv("KH_DOCS"); struct stat gst; he_mkdirs(gd);
    if(stat(gd,&gst)||!S_ISDIR(gst.st_mode)) kh_fatal(NULL,"app-docs-not-writable: cannot create the game's Documents folder %s",gd);
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
