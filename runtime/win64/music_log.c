/* music_log.c (task 32, item 6c) : a log line whenever the background music changes.  Readers and existing hooks only: the state
 * is read once per game frame from kh_route_state_tick (the 0x105e10 hook that already runs the route's readers), the music file
 * requests come from the file layer's own log call.  Nothing is hooked for this and nothing is written to the game.
 *
 *   KHNMC_MUSICLOG=<file>   (or `stderr`)   off when unset.
 *
 * What is read (the game's music manager, exe 0x155840 / 0x155980 / 0x155c30; two slots of 16 bytes at 0x140abad40):
 *   +0  the track the slot is to play (set from the room's pair by 0x155840, or by an event's `play music` command 0x2cf3b0)
 *   +4  the track whose file the slot holds          +8  the slot's volume word (0x3000 = full, 0x1800 = half, 0 = silent)
 *   0x140abad18  1 = the room has two tracks (field / battle) that cross-fade, 0 = one track
 *   0x140abad19  the cross-fade's side (1 = battle)
 * Lines (one per change; `map` is world:area as in the map report):
 *   kh: music game-frame=N map WW:AA slot0 track=T file=F vol=0xV slot1 track=T file=F vol=0xV mode=single|field+battle side=field|battle
 *   kh: music load game-frame=N map WW:AA HIT|MISS <relative name> <bytes>          every request for a file under bgm/ or vagstream/
 * tools/music_compare.py compares two such logs. */
#include "khnmc_rt.h"
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define YS_BGM    0xabad40ull
#define YS_DUAL   0xabad18ull
#define YS_SIDE   0xabad19ull
#define NOW_PLACE 0x717008ull
u64 kh_input_game_frames(void);

typedef struct { u32 want[2],file[2],vol[2]; u8 dual,side,world,area; } MState;
static int on=-1; static FILE *out; static MState prev; static int have;
static pthread_mutex_t mm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 n_state, n_load;

static void init(void){
    const char *e=getenv("KHNMC_MUSICLOG");
    if(!e||!*e){ on=0; return; }
    if(!strcmp(e,"stderr")){ out=stderr; on=1; return; }
    out=fopen(e,"w"); if(!out){ kh_warn("music log disabled: %s (%s)",e,strerror(errno)); on=0; return; }
    on=1;
}
static void read_state(KCtx *g,MState *m){
    for(int i=0;i<2;i++){ m->want[i]=rd32(g,GS_BASE+YS_BGM+16*i); m->file[i]=rd32(g,GS_BASE+YS_BGM+16*i+4); m->vol[i]=rd32(g,GS_BASE+YS_BGM+16*i+8); }
    m->dual=rd8(g,GS_BASE+YS_DUAL); m->side=rd8(g,GS_BASE+YS_SIDE); m->world=rd8(g,GS_BASE+NOW_PLACE); m->area=rd8(g,GS_BASE+NOW_PLACE+1);
}
static int same_music(const MState *a,const MState *b){
    return !memcmp(a->want,b->want,sizeof a->want)&&!memcmp(a->file,b->file,sizeof a->file)&&!memcmp(a->vol,b->vol,sizeof a->vol)&&a->dual==b->dual&&a->side==b->side;
}
static void line(FILE *f,const MState *m,u64 n){
    fprintf(f,"kh: music game-frame=%llu map %02x:%02x slot0 track=%u file=%u vol=0x%x slot1 track=%u file=%u vol=0x%x mode=%s side=%s\n",(unsigned long long)n,m->world,m->area,
            m->want[0],m->file[0],m->vol[0],m->want[1],m->file[1],m->vol[1],m->dual?"field+battle":"single",m->side==1?"battle":"field");
}
/* once per game frame, on the game thread */
void kh_music_tick(KCtx *g,u64 n){
    if(on<0){ pthread_mutex_lock(&mm); if(on<0) init(); pthread_mutex_unlock(&mm); }
    if(!on) return;
    MState m; read_state(g,&m);
    if(have&&same_music(&m,&prev)) return;                 /* a new map alone is not a change of the music */
    pthread_mutex_lock(&mm); line(out,&m,n); fflush(out); pthread_mutex_unlock(&mm);
    prev=m; have=1; n_state++;
}
/* from the file layer (any thread): a request for a music file */
void kh_music_note_file(KCtx *g,const char *path,int hit,long long size){
    if(on<0){ pthread_mutex_lock(&mm); if(on<0) init(); pthread_mutex_unlock(&mm); }
    if(!on||!path) return;
    const char *p=path,*s; const char *found=NULL;
    for(s=p;*s;s++){                                       /* the relative name starts at the last "bgm/" or "vagstream/" path component */
        if(s==p||s[-1]=='/'||s[-1]=='\\'){
            if(!strncasecmp(s,"bgm/",4)||!strncasecmp(s,"bgm\\",4)||!strncasecmp(s,"vagstream/",10)||!strncasecmp(s,"vagstream\\",10)) found=s;
        }
    }
    if(!found) return;
    char rel[512]; size_t k=0; for(s=found;*s&&k+1<sizeof rel;s++) rel[k++]=*s=='\\'?'/':(char)tolower((unsigned char)*s); rel[k]=0;
    u8 w=g?rd8(g,GS_BASE+NOW_PLACE):0xff, a=g?rd8(g,GS_BASE+NOW_PLACE+1):0xff;
    pthread_mutex_lock(&mm);
    fprintf(out,"kh: music load game-frame=%llu map %02x:%02x %s %s %lld\n",(unsigned long long)kh_input_game_frames(),w,a,hit?"HIT":"MISS",rel,size);
    fflush(out); pthread_mutex_unlock(&mm); n_load++;
}
void kh_music_report(FILE *f){
    if(on==1) fprintf(f,"kh: music log: %llu state lines, %llu music file requests\n",(unsigned long long)n_state,(unsigned long long)n_load);
}

