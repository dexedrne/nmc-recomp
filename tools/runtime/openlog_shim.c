/* openlog_shim.c : LD_PRELOAD shim (Linux, glibc) that logs every path the process hands to the host file system, for
 * tools/make_route_subset.sh.  One line per call, appended to $KH_OPENLOG:  <op> <result> <path>
 *   op: open (open/open64/openat/openat64/fopen/fopen64/freopen), dir (opendir), stat (stat/lstat/fstatat/access/statx)
 *   result: ok or e<errno> (e2 = ENOENT)
 * Large files (regular, at least KH_OPENLOG_BIG bytes, default 64 MiB) opened through the open() family are followed by descriptor,
 * so that a subset can hold only the parts a run reads:   range <offset>+<length> <path>   for every read / pread on them.
 * Any other way of reading such a file (fopen, mmap, readv, preadv) is logged as   whole ok <path>   : the subset then takes the whole
 * file, never a guess.  Relative paths are logged as given, preceded by the working directory when it can be read.  The shim only
 * observes: it calls the real function with unchanged arguments and returns its result.
 * Build: cc -shared -fPIC -O1 -o openlog_shim.so openlog_shim.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static int logfd=-2;
static void emit(const char *op,int ok,int err,int dirfd,const char *path){
    int saved=errno;
    if(logfd==-2){ const char *p=getenv("KH_OPENLOG"); int (*ropen)(const char*,int,...)=dlsym(RTLD_NEXT,"open"); logfd=p&&*p?ropen(p,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644):-1; }
    if(logfd>=0&&path){ char line[4600],cwd[1024]; int n;
        if(path[0]!='/'&&dirfd==AT_FDCWD&&getcwd(cwd,sizeof cwd)) n=snprintf(line,sizeof line,"%s %s%.0d %s/%s\n",op,ok?"ok":"e",ok?0:err,cwd,path);
        else n=snprintf(line,sizeof line,"%s %s%.0d %s\n",op,ok?"ok":"e",ok?0:err,path);
        if(n>0){ if(n>=(int)sizeof line) n=(int)sizeof line-1; if(write(logfd,line,(size_t)n)<0){} } }
    errno=saved;
}
#define REAL(name) static __typeof__(name) *real; if(!real) real=dlsym(RTLD_NEXT,#name)
static int has_mode(int flags){ return (flags&O_CREAT)||((flags&O_TMPFILE)==O_TMPFILE); }
/* ---- large files by descriptor ---- */
#define MAXFD 4096
static char *big[MAXFD];                                   /* path of a followed descriptor (set once the open returned; cleared by close) */
static long long big_min(void){ static long long v=-1; if(v<0){ const char *e=getenv("KH_OPENLOG_BIG"); v=e&&*e?atoll(e):(64ll<<20); } return v; }
static int is_big(int fd){ struct stat st; int saved=errno; int r=fd>=0&&fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&(long long)st.st_size>=big_min(); errno=saved; return r; }
static void follow(int fd,int dirfd,const char *path){
    if(fd<0||fd>=MAXFD||!path||!is_big(fd)) return; int saved=errno; char full[4600],cwd[1024];
    if(path[0]!='/'&&dirfd==AT_FDCWD&&getcwd(cwd,sizeof cwd)) snprintf(full,sizeof full,"%s/%s",cwd,path); else snprintf(full,sizeof full,"%s",path);
    char *old=__atomic_exchange_n(&big[fd],strdup(full),__ATOMIC_SEQ_CST); free(old); errno=saved; }
