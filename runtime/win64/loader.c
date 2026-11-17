/* loader.c : PE image loader + process driver (main).
 *
 * Address model (KH_XLATE_MODE 0, identity): one MAP_FIXED_NOREPLACE reservation of "guest space" at 0x140000000 holds the image at
 * its preferred base (so no relocation is ever needed) followed by the guest arena (heap, thread and fiber stacks; layout in core.c).
 * The reservation is PROT_NONE; the image is committed here, the arena commits itself as it grows.
 * Fallback for hosts that cannot map that address (iOS): build with KH_XLATE_MODE=2 (host = guest + bias), map guest space anywhere,
 * set gs_bias = host - 0x140000000 in every KCtx; guest addresses (and so relocations, IAT values, vtables, saved pointers) never change.
 */
#include "khnmc_rt.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

void ga_init(u64 lo,u64 hi); void kh_trace_init(void); void kh_install_signals(void); u64 kh_trace_count(void);
void kh_set_cur_g(KCtx *g);
extern u64 kh_import_addr_base;
extern int kh_cnt_init_xi, kh_cnt_init_xc;
#if KH_IOS_PATHS
static char exe_path[1024];                   /* always from KH_EXE: host_env.c sets the app container default (Documents/kh2fm.exe) */
#else
static char exe_path[1024]="/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe";
#endif
static const char *entry_mode="crt";
static u64 guest_space_size=0x300000000ull;   /* 12 GiB reserve, lazily committed */
static int exit_code_main=-1; static int selftest; static int hd_check; static const char *hd_check_report;

#define RD16(p) (*(const u16*)(p))
#define RD32(p) (*(const u32*)(p))
static int load_image(void){
    int fd=open(exe_path,O_RDONLY); if(fd<0){ perror(exe_path); return -1; }
    struct stat st; fstat(fd,&st); u8 *f=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd); if(f==MAP_FAILED){ perror("mmap exe"); return -1; }
    if(RD16(f)!=0x5a4d){ fprintf(stderr,"not an MZ file\n"); return -1; }
    u32 pe=RD32(f+0x3c); if(RD32(f+pe)!=0x4550){ fprintf(stderr,"no PE signature\n"); return -1; }
    const u8 *fh=f+pe+4; u16 machine=RD16(fh), nsec=RD16(fh+2), optsz=RD16(fh+16); const u8 *oh=fh+20;
    if(machine!=0x8664||RD16(oh)!=0x20b){ fprintf(stderr,"not PE32+ x64\n"); return -1; }
    u64 base=*(const u64*)(oh+24); u32 szimg=RD32(oh+56), szhdr=RD32(oh+60);
    if(base!=GS_BASE||szimg!=IMG_SIZE){ fprintf(stderr,"unexpected image base/size %llx %x\n",(unsigned long long)base,szimg); return -1; }
    /* guest space */
    void *want=(void*)GS_BASE; int flags=MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE;
    u64 image_hi=(GS_BASE+IMG_SIZE+0x100000+0xffffull)&~0xffffull;      /* image + 1 MiB of slack, 64 KiB aligned: the arena starts here */
#if KH_XLATE_MODE==0
    void *m=mmap(want,guest_space_size,PROT_NONE,flags|MAP_FIXED_NOREPLACE,-1,0);
    if(m!=want){ fprintf(stderr,"kh: cannot map guest space at 0x%llx (%s). Build with KH_XLATE_MODE=2 for bias mode.\n",(unsigned long long)GS_BASE,strerror(errno)); return -1; }
    gs_bias=0;
    if(mprotect(m,image_hi-GS_BASE,PROT_READ|PROT_WRITE|PROT_EXEC)){ perror("kh: commit image"); return -1; }
#else
    void *m=mmap(0,guest_space_size,PROT_NONE,flags,-1,0);
    if(m==MAP_FAILED){ perror("mmap guest space"); return -1; } gs_bias=(uintptr_t)m-GS_BASE;
    if(mprotect(m,image_hi-GS_BASE,PROT_READ|PROT_WRITE)){ perror("kh: commit image"); return -1; }
#endif
    gs_host_base=(uintptr_t)m; gs_size=guest_space_size;
#if KH_XLATE_MODE!=0
    fprintf(stderr,"kh: guest space: bias mode, guest 0x%llx..0x%llx is host 0x%llx..0x%llx (bias 0x%llx), host page %lu bytes\n",(unsigned long long)GS_BASE,(unsigned long long)(GS_BASE+guest_space_size),
            (unsigned long long)gs_host_base,(unsigned long long)(gs_host_base+guest_space_size),(unsigned long long)gs_bias,(unsigned long)sysconf(_SC_PAGESIZE));
