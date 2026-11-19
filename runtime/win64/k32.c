/* k32.c : kernel32 (loader, memory, time, strings, files, find, mapping, diagnostics), winmm, advapi32 registry, shell32, ole32,
 * oleaut32, imagehlp, dbghelp.  Thread/sync/fiber parts of kernel32 live in threads.c. */
#include "khnmc_rt.h"
#include "savestate.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mman.h>

#define WIN_EXE "C:\\KH2\\KINGDOM HEARTS II FINAL MIX.exe"
#define WIN_DOCS "C:\\Users\\khnmc\\Documents"
#define INVALID_HANDLE ((u64)-1)
u64 kh_spawn_hostfn_thread(KCtx *g,const char *name,KhFn fn,u64 param);

/* ---- handles owned here ----------------------------------------------------------------------------------------------- */
typedef struct { int fd; char path[1024]; } KFile;
typedef struct { DIR *d; char dir[1024]; char pat[260]; } KFind;
typedef struct { u64 ga; u64 size; char name[128]; } KMap;
void kh_handle_free(int kind,void *o){
    if(kind==HK_FILE){ KFile *f=o; if(f->fd>=0) close(f->fd); free(f); }
    else if(kind==HK_FIND){ KFind *f=o; if(f->d) closedir(f->d); free(f); }
    else if(kind==HK_MAPPING){ free(o); } else free(o);
}

/* ---- last error / debug ------------------------------------------------------------------------------------------------ */
IMP(K32,GetLastError){ SET_RAX(g,kh_get_last_error()); }
IMP(K32,OutputDebugStringA){ if(kh_verbose) fprintf(stderr,"kh: OutputDebugStringA: %s\n",gstr(g,A0(g))); }
IMP(K32,OutputDebugStringW){ if(kh_verbose){ char t[1024]; gw_to_utf8(g,A0(g),t,sizeof t); fprintf(stderr,"kh: OutputDebugStringW: %s\n",t); } }
IMP(K32,FormatMessageA){ if(A4(g)) {} SET_RAX(g,0); }
IMP(K32,IsDebuggerPresent){ SET_RAX(g,0); }
IMP(K32,IsProcessorFeaturePresent){ u32 f=(u32)A0(g); static const int on[]={3,6,10,13,14,17,23,36,37,38,39,40}; int r=0; for(unsigned i=0;i<sizeof on/sizeof*on;i++) if(on[i]==(int)f) r=1; SET_RAX(g,r); }
IMP(K32,SetUnhandledExceptionFilter){ SET_RAX(g,0); }
IMP(K32,UnhandledExceptionFilter){ kh_fatal(g,"UnhandledExceptionFilter reached (guest crash filter): exception pointers %llx",(unsigned long long)A0(g)); }
IMP(K32,RaiseException){ u32 code=(u32)A0(g);
    if(code==0x406D1388u){                                                           /* SetThreadName convention: THREADNAME_INFO {0x1000, szName, dwThreadID, dwFlags} as ULONG_PTRs */
        u64 args=A3(g); if((u32)A2(g)>=3&&args&&GA_OK(args)&&GA_OK(args+23)&&rd32(g,args)==0x1000u){ u64 nm=rd64(g,args+8); if(nm&&GA_OK(nm)){ extern void kh_thread_set_name(u32,const char*); kh_thread_set_name((u32)rd64(g,args+16),gstr(g,nm)); } }
        return; }
    kh_fatal(g,"RaiseException(0x%x) reached (terminate-on-throw policy)",code); }
