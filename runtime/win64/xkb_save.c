#include "xion_swap.h"
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
typedef struct { char name[64]; u8 digest[32]; } Record;
typedef struct { u8 version; Record rows[MAX_SLOTS]; u8 mode[MAX_SLOTS]; } Chunk;
static Record change,source;
static int tagged,source_tag;
static u64 change_card;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

void kh_rewind_xkb_save(KhRecords *r){
    pthread_mutex_lock(&lock);
    KH_RECORD(r,change_card);
    KH_VALUE(r,change); KH_VALUE(r,source); KH_VALUE(r,tagged); KH_VALUE(r,source_tag);
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
static void change_record(Chunk *c,const Record *r,int add){
    for(int i=0;i<MAX_SLOTS;i++) if(!strcmp(c->rows[i].name,r->name)) {memset(&c->rows[i],0,sizeof *r); c->mode[i]=0;}
    if(add){
        for(int i=0;i<MAX_SLOTS;i++) if(!c->rows[i].name[0]){ c->rows[i]=*r; c->mode[i]=add; return; }
        kh_fatal(NULL,"xkb-save-slot-capacity");
    }
}
static int contains(const Chunk *c,const Record *r){
    for(int i=0;i<MAX_SLOTS;i++) if(!memcmp(&c->rows[i],r,sizeof *r)) return c->mode[i];
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
    if(source_tag && rd8(g,buf+0x419c)!=6 && rd32(g,buf+8)==kh_save_crc(GUEST_PTR(buf))){
        kh_xion_save_portrait(g,buf);
        digest(&source,GUEST_PTR(buf),size); /* track the returned copy for load and copy-slot */
    }
    if(kh_xkb_menu) kh_warn("xkb-card-read: %s %u XKB %d",source.name,size,source_tag);
    pthread_mutex_unlock(&lock);
}
void kh_xkb_load(KCtx *g){
    pthread_mutex_lock(&lock);
    Record live=source; digest(&live,GUEST_PTR(GS_BASE+0x9a98b0),SAVE_SIZE);
    kh_xkb_set_mode(g,source_tag==rd32(g,GS_BASE+0x9abd48) && source_tag && !memcmp(&live,&source,sizeof live));
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
        tagged=source.name[0]&&!memcmp(source.digest,change.digest,32)?source_tag:kh_xion_enabled?(int)rd32(g,buf+0x2498):0;
        if(tagged<0 || tagged>3 || (kh_xion_enabled && !source.name[0] && !tagged)) kh_fatal(g,"xkb-save-mode: %d",tagged);
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
    return fail;
}
