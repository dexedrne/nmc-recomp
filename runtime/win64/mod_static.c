/* mod_static.c : Task 12 - the mod's STATIC edits (KH2Tweaks_Refactor.dll writes 89 byte ranges into the running exe at init).
 *
 * The program here is the VANILLA exe lifted to C, and the loader maps the vanilla file.  So the static edits are applied in two
 * halves that must agree:
 *   data  .rdata/.data tables and the DLL's 4 KiB RW buffer (`.nmc`, the target of 8 rip-relative code edits) are written into the
 *         in-memory image by kh_mod_static_init(), after the PE is mapped and before the CRT entry;
 *   code  the 28 functions whose bytes change (+ the 2 functions with an inline detour site) are RE-LIFTED from the patched image
 *         (tools/mod_static_lift.py -> gen/mod_static_lifted.c) and replace the vanilla lifted bodies through the archive alias
 *         mechanism (Makefile MOD_STATIC_FUNCS); the vanilla bodies stay reachable as orig_f_<rva>.
 * The code bytes in the mapped image are updated too, so the image equals the lifter's input image byte for byte (lifted code
 * does not execute image bytes; this is the consistency half of the old-byte check, and keeps any code that reads .text exact).
 *
 * The edit table is generated from the mod's decoded edit list (tools/gen_mod_static.py -> gen/mod_static_table.h, local only).
 * Every edit's old bytes are verified before ANYTHING is written; a mismatch is a named fatal.  The 2 runtime-only edits (script
 * syscall table pointer at 0x7534a8) belong to mod_syscalls.c. */
#include "mod_config.h"
#include "mod_static_table.h"

KhFn kh_lookup(u64 target);
u64 ga_arena_lo(void);
extern u64 kh_mod_dual_slots_va; extern const u32 kh_mod_dual_slots_image[6];
struct kh_mod_static_fn { u32 rva; KhFn mod, orig; int chained; };
extern const struct kh_mod_static_fn kh_mod_static_fn_table[]; extern const u32 kh_mod_static_fn_count;

static int applied;
static const char *const kind_name[3]={".text",".rdata",".data"};

int kh_mod_static_nedits(void){ return KH_MOD_STATIC_NEDITS; }
int kh_mod_static_edit(int i,u32 *rva,u32 *len,int *kind,int *rel32,const u8 **oldb,const u8 **newb){
    if(i<0||i>=KH_MOD_STATIC_NEDITS) return 0;
    const struct kh_mod_static_edit *e=&kh_mod_static_edits[i];
    *rva=e->rva; *len=e->len; *kind=e->kind; *rel32=e->rel32; *oldb=e->old; *newb=e->new_; return 1;
}
void kh_mod_static_counts(int *code,int *rdata,int *data){ *code=KH_MOD_STATIC_NCODE; *rdata=KH_MOD_STATIC_NRDATA; *data=KH_MOD_STATIC_NDATA; }
u32 kh_mod_static_nmc(u32 *size,const u8 **init,u32 *init_len){ *size=KH_MOD_NMC_SIZE; *init=kh_mod_nmc_init; *init_len=KH_MOD_NMC_INIT_LEN; return KH_MOD_NMC_RVA; }

static void hex(char *out,const u8 *p,unsigned n){ for(unsigned i=0;i<n;i++) sprintf(out+2*i,"%02x",p[i]); out[2*n]=0; }

/* Index of the first edit whose old bytes are not at base+rva, or -1 when the whole table matches.  Writes nothing. */
int kh_mod_static_check(KCtx *g,u64 base){
    for(int i=0;i<KH_MOD_STATIC_NEDITS;i++){
        const struct kh_mod_static_edit *e=&kh_mod_static_edits[i];
        if(memcmp(hp(g,base+e->rva),e->old,e->len)) return i;
    }
    return -1;
}
/* Verify every edit, verify the buffer page is unused, then write.  `base` is the guest address of the image (GS_BASE in the
 * real run; a scratch block in the selftest) and `nmc` the guest address of the 4 KiB buffer. */
void kh_mod_static_apply(KCtx *g,u64 base,u64 nmc){
    int bad=kh_mod_static_check(g,base);
    if(bad>=0){
        const struct kh_mod_static_edit *e=&kh_mod_static_edits[bad]; char have[80],want[80];
        hex(have,hp(g,base+e->rva),e->len); hex(want,e->old,e->len);
        kh_fatal(g,"mod-static-old-bytes: edit %d of %d at rva 0x%x (%s, option %s, %u bytes): the image has %s where the pinned kh2fm.exe (sha256 %.16s...) has %s; nothing was written",
                 bad,KH_MOD_STATIC_NEDITS,e->rva,kind_name[e->kind],e->group,(unsigned)e->len,have,want,KH_MOD_STATIC_EXE_SHA256);
    }
    const u8 *page=hp(g,nmc);
    for(u32 i=0;i<KH_MOD_NMC_SIZE;i++) if(page[i])
        kh_fatal(g,"mod-static-nmc-busy: guest 0x%llx (+0x%x in the 4 KiB mod buffer) holds 0x%02x before the buffer is installed; nothing was written",(unsigned long long)(nmc+i),i,page[i]);
    for(int i=0;i<KH_MOD_STATIC_NEDITS;i++){ const struct kh_mod_static_edit *e=&kh_mod_static_edits[i]; memcpy(hp(g,base+e->rva),e->new_,e->len); }
    memcpy(hp(g,nmc),kh_mod_nmc_init,KH_MOD_NMC_INIT_LEN);
}