IMP(K32,RtlCaptureContext){ memset(hp(g,A0(g)),0,0x4d0); }
IMP(K32,RtlLookupFunctionEntry){ SET_RAX(g,0); }
IMP(K32,RtlVirtualUnwind){ kh_fatal(g,"RtlVirtualUnwind reached (crash filter path)"); }
IMP(K32,TerminateProcess){ extern void kh_exit_process(int); kh_exit_process((int)A1(g)); }
IMP(K32,GetExitCodeProcess){ if(A1(g)) wr32(g,A1(g),259); SET_RAX(g,1); }
IMP(K32,DecodePointer){ SET_RAX(g,A0(g)); }
IMP(K32,InitializeSListHead){ memset(hp(g,A0(g)),0,16); }
IMP(K32,GetProcessHeap){ SET_RAX(g,0x1000); }
IMP(K32,HeapAlloc){ u64 p=ga_alloc_src(A2(g),16,GA_SRC_HEAP); if(p&&(A1(g)&8)) memset(hp(g,p),0,A2(g)); SET_RAX(g,p); }
IMP(K32,HeapFree){ ga_free(A2(g)); SET_RAX(g,1); }
IMP(K32,GlobalMemoryStatus){ u64 p=A0(g); wr32(g,p,56); wr32(g,p+4,25); wr64(g,p+8,8ull<<30); wr64(g,p+16,6ull<<30); wr64(g,p+24,16ull<<30); wr64(g,p+32,12ull<<30); wr64(g,p+40,1ull<<47); wr64(g,p+48,1ull<<46); }
IMP(K32,GetDiskFreeSpaceExA){ if(A1(g)) wr64(g,A1(g),200ull<<30); if(A2(g)) wr64(g,A2(g),400ull<<30); if(A3(g)) wr64(g,A3(g),200ull<<30); SET_RAX(g,1); }
IMP(K32,QueryPerformanceCounter){ wr64(g,A0(g),kh_qpc()); SET_RAX(g,1); }
IMP(K32,QueryPerformanceFrequency){ wr64(g,A0(g),10000000ull); SET_RAX(g,1); }
#define EPOCH_DIFF 116444736000000000ull
IMP(K32,GetSystemTimeAsFileTime){ extern u64 kh_det_tick(void); if(kh_cut_enabled){ wr64(g,A0(g),EPOCH_DIFF+kh_cut_unix_ns()/100); return; } if(ga_deterministic){ wr64(g,A0(g),0x01DBA00000000000ull+kh_det_tick()*10000ull); return; } struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); wr64(g,A0(g),(u64)ts.tv_sec*10000000ull+(u64)ts.tv_nsec/100+EPOCH_DIFF); }
IMP(K32,FileTimeToLocalFileTime){ wr64(g,A1(g),rd64(g,A0(g))); SET_RAX(g,1); }
IMP(K32,FileTimeToSystemTime){ u64 ft=rd64(g,A0(g)); time_t t=(time_t)((ft-EPOCH_DIFF)/10000000ull); struct tm r; gmtime_r(&t,&r); u64 d=A1(g);
    wr16(g,d,(u16)(r.tm_year+1900)); wr16(g,d+2,(u16)(r.tm_mon+1)); wr16(g,d+4,(u16)r.tm_wday); wr16(g,d+6,(u16)r.tm_mday); wr16(g,d+8,(u16)r.tm_hour); wr16(g,d+10,(u16)r.tm_min); wr16(g,d+12,(u16)r.tm_sec); wr16(g,d+14,(u16)(((ft-EPOCH_DIFF)/10000ull)%1000)); SET_RAX(g,1); }
IMP(K32,lstrlenA){ SET_RAX(g,(u32)strlen(gstr(g,A0(g)))); }
IMP(K32,MultiByteToWideChar){ u32 sl=(u32)A3(g); const char *s=gstr(g,A2(g)); u64 d=A4(g); u32 dn=(u32)ARG(g,5);
    size_t n=(s32)sl<0?strlen(s)+1:sl; char *tmp=malloc(n+1); memcpy(tmp,s,n); tmp[n]=0; /* units needed */
    size_t units=0; for(const unsigned char *p=(const unsigned char*)tmp;p<(const unsigned char*)tmp+n;){ unsigned c=*p; int ex=c>=0xF0?3:c>=0xE0?2:c>=0xC0?1:0; p+=1+ex; units+=(c>=0xF0)?2:1; }
    if(!dn){ free(tmp); SET_RAX(g,units); return; } if(units>dn){ free(tmp); kh_set_last_error(122); SET_RAX(g,0); return; }
    /* convert: utf8_to_gw stops at NUL so handle embedded terminator of counted input */
    utf8_to_gw(g,d,dn,tmp); if((s32)sl>=0 && n>0 && tmp[n-1]!=0 && units<dn) wr16(g,d+2*units,0); free(tmp); SET_RAX(g,units); }
IMP(K32,WideCharToMultiByte){ u32 sl=(u32)A3(g); u64 d=A4(g); u32 dn=(u32)ARG(g,5); u64 src=A2(g); char *tmp=malloc(8*(size_t)((s32)sl<0?gw_len(g,src)+1:sl)+8);
    if((s32)sl<0){ gw_to_utf8(g,src,tmp,8*(gw_len(g,src)+1)+8); } else { u64 t=ga_alloc(2*(u64)sl+2,16); memcpy(hp(g,t),hp(g,src),2*(size_t)sl); wr16(g,t+2*(u64)sl,0); gw_to_utf8(g,t,tmp,8*(size_t)sl+8); ga_free(t); }
    size_t n=strlen(tmp)+((s32)sl<0?1:0); if(!dn){ free(tmp); SET_RAX(g,n); return; } if(n>dn){ free(tmp); kh_set_last_error(122); SET_RAX(g,0); return; } memcpy(hp(g,d),tmp,n); free(tmp); SET_RAX(g,n); }
IMP(K32,GetStartupInfoW){ memset(hp(g,A0(g)),0,0x68); wr32(g,A0(g),0x68); }
IMP(K32,GetModuleFileNameA){ const char *p=WIN_EXE; u64 b=A1(g),n=A2(g); size_t l=strlen(p); if(l+1>n){ if(n){ memcpy(hp(g,b),p,n-1); wr8(g,b+n-1,0); } kh_set_last_error(122); SET_RAX(g,n); return; } memcpy(hp(g,b),p,l+1); SET_RAX(g,l); }

