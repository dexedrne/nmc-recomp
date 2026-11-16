/* khrt_dispatch.c : indirect call / jump dispatch (guest address -> function), fault and unimplemented-import handlers. */
#include "khrt_tables.h"
#include <stdio.h>
#include <stdlib.h>

u64 kh_image_size = 0x2c2b000;                    /* SizeOfImage of kh2fm.exe; the loader may overwrite */
u64 kh_import_addr_base = KH_IMPORT_ADDR_BASE_DEFAULT;
static u32 *ht; static u32 hmask; static int ready;

static inline u32 hh(u32 rva){ return (rva*2654435761u)>>8; }
static void build(void){
    u32 n=1; while(n<kh_fn_count*2) n<<=1;
    ht=(u32*)malloc(sizeof(u32)*n); memset(ht,0xff,sizeof(u32)*n); hmask=n-1;
    for(u32 i=0;i<kh_fn_count;i++){ u32 k=hh(kh_fn_table[i].rva)&hmask; while(ht[k]!=0xffffffffu) k=(k+1)&hmask; ht[k]=i; }
    ready=1;
}
KhFn kh_lookup(u64 target){
    if(!ready) build();
    u64 rva=target-KH_IMAGE_BASE;
    if(rva<kh_image_size){
        u32 k=hh((u32)rva)&hmask;
        while(ht[k]!=0xffffffffu){ if(kh_fn_table[ht[k]].rva==(u32)rva) return kh_fn_table[ht[k]].fn; k=(k+1)&hmask; }
        return 0;
    }
    u64 d=target-kh_import_addr_base;
    if(d<16ull*kh_import_count && (d&15)==0) return kh_import_table[d>>4].fn;
    return 0;
}
void kh_dispatch(KCtx *g, u64 target){
    KhFn f=kh_lookup(target);
    if(!f) kh_fault(g,target,"dispatch: not a function start");
    f(g);
}
void kh_fault(KCtx *g, u64 va, const char *kind){
    fprintf(stderr,"kh_fault: %s at guest 0x%llx (rip state 0x%llx)\n",kind,(unsigned long long)va,(unsigned long long)KH_R64(g,KO_RIP));
    abort();
}
void kh_unimplemented_import(KCtx *g, const char *dll, const char *name){
    fprintf(stderr,"kh: unimplemented import %s!%s called\n",dll,name); (void)g; abort();
}
void kh_nep_stub(KCtx *g, u32 rva){ fprintf(stderr,"kh: .nep CLR stub 0x%x called (no CLR in this port)\n",rva); (void)g; abort(); }