#endif
#ifdef MADV_DONTDUMP
    madvise(m,guest_space_size,MADV_DONTDUMP);
#endif
    memcpy((void*)(gs_host_base),f,szhdr);
    const u8 *sh=oh+optsz;
    for(int i=0;i<nsec;i++,sh+=40){ u32 va=RD32(sh+12), vsz=RD32(sh+8), raw=RD32(sh+20), rsz=RD32(sh+16); (void)vsz; if(rsz&&raw) memcpy((void*)(gs_host_base+va),f+raw,rsz); }
    /* IAT: the synthetic import addresses the lifter expects (kh_import_table order = PE import order) */
    for(u32 i=0;i<kh_import_count;i++){ u64 slot=GS_BASE+kh_import_table[i].slot_rva; u64 v=kh_import_addr_base+16ull*i; memcpy((void*)(slot+gs_bias),&v,8); }
    /* TLS directory */
    u32 tlsrva=RD32(oh+112+9*8); if(tlsrva){ const u8 *t=(const u8*)(gs_host_base+tlsrva);
        kh_tls_tmpl_lo=*(const u64*)t; kh_tls_tmpl_hi=*(const u64*)(t+8); kh_tls_index_va=*(const u64*)(t+16); kh_tls_callbacks_va=*(const u64*)(t+24); }
    munmap(f,st.st_size);
    ga_init(image_hi, GS_BASE+guest_space_size);
    return 0;
}

void kh_exit_process(int code){
    extern void kh_report_seams(FILE*);
    { u64 lb,ac; extern void ga_stats(u64*,u64*); ga_stats(&lb,&ac); fprintf(stderr,"kh: process exit(%d) after %llu import calls; init tables run: %d __xi + %d __xc entries; guest heap live %llu bytes in %llu allocations\n",code,(unsigned long long)kh_trace_count(),kh_cnt_init_xi,kh_cnt_init_xc,(unsigned long long)lb,(unsigned long long)ac); }
    { extern void kh_mod_difficulty_dump(void); kh_mod_difficulty_dump(); }
    { extern void kh_mod_hooks_dump(void); kh_mod_hooks_dump(); }
    { extern void kh_bgload_report(FILE*); kh_bgload_report(stderr); }
    { extern void kh_moose_report(FILE*); kh_moose_report(stderr); }
    { extern void kh_idtable_exit_report(FILE*); kh_idtable_exit_report(stderr); }
    { extern void kh_sync_report(FILE*); kh_sync_report(stderr); }
    { extern void kh_input_report(FILE*); kh_input_report(stderr); }
    kh_vclock_report(stderr);
    { extern void kh_gfx_vkd3d_finish(void); kh_gfx_vkd3d_finish(); }
    { extern void kh_import_counts_report(FILE*); kh_import_counts_report(stderr); }      /* KH_IMPORT_COUNTS=1 */
    { extern void kh_import_sites_report(FILE*); kh_import_sites_report(stderr); }        /* KH_IMPORT_SITES=text */
    { extern void kh_libm_args_close(void); kh_libm_args_close(); }                       /* KH_LIBM_ARGS=file */
    kh_report_seams(stderr); { extern void kh_dump_threads(FILE*); if(getenv("KH_DUMP_THREADS")) kh_dump_threads(stderr); } fflush(stderr); fflush(stdout); kh_host_exit(code);
}

/* oracle snapshot: after the main thread is set up, write the sparse guest arena (every non-zero 4 KiB page) + boot state so an independent
 * emulation of the ORIGINAL bytes (tools/oracle_unicorn.py) starts from exactly the same memory. KH_SNAPSHOT=prefix -> prefix.json / prefix.bin */