/* ---- module loader ------------------------------------------------------------------------------------------------------ */
#define MAXPROC 256
static struct { char dll[64]; char name[96]; u64 addr; } procs[MAXPROC]; static int nprocs;
void kh_register_proc(const char *dll,const char *name,u64 addr){ if(nprocs<MAXPROC){ snprintf(procs[nprocs].dll,64,"%s",dll); snprintf(procs[nprocs].name,96,"%s",name); procs[nprocs].addr=addr; nprocs++; } }
static const char *known_dlls[]={"kernel32.dll","kernelbase.dll","user32.dll","ntdll.dll","xaudio2_9.dll","xaudio2_8.dll","xinput1_4.dll","xinput9_1_0.dll","dinput8.dll","d3d12.dll","dxgi.dll","d3d11.dll","mfplat.dll","mfreadwrite.dll","advapi32.dll","ole32.dll","shell32.dll","winmm.dll","d3dcompiler_47.dll","steam_api64.dll",0};
static int dll_index(const char *n){ char b[128]; size_t i=0; for(;n[i]&&i<127;i++) b[i]=(char)tolower((unsigned char)n[i]); b[i]=0; const char *base=strrchr(b,'\\'); if(base) base++; else base=b; for(int k=0;known_dlls[k];k++) if(!strcmp(known_dlls[k],base)) return k+1; return 0; }
IMP(K32,LoadLibraryExW){ char n[260]; gw_to_utf8(g,A0(g),n,sizeof n); int i=dll_index(n); if(!i){ kh_warn("LoadLibraryExW(\"%s\") -> not found",n); kh_set_last_error(126); SET_RAX(g,0); return; } SET_RAX(g,0x7ffa00000000ull+0x10000ull*(u64)i); }
IMP(K32,GetModuleHandleW){ if(!A0(g)){ SET_RAX(g,GS_BASE); return; } char n[260]; gw_to_utf8(g,A0(g),n,sizeof n); int i=dll_index(n); if(!i){ kh_set_last_error(126); SET_RAX(g,0); return; } SET_RAX(g,0x7ffa00000000ull+0x10000ull*(u64)i); }
IMP(K32,GetProcAddress){ u64 h=A0(g); const char *nm=gstr(g,A1(g)); char nb[32]; if(A1(g)<0x10000){ snprintf(nb,sizeof nb,"#%llu",(unsigned long long)A1(g)); nm=nb; }
    int idx=(int)((h-0x7ffa00000000ull)>>16); const char *dll=(idx>=1&&known_dlls[idx-1])?known_dlls[idx-1]:"?";
    for(int i=0;i<nprocs;i++) if(!strcasecmp(procs[i].dll,dll)&&!strcmp(procs[i].name,nm)){ SET_RAX(g,procs[i].addr); return; }
    kh_warn("GetProcAddress(%s, %s) -> NULL (no runtime implementation)",dll,nm); kh_set_last_error(127); SET_RAX(g,0); }

/* ---- files (HANDLE based) ------------------------------------------------------------------------------------------------ */
IMP(K32,CreateFileA){ char p[2048]; int mapped=kh_winpath(gstr(g,A0(g)),p,sizeof p); u32 acc=(u32)A1(g),disp=(u32)A4(g); int fl=0;
    if((acc&0x80000000u)&&(acc&0x40000000u)) fl=O_RDWR; else if(acc&0x40000000u) fl=O_WRONLY; else fl=O_RDONLY;
    switch(disp){ case 1: fl|=O_CREAT|O_EXCL; break; case 2: fl|=O_CREAT|O_TRUNC; break; case 4: fl|=O_CREAT; break; case 5: fl|=O_TRUNC; break; default: break; }
    int fd=open(p,fl,0644); if(kh_verbose||fd<0) kh_warn("file: CreateFileA(\"%s\" -> \"%s\") = %d",gstr(g,A0(g)),p,fd); if(fd<0){ kh_set_last_error(errno==ENOENT?(mapped?2:3):(errno==EEXIST?80:5)); SET_RAX(g,INVALID_HANDLE); return; }
    KFile *f=calloc(1,sizeof *f); f->fd=fd; snprintf(f->path,sizeof f->path,"%s",p); SET_RAX(g,h_new(HK_FILE,f)); }
