#include "rewind.h"

struct KhBlock {
    struct KhBlock *next;
    size_t size;
    int value;
    unsigned char data[];
};
void kh_records_begin(KhRecords *r,int load){
    r->position=0; r->loading=load; r->read=r->first; r->tail=&r->first;
}
void kh_records_end(KhRecords *r){
    if(r->loading&&r->read) kh_fatal(NULL,"savestate-record-count");
}
void kh_records_free(KhRecords *r){
    while(r->first){ struct KhBlock *b=r->first; r->first=b->next; free(b); }
}
static void record(KhRecords *r,void *p,size_t size,int sparse,int value){
    static const unsigned char zero[16384];
    r->position++;
    if(r->loading){
        struct KhBlock *b=r->read;
        if(!b||(b->size!=size&&!(sparse&&!b->size))) kh_fatal(NULL,"savestate-record-size");
        const void *src=b->size?b->data:zero;
        if(size&&memcmp(p,src,size)) memcpy(p,src,size);
        r->read=b->next;
    }else{
        size_t n=sparse&&!memcmp(p,zero,size)?0:size;
        struct KhBlock *b=malloc(sizeof *b+n);
        if(!b) kh_fatal(NULL,"savestate-record-memory");
        b->next=NULL; b->size=n; b->value=value; if(n) memcpy(b->data,p,n);
        *r->tail=b; r->tail=&b->next;
    }
}
void kh_record(KhRecords *r,void *p,size_t n){ record(r,p,n,0,0); }
/* A value may change between bootstraps. The local schema still supplies its
 * size. Pointers, dimensions and graph topology always use structural records. */
void kh_record_value(KhRecords *r,void *p,size_t n){ record(r,p,n,0,1); }
void kh_record_time(KhRecords *r,u64 *when){
    u64 delta=*when?*when-kh_vclock_origin():UINT64_MAX;
    KH_VALUE(r,delta);
    if(r->loading) *when=delta==UINT64_MAX?0:kh_vclock_origin()+delta;
}
void kh_record_pages(KhRecords *r,void *mem,size_t size){
    unsigned char *p=mem;
    while(size){ size_t n=size<16384?size:16384; record(r,p,n,1,0); p+=n; size-=n; }
}

#include "slot_disk.h"
#include "slot_hash.h"
#include <stddef.h>
#include <sys/stat.h>

/* The length of every allocator record is fixed by the build or bounded by
 * its 16 KiB page size and per-window counters. Never allocate a disk length
 * before checking both the individual record and the remaining section. */
enum { MAX_BLOCK=1024*1024 };
#define MAX_SLOT_BYTES (16ull<<30)
#include <zlib.h>

