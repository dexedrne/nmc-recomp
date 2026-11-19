/* Fresh-process continuation microtest, using three actual lifted functions and
 * synthetic guest bytes. The test driver extracts the audited basic blocks. */
#include "khrt.h"
#include <assert.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <sys/mman.h>

static KCtx ctx;
static ucontext_t main_ctx, fiber_ctx;
static unsigned char native_stack[65536];
static unsigned resume_label;
static int loading, parked;
enum { PAGE_BYTES=4096 };
static struct {
    u32 outer, inner;
    u64 fiber_local;
    u8 regs[KH_ST_SIZE], guest_page[PAGE_BYTES];
} cut;
static u8 *arena;
static void leaf_yield(KCtx *g){
    volatile u64 live=KH_RBX(g)^0x193711ull;
    cut.fiber_local=live;
    parked=1;
    assert(!swapcontext(&fiber_ctx,&main_ctx));
    KH_R64(g,KO_R13)=live;
}
#include "continuations.inc"

static void run_fiber(void){
    if(loading){
        KH_R64(&ctx,KO_R13)=cut.fiber_local;
        f_160560(&ctx);
        resume_label=cut.inner;
        f_160530(&ctx);
        resume_label=cut.outer;
    }
    f_1568b0(&ctx);
    parked=0;
}
static int save_bytes(const char *name,const void *a,size_t bytes){
    FILE *f=fopen(name,"wb");
    if(!f) return 0;
    int ok=fwrite(a,1,bytes,f)==bytes;
    return fclose(f)==0&&ok;
}
int main(int argc,char **argv){
    assert(argc==2);
    loading=!strcmp(argv[1],"load");
    /* A fixed, distinct bias on each branch catches accidental host pointers. */
    void *addr=(void*)(loading?0x3000000000ull:0x2000000000ull);
    arena=mmap(addr,0xc00000,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0);
    assert(arena==addr);
    ctx.bias=(uintptr_t)arena-KH_IMAGE_BASE;
    if(loading){
        FILE *f=fopen("micro.slot","rb");
        u8 format[32],expected[32],actual[32];
        const char *reason=NULL;
        if(!f||fread(format,1,32,f)!=32||fread(expected,1,32,f)!=32||
           fread(&cut,1,sizeof cut,f)!=sizeof cut||fgetc(f)!=EOF) reason="length";
        else if(memcmp(format,lift_id,32)) reason="lift-mismatch";
        else if(!SHA256((const u8*)&cut,sizeof cut,actual)||memcmp(actual,expected,32)) reason="checksum";
        else if(cut.outer!=0x1568be||cut.inner!=0x16053e) reason="caller-chain";
        if(reason){
            fprintf(stderr,"micro-slot-refused: %s\n",reason);
            return 3;
        }
        assert(!fclose(f));
        memcpy(ctx.st,cut.regs,sizeof ctx.st);
        memcpy(arena,cut.guest_page,PAGE_BYTES);
    }else{
        KH_RSP(&ctx)=KH_IMAGE_BASE+0xf00;
        KH_RCX(&ctx)=KH_IMAGE_BASE+0x80;
        KH_RBX(&ctx)=0xfedcba9876543210ull;
    }
    assert(!getcontext(&fiber_ctx));
    fiber_ctx.uc_stack.ss_sp=native_stack;
    fiber_ctx.uc_stack.ss_size=sizeof native_stack;
    fiber_ctx.uc_link=&main_ctx;
    makecontext(&fiber_ctx,run_fiber,0);
    assert(!swapcontext(&main_ctx,&fiber_ctx));
    if(!loading){
        assert(parked);
        KCtx *g=&ctx;
        cut.inner=(u32)(LD(u64,KH_RSP(g))-KH_IMAGE_BASE);
        cut.outer=(u32)(LD(u64,KH_RSP(g)+0x30)-KH_IMAGE_BASE);
        assert(cut.outer==0x1568be&&cut.inner==0x16053e);
        memcpy(cut.regs,ctx.st,sizeof ctx.st);
        memcpy(cut.guest_page,arena,PAGE_BYTES);
        FILE *slot=fopen("micro.slot","wb");
        u8 digest[32];
        assert(slot&&SHA256((const u8*)&cut,sizeof cut,digest));
        assert(fwrite(lift_id,1,32,slot)==32&&fwrite(digest,1,32,slot)==32);
        assert(fwrite(&cut,1,sizeof cut,slot)==sizeof cut&&!fclose(slot));
        assert(!swapcontext(&main_ctx,&fiber_ctx));
    }
    assert(!parked&&KH_RSP(&ctx)==KH_IMAGE_BASE+0xf08&&KH_RAX(&ctx)==KH_IMAGE_BASE+0x80);
    memcpy(cut.regs,ctx.st,sizeof ctx.st);
    memcpy(cut.guest_page,arena,PAGE_BYTES);
    assert(save_bytes(loading?"load.end":"save.end",&cut,sizeof cut));
    return 0;
}