IMP(K32,CreateFileW){ char p[2048]; int mapped=kh_winpath_w(g,A0(g),p,sizeof p); u32 acc=(u32)A1(g),disp=(u32)A4(g); int fl;
    if((acc&0x80000000u)&&(acc&0x40000000u)) fl=O_RDWR; else if(acc&0x40000000u) fl=O_WRONLY; else fl=O_RDONLY;
    switch(disp){ case 1: fl|=O_CREAT|O_EXCL; break; case 2: fl|=O_CREAT|O_TRUNC; break; case 4: fl|=O_CREAT; break; case 5: fl|=O_TRUNC; break; default: break; }
    int fd=open(p,fl,0644); if(fd<0){ kh_set_last_error(errno==ENOENT?(mapped?2:3):5); SET_RAX(g,INVALID_HANDLE); return; }
    KFile *f=calloc(1,sizeof *f); f->fd=fd; snprintf(f->path,sizeof f->path,"%s",p); SET_RAX(g,h_new(HK_FILE,f)); }
IMP(K32,ReadFile){ KFile *f=h_get(A0(g),HK_FILE); if(!f){ kh_set_last_error(6); SET_RAX(g,0); return; } ssize_t n=read(f->fd,hp(g,A1(g)),(size_t)(u32)A2(g)); if(n<0){ kh_set_last_error(5); SET_RAX(g,0); return; } if(A3(g)) wr32(g,A3(g),(u32)n); SET_RAX(g,1); }
IMP(K32,WriteFile){ KFile *f=h_get(A0(g),HK_FILE); if(!f){ kh_set_last_error(6); SET_RAX(g,0); return; } ssize_t n=write(f->fd,hp(g,A1(g)),(size_t)(u32)A2(g)); if(n<0){ kh_set_last_error(5); SET_RAX(g,0); return; } if(A3(g)) wr32(g,A3(g),(u32)n); SET_RAX(g,1); }
IMP(K32,CopyFileA){ char s[2048],d[2048]; kh_winpath(gstr(g,A0(g)),s,sizeof s); kh_winpath(gstr(g,A1(g)),d,sizeof d); int in=open(s,O_RDONLY); if(in<0){ kh_set_last_error(2); SET_RAX(g,0); return; }
    int out=open(d,O_WRONLY|O_CREAT|((A2(g)&1)?O_EXCL:O_TRUNC),0644); if(out<0){ close(in); kh_set_last_error(80); SET_RAX(g,0); return; } char buf[65536]; ssize_t n; while((n=read(in,buf,sizeof buf))>0) if(write(out,buf,(size_t)n)!=n) break; close(in); close(out); SET_RAX(g,1); }
IMP(K32,DeleteFileA){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); int r=unlink(p); if(r) kh_set_last_error(2); SET_RAX(g,r==0); }
IMP(K32,CreateDirectoryA){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); int r=mkdir(p,0755); if(r) kh_set_last_error(errno==EEXIST?183:3); SET_RAX(g,r==0); }
IMP(K32,RemoveDirectoryA){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); int r=rmdir(p); if(r) kh_set_last_error(3); SET_RAX(g,r==0); }
IMP(K32,CreateFileMappingA){ /* (file, attrs, protect, hi, lo, name) : only anonymous (INVALID_HANDLE_VALUE) mappings are supported (exit-code mailbox) */
    if(A0(g)!=INVALID_HANDLE) kh_fatal(g,"CreateFileMappingA on a real file is not supported");
    u64 size=((A3(g)&0xffffffffull)<<32)|(A4(g)&0xffffffffull); KMap *m=calloc(1,sizeof *m); m->size=size?size:4096; m->ga=ga_pages_src(m->size,GA_SRC_MAP); if(ARG(g,5)) snprintf(m->name,sizeof m->name,"%s",gstr(g,ARG(g,5))); SET_RAX(g,h_new(HK_MAPPING,m)); }
IMP(K32,MapViewOfFile){ KMap *m=h_get(A0(g),HK_MAPPING); if(!m){ kh_set_last_error(6); SET_RAX(g,0); return; } SET_RAX(g,m->ga); }
IMP(K32,UnmapViewOfFile){ SET_RAX(g,1); }
/* task 44: the two file checks of the movie path are answered by the movie seam in its default mode (runtime/null_mf.c): a handle with
 * no directory behind it, the name as asked for, nothing to enumerate */
