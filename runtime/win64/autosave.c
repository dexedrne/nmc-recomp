#include "settings.h"
/* Native port of equations19's auto-save, based on Denhonator's KHPCSpeedrunTools. */
#include "autosave.h"
#include "xion_swap.h"
#include "slot_disk.h"
#include "slot_hash.h"
#include "kh_look.h"
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
static _Atomic int slots,indicator;
static _Atomic u64 indicator_until;
static int indicator_visible(u64 now,u64 until){ return now<until; }
int kh_autosave_indicator(void){
    u64 until=atomic_load(&indicator_until);
    return until && indicator_visible(kh_host_ns(),until);
}


void kh_autosave_init(void){
    slots=0; indicator=0; atomic_store(&indicator_until,0);
    const char *e=kh_menu_choice("KHNMC_AUTOSAVE");
    if(!e || !*e || !strcmp(e,"0")) return;
    if(strcmp(e,"1")){ kh_warn("autosave disabled: KHNMC_AUTOSAVE expects 0 or 1"); return; }
    e=getenv("KHNMC_AUTOSAVE_INDICATOR");
    if(e && strcmp(e,"0") && strcmp(e,"1")) kh_warn("autosave indicator disabled: expected 0 or 1");
    indicator=!e || !strcmp(e,"1");
    e=getenv("KHNMC_AUTOSAVE_SLOTS");
    char *end;
    long count=e ? strtol(e,&end,10) : 3;
    if((e && (*end || end==e)) || count<1 || count>INT_MAX)
        { kh_warn("autosave disabled: expected a positive slot count"); return; }
    slots=(int)count;

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
u32 kh_save_crc(const u8 *p){
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
    char directory[PATH_MAX];
    if(snprintf(directory,sizeof directory,"%s/autosave",kh_slot_autosave_documents())>=(int)sizeof directory)
        kh_fatal(NULL,"autosave-directory: path too long");
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

/* The optional trailer is committed by the same rename as the checkpoint. */
static const u8 mode_trailer[]={'k','h','X','b',1};

/* Axel Won't Cry.  Its checkpoint file is an ordinary one, without the trailer: a build from before this mode accepts a
 * checkpoint of exactly SAVE_SIZE bytes (plain Critical) or one with the 5-byte khXb trailer (which it would load as Xion
 * Knows Best) and calls every other size invalid.  The mode is the side file KH2autosave-<n>.axel, 38 bytes:
 *     +0  "khXa", u8 1 (version)    +5  u8 saved difficulty (2)    +6  SHA-256 of the checkpoint's SAVE_SIZE bytes
 * It is written (rename) before the checkpoint and counts only while its digest matches the checkpoint beside it, so a
 * file left from an earlier checkpoint of that number, or by a failed write, is nothing.  The older build never opens it. */
typedef struct { u8 magic[5],level,digest[32]; } AxelMark;
static const u8 axel_magic[]={'k','h','X','a',1};
static void axel_name(char name[40],int n){ snprintf(name,40,"KH2autosave-%d.axel",n); }
static void axel_mark_make(AxelMark *m,const u8 *data){
    memcpy(m->magic,axel_magic,sizeof m->magic); m->level=data[0x2498];
    SlotHash h; slot_hash_init(&h); slot_hash_add(&h,data,SAVE_SIZE); slot_hash_end(m->digest,&h);
}
static int axel_mark_valid(const AxelMark *m,const u8 *data){
    AxelMark expected; axel_mark_make(&expected,data);
    return m->level==2 && !data[0x2499] && !data[0x249a] && !data[0x249b] && !memcmp(m,&expected,sizeof expected);
}
static int axel_mark_publish(int dir,int n,const u8 *data,int axel){
    char name[40]; axel_name(name,n);
    if(!axel) return !unlinkat(dir,name,0) || errno==ENOENT;
    AxelMark m; axel_mark_make(&m,data);
    int fd=openat(dir,".axel.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return 0;
    int ok=write(fd,&m,sizeof m)==(ssize_t)sizeof m && !fsync(fd);
    if(close(fd)) ok=0;
    if(ok) ok=!renameat(dir,".axel.tmp",dir,name);
    if(!ok) unlinkat(dir,".axel.tmp",0);
    return ok;
}
static int axel_mark_read(int dir,int n,const u8 *data){
    char name[40]; axel_name(name,n);
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return 0;
    AxelMark m; u8 extra;
    struct stat st;
    int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && read(fd,&m,sizeof m)==(ssize_t)sizeof m && read(fd,&extra,1)==0 &&
           axel_mark_valid(&m,data);
    close(fd);
    return ok;
}
static int publish(int dir,int n,const u8 *data){
    char name[40]; filename(name,n);
    if(!axel_mark_publish(dir,n,data,kh_axel_enabled)) return 0;
    int fd=openat(dir,".slot.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return 0;
    FILE *f=fdopen(fd,"wb");
    if(!f){ close(fd); return 0; }
    u8 trailer[sizeof mode_trailer]; memcpy(trailer,mode_trailer,sizeof trailer);
    trailer[4]=data[0x2498];
    int ok=fwrite(data,1,SAVE_SIZE,f)==SAVE_SIZE &&
        (!kh_xion_enabled || fwrite(trailer,sizeof trailer,1,f)==1) && !fflush(f) && !fsync(fd);
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
    int count=slots;
    if(!count||kh_theater_active()){ orig_f_3a0660(g); return; }
    u32 before=rd32(g,CONTINUE+12);
    orig_f_3a0660(g);
    if(rd32(g,CONTINUE+12)==before) return;
    const u8 *p=GUEST_PTR(CONTINUE);
    if(!header_ok(p) || rd8(g,GS_BASE+0x2a11428) || rd64(g,GS_BASE+0x2a11478) ||
       rd64(g,GS_BASE+0x2a0ff68) || kh_route_mission(g)){
        kh_warn("autosave-skip: checkpoint world %u room %u",p[12],p[13]); return;
    }
    wr32(g,CONTINUE+8,kh_save_crc(p));
    int dir=open_directory(1);
    if(dir<0){ kh_warn("autosave-write: %s: %s",kh_slot_autosave_documents(),strerror(errno)); return; }
    int last=newest(dir);
    int n=last<0 ? 1 : last%count+1;
    if((last<0 && errno!=ENOENT) || !publish(dir,n,p)) kh_warn("autosave-write: %s: %s",kh_slot_autosave_documents(),strerror(errno));
    else {
        if(indicator) atomic_store(&indicator_until,kh_host_ns()+1500000000ull);
        kh_warn("autosave-write: KH2autosave-%d.dat %.2s%02u",n,kh_warp_world_names[p[12]],p[13]);
    }
    close(dir);
}

static int read_checkpoint(KCtx *g,int previous,int load,u32 *level,int *tag){
    int dir=open_directory(0);
    int n=dir<0 ? -1 : newest(dir);
    if(n>0 && previous) n=slots==1 ? -1 : (n==1 ? slots : n-1);
    char name[40]="newest";
    int fd=-1;
    if(n>0){ filename(name,n); fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK); }
    FILE *f=fd<0 ? NULL : fdopen(fd,"rb");
    int ok=0;
    if(f){
        struct stat st;
        if(!fstat(fd,&st) && S_ISREG(st.st_mode) && (st.st_size==SAVE_SIZE || st.st_size==SAVE_SIZE+sizeof mode_trailer)){
            u64 candidate=ga_alloc(SAVE_SIZE,16);
            const u8 *data=GUEST_PTR(candidate);
            ok=fread(GUEST_PTR(candidate),1,SAVE_SIZE,f)==SAVE_SIZE && header_ok(data) &&
               rd64(g,GS_BASE+0xbef898) && native_valid(g,candidate);
            int tagged=st.st_size!=SAVE_SIZE;
            u8 trailer[sizeof mode_trailer];
            if(ok && tagged) ok=fread(trailer,sizeof trailer,1,f)==1 && !memcmp(trailer,mode_trailer,4) &&
                trailer[4]>=1 && trailer[4]<=3 && rd32(g,candidate+0x2498)==trailer[4];
            int axel=ok && !tagged && axel_mark_read(dir,n,data);
            if(ok){
                *level=rd32(g,candidate+0x2498);
                *tag=tagged ? trailer[4] : axel ? (KH_SAVE_TAG_AXEL|2) : 0;
            }
            if(ok && load){
                kh_xkb_set_mode(g,!kh_xkb_menu?KH_MODE_NONE:axel?KH_MODE_AXEL:tagged);
                memcpy(GUEST_PTR(SAVE),data,SAVE_SIZE);
                kh_warn("autosave-load: %s %.2s%02u",name,kh_warp_world_names[data[12]],data[13]);
            }
            ga_free(candidate);
        }
        fclose(f);
    } else if(fd>=0) close(fd);
    if(!ok && load) kh_warn("autosave-fallback: %s missing or invalid, using selected save",name);
    if(dir>=0) close(dir);
    return ok;
}

int kh_autosave_shortcut(void){
    if(!slots) return 0;
    u16 buttons=kh_input_buttons();
    return buttons&0x20 ? 1+!!(buttons&0x100) : 0;
}

int kh_autosave_preview(KCtx *g,int shortcut,u32 *level,int *tag){
    return read_checkpoint(g,shortcut==2,0,level,tag);
}

void kh_autosave_load(KCtx *g){
    int shortcut=kh_autosave_shortcut();
    u32 level; int tag;
    if(shortcut) read_checkpoint(g,shortcut==2,1,&level,&tag);
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
    wr32(g,buf+8,kh_save_crc(p));
    int fails=kh_retry_rewind_selftest();
    fails+=!header_ok(p) || !native_valid(g,buf);
    fails+=indicator_visible(0,0) || !indicator_visible(1499999999,1500000000) || indicator_visible(1500000000,1500000000);
    p[SAVE_SIZE-1]^=1;
    fails+=native_valid(g,buf)!=0;
    fails+=allowed(1,1)||allowed(5,5)||allowed(18,21)||allowed(8,13)||allowed(16,26)||
           allowed(14,11)||allowed(9,0)||allowed(4,45)||!allowed(18,19);
    {   /* The Axel Won't Cry side file: bound to its checkpoint's bytes, absent for every other campaign. */
        int axel=0;
        char dir[KH_TMP_MAX]; kh_tmp_path(dir,sizeof dir,"khnmc-axel-auto-XXXXXX");
        int d=mkdtemp(dir)?open(dir,O_RDONLY|O_DIRECTORY):-1;
        if(d<0) axel++;
        else{
            memset(p+0x2498,0,4); p[0x2498]=2;
            AxelMark m; axel_mark_make(&m,p);
            axel+=!axel_mark_valid(&m,p) || sizeof m!=38;
            m.magic[3]='b'; axel+=axel_mark_valid(&m,p); m.magic[3]='a';
            m.magic[4]=2; axel+=axel_mark_valid(&m,p); m.magic[4]=1;
            m.digest[31]^=1; axel+=axel_mark_valid(&m,p); m.digest[31]^=1;
            axel+=!axel_mark_publish(d,2,p,1) || !axel_mark_read(d,2,p) || axel_mark_read(d,1,p);
            p[100]^=1; axel+=axel_mark_read(d,2,p); p[100]^=1;       /* the mark of an earlier checkpoint of this number */
            p[0x2498]=3; axel_mark_make(&m,p); axel+=axel_mark_valid(&m,p);   /* Less Than Nothing is never this mode */
            axel+=!axel_mark_publish(d,3,p,1) || axel_mark_read(d,3,p);
            p[0x2498]=2;
            axel+=!axel_mark_publish(d,2,p,0) || axel_mark_read(d,2,p) || !axel_mark_publish(d,2,p,0);   /* removed, twice */
            unlinkat(d,"KH2autosave-3.axel",0); close(d); rmdir(dir);
        }
        fprintf(stderr,"kh: autosave axel mark: digest binding, stale mark, wrong level, removal: %d failures\n",axel);
        fails+=axel;
    }
    ga_free(buf);
    kh_call(&ctx,GS_BASE+0x325dd0,0,0,0,0);
    wr64(g,GS_BASE+0xbef890,old_manager); wr64(g,GS_BASE+0xbef898,old_table);
    wr32(g,GS_BASE+0x745170,old_crc);
    fprintf(stderr,"kh: autosave selftest: native validation, payload corruption and room exclusions: %d failures\n",fails);
    return fails;
}
