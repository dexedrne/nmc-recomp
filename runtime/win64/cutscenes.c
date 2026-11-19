/* cutscenes.c (task 50) : KHNMC_CUTSCENES=story : the original story scenes in place of the Nobody May Cry mod's empty stub.
 *
 * The mod removed 670 in-engine scenes by replacing each one's entry (BAR type 22) in its area file with one 56-byte empty scene
 * (runtime/events_stub.c, task 32).  KHNMC_CUTSCENES=story answers a lookup that finds such a stub with the ORIGINAL entry of that
 * room and name, taken from a pack that tools/cutscene_pack.py writes from the original PC area files.  The pack holds only the
 * scenes that tool classified SAFE (implementation notes section 2: no movie, no command type the mod's own scenes never use, no actor
 * whose object row the mod changed, no room the mod rebuilt, every file present).  Where: exe 0x3a1f70 (find an entry by name in
 * the loaded area file; its one caller is the event starter 0x3ac350), after the game's own lookup, before events_stub.c.  Only an
 * entry that IS the mod's plain stub (exactly the five commands project, setup, start, end frame, fade) is replaced, and only when
 * the pack has that name for the room the game is in (the place's world and room bytes 0x140717008 / 0x140717009: the area file
 * the lookup searched).  Everything around the scene stays the mod's: the AreaSettings record that named it (its progress flag and
 * its jump), the programs, the music after it.
 *
 *   unset / `nmc`   the entries as they are in the files (the mod's stubs).  Nothing here reads or writes anything (the default).
 *   `story`         the pack's original for every stub it has; a stub it does not have stays the stub (counted, one log line each)
 *   anything else   named fatal `cutscenes-mode`; `story` together with KHNMC_EVENTS=stub / stub+music: named fatal `cutscenes-mode`
 * The pack: KHNMC_CUTSCENES_PACK, else `story-events.khcs` in KHNMC_DATA (case-insensitive).  Missing, unreadable or not a valid pack
 * (magic, version, sorted unique keys, bounds, alignment, checksum, class, an entry that is not an event: commands to a zero
 * terminator): named fatal `cutscenes-pack`.  An original is copied once into guest memory and stays there for the run (the game
 * reads the entry while the scene plays).
 * Test aid: KHNMC_CUTSCENES_SAVEDUMP=DIR writes the save block (GAME_SAVE 0x1409a98b0, SAVE_LEN bytes) to DIR/NNNN-<area>-<event>.bin
 * at every event lookup, in any mode, so that two runs can be compared scene by scene (files only: the game is not touched).
 * Counters at exit: `kh: cutscenes: ...` (only when KHNMC_CUTSCENES or the save dump is set). */
#include "khnmc_rt.h"
#include "warp.h"
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define PLACE_W    (GS_BASE+0x717008ull)
#define PLACE_R    (GS_BASE+0x717009ull)
#define SAVE_BASE  (GS_BASE+0x9a98b0ull)      /* GAME_SAVE: exe 0x152380 returns it */
#define SAVE_LEN   0x10fc0u
#define PACK_MAX   (64u<<20)
enum { C_UNSET=-1, C_NMC=0, C_STORY=1 };
typedef struct { char area[4], name[4]; u32 klass, off, size, hash; } PackRec;
static int cmode=C_UNSET;
static u8 *pack; static size_t pack_len; static PackRec *recs; static u32 nrec; static u64 *guest; static char pack_path[4200];
static char dump_dir[4096]; static int dump_on=-1; static unsigned dump_n;
static pthread_mutex_t cm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 c_lookup, c_none, c_real, c_stub, c_restored, c_kept, c_noplace, c_dumps;

