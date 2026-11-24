#include "xion_swap.h"
#include "kh_look.h"
#include "rewind.h"
#include "autosave.h"
#include "slot_hash.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <zlib.h>

/* The card directory compacts on deletion. Use its logical filename, never its array index. */
enum { MAX_SLOTS=100, SAVE_SIZE=0x10fc0 };
/* The khXb chunk (version 2, 9,701 bytes; the size, the version byte and the CRC are unchanged by the Axel extension):
 *     +0      u8  version = 2
 *     +1      100 rows of 96 bytes: char name[64] (the slot's logical file name, NUL-padded), u8 digest[32] (SHA-256 of the slot)
 *     +9601   u8  mode[100]: 0 = no record, else the saved difficulty the tag was written with (2 Critical, 3 Less Than Nothing;
 *                 1 = a version-1 record); every build rejects a chunk with a mode above 3
 * Before the extension name[63] was always 0 (record() clears the row and copies a name of at most 63 characters).
 * After it, in a row whose name is shorter than 63 characters, name[63] is the campaign VARIANT:
 *     0 = the Xion modes, exactly as before (mode 2 Xion Knows Best, mode 3 Xion Always Cries)
 *     1 = Axel Won't Cry, only with mode 2 (it is Critical)
 * An older build validates such a chunk as before (the name still has its NUL, the mode is 2), keeps every other slot's tag,
 * and its whole-row memcmp no longer matches the Axel slot: that slot reads as an untagged save, i.e. plain Critical with
 * Roxas, and the row is carried through its rewrites untouched (it rewrites the row only when that very slot is saved or
 * deleted, by name).  A mode byte of 4 would instead make the older build drop the whole chunk: every Xion slot of the card
 * would lose its tag on its next write.
 * In this file a tag is the mode byte plus TAG_AXEL for variant 1: 0, 1..3, or TAG_AXEL|2. */
enum { TAG_AXEL=KH_SAVE_TAG_AXEL, VARIANT_AT=63 };
typedef struct { char name[64]; u8 digest[32]; } Record;
typedef struct { u8 version; Record rows[MAX_SLOTS]; u8 mode[MAX_SLOTS]; } Chunk;
static Record change,source;
static int tagged,source_tag;
static u64 change_card;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static void forget_lazy_label(void);   /* defined with difficulty_text below */

void kh_rewind_xkb_save(KhRecords *r){
    pthread_mutex_lock(&lock);
    KH_RECORD(r,change_card);
    KH_VALUE(r,change); KH_VALUE(r,source); KH_VALUE(r,tagged); KH_VALUE(r,source_tag);
    /* The seventh difficulty label is allocated in the guest arena on first use.  A snapshot taken before that allocation restores
     * an allocator state without it, so the host pointer would be kept over a block the snapshot freed (review round 9, item 4):
     * drop it on load and let difficulty_label encode it again in the restored arena. */
    if(r->loading) forget_lazy_label();
    pthread_mutex_unlock(&lock);
}

static void digest(Record *r,const void *data,size_t n){
    SlotHash hash; slot_hash_init(&hash); slot_hash_add(&hash,data,n); slot_hash_end(r->digest,&hash);
}
static const char *logical_name(const char *path){
    const char *name=strrchr(path,'/');
    return name?name+1:path;
}
static int record(Record *r,const char *path,const void *bytes,size_t n){
    const char *name=logical_name(path);
    if(!*name||strlen(name)>=sizeof r->name) return 0;
    memset(r,0,sizeof *r); strcpy(r->name,name); digest(r,bytes,n); return 1;
}
/* 0 for a 63-character name: its last byte is the name's own terminator. */
static int row_variant(const Record *r){ return r->name[VARIANT_AT-1]?0:(u8)r->name[VARIANT_AT]; }
static int valid_tag(int tag){ return tag>=0 && (tag<=3 || tag==(TAG_AXEL|2)); }
static void change_record(Chunk *c,const Record *r,int add){
    for(int i=0;i<MAX_SLOTS;i++) if(!strcmp(c->rows[i].name,r->name)) {memset(&c->rows[i],0,sizeof *r); c->mode[i]=0;}
    if(add){
        if(!valid_tag(add)) kh_fatal(NULL,"xkb-save-mode: %d",add);
        if((add&TAG_AXEL) && r->name[VARIANT_AT-1]) kh_fatal(NULL,"xkb-save-name-variant: %s",r->name);
        for(int i=0;i<MAX_SLOTS;i++) if(!c->rows[i].name[0]){
            c->rows[i]=*r; c->mode[i]=add&3;
            if(add&TAG_AXEL) c->rows[i].name[VARIANT_AT]=1;
            return;
        }
        kh_fatal(NULL,"xkb-save-slot-capacity");
    }
}
static int contains(const Chunk *c,const Record *r){
    for(int i=0;i<MAX_SLOTS;i++){
        int variant=row_variant(&c->rows[i]);
        if(!variant){ if(!memcmp(&c->rows[i],r,sizeof *r)) return c->mode[i]; continue; }
        Record row=c->rows[i]; row.name[VARIANT_AT]=0;
        if(!memcmp(&row,r,sizeof row)) return c->mode[i]|TAG_AXEL;
    }
    return 0;
}
static const unsigned char png_end[]={0,0,0,0,'I','E','N','D',0xae,0x42,0x60,0x82};
static u32 chunk_crc(const Chunk *c,size_t size){return crc32(crc32(0,(void*)"khXb",4),(const u8*)c,size);}

