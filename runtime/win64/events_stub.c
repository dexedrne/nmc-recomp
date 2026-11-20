/* events_stub.c (task 32) : KHNMC_EVENTS=stub : hand the game the mod's empty scene for EVERY event entry of an area file.
 *
 * The Nobody May Cry mod removes a cutscene by replacing its entry (BAR type 22) in the area file with one 56-byte entry, the same
 * for all 670: project header, setup, start, end frame 50, one fade at frame 1.  440 entries are still real scenes.  This option
 * does at load time, where the area file's entries are read, what the mod did in the files: exe 0x3a1f70 (the one place that finds
 * an event entry by name in the loaded area file, called only by the event starter 0x3ac350) returns the stub's bytes instead of
 * the entry's.  No file is edited, nothing else of the area file changes (the programs, the jump that follows the event, the
 * progress flags: all as before).  An entry that already is the stub is passed through untouched, and so are the mod's two stubs
 * that carry a music command (the stub's five commands plus music commands only).
 *
 *   KHNMC_EVENTS=stub         every event entry is the mod's stub (profile B of tools/prune_lists.py).  ONE exception, for the
 *                             music: a scene that sets the event's "keep my volume" flag (a volume command 0x2e with a non-zero
 *                             flag word) gets the stub PLUS its own music commands (below).  Why (read from the exe, task 32 6b):
 *                             when the next field starts, 0x155840 / 0x155980 put the destination room's pair into the music slots
 *                             whatever a scene played, so a plain stub leaves the same track after it; only the keep flag lets a
 *                             scene's state survive (0x2cf640 writes the scene's last volume per slot at its end, 0x155980 then
 *                             leaves an unchanged track at that volume instead of bringing it back to full).  In the tree that is
 *                             21 of the 440 real scenes, 7 of them reachable (tools/event_refs.py `after`); without this they leave the room's
 *                             track at full volume where the game leaves it at half.
 *   KHNMC_EVENTS=stub+music   the stub plus the entry's OWN music commands for EVERY scene that has music commands.
 *   music commands carried:   0x2f read, 0x30 play, 0x2e volume, in their order, at frames clamped into the stub's 50 (reads first,
 *                             then the frame-0 commands, the stub's fade, the later ones); the project header is then the entry's own
 *                             (it sizes the scene's memory, and the music file is read into it).
 *   unset / `real`            the entries as they are in the files (default)
 *   anything else             named fatal `events-mode`
 * The stub's bytes are not in this source: they are read from the tree (the first 56-byte entry of that shape in KHNMC_DATA's
 * ard/us/, sorted by name).  No such entry: named fatal `events-stub-template`.  Counters at exit: `kh: events stub: ...`. */
#include "kh_look.h"
#include "rmc.h"
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

void orig_f_3a1f70(KCtx *g);
#define STUB_LEN  56
#define BUF_LEN   4096
#define RING      8
#define EV_MAX    (4u<<20)                       /* no event entry is larger (the largest of the tree is 31 KiB) */
enum { M_UNSET=-1, M_REAL=0, M_STUB=1, M_STUB_MUSIC=2 };
static int mode=M_UNSET;
static u8 tmpl[STUB_LEN]; static int have_tmpl; static char tmpl_from[300];
static u64 ring[RING]; static unsigned ri;
static pthread_mutex_t em=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 n_lookup, n_none, n_already, n_already_music, n_replaced, n_music_cmds, n_carried, n_carried_keep;

