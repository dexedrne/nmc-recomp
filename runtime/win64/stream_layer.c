/* stream_layer.c : the CFileMan stream-fd API on the loose tree (docs/platform/01-FILE-LAYER.md, "Streams").
 *
 * Sound streams (VAG_STREAM::play -> AxaSoundStream thread 0x13c090, Sound::CStream 0x13be30) do not use the loaders: they call
 *   int OpenStream(CFileMan*, const char *path)                         0x106da0   fd > 0, or -1
 *   int ReadStream(CFileMan*, int fd, void *dst, int offset, int len)    0x106fa0   bytes read
 *   int CloseStream(CFileMan*, int fd)                                   0x106b40
 * The originals look the name up in the package HEDs, re-open the .pkg with a private fd and read at entryOffset + 0x10 + offset.
 * With empty packages (loose tree) that lookup can never succeed, so every stream open failed and no stream sound was ever
 * submitted.  Here: loose first (the file under KHNMC_DATA, read at `offset`, no package header), otherwise the untouched lifted
 * original when real packages are registered (same rule as the loaders in file_layer.c).  Nothing is decoded or changed here.
 * Every open is written to KHNMC_FILELOG as `HIT STREAM <size> <path>` or `MISS NONE -1 <path>`. */
#include "khnmc_rt.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

void orig_f_106da0(KCtx *g); void orig_f_106fa0(KCtx *g); void orig_f_106b40(KCtx *g);
int kh_file_loose_path(KCtx *g,const char *path,char *host,size_t cap);
void kh_file_log_entry(KCtx *g,const char *path,const char *source,long long size);
int kh_file_package_fallback(const char *path);

#define MAXSTREAM 64
static struct { int fd; long long size; unsigned long long reads, bytes; char name[96]; } streams[MAXSTREAM];
static pthread_mutex_t sm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 st_open, st_miss, st_reads, st_bytes, st_short, st_close, st_package;
static int find_stream(int fd){ for(int i=0;i<MAXSTREAM;i++) if(streams[i].fd==fd&&fd>0) return i; return -1; }