extern u64 kh_steam_objs[9]; u64 ga_bump_ptr(void); u64 ga_arena_lo(void); u32 kh_host_fn_count(void); const char *kh_host_fn_name(u64 a);
static void write_snapshot(KCtx *g,KThread *t,const char *prefix){
    char fn[1100]; snprintf(fn,sizeof fn,"%s.bin",prefix); FILE *bin=fopen(fn,"wb"); snprintf(fn,sizeof fn,"%s.json",prefix); FILE *js=fopen(fn,"w"); if(!bin||!js) return;
    u64 lo=ga_arena_lo(),hi=(ga_bump_ptr()+0xfff)&~0xfffull; u64 off=0; int first=1;
    fprintf(js,"{\"rsp\":%llu,\"teb\":%llu,\"tlsarr\":%llu,\"tlsblk\":%llu,\"bump\":%llu,\"arena_lo\":%llu,\"arena_hi\":%llu,\"steam\":[",(unsigned long long)KH_RSP(g),(unsigned long long)t->teb,(unsigned long long)t->tlsarr,(unsigned long long)t->tlsblk,(unsigned long long)ga_bump_ptr(),(unsigned long long)lo,(unsigned long long)hi);
    for(int i=0;i<9;i++) fprintf(js,"%s%llu",i?",":"",(unsigned long long)kh_steam_objs[i]);
    fprintf(js,"],\"hostfn\":["); for(u32 i=0;i<kh_host_fn_count();i++) fprintf(js,"%s\"%s\"",i?",":"",kh_host_fn_name(KH_HOSTFN_BASE+16ull*i));
    fprintf(js,"],\"pages\":[");
    for(u64 a=lo&~0xfffull;a<hi;a+=4096){ const u8 *p=(const u8*)XL(g,a); int nz=0; for(int i=0;i<4096;i+=8) if(*(const u64*)(p+i)){ nz=1; break; } if(!nz) continue; fprintf(js,"%s[%llu,%llu]",first?"":",",(unsigned long long)a,(unsigned long long)off); first=0; fwrite(p,1,4096,bin); off+=4096; }
    fprintf(js,"]}\n"); fclose(js); fclose(bin); fprintf(stderr,"kh: snapshot written (%llu bytes of non-zero pages)\n",(unsigned long long)off);
}
static void *guest_main(void *arg){
    (void)arg; kh_thread_init_main(); KThread *t=cur_thread(); KCtx *g=t->g;
    fprintf(stderr,"kh: guest main thread up: image 0x%llx..0x%llx, arena 0x%llx.., TEB 0x%llx, TLS template %llx..%llx idx@%llx callbacks@%llx\n",
            (unsigned long long)GS_BASE,(unsigned long long)(GS_BASE+IMG_SIZE),(unsigned long long)(GS_BASE+IMG_SIZE+0x100000),(unsigned long long)t->teb,
            (unsigned long long)kh_tls_tmpl_lo,(unsigned long long)kh_tls_tmpl_hi,(unsigned long long)kh_tls_index_va,(unsigned long long)kh_tls_callbacks_va);
    { extern u64 ga_bump_ptr(void); fprintf(stderr,"kh: boot-state rsp=0x%llx teb=0x%llx tlsarr=0x%llx tlsblk=0x%llx bump=0x%llx det=%d\n",(unsigned long long)KH_RSP(g),(unsigned long long)t->teb,(unsigned long long)t->tlsarr,(unsigned long long)t->tlsblk,(unsigned long long)ga_bump_ptr(),ga_deterministic); }
    if(getenv("KH_SNAPSHOT")) write_snapshot(g,t,getenv("KH_SNAPSHOT"));
    /* Windows runs the TLS callbacks with DLL_PROCESS_ATTACH before the entry point */
    /* Task 12: mod static edits + buffer.  PC order: the mod DLL patches the image from its DllMain, loaded by Panacea's DllMain (a static
     * import of the exe), i.e. before the exe's TLS callbacks, CRT initialisers and entry point. */
    { extern void kh_mod_static_init(KCtx *); kh_mod_static_init(g); }
    /* Task 19: NeverlandFly.dll, the DLL Panacea loads after the main mod DLL (file-name order): signatures, 54 bytes, the row-step thread */
    { extern void kh_mod_neverland_init(KCtx *); kh_mod_neverland_init(g); }
    kh_run_tls_callbacks(g,1);
    kh_mod_syscalls_init(g);
    { extern void kh_mod_difficulty_init(KCtx *); kh_mod_difficulty_init(g); }
    { extern void kh_mod_hooks_b_init(KCtx *); kh_mod_hooks_b_init(g); }     /* Task 23: the DLL's command word lists (its .bss), before any game code */
    { extern void kh_moose_init(KCtx *); kh_moose_init(g); }            /* the mod's pool sizes: before the game's own initialisation, like the DLL */
    if(selftest){ extern int kh_selftest(KCtx*); int f=kh_selftest(g); kh_exit_process(f?1:0); }
    u64 entry;
    if(!strcmp(entry_mode,"crt")) entry=GS_BASE+0x4393d0;            /* WinMainCRTStartup: security cookie, CRT init, __xi/__xc tables, WinMain */
    else if(!strcmp(entry_mode,"winmain")) entry=GS_BASE+0x143720;
    else if(!strcmp(entry_mode,"axa")) entry=GS_BASE+0x14f1e0;
    else { entry=strtoull(entry_mode,0,0); }
    u64 rc;
    if(!strcmp(entry_mode,"winmain")) rc=kh_call(g,entry,GS_BASE,0,0,1);
    else rc=kh_call(g,entry,0,0,0,0);
    fprintf(stderr,"kh: guest entry 0x%llx returned 0x%llx\n",(unsigned long long)entry,(unsigned long long)rc);
    exit_code_main=(int)rc; kh_exit_process((int)rc); return NULL;
}