int kh_movie_find(KCtx *g,const char *win,const char *host,u64 site_rva);
IMP(K32,FindFirstFileA){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p);
    if(kh_movie_find(g,gstr(g,A0(g)),p,guest_return_addr(g)-GS_BASE)){ KFind *mf=calloc(1,sizeof *mf); const char *w=gstr(g,A0(g)), *a=strrchr(w,'/'), *b=strrchr(w,'\\'); if(b>a) a=b;
        u64 data=A1(g); memset(hp(g,data),0,0x140); wr32(g,data,0x80); strncpy((char*)hp(g,data+0x2c),a?a+1:w,259); SET_RAX(g,h_new(HK_FIND,mf)); return; }
    char *sl=strrchr(p,'/'); KFind *f=calloc(1,sizeof *f); if(sl){ *sl=0; snprintf(f->dir,sizeof f->dir,"%s",p); snprintf(f->pat,sizeof f->pat,"%s",sl+1); } else { snprintf(f->dir,sizeof f->dir,"."); snprintf(f->pat,sizeof f->pat,"%s",p); }
    f->d=opendir(f->dir); if(!f->d){ free(f); kh_set_last_error(3); SET_RAX(g,INVALID_HANDLE); return; }
    u64 h=h_new(HK_FIND,f); u64 data=A1(g); struct dirent *de; for(;;){ de=readdir(f->d); if(!de){ h_close(h); kh_set_last_error(2); SET_RAX(g,INVALID_HANDLE); return; }
        if(fnmatch(f->pat,de->d_name,FNM_CASEFOLD)==0) break; }
    char full[2200]; snprintf(full,sizeof full,"%s/%s",f->dir,de->d_name); struct stat st; stat(full,&st); memset(hp(g,data),0,0x140); wr32(g,data,S_ISDIR(st.st_mode)?0x10:0x80); wr32(g,data+28,(u32)((u64)st.st_size>>32)); wr32(g,data+32,(u32)st.st_size);
    strncpy((char*)hp(g,data+0x2c),de->d_name,259); SET_RAX(g,h); }
IMP(K32,FindNextFileA){ KFind *f=h_get(A0(g),HK_FIND); if(!f){ SET_RAX(g,0); return; } if(!f->d){ kh_set_last_error(18); SET_RAX(g,0); return; } u64 data=A1(g); struct dirent *de; for(;;){ de=readdir(f->d); if(!de){ kh_set_last_error(18); SET_RAX(g,0); return; } if(fnmatch(f->pat,de->d_name,FNM_CASEFOLD)==0) break; }
    char full[2200]; snprintf(full,sizeof full,"%s/%s",f->dir,de->d_name); struct stat st; stat(full,&st); memset(hp(g,data),0,0x140); wr32(g,data,S_ISDIR(st.st_mode)?0x10:0x80); wr32(g,data+28,(u32)((u64)st.st_size>>32)); wr32(g,data+32,(u32)st.st_size);
    strncpy((char*)hp(g,data+0x2c),de->d_name,259); SET_RAX(g,1); }
IMP(K32,FindClose){ SET_RAX(g,h_close(A0(g))); }
IMP(imagehlp,MakeSureDirectoryPathExists){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); for(char *q=p+1;*q;q++) if(*q=='/'){ *q=0; mkdir(p,0755); *q='/'; } SET_RAX(g,1); }
IMP(dbghelp,MiniDumpWriteDump){ SET_RAX(g,0); }

/* ---- launcher process enumeration (WaitTitleProject.exe control): there is never another process ------------------------- */
IMP(K32,CreateToolhelp32Snapshot){ SET_RAX(g,INVALID_HANDLE); kh_set_last_error(5); }
IMP(K32,Process32First){ SET_RAX(g,0); }
IMP(K32,Process32Next){ SET_RAX(g,0); }
IMP(K32,OpenProcess){ SET_RAX(g,0); kh_set_last_error(5); }

/* ---- winmm ---------------------------------------------------------------------------------------------------------------- */
IMP(winmm,timeBeginPeriod){ SET_RAX(g,0); }
IMP(winmm,timeEndPeriod){ SET_RAX(g,0); }
IMP(winmm,timeGetDevCaps){ if(A0(g)){ wr32(g,A0(g),1); wr32(g,A0(g)+4,1000000); } SET_RAX(g,0); }
static u64 t0ms; IMP(winmm,timeGetTime){ if(kh_cut_enabled){ SET_RAX(g,(u32)(1000+kh_cut_elapsed_ns()/1000000)); return; } u64 n=kh_now_ns()/1000000; if(!t0ms) t0ms=n; SET_RAX(g,(u32)(n-t0ms+1000)); }
typedef struct Timer { u32 id; u32 delay; u64 cb, user; u32 flags; volatile int killed; struct Timer *next;
    u64 fires, settled, no_consumer, guard_hits; u64 seth[4]; u64 setsat[4]; int setwait[4]; int nset; KThread *owner; int owner_seq; int anchor; } Timer;   /* anchor: review of task 28, 1 = created before the first vblank and anchored there, -1 = not anchored (guard) */   /* frames clock: consumer tracking, same-instant order (TASK28) */
static Timer *timers; static u32 next_timer=1; static pthread_mutex_t tmm=PTHREAD_MUTEX_INITIALIZER;
/* A multimedia timer fires on a fixed grid (start + n*period), it does not drift by the time its handler takes: absolute deadlines.
 * A timer that fell behind by more than a period drops the missed periods, as Windows does.  TIME_CALLBACK_EVENT_SET (0x10) is
 * SetEvent, TIME_CALLBACK_EVENT_PULSE (0x20) is PulseEvent (the GX frame timer: period timer pulsing an auto-reset event). */