/* Every writer inserts immediately before IEND. A stripped, stale or unrecognized tag is ordinary. */
static int read_chunk(const char *path,Chunk *out){
    memset(out,0,sizeof *out); out->version=2;
    FILE *f=fopen(path,"rb"); if(!f) return errno==ENOENT;
    u8 type[4],end[12]; u32 len,crc;
    int ok=0;
    for(int version=2;version>=1;version--){
        Chunk c={0};
        size_t size=version==2?sizeof c:offsetof(Chunk,mode);
        if(fseek(f,-(long)(size+24),SEEK_END) || fread(&len,4,1,f)!=1 || ntohl(len)!=size ||
           fread(type,4,1,f)!=1 || memcmp(type,"khXb",4) || fread(&c,size,1,f)!=1 || c.version!=version ||
           fread(&crc,4,1,f)!=1 || ntohl(crc)!=chunk_crc(&c,size) ||
           fread(end,12,1,f)!=1 || memcmp(end,png_end,12)) continue;
        ok=1;
        for(int i=0;i<MAX_SLOTS;i++){
            if(version==1) c.mode[i]=!!c.rows[i].name[0];
            if(!memchr(c.rows[i].name,0,sizeof c.rows[i].name) || c.mode[i]>3 ||
               !!c.rows[i].name[0]!=!!c.mode[i]) ok=0;
            /* The variant byte: 0, or 1 on a Critical record of a version-2 chunk.  Anything else is a chunk this build
             * does not know: like every unrecognized chunk it is ordinary (all its slots read as untagged saves), and said. */
            int variant=row_variant(&c.rows[i]);
            if(ok && ((!c.rows[i].name[0] && c.rows[i].name[VARIANT_AT]) || variant>1 ||
                      (variant && (version==1 || c.mode[i]!=2)))){
                ok=0;
                static _Atomic int said;
                if(!said++) kh_warn("xkb-chunk-variant: %s: slot record %d has mode %u variant %u; the chunk is ignored",
                                    path,i,c.mode[i],(u8)c.rows[i].name[VARIANT_AT]);
            }
        }
        if(ok){c.version=2; *out=c;}
        break;
    }
    ok=!ferror(f); fclose(f); return ok;
}
static int has_records(const Chunk *c){
    for(int i=0;i<MAX_SLOTS;i++) if(c->rows[i].name[0]) return 1;
    return 0;
}
static int finish_png(const char *path,const Chunk *c){
    FILE *f=fopen(path,"r+b"); if(!f) return 0;
    u32 len=htonl(sizeof *c),crc=htonl(chunk_crc(c,sizeof *c)); u8 tail[12];
    int ok=!fseek(f,-12,SEEK_END)&&fread(tail,12,1,f)==1&&!memcmp(tail,png_end,12)&&!fseek(f,-12,SEEK_END);
    if(ok&&has_records(c)) ok=fwrite(&len,4,1,f)&&fwrite("khXb",4,1,f)&&fwrite(c,sizeof *c,1,f)&&fwrite(&crc,4,1,f)&&fwrite(tail,12,1,f);
    ok=ok&&!fflush(f)&&!fsync(fileno(f)); return fclose(f)==0&&ok;
}
void orig_f_145170(KCtx *);
void f_145170(KCtx *g){
    if(rd64(g,KH_RSP(g))!=GS_BASE+0x100a43){ orig_f_145170(g);return; }
    char win[2048],path[2048],tmp[2100],dir[2048];
    gw_to_utf8(g,A1(g),win,sizeof win); kh_winpath(win,path,sizeof path);
    Chunk c;
    if(!kh_xkb_menu){
        if(!read_chunk(path,&c)) kh_fatal(g,"xkb-save-read");
        if(!has_records(&c)){
            pthread_mutex_lock(&lock);
            if(change_card==KH_RDI(g)) change_card=0;
            pthread_mutex_unlock(&lock);
            orig_f_145170(g);return;
        }
    }
    snprintf(tmp,sizeof tmp,"%s.XXXXXX",path); strcpy(dir,path);
    char *slash=strrchr(dir,'/'); if(!slash) kh_fatal(g,"xkb-save-directory"); *slash=0;
    int d=open(dir,O_RDONLY|O_DIRECTORY);
    int fd=-1,ok=0;
    pthread_mutex_lock(&lock);
    u64 card=KH_RDI(g); /* f_100810 keeps the card manager in RDI through the encode call. */
    if(d>=0&&!flock(d,LOCK_EX)&&read_chunk(path,&c)&&(fd=mkstemp(tmp))>=0){
        close(fd);
        if(change_card==card && change.name[0]) change_record(&c,&change,tagged);
        size_t n=strlen(win)+8;
        u64 w=ga_alloc(n*2,16);
        char alt[2100]; snprintf(alt,sizeof alt,"%s%s",win,tmp+strlen(path)); utf8_to_gw(g,w,n,alt);
        KH_RDX(g)=w; orig_f_145170(g); ga_free(w);
        ok=(u8)KH_RAX(g)&&finish_png(tmp,&c)&&!rename(tmp,path);
        if(!ok) unlink(tmp);
    } else KH_RET(g);
    if(ok){ if(change_card==card) change_card=0; }
    else kh_warn("xkb-save-publish: could not publish container");
    if(d>=0) close(d);
    pthread_mutex_unlock(&lock); SET_RAX(g,ok);
}