static int kh_main_body(int argc,char **argv){
    setvbuf(stderr,NULL,_IOLBF,0);
    kh_host_env_init(&argc,&argv);                  /* host_env.c: nothing unless KH_IOS_PATHS (app container directories, Documents/khnmc.conf) */
    const char *e;
    if((e=getenv("KH_EXE"))) snprintf(exe_path,sizeof exe_path,"%s",e);
    if((e=getenv("KH_ROOT"))) snprintf(kh_root,sizeof kh_root,"%s",e);
    if((e=getenv("KH_DOCS"))) snprintf(kh_docs,sizeof kh_docs,"%s",e);
    if((e=getenv("KH_ENTRY"))) entry_mode=e;
    if((e=getenv("KH_VERBOSE"))) kh_verbose=*e=='1';
    if((e=getenv("KH_DET"))) ga_deterministic=*e=='1';
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--exe")&&i+1<argc) snprintf(exe_path,sizeof exe_path,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--root")&&i+1<argc) snprintf(kh_root,sizeof kh_root,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--docs")&&i+1<argc) snprintf(kh_docs,sizeof kh_docs,"%s",argv[++i]);
        else if(!strcmp(argv[i],"--entry")&&i+1<argc) entry_mode=argv[++i];
        else if(!strcmp(argv[i],"-v")) kh_verbose=1;
        else if(!strcmp(argv[i],"--selftest")) selftest=1;
        else if(!strcmp(argv[i],"--hd-check")){ hd_check=1; if(i+1<argc&&strncmp(argv[i+1],"--",2)) hd_check_report=argv[++i]; }
        else { fprintf(stderr,"usage: %s [--exe kh2fm.exe] [--root GAMEDIR] [--docs DIR] [--entry crt|winmain|axa|ADDR] [-v] [--selftest] [--hd-check [REPORT.tsv]]\n env: KH_TRACE=file KH_TRACE_MAX=n KH_STOP_AT_GX=1 KH_DET=1\n"
                              " --hd-check: build the HD asset table of every game file of the tree KHNMC_DATA (KHNMC_HD_PACKAGES, KHNMC_HD_MODLIST as in a run) and print the totals; no game image is loaded, nothing runs\n",argv[0]); return 2; }
    }
    if(hd_check){ extern int kh_hd_check(const char *); return kh_hd_check(hd_check_report); }      /* host-only: before the docs dir, the trace and the guest image */
    mkdir(kh_docs,0755);
    kh_trace_init(); kh_install_signals();
    kh_vclock_init();
    { extern void kh_bgload_init(void); kh_bgload_init(); }
    { extern int ga_no_stress; ga_no_stress=selftest; }
    if(load_image()) return 1;
    { extern void kh_idtable_init(void); kh_idtable_init(); }
    kh_seam_steam_init(); kh_seam_gfx_init(); kh_seam_audio_init(); kh_seam_input_init(); kh_seam_window_init(); kh_seam_mf_init();
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,128ull<<20); pthread_t th;
    if(pthread_create(&th,&at,guest_main,0)){ perror("pthread_create"); return 1; }
    pthread_join(th,0); return exit_code_main;
}
/* The game ends in kh_exit_process -> kh_host_exit; the early returns above (usage 2, image / thread errors 1) come back here and
 * pass the same hook, so a status file (host_env.c) never keeps saying "running" for a process that has ended.
 * An app shell that owns the process's main() (runtime/ios/ios_main.m) builds this file with -DKH_MAIN_NAME=kh_main and calls it. */
#ifdef KH_MAIN_NAME
int KH_MAIN_NAME(int argc,char **argv){
#else
int main(int argc,char **argv){
#endif
    int rc=kh_main_body(argc,argv);
    void (*h)(int)=kh_exit_hook; if(h){ fflush(stdout); fflush(stderr); h(rc); }
    return rc;
}