/* the commands of an event: (u16 byte length incl. this header, u16 type) ..., a zero length ends the list (exe 0x2c8f40) */
static int shape_is_stub(const u8 *d,size_t n){
    static const u16 want[5]={0x00,0x0f,0x10,0x08,0x12}; size_t p=0; int k=0;
    for(;;){
        if(p+2>n) return 0;
        u16 len=(u16)(d[p]|d[p+1]<<8); if(!len) return k==5;
        if(len<4||p+len>n||k>=5) return 0;
        if((u16)(d[p+2]|d[p+3]<<8)!=want[k]) return 0;
        k++; p+=len;
    }
}
static void load_template(KCtx *g){
    const char *root=getenv("KHNMC_DATA"); if(!root) root="data";
    char dir[4096]; if(!kh_resolve_ci(root,"ard/us",dir,sizeof dir)) return;
    struct dirent **list=NULL; int n=scandir(dir,&list,NULL,alphasort);
    if(n<0) return;
    for(int i=0;i<n&&!have_tmpl;i++){
        const char *nm=list[i]->d_name; size_t l=strlen(nm); if(l<5||strcasecmp(nm+l-4,".ard")) continue;
        char p[4500],rel[512]; snprintf(rel,sizeof rel,"ard/us/%s",nm);
        if(!kh_rmc_path(rel,p,sizeof p)) snprintf(p,sizeof p,"%s/%s",dir,nm); FILE *f=fopen(p,"rb"); if(!f) continue;
        struct stat st; if(fstat(fileno(f),&st)||st.st_size<16||st.st_size>(64<<20)){ fclose(f); continue; }
        u8 *b=malloc((size_t)st.st_size); if(!b){ fclose(f); kh_fatal(g,"events-stub-template: out of memory"); }
        if(fread(b,1,(size_t)st.st_size,f)==(size_t)st.st_size&&!memcmp(b,"BAR\1",4)){
            u32 cnt=(u32)(b[4]|b[5]<<8|b[6]<<16|(u32)b[7]<<24);
            for(u32 k=0;k<cnt&&16+16ull*(k+1)<=(u64)st.st_size;k++){
                const u8 *e=b+16+16*k; u16 type=(u16)(e[0]|e[1]<<8); u32 off=(u32)(e[8]|e[9]<<8|e[10]<<16|(u32)e[11]<<24), size=(u32)(e[12]|e[13]<<8|e[14]<<16|(u32)e[15]<<24);
                if(type==22&&size==STUB_LEN&&(u64)off+size<=(u64)st.st_size&&shape_is_stub(b+off,size)){ memcpy(tmpl,b+off,STUB_LEN); have_tmpl=1; snprintf(tmpl_from,sizeof tmpl_from,"%s",nm); break; }
            }
        }
        free(b); fclose(f);
    }
    for(int i=0;i<n;i++) free(list[i]); free(list);

}
static void init(KCtx *g){
    const char *e=getenv("KHNMC_EVENTS"); int m;
    if(!e||!*e||!strcmp(e,"real")) m=M_REAL;
    else if(!strcmp(e,"stub")) m=M_STUB;
    else if(!strcmp(e,"stub+music")) m=M_STUB_MUSIC;
    else { kh_warn("events-mode: KHNMC_EVENTS=%s is not a mode; using real",e); m=M_REAL; }
    if(m!=M_REAL){
        load_template(g);
        if(!have_tmpl){kh_warn("events-stub-template: no area file in the data tree provides the required template; using real"); mode=M_REAL; return;}
        fprintf(stderr,"kh: events stub: KHNMC_EVENTS=%s: every event entry is answered with the mod's %d-byte empty scene (taken from %s)%s\n",
                                              e,STUB_LEN,tmpl_from,m==M_STUB_MUSIC?" plus the entry's own music commands":""); }
    mode=m;
}
/* selftest only */
void kh_events_stub_test_reset(void){ mode=M_UNSET; have_tmpl=0; n_lookup=n_none=n_already=n_already_music=n_replaced=n_music_cmds=n_carried=n_carried_keep=0; }

static u16 g16(KCtx *g,u64 a){ return rd16(g,a); }
/* The entry at guest address p -> the address to hand to the game.  Bounds: every byte read is checked to be guest memory. */
u64 kh_events_stub_build(KCtx *g,u64 p){
    if(mode==M_UNSET){ pthread_mutex_lock(&em); if(mode==M_UNSET) init(g); pthread_mutex_unlock(&em); }
    if(mode==M_REAL||!p) return p;
    /* walk the entry: its commands' types, the first command (project header), the music commands */
    u64 q=p; int ncmd=0, nplain=0, stub_shape=1; static const u16 want[5]={0x00,0x0f,0x10,0x08,0x12};
    u64 music[160]; int nmusic=0, keep=0; u64 project=0; u16 project_len=0;
    for(;;){
        if(!GA_OK(q)||!GA_OK(q+3)) kh_fatal(g,"events-stub: the event entry at guest 0x%llx runs out of guest memory at 0x%llx",(unsigned long long)p,(unsigned long long)q);
        u16 len=g16(g,q); if(!len) break;
        if(len<4||q+len-p>EV_MAX||!GA_OK(q+len+1)) kh_fatal(g,"events-stub: the event entry at guest 0x%llx has a command of %u bytes at +0x%llx: not an event",(unsigned long long)p,len,(unsigned long long)(q-p));
        u16 ty=g16(g,q+2);
        if(!ncmd&&ty==0){ project=q; project_len=len; }
        if(ty==0x2e||ty==0x2f||ty==0x30){ if(nmusic>=160) kh_fatal(g,"events-stub: more than 160 music commands in the event entry at guest 0x%llx",(unsigned long long)p); music[nmusic++]=q;
            if(ty==0x2e&&len>=8&&g16(g,q+6)) keep=1; }                                          /* volume command, flag word non-zero: 0x2cf490 / 0x2cf5d0 set the keep flag */
        else { if(nplain>=5||ty!=want[nplain<5?nplain:0]) stub_shape=0; nplain++; }
        ncmd++; q+=len;
    }
    /* the mod's own empty scene: untouched.  That includes its two stubs that carry a music command (eh03 703 / 704: the stub's five
     * commands plus one volume command, the mod's way to fade the track before the next program changes it). */
    if(stub_shape&&nplain==5){ n_already++; if(nmusic) n_already_music++; return p; }
    pthread_mutex_lock(&em);
    unsigned slot=ri++%RING; if(!ring[slot]){ ring[slot]=ga_alloc(BUF_LEN,16); if(!ring[slot]){ pthread_mutex_unlock(&em); kh_fatal(g,"events-stub: no guest memory for a stub"); } }
    u64 b=ring[slot]; pthread_mutex_unlock(&em);
    u8 *o=hp(g,b); size_t n=0;
    int carry=nmusic&&(mode==M_STUB_MUSIC||keep);                                       /* plain stub mode: only a scene whose music state survives it */
    /* template: [0x00 project 24][0x0f 4][0x10 8][0x08 8][0x12 10][00 00] */
    size_t tp=0; u16 l0=(u16)(tmpl[0]|tmpl[1]<<8);
    if(carry&&project&&project_len<=64){ memcpy(o,hp(g,project),project_len); n=project_len; }   /* the entry's own project header: its memory size */
    else { memcpy(o,tmpl,l0); n=l0; }
    tp=l0;
    for(int k=1;k<4;k++){ u16 l=(u16)(tmpl[tp]|tmpl[tp+1]<<8); memcpy(o+n,tmpl+tp,l); n+=l; tp+=l; }                  /* setup, start, end frame */
    u16 end=(u16)(tmpl[tp-4]|tmpl[tp-3]<<8);                                                                         /* the end frame (50) */
    u16 lf=(u16)(tmpl[tp]|tmpl[tp+1]<<8), fade_frame=(u16)(tmpl[tp+4]|tmpl[tp+5]<<8);
    if(carry){
        n_carried++; if(keep) n_carried_keep++;
        u16 cap=end>10?(u16)(end-10):0;
        for(int pass=0;pass<2;pass++){
            if(pass==1){ memcpy(o+n,tmpl+tp,lf); n+=lf; }                                                             /* the fade, between the frame-0 and the later music commands */
            for(int i=0;i<nmusic;i++){
                u16 len=g16(g,music[i]), ty=g16(g,music[i]+2), fr=g16(g,music[i]+4); u16 nf=fr<cap?fr:cap;
                int early=ty==0x2f||nf<=fade_frame-1;                                                                 /* reads first (the load pass takes them whatever their frame) */
                if((pass==0)!=early) continue;
                if(n+len+lf+4>BUF_LEN) kh_fatal(g,"events-stub: the music commands of the event entry at guest 0x%llx do not fit a stub",(unsigned long long)p);
                memcpy(o+n,hp(g,music[i]),len); if(ty!=0x2f){ o[n+4]=(u8)nf; o[n+5]=(u8)(nf>>8); } n+=len; n_music_cmds++;
            }
        }
    } else { memcpy(o+n,tmpl+tp,lf); n+=lf; }
    o[n]=0; o[n+1]=0; n+=2; memset(o+n,0,16);
    n_replaced++;
    return b;
}
/* exe 0x3a1f70(name as 4 characters) -> the entry's data, or 0.  Its only caller is the event starter 0x3ac350. */
u64 kh_input_game_frames(void);
/* Both event passes use the same object row. Keep the logical actor and slot in
 * the event table: f_2d04e0 uses the original ID to find the ANB directory. */
