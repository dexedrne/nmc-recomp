/* Optional story lookup. KHCS v2 binds the room/name inventory, record table,
 * original bytes, asset paths and exact NMC templates to one reviewed digest. */
#include "khnmc_rt.h"
#include "warp.h"
#include "savestate.h"
#include "route_state.h"
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#define pack_sha256(bytes,n,out) CC_SHA256(bytes,(CC_LONG)(n),out)
#else
#include <openssl/sha.h>
#define pack_sha256 SHA256
#endif

/* f_2b3400 is the only caller. It uses AL to expose the Theater title row. */
static int theater_unlock_disabled;
void kh_theater_disable_unlock(void){ theater_unlock_disabled=1; }
void orig_f_2b73f0(KCtx *g);
void f_2b73f0(KCtx *g){
    orig_f_2b73f0(g);
    const char *unlock=getenv("KHNMC_THEATER_UNLOCK");
    if(!theater_unlock_disabled && unlock && !strcmp(unlock,"1")) SET_RAX(g,1);
}

/* The picker calls this predicate for chapter and scene progress words. */
void orig_f_2b9690(KCtx *g);
void f_2b9690(KCtx *g){
    orig_f_2b9690(g);
    const char *unlock=getenv("KHNMC_THEATER_UNLOCK"), *mode=getenv("KHNMC_CUTSCENES");
    if(!theater_unlock_disabled && unlock && !strcmp(unlock,"1") && mode && !strcmp(mode,"theater")) SET_RAX(g,1);
}