static void manager_path(KCtx *g,u64 manager,char path[2048]){
    char win[512]; snprintf(win,sizeof win,"%s%s",gstr(g,manager+0x40),gstr(g,manager+0x144));
    kh_winpath(win,path,2048);
}
/* Save and Load share save.2ld. Sequence group 7 selects the Xion eye strip.
 * f_2ee350 and f_2ee4a0 copy the zero-based portrait byte to slot row +0x23. */
void kh_xion_save_portrait(KCtx *g,u64 buf){
    if(rd8(g,buf+0x419c)==6) return;
    wr8(g,buf+0x419c,6);
    wr32(g,buf+8,kh_save_crc(GUEST_PTR(buf)));
}
void orig_f_100ef0(KCtx *);
void f_100ef0(KCtx *g){
    u64 manager=A0(g),buf=A1(g),name=A2(g); u32 size=A3(g);
    orig_f_100ef0(g);
    if((u32)KH_RAX(g)!=size||size!=SAVE_SIZE) return;
    pthread_mutex_lock(&lock);
    char path[2048]; Chunk c; manager_path(g,manager,path);
    source_tag=record(&source,gstr(g,name),GUEST_PTR(buf),size)&&read_chunk(path,&c)?contains(&c,&source):0;
    /* The eye strip is Xion's; an Axel Won't Cry slot keeps the portrait the save has. */
    if(source_tag && !(source_tag&TAG_AXEL) && rd8(g,buf+0x419c)!=6 && rd32(g,buf+8)==kh_save_crc(GUEST_PTR(buf))){
        kh_xion_save_portrait(g,buf);
        digest(&source,GUEST_PTR(buf),size); /* track the returned copy for load and copy-slot */
    }
    if(kh_xkb_menu) kh_warn("xkb-card-read: %s %u XKB %d",source.name,size,source_tag);
    pthread_mutex_unlock(&lock);
}
void kh_xkb_load(KCtx *g){
    pthread_mutex_lock(&lock);
    Record live=source; digest(&live,GUEST_PTR(GS_BASE+0x9a98b0),SAVE_SIZE);
    int match=(u32)(source_tag&3)==rd32(g,GS_BASE+0x9abd48) && source_tag && !memcmp(&live,&source,sizeof live);
    kh_xkb_set_mode(g,!match?KH_MODE_NONE:(source_tag&TAG_AXEL)?KH_MODE_AXEL:KH_MODE_XION);
    pthread_mutex_unlock(&lock);
}
void kh_xkb_serialized(void){
    pthread_mutex_lock(&lock); source_tag=0; memset(&source,0,sizeof source); pthread_mutex_unlock(&lock);
}
void orig_f_101590(KCtx *);
void f_101590(KCtx *g){
    u64 buf=A1(g),name=A2(g); u32 size=A3(g);
    if(size==SAVE_SIZE){
        pthread_mutex_lock(&lock);
        if(!record(&change,gstr(g,name),GUEST_PTR(buf),size)) kh_fatal(g,"xkb-slot-name");
        change_card=A0(g);
        /* Compared before the OR below: a level above 3 must stay a named fatal, never an alias of another tag. */
        u32 level=rd32(g,buf+0x2498);
        int campaign=kh_xion_enabled?(int)level:kh_axel_enabled?(level<=3?(int)(TAG_AXEL|level):-1):0;
        tagged=source.name[0]&&!memcmp(source.digest,change.digest,32)?source_tag:campaign;
        if(!valid_tag(tagged) || ((kh_xion_enabled || kh_axel_enabled) && !source.name[0] && !tagged))
            kh_fatal(g,"xkb-save-mode: %d",tagged);
        if(kh_xkb_menu) kh_warn("xkb-card-write: %s %u XKB %d",change.name,size,tagged);
        pthread_mutex_unlock(&lock);
    }
    orig_f_101590(g);
}
void orig_f_1004d0(KCtx *);
void f_1004d0(KCtx *g){
    pthread_mutex_lock(&lock);
    if(!record(&change,gstr(g,A1(g)),"",0)) kh_fatal(g,"xkb-delete-name");
    change_card=A0(g); tagged=0; pthread_mutex_unlock(&lock);
    orig_f_1004d0(g);
}