enum { ACTOR_SORA = 571, NMC_ROXAS = 84, EVENT_OBJECT = 1u << 30,
       DONALD_WEAPON = 575, GOOFY_WEAPON = 576, MAGES_STAFF = 108, KNIGHTS_SHIELD = 109 };
#include "xion_swap.h"
static int scene_roxas(KCtx *g){
    if(kh_xion_enabled) return 1;
    const char *v=getenv("KHNMC_SCENE_ROXAS");
    if(!v || !*v || !strcmp(v,"0")) return 0;
    if(!strcmp(v,"1")) return 1;
    kh_fatal(g,"scene-roxas-mode: KHNMC_SCENE_ROXAS=%s (expected 0 or 1)",v);
}

/* A scene can name a partner's equipped weapon even with an empty party sheet.
 * Supply the stock staff/shield prop to both event passes, without equipping it. */
static int weapon_actor(u32 id){ return id==DONALD_WEAPON || id==GOOFY_WEAPON; }
static u32 event_weapon(KCtx *g,u32 actor){
    KCtx probe=*g;
    if(kh_call(&probe,GS_BASE+0x3dfeb0,EVENT_OBJECT|actor,0,0,0)) return actor;
    u32 prop=actor==DONALD_WEAPON?MAGES_STAFF:KNIGHTS_SHIELD;
    fprintf(stderr,"kh: event-weapon-fallback: actor %u has no equipped model, using stock prop %u\n",actor,prop);
    return prop;
}

static int high_sora(u32 id){ return id==572 || id==264; } /* ACTOR_SORA_H or explicit H_EX500 */

void orig_f_2d0350(KCtx *g);
void f_2d0350(KCtx *g){
    if(high_sora((u32)A1(g)) && scene_roxas(g)) kh_scene_pack_begin();
    if((u32)A1(g)==ACTOR_SORA && scene_roxas(g)){
        A1(g)=NMC_ROXAS;
        fprintf(stderr,"kh: scene Roxas: frame=%llu preload P_EX100 slot=%u\n",
                (unsigned long long)kh_input_game_frames(),(u32)A2(g));
    }
    if(weapon_actor((u32)A1(g))) A1(g)=event_weapon(g,(u32)A1(g));
    orig_f_2d0350(g);
}

static int event_spawn_site(u64 caller){
    return caller==GS_BASE+0x2d21db || caller==GS_BASE+0x2d2453 || caller==GS_BASE+0x2d2552;
}
static int sora_spawn(u32 id,u64 caller){
    return id==(EVENT_OBJECT|ACTOR_SORA) && event_spawn_site(caller);
}

void orig_f_3df930(KCtx *g);
void f_3df930(KCtx *g){
    if((u32)A0(g)==(EVENT_OBJECT|ACTOR_SORA) && scene_roxas(g) &&
       sora_spawn((u32)A0(g),rd64(g,KH_RSP(g)))){
        A0(g)=EVENT_OBJECT|NMC_ROXAS;
        fprintf(stderr,"kh: scene Roxas: frame=%llu spawn P_EX100\n",
                (unsigned long long)kh_input_game_frames());
    }
    u32 id=(u32)A0(g);
    if(high_sora(id ^ EVENT_OBJECT) && event_spawn_site(rd64(g,KH_RSP(g))) && scene_roxas(g)) kh_scene_pack_begin();
    if((id & EVENT_OBJECT) && weapon_actor(id ^ EVENT_OBJECT)){
        u64 ret=rd64(g,KH_RSP(g));
        if(event_spawn_site(ret))
            A0(g)=EVENT_OBJECT|event_weapon(g,id & ~EVENT_OBJECT);
    }
    orig_f_3df930(g);
}