void f_106da0(KCtx *g){                                   /* OpenStream */
    const char *path=gstr(g,A1(g)); char host[4096]; struct stat st; int fd=-1;
    if(kh_file_loose_path(g,path,host,sizeof host)){
        fd=open(host,O_RDONLY|O_CLOEXEC);
        if(fd>=0&&(fstat(fd,&st)||!S_ISREG(st.st_mode))){ close(fd); fd=-1; }
        if(fd==0){ int d=fcntl(fd,F_DUPFD_CLOEXEC,3); close(fd); fd=d; }               /* the callers treat fd <= 0 as failure */
    }
    if(fd>0){
        pthread_mutex_lock(&sm); int slot=-1; for(int i=0;i<MAXSTREAM;i++) if(streams[i].fd<=0){ slot=i; break; }
        if(slot<0){ pthread_mutex_unlock(&sm); kh_fatal(g,"stream layer: more than %d open streams",MAXSTREAM); }
        streams[slot].fd=fd; streams[slot].size=st.st_size; streams[slot].reads=streams[slot].bytes=0;
        const char *base=strrchr(path,'/'); const char *b2=strrchr(path,'\\'); if(b2&&(!base||b2>base)) base=b2;
        snprintf(streams[slot].name,sizeof streams[slot].name,"%s",base?base+1:path);
        pthread_mutex_unlock(&sm);
        st_open++; kh_file_log_entry(g,path,"STREAM",st.st_size);
        fprintf(stderr,"kh: stream open fd=%d size=%lld %s\n",fd,(long long)st.st_size,path);
        SET_RAX(g,(u32)fd); KH_RET(g); return;
    }
    if(kh_file_package_fallback(path)){                   /* real packages registered: let the game's own package code try */
        char keep[1024]; snprintf(keep,sizeof keep,"%s",path);
        orig_f_106da0(g); int r=(s32)KH_RAX(g);
        if(r>0){ st_package++; kh_file_log_entry(g,keep,"PACKAGE-STREAM",0); } else { st_miss++; kh_file_log_entry(g,keep,NULL,-1); }
        fprintf(stderr,"kh: stream open through the package code = %d %s\n",r,keep);
        return;
    }
    st_miss++; kh_file_log_entry(g,path,NULL,-1);
    fprintf(stderr,"kh: stream open MISS %s\n",path);
    SET_RAX(g,0xffffffffu); KH_RET(g);
}
void f_106fa0(KCtx *g){                                   /* ReadStream(this, fd, dst, offset, len) */
    int fd=(s32)A1(g); u64 dst=A2(g); s64 off=(s32)A3(g); u32 len=(u32)A4(g);
    pthread_mutex_lock(&sm); int i=find_stream(fd); pthread_mutex_unlock(&sm);
    if(i<0){ orig_f_106fa0(g); return; }                  /* an fd of the package path */
    if(off<0||(len&&(!GA_OK(dst)||!GA_OK(dst+len-1)))) kh_fatal(g,"stream layer: ReadStream(fd=%d, dst=%llx, offset=%lld, len=%u) is out of range",fd,(unsigned long long)dst,(long long)off,len);
    size_t got=0;
    while(got<len){ ssize_t k=pread(fd,(char*)hp(g,dst)+got,len-got,(off_t)(off+(s64)got)); if(k<0){ if(errno==EINTR) continue; kh_fatal(g,"stream layer: read error on fd %d: %s",fd,strerror(errno)); } if(!k) break; got+=(size_t)k; }
    pthread_mutex_lock(&sm); i=find_stream(fd); if(i>=0){ streams[i].reads++; streams[i].bytes+=got; } pthread_mutex_unlock(&sm);
    st_reads++; st_bytes+=got; if(got<len) st_short++;    /* a short read at the end of the file is what _read returns too */
    SET_RAX(g,(u32)got); KH_RET(g);
}
void f_106b40(KCtx *g){                                   /* CloseStream(this, fd) */
    int fd=(s32)A1(g);
    pthread_mutex_lock(&sm); int i=find_stream(fd);
    if(i<0){ pthread_mutex_unlock(&sm); orig_f_106b40(g); return; }
    fprintf(stderr,"kh: stream close fd=%d %s reads=%llu bytes=%llu of %lld\n",fd,streams[i].name,streams[i].reads,streams[i].bytes,streams[i].size);
    streams[i].fd=0; pthread_mutex_unlock(&sm);
    st_close++; SET_RAX(g,(u32)close(fd)); KH_RET(g);
}
void kh_stream_report(FILE *f){
    if(!st_open&&!st_miss&&!st_package) return;
    fprintf(f,"kh: streams: loose opens=%llu package opens=%llu misses=%llu reads=%llu bytes=%llu short-reads=%llu closes=%llu\n",(unsigned long long)st_open,(unsigned long long)st_package,(unsigned long long)st_miss,
            (unsigned long long)st_reads,(unsigned long long)st_bytes,(unsigned long long)st_short,(unsigned long long)st_close);
    pthread_mutex_lock(&sm);
    for(int i=0;i<MAXSTREAM;i++) if(streams[i].fd>0) fprintf(f,"kh: streams: still open fd=%d %s reads=%llu bytes=%llu of %lld\n",streams[i].fd,streams[i].name,streams[i].reads,streams[i].bytes,streams[i].size);
    pthread_mutex_unlock(&sm);
}
/* synthetic tree: open (case-insensitive, BasePath prefix), read at offsets, short read at the end, close, miss, traversal */
int kh_stream_selftest(KCtx *g){
    int bad=0,checks=0; char dir[KH_TMP_MAX],path[KH_TMP_MAX+256]; kh_tmp_path(dir,sizeof dir,"khnmc-stream-XXXXXX");   /* host_env.c: /tmp, or the app container on iOS */
#define TEST(c) do{ checks++; if(!(c)){ fprintf(stderr,"stream selftest FAIL: %s (line %d)\n",#c,__LINE__); bad++; } }while(0)
    const char *oldroot=getenv("KHNMC_DATA"),*oldlog=getenv("KHNMC_FILELOG"); char *root=oldroot?strdup(oldroot):NULL,*log=oldlog?strdup(oldlog):NULL;
    if(!mkdtemp(dir)) return 1;
    snprintf(path,sizeof path,"%s/VagStream",dir); mkdir(path,0700);
    snprintf(path,sizeof path,"%s/VagStream/Title.win32.scd",dir); FILE *f=fopen(path,"wb"); if(!f) return 1;
    for(int i=0;i<1000;i++) fputc(i&0xff,f); fclose(f);
    char lp[KH_TMP_MAX+256]; snprintf(lp,sizeof lp,"%s/log",dir); setenv("KHNMC_DATA",dir,1); setenv("KHNMC_FILELOG",lp,1);
    char win[2048]; snprintf(win,sizeof win,"%s/vagstream/title.win32.scd",gstr(g,GS_BASE+0x2bd4340));
    u64 name=ga_strdup(win),missing=ga_strdup("vagstream/none.win32.scd"),parent=ga_strdup("../Title.win32.scd"),buf=ga_calloc(1,64);
    u64 o0=st_open,m0=st_miss;
    int fd=(s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,name,0,0); TEST(fd>0&&st_open==o0+1);
    int fd2=(s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,name,0,0); TEST(fd2>0&&fd2!=fd);                  /* two independent streams on one file */
    TEST((s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,missing,0,0)==-1&&st_miss==m0+1);
    TEST((s32)kh_call(g,GS_BASE+0x106da0,GS_BASE+0x79cfc0,parent,0,0)==-1);
    memset(hp(g,buf),0xee,64);
    TEST(kh_call5(g,GS_BASE+0x106fa0,GS_BASE+0x79cfc0,(u64)fd,buf,0,16)==16&&rd8(g,buf)==0&&rd8(g,buf+15)==15&&rd8(g,buf+16)==0xee);   /* offset 0 = first byte: no package header */
    TEST(kh_call5(g,GS_BASE+0x106fa0,GS_BASE+0x79cfc0,(u64)fd,buf,300,4)==4&&rd8(g,buf)==(300&0xff)&&rd8(g,buf+3)==(303&0xff));
    TEST(kh_call5(g,GS_BASE+0x106fa0,GS_BASE+0x79cfc0,(u64)fd2,buf,990,32)==10&&rd8(g,buf)==(990&0xff)&&rd8(g,buf+9)==(999&0xff));     /* short read at the end */
    TEST(kh_call5(g,GS_BASE+0x106fa0,GS_BASE+0x79cfc0,(u64)fd,buf,5000,8)==0);
    TEST(kh_call5(g,GS_BASE+0x106fa0,GS_BASE+0x79cfc0,(u64)fd,buf,7,1)==1&&rd8(g,buf)==7);                                             /* reads do not move a shared position */
    TEST((s32)kh_call(g,GS_BASE+0x106b40,GS_BASE+0x79cfc0,(u64)fd,0,0)==0&&(s32)kh_call(g,GS_BASE+0x106b40,GS_BASE+0x79cfc0,(u64)fd2,0,0)==0);
    TEST(fcntl(fd,F_GETFD)==-1&&find_stream(fd)<0);
    f=fopen(lp,"r"); char line[4096]; int hits=0,miss=0; while(f&&fgets(line,sizeof line,f)){ if(!strncmp(line,"HIT STREAM 1000 ",16)) hits++; if(!strncmp(line,"MISS NONE -1 ",13)) miss++; } if(f) fclose(f);
    TEST(hits==2&&miss==2);
    ga_free(name); ga_free(missing); ga_free(parent); ga_free(buf);
    if(root) setenv("KHNMC_DATA",root,1); else unsetenv("KHNMC_DATA");
    if(log) setenv("KHNMC_FILELOG",log,1); else unsetenv("KHNMC_FILELOG");
    free(root); free(log); unlink(path); unlink(lp); snprintf(path,sizeof path,"%s/VagStream",dir); rmdir(path); rmdir(dir);
    fprintf(stderr,"kh: stream selftest: %d checks, %d failures\n",checks,bad); return bad;
}