static const char *const difficulties[]={
    "Beginner","Standard","Critical","Less Than Nothing","Xion Knows Best","Xion Always Cries","Axel Won't Cry"
};
enum { LABELS=sizeof difficulties/sizeof *difficulties, STOCK_LABELS=6 };
static u64 difficulty_text[LABELS],autosave_title[2];
/* The seventh label is encoded when a slot first shows it: without such a slot nothing more is allocated than before.  A savestate
 * load drops that one pointer (kh_rewind_xkb_save): the snapshot may predate its allocation. */
static void forget_lazy_label(void){ difficulty_text[LABELS-1]=0; }
static u64 difficulty_label(unsigned label){
    if(!label || label>LABELS) kh_fatal(NULL,"save-difficulty-label: %u",label);
    if(!difficulty_text[label-1]) difficulty_text[label-1]=kh_encode_menu_text(difficulties[label-1],label>3);
    return difficulty_text[label-1];
}
static _Atomic int show_difficulty;
static int trace_rows;
void kh_menu_difficulty_labels(int on){show_difficulty=on;}

void kh_save_difficulty_init(void){
    const char *e=getenv("KHNMC_SAVE_DIFFICULTY");
    if(e&&*e&&strcmp(e,"0")&&strcmp(e,"1")) kh_fatal(NULL,"save-difficulty-switch: expected 0 or 1");
    show_difficulty=!e||strcmp(e,"0");
    trace_rows=getenv("KHNMC_ROUTE_TRACE")!=NULL;
    {
        /* The three long names share a column with up to 99,999 Munny. */
        for(unsigned i=0;i<STOCK_LABELS;i++) difficulty_text[i]=kh_encode_menu_text(difficulties[i],i>=3);
        autosave_title[0]=kh_encode_menu_text("Latest autosave",0);
        autosave_title[1]=kh_encode_menu_text("Previous autosave",0);
    }
}

/* Byte +0x25 is padding between the row flags/portrait and timestamp.
 * Keep the label with its row when the game copies the card directory. */
static unsigned difficulty_index(u32 level,int tag){
    if(level>3) return 0;
    if(level==2 && tag==(TAG_AXEL|2)) return 7;
    return 1+level+(level>=2 && tag==(int)level ? 2 : 0);
}
static void remember_difficulty(KCtx *g,int index,u64 buf,int tag){
    u64 rows=rd64(g,GS_BASE+0xbee368);
    if(index<0) index=(s8)rd8(g,rows+1);
    if((unsigned)index>=99) kh_fatal(g,"save-difficulty-row: %d",index);
    u64 row=rows+40*index;
    u32 level=rd32(g,buf+0x2498);
    unsigned label=difficulty_index(level,tag);
    wr8(g,row+0x25,label);
    /* The native formatter indexes a four-element message table with this signed byte. */
    if(!label) wr8(g,row+0x20,1);
    if(trace_rows && label) kh_warn("save-difficulty: slot %d level %u tag %d %s",index+1,level,tag,difficulties[label-1]);
}

void orig_f_2ee350(KCtx *);
void f_2ee350(KCtx *g){
    int slot=(int)A0(g);
    orig_f_2ee350(g);
    if(show_difficulty){
        pthread_mutex_lock(&lock);
        remember_difficulty(g,slot,rd64(g,GS_BASE+0xbee360),tagged);
        pthread_mutex_unlock(&lock);
    }
}
void orig_f_2ee4a0(KCtx *);
void f_2ee4a0(KCtx *g){
    int slot=(int)A0(g),valid=A2(g)&0x10;
    u64 buf=A1(g);
    orig_f_2ee4a0(g);
    if(show_difficulty && valid){
        if(!buf) buf=rd64(g,GS_BASE+0xbee360);
        pthread_mutex_lock(&lock);
        remember_difficulty(g,slot,buf,source_tag);
        pthread_mutex_unlock(&lock);
    }
}

/* Keep LV, Munny and location. Native layout type 2 is the upper-right
 * difficulty anchor; type 4 puts the clock on the lower right. */