static void timer_fire(KCtx *g,Timer *t){
    if(t->flags&0x20){ if(!kh_event_pulse(t->cb)) kh_warn("winmm timer %u: pulse of a closed or invalid event handle %llx",t->id,(unsigned long long)t->cb); }
    else if(t->flags&0x10){ if(!kh_event_set(t->cb)) kh_warn("winmm timer %u: set of a closed or invalid event handle %llx",t->id,(unsigned long long)t->cb); }
    else {
        if(kh_cut_enabled) kh_cut_callback_depth++;
        kh_call5(g,t->cb,t->id,0,t->user,0,0);
        if(kh_cut_enabled) kh_cut_callback_depth--;
    }
}
/* KHNMC_CLOCK=frames: the timer's grid is on the virtual clock.
 * Before TASK28 the timer thread woke whenever the clock passed its next grid instant and fired ONCE (missed periods dropped), while the
 * clock moved on: the sound driver's handlers ("CDevSd MSECHandler" and "CDevSd VSYNCHandler", two 16 ms timers; the loop 0xfb040:
 * wait for the semaphore, run the tick, wait again; the timer callback 0xfb020 releases that semaphore of maximum count 1) then ran at
 * whatever virtual time the clock had reached, concurrently with the mixer's passes.  Nearly every XAudio2 call of the game comes from
 * those handlers (TASK28 audio logs), so their timing in host terms decided which pass a call reached.
 * Now each timer is a CLOCK CLIENT (after the mixer's pass of the same instant; timers of one instant in the order of their creating
 * threads' names, kh_vclock_thread_key, not of their ids, which thread start-up order assigns): the clock stops at every grid
 * instant (no period is dropped: a 1 ms timer fires 1000 times per virtual second), the timer fires, and it announces its next instant
 * only when the consumer of what it signalled is back in its wait (kh_wobj_settle: the wait was satisfied after the fire and the
 * consumer blocks on the event again).  So a handler's tick runs entirely at its own instant: its QueryPerformanceCounter reads that
 * instant and every call it makes is stamped with it.  A consumer that is not waiting when the timer fires (none yet, or busy with
 * something else) is not waited for (counted: no-consumer); one that is not back within VC_TIMER_GUARD_MS of host time is let go
 * (counted: guard) so that a handler that waits for virtual time to pass cannot stop the clock.  Both counts are in the report. */
#define VC_TIMER_GUARD_MS 5000u
static _Atomic u64 tm_fires, tm_settled, tm_noconsumer, tm_guard; static Timer *tm_list_frames[16]; static int tm_nframes;
static void timer_set_hook(void *ctx,u64 h){ Timer *t=ctx; if(t->nset>=4) return;
    int w=0; u64 sat=0; if(kh_wobj_waitstate(h,&w,&sat)) return;
    t->seth[t->nset]=h; t->setwait[t->nset]=w; t->setsat[t->nset]=sat; t->nset++; }