static const char reviewed_digest[]="155fa7e650a5ba0fed00bfc49555aea940446ca1a11b3645d5b314521ba90ef6";
static const char theater_digest[]="1420a4f2a81ec6ee7545a0fbd95b3bcfa2be12a64c968aface97c2507b9a9623";
static const char *expected_digest=reviewed_digest;
static unsigned expected_count=0;
static u32 catalog_offset;
static void digest_hex(const u8 *data,size_t size,char hex[65]){
    u8 digest[32]; pack_sha256(data,size,digest);
    for(unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",digest[i]);
}

#define PLACE_W    (GS_BASE+0x717008ull)
#define PLACE_R    (GS_BASE+0x717009ull)
#define SAVE_BASE  (GS_BASE+0x9a98b0ull)      /* GAME_SAVE: exe 0x152380 returns it */
#define SAVE_LEN   0x10fc0u
#define PACK_MAX   (64u<<20)
enum { C_UNSET=-1, C_NMC=0, C_STORY=1, C_THEATER=2 };
typedef struct { char area[4], name[4]; u32 klass, off, size, hash; } PackRec;
static int cmode=C_UNSET;
static u8 *pack; static size_t pack_len; static PackRec *recs; static u32 nrec; static u64 *guest; static char pack_path[4200];
static char dump_dir[4096]; static int dump_on=-1; static unsigned dump_n;
static pthread_mutex_t cm=PTHREAD_MUTEX_INITIALIZER;
static _Atomic u64 c_lookup, c_none, c_real, c_stub, c_restored, c_kept, c_noplace, c_dumps, c_missing;

static int awaiting_field, ended, last_menu=-2, last_mission=-2;
static FILE *live_log;
static void close_live_log(void){
    if(live_log){ fclose(live_log); live_log=NULL; }
    awaiting_field=ended=0;
}
static u32 g32(const u8 *p){ return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static u32 fnv1a(const u8 *p,size_t n){ u32 h=0x811c9dc5u; for(size_t i=0;i<n;i++){ h^=p[i]; h*=0x01000193u; } return h; }
/* an event entry: (u16 length incl. this header, u16 type) ..., a zero length ends it (exe 0x2c8f40) */
static int is_event(const u8 *d,size_t n){
    size_t p=0; int k=0;
    for(;;){ if(p+2>n) return 0; u16 len=(u16)(d[p]|d[p+1]<<8); if(!len) return k>0 && p+2==n; if(len<4||p+len>n) return 0; p+=len; k++; }
}
static int exact_stub(const u8 *data,u32 size){
    if(size==56) return !memcmp(data,pack+catalog_offset,56);
    if(size==68) return !memcmp(data,pack+catalog_offset+56,68);
    return 0;
}
/* 3a1f70 -> 3a6480 returns the first matching BAR record; +12 is its
 * declared size. A matching prefix of a longer entry must pass through. */
static u32 entry_size(KCtx *g,u32 name){
    u64 bar=rd64(g,GS_BASE+0x2a0f828ull);
    if(!GA_OK(bar)||!GA_OK(bar+15)) return 0;
    u32 count=rd32(g,bar+4);
    if(count>65536 || !GA_OK(bar+15+16ull*count)) return 0;
    for(u32 i=0;i<count;i++){
        u64 r=bar+16+16ull*i;
        if(rd16(g,r)==22 && rd32(g,r+4)==name) return rd32(g,r+12);
    }
    return 0;
}
static int key_cmp(const char *a,const char *an,const char *b,const char *bn){ int c=memcmp(a,b,4); return c?c:memcmp(an,bn,4); }
static void pack_fatal(KCtx *g,const char *why){ kh_fatal(g,"cutscenes-pack: %s: %s",pack_path,why); }
static int load_pack(KCtx *g,int mode){
    int theater=mode==C_THEATER;
    const char *name=theater?"theater-events.khtr":"story-events.khcs";
    const char *e=getenv(theater?"KHNMC_THEATER_PACK":"KHNMC_CUTSCENES_PACK");
    if(e&&*e) snprintf(pack_path,sizeof pack_path,"%s",e);
    else { const char *root=getenv("KHNMC_DATA"); if(!root||!*root) root="data";
           if(!kh_resolve_ci(root,name,pack_path,sizeof pack_path)) snprintf(pack_path,sizeof pack_path,"%s/%s",root,name); }
    FILE *f=fopen(pack_path,"rb"); if(!f){
        kh_warn("cutscenes-pack: %s: cannot open it (%s); cutscenes disabled",pack_path,strerror(errno));
        return 0;
    }
    struct stat st; if(fstat(fileno(f),&st)||st.st_size<16||(u64)st.st_size>PACK_MAX){ fclose(f); pack_fatal(g,"not a file of 16 bytes .. 64 MiB"); }
    pack_len=(size_t)st.st_size; pack=malloc(pack_len); if(!pack){ fclose(f); pack_fatal(g,"out of memory"); }
    if(fread(pack,1,pack_len,f)!=pack_len){ fclose(f); pack_fatal(g,"short read"); }
    fclose(f);
    if(memcmp(pack,theater?"KHTR":"KHCS",4)) pack_fatal(g,theater?"not a KHTR pack":"not a KHCS pack");
    if(g32(pack+4)!=2) pack_fatal(g,"version is not 2");
    nrec=g32(pack+8); if(16+24ull*nrec>pack_len) pack_fatal(g,"the record table does not fit the file");
    if(nrec!=(theater?612:expected_count)) pack_fatal(g,"reviewed entry count mismatch");
    catalog_offset=g32(pack+12);
    if(catalog_offset<16+24ull*nrec || (u64)catalog_offset+124+8>pack_len)
        pack_fatal(g,"incomplete reviewed catalog or stub templates");
    recs=calloc(nrec?nrec:1,sizeof *recs); guest=calloc(nrec?nrec:1,sizeof *guest); if(!recs||!guest) pack_fatal(g,"out of memory");
    for(u32 i=0;i<nrec;i++){
        const u8 *r=pack+16+24*i; PackRec *x=&recs[i]; memcpy(x->area,r,4); memcpy(x->name,r+4,4);
        x->klass=g32(r+8); x->off=g32(r+12); x->size=g32(r+16); x->hash=g32(r+20);
        char w[200];
        if(theater ? x->klass!=3 : (x->klass!=1&&x->klass!=2)){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) has class %u (1 SAFE, 2 SAFE-AFTER-EDIT)",i,x->area,x->name,x->klass); pack_fatal(g,w); }
        if(x->off%16||(u64)x->off+x->size>=catalog_offset||x->off<16+24ull*nrec){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) lies outside the data area",i,x->area,x->name); pack_fatal(g,w); }
        if(fnv1a(pack+x->off,x->size)!=x->hash){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) fails its checksum",i,x->area,x->name); pack_fatal(g,w); }
        if(!is_event(pack+x->off,x->size)){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) is not an event entry (commands to a zero terminator)",i,x->area,x->name); pack_fatal(g,w); }
        if(i&&key_cmp(recs[i-1].area,recs[i-1].name,x->area,x->name)>=0){ snprintf(w,sizeof w,"entry %u (%.4s/%.4s) is not after the one before it (the keys must be sorted and unique)",i,x->area,x->name); pack_fatal(g,w); }
    }
    char digest[65]; digest_hex(pack,pack_len,digest);
    if(strcmp(digest,theater?theater_digest:expected_digest)) pack_fatal(g,"reviewed manifest SHA-256 mismatch");
    fprintf(stderr,"kh: cutscenes: KHNMC_CUTSCENES=%s: %u original scenes from %s (%zu bytes) replace the mod's stub where the game asks for them\n",getenv("KHNMC_CUTSCENES"),nrec,pack_path,pack_len);
    return 1;
}
static void cinit(KCtx *g){
    const char *e=getenv("KHNMC_CUTSCENES"); int m;
    if(!e||!*e||!strcmp(e,"nmc")) m=C_NMC;
    else if(!strcmp(e,"story")) m=C_STORY;
    else if(!strcmp(e,"theater")) m=C_THEATER;
    else { kh_warn("cutscenes-mode: KHNMC_CUTSCENES=%s is not a mode; using nmc",e); m=C_NMC; }
    if(m!=C_NMC){ const char *ev=getenv("KHNMC_EVENTS");
        if(ev && (!strcmp(ev,"stub") || !strcmp(ev,"stub+music"))){
            kh_warn("cutscenes-mode: KHNMC_CUTSCENES=%s and KHNMC_EVENTS=%s contradict; cutscenes disabled",e,ev);
            m=C_NMC;
        }else if(m==C_STORY && !load_pack(g,m)) m=C_NMC;
    }
    const char *d=getenv("KHNMC_CUTSCENES_SAVEDUMP");
    dump_on=0;
    if(d&&*d){
        char path[4300];
        if(snprintf(dump_dir,sizeof dump_dir,"%s",d)<(int)sizeof dump_dir &&
           (!mkdir(dump_dir,0755)||errno==EEXIST)){
            snprintf(path,sizeof path,"%s/live.log",dump_dir);
            live_log=fopen(path,"w");
        }
        if(live_log) dump_on=1;
        else kh_warn("cutscenes save dump disabled: directory unavailable");
    }
    cmode=m;
}
int kh_theater_pack_init(KCtx *g){
    if(cmode==C_UNSET) cinit(g);
    if(cmode==C_THEATER && !pack && !load_pack(g,C_THEATER)) cmode=C_NMC;
    if(cmode!=C_THEATER) kh_theater_disable_unlock();
    return cmode==C_THEATER;
}
/* selftest only */
void kh_cutscenes_test_reset(void){
    close_live_log(); cmode=C_UNSET; dump_on=-1; dump_n=0; free(pack); pack=NULL; pack_len=0; free(recs); recs=NULL;
    if(guest){ for(u32 i=0;i<nrec;i++) if(guest[i]) ga_free(guest[i]); } free(guest); guest=NULL; nrec=0;
    c_lookup=c_none=c_real=c_stub=c_restored=c_kept=c_noplace=c_missing=c_dumps=0;
}
static void save_dump(KCtx *g,const char *area,u32 nm){
    if(!GA_OK(SAVE_BASE)||!GA_OK(SAVE_BASE+SAVE_LEN-1)) kh_fatal(g,"cutscenes-savedump: the save block 0x%llx..+0x%x is not guest memory",(unsigned long long)SAVE_BASE,SAVE_LEN);
    char nb[5]={0}; memcpy(nb,&nm,4); for(int i=0;i<4;i++) if(nb[i]&&(nb[i]<'0'||nb[i]>'z')) nb[i]='_';
    char p[4300]; snprintf(p,sizeof p,"%s/%04u-%s-%s.bin",dump_dir,dump_n++,area,nb[0]?nb:"none");
    FILE *f=fopen(p,"wb"); if(!f) kh_fatal(g,"cutscenes-savedump: cannot write %s (%s)",p,strerror(errno));
    if(fwrite(hp(g,SAVE_BASE),1,SAVE_LEN,f)!=SAVE_LEN){ fclose(f); kh_fatal(g,"cutscenes-savedump: short write to %s",p); }
    fclose(f); c_dumps++;
}
/* Log semantic arguments, never process-specific pointers. These hooks always
 * forward the original call, including when instrumentation is disabled. */
#define OBSERVE_CALL(rva,nargs) \
    void orig_f_##rva(KCtx *); void f_##rva(KCtx *g){ \
        if(awaiting_field){ fprintf(live_log,"call " #rva " %u",(unsigned)A0(g)); \
            if(nargs==2) fprintf(live_log," %u",(unsigned)A1(g)); fputc('\n',live_log); } \
        orig_f_##rva(g); }
OBSERVE_CALL(3aabe0,2)
OBSERVE_CALL(3ad5e0,1)
OBSERVE_CALL(3ad610,1)
OBSERVE_CALL(39afc0,1)
OBSERVE_CALL(2d9a30,1)

static void state_snapshot(KCtx *g,const char *stage){
    save_dump(g,stage,0);
    int menu=rd8(g,GS_BASE+0x9006b0)?(int)rd32(g,GS_BASE+0x900724):-1;
    u64 mission=rd64(g,GS_BASE+0x2a0ff68);
    u64 header=mission?rd64(g,mission+8):0;
    fprintf(live_log,"boundary menu %d mission %d\n",menu,header?(int)rd16(g,header+2):-1);
    /* Project-name exit flags and the 0x16 layer mask live outside GAME_SAVE. */
    fprintf(live_log,"event flags %016llx layers %016llx\n",
            (unsigned long long)rd64(g,GS_BASE+0x79cac0),
            (unsigned long long)rd64(g,GS_BASE+0xabcbd8));
    fputs(stage,live_log);
    for(int i=0;i<10;i++) fprintf(live_log," %u",rd8(g,PLACE_W+i));
    for(int i=0;i<2;i++){ u64 slot=GS_BASE+0xabad40+16*i;
        fprintf(live_log," slot%d %u %u %u",i,rd32(g,slot),rd32(g,slot+4),rd32(g,slot+8)); }
    fprintf(live_log," dual %u side %u\n",rd8(g,GS_BASE+0xabad18),rd8(g,GS_BASE+0xabad19));
    int values[3],targets[3],count=kh_route_mission_counters(g,values,targets);
    for(int i=0;i<count;i++) fprintf(live_log,"mission counter %d %d\n",values[i],targets[i]);
    char digest[65]; digest_hex(hp(g,GS_BASE+0x2a114a0),6*0x40,digest);
    fprintf(live_log,"pause records %s\n",digest);
    u64 o=rd64(g,GS_BASE+0x2a171c8);
    for(unsigned i=0;o;i++){
        if(i>=4096 || !GA_OK(o) || !GA_OK(o+0xa93)) kh_fatal(g,"cutscenes-savedump: invalid field object list");
        char model[40]; kh_route_object_name(g,o,model,sizeof model);
        fprintf(live_log,"object %s %#x flags120 %#x\n",model,rd32(g,o+0x124),rd32(g,o+0x120));
        u32 id=rd32(g,o+0xa90);
        o=id?(rd64(g,GS_BASE+0x2b0d720+8ull*((id&0x7fffffffu)>>25))|(id&0x1ffffffu)):0;
    }
    if(fflush(live_log)) kh_fatal(g,"cutscenes-savedump: live log write failed");
}
int kh_cutscenes_end_pending(KCtx *g){
    if(!awaiting_field || rd32(g,GS_BASE+0xb65210)==4) return 0;
    return rd8(g,GS_BASE+0xb64f9c)?2:1;
}
void kh_cutscenes_end_snapshot(KCtx *g,int reason){
    ended=1;
    state_snapshot(g,reason==2?"skip":"exit");
}
void kh_cutscenes_field_tick(KCtx *g,int field_ran){
    if(!awaiting_field) return;
    int menu=rd8(g,GS_BASE+0x9006b0)?(int)rd32(g,GS_BASE+0x900724):-1;
    u64 mission=rd64(g,GS_BASE+0x2a0ff68), header=mission?rd64(g,mission+8):0;
    int mission_id=header?rd16(g,header+2):-1;
    if(menu!=last_menu || mission_id!=last_mission){
        fprintf(live_log,"menu %d mission %d\n",menu,mission_id);
        last_menu=menu; last_mission=mission_id;
    }
    if(!ended || !field_ran || menu>=0 || rd8(g,PLACE_W)==0xff || rd8(g,GS_BASE+0x9ba8d1) || rd64(g,GS_BASE+0x9ba928)) return;
    awaiting_field=0;
    state_snapshot(g,"field");
}
u64 kh_input_game_frames(void);
/* the event starter's lookup of entry `nm` found p (0 = none) -> the entry to hand to the game */
static u64 answer(KCtx *g,u32 nm,u64 p,u32 size){
    if(cmode==C_UNSET){ pthread_mutex_lock(&cm); if(cmode==C_UNSET) cinit(g); pthread_mutex_unlock(&cm); }
    if(cmode==C_NMC&&!dump_on) return p;                                  /* the default: nothing read, nothing written */
    u8 w=rd8(g,PLACE_W), r=rd8(g,PLACE_R); char area[5]={0};
    if(w<KH_WARP_WORLDS&&r<100) snprintf(area,sizeof area,"%s%02u",kh_warp_world_names[w],(unsigned)r);
    if(dump_on){
        save_dump(g,area[0]?area:"none",nm);
        if(p){ awaiting_field=1; ended=0; last_menu=last_mission=-2;
            fprintf(live_log,"lookup %s %.4s\n",area,(const char *)&nm); }
    }
    if(cmode==C_NMC || (cmode==C_THEATER && (!kh_theater_active() || !(rd8(g,GS_BASE+0xbedf10)&2)))) return p;
    c_lookup++;
    const char *nb=(const char*)&nm; unsigned long long gf=(unsigned long long)kh_input_game_frames();
    if(!p){ c_none++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: no entry of that name\n",gf,area[0]?area:"?",nb); return p; }
    if(size==UINT32_MAX) size=entry_size(g,nm);
    if((size!=56 && size!=68) || !GA_OK(p) || !GA_OK(p+size-1) || !exact_stub(hp(g,p),size)){ c_real++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: a scene the mod kept (passed through)\n",gf,area[0]?area:"?",nb); return p; }
    if(size==68 && cmode==C_STORY){ c_kept++; c_stub++; fprintf(stderr,"kh: cutscenes: %s/%.4s retains NMC volume stub\n",area,nb); return p; }   /* the two eh03 entries keep their volume command */
    c_stub++;
    if(!area[0]){ c_noplace++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %02x:%02x event %.4s: the mod's stub, and the place is no room (kept)\n",gf,w,r,nb); return p; }
    char key[4]={0}; for(int i=0;i<4;i++) key[i]=nb[i];
    u32 lo=0,hi=nrec;
    while(lo<hi){ u32 mid=(lo+hi)/2; int c=key_cmp(recs[mid].area,recs[mid].name,area,key); if(!c){ lo=mid; hi=mid+1; break; } if(c<0) lo=mid+1; else hi=mid; }
    if(lo>=nrec||key_cmp(recs[lo].area,recs[lo].name,area,key)){ c_kept++; fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: the mod's stub, no original in the pack (kept)\n",gf,area,nb); return p; }
    const char *asset=(const char *)pack+recs[lo].off+recs[lo].size;
    while(*asset){
        char group[2048];
        if(strlen(asset)>=sizeof group) pack_fatal(g,"asset path group too long");
        strcpy(group,asset);
        char *rest=group,*path; int found=0;
        while((path=strsep(&rest,"|"))!=NULL){
            extern int kh_cutscene_asset_exists(KCtx *,const char *);
            if(kh_cutscene_asset_exists(g,path)){ found=1; break; }
        }
        if(!found){
            c_missing++;
            fprintf(stderr,"kh: cutscenes-asset: %s/%.4s missing %s (kept stub)\n",area,nb,asset);
            return p;
        }
        asset+=strlen(asset)+1;
    }
    pthread_mutex_lock(&cm);
    if(!guest[lo]){ u64 b=ga_alloc(recs[lo].size+16,16); if(!b){ pthread_mutex_unlock(&cm); kh_fatal(g,"cutscenes: no guest memory for the %u-byte scene %s/%.4s",recs[lo].size,area,nb); }
                    memcpy(hp(g,b),pack+recs[lo].off,recs[lo].size); memset(hp(g,b)+recs[lo].size,0,16); guest[lo]=b; }
    u64 q=guest[lo]; pthread_mutex_unlock(&cm);
    c_restored++;
    fprintf(stderr,"kh: cutscenes: game-frame=%llu map %s event %.4s: the original scene (%u bytes, %s) in place of the mod's stub\n",gf,area,nb,recs[lo].size,cmode==C_THEATER?"THEATER":recs[lo].klass==1?"SAFE":"SAFE-AFTER-EDIT");
    return q;
}
u64 kh_cutscenes_answer(KCtx *g,u32 nm,u64 p){ return answer(g,nm,p,UINT32_MAX); }
void kh_cutscenes_report(FILE *f){
    if(cmode==C_STORY||cmode==C_THEATER||dump_on==1)
        fprintf(f,"kh: cutscenes: mode %s: %llu event lookups: %llu originals served in place of the mod's stub, %llu stubs kept (no original in the pack), %llu stubs kept (no room), %llu scenes the mod kept, %llu found no entry; %llu save dumps; %llu missing-asset fallbacks\n",
                cmode==C_THEATER?"theater":cmode==C_STORY?"story":"nmc",(unsigned long long)c_lookup,(unsigned long long)c_restored,(unsigned long long)c_kept,(unsigned long long)c_noplace,(unsigned long long)c_real,(unsigned long long)c_none,(unsigned long long)c_dumps,(unsigned long long)c_missing);
}

/* ---- selftest: a synthetic pack in a temp tree, synthetic entries in guest memory; the fatal cases in forked children -------- */
static int checks,bad;
#define T(c) do{ checks++; if(!(c)){ bad++; fprintf(stderr,"cutscenes selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static size_t cmd(u8 *o,u16 type,const u8 *pl,u16 n){ u16 len=(u16)(n+4); o[0]=(u8)len; o[1]=(u8)(len>>8); o[2]=(u8)type; o[3]=(u8)(type>>8); if(n) memcpy(o+4,pl,n); return len; }
static size_t mk_stub(u8 *o){ u8 pj[20]={0}; size_t n=cmd(o,0,pj,20); n+=cmd(o+n,0x0f,NULL,0); u8 st[4]={0}; n+=cmd(o+n,0x10,st,4); u8 en[4]={50,0,0x12,0}; n+=cmd(o+n,0x08,en,4);
                              u8 fd[6]={1,0,0,0,1,0}; n+=cmd(o+n,0x12,fd,6); o[n]=0; o[n+1]=0; return n+2; }
static size_t mk_scene(u8 *out,u8 end_frame){
    u8 end[4]={end_frame,0,0x12,0},fade[6]={1,0,0,0,1,0};
    size_t n=cmd(out,0x0f,NULL,0);
    n+=cmd(out+n,0x08,end,sizeof end);
    n+=cmd(out+n,0x12,fade,sizeof fade);
    out[n]=out[n+1]=0; return n+2;
}
typedef struct { const char *area,*name; u32 klass; const u8 *d; u32 n; } TRec;
static void put32(u8 *p,u32 v){ p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
/* the format of tools/cutscene_pack.py write_pack(); `damage`: 1 checksum, 2 magic, 3 not an event, 4 unsorted, 5 class 7 */
static void write_pack(const char *path,const TRec *t,int n,int damage){
    u8 buf[4096]; memset(buf,0,sizeof buf); memcpy(buf,"KHCS",4); put32(buf+4,2); put32(buf+8,(u32)n);
    u32 off=(u32)((16+24*n+15)&~15);
    for(int i=0;i<n;i++){
        u8 *r=buf+16+24*i; memcpy(r,t[i].area,4); size_t l=strlen(t[i].name); memcpy(r+4,t[i].name,l);
        put32(r+8,damage==5&&i==0?7:t[i].klass); put32(r+12,off); put32(r+16,t[i].n); memcpy(buf+off,t[i].d,t[i].n);
        if(damage==3&&i==0) buf[off]=3;                                        /* a first command of 3 bytes */
        put32(r+20,fnv1a(buf+off,t[i].n)); if(i==0) strcpy((char *)buf+off+t[i].n,"required.map");
        off+=(t[i].n+(i==0?14:1)+15)&~15u;
    }
    if(damage==1) buf[g32(buf+16+12)]^=0x5a;                    /* the last entry's last byte, after its checksum */
    if(damage==2) buf[0]='X';
    if(damage==4&&n>1){ u8 tmp[24]; memcpy(tmp,buf+16,24); memcpy(buf+16,buf+40,24); memcpy(buf+40,tmp,24); }
    put32(buf+12,off);
    u8 st[80]; mk_stub(st); memcpy(buf+off,st,56);
    memcpy(buf+off+56,st,36); memcpy(buf+off+56+48,st+36,20);
    off+=124; memcpy(buf+off,"al00101",8); off+=8;
    if(damage==6) buf[16+5]='2';
    if(damage==7) put32(buf+8,0);
    FILE *f=fopen(path,"wb"); if(!f) kh_fatal(NULL,"cutscenes selftest: cannot write %s",path); fwrite(buf,1,off,f); fclose(f);
}
static int child(KCtx *g,u64 p,u32 nm,char *out,size_t cap){
    FILE *cf=kh_tmpfile(); if(!cf) kh_fatal(g,"cutscenes selftest: tmpfile failed");
    pid_t c=kh_fork(); if(c<0) kh_fatal(g,"cutscenes selftest: fork failed");
    if(!c){ if(dup2(fileno(cf),STDERR_FILENO)<0) _exit(98); answer(g,nm,p,56); _exit(99); }
    int st=0; if(waitpid(c,&st,0)!=c){ fclose(cf); return -1; }
    rewind(cf); size_t n=fread(out,1,cap-1,cf); out[n]=0; fclose(cf); return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
static u32 NM(const char *s){ u32 v=0; memcpy(&v,s,strlen(s)<4?strlen(s):4); return v; }
/* tree check (the selftest given KHNMC_DATA with ard/us and KHNMC_CUTSCENES_PACK): every scene of the pack is, in this tree, the
 * mod's plain stub of that name in that area file (the first type-22 entry of the name, as 0x3a1f70 finds it) */
static int tree_check(KCtx *g,const char *root,const char *packp){
    char dir[4096];
    if(!root||!*root||!packp||!*packp||!kh_resolve_ci(root,"ard/us",dir,sizeof dir)){
        fprintf(stderr,"kh: cutscenes tree check: not run: it needs KHNMC_DATA (a tree with ard/us) and KHNMC_CUTSCENES_PACK\n"); return 0; }
    setenv("KHNMC_CUTSCENES","story",1); setenv("KHNMC_CUTSCENES_PACK",packp,1); kh_cutscenes_test_reset(); cinit(g);
    if(!pack) return 1;
    int tc=0,tb=0;
    unsigned catalog_count=(pack_len-catalog_offset-124)/8;
    for(unsigned i=0;i<catalog_count;i++){
        const char *area=(const char *)pack+catalog_offset+124+8*i,*name=area+4;
        char rel[16],path[4200]; snprintf(rel,sizeof rel,"%.4s.ard",area);
        if(!kh_resolve_ci(dir,rel,path,sizeof path)){ tc++; tb++; fprintf(stderr,"cutscenes tree selftest FAIL: %.4s/%.4s: no area file %s in %s\n",area,name,rel,dir); continue; }
        FILE *f=fopen(path,"rb"); if(!f){ tc++; tb++; fprintf(stderr,"cutscenes tree selftest FAIL: cannot open %s\n",path); continue; } fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        u8 *b=malloc((size_t)sz+1); if(!b||fread(b,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(b); kh_fatal(g,"cutscenes tree check: cannot read %s",path); } fclose(f);
        u32 cnt=sz>=16&&!memcmp(b,"BAR\1",4)?g32(b+4):0; int found=0, stub=0;
        for(u32 k=0;k<cnt&&16+16ull*(k+1)<=(u64)sz&&!found;k++){
            const u8 *e=b+16+16*k; char nm4[4]={0}; memcpy(nm4,e+4,4);
            if((e[0]|e[1]<<8)!=22||memcmp(nm4,name,4)) continue;
            found=1; u32 off=g32(e+8), esz=g32(e+12); stub=(u64)off+esz<=(u64)sz&&exact_stub(b+off,esz);
        }
        free(b); tc++;
        if(!found||!stub){ tb++; if(tb<20) fprintf(stderr,"cutscenes tree selftest FAIL: %.4s/%.4s: %s\n",area,name,found?"the tree's entry is not the mod's exact stub":"no entry of that name"); }
    }
    fprintf(stderr,"kh: cutscenes tree check: %u catalog keys of %s: each is the mod's exact stub of that name in %s\n",catalog_count,packp,dir);
    fprintf(stderr,"kh: cutscenes tree selftest: %d checks, %d failures\n",tc,tb);
    kh_cutscenes_test_reset(); return tb;
}
int kh_cutscenes_selftest(KCtx *g){
    checks=bad=0;
    int old_unlock_disabled=theater_unlock_disabled; theater_unlock_disabled=0;
    const char *keys[]={"KHNMC_DATA","KHNMC_CUTSCENES","KHNMC_CUTSCENES_PACK","KHNMC_EVENTS","KHNMC_CUTSCENES_SAVEDUMP","KHNMC_THEATER_UNLOCK"}; enum{NK=6}; char *old[NK];
    for(int i=0;i<NK;i++){ const char *v=getenv(keys[i]); old[i]=v?strdup(v):NULL; unsetenv(keys[i]); }
    u64 gate=GS_BASE+0xb1d778;
    u8 prior=rd8(g,gate);
    wr8(g,gate,0);
    T(kh_call(g,GS_BASE+0x2b73f0,0,0,0,0)==0);
    setenv("KHNMC_THEATER_UNLOCK","1",1);
    T(kh_call(g,GS_BASE+0x2b73f0,0,0,0,0)==1 && rd8(g,gate)==0);
    unsetenv("KHNMC_THEATER_UNLOCK");
    wr8(g,gate,1);
    T(kh_call(g,GS_BASE+0x2b73f0,0,0,0,0)==1);
    wr8(g,gate,prior);
    u64 row=ga_calloc(1,12);
    wr16(g,row+10,0xffff); /* f_39a980 always returns false for this flag. */
    setenv("KHNMC_CUTSCENES","theater",1);
    T(!(kh_call(g,GS_BASE+0x2b9690,row,0,0,0)&255));
    setenv("KHNMC_THEATER_UNLOCK","1",1);
    T((kh_call(g,GS_BASE+0x2b9690,row,0,0,0)&255)==1 && rd16(g,row+10)==0xffff);
    unsetenv("KHNMC_CUTSCENES");
    T(!(kh_call(g,GS_BASE+0x2b9690,row,0,0,0)&255));
    unsetenv("KHNMC_THEATER_UNLOCK");
    ga_free(row);
    u32 old_catalog=catalog_offset; u64 old_missing=c_missing;
    FILE *old_live=live_log; live_log=NULL;
    int old_await=awaiting_field,old_ended=ended,old_menu=last_menu,old_mission=last_mission;
    int sv_mode=cmode,sv_dump=dump_on; u8 *sv_pack=pack; size_t sv_len=pack_len; PackRec *sv_recs=recs; u32 sv_n=nrec; u64 *sv_guest=guest; unsigned sv_dn=dump_n;
    u64 sv_c[8]={c_lookup,c_none,c_real,c_stub,c_restored,c_kept,c_noplace,c_dumps}; char sv_dir[4096]; memcpy(sv_dir,dump_dir,sizeof sv_dir);
    pack=NULL; recs=NULL; guest=NULL; nrec=0; kh_cutscenes_test_reset();
    u8 w0=rd8(g,PLACE_W), r0=rd8(g,PLACE_R);
    char dir[KH_TMP_MAX],p[KH_TMP_MAX+300]; kh_tmp_path(dir,sizeof dir,"khnmc-cutscenes-XXXXXX"); if(!mkdtemp(dir)) return 1;
    u8 stub[80],sa[200],sb[200],sc[200]; size_t ls=mk_stub(stub), la=mk_scene(sa,0x41), lb=mk_scene(sb,0x42), lc=mk_scene(sc,0x43);
    TRec tr[3]={{"al00","101",1,sa,(u32)la},{"tt05","406",1,sb,(u32)lb},{"tt05","40a",2,sc,(u32)lc}};       /* sorted: al00 < tt05; "406" < "40a" */
    snprintf(p,sizeof p,"%s/story-events.khcs",dir); write_pack(p,tr,3,0);
    char test_digest[65]; u8 raw[4096]; FILE *tf=fopen(p,"rb"); if(!tf) return 1;
    size_t got=fread(raw,1,sizeof raw,tf); fclose(tf); digest_hex(raw,got,test_digest);
    expected_count=3; expected_digest=test_digest;
    u64 gs=ga_alloc(96,16), gr=ga_alloc(256,16), gm=ga_alloc(128,16); memcpy(hp(g,gs),stub,ls); memcpy(hp(g,gr),sa,la);
    { u8 near[120]; memcpy(near,stub,36); u8 v[8]={0,0,0,0,0,1,1,0}; size_t n=36+cmd(near+36,0x2e,v,8); memcpy(near+n,stub+36,ls-36); n+=ls-36; memcpy(hp(g,gm),near,n); }  /* six commands, with altered music payload */
    T(is_event(sa,la)); T(!is_event(sa,la-2)); T(!is_event(stub,10));
    wr8(g,PLACE_W,2); wr8(g,PLACE_R,5);                                          /* tt05 */
    /* default and `nmc`: the entry as it is, even with a pack path that does not exist (nothing is read) */
    setenv("KHNMC_CUTSCENES_PACK","/nonexistent/x.khcs",1);
    kh_cutscenes_test_reset(); T(answer(g,NM("406"),gs,56)==gs); T(cmode==C_NMC&&!pack&&c_lookup==0);
    setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); T(answer(g,NM("406"),gs,56)==gs); T(answer(g,NM("406"),0,0)==0); T(!pack&&c_lookup==0);
    unsetenv("KHNMC_CUTSCENES_PACK");
    /* A matching original and even the native replay bit need a private slot. */
    setenv("KHNMC_DATA",dir,1); setenv("KHNMC_CUTSCENES","theater",1); kh_cutscenes_test_reset();
    u64 replay=GS_BASE+0xbedf10;
    u8 old_replay=rd8(g,replay);
    wr8(g,replay,old_replay|2);
    T(answer(g,NM("406"),gs,56)==gs && c_lookup==0 && c_restored==0);
    wr8(g,replay,old_replay);
    /* story: the pack from KHNMC_DATA */
    setenv("KHNMC_DATA",dir,1); setenv("KHNMC_CUTSCENES","story",1); kh_cutscenes_test_reset();
    u64 a=answer(g,NM("406"),gs,56); T(a&&a!=gs&&nrec==3); T(a&&!memcmp(hp(g,a),sb,lb)); T(c_restored==1&&c_stub==1);
    T(answer(g,NM("406"),gs,56)==a); T(c_restored==2);                  /* the same copy: allocated once */
    u64 c2=answer(g,NM("40a"),gs,56); T(c2&&c2!=a&&!memcmp(hp(g,c2),sc,lc));
    T(answer(g,NM("101"),gs,56)==gs); T(c_kept==1);                    /* al00/101 is in the pack, but the game is in tt05 */
    T(!exact_stub(stub,55)&&!exact_stub(stub,57));
    stub[10]^=1; T(!exact_stub(stub,56)); stub[10]^=1;
    T(exact_stub(pack+catalog_offset+56,68));
    char required[KH_TMP_MAX+40]; snprintf(required,sizeof required,"%s/required.map",dir);
    wr8(g,PLACE_W,7); wr8(g,PLACE_R,0); T(answer(g,NM("101"),gs,56)==gs && c_missing==1);
    tf=fopen(required,"wb"); if(!tf) return 1; fputc(1,tf); fclose(tf);
    wr8(g,PLACE_W,7); wr8(g,PLACE_R,0); u64 d=answer(g,NM("101"),gs,56); T(d!=gs&&!memcmp(hp(g,d),sa,la));   /* al00 */
    T(answer(g,NM("406"),gs,56)==gs); T(c_kept==2);                    /* al00/406: not in the pack */
    T(answer(g,NM("101"),gr,la)==gr); T(c_real==1);                    /* a scene the mod kept: passed through */
    T(answer(g,NM("101"),gm,68)==gm); T(c_real==2);                    /* the stub with a volume command: not the plain stub */
    T(answer(g,NM("101"),0,0)==0); T(c_none==1);
    wr8(g,PLACE_W,0xff); T(answer(g,NM("406"),gs,56)==gs); T(c_noplace==1);
    T(!memcmp(hp(g,gs),stub,ls)&&!memcmp(hp(g,gr),sa,la));                       /* the game's entries are never written */
    { FILE *rf=kh_tmpfile(); T(rf!=NULL); if(rf){ kh_cutscenes_report(rf); rewind(rf); char bb[600]; size_t n=fread(bb,1,sizeof bb-1,rf); bb[n]=0; fclose(rf);
      T(strstr(bb,"mode story: 11 event lookups: 4 originals served")&&strstr(bb,"2 stubs kept (no original in the pack), 1 stubs kept (no room), 2 scenes the mod kept, 1 found no entry")); } }
    /* Exercise the real guest entry point, the relocated BAR size and the
     * returned scene bytes. Removing the hook must break this assertion. */
    u64 bar=ga_calloc(1,48), slot=GS_BASE+0x2a0f828ull, oldbar=rd64(g,slot);
    wr32(g,bar+4,1); wr16(g,bar+16,22); wr32(g,bar+20,NM("406"));
    u64 slot63=GS_BASE+0x2b0d720ull+8*63, saved63=rd64(g,slot63);
    u32 guard=rd32(g,slot63+8); wr32(g,slot63+8,0x80000000u);
    wr64(g,slot63,gs&~0x1ffffffull); wr32(g,bar+24,0xfe000000u | (gs&0x1ffffffu)); wr32(g,bar+28,56);
    wr64(g,slot,bar); wr8(g,PLACE_W,2); wr8(g,PLACE_R,5);
    T(kh_call(g,GS_BASE+0x3a1f70,NM("406"),0,0,0)==a);
    wr32(g,bar+28,57); T(kh_call(g,GS_BASE+0x3a1f70,NM("406"),0,0,0)==gs);
    wr32(g,bar+28,56); wr8(g,gs+10,1); T(kh_call(g,GS_BASE+0x3a1f70,NM("406"),0,0,0)==gs);
    wr8(g,gs+10,stub[10]); wr64(g,slot,oldbar); wr64(g,slot63,saved63); wr32(g,slot63+8,guard); ga_free(bar);
    unlink(required);
    wr8(g,PLACE_W,7); wr8(g,PLACE_R,0); T(answer(g,NM("101"),gs,56)==gs && c_missing==2);
    wr8(g,PLACE_W,11); wr8(g,PLACE_R,1); T(answer(g,NM("902"),gs,56)==gs);
    /* the save dump: one file per lookup, in nmc mode too, the entry untouched */
    snprintf(p,sizeof p,"%s/dump",dir); setenv("KHNMC_CUTSCENES_SAVEDUMP",p,1); setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); wr8(g,PLACE_W,2); wr8(g,PLACE_R,5);
    T(answer(g,NM("406"),gs,56)==gs); T(c_dumps==1&&c_lookup==0&&c_restored==0);   /* nmc + dump: counted as a dump only */
    { char fp[KH_TMP_MAX+400]; snprintf(fp,sizeof fp,"%s/0000-tt05-406.bin",p); struct stat st; T(!stat(fp,&st)&&st.st_size==SAVE_LEN); unlink(fp); snprintf(fp,sizeof fp,"%s/live.log",p); unlink(fp); rmdir(p); }
    unsetenv("KHNMC_CUTSCENES_SAVEDUMP");
    /* the named fatals */
    if(!kh_fork_ok()) kh_selftest_not_run("cutscenes","the named fatals cutscenes-mode and cutscenes-pack (11 children)",22);
    else {
        char out[16384]; int rc;
        setenv("KHNMC_CUTSCENES","Story",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==99); T(strstr(out,"cutscenes-mode: KHNMC_CUTSCENES=Story is not a mode")!=NULL);
        setenv("KHNMC_CUTSCENES","story",1); setenv("KHNMC_EVENTS","stub",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==99); T(strstr(out,"cutscenes-mode: KHNMC_CUTSCENES=story and KHNMC_EVENTS=stub contradict")!=NULL);
        unsetenv("KHNMC_EVENTS");
        setenv("KHNMC_CUTSCENES_PACK","/nonexistent/x.khcs",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==99); T(strstr(out,"cutscenes-pack: /nonexistent/x.khcs: cannot open it")!=NULL);
        snprintf(p,sizeof p,"%s/bad.khcs",dir); setenv("KHNMC_CUTSCENES_PACK",p,1);
        static const char *want[8]={NULL,"fails its checksum","not a KHCS pack","is not an event entry","is not after the one before it","has class 7","manifest SHA-256 mismatch","entry count mismatch"};
        for(int k=1;k<=7;k++){ write_pack(p,tr,3,k); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==3); T(strstr(out,"cutscenes-pack:")&&strstr(out,want[k])); }
        unlink(p); unsetenv("KHNMC_CUTSCENES_PACK");
        setenv("KHNMC_CUTSCENES","nmc",1); kh_cutscenes_test_reset(); rc=child(g,gs,NM("406"),out,sizeof out); T(rc==99); T(strstr(out,"FATAL")==NULL);   /* nmc: no fatal, no output */
    }
    wr8(g,PLACE_W,w0); wr8(g,PLACE_R,r0);
    kh_cutscenes_test_reset(); ga_free(gs); ga_free(gr); ga_free(gm);
    snprintf(p,sizeof p,"%s/story-events.khcs",dir); unlink(p); rmdir(dir);
    unsetenv("KHNMC_CUTSCENES"); unsetenv("KHNMC_EVENTS");
    expected_count=0; expected_digest=reviewed_digest; unlink(required); rmdir(dir);
    int tbad=tree_check(g,old[0],old[2]);                                     /* the real tree and pack, when the selftest is given them */
    for(int i=0;i<NK;i++){ if(old[i]) setenv(keys[i],old[i],1); else unsetenv(keys[i]); free(old[i]); }
    live_log=old_live; awaiting_field=old_await; ended=old_ended; last_menu=old_menu; last_mission=old_mission;
    catalog_offset=old_catalog; c_missing=old_missing;
    cmode=sv_mode; dump_on=sv_dump; pack=sv_pack; pack_len=sv_len; recs=sv_recs; nrec=sv_n; guest=sv_guest; dump_n=sv_dn; memcpy(dump_dir,sv_dir,sizeof sv_dir);
    c_lookup=sv_c[0]; c_none=sv_c[1]; c_real=sv_c[2]; c_stub=sv_c[3]; c_restored=sv_c[4]; c_kept=sv_c[5]; c_noplace=sv_c[6]; c_dumps=sv_c[7];
    theater_unlock_disabled=old_unlock_disabled;
    fprintf(stderr,"kh: cutscenes selftest: %d checks, %d failures\n",checks,bad); return bad+tbad;
}

#include "rewind.h"
void kh_rewind_cutscenes(KhRecords *r){ if(guest) kh_record(r,guest,nrec*sizeof *guest); }