static void label_summary(KCtx *g,u64 fields,unsigned label){
    if(!label || label>LABELS || rd32(g,fields)!=0) return;
    for(int i=2;i<5;i++){
        u64 field=fields+16*i;
        u32 type=rd32(g,field);
        if(type==2){
            if(i==2 && rd64(g,field+8)!=0 && rd32(g,field+16)!=0xffffffffu){
                wr64(g,field+8,difficulty_label(label));
                return;
            }
            wr32(g,field,4); /* The language layout uses type 2 for its last field (time). */
        }else if(type==0xffffffffu){
            wr32(g,field,2);
            wr64(g,field+8,difficulty_label(label));
            return;
        }
    }
}
void orig_f_323250(KCtx *);
void f_323250(KCtx *g){
    orig_f_323250(g);
    if(!show_difficulty) return;
    u64 list=rd64(g,GS_BASE+0xbef650),fields=rd64(g,GS_BASE+0xbef678);
    int first=(s16)rd16(g,list+0x12),count=(s16)rd16(g,list+0x16);
    int visible=(s16)rd16(g,list+4);
    if(!visible) return;
    if(count<=0 || count>99 || visible<0 || visible>4 || first<0) kh_fatal(g,"save-difficulty-list: %d/%d/%d",first,count,visible);
    for(int i=0;i<visible;i++){
        int slot=(first+i)%count;
        u64 row=rd64(g,GS_BASE+0xbee368)+40*slot;
        if(rd32(g,row+8)&0x10) label_summary(g,fields+i*80,rd8(g,row+0x25));
    }
}

/* The ring substitutes its checkpoint only when confirming a populated card row.
 * Cache each validated shortcut in padding of the zero-initialized menu directory
 * (f_2edbf0). Each preview uses the same validation and fallback as the load. */
void orig_f_321950(KCtx *);
void f_321950(KCtx *g){
    int shortcut=show_difficulty && rd32(g,GS_BASE+0x900724)==5 ? kh_autosave_shortcut() : 0;
    if(!shortcut){ orig_f_321950(g); return; }
    u64 list=rd64(g,GS_BASE+0xbef650);
    int cursor=(s16)rd16(g,list);
    if((unsigned)cursor>=4 || rd64(g,KH_RSP(g)+0x28)!=rd64(g,GS_BASE+0xbef6b8+8*cursor)){
        orig_f_321950(g); return;
    }
    u64 fields=rd64(g,GS_BASE+0xbef678)+80*cursor;
    if(rd32(g,fields)!=0){ orig_f_321950(g); return; }
    u64 cache=rd64(g,GS_BASE+0xbee368)+0x25+shortcut;
    unsigned cached=rd8(g,cache);
    if(!cached){
        u32 level=0; int tag=0;
        unsigned label=kh_autosave_preview(g,shortcut,&level,&tag)?difficulty_index(level,tag):0;
        cached=1+label;
        wr8(g,cache,cached);
        if(trace_rows) kh_warn("save-difficulty: %s autosave %s",shortcut==1?"latest":"previous",label?difficulties[label-1]:"unavailable");
    }
    if(cached<2 || cached>1+LABELS){ orig_f_321950(g); return; }
    u8 saved[80];
    memcpy(saved,GUEST_PTR(fields),sizeof saved);
    wr64(g,fields+8,autosave_title[shortcut-1]);
    wr32(g,fields+16,2); wr64(g,fields+24,difficulty_label(cached-1));
    wr64(g,fields+40,0);
    orig_f_321950(g);
    memcpy(GUEST_PTR(fields),saved,sizeof saved);
}

/* The route covers the native PNG encoder. Here each logical operation gets a new
 * carrier tail, as if the encoder had just finished, before an independent reread. */
static int test_publish(const char *path,u64 card){
    Chunk c;
    if(change_card!=card||!read_chunk(path,&c)) return 0;
    change_record(&c,&change,tagged);
    FILE *f=fopen(path,"wb");
    if(!f) return 0;
    int ok=fwrite(png_end,sizeof png_end,1,f)==1;
    if(fclose(f)) ok=0;
    change_card=0;
    return ok&&finish_png(path,&c);
}
/* ---- the Axel extension of the chunk (layout at the top of this file) ------------------------------------------------- */
/* What a build from before the extension does, word for word: its validation, its whole-row lookup, its insertion. */
static int old_valid(const Chunk *c){
    for(int i=0;i<MAX_SLOTS;i++)
        if(!memchr(c->rows[i].name,0,sizeof c->rows[i].name) || c->mode[i]>3 || !!c->rows[i].name[0]!=!!c->mode[i]) return 0;
    return 1;
}
static int old_contains(const Chunk *c,const Record *r){
    for(int i=0;i<MAX_SLOTS;i++) if(!memcmp(&c->rows[i],r,sizeof *r)) return c->mode[i];
    return 0;
}
static void old_change_record(Chunk *c,const Record *r,int add){
    for(int i=0;i<MAX_SLOTS;i++) if(!strcmp(c->rows[i].name,r->name)) {memset(&c->rows[i],0,sizeof *r); c->mode[i]=0;}
    if(add) for(int i=0;i<MAX_SLOTS;i++) if(!c->rows[i].name[0]){ c->rows[i]=*r; c->mode[i]=add; return; }
}
static int carrier(const char *path,const Chunk *c){
    FILE *f=fopen(path,"wb");
    if(!f) return 0;
    int ok=fwrite(png_end,sizeof png_end,1,f)==1;
    if(fclose(f)) ok=0;
    return ok && finish_png(path,c);
}
static long slurp(const char *path,u8 *out,size_t cap){
    FILE *f=fopen(path,"rb");
    if(!f) return -1;
    long n=(long)fread(out,1,cap,f);
    fclose(f);
    return n;
}
static int axel_tag_selftest(void){
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"xkb-axel-XXXXXX");
    int fd=mkstemp(path);
    if(fd<0) return 1;
    close(fd);
    int checks=0,fail=0;