void f_3a1f70(KCtx *g){
    u32 nm=(u32)A0(g);                                   /* the entry's name: 4 characters in ecx (call site 0x3ac388) */
    orig_f_3a1f70(g);
    n_lookup++;
    u64 p=KH_RAX(g);
    { extern u64 kh_cutscenes_answer(KCtx *,u32,u64); u64 c=kh_cutscenes_answer(g,nm,p);   /* task 50: KHNMC_CUTSCENES=story (runtime/cutscenes.c) */
      if(c!=p){ SET_RAX(g,c); return; } }                                                  /* an original served: it is not a stub to replace */
    if(!p){ n_none++; if(mode>M_REAL) fprintf(stderr,"kh: events stub: game-frame=%llu map %02x:%02x event %.4s: no entry of that name\n",(unsigned long long)kh_input_game_frames(),
                                                         rd8(g,GS_BASE+0x717008),rd8(g,GS_BASE+0x717009),(const char*)&nm); return; }
    u64 before=n_replaced, before_c=n_carried, before_m=n_music_cmds;
    u64 q=kh_events_stub_build(g,p);
    if(q!=p) SET_RAX(g,q);
    if(mode>M_REAL){                                     /* one line per lookup (a few per map): which scene the run did not play */
        char proj[16]={0}; if(rd16(g,p+2)==0&&rd16(g,p)>=10) for(int i=0;i<15&&i+10<rd16(g,p);i++){ u8 c=rd8(g,p+10+i); if(!c) break; proj[i]=(c>=32&&c<127)?(char)c:'?'; }
        fprintf(stderr,"kh: events stub: game-frame=%llu map %02x:%02x event %.4s (%s): %s\n",(unsigned long long)kh_input_game_frames(),rd8(g,GS_BASE+0x717008),rd8(g,GS_BASE+0x717009),
                (const char*)&nm,proj[0]?proj:"?",n_replaced==before?"the mod's stub already, passed through":n_carried==before_c?"a real scene, answered with the stub":
                mode==M_STUB?"a real scene that sets the keep flag, answered with the stub plus its music commands":"a real scene, answered with the stub plus its music commands");
        if(n_carried!=before_c) fprintf(stderr,"kh: events stub: event %.4s: %llu music commands carried\n",(const char*)&nm,(unsigned long long)(n_music_cmds-before_m));
    }
}
void kh_events_stub_report(FILE *f){
    { extern void kh_cutscenes_report(FILE *); kh_cutscenes_report(f); }                    /* task 50 */
    if(mode>M_REAL) fprintf(f,"kh: events stub: mode %s: %llu event lookups, %llu replaced by the stub, %llu were the mod's stub already (%llu of them its stub with a music command), %llu found no entry; "
                            "%llu stubs carry their scene's music commands (%llu because the scene sets the keep flag), %llu music commands carried over\n",
                            mode==M_STUB?"stub":"stub+music",(unsigned long long)n_lookup,(unsigned long long)n_replaced,(unsigned long long)n_already,(unsigned long long)n_already_music,(unsigned long long)n_none,
                            (unsigned long long)n_carried,(unsigned long long)n_carried_keep,(unsigned long long)n_music_cmds);
}

