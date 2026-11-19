#include "savestate.h"
#include <setjmp.h>

/* Import entry still has its return address on the guest stack. Match the entire
 * chain to this thread's kh_call sentinel before discarding any C activation. */
#define SUCCESSORS(X) X(44f710) X(44d960) X(979a2) X(97cfa) X(97d4f) X(fb0e0) X(fab61) X(fe87c) X(fddde) \
    X(130056) X(1300ff) X(15472f) X(128c4f) X(129119) X(fddab) X(15edf5) X(3a0d8c) \
    X(105479) X(14f4ac) X(14373f) X(4392de)
#define DECL(site) void kh_resume_##site(KCtx *);
SUCCESSORS(DECL)
struct Frame { unsigned offset,rva; KhFn resume; };
struct Chain { unsigned entry,size; enum KhCutWait kind; struct Frame frames[6]; };
#define AT(offset,site) {offset,0x##site,kh_resume_##site}
static const struct Chain chains[]={
    {0x44ea80,0x770,KH_CUT_SLEEP,{AT(0,44f710)}},
    {0x44d850,0x30,KH_CUT_HANDLE,{AT(0,44d960)}},
    {0xfab10,0x90,KH_CUT_HANDLE,{AT(0,fb0e0),AT(0x60,fab61)}},
    {0xfab10,0xd0,KH_CUT_SLEEP,{AT(0,979a2),AT(0xa0,fab61)}},
    {0xfab10,0xd0,KH_CUT_SLEEP,{AT(0,97cfa),AT(0xa0,fab61)}},
    {0xfab10,0xd0,KH_CUT_HANDLE,{AT(0,97d4f),AT(0xa0,fab61)}},
    {0xfddd0,0x80,KH_CUT_SLEEP,{AT(0,fe87c),AT(0x50,fddde)}},
    {0xfddd0,0x80,KH_CUT_HANDLE,{AT(0,130056),AT(0x50,fddde)}},
    {0xfddd0,0x80,KH_CUT_HANDLE,{AT(0,1300ff),AT(0x50,fddde)}},
    {0x129110,0x250,KH_CUT_RELEASE,{AT(0,15472f),AT(0x30,128c4f),AT(0x220,129119)}},
    {0x16c9b0,0xf0,KH_CUT_HANDLE,{AT(0,fddab),AT(0x30,15edf5),AT(0x60,3a0d8c),
        {0x90,0x15ed58,NULL},{0xc0,0x16c9bd,NULL}}},
    {0x4393d0,0x1750,KH_CUT_SHELL,{AT(0,105479),AT(0x40,14f4ac),AT(0x16e0,14373f),AT(0x1710,4392de)}}
};
static _Thread_local struct Worker {
    jmp_buf dispatch;
    u64 entry_sp,entry;
    const struct Chain *chain;
    KhFn body;
} *worker;
_Thread_local enum KhCutWait kh_cut_wait_owner;

void kh_cut_transfer_wait(KCtx *g,enum KhCutWait kind,KhFn body){
    if(!worker||kh_cut_wait_owner||kh_cut_callback_depth||!kh_cut_loader_safe()) return;
    KThread *t=cur_thread();
    u64 sp=KH_RSP(g),top=worker->entry_sp;
    if(g!=t->g||sp<t->stack_lo||top<sp||top>t->stack_hi-8||
       rd64(g,top)!=0x7ff7ffff0000ull) return;
    for(unsigned i=0;i<sizeof chains/sizeof chains[0];i++){
        const struct Chain *c=&chains[i];
        if(worker->entry!=c->entry||kind!=c->kind||top-sp!=c->size) continue;
        unsigned j=0;
        while(c->frames[j].rva&&rd64(g,sp+c->frames[j].offset)==GS_BASE+c->frames[j].rva) j++;
        if(c->frames[j].rva) continue;
        worker->chain=c;
        worker->body=body;
        longjmp(worker->dispatch,1);
    }
}

/* Keep mutable dispatch state in TLS. setjmp's automatic locals must survive
 * repeated transfers from the reconstructed successors. */
void kh_cut_worker_loop(KCtx *g,u64 target){
    static _Thread_local struct Worker state;
    KThread *t=cur_thread();
    if(!t||worker||(t->start!=target&&!(t->is_main&&target==GS_BASE+0x4393d0))){
        kh_dispatch(g,target);
        return;
    }
    u64 entry=target-GS_BASE;
    if(entry==0xfddd0&&rd64(g,KH_RCX(g)+0x20)==GS_BASE+0xfe930){
        kh_cut_stream_loop(g);
        return;
    }
    unsigned i=0;
    while(i<sizeof chains/sizeof chains[0]&&chains[i].entry!=entry) i++;
    if(i==sizeof chains/sizeof chains[0]){ kh_dispatch(g,target); return; }
    worker=&state;
    state.entry=entry;
    state.entry_sp=KH_RSP(g);
    if(!setjmp(state.dispatch)) kh_dispatch(g,target);
    else{
        const struct Chain *c=state.chain;
        kh_cut_wait_owner=c->kind;
        state.body(g);
        kh_cut_wait_owner=0;
        if(c->kind!=KH_CUT_SHELL) KH_RET(g);
        for(const struct Frame *f=c->frames;f->rva;f++){
            u64 at=state.entry_sp-c->size+f->offset;
            if(KH_RSP(g)!=at+8||rd64(g,at)!=GS_BASE+f->rva||!f->resume)
                kh_fatal(g,"savestate-worker-return: unexpected successor at %x",f->rva);
            if(f->rva==0x3a0d8c) kh_cut_loader_relock(g);
            f->resume(g);
            if(f->rva==0x3a0d8c) kh_fatal(g,"savestate-loader-return: loader loop returned");
        }
    }
    worker=NULL;
}