static _Atomic u64 tm_anchored, tm_unanchored;
static void timer_proc_virtual(KCtx *g,Timer *t){
    /* review of task 28: a timer created before the first WaitForVBlank used to start its grid at the host instant of creation, a phase
     * against the vblanks that depends on the host.  It now waits for the first vblank (not idle: under sync=all the clock cannot leave
     * its origin before this thread has registered there) and its grid starts at the origin, as if created there.  A timer that sees no
     * vblank within the guard keeps the host grid: named here and in the report. */
    if(!kh_vclock_stepped()){ u64 h0=kh_host_ns();
        while(!kh_vclock_stepped()&&!t->killed&&kh_host_ns()-h0<KH_VCLOCK_ANCHOR_GUARD_NS) kh_vclock_wait_stepped(10000000ull);
        if(kh_vclock_stepped()) t->anchor=1;
        else if(!t->killed){ t->anchor=-1; tm_unanchored++; kh_warn("vclock: winmm timer %u (%u ms) was created before the first WaitForVBlank and none came within %.0f s of host time: its grid follows the host (not anchored, a determinism break)",t->id,t->delay,KH_VCLOCK_ANCHOR_GUARD_NS/1e9); } }
    u64 per=(u64)t->delay*1000000ull, now0=kh_vclock_now_ns(), start=t->anchor==1?kh_vclock_origin():now0, next=start+per;
    if(t->anchor==1&&next<=now0) next=start+((now0-start)/per+1)*per;                 /* sync=timers: the clock may have moved on meanwhile */
    char nm[32]; snprintf(nm,sizeof nm,"winmm-timer-%u",t->id);
    int cid=kh_vclock_client_add_prio(strdup(nm),next,16);
    kh_vclock_client_set_key(cid,16,t->owner,t->owner_seq);                            /* the creating thread's name / start routine (read at each comparison), then its own creation order */
    if(t->anchor==1) tm_anchored++;                                                    /* counted once registered at the origin (the selftest waits for it) */
    if(tm_nframes<16) tm_list_frames[tm_nframes++]=t;
    for(;;){
        kh_vclock_client_wait(cid,next);                                                /* the clock is held at `next` until this client announces a later instant */
        if(t->killed) break;
        t->nset=0;
        if(t->flags&0x30) timer_set_hook(t,t->cb);                                      /* SetEvent / PulseEvent by the timer itself */
        else kh_set_event_hook(timer_set_hook,t);                                       /* a callback: what it signals (0xfb020: ReleaseSemaphore) */
        timer_fire(g,t); kh_set_event_hook(NULL,NULL); t->fires++; tm_fires++;
        for(int i=0;i<t->nset;i++){
            if(t->setwait[i]<=0){ t->no_consumer++; tm_noconsumer++; continue; }
            int rc=kh_wobj_settle(t->seth[i],t->setsat[i],t->setwait[i],VC_TIMER_GUARD_MS,&t->killed);
            if(rc==1){ t->guard_hits++; tm_guard++; kh_warn("vclock: winmm timer %u: the consumer of event %llx was not back in its wait %u ms of host time after the tick at virtual %llu ns: the clock goes on without it",t->id,(unsigned long long)t->seth[i],VC_TIMER_GUARD_MS,(unsigned long long)next); }
            else { t->settled++; tm_settled++; }
        }
        if(!(t->flags&1)) break;
        u64 now=kh_vclock_now_ns(); next=start+((now-start)/per+1)*per;
        kh_vclock_client_due(cid,next);
    }
    kh_vclock_client_drop(cid);
}
u64 kh_timer_anchor_counts(u64 *unanchored){ if(unanchored) *unanchored=tm_unanchored; return tm_anchored; }
void kh_timer_frames_report(FILE *f){
    if(!kh_vclock_on||!tm_nframes) return;
    fprintf(f,"kh: vclock: winmm timers as clock clients: %llu ticks, %llu waited until their consumer was back in its wait, %llu without a waiting consumer, %llu let go by the %u ms guard; created before the first vblank: %llu anchored at it, %llu NOT anchored;",
            (unsigned long long)tm_fires,(unsigned long long)tm_settled,(unsigned long long)tm_noconsumer,(unsigned long long)tm_guard,VC_TIMER_GUARD_MS,(unsigned long long)tm_anchored,(unsigned long long)tm_unanchored);
    for(int i=0;i<tm_nframes;i++){ Timer *t=tm_list_frames[i]; fprintf(f," timer %u (%u ms, flags %x): %llu ticks / %llu settled / %llu no consumer / %llu guard%s%s",t->id,t->delay,t->flags,(unsigned long long)t->fires,(unsigned long long)t->settled,(unsigned long long)t->no_consumer,(unsigned long long)t->guard_hits,t->anchor==1?" (created before the first vblank, anchored at it)":t->anchor<0?" (NOT anchored)":"",i+1<tm_nframes?";":""); }
    fputc('\n',f);
}
static void timer_proc(KCtx *g){
    Timer *t;
    if(kh_cut_enabled){
        /* The thread parameter survives in guest registers across callbacks. */
        u64 id=A0(g);
        pthread_mutex_lock(&tmm);
        for(t=timers;t&&t->id!=id;t=t->next){}
        pthread_mutex_unlock(&tmm);
        if(!t) kh_fatal(g,"savestate-timer-binding: unknown id %llu",(unsigned long long)id);
    }else t=(Timer*)(uintptr_t)A0(g);
    if(kh_vclock_on&&!ga_deterministic){ timer_proc_virtual(g,t); KH_RET(g); return; }
    struct timespec next; clock_gettime(CLOCK_MONOTONIC,&next);
    for(;;){
        if(ga_deterministic){ struct timespec ts={t->delay/1000,(long)(t->delay%1000)*1000000L}; nanosleep(&ts,0); }
        else { next.tv_sec+=t->delay/1000; next.tv_nsec+=(long)(t->delay%1000)*1000000L; if(next.tv_nsec>=1000000000L){ next.tv_sec++; next.tv_nsec-=1000000000L; }
            struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
            s64 late=((s64)now.tv_sec-next.tv_sec)*1000000000ll+((s64)now.tv_nsec-next.tv_nsec);
            if(late>(s64)t->delay*1000000ll) next=now;                                      /* more than one period behind: resynchronise */
            else kh_sleep_until(&next); }
        if(t->killed) break;
        timer_fire(g,t);
        if(!(t->flags&1)) break; }
    KH_RET(g); }
IMP(winmm,timeSetEvent){ Timer *t=calloc(1,sizeof *t); t->owner=cur_thread(); t->owner_seq=t->owner?t->owner->vc_ntimers++:0; t->delay=(u32)A0(g)?(u32)A0(g):1; t->cb=A2(g); t->user=A3(g); t->flags=(u32)ARG(g,4); pthread_mutex_lock(&tmm); t->id=next_timer++; t->next=timers; timers=t; pthread_mutex_unlock(&tmm);
    kh_spawn_hostfn_thread(g,"winmm_timer",timer_proc,kh_cut_enabled?t->id:(u64)(uintptr_t)t); SET_RAX(g,t->id); }