static void range(int fd,long long off,long long len){ char *p=fd>=0&&fd<MAXFD?__atomic_load_n(&big[fd],__ATOMIC_SEQ_CST):NULL; if(p&&len>0){ char op[64]; snprintf(op,sizeof op,"range %lld+%lld",off,len); int saved=errno; emit(op,1,0,-1,p); errno=saved; } }
static void whole(int fd,const char *path){ if(path&&is_big(fd)) emit("whole",1,0,AT_FDCWD,path); else if(fd>=0&&fd<MAXFD){ char *p=__atomic_load_n(&big[fd],__ATOMIC_SEQ_CST); if(p) emit("whole",1,0,-1,p); } }
int close(int fd){ REAL(close); if(fd>=0&&fd<MAXFD){ char *old=__atomic_exchange_n(&big[fd],NULL,__ATOMIC_SEQ_CST); free(old); } return real(fd); }
ssize_t read(int fd,void *buf,size_t n){ REAL(read); long long off=-1; if(fd>=0&&fd<MAXFD&&__atomic_load_n(&big[fd],__ATOMIC_SEQ_CST)){ int saved=errno; off=lseek(fd,0,SEEK_CUR); errno=saved; } ssize_t r=real(fd,buf,n); if(off>=0) range(fd,off,r); return r; }
ssize_t pread(int fd,void *buf,size_t n,off_t off){ REAL(pread); ssize_t r=real(fd,buf,n,off); range(fd,off,r); return r; }
ssize_t pread64(int fd,void *buf,size_t n,off64_t off){ REAL(pread64); ssize_t r=real(fd,buf,n,off); range(fd,off,r); return r; }
ssize_t readv(int fd,const struct iovec *iov,int cnt){ REAL(readv); whole(fd,NULL); return real(fd,iov,cnt); }
ssize_t preadv(int fd,const struct iovec *iov,int cnt,off_t off){ REAL(preadv); whole(fd,NULL); return real(fd,iov,cnt,off); }
void *mmap(void *a,size_t n,int prot,int fl,int fd,off_t off){ REAL(mmap); if(fd>=0) whole(fd,NULL); return real(a,n,prot,fl,fd,off); }
void *mmap64(void *a,size_t n,int prot,int fl,int fd,off64_t off){ REAL(mmap64); if(fd>=0) whole(fd,NULL); return real(a,n,prot,fl,fd,off); }
int open(const char *path,int flags,...){ REAL(open); mode_t m=0; if(has_mode(flags)){ va_list ap; va_start(ap,flags); m=va_arg(ap,mode_t); va_end(ap); } int r=real(path,flags,m); emit("open",r>=0,errno,AT_FDCWD,path); follow(r,AT_FDCWD,path); return r; }
int open64(const char *path,int flags,...){ REAL(open64); mode_t m=0; if(has_mode(flags)){ va_list ap; va_start(ap,flags); m=va_arg(ap,mode_t); va_end(ap); } int r=real(path,flags,m); emit("open",r>=0,errno,AT_FDCWD,path); follow(r,AT_FDCWD,path); return r; }
int openat(int dfd,const char *path,int flags,...){ REAL(openat); mode_t m=0; if(has_mode(flags)){ va_list ap; va_start(ap,flags); m=va_arg(ap,mode_t); va_end(ap); } int r=real(dfd,path,flags,m); emit("open",r>=0,errno,dfd,path); follow(r,dfd,path); return r; }
int openat64(int dfd,const char *path,int flags,...){ REAL(openat64); mode_t m=0; if(has_mode(flags)){ va_list ap; va_start(ap,flags); m=va_arg(ap,mode_t); va_end(ap); } int r=real(dfd,path,flags,m); emit("open",r>=0,errno,dfd,path); follow(r,dfd,path); return r; }
FILE *fopen(const char *path,const char *mode){ REAL(fopen); FILE *r=real(path,mode); emit("open",r!=NULL,errno,AT_FDCWD,path); if(r) whole(fileno(r),path); return r; }
FILE *fopen64(const char *path,const char *mode){ REAL(fopen64); FILE *r=real(path,mode); emit("open",r!=NULL,errno,AT_FDCWD,path); if(r) whole(fileno(r),path); return r; }
FILE *freopen(const char *path,const char *mode,FILE *f){ REAL(freopen); FILE *r=real(path,mode,f); emit("open",r!=NULL,errno,AT_FDCWD,path); if(r) whole(fileno(r),path); return r; }
DIR *opendir(const char *path){ REAL(opendir); DIR *r=real(path); emit("dir",r!=NULL,errno,AT_FDCWD,path); return r; }
int stat(const char *path,struct stat *st){ REAL(stat); int r=real(path,st); emit("stat",r==0,errno,AT_FDCWD,path); return r; }
int lstat(const char *path,struct stat *st){ REAL(lstat); int r=real(path,st); emit("stat",r==0,errno,AT_FDCWD,path); return r; }
int stat64(const char *path,struct stat64 *st){ REAL(stat64); int r=real(path,st); emit("stat",r==0,errno,AT_FDCWD,path); return r; }
int lstat64(const char *path,struct stat64 *st){ REAL(lstat64); int r=real(path,st); emit("stat",r==0,errno,AT_FDCWD,path); return r; }
int fstatat(int dfd,const char *path,struct stat *st,int fl){ REAL(fstatat); int r=real(dfd,path,st,fl); if(path&&*path) emit("stat",r==0,errno,dfd,path); return r; }
int fstatat64(int dfd,const char *path,struct stat64 *st,int fl){ REAL(fstatat64); int r=real(dfd,path,st,fl); if(path&&*path) emit("stat",r==0,errno,dfd,path); return r; }
int access(const char *path,int mode){ REAL(access); int r=real(path,mode); emit("stat",r==0,errno,AT_FDCWD,path); return r; }
int statx(int dfd,const char *restrict path,int fl,unsigned mask,struct statx *restrict st){ REAL(statx); int r=real(dfd,path,fl,mask,st); if(path&&*path) emit("stat",r==0,errno,dfd,path); return r; }