static int hashed_write(FILE *f,SlotHash *hash,const void *p,size_t n){
    if(!n) return 1;
    slot_hash_add(hash,p,n);
    return fwrite(p,1,n,f)==n;
}
static int hashed_read(FILE *f,SlotHash *hash,void *p,size_t n){
    if(!n) return 1;
    if(fread(p,1,n,f)!=n) return 0;
    slot_hash_add(hash,p,n);
    return 1;
}
static void slot_checksum(const KhSlotHeader *h,SlotHash *body,u8 *digest){
    u8 payload[32];
    slot_hash_end(payload,body);
    SlotHash hash; slot_hash_init(&hash);
    slot_hash_add(&hash,h,offsetof(KhSlotHeader,checksum));
    slot_hash_add(&hash,payload,sizeof payload);
    slot_hash_end(digest,&hash);
}
const char *kh_slot_write(FILE *f,KhSlotHeader *h,KhRecords *r,const KhPadHistory *pad,const KhBootDocs *docs){
    h->changes=pad->count; h->polls=pad->polls;
    if(pad->count>4000000) return "savestate-pad-history-size";
    size_t journal_bytes=pad->count*sizeof *pad->changes;
    h->docs=docs->size;
    h->blocks=0; h->bytes=journal_bytes+h->docs;
    SlotHash hash; slot_hash_init(&hash);
    if(fwrite(h,sizeof *h,1,f)!=1 || !hashed_write(f,&hash,pad->changes,journal_bytes)||!hashed_write(f,&hash,docs->data,docs->size)) return "savestate-slot-write";
    unsigned char *packed=malloc(MAX_BLOCK);
    if(!packed) return "savestate-slot-memory";
    const char *err=NULL;
    for(struct KhBlock *b=r->first;b;b=b->next){
        if(b->size>MAX_BLOCK){ err="savestate-arena-block-size"; break; }
        u32 lengths[2]={b->size,b->size};
        const u8 *payload=b->data;
        if(b->size>=512){
            uLongf n=b->size-1;
            int z=compress2(packed,&n,b->data,b->size,Z_BEST_SPEED);
            if(z==Z_OK){ payload=packed; lengths[1]=n; }
            else if(z!=Z_BUF_ERROR){ err="savestate-slot-compress"; break; }
        }
        h->blocks++; h->bytes+=sizeof lengths+lengths[1];
        if(!hashed_write(f,&hash,lengths,sizeof lengths)||!hashed_write(f,&hash,payload,lengths[1])){
            err="savestate-slot-write"; break;
        }
    }
    free(packed);
    if(err) return err;
    if(h->bytes>MAX_SLOT_BYTES||h->blocks>MAX_SLOT_BYTES/16384+65536) return "savestate-arena-size";
    slot_checksum(h,&hash,h->checksum);
    if(fseeko(f,0,SEEK_SET)||fwrite(h,sizeof *h,1,f)!=1) return "savestate-slot-header";
    return NULL;
}
const char *kh_slot_read(FILE *f,const KhSlotHeader *h,KhRecords *r,KhPadHistory *pad,KhBootDocs *docs,const KhRecords *expected){
    if(!h->blocks||h->bytes>MAX_SLOT_BYTES||h->blocks>MAX_SLOT_BYTES/16384+65536||
       h->blocks>h->bytes/(2*sizeof(u32))) return "savestate-load-length";
    struct stat st;
    if(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||(u64)st.st_size!=sizeof *h+h->bytes)
        return "savestate-load-length";
    SlotHash hash; slot_hash_init(&hash);
    if(r) kh_records_begin(r,0);
    u64 remaining=h->bytes;
    if(h->changes>remaining/sizeof(KhPadChange)||h->changes>h->polls||
       h->changes>4000000 || (!h->changes&&h->polls)) return "savestate-pad-history-size";
    size_t bytes=h->changes*sizeof(KhPadChange);
    pad->count=h->changes; pad->polls=h->polls;
    if(bytes){
        pad->changes=malloc(bytes);
        if(!pad->changes) return "savestate-load-memory";
        if(!hashed_read(f,&hash,pad->changes,bytes)) return "savestate-load-truncated";
        for(size_t i=0;i<pad->count;i++){
            KhPadChange *c=pad->changes+i;
            if(c->reserved || c->poll>=pad->polls || (i?c->poll<=c[-1].poll:c->poll!=0))
                return "savestate-pad-history-order";
        }
        remaining-=bytes;
    }
    if(h->docs>KH_SLOT_DOCS_MAX||h->docs>remaining) return "savestate-documents-size";
    if(h->docs){
        docs->data=malloc(h->docs);
        if(!docs->data) return "savestate-documents-memory";
        docs->size=docs->capacity=h->docs;
        if(!hashed_read(f,&hash,docs->data,docs->size)) return "savestate-load-truncated";
        remaining-=h->docs;
    }
    struct KhBlock *match=expected?expected->first:NULL;
    u8 *scratch=malloc(MAX_BLOCK);
    if(!scratch) return "savestate-load-memory";
    const char *err=NULL;
    u64 decoded=0;
    for(u64 i=0;i<h->blocks;i++){
        u32 lengths[2];
        if(remaining<sizeof lengths||!hashed_read(f,&hash,lengths,sizeof lengths)){ err="savestate-load-truncated"; break; }
        remaining-=sizeof lengths;
        u32 n=lengths[0],stored=lengths[1];
        decoded+=n;
        if(n>MAX_BLOCK||stored>n||stored>remaining||(!stored&&n)||decoded>MAX_SLOT_BYTES){ err="savestate-load-block-size"; break; }
        if(!hashed_read(f,&hash,scratch,stored)){ err="savestate-load-truncated"; break; }
        remaining-=stored;
        if(!r) continue;
        if(expected&&(!match||match->size!=n)){
            kh_warn("savestate-cold-record-size: record %llu disk=%u local=%zu",(unsigned long long)i,n,match?match->size:0);
            err="savestate-cold-layout-mismatch"; break;
        }
        struct KhBlock *b=malloc(sizeof *b+n);
        if(!b){ err="savestate-load-memory"; break; }
        b->size=n; b->value=0; b->next=NULL; *r->tail=b; r->tail=&b->next;
        if(n==stored) memcpy(b->data,scratch,n);
        else{
            uLongf out=n;
            if(uncompress(b->data,&out,scratch,stored)!=Z_OK||out!=n){ err="savestate-load-compression"; break; }
        }
        if(match&&!match->value&&memcmp(b->data,match->data,n)){
            unsigned j=0; while(j<n&&b->data[j]==match->data[j]) j++;
            kh_warn("savestate-cold-record-bytes: record %llu offset=%u",(unsigned long long)i,j);
            err="savestate-cold-binding"; break;
        }
        if(match) match=match->next;
    }
    free(scratch);
    if(err) return err;
    if(match) return "savestate-cold-layout-mismatch";
    u8 digest[32]; slot_checksum(h,&hash,digest);
    if(remaining||memcmp(digest,h->checksum,sizeof digest)) return "savestate-load-checksum";
    return NULL;
}