#define AX(c) do { checks++; if(!(c)){ fprintf(stderr,"xkb axel tag FAIL: %s\n",#c); fail++; } } while(0)
    enum { FILE_SIZE=sizeof(Chunk)+24 };   /* length, type, data, CRC, then IEND */
    static u8 before[FILE_SIZE+16],after[FILE_SIZE+16];
    Record a,b,x,longname; Chunk old={.version=2},c;
    record(&a,"card/BISLPM-66675FM-01","A",1); record(&b,"card/BISLPM-66675FM-02","B",1);
    record(&x,"card/BISLPM-66675FM-03","X",1);
    char sixty_three[64]; memset(sixty_three,'n',63); sixty_three[63]=0;
    record(&longname,sixty_three,"L",1);
    /* 1. A chunk as an older build wrote it: read and written again by this build, byte for byte the same. */
    old_change_record(&old,&a,2); old_change_record(&old,&b,3); old_change_record(&old,&longname,3);
    AX(carrier(path,&old) && slurp(path,before,sizeof before)==FILE_SIZE);
    AX(read_chunk(path,&c) && !memcmp(&c,&old,sizeof c));
    AX(contains(&c,&a)==2 && contains(&c,&b)==3 && contains(&c,&longname)==3 && !contains(&c,&x));
    AX(carrier(path,&c) && slurp(path,after,sizeof after)==FILE_SIZE && !memcmp(before,after,FILE_SIZE));
    /* The same after this build tagged another slot as Axel Won't Cry and then deleted it again. */
    change_record(&c,&x,TAG_AXEL|2);
    AX(carrier(path,&c) && read_chunk(path,&c) && contains(&c,&x)==(TAG_AXEL|2));
    change_record(&c,&x,0);
    AX(carrier(path,&c) && slurp(path,after,sizeof after)==FILE_SIZE && !memcmp(before,after,FILE_SIZE));
    /* 2. The new mode round trip, and what an older build makes of that chunk. */
    change_record(&c,&x,TAG_AXEL|2);
    AX(carrier(path,&c) && slurp(path,after,sizeof after)==FILE_SIZE);
    Chunk fresh;
    AX(read_chunk(path,&fresh) && !memcmp(&fresh,&c,sizeof c));
    AX(contains(&fresh,&x)==(TAG_AXEL|2) && contains(&fresh,&a)==2 && contains(&fresh,&b)==3 && contains(&fresh,&longname)==3);
    AX(old_valid(&fresh));                                        /* the older build keeps the chunk: no slot loses its tag */
    AX(old_contains(&fresh,&a)==2 && old_contains(&fresh,&b)==3); /* the Xion slots as before */
    AX(old_contains(&fresh,&x)==0);                               /* the Axel slot: untagged = plain Critical */
    Chunk carried=fresh;                                          /* the older build saves another slot: the Axel row survives */
    old_change_record(&carried,&b,2);
    AX(contains(&carried,&x)==(TAG_AXEL|2) && contains(&carried,&b)==2);
    old_change_record(&carried,&x,0);                             /* and its own overwrite / delete of that slot clears the row by name */
    AX(!contains(&carried,&x) && !old_contains(&carried,&x) && contains(&carried,&a)==2);
    /* Only the one byte differs from the same slot tagged Xion Knows Best. */
    Chunk twin=old; change_record(&twin,&x,2);
    int differing=0,where=-1;
    for(size_t i=0;i<sizeof twin;i++) if(((const u8 *)&twin)[i]!=((const u8 *)&c)[i]){ differing++; where=(int)i; }
    AX(differing==1 && where==1+3*(int)sizeof(Record)+VARIANT_AT);
    /* 3. Unknown and corrupt values: the chunk is ignored (every slot ordinary), never half read. */
    static const struct { int row,mode,variant; } bad[]={{3,2,2},{3,2,255},{3,3,1},{3,1,1},{5,0,1}};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++){
        Chunk k=c;
        k.mode[bad[i].row]=bad[i].mode; k.rows[bad[i].row].name[VARIANT_AT]=(char)bad[i].variant;
        Chunk got;
        AX(carrier(path,&k) && read_chunk(path,&got) && !has_records(&got) && !contains(&got,&a) && !contains(&got,&x));
    }
    { Chunk k=c; k.mode[0]=4; Chunk got; AX(carrier(path,&k) && read_chunk(path,&got) && !has_records(&got)); }   /* the old rule */
    { Chunk k=c; k.rows[2].name[VARIANT_AT]=1; Chunk got;        /* a 63-character name owns its last byte: not a variant */
      AX(carrier(path,&k) && read_chunk(path,&got) && !has_records(&got)); }                                      /* no NUL: the old rule */
    AX(carrier(path,&c)); after[FILE_SIZE-13]^=1;                 /* a flipped CRC bit */
    { FILE *f=fopen(path,"wb"); Chunk got; AX(f && fwrite(after,FILE_SIZE,1,f)==1 && !fclose(f) && read_chunk(path,&got) && !has_records(&got)); }
    AX(valid_tag(0) && valid_tag(2) && valid_tag(3) && valid_tag(TAG_AXEL|2));
    AX(!valid_tag(TAG_AXEL) && !valid_tag(TAG_AXEL|1) && !valid_tag(TAG_AXEL|3) && !valid_tag(8) && !valid_tag(-1));
    AX(difficulty_index(2,TAG_AXEL|2)==7 && difficulty_index(3,TAG_AXEL|2)==4 && difficulty_index(1,TAG_AXEL|2)==2);
    AX(difficulty_index(2,2)==5 && difficulty_index(3,3)==6 && difficulty_index(2,0)==3);
