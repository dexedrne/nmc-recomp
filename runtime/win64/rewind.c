#include "rewind.h"

struct KhBlock {
    struct KhBlock *next;
    size_t size;
    unsigned char data[];
};
void kh_records_begin(KhRecords *r,int load){
    r->loading=load; r->read=r->first; r->tail=&r->first;
}
void kh_records_end(KhRecords *r){
    if(r->loading&&r->read) kh_fatal(NULL,"savestate-record-count");
}
void kh_records_free(KhRecords *r){
    while(r->first){ struct KhBlock *b=r->first; r->first=b->next; free(b); }
}
static void record(KhRecords *r,void *p,size_t size,int sparse){
    static const unsigned char zero[16384];
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
        b->next=NULL; b->size=n; if(n) memcpy(b->data,p,n);
        *r->tail=b; r->tail=&b->next;
    }
}
void kh_record(KhRecords *r,void *p,size_t n){ record(r,p,n,0); }
void kh_record_pages(KhRecords *r,void *mem,size_t size){
    unsigned char *p=mem;
    while(size){ size_t n=size<16384?size:16384; record(r,p,n,1); p+=n; size-=n; }
}