static u32 g32(const u8 *p){ return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static u32 fnv1a(const u8 *p,size_t n){ u32 h=0x811c9dc5u; for(size_t i=0;i<n;i++){ h^=p[i]; h*=0x01000193u; } return h; }
/* an event entry: (u16 length incl. this header, u16 type) ..., a zero length ends it (exe 0x2c8f40) */
static int is_event(const u8 *d,size_t n){
    size_t p=0; int k=0;
    for(;;){ if(p+2>n) return 0; u16 len=(u16)(d[p]|d[p+1]<<8); if(!len) return k>0; if(len<4||p+len>n) return 0; p+=len; k++; }
}
/* the mod's plain stub: exactly project, setup, start, end frame, fade (the two eh03 stubs with a volume command are not it) */
static int plain_stub(KCtx *g,u64 p){
    static const u16 want[5]={0x00,0x0f,0x10,0x08,0x12}; u64 q=p; int k=0;
    for(;;){
        if(!GA_OK(q)||!GA_OK(q+3)) return 0;
        u16 len=rd16(g,q); if(!len) return k==5;
        if(len<4||k>=5||q+len-p>4096||rd16(g,q+2)!=want[k]) return 0;
        k++; q+=len;
    }
}
static int key_cmp(const char *a,const char *an,const char *b,const char *bn){ int c=memcmp(a,b,4); return c?c:memcmp(an,bn,4); }
static void pack_fatal(KCtx *g,const char *why){ kh_fatal(g,"cutscenes-pack: %s: %s (KHNMC_CUTSCENES=story needs the pack of original scenes that tools/cutscene_pack.py writes; set KHNMC_CUTSCENES_PACK or put story-events.khcs into the data tree)",pack_path,why); }
static void load_pack(KCtx *g){
    const char *e=getenv("KHNMC_CUTSCENES_PACK");
    if(e&&*e) snprintf(pack_path,sizeof pack_path,"%s",e);
    else { const char *root=getenv("KHNMC_DATA"); if(!root||!*root) root="data";
           if(!kh_resolve_ci(root,"story-events.khcs",pack_path,sizeof pack_path)) snprintf(pack_path,sizeof pack_path,"%s/story-events.khcs",root); }
    FILE *f=fopen(pack_path,"rb"); if(!f){ char w[200]; snprintf(w,sizeof w,"cannot open it (%s)",strerror(errno)); pack_fatal(g,w); }
    struct stat st; if(fstat(fileno(f),&st)||st.st_size<16||(u64)st.st_size>PACK_MAX){ fclose(f); pack_fatal(g,"not a file of 16 bytes .. 64 MiB"); }
    pack_len=(size_t)st.st_size; pack=malloc(pack_len); if(!pack){ fclose(f); pack_fatal(g,"out of memory"); }
    if(fread(pack,1,pack_len,f)!=pack_len){ fclose(f); pack_fatal(g,"short read"); }
    fclose(f);
    if(memcmp(pack,"KHCS",4)) pack_fatal(g,"not a KHCS pack (magic)");
    if(g32(pack+4)!=1) pack_fatal(g,"version is not 1");
    nrec=g32(pack+8); if(16+24ull*nrec>pack_len) pack_fatal(g,"the record table does not fit the file");
    recs=calloc(nrec?nrec:1,sizeof *recs); guest=calloc(nrec?nrec:1,sizeof *guest); if(!recs||!guest) pack_fatal(g,"out of memory");
    for(u32 i=0;i<nrec;i++){
        const u8 *r=pack+16+24*i; PackRec *x=&recs[i]; memcpy(x->area,r,4); memcpy(x->name,r+4,4);
        x->klass=g32(r+8); x->off=g32(r+12); x->size=g32(r+16); x->hash=g32(r+20);
        char w[200];
        if(x->klass!=1&&x->klass!=2){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) has class %u (1 SAFE, 2 SAFE-AFTER-EDIT)",i,x->area,x->name,x->klass); pack_fatal(g,w); }
        if(x->off%16||(u64)x->off+x->size>pack_len||x->off<16+24ull*nrec){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) lies outside the data area",i,x->area,x->name); pack_fatal(g,w); }
        if(fnv1a(pack+x->off,x->size)!=x->hash){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) fails its checksum",i,x->area,x->name); pack_fatal(g,w); }
        if(!is_event(pack+x->off,x->size)){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) is not an event entry (commands to a zero terminator)",i,x->area,x->name); pack_fatal(g,w); }
        if(i&&key_cmp(recs[i-1].area,recs[i-1].name,x->area,x->name)>=0){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) is not after the one before it (the keys must be sorted and unique)",i,x->area,x->name); pack_fatal(g,w); }
    }
    fprintf(stderr,"kh: cutscenes: KHNMC_CUTSCENES=story: %u original scenes from %s (%zu bytes) replace the mod's stub where the game asks for them\n",nrec,pack_path,pack_len);
}
static void cinit(KCtx *g){
    const char *e=getenv("KHNMC_CUTSCENES"); int m;
    if(!e||!*e||!strcmp(e,"nmc")) m=C_NMC;
    else if(!strcmp(e,"story")) m=C_STORY;
    else kh_fatal(g,"cutscenes-mode: KHNMC_CUTSCENES=%s is not a mode (nmc = the mod's empty scenes, the default; story = the original story scenes)",e);
    if(m==C_STORY){ const char *ev=getenv("KHNMC_EVENTS");
        if(ev&&*ev&&strcmp(ev,"real")) kh_fatal(g,"cutscenes-mode: KHNMC_CUTSCENES=story and KHNMC_EVENTS=%s contradict each other (the one restores scenes, the other empties them): unset one",ev);
        load_pack(g); }
    const char *d=getenv("KHNMC_CUTSCENES_SAVEDUMP");
    if(d&&*d){ snprintf(dump_dir,sizeof dump_dir,"%s",d); if(mkdir(dump_dir,0755)&&errno!=EEXIST) kh_fatal(g,"cutscenes-savedump: cannot make %s (%s)",dump_dir,strerror(errno)); dump_on=1; }
    else dump_on=0;
    cmode=m;
}
/* selftest only */
void kh_cutscenes_test_reset(void){
    cmode=C_UNSET; dump_on=-1; dump_n=0; free(pack); pack=NULL; pack_len=0; free(recs); recs=NULL;
    if(guest){ for(u32 i=0;i<nrec;i++) if(guest[i]) ga_free(guest[i]); } free(guest); guest=NULL; nrec=0;
    c_lookup=c_none=c_real=c_stub=c_restored=c_kept=c_noplace=c_dumps=0;
}
static void save_dump(KCtx *g,const char *area,u32 nm){
    if(!GA_OK(SAVE_BASE)||!GA_OK(SAVE_BASE+SAVE_LEN-1)) kh_fatal(g,"cutscenes-savedump: the save block 0x%llx..+0x%x is not guest memory",(unsigned long long)SAVE_BASE,SAVE_LEN);
    char nb[5]={0}; memcpy(nb,&nm,4); for(int i=0;i<4;i++) if(nb[i]&&(nb[i]<'0'||nb[i]>'z')) nb[i]='_';
    char p[4300]; snprintf(p,sizeof p,"%s/%04u-%s-%s.bin",dump_dir,dump_n++,area,nb[0]?nb:"none");
    FILE *f=fopen(p,"wb"); if(!f) kh_fatal(g,"cutscenes-savedump: cannot write %s (%s)",p,strerror(errno));
    if(fwrite(hp(g,SAVE_BASE),1,SAVE_LEN,f)!=SAVE_LEN){ fclose(f); kh_fatal(g,"cutscenes-savedump: short write to %s",p); }
    fclose(f); c_dumps++;
}
u64 kh_input_game_frames(void);
/* the event starter's lookup of entry `nm` found p (0 = none) -> the entry to hand to the game */
u64 kh_cutscenes_answer(KCtx *g,u32 nm,u64 p){
    if(cmode==C_UNSET){ pthread_mutex_lock(&cm); if(cmode==C_UNSET) cinit(g); pthread_mutex_unlock(&cm); }
    if(cmode==C_NMC&&!dump_on) return p;                                  /* the default: nothing read, nothing written */
    u8 w=rd8(g,PLACE_W), r=rd8(g,PLACE_R); char area[5]={0};
    if(w<KH_WARP_WORLDS&&r<100) snprintf(area,sizeof area,"%s%02u",kh_warp_world_names[w],(unsigned)r);
    if(dump_on) save_dump(g,area[0]?area:"none",nm);
    if(cmode==C_NMC) return p;
    c_lookup++;
    const char *nb=(const char*)&nm; unsigned long long gf=(unsigned long long)kh_input_game_frames();
    if(!p){ c_none++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: no entry of that name\n",gf,area[0]?area:"?",nb); return p; }
    if(!plain_stub(g,p)){ c_real++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: a scene the mod kept (passed through)\n",gf,area[0]?area:"?",nb); return p; }
    c_stub++;
    if(!area[0]){ c_noplace++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %02x:%02x event %.4s: the mod's stub, and the place is no room (kept)\n",gf,w,r,nb); return p; }
    char key[4]={0}; for(int i=0;i<4;i++) key[i]=nb[i];
    u32 lo=0,hi=nrec;
    while(lo<hi){ u32 mid=(lo+hi)/2; int c=key_cmp(recs[mid].area,recs[mid].name,area,key); if(!c){ lo=mid; hi=mid+1; break; } if(c<0) lo=mid+1; else hi=mid; }
    if(lo>=nrec||key_cmp(recs[lo].area,recs[lo].name,area,key)){ c_kept++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: the mod's stub, no original in the pack (kept)\n",gf,area,nb); return p; }
    pthread_mutex_lock(&cm);
    if(!guest[lo]){ u64 b=ga_alloc(recs[lo].size+16,16); if(!b){ pthread_mutex_unlock(&cm); kh_fatal(g,"cutscenes: no guest memory for the %u-byte scene %s/%.4s",recs[lo].size,area,nb); }
                    memcpy(hp(g,b),pack+recs[lo].off,recs[lo].size); memset(hp(g,b)+recs[lo].size,0,16); guest[lo]=b; }
    u64 q=guest[lo]; pthread_mutex_unlock(&cm);
    c_restored++;
    fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: the original scene (%u bytes, %s) in place of the mod's stub\n",gf,area,nb,recs[lo].size,recs[lo].klass==1?"SAFE":"SAFE-AFTER-EDIT");
    return q;
}
void kh_cutscenes_report(FILE *f){
    if(cmode==C_STORY||dump_on==1)
        fprintf(f,"kh: cutscenes: mode %s: %llu event lookups: %llu originals served in place of the mod's stub, %llu stubs kept (no original in the pack), %llu stubs kept (no room), %llu scenes the mod kept, %llu found no entry; %llu save dumps\n",
                cmode==C_STORY?"story":"nmc",(unsigned long long)c_lookup,(unsigned long long)c_restored,(unsigned long long)c_kept,(unsigned long long)c_noplace,(unsigned long long)c_real,(unsigned long long)c_none,(unsigned long long)c_dumps);
}

/* ---- selftest: a synthetic pack in a temp tree, synthetic entries in guest memory; the fatal cases in forked children -------- */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"cutscenes selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static size_t cmd(u8 *o,u16 type,const u8 *pl,u16 n){ u16 len=(u16)(n+4); o[0]=(u8)len; o[1]=(u8)(len>>8); o[2]=(u8)type; o[3]=(u8)(type>>8); if(n) memcpy(o+4,pl,n); return len; }
static size_t mk_stub(u8 *o){ u8 pj[20]={0}; size_t n=cmd(o,0,pj,20); n+=cmd(o+n,0x0f,NULL,0); u8 st[4]={0}; n+=cmd(o+n,0x10,st,4); u8 en[4]={50,0,0x12,0}; n+=cmd(o+n,0x08,en,4);
                              u8 fd[6]={1,0,0,0,1,0}; n+=cmd(o+n,0x12,fd,6); o[n]=0; o[n+1]=0; return n+2; }
static size_t mk_scene(u8 *o,u8 fill){ u8 pj[20]={0}; pj[6]=fill; size_t n=cmd(o,0,pj,20); n+=cmd(o+n,0x0f,NULL,0); u8 en[4]={0x90,1,0,0}; n+=cmd(o+n,0x08,en,4);
                                       u8 a[40]; memset(a,fill,sizeof a); n+=cmd(o+n,0x24,a,40); o[n]=0; o[n+1]=0; return n+2; }
typedef struct { const char *area,*name; u32 klass; const u8 *d; u32 n; } TRec;
static void put32(u8 *p,u32 v){ p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
/* the format of tools/cutscene_pack.py write_pack(); `damage`: 1 checksum, 2 magic, 3 not an event, 4 unsorted, 5 class 7 */
static void write_pack(const char *path,const TRec *t,int n,int damage){
    u8 buf[4096]; memset(buf,0,sizeof buf); memcpy(buf,"KHCS",4); put32(buf+4,1); put32(buf+8,(u32)n);
    u32 off=(u32)((16+24*n+15)&~15);
    for(int i=0;i<n;i++){
        u8 *r=buf+16+24*i; memcpy(r,t[i].area,4); size_t l=strlen(t[i].name); memcpy(r+4,t[i].name,l);
        put32(r+8,damage==5&&i==0?7:t[i].klass); put32(r+12,off); put32(r+16,t[i].n); memcpy(buf+off,t[i].d,t[i].n);
        if(damage==3&&i==0) buf[off]=3;                                        /* a first command of 3 bytes */
        put32(r+20,fnv1a(buf+off,t[i].n)); off+=(t[i].n+15)&~15u;
    }
    if(damage==1) buf[off-1-((16-(t[n-1].n%16))%16)]^=0x5a;                    /* the last entry's last byte, after its checksum */
    if(damage==2) buf[0]='X';
    if(damage==4&&n>1){ u8 tmp[24]; memcpy(tmp,buf+16,24); memcpy(buf+16,buf+40,24); memcpy(buf+40,tmp,24); }
    FILE *f=fopen(path,"wb"); if(!f) kh_fatal(NULL,"cutscenes selftest: cannot write %s",path); fwrite(buf,1,off,f); fclose(f);
}
static int child(KCtx *g,u64 p,u32 nm,char *out,size_t cap){
    FILE *cf=kh_tmpfile(); if(!cf) kh_fatal(g,"cutscenes selftest: tmpfile failed");
    pid_t c=kh_fork(); if(c<0) kh_fatal(g,"cutscenes selftest: fork failed");
    if(!c){ if(dup2(fileno(cf),STDERR_FILENO)<0) _exit(98); kh_cutscenes_answer(g,nm,p); _exit(99); }
    int st=0; if(waitpid(c,&st,0)!=c){ fclose(cf); return -1; }
    rewind(cf); size_t n=fread(out,1,cap-1,cf); out[n]=0; fclose(cf); return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
static u32 NM(const char *s){ u32 v=0; memcpy(&v,s,strlen(s)<4?strlen(s):4); return v; }
/* tree check (the selftest given KHNMC_DATA with ard/us and KHNMC_CUTSCENES_PACK): every scene of the pack is, in this tree, the
 * mod's plain stub of that name in that area file (the first type-22 entry of the name, as 0x3a1f70 finds it) */
static int host_plain_stub(const u8 *d,size_t n){
    static const u16 want[5]={0x00,0x0f,0x10,0x08,0x12}; size_t p=0; int k=0;
    for(;;){ if(p+2>n) return 0; u16 len=(u16)(d[p]|d[p+1]<<8); if(!len) return k==5; if(len<4||k>=5||p+len>n||(u16)(d[p+2]|d[p+3]<<8)!=want[k]) return 0; k++; p+=len; }
}
static int tree_check(KCtx *g,const char *root,const char *packp){
    char dir[4096];
    if(!root||!*root||!packp||!*packp||!kh_resolve_ci(root,"ard/us",dir,sizeof dir)){
        fprintf(stderr,"kh: cutscenes tree check: not run: it needs KHNMC_DATA (a tree with ard/us) and KHNMC_CUTSCENES_PACK\n"); return 0; }
    setenv("KHNMC_CUTSCENES","story",1); setenv("KHNMC_CUTSCENES_PACK",packp,1); kh_cutscenes_test_reset(); cinit(g);
    int tc=0,tb=0;
    for(u32 i=0;i<nrec;i++){
        char rel[16],path[4200]; snprintf(rel,sizeof rel,"%.4s.ard",recs[i].area);
        if(!kh_resolve_ci(dir,rel,path,sizeof path)){ tc++; tb++; fprintf(stderr,"cutscenes tree selftest FAIL: %.4s/%.4s: no area file %s in %s\n",recs[i].area,recs[i].name,rel,dir); continue; }
        FILE *f=fopen(path,"rb"); if(!f){ tc++; tb++; fprintf(stderr,"cutscenes tree selftest FAIL: cannot open %s\n",path); continue; } fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        u8 *b=malloc((size_t)sz+1); if(!b||fread(b,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(b); kh_fatal(g,"cutscenes tree check: cannot read %s",path); } fclose(f);
        u32 cnt=sz>=16&&!memcmp(b,"BAR\1",4)?g32(b+4):0; int found=0, stub=0;
        for(u32 k=0;k<cnt&&16+16ull*(k+1)<=(u64)sz&&!found;k++){
            const u8 *e=b+16+16*k; char nm4[4]={0}; memcpy(nm4,e+4,4);
            if((e[0]|e[1]<<8)!=22||memcmp(nm4,recs[i].name,4)) continue;
            found=1; u32 off=g32(e+8), esz=g32(e+12); stub=(u64)off+esz<=(u64)sz&&host_plain_stub(b+off,esz);
        }
        free(b); tc++;
        if(!found||!stub){ tb++; if(tb<20) fprintf(stderr,"cutscenes tree selftest FAIL: %.4s/%.4s: %s\n",recs[i].area,recs[i].name,found?"the tree's entry is not the mod's plain stub":"no entry of that name"); }
    }
    fprintf(stderr,"kh: cutscenes tree check: %u scenes of %s: each is the mod's plain stub of that name in %s\n",nrec,packp,dir);
    fprintf(stderr,"kh: cutscenes tree selftest: %d checks, %d failures\n",tc,tb);
    kh_cutscenes_test_reset(); return tb;
}
int kh_cutscenes_selftest(KCtx *g){
    checks=bad=0;
    const char *keys[]={"KHNMC_DATA","KHNMC_CUTSCENES","KHNMC_CUTSCENES_PACK","KHNMC_EVENTS","KHNMC_CUTSCENES_SAVEDUMP"}; enum{NK=5}; char *old[NK];
    for(int i=0;i<NK;i++){ const char *v=getenv(keys[i]); old[i]=v?strdup(v):NULL; unsetenv(keys[i]); }
    int sv_mode=cmode,sv_dump=dump_on; u8 *sv_pack=pack; size_t sv_len=pack_len; PackRec *sv_recs=recs; u32 sv_n=nrec; u64 *sv_guest=guest; unsigned sv_dn=dump_n;
    u64 sv_c[8]={c_lookup,c_none,c_real,c_stub,c_restored,c_kept,c_noplace,c_dumps}; char sv_dir[4096]; memcpy(sv_dir,dump_dir,sizeof sv_dir);
    pack=NULL; recs=NULL; guest=NULL; nrec=0; kh_cutscenes_test_reset();
    u8 w0=rd8(g,PLACE_W), r0=rd8(g,PLACE_R);
    char dir[KH_TMP_MAX],p[KH_TMP_MAX+300]; kh_tmp_path(dir,sizeof dir,"khnmc-cutscenes-XXXXXX"); if(!mkdtemp(dir)) return 1;
    u8 stub[80],sa[200],sb[200],sc[200]; size_t ls=mk_stub(stub), la=mk_scene(sa,0x41), lb=mk_scene(sb,0x42), lc=mk_scene(sc,0x43);
    TRec tr[3]={{"al00","101",1,sa,(u32)la},{"tt05","406",1,sb,(u32)lb},{"tt05","40a",2,sc,(u32)lc}};       /* sorted: al00 < tt05; "406" < "40a" */
    snprintf(p,sizeof p,"%s/story-events.khcs",dir); write_pack(p,tr,3,0);
    u64 gs=ga_alloc(96,16), gr=ga_alloc(256,16), gm=ga_alloc(128,16); memcpy(hp(g,gs),stub,ls); memcpy(hp(g,gr),sa,la);
    { u8 near[120]; memcpy(near,stub,36); u8 v[8]={0,0,0,0,0,1,1,0}; size_t n=36+cmd(near+36,0x2e,v,8); memcpy(near+n,stub+36,ls-36); n+=ls-36; memcpy(hp(g,gm),near,n); }  /* eh03 703's shape */
    T(plain_stub(g,gs)); T(!plain_stub(g,gr)); T(!plain_stub(g,gm));
    { u64 gx=ga_alloc(96,16); memcpy(hp(g,gx),stub,ls); wr16(g,gx+ls-2-10+2,0x14); T(rd16(g,gx+ls-12)==10&&!plain_stub(g,gx)); ga_free(gx); }   /* five commands, the fifth not the fade: not the stub */ T(is_event(sa,la)); T(!is_event(sa,la-2)); T(!is_event(stub,10));
    wr8(g,PLACE_W,2); wr8(g,PLACE_R,5);                                          /* tt05 */
    /* default and `nmc`: the entry as it is, even with a pack path that does not exist (nothing is read) */
    setenv("KHNMC_CUTSCENES_PACK","/nonexistent/x.khcs",1);
    kh_cutscenes_test_reset(); T(kh_cutscenes_answer(g,NM("406"),gs)==gs); T(cmode==C_NMC&&!pack&&c_lookup==0);
    setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); T(kh_cutscenes_answer(g,NM("406"),gs)==gs); T(kh_cutscenes_answer(g,NM("406"),0)==0); T(!pack&&c_lookup==0);
    unsetenv("KHNMC_CUTSCENES_PACK");
    /* story: the pack from KHNMC_DATA */
    setenv("KHNMC_DATA",dir,1); setenv("KHNMC_CUTSCENES","story",1); kh_cutscenes_test_reset();
    u64 a=kh_cutscenes_answer(g,NM("406"),gs); T(a&&a!=gs&&nrec==3); T(a&&!memcmp(hp(g,a),sb,lb)); T(c_restored==1&&c_stub==1);
    T(kh_cutscenes_answer(g,NM("406"),gs)==a); T(c_restored==2);                  /* the same copy: allocated once */
    u64 c2=kh_cutscenes_answer(g,NM("40a"),gs); T(c2&&c2!=a&&!memcmp(hp(g,c2),sc,lc));
    T(kh_cutscenes_answer(g,NM("101"),gs)==gs); T(c_kept==1);                    /* al00/101 is in the pack, but the game is in tt05 */
    wr8(g,PLACE_W,7); wr8(g,PLACE_R,0); u64 d=kh_cutscenes_answer(g,NM("101"),gs); T(d!=gs&&!memcmp(hp(g,d),sa,la));   /* al00 */
    T(kh_cutscenes_answer(g,NM("406"),gs)==gs); T(c_kept==2);                    /* al00/406: not in the pack */
    T(kh_cutscenes_answer(g,NM("101"),gr)==gr); T(c_real==1);                    /* a scene the mod kept: passed through */
    T(kh_cutscenes_answer(g,NM("101"),gm)==gm); T(c_real==2);                    /* the stub with a volume command: not the plain stub */
    T(kh_cutscenes_answer(g,NM("101"),0)==0); T(c_none==1);
    wr8(g,PLACE_W,0xff); T(kh_cutscenes_answer(g,NM("406"),gs)==gs); T(c_noplace==1);
    T(!memcmp(hp(g,gs),stub,ls)&&!memcmp(hp(g,gr),sa,la));                       /* the game's entries are never written */
    { FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){ kh_cutscenes_report(rf); rewind(rf); char bb[600]; size_t n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf);
      T(strstr(bb,"mode story: 10 event lookups: 4 originals served")&&strstr(bb,"2 stubs kept (no original in the pack), 1 stubs kept (no room), 2 scenes the mod kept, 1 found no entry")); } }
    /* the save dump: one file per lookup, in nmc mode too, the entry untouched */
    snprintf(p,sizeof p,"%s/dump",dir); setenv("KHNMC_CUTSCENES_SAVEDUMP",p,1); setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); wr8(g,PLACE_W,2); wr8(g,PLACE_R,5);
    T(kh_cutscenes_answer(g,NM("406"),gs)==gs); T(c_dumps==1&&c_lookup==0&&c_restored==0);   /* nmc + dump: counted as a dump only */
    { char fp[KH_TMP_MAX+400]; snprintf(fp,sizeof fp,"%s/0000-tt05-406.bin",p); struct stat st; T(!stat(fp,&st)&&st.st_size==SAVE_LEN); unlink(fp); rmdir(p); }
    unsetenv("KHNMC_CUTSCENES_SAVEDUMP");
    /* the named fatals */
    if(!kh_fork_ok()) kh_selftest_not_run("cutscenes","the named fatals cutscenes-mode and cutscenes-pack (9 children)",18);
    else {
        char out[16384]; int rc;
        setenv("KHNMC_CUTSCENES","Story",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==3); T(strstr(out,"cutscenes-mode: KHNMC_CUTSCENES=Story is not a mode")!=NULL);
        setenv("KHNMC_CUTSCENES","story",1); setenv("KHNMC_EVENTS","stub",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==3); T(strstr(out,"cutscenes-mode: KHNMC_CUTSCENES=story and KHNMC_EVENTS=stub contradict")!=NULL);
        unsetenv("KHNMC_EVENTS");
        setenv("KHNMC_CUTSCENES_PACK","/nonexistent/x.khcs",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==3); T(strstr(out,"cutscenes-pack: /nonexistent/x.khcs: cannot open it")!=NULL);
        snprintf(p,sizeof p,"%s/bad.khcs",dir); setenv("KHNMC_CUTSCENES_PACK",p,1);
        static const char *want[6]={NULL,"fails its checksum","not a KHCS pack","is not an event entry","is not after the one before it","has class 7"};
        for(int k=1;k<=5;k++){ write_pack(p,tr,3,k); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==3); T(strstr(out,"cutscenes-pack:")&&strstr(out,want[k])); }
        unlink(p); unsetenv("KHNMC_CUTSCENES_PACK");
        setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==99); T(strstr(out,"FATAL")==NULL);   /* nmc: no fatal, no output */
    }
    wr8(g,PLACE_W,w0); wr8(g,PLACE_R,r0);
    kh_cutscenes_test_reset(); ga_free(gs); ga_free(gr); ga_free(gm);
    snprintf(p,sizeof p,"%s/story-events.khcs",dir); unlink(p); rmdir(dir);
    unsetenv("KHNMC_CUTSCENES"); unsetenv("KHNMC_EVENTS");
    int tbad=tree_check(g,old[0],old[2]);                                     /* the real tree and pack, when the selftest is given them */
    for(int i=0;i<NK;i++){ if(old[i]) setenv(keys[i],old[i],1); else unsetenv(keys[i]); free(old[i]); }
    cmode=sv_mode; dump_on=sv_dump; pack=sv_pack; pack_len=sv_len; recs=sv_recs; nrec=sv_n; guest=sv_guest; dump_n=sv_dn; memcpy(dump_dir,sv_dir,sizeof sv_dir);
    c_lookup=sv_c[0]; c_none=sv_c[1]; c_real=sv_c[2]; c_stub=sv_c[3]; c_restored=sv_c[4]; c_kept=sv_c[5]; c_noplace=sv_c[6]; c_dumps=sv_c[7];
    fprintf(stderr,"kh: cutscenes selftest: %d checks, %d failures\n",checks,bad); return bad+tbad;
}