/* ---- selftest: the reader on synthetic values at the real addresses (no game runs in the selftest; the bytes are put back) ---- */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"music log selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static int count_lines(FILE *f,char *last,size_t cap){ rewind(f); char l[600]; int n=0; last[0]=0; while(fgets(l,sizeof l,f)){ n++; snprintf(last,cap,"%s",l); } fseek(f,0,SEEK_END); return n; }
int kh_music_log_selftest(KCtx *g){
    checks=bad=0;
    u8 save_bgm[32],save_flags[2],save_place[2]; memcpy(save_bgm,hp(g,GS_BASE+YS_BGM),32); memcpy(save_flags,hp(g,GS_BASE+YS_DUAL),2); memcpy(save_place,hp(g,GS_BASE+NOW_PLACE),2);
    int save_on=on; FILE *save_out=out; MState save_prev=prev; int save_have=have; u64 s_state=n_state,s_load=n_load;
    FILE *f=kh_tmpfile(); if(!f) kh_fatal(g,"music log selftest: tmpfile failed");
    char last[600];
    on=0; kh_music_tick(g,1); kh_music_note_file(g,"bgm/music118.win32.scd",1,10); T(count_lines(f,last,sizeof last)==0);      /* off: silent */
    on=1; out=f; have=0;
    memset(hp(g,GS_BASE+YS_BGM),0,32); wr8(g,GS_BASE+YS_DUAL,0); wr8(g,GS_BASE+YS_SIDE,0); wr8(g,GS_BASE+NOW_PLACE,0x12); wr8(g,GS_BASE+NOW_PLACE+1,0x03);
    kh_music_tick(g,100); T(count_lines(f,last,sizeof last)==1);                                                              /* the first state is a line */
    T(!strcmp(last,"kh: music game-frame=100 map 12:03 slot0 track=0 file=0 vol=0x0 slot1 track=0 file=0 vol=0x0 mode=single side=field\n"));
    kh_music_tick(g,101); kh_music_tick(g,102); T(count_lines(f,last,sizeof last)==1);                                         /* no change, no line */
    wr32(g,GS_BASE+YS_BGM,242); kh_music_tick(g,103); T(count_lines(f,last,sizeof last)==2); T(strstr(last,"game-frame=103")&&strstr(last,"slot0 track=242 file=0 vol=0x0"));
    wr32(g,GS_BASE+YS_BGM+4,242); wr32(g,GS_BASE+YS_BGM+8,0x3000); kh_music_tick(g,104); T(count_lines(f,last,sizeof last)==3); T(strstr(last,"slot0 track=242 file=242 vol=0x3000 slot1 track=0")!=NULL);
    wr32(g,GS_BASE+YS_BGM+16,119); wr32(g,GS_BASE+YS_BGM+20,119); wr32(g,GS_BASE+YS_BGM+24,0x1800); wr8(g,GS_BASE+YS_DUAL,1);
    kh_music_tick(g,105); T(count_lines(f,last,sizeof last)==4); T(strstr(last,"slot1 track=119 file=119 vol=0x1800 mode=field+battle side=field")!=NULL);
    wr8(g,GS_BASE+YS_SIDE,1); kh_music_tick(g,106); T(count_lines(f,last,sizeof last)==5); T(strstr(last,"side=battle")!=NULL);
    wr8(g,GS_BASE+NOW_PLACE,0x02); wr8(g,GS_BASE+NOW_PLACE+1,0x07); kh_music_tick(g,107); T(count_lines(f,last,sizeof last)==5); /* a map change alone is not a music change */
    wr32(g,GS_BASE+YS_BGM+8,0); kh_music_tick(g,108); T(count_lines(f,last,sizeof last)==6); T(strstr(last,"game-frame=108 map 02:07 slot0 track=242 file=242 vol=0x0")!=NULL);
    wr32(g,GS_BASE+YS_BGM+16+4,0); kh_music_tick(g,109); T(count_lines(f,last,sizeof last)==7); T(strstr(last,"slot1 track=119 file=0 ")!=NULL);   /* each of the six words counts */
    wr32(g,GS_BASE+YS_BGM+16,0); kh_music_tick(g,110); T(count_lines(f,last,sizeof last)==8);
    wr32(g,GS_BASE+YS_BGM+16+8,7); kh_music_tick(g,111); T(count_lines(f,last,sizeof last)==9); T(strstr(last,"slot1 track=0 file=0 vol=0x7")!=NULL);
    /* file requests: only music folders, any spelling, with the result and the size */
    kh_music_note_file(g,"C:/hd28/STEAM/BGM\\Music118.win32.scd",1,2152544); T(count_lines(f,last,sizeof last)==10); T(strstr(last,"kh: music load game-frame=")&&strstr(last," map 02:07 HIT bgm/music118.win32.scd 2152544\n"));
    kh_music_note_file(g,"vagstream/Title.win32.scd",0,-1); T(count_lines(f,last,sizeof last)==11); T(strstr(last," MISS vagstream/title.win32.scd -1\n")!=NULL);
    kh_music_note_file(g,"obj/bgm_thing.mdlx",1,5); kh_music_note_file(g,"se/se000.win32.scd",1,5); kh_music_note_file(g,"xbgm/music.scd",1,5); T(count_lines(f,last,sizeof last)==11);
    T(n_state==s_state+9&&n_load==s_load+2);
    memcpy(hp(g,GS_BASE+YS_BGM),save_bgm,32); memcpy(hp(g,GS_BASE+YS_DUAL),save_flags,2); memcpy(hp(g,GS_BASE+NOW_PLACE),save_place,2);
    on=save_on; out=save_out; prev=save_prev; have=save_have; n_state=s_state; n_load=s_load; fclose(f);
    fprintf(stderr,"kh: music log selftest: %d checks, %d failures\n",checks,bad); return bad;
}