#undef AX
    unlink(path);
    fprintf(stderr,"kh: xkb axel tag: old chunk byte-identical, new mode round trip, older-build view, rejected values: %d checks, %d failures\n",checks,fail);
    return fail;
}
int kh_xkb_save_selftest(KCtx *g){
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"xkb-chunk-XXXXXX");
    int fd=mkstemp(path);
    if(fd<0) return 1;
    close(fd);
    if(strlen(path)>=0x104){ unlink(path); return 1; }
    KCtx ctx=*g;
    u64 card=ga_alloc(0x200,16), directory=ga_alloc(3*0x158,16);
    u64 slots=ga_alloc(3*SAVE_SIZE,16), buf=ga_alloc(SAVE_SIZE,16), names=ga_alloc(32,16);
    memset(GUEST_PTR(card),0,0x200); memset(GUEST_PTR(directory),0,3*0x158);
    memset(GUEST_PTR(buf),0,SAVE_SIZE);
    wr32(g,buf+0x2498,1); wr32(g,buf+8,kh_save_crc(GUEST_PTR(buf)));
    memcpy(GUEST_PTR(names),"card/slotA",11); memcpy(GUEST_PTR(names+16),"card/slotB",11);
    strcpy(GUEST_PTR(card+0x40),path);
    wr64(g,card+0x18,directory); wr64(g,card+0x20,slots); wr32(g,card+0x28,3); wr64(g,card+0x30,SAVE_SIZE);
    int prior=kh_xion_enabled,fail=0;
    for(unsigned i=0;i<4;i++) fail+=difficulty_index(i,0)!=i+1;
    fail+=difficulty_index(2,2)!=5 || difficulty_index(3,3)!=6 || difficulty_index(3,2)!=4 || difficulty_index(256,0)!=0;
    if(show_difficulty){
        /* The native formatter can omit difficulty, location, or both. */
        u64 fields=buf+0x100;
        for(int existing=0;existing<=1;existing++) for(int place=0;place<=1;place++){
            memset(GUEST_PTR(fields),0xff,80);
            wr32(g,fields,0); wr32(g,fields+16,1);
            int n=2;
            if(existing){ wr32(g,fields+16*n,2); wr64(g,fields+16*n+8,names); n++; }
            if(place) wr32(g,fields+16*n++,3);
            u64 time=fields+16*n;
            wr32(g,time,existing?4:2); wr64(g,time+8,names+16);
            label_summary(g,fields,6);
            u64 label=existing ? fields+32 : time+16;
            fail+=rd32(g,label)!=2 || rd64(g,label+8)!=difficulty_text[5];
            fail+=rd32(g,time)!=4 || rd64(g,time+8)!=names+16;
        }
        wr32(g,fields,5); wr32(g,fields+32,0xffffffffu);
        label_summary(g,fields,4); fail+=rd32(g,fields+32)!=0xffffffffu;
        wr32(g,fields,0); label_summary(g,fields,0); fail+=rd32(g,fields+32)!=0xffffffffu;
        memset(GUEST_PTR(fields),0,80);
    }
    kh_xkb_serialized(); kh_xion_enabled=1;
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names,SAVE_SIZE);
    fail+=!test_publish(path,card);
    kh_xion_enabled=0;
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names+16,SAVE_SIZE);
    fail+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); fail+=source_tag!=0 || rd8(g,buf+0x419c)!=0;
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names,SAVE_SIZE);
    fail+=source_tag!=1 || rd8(g,buf+0x419c)!=6 || rd32(g,buf+8)!=kh_save_crc(GUEST_PTR(buf));
    /* Copy A's read payload over B while the current campaign is Standard. */
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names+16,SAVE_SIZE); fail+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x1004d0,card,names,0,0); fail+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); fail+=source_tag!=1;
    Chunk c; Record a; record(&a,"slotA",GUEST_PTR(buf),SAVE_SIZE);
    fail+=!read_chunk(path,&c)||contains(&c,&a);
    /* A newly serialized Standard save with identical bytes clears B's old record. */
    kh_xkb_serialized();
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names+16,SAVE_SIZE); fail+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); fail+=source_tag!=0;
    kh_xion_enabled=prior; kh_xkb_serialized();
    memset(&change,0,sizeof change); tagged=0;
    ga_free(names); ga_free(buf); ga_free(slots); ga_free(directory); ga_free(card); unlink(path);
    fprintf(stderr,"kh: xkb native slot identity, copy, overwrite, deletion and chunk round trips: %d failures\n",fail);
    /* The Axel Won't Cry campaign through the same native slot calls: write, publish, read back, copy, then an ordinary overwrite. */
    int axel=0,prior_axel=kh_axel_enabled; prior=kh_xion_enabled;
    card=ga_alloc(0x200,16); directory=ga_alloc(3*0x158,16); slots=ga_alloc(3*SAVE_SIZE,16); buf=ga_alloc(SAVE_SIZE,16); names=ga_alloc(32,16);
    memset(GUEST_PTR(card),0,0x200); memset(GUEST_PTR(directory),0,3*0x158); memset(GUEST_PTR(buf),0,SAVE_SIZE);
    kh_tmp_path(path,sizeof path,"xkb-chunk-XXXXXX"); fd=mkstemp(path);
    if(fd<0) return fail+1;
    close(fd);
    wr32(g,buf+0x2498,2); wr32(g,buf+8,kh_save_crc(GUEST_PTR(buf)));
    memcpy(GUEST_PTR(names),"card/slotA",11); memcpy(GUEST_PTR(names+16),"card/slotB",11);
    strcpy(GUEST_PTR(card+0x40),path);
    wr64(g,card+0x18,directory); wr64(g,card+0x20,slots); wr32(g,card+0x28,3); wr64(g,card+0x30,SAVE_SIZE);
    kh_xkb_serialized(); kh_xion_enabled=0; kh_axel_enabled=1;
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names,SAVE_SIZE); axel+=tagged!=(TAG_AXEL|2);
    axel+=!test_publish(path,card);
    kh_axel_enabled=0;
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names,SAVE_SIZE);
    axel+=source_tag!=(TAG_AXEL|2) || rd8(g,buf+0x419c)!=0;            /* read back as Axel Won't Cry; the portrait is the save's own */
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names+16,SAVE_SIZE); axel+=tagged!=(TAG_AXEL|2);   /* copy A over B in an ordinary campaign */
    axel+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); axel+=source_tag!=(TAG_AXEL|2);
    kh_xkb_serialized(); kh_xion_enabled=1;                               /* a Xion Knows Best save of the same bytes over A */
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names,SAVE_SIZE); axel+=tagged!=2;
    axel+=!test_publish(path,card);
    kh_xion_enabled=0;
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); axel+=source_tag!=(TAG_AXEL|2) || rd8(g,buf+0x419c)!=0;
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names,SAVE_SIZE); axel+=source_tag!=2 || rd8(g,buf+0x419c)!=6;
    kh_xkb_serialized();                                                  /* an ordinary Critical save over B clears its record */
    wr8(g,buf+0x419c,0); wr32(g,buf+8,kh_save_crc(GUEST_PTR(buf)));
    kh_call(&ctx,GS_BASE+0x101590,card,buf,names+16,SAVE_SIZE); axel+=tagged!=0;
    axel+=!test_publish(path,card);
    kh_call(&ctx,GS_BASE+0x100ef0,card,buf,names+16,SAVE_SIZE); axel+=source_tag!=0;
    kh_xion_enabled=prior; kh_axel_enabled=prior_axel; kh_xkb_serialized();
    memset(&change,0,sizeof change); tagged=0;
    ga_free(names); ga_free(buf); ga_free(slots); ga_free(directory); ga_free(card); unlink(path);
    fprintf(stderr,"kh: xkb axel native slot write, read, copy and overwrite: %d failures\n",axel);
    return fail+axel+axel_tag_selftest();
}