/* ---- selftest: a synthetic tree for the template, synthetic entries in guest memory; the fatal cases in forked children ------- */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"events stub selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static size_t cmd(u8 *o,u16 type,const u8 *payload,u16 plen){ u16 len=(u16)(plen+4); o[0]=(u8)len; o[1]=(u8)(len>>8); o[2]=(u8)type; o[3]=(u8)(type>>8); if(plen) memcpy(o+4,payload,plen); return len; }
/* a synthetic stub of the mod's shape (56 bytes) and a synthetic real scene; nothing here is the game's data */
static size_t make_stub(u8 *o,const char *name){
    u8 pj[20]={0}; pj[0]=0x00; pj[1]=0x10; pj[2]=0x20; pj[4]=2; snprintf((char*)pj+6,14,"%s",name); size_t n=cmd(o,0x00,pj,20);
    n+=cmd(o+n,0x0f,NULL,0); u8 st[4]={0}; n+=cmd(o+n,0x10,st,4); u8 en[4]={50,0,0x12,0}; n+=cmd(o+n,0x08,en,4); u8 fd[6]={1,0,0,0,1,0}; n+=cmd(o+n,0x12,fd,6); o[n]=0; o[n+1]=0; return n+2;
}
static size_t make_real(u8 *o,int with_music){                 /* 0: no music; 1: music commands; 2: and the keep flag */
    u8 pj[20]={0}; pj[0]=0x00; pj[1]=0xd4; pj[2]=0x1a; pj[4]=2; snprintf((char*)pj+6,14,"zz_event_777"); size_t n=cmd(o,0x00,pj,20);
    n+=cmd(o+n,0x0f,NULL,0); u8 st[4]={20,0,0,0}; n+=cmd(o+n,0x10,st,4); u8 en[4]={0xb9,0x05,0x12,0}; n+=cmd(o+n,0x08,en,4);
    u8 as[40]; memset(as,0x41,sizeof as); n+=cmd(o+n,0x24,as,40);
    if(with_music){ u8 rd[4]={0,0,118,0}; n+=cmd(o+n,0x2f,rd,4); }
    u8 ac[12]={0x8e,0x08,0,0,'p','_','e','x',0,0,0,0}; n+=cmd(o+n,0x01,ac,12);
    if(with_music){ u8 pl[6]={0,0,0,0,118,0}; n+=cmd(o+n,0x30,pl,6); u8 v1[8]={0x3c,0x04,0,0,0,0,3,0}; n+=cmd(o+n,0x2e,v1,8); }
    u8 fd[6]={0x9c,0x05,0x14,0,1,0}; n+=cmd(o+n,0x12,fd,6);
    if(with_music){ u8 v2[8]={0xb0,0x05,(u8)(with_music==2),0,0,2,0,0}; n+=cmd(o+n,0x2e,v2,8); }
    o[n]=0; o[n+1]=0; return n+2;
}
static void write_ard(const char *path,const u8 *ev,size_t evlen,int type){
    u8 h[16+32]={0}; memcpy(h,"BAR\1",4); h[4]=2;
    h[16]=13; memcpy(h+20,"evt",3); h[24]=48; h[28]=4;                         /* an area script entry first: 4 bytes */
    h[32]=(u8)type; memcpy(h+36,"101",3); h[40]=52; h[44]=(u8)evlen;
    FILE *f=fopen(path,"wb"); if(!f) kh_fatal(NULL,"events stub selftest: cannot write %s",path);
    fwrite(h,1,48,f); fwrite("\xff\xff\0\0",1,4,f); fwrite(ev,1,evlen,f); fclose(f);
}
static void child_build(KCtx *g,u64 p){ kh_events_stub_build(g,p); }
static int in_child(KCtx *g,u64 arg,char *out,size_t cap){
    FILE *cf=kh_tmpfile(); if(!cf) kh_fatal(g,"events stub selftest: tmpfile failed");
    pid_t c=kh_fork(); if(c<0) kh_fatal(g,"events stub selftest: fork failed");
    if(!c){ if(dup2(fileno(cf),STDERR_FILENO)<0) _exit(98); child_build(g,arg); _exit(99); }
    int st=0; if(waitpid(c,&st,0)!=c){ fclose(cf); return -1; }
    rewind(cf); size_t n=fread(out,1,cap-1,cf); out[n]=0; fclose(cf); return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
/* the command types of a guest entry, as a string like "00 0f 10 08 12" */
static void types_of(KCtx *g,u64 p,char *out,size_t cap){ size_t n=0; out[0]=0; for(;;){ u16 len=rd16(g,p); if(!len||len<4) break; n+=(size_t)snprintf(out+n,cap-n,"%s%02x",n?" ":"",rd16(g,p+2)); p+=len; if(n+4>=cap) break; } }
/* ---- tree check (runs when KHNMC_DATA names a tree with ard/us): every type-22 entry of every area file through the builder ---- */
typedef struct { int trk[2], rec[2], keep, nreads; int reads[16]; } MusicEnd;
static int vol_target(int a,int b){ static const int t[3]={0x3000,0x1800,0}; if(!a&&!b) return 0x3000; if(a==1&&b==1) return 0x1800; return b<3?t[b]:-2; }
/* the music state an entry leaves (exe 0x2cf3b0 play, 0x2cf5d0 / 0x2cf490 volume, 0x2cf220 read): host bytes */
static int music_end(const u8 *d,size_t n,MusicEnd *m,int *plain_types,int *nplain,int *end_frame){
    memset(m,0,sizeof *m); m->trk[0]=m->trk[1]=m->rec[0]=m->rec[1]=-1; *nplain=0; *end_frame=-1;
    for(size_t p=0;;){
        if(p+2>n) return 0;
        u16 len=(u16)(d[p]|d[p+1]<<8); if(!len) return 1;
        if(len<4||p+len>n) return 0;
        u16 ty=(u16)(d[p+2]|d[p+3]<<8); const u8 *pl=d+p+4;
        if(ty==0x30&&len>=10){ int slot=pl[2]|pl[3]<<8, t=pl[4]|pl[5]<<8; for(int s=0;s<2;s++) if(slot==s||slot==2) m->trk[s]=t; }
        else if(ty==0x2e&&len>=12){ int slot=pl[7]; if(pl[2]|pl[3]) m->keep=1; if(slot<2) m->rec[slot]=vol_target(pl[4],pl[5]); }
        else if(ty==0x2f&&len>=8){ if(m->nreads<16) m->reads[m->nreads++]=pl[2]|pl[3]<<8; }
        else { if(*nplain<16) plain_types[*nplain]=ty; (*nplain)++; if(ty==0x08&&len>=6) *end_frame=pl[0]|pl[1]<<8; }
        p+=len;
    }
}
static int tree_check(KCtx *g,const char *root){
    int tc=0,tb=0; char dir[4096];
#define TT(c) do{ tc++; if(!(c)){ tb++; if(tb<20) fprintf(stderr,"events stub tree selftest FAIL: %s/%.4s: %s (line %d)\n",nm,ename,#c,__LINE__); } }while(0)
    if(!root||!*root||!kh_resolve_ci(root,"ard/us",dir,sizeof dir)){
        fprintf(stderr,"kh: events stub tree check: not run: KHNMC_DATA (%s) has no ard/us (run the selftest with KHNMC_DATA=<the data tree> for it)\n",root&&*root?root:"unset");
        return 0;
    }
    setenv("KHNMC_DATA",root,1); setenv("KHNMC_EVENTS","stub",1); kh_events_stub_test_reset(); have_tmpl=0;
    struct dirent **list=NULL; int nf=scandir(dir,&list,NULL,alphasort); if(nf<0) kh_fatal(g,"events stub tree check: cannot list %s",dir);
    long n_ent=0,n_pass=0,n_plain=0,n_carry=0; static const int want[5]={0x00,0x0f,0x10,0x08,0x12};
    for(int i=0;i<nf;i++){
        const char *nm=list[i]->d_name; size_t l=strlen(nm); if(l<5||strcasecmp(nm+l-4,".ard")) continue;
        char p[4500],rel[512]; snprintf(rel,sizeof rel,"ard/us/%s",nm);
        if(!kh_rmc_path(rel,p,sizeof p)) snprintf(p,sizeof p,"%s/%s",dir,nm); FILE *f=fopen(p,"rb"); if(!f) kh_fatal(g,"events stub tree check: cannot read %s",p);
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); u8 *b=malloc((size_t)sz+1); if(!b||fread(b,1,(size_t)sz,f)!=(size_t)sz) kh_fatal(g,"events stub tree check: cannot read %s",p); fclose(f);
        u32 cnt=sz>=16&&!memcmp(b,"BAR\1",4)?(u32)(b[4]|b[5]<<8|b[6]<<16|(u32)b[7]<<24):0;
        for(u32 k=0;k<cnt&&16+16ull*(k+1)<=(u64)sz;k++){
            const u8 *e=b+16+16*k; u16 type=(u16)(e[0]|e[1]<<8); char ename[5]; memcpy(ename,e+4,4); ename[4]=0;
            u32 off=(u32)(e[8]|e[9]<<8|e[10]<<16|(u32)e[11]<<24), esz=(u32)(e[12]|e[13]<<8|e[14]<<16|(u32)e[15]<<24);
            if(type!=22||(u64)off+esz>(u64)sz) continue;
            n_ent++;
            MusicEnd mo,mb; int po[16],pb[16],npo,npb,endo,endb;
            TT(music_end(b+off,esz,&mo,po,&npo,&endo));
            int shape=npo==5; for(int j=0;j<5&&j<npo;j++) if(po[j]!=want[j]) shape=0;
            u64 gp=ga_alloc(esz+16,16); memcpy(hp(g,gp),b+off,esz); memset(hp(g,gp)+esz,0,16);
            u64 r=kh_events_stub_build(g,gp);
            if(shape){ TT(r==gp); n_pass++; ga_free(gp); continue; }                            /* the mod's stub (with or without a volume command) */
            TT(r&&r!=gp);
            u8 *rb=hp(g,r); size_t rn=0; while(rn+2<=BUF_LEN&&(rb[rn]|rb[rn+1]<<8)) rn+=(size_t)(rb[rn]|rb[rn+1]<<8); rn+=2;
            TT(music_end(rb,rn,&mb,pb,&npb,&endb));
            int bshape=npb==5; for(int j=0;j<5&&j<npb;j++) if(pb[j]!=want[j]) bshape=0;
            TT(bshape); TT(endb==50);                                                            /* the stub's five commands, its end frame */
            if(mo.keep){
                n_carry++;
                TT(mb.keep); TT(mb.trk[0]==mo.trk[0]&&mb.trk[1]==mo.trk[1]); TT(mb.rec[0]==mo.rec[0]&&mb.rec[1]==mo.rec[1]);   /* the same music state at its end */
                TT(mb.nreads==mo.nreads&&!memcmp(mb.reads,mo.reads,sizeof(int)*(size_t)mo.nreads));
                TT(!memcmp(rb,b+off,(size_t)(b[off]|b[off+1]<<8)));                               /* its own project header */
            } else { n_plain++; TT(rn==STUB_LEN&&!memcmp(rb,tmpl,STUB_LEN)); }                  /* exactly the mod's stub */
            ga_free(gp);
        }
        free(b);
    }
    for(int i=0;i<nf;i++) free(list[i]); free(list);
#undef TT
    fprintf(stderr,"kh: events stub tree check: %ld event entries in %s: %ld the mod's stub (passed through), %ld answered with the plain stub, %ld with the stub plus their music commands (the keep flag)\n",
            n_ent,dir,n_pass,n_plain,n_carry);
    fprintf(stderr,"kh: events stub tree selftest: %d checks, %d failures\n",tc,tb);
    return tb;
}
int kh_events_stub_selftest(KCtx *g){
    checks=bad=0;
    T(sora_spawn(EVENT_OBJECT|571,GS_BASE+0x2d21db));
    T(sora_spawn(EVENT_OBJECT|571,GS_BASE+0x2d2453));
    T(sora_spawn(EVENT_OBJECT|571,GS_BASE+0x2d2552));
    T(!sora_spawn(571,GS_BASE+0x2d21db));
    T(!sora_spawn(EVENT_OBJECT|571,GS_BASE+0x3df970));
    T(!sora_spawn(EVENT_OBJECT|572,GS_BASE+0x2d21db));
    T(!sora_spawn(EVENT_OBJECT|90,GS_BASE+0x2d21db));
    T(!sora_spawn(EVENT_OBJECT|707,GS_BASE+0x2d21db));
    const char *keys[]={"KHNMC_DATA","KHNMC_EVENTS"}; char *old[2]; for(int i=0;i<2;i++){ const char *v=getenv(keys[i]); old[i]=v?strdup(v):NULL; }
    int save_mode=mode, save_have=have_tmpl; u8 save_tmpl[STUB_LEN]; memcpy(save_tmpl,tmpl,STUB_LEN);
    u64 sv[8]={n_lookup,n_none,n_already,n_replaced,n_music_cmds,n_already_music,n_carried,n_carried_keep};
    char dir[KH_TMP_MAX],p[KH_TMP_MAX+300]; kh_tmp_path(dir,sizeof dir,"khnmc-evstub-XXXXXX"); if(!mkdtemp(dir)) return 1;
    snprintf(p,sizeof p,"%s/ard",dir); mkdir(p,0700); snprintf(p,sizeof p,"%s/ard/us",dir); mkdir(p,0700);
    u8 stub[80],real[400],realm[400],realk[400]; size_t ls=make_stub(stub,"aa_event_001"), lr=make_real(real,0), lm=make_real(realm,1), lk=make_real(realk,2);
    T(ls==STUB_LEN); T(shape_is_stub(stub,ls)); T(!shape_is_stub(real,lr)); T(!shape_is_stub(stub,ls-2)); T(!shape_is_stub(stub,10));
    { u8 six[80]; memcpy(six,stub,ls-2); u8 fd[6]={2,0,0,0,1,0}; size_t n=ls-2+cmd(six+ls-2,0x12,fd,6); six[n]=0; six[n+1]=0; T(!shape_is_stub(six,n+2)); }   /* a sixth command: not the stub */
    snprintf(p,sizeof p,"%s/ard/us/aa00.ard",dir); write_ard(p,real,lr,22);          /* first file by name: a real scene only */
    snprintf(p,sizeof p,"%s/ard/us/bb00.ard",dir); write_ard(p,stub,ls,22);          /* the template comes from here */
    snprintf(p,sizeof p,"%s/ard/us/notes.txt",dir); { FILE *f=fopen(p,"w"); if(f){ fputs("not an area file",f); fclose(f); } }
    u64 gs=ga_alloc(96,16), gr=ga_alloc(512,16), gm=ga_alloc(512,16), gk=ga_alloc(512,16); memcpy(hp(g,gs),stub,ls); memcpy(hp(g,gr),real,lr); memcpy(hp(g,gm),realm,lm); memcpy(hp(g,gk),realk,lk);
    setenv("KHNMC_DATA",dir,1);
    /* default: the entries as they are */
    unsetenv("KHNMC_EVENTS"); kh_events_stub_test_reset(); T(kh_events_stub_build(g,gr)==gr); T(kh_events_stub_build(g,gs)==gs); T(n_replaced==0);
    setenv("KHNMC_EVENTS","real",1); kh_events_stub_test_reset(); T(kh_events_stub_build(g,gm)==gm);
    /* stub: a real scene is answered with the template's bytes, the mod's own stub passes through, 0 stays 0 */
    setenv("KHNMC_EVENTS","stub",1); kh_events_stub_test_reset();
    u64 a=kh_events_stub_build(g,gr); T(a&&a!=gr); T(have_tmpl&&!strcmp(tmpl_from,"bb00.ard")); T(!memcmp(hp(g,a),stub,STUB_LEN)); T(n_replaced==1);
    u64 b2=kh_events_stub_build(g,gm); T(b2&&b2!=gm&&b2!=a); T(!memcmp(hp(g,b2),stub,STUB_LEN));          /* music commands without the keep flag: NOT carried in plain stub mode */
    T(n_carried==0&&n_music_cmds==0);
    { u64 k=kh_events_stub_build(g,gk); char ty[200]; types_of(g,k,ty,sizeof ty);                           /* the keep flag: carried, also in plain stub mode */
      T(!strcmp(ty,"00 0f 10 08 2f 30 12 2e 2e")); T(!memcmp(hp(g,k),realk,24)); T(n_carried==1&&n_carried_keep==1&&n_music_cmds==4);
      u64 q=k+24+4+8+8+8+10+10+12; T(rd16(g,q+2)==0x2e&&rd16(g,q+6)==1&&rd8(g,q+9)==2); n_carried=n_carried_keep=n_music_cmds=0; }
    T(kh_events_stub_build(g,gs)==gs); T(n_already==1); T(kh_events_stub_build(g,0)==0);
    { u8 near[120]; size_t n=0; u8 v[8]={0,0,0,0,0,1,1,0};                         /* the mod's stub with a volume command in it (the shape of eh03 703 / 704) */
      memcpy(near,stub,24+4+8); n=36; n+=cmd(near+n,0x2e,v,8); memcpy(near+n,stub+36,ls-36); n+=ls-36;
      u64 gn=ga_alloc(160,16); memcpy(hp(g,gn),near,n); T(kh_events_stub_build(g,gn)==gn); T(n_already==2&&n_already_music==1);
      u8 a2[4]={0,0,0,0}; size_t m=36; m+=cmd(near+m,0x14,a2,2); memcpy(near+m,stub+36,ls-36); m+=ls-36; memcpy(hp(g,gn),near,m);   /* any other command in it: a scene, replaced */
      u64 r2=kh_events_stub_build(g,gn); T(r2!=gn&&!memcmp(hp(g,r2),stub,STUB_LEN)); ga_free(gn); }
    T(!memcmp(hp(g,gr),real,lr)); T(!memcmp(hp(g,gm),realm,lm));                                           /* the entries themselves are not written */
    { u64 seen[RING+2]; int distinct=1; for(int i=0;i<RING+2;i++){ seen[i]=kh_events_stub_build(g,gr); for(int k=0;k<i&&k<RING-1;k++) if(i-k<RING&&seen[i]==seen[k]) distinct=0; } T(distinct); T(seen[RING]==seen[0]); }   /* a ring: the last 8 answers stay valid */
    /* stub+music: own project header, the reads, the frame-0 commands, the fade, the later commands at clamped frames */
    setenv("KHNMC_EVENTS","stub+music",1); kh_events_stub_test_reset();
    u64 c=kh_events_stub_build(g,gm); char ty[200]; types_of(g,c,ty,sizeof ty);
    T(!strcmp(ty,"00 0f 10 08 2f 30 12 2e 2e")); T(!memcmp(hp(g,c),realm,24)); T(memcmp(hp(g,c),stub,24)!=0);
    { u64 q=c+24+4+8; T(rd16(g,q+4)==50);                                                                   /* the stub's end frame, not the scene's 1465 */
      q+=8; T(rd16(g,q+2)==0x2f&&rd16(g,q+6)==118); q+=8; T(rd16(g,q+2)==0x30&&rd16(g,q+4)==0&&rd16(g,q+8)==118); q+=10; T(rd16(g,q+2)==0x12&&rd16(g,q+4)==1);
      q+=10; T(rd16(g,q+2)==0x2e&&rd16(g,q+4)==40&&rd8(g,q+10)==3); q+=12; T(rd16(g,q+2)==0x2e&&rd16(g,q+4)==40&&rd16(g,q+6)==0&&rd8(g,q+9)==2); q+=12; T(rd16(g,q)==0); }
    T(n_music_cmds==4);
    u64 d=kh_events_stub_build(g,gr); T(!memcmp(hp(g,d),stub,STUB_LEN));                                    /* a scene without music commands: the plain stub */
    T(kh_events_stub_build(g,gs)==gs);
    { FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){ kh_events_stub_report(rf); rewind(rf); char bb[400]; size_t n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf); T(strstr(bb,"mode stub+music")&&strstr(bb,"2 replaced by the stub, 1 were the mod's stub already (0 of them")&&strstr(bb,"1 stubs carry their scene's music commands (0 because")&&strstr(bb,"4 music commands carried over")); } }
    if(!kh_fork_ok()) kh_selftest_not_run("events stub","the named fatals events-mode, events-stub-template and events-stub (4 children)",9);
    else {
        char out[16384]; int rc;
        setenv("KHNMC_EVENTS","none",1); kh_events_stub_test_reset(); rc=in_child(g,gr,out,sizeof out); T(rc==99); T(strstr(out,"events-mode: KHNMC_EVENTS=none is not a mode")!=NULL);
        setenv("KHNMC_EVENTS","stub",1); snprintf(p,sizeof p,"%s/ard/us/bb00.ard",dir); write_ard(p,stub,ls,21);   /* same bytes, but not an event entry */
        kh_events_stub_test_reset(); rc=in_child(g,gr,out,sizeof out); T(rc==99); T(strstr(out,"events-stub-template:")!=NULL&&strstr(out,"no area file in")!=NULL);
        write_ard(p,stub,ls,22); kh_events_stub_test_reset();
        u64 gb=ga_alloc(64,16); memset(hp(g,gb),0,64); wr16(g,gb,3); wr16(g,gb+2,1);                                /* a command of 3 bytes */
        rc=in_child(g,gb,out,sizeof out); T(rc==3); T(strstr(out,"events-stub: the event entry at guest")!=NULL&&strstr(out,"not an event")!=NULL);
        rc=in_child(g,gr,out,sizeof out); T(rc==99); T(strstr(out,"kh: FATAL:")==NULL);
        ga_free(gb);
    }
    int tbad=tree_check(g,old[0]);                                                                         /* the real tree, when the selftest is given one */
    for(int i=0;i<2;i++){ if(old[i]) setenv(keys[i],old[i],1); else unsetenv(keys[i]); free(old[i]); }
    mode=save_mode; have_tmpl=save_have; memcpy(tmpl,save_tmpl,STUB_LEN); n_lookup=sv[0]; n_none=sv[1]; n_already=sv[2]; n_replaced=sv[3]; n_music_cmds=sv[4]; n_already_music=sv[5]; n_carried=sv[6]; n_carried_keep=sv[7];
    ga_free(gs); ga_free(gr); ga_free(gm); ga_free(gk);
    const char *rm[]={"ard/us/aa00.ard","ard/us/bb00.ard","ard/us/notes.txt"}; for(size_t i=0;i<3;i++){ snprintf(p,sizeof p,"%s/%s",dir,rm[i]); unlink(p); }
    snprintf(p,sizeof p,"%s/ard/us",dir); rmdir(p); snprintf(p,sizeof p,"%s/ard",dir); rmdir(p); rmdir(dir);
    fprintf(stderr,"kh: events stub selftest: %d checks, %d failures\n",checks,bad); return bad+tbad;
}
