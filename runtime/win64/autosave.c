/* Native port of equations19's auto-save, based on Denhonator's KHPCSpeedrunTools. */
#include "autosave.h"
#include "route_state.h"
#include "warp.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONTINUE (GS_BASE+0x29fb580)
#define SAVE (GS_BASE+0x9a98b0)
enum { SAVE_SIZE=0x10fc0 };
static int slots;
static char directory[PATH_MAX];

void kh_autosave_init(void){
    slots=0;
    const char *e=getenv("KHNMC_AUTOSAVE");
    if(!e || !*e || !strcmp(e,"0")) return;
    if(strcmp(e,"1")) kh_fatal(NULL,"autosave-mode: expected 0 or 1");
    e=getenv("KHNMC_AUTOSAVE_SLOTS");
    char *end;
    long count=e ? strtol(e,&end,10) : 3;
    if((e && (*end || end==e)) || count<1 || count>INT_MAX)
        kh_fatal(NULL,"autosave-slots: expected a positive slot count");
    slots=(int)count;
    if(snprintf(directory,sizeof directory,"%s/autosave",kh_autosave_documents())>=(int)sizeof directory)
        kh_fatal(NULL,"autosave-directory: path too long");
}

static int allowed(unsigned w,unsigned r){
    return w>0 && w<KH_WARP_WORLDS && r<64 &&
        !(w==1 && r==1) && !(w==5 && r==5) && !(w==18 && r>=20 && r<=29) &&
        !(w==8 && r==13) && !(w==16 && r==26) && !(w==14 && r==11) &&
        w!=9 && !(w==4 && r>=42 && r<=45);
}

/* f_325e20's table exists only while the game's save manager is open. */
static u32 crc_table[256];
static pthread_once_t crc_once=PTHREAD_ONCE_INIT;
static void init_crc(void){
    for(unsigned i=0;i<256;i++){
        u32 c=i<<24;
        for(unsigned j=0;j<255;j++) c=(c<<1)^((c>>31) ? 0x04c11db7u : 0);
        crc_table[i]=c;
    }
}
static u32 checkpoint_crc(const u8 *p){
    pthread_once(&crc_once,init_crc);
    u32 c=~0u;
    for(size_t i=0;i<SAVE_SIZE;i++){
        if(i>=8 && i<12) continue;
        c=(c<<8)^crc_table[(c>>24)^p[i]];
    }
    return ~c;
}

/* Native validation updates the save manager's running CRC. Preserve it across the extra call. */
static u32 native_valid(KCtx *g,u64 buffer){
    KCtx ctx=*g;
    u32 prior=rd32(g,GS_BASE+0x745170);
    u32 result=kh_call(&ctx,GS_BASE+0x2ecc00,buffer,0,0,0);
    wr32(g,GS_BASE+0x745170,prior);
    return (u8)result;
}

static int header_ok(const u8 *p){
    return !memcmp(p,"KH2J\x3a\0\0\0",8) && allowed(p[12],p[13]);
}

static int open_directory(int writing){
    if(writing && mkdir(directory,0700) && errno!=EEXIST) return -1;
    int fd=open(directory,O_RDONLY|O_DIRECTORY);
    if(fd>=0 && flock(fd,writing ? LOCK_EX : LOCK_SH)){ close(fd); return -1; }
    return fd;
}

static void filename(char name[40],int n){ snprintf(name,40,"KH2autosave-%d.dat",n); }

/* The relative symlink is also a human-readable newest marker. */
static int newest(int dir){
    char name[40],canonical[40];
    ssize_t len=readlinkat(dir,"newest",name,sizeof name-1);
    if(len<0) return -1;
    name[len]=0;
    char *end;
    long n=!strncmp(name,"KH2autosave-",12) ? strtol(name+12,&end,10) : 0;
    if(n>0 && n<=INT_MAX){ filename(canonical,(int)n); if(!strcmp(canonical,name)) return (int)n; }
    errno=EINVAL;
    return -1;
}