IMP(winmm,timeKillEvent){ u32 id=(u32)A0(g); int ok=1; pthread_mutex_lock(&tmm); for(Timer *t=timers;t;t=t->next) if(t->id==id){ t->killed=1; ok=0; } pthread_mutex_unlock(&tmm); SET_RAX(g,ok?0xffffffffu:0); }

/* ---- advapi32 registry: in-memory key/value store (documented stub; HKCU\Software\SQUARE\KHProj) --------------------------- */
typedef struct RegV { char key[512]; u32 type; u8 *data; u32 size; struct RegV *next; } RegV; static RegV *regvals; static pthread_mutex_t rgm=PTHREAD_MUTEX_INITIALIZER;
IMP(advapi32,RegCreateKeyExA){ char *k=strdup(gstr(g,A1(g))); u64 h=h_new(HK_HEAP,k); wr64(g,ARG(g,7),h); if(ARG(g,8)) wr32(g,ARG(g,8),1); SET_RAX(g,0); }
IMP(advapi32,RegCloseKey){ h_close(A0(g)); SET_RAX(g,0); }
IMP(advapi32,RegSetValueExA){ char *k=h_get(A0(g),HK_HEAP); if(!k){ SET_RAX(g,6); return; } RegV *v=calloc(1,sizeof *v); snprintf(v->key,sizeof v->key,"%s\\%s",k,gstr(g,A1(g))); v->type=(u32)A3(g); v->size=(u32)ARG(g,5); v->data=malloc(v->size+1); memcpy(v->data,hp(g,ARG(g,4)),v->size);
    pthread_mutex_lock(&rgm); v->next=regvals; regvals=v; pthread_mutex_unlock(&rgm); SET_RAX(g,0); }
IMP(advapi32,RegQueryValueExA){ char *k=h_get(A0(g),HK_HEAP); char key[512]; if(!k){ SET_RAX(g,6); return; } snprintf(key,sizeof key,"%s\\%s",k,gstr(g,A1(g)));
    pthread_mutex_lock(&rgm); RegV *f=NULL; for(RegV *v=regvals;v;v=v->next) if(!strcmp(v->key,key)){ f=v; break; } pthread_mutex_unlock(&rgm);
    if(!f){ SET_RAX(g,2); return; } if(A3(g)) wr32(g,A3(g),f->type); if(A4(g)&&A5(g)){ u32 cap=rd32(g,A5(g)); if(cap<f->size){ wr32(g,A5(g),f->size); SET_RAX(g,234); return; } memcpy(hp(g,A4(g)),f->data,f->size); } if(A5(g)) wr32(g,A5(g),f->size); SET_RAX(g,0); }

/* ---- shell32 / ole32 / oleaut32 ----------------------------------------------------------------------------------------- */
IMP(shell32,SHGetFolderPathA){ strcpy((char*)hp(g,ARG(g,4)),WIN_DOCS); SET_RAX(g,0); }
IMP(shell32,ShellExecuteA){ kh_warn("ShellExecuteA(%s) ignored",A2(g)?gstr(g,A2(g)):"(null)"); SET_RAX(g,33); }
IMP(ole32,CoInitialize){ SET_RAX(g,0); }
IMP(ole32,CoInitializeEx){ SET_RAX(g,0); }
IMP(ole32,CoUninitialize){ }
IMP(ole32,CoSetProxyBlanket){ SET_RAX(g,0); }
IMP(ole32,CoTaskMemAlloc){ SET_RAX(g,ga_alloc_src(A0(g),16,GA_SRC_HEAP)); }
IMP(ole32,CoTaskMemFree){ ga_free(A0(g)); }
IMP(ole32,PropVariantClear){ SET_RAX(g,0); }
IMP(ole32,CoCreateInstance){ if(kh_audio_cocreate(g)) return; kh_warn("CoCreateInstance(clsid %08x-%04x...) -> REGDB_E_CLASSNOTREG (site rva 0x%llx)",rd32(g,A0(g)),rd16(g,A0(g)+4),(unsigned long long)(guest_return_addr(g)-GS_BASE)); if(ARG(g,4)) wr64(g,ARG(g,4),0); SET_RAX(g,0x80040154u); }
IMP(oleaut32,SysAllocString){ u64 s=A0(g); if(!s){ SET_RAX(g,0); return; } size_t n=gw_len(g,s); u64 b=ga_alloc_src(4+2*n+2,16,GA_SRC_HEAP); wr32(g,b,(u32)(2*n)); memcpy(hp(g,b+4),hp(g,s),2*n+2); SET_RAX(g,b+4); }
IMP(oleaut32,SysFreeString){ if(A0(g)) ga_free(A0(g)-4); }