void kh_mod_static_init(KCtx *g){
    if(applied) kh_fatal(g,"mod-static-reinit: the static edits are already applied to the image");
    /* the re-lifted object and the edit table must describe the same function set, and the dispatch table must reach the replacements */
    if(kh_mod_static_fn_count!=KH_MOD_STATIC_NFUNCS)
        kh_fatal(g,"mod-static-func-list: gen/mod_static_lifted.c has %u functions, gen/mod_static_table.h lists %d",kh_mod_static_fn_count,KH_MOD_STATIC_NFUNCS);
    for(u32 i=0;i<kh_mod_static_fn_count;i++){
        const struct kh_mod_static_fn *f=&kh_mod_static_fn_table[i];
        if(f->rva!=kh_mod_static_funcs[i]) kh_fatal(g,"mod-static-func-list: entry %u is 0x%x in the lifted object and 0x%x in the edit table",i,f->rva,kh_mod_static_funcs[i]);
        KhFn cur=kh_lookup(GS_BASE+f->rva);
        if(f->mod==f->orig||!cur) kh_fatal(g,"mod-static-link: f_%x has no distinct re-lifted replacement",f->rva);
        if(!f->chained&&cur!=f->mod) kh_fatal(g,"mod-static-link: f_%x in the dispatch table is not the re-lifted replacement (Makefile MOD_STATIC_FUNCS)",f->rva);
        if(f->chained&&(cur==f->mod||cur==f->orig)) kh_fatal(g,"mod-static-link: f_%x is marked chained but no runtime hook owns it",f->rva);
    }
    u64 nmc=GS_BASE+KH_MOD_NMC_RVA;
    if(KH_MOD_NMC_RVA!=IMG_SIZE||nmc+KH_MOD_NMC_SIZE>ga_arena_lo())
        kh_fatal(g,"mod-static-nmc-range: buffer 0x%llx+0x%x must sit right after the image (0x%llx) and below the guest arena (0x%llx)",
                 (unsigned long long)nmc,KH_MOD_NMC_SIZE,(unsigned long long)(GS_BASE+IMG_SIZE),(unsigned long long)ga_arena_lo());
    kh_mod_static_apply(g,GS_BASE,nmc);
    kh_mod_dual_slots_va=ga_alloc(sizeof kh_mod_dual_slots_image,16);
    if(!kh_mod_dual_slots_va) kh_fatal(g,"mod-static: guest allocation of the dual-wield slot block failed");
    memcpy(hp(g,kh_mod_dual_slots_va),kh_mod_dual_slots_image,sizeof kh_mod_dual_slots_image);
    extern void kh_tweak_static_options(KCtx *,u64);
    kh_tweak_static_options(g,nmc);
    applied=1;
    fprintf(stderr,"kh: mod static: %d edits verified and applied (%d code / %d .rdata / %d .data; %d+%d+%d bytes), buffer 0x%llx+0x%x (%d init bytes), %d re-lifted functions linked\n",
            KH_MOD_STATIC_NEDITS,KH_MOD_STATIC_NCODE,KH_MOD_STATIC_NRDATA,KH_MOD_STATIC_NDATA,KH_MOD_STATIC_CODE_BYTES,KH_MOD_STATIC_RDATA_BYTES,KH_MOD_STATIC_DATA_BYTES,
            (unsigned long long)nmc,KH_MOD_NMC_SIZE,KH_MOD_NMC_INIT_LEN,KH_MOD_STATIC_NFUNCS);
}

/* Exit report (identity of the image is all that is read here; called from kh_mod_hooks_dump).  An edit whose bytes changed after
 * load was rewritten by the running game, exactly as it would be on the PC: e.g. 0x718c18 (camera table) is assigned by the
 * exe's own dynamic initialiser 0x3e40 during CRT start-up, after the mod has patched it. */
void kh_mod_static_dump(void){
    if(!applied){ fprintf(stderr,"kh: mod static: NOT applied\n"); return; }
    int kept=0,changed=0; char list[512]; size_t o=0; list[0]=0;
    for(int i=0;i<KH_MOD_STATIC_NEDITS;i++){
        const struct kh_mod_static_edit *e=&kh_mod_static_edits[i]; const u8 *cur=(const u8*)(gs_host_base+e->rva);
        if(!memcmp(cur,e->new_,e->len)) kept++;
        else { changed++; char now[80]; hex(now,cur,e->len); if(o+100<sizeof list) o+=(size_t)snprintf(list+o,sizeof list-o," 0x%x(%s now %s)",e->rva,kind_name[e->kind],now); }
    }
    const u8 *page=(const u8*)(gs_host_base+KH_MOD_NMC_RVA); u32 init_changed=0,extra=0;
    for(u32 i=0;i<KH_MOD_NMC_SIZE;i++){ u8 want=i<KH_MOD_NMC_INIT_LEN?kh_mod_nmc_init[i]:0; if(page[i]!=want){ if(i<KH_MOD_NMC_INIT_LEN) init_changed++; else extra++; } }
    fprintf(stderr,"kh: mod static: at exit %d of %d edits still hold the mod bytes, %d rewritten by the game:%s; buffer: %u init bytes changed, %u bytes written beyond them\n",
            kept,KH_MOD_STATIC_NEDITS,changed,changed?list:" none",init_changed,extra);
}