static int publish(int dir,int n,const u8 *data){
    char name[40]; filename(name,n);
    int fd=openat(dir,".slot.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return 0;
    FILE *f=fdopen(fd,"wb");
    if(!f){ close(fd); return 0; }
    int ok=fwrite(data,1,SAVE_SIZE,f)==SAVE_SIZE && !fflush(f) && !fsync(fd);
    if(fclose(f)) ok=0;
    if(ok) ok=renameat(dir,".slot.tmp",dir,name)==0;
    if(ok){
        unlinkat(dir,".newest.tmp",0);
        ok=!symlinkat(name,dir,".newest.tmp") && !renameat(dir,".newest.tmp",dir,"newest");
    }
    if(!ok){ unlinkat(dir,".slot.tmp",0); unlinkat(dir,".newest.tmp",0); }
    return ok;
}

void orig_f_3a0660(KCtx *);
void kh_retry_before_3a0660(KCtx *g);                 /* tweak_options.c (task 60): retry's checkpoint, before the refresh */
void f_3a0660(KCtx *g){
    kh_retry_before_3a0660(g);
    if(!slots){ orig_f_3a0660(g); return; }
    u32 before=rd32(g,CONTINUE+12);
    orig_f_3a0660(g);
    if(rd32(g,CONTINUE+12)==before) return;
    const u8 *p=GUEST_PTR(CONTINUE);
    if(!header_ok(p) || rd8(g,GS_BASE+0x2a11428) || rd64(g,GS_BASE+0x2a11478) ||
       rd64(g,GS_BASE+0x2a0ff68) || kh_route_mission(g)){
        kh_warn("autosave-skip: checkpoint world %u room %u",p[12],p[13]); return;
    }
    wr32(g,CONTINUE+8,checkpoint_crc(p));
    int dir=open_directory(1);
    if(dir<0){ kh_warn("autosave-write: %s: %s",directory,strerror(errno)); return; }
    int last=newest(dir);
    int n=last<0 ? 1 : last%slots+1;
    if((last<0 && errno!=ENOENT) || !publish(dir,n,p)) kh_warn("autosave-write: %s: %s",directory,strerror(errno));
    else kh_warn("autosave-write: KH2autosave-%d.dat %.2s%02u",n,kh_warp_world_names[p[12]],p[13]);
    close(dir);
}

void kh_autosave_load(KCtx *g){
    if(!slots) return;
    u16 buttons=kh_input_buttons();
    if(!(buttons&0x20)) return;
    int dir=open_directory(0);
    int n=dir<0 ? -1 : newest(dir);
    if(n>0 && (buttons&0x100)) n=slots==1 ? -1 : (n==1 ? slots : n-1);
    char name[40]="newest";
    int fd=-1;
    if(n>0){ filename(name,n); fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK); }
    FILE *f=fd<0 ? NULL : fdopen(fd,"rb");
    int ok=0;
    if(f){
        struct stat st;
        if(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==SAVE_SIZE){
            u64 candidate=ga_alloc(SAVE_SIZE,16);
            const u8 *data=GUEST_PTR(candidate);
            ok=fread(GUEST_PTR(candidate),1,SAVE_SIZE,f)==SAVE_SIZE && header_ok(data) &&
               rd64(g,GS_BASE+0xbef898) && native_valid(g,candidate);
            if(ok){
                memcpy(GUEST_PTR(SAVE),data,SAVE_SIZE);
                kh_warn("autosave-load: %s %.2s%02u",name,kh_warp_world_names[data[12]],data[13]);
            }
            ga_free(candidate);
        }
        fclose(f);
    } else if(fd>=0) close(fd);
    if(!ok) kh_warn("autosave-fallback: %s missing or invalid, using selected save",name);
    if(dir>=0) close(dir);
}

int kh_autosave_selftest(KCtx *g){
    KCtx ctx=*g;
    u64 old_table=rd64(g,GS_BASE+0xbef898), old_manager=rd64(g,GS_BASE+0xbef890);
    u32 old_crc=rd32(g,GS_BASE+0x745170);
    kh_call(&ctx,GS_BASE+0x325e20,0,0,0,0);
    u64 buf=ga_alloc(SAVE_SIZE,16);
    u8 *p=GUEST_PTR(buf);
    for(size_t i=0;i<SAVE_SIZE;i++) p[i]=(u8)(i*37u);
    memcpy(p,"KH2J\x3a\0\0\0",8); p[12]=2; p[13]=2;
    wr32(g,buf+8,checkpoint_crc(p));
    int fails=!header_ok(p) || !native_valid(g,buf);
    p[SAVE_SIZE-1]^=1;
    fails+=native_valid(g,buf)!=0;
    fails+=allowed(1,1)||allowed(5,5)||allowed(18,21)||allowed(8,13)||allowed(16,26)||
           allowed(14,11)||allowed(9,0)||allowed(4,45)||!allowed(18,19);
    ga_free(buf);
    kh_call(&ctx,GS_BASE+0x325dd0,0,0,0,0);
    wr64(g,GS_BASE+0xbef890,old_manager); wr64(g,GS_BASE+0xbef898,old_table);
    wr32(g,GS_BASE+0x745170,old_crc);
    fprintf(stderr,"kh: autosave selftest: native validation, payload corruption and room exclusions: %d failures\n",fails);
    return fails;
}
