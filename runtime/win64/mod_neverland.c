/* mod_neverland.c : Task 19 - the mod's third DLL, NeverlandFly.dll (sha256 1fca202b...7dfb), decoded statically (objdump of the file;
 * no DLL code was executed, natively or emulated).  implementation notes has the listing-level decode; tools/gen_mod_neverland.py re-derives
 * every byte and offset below from the DLL file and writes gen/mod_neverland_table.h (mod-derived, local only).
 *
 * The DLL installs NO detour.  It scans the exe for five signatures and then
 *   (1) exe 0x424570  YS::ACTION_LM::vfn7 (the per-frame motion choice of the swim action; action table index 29, id 0x1d):
 *       entry jump into a 13-byte code cave it writes into the padding behind the function, plus two operand changes.  Motion slots
 *       0 / 1,2 / 0x9a,0x9b (idle / move slow,fast / vertical) become 0x14 / 0x15 / 0x16,0x17: in the mod's P_EX100*.mset these
 *       slots hold the motions named "Hovr", "forw", "up  ", "down";
 *   (2) exe 0x306400  (menu 3D party models): the two calls that zero a model's rotation x and y (0x2ff890, 0x2ff8b0) are replaced
 *       by ONE call of 0x2ffa30 with -25.0f: translation y of the model slot (hud + 0x854 + 0x1c0 * byte[hud + 0xf44 + member]);
 *   (3) a thread: every 250 ms  byte[exe 0x15b962] = (f_2e5380() == 1) ? 0x16 : 0x1c.  That byte is the imm8 of `add ebp,imm8` in
 *       dk::COMMAND_DRAW 0x15b6d0 (the per-row y step of the command menu); f_2e5380 = !(bit 6 of the dword at GAME_SAVE+0x41a4);
 *   (4) printf("flytest\n") once, between the writes and the thread start.
 * There is no world, area, form or input test anywhere in the DLL: whether (1) has any effect is decided by who is in ACTION_LM.
 *
 * Port: (1), (2) and the function of (3) are RE-LIFTED from the patched image by the Task 12 mechanism (Makefile
 * MOD_NEVERLAND_HOOKS; gen/mod_static_lifted.c: mod_f_424570 with the cave as a block, mod_f_306400, mod_f_15b6d0 with the site
 * handler kh_mod_site_15b960 of mod_static_sites.c that fetches the immediate from the image byte).  This file
 *   - owns f_<rva> of the three functions: a counter, then the re-lifted body (nothing guest-visible is added);
 *   - verifies the five signatures on the live image with the DLL's first-match rule, verifies the old bytes of the six edits and
 *     writes them (the image must equal the lifter's input: mod_f_306400 loads the float from image address 0x306784);
 *   - is the thread (3): a host thread, 250 ms period like the DLL's Sleep(250), that reads the save dword and stores the byte.
 * Anything the DLL would not survive (a signature without a match -> store through address 0) is a named fatal here. */
#include "mod_config.h"
#include "mod_neverland_table.h"
#include <time.h>
#include <errno.h>

_Static_assert(GS_BASE==KH_MOD_NF_IMAGE_BASE,"the generated edits hold absolute addresses for this image base");
KhFn kh_lookup(u64 target);
struct kh_mod_static_fn { u32 rva; KhFn mod, orig; int chained; };
extern const struct kh_mod_static_fn kh_mod_neverland_fn_table[]; extern const u32 kh_mod_neverland_fn_count;
void mod_f_424570(KCtx *); void mod_f_306400(KCtx *); void mod_f_15b6d0(KCtx *);

enum { H_424570, H_306400, H_15B6D0, H_N };
static const char *const hook_names[H_N]={"424570 (YS::ACTION_LM::vfn7, flight motion choice)","306400 (menu 3D models, y offset)","15b6d0 (dk::COMMAND_DRAW, row step)"};
static _Atomic u64 hits[H_N], draw_imm[3];                    /* draw_imm: f_15b6d0 entered while the row-step byte was EQ / NE / anything else */
static void enter(int h){ if(atomic_fetch_add(&hits[h],1)==0) fprintf(stderr,"kh: mod neverland %s first call\n",hook_names[h]); }
static u8 *imm_ptr(void){ return (u8*)(gs_host_base+KH_MOD_NF_IMM_RVA); }

/* ---- the three functions: counter, then the body re-lifted from the patched image ------------------------------------------------ */
void f_424570(KCtx *g){ enter(H_424570); mod_f_424570(g); }
void f_306400(KCtx *g){ enter(H_306400); mod_f_306400(g); }
void f_15b6d0(KCtx *g){
    enter(H_15B6D0);
    u8 b=__atomic_load_n(imm_ptr(),__ATOMIC_RELAXED); atomic_fetch_add(&draw_imm[b==KH_MOD_NF_IMM_EQ?0:b==KH_MOD_NF_IMM_NE?1:2],1);
    mod_f_15b6d0(g);
}

/* ---- signatures: the DLL's scan (DLL 0x1070, 0x12d0, 0x15a0): candidates base .. base+size-2, lowest address wins ---------------- */
u32 kh_mod_neverland_scan(KCtx *g,u64 base,u64 size,int sig,u32 *nhits){
    if(sig<0||sig>=KH_MOD_NF_NSIGS) kh_fatal(g,"mod-neverland: signature index %d",sig);
    const struct kh_mod_nf_sig *s=&kh_mod_nf_sigs[sig]; const u8 *m=hp(g,base); u32 first=0, n=0;
    if(s->mask[0]!='x') kh_fatal(g,"mod-neverland: signature %s starts with a wildcard (not the analysed DLL)",s->name);
    for(u64 p=0;p+1<size;){
        const u8 *c=memchr(m+p,s->pattern[0],(size_t)(size-1-p)); if(!c) break;
        p=(u64)(c-m); unsigned k=1;
        while(k<s->len&&(s->mask[k]=='?'||m[p+k]==s->pattern[k])) k++;          /* reads up to len-1 bytes past the last candidate, like the DLL */
        if(k==s->len){ if(!n) first=(u32)p; n++; }
        p++;
    }
    if(nhits) *nhits=n;
    return first;
}
/* -1 when all five signatures have the first match and the match count the generator found in the pinned exe, else the index. */
int kh_mod_neverland_sig_check(KCtx *g,u64 base,u64 size,u32 *first_out,u32 *nhits_out){
    for(int i=0;i<KH_MOD_NF_NSIGS;i++){
        u32 n=0, f=kh_mod_neverland_scan(g,base,size,i,&n);
        if(n!=kh_mod_nf_sigs[i].nhits||f!=kh_mod_nf_sigs[i].first){ if(first_out) *first_out=f; if(nhits_out) *nhits_out=n; return i; }
    }
    return -1;
}
void kh_mod_neverland_sig_verify(KCtx *g,u64 base,u64 size){
    u32 f=0,n=0; int bad=kh_mod_neverland_sig_check(g,base,size,&f,&n);
    if(bad>=0){
        const struct kh_mod_nf_sig *s=&kh_mod_nf_sigs[bad];
        /* the DLL does not test a scan result: S2, S3 and S5 are bases of stores, S1 and S4 are called */
        if(!n) kh_fatal(g,"mod-neverland-signature: %s (%u bytes, mask %s) has no match in the image; NeverlandFly.dll does not check this and would use address 0; nothing was written",s->name,s->len,s->mask);
        kh_fatal(g,"mod-neverland-signature: %s (%u bytes, mask %s): first match 0x%x of %u, the pinned kh2fm.exe has 0x%x of %u; nothing was written",
                 s->name,s->len,s->mask,f,n,s->first,s->nhits);
    }
}

/* ---- the six byte edits ---------------------------------------------------------------------------------------------------------- */
int kh_mod_neverland_nedits(void){ return KH_MOD_NF_NEDITS; }
int kh_mod_neverland_edit(int i,u32 *rva,u32 *len,int *pad,u32 *func,const u8 **oldb,const u8 **newb){
    if(i<0||i>=KH_MOD_NF_NEDITS) return 0;
    const struct kh_mod_nf_edit *e=&kh_mod_nf_edits[i];
    *rva=e->rva; *len=e->len; *pad=e->pad; *func=e->func; *oldb=e->old; *newb=e->new_; return 1;
}
int kh_mod_neverland_check(KCtx *g,u64 base){
    for(int i=0;i<KH_MOD_NF_NEDITS;i++){ const struct kh_mod_nf_edit *e=&kh_mod_nf_edits[i]; if(memcmp(hp(g,base+e->rva),e->old,e->len)) return i; }
    return -1;
}
static void hex(char *out,const u8 *p,unsigned n){ for(unsigned i=0;i<n;i++) sprintf(out+2*i,"%02x",p[i]); out[2*n]=0; }
void kh_mod_neverland_apply(KCtx *g,u64 base){
    int bad=kh_mod_neverland_check(g,base);
    if(bad>=0){
        const struct kh_mod_nf_edit *e=&kh_mod_nf_edits[bad]; char have[80],want[80];
        hex(have,hp(g,base+e->rva),e->len); hex(want,e->old,e->len);
        kh_fatal(g,"mod-neverland-old-bytes: edit %d of %d at rva 0x%x (%u bytes, %s f_%x): the image has %s where the pinned kh2fm.exe (sha256 %.16s...) has %s; nothing was written",
                 bad,KH_MOD_NF_NEDITS,e->rva,(unsigned)e->len,e->pad?"padding behind":"code of",e->func,have,want,KH_MOD_NF_EXE_SHA256);
    }
    for(int i=0;i<KH_MOD_NF_NEDITS;i++){ const struct kh_mod_nf_edit *e=&kh_mod_nf_edits[i]; memcpy(hp(g,base+e->rva),e->new_,e->len); }
}

/* ---- (4) the DLL's printf: stdout, once ------------------------------------------------------------------------------------------ */
void kh_mod_neverland_banner(void){ fputs(KH_MOD_NF_TEXT,stdout); fflush(stdout); }

/* ---- (3) the thread, DLL 0x1290 -------------------------------------------------------------------------------------------------
 *     ebx = 0x16
 *  L: call [S1]                 exe 0x2e5380: sub rsp,28 ; call 0x152380 (lea rax,[GAME_SAVE] ; ret) ; mov eax,[rax+0x41a4] ; shr eax,6 ;
 *                                             not al ; and al,1 ; add rsp,28 ; ret
 *     cmp al,1 ; ecx = 0x1c ; rax = [S5] ; cmove ecx,ebx ; mov [rax+0x292],cl ; Sleep(250) ; jmp L
 * The DLL thread calls the exe function on its own (host) stack.  Here the step reads the same dword directly, so the thread needs
 * no guest stack and adds nothing to the guest arena; kh_mod_neverland_init proves at every start, and the selftest for more
 * values, that this equals the linked f_2e5380 for both states of the bit (a later hook on 0x2e5380 or 0x152380 is caught there). */
static u8 s1_al(void){ u32 v; memcpy(&v,(const void*)(gs_host_base+KH_MOD_NF_SAVE_RVA+KH_MOD_NF_FLAG_OFF),4); return (u8)(~(u8)(v>>KH_MOD_NF_FLAG_BIT))&1; }
static pthread_mutex_t step_m=PTHREAD_MUTEX_INITIALIZER;      /* one step at a time: the thread, or a test that holds the thread */
static _Atomic u64 polls, stores[2], changes, min_gap_ns=~0ull, last_ns;
static _Atomic int started;
static int applied;
u8 kh_mod_neverland_step(void){
    u8 v=s1_al()==1?KH_MOD_NF_IMM_EQ:KH_MOD_NF_IMM_NE, old=__atomic_load_n(imm_ptr(),__ATOMIC_RELAXED);
    __atomic_store_n(imm_ptr(),v,__ATOMIC_RELAXED);
    atomic_fetch_add(&polls,1); atomic_fetch_add(&stores[v==KH_MOD_NF_IMM_NE],1); if(old!=v) atomic_fetch_add(&changes,1);
    return v;
}
void kh_mod_neverland_hold(int on){ if(on) pthread_mutex_lock(&step_m); else pthread_mutex_unlock(&step_m); }
static void *poll_thread(void *arg){
    (void)arg;
    for(;;){
        pthread_mutex_lock(&step_m);
        u64 now=kh_now_ns(), prev=atomic_exchange(&last_ns,now);
        if(prev&&now-prev<atomic_load(&min_gap_ns)) atomic_store(&min_gap_ns,now-prev);
        kh_mod_neverland_step();
        pthread_mutex_unlock(&step_m);
        struct timespec ts={KH_MOD_NF_PERIOD_MS/1000,(long)(KH_MOD_NF_PERIOD_MS%1000)*1000000L};
        while(nanosleep(&ts,&ts)&&errno==EINTR){}
    }
    return NULL;
}
/* review of task 28: with KHNMC_CLOCK=frames the host thread above stepped on HOST time while the virtual clock stood still during loads,
 * and it is no guest thread, so the quiescence test of KHNMC_CLOCK_SYNC=all could not order it: the virtual frame at which the guest first
 * saw a new byte depended on the host.  In that mode the step is driven by the virtual display instead: a vblank listener (vclock.c; it
 * runs on the timing thread inside WaitForVBlank, before the frame that vblank releases) steps at every KH_MOD_NF_VBLANKS-th virtual
 * vblank = every KH_MOD_NF_PERIOD_MS of virtual time, and once at init as the thread's first iteration did.  No host thread then. */
#define KH_MOD_NF_VBLANKS ((u64)KH_MOD_NF_PERIOD_MS*60u/1000u)
_Static_assert((KH_MOD_NF_PERIOD_MS*60u)%1000u==0,"the DLL's period is a whole number of 60 Hz vblanks");
static int by_vblank; static _Atomic u64 first_change_vblank, last_change_vblank;
/* the step rule of the listener; the caller holds step_m */
u8 kh_mod_neverland_vblank_step(u64 vblank,u64 vnow){
    if(vblank%KH_MOD_NF_VBLANKS) return 0;
    u64 prev=atomic_exchange(&last_ns,vnow); if(prev&&vnow-prev<atomic_load(&min_gap_ns)) atomic_store(&min_gap_ns,vnow-prev);
    u64 c0=atomic_load(&changes); u8 v=kh_mod_neverland_step();
    if(atomic_load(&changes)!=c0){ if(!atomic_load(&first_change_vblank)) atomic_store(&first_change_vblank,vblank); atomic_store(&last_change_vblank,vblank); }
    return v;
}
static void nf_vblank(KCtx *g,u64 vblank,u64 vnow){ (void)g; pthread_mutex_lock(&step_m); kh_mod_neverland_vblank_step(vblank,vnow); pthread_mutex_unlock(&step_m); }
int kh_mod_neverland_by_vblank(void){ return by_vblank; }
void kh_mod_neverland_thread_stats(u64 *npolls,u64 *min_gap){ *npolls=atomic_load(&polls); *min_gap=atomic_load(&min_gap_ns); }
int kh_mod_neverland_counts(int hook,u64 *calls){ if(hook<0||hook>=H_N) return 0; *calls=atomic_load(&hits[hook]); return 1; }

/* f_2e5380 as linked (through the dispatch table) against the direct read, for both states of the bit.  Only called while nothing
 * else runs guest code (start-up; selftest with the thread held): the save dword is changed and restored. */
int kh_mod_neverland_s1_agrees(KCtx *g,u32 value,u8 *al_guest,u8 *al_direct){
    u64 p=GS_BASE+KH_MOD_NF_SAVE_RVA+KH_MOD_NF_FLAG_OFF; u32 keep=rd32(g,p);
    wr32(g,p,value);
    u8 a=(u8)kh_call(g,GS_BASE+KH_MOD_NF_S1_RVA,0,0,0,0), d=s1_al();
    wr32(g,p,keep);
    if(al_guest) *al_guest=a; if(al_direct) *al_direct=d;
    return a==d;
}

void kh_mod_neverland_init(KCtx *g){
    if(applied) kh_fatal(g,"mod-neverland-reinit: NeverlandFly's edits are already applied to the image");
    if(kh_mod_neverland_fn_count!=KH_MOD_NF_NFUNCS||KH_MOD_NF_NFUNCS!=H_N)
        kh_fatal(g,"mod-neverland-func-list: gen/mod_static_lifted.c has %u NeverlandFly functions, gen/mod_neverland_table.h lists %d",kh_mod_neverland_fn_count,KH_MOD_NF_NFUNCS);
    static const KhFn own[H_N]={f_424570,f_306400,f_15b6d0}; static const u32 own_rva[H_N]={0x424570,0x306400,0x15b6d0};
    for(u32 i=0;i<kh_mod_neverland_fn_count;i++){
        const struct kh_mod_static_fn *f=&kh_mod_neverland_fn_table[i]; int k=-1;
        for(int j=0;j<H_N;j++) if(own_rva[j]==f->rva) k=j;
        if(f->rva!=kh_mod_nf_funcs[i]||k<0) kh_fatal(g,"mod-neverland-func-list: entry %u is 0x%x in the lifted object and 0x%x in the edit table",i,f->rva,kh_mod_nf_funcs[i]);
        if(!f->chained||f->mod==f->orig||kh_lookup(GS_BASE+f->rva)!=own[k])
            kh_fatal(g,"mod-neverland-link: f_%x in the dispatch table must be this file's counter in front of the re-lifted body (Makefile MOD_NEVERLAND_HOOKS)",f->rva);
    }
    /* the DLL's order is scan, write, scan, write; here everything is checked before the first byte is written */
    kh_mod_neverland_sig_verify(g,GS_BASE,IMG_SIZE);
    if(*imm_ptr()!=KH_MOD_NF_IMM_EQ) kh_fatal(g,"mod-neverland-imm: exe 0x%x holds 0x%02x, the pinned exe has 0x%02x",KH_MOD_NF_IMM_RVA,*imm_ptr(),KH_MOD_NF_IMM_EQ);
    kh_mod_neverland_apply(g,GS_BASE);
    kh_mod_neverland_banner();
    for(int bit=0;bit<2;bit++){
        u8 a,d; u32 v=rd32(g,GS_BASE+KH_MOD_NF_SAVE_RVA+KH_MOD_NF_FLAG_OFF); v=bit?v|(1u<<KH_MOD_NF_FLAG_BIT):v&~(1u<<KH_MOD_NF_FLAG_BIT);
        if(!kh_mod_neverland_s1_agrees(g,v,&a,&d)||d!=(u8)!bit)
            kh_fatal(g,"mod-neverland-s1: exe 0x%x returns AL=%u for save flags 0x%08x, the thread's direct read gives %u (expected %u): the function is no longer the analysed one",
                     KH_MOD_NF_S1_RVA,a,v,d,!bit);
    }
    applied=1;
    if(kh_vclock_on){                                                                  /* KHNMC_CLOCK=frames: the virtual display steps it (above) */
        by_vblank=1; pthread_mutex_lock(&step_m); atomic_store(&last_ns,kh_now_ns()); kh_mod_neverland_step(); pthread_mutex_unlock(&step_m);
        kh_vclock_on_vblank(nf_vblank);
    } else {
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
        int rc=pthread_create(&th,&at,poll_thread,NULL); pthread_attr_destroy(&at);
        if(rc) kh_fatal(g,"mod-neverland-thread: pthread_create failed (%d)",rc);
    }
    atomic_store(&started,1);
    fprintf(stderr,"kh: mod neverland: 5 signatures verified (S1 0x%x S2 0x%x S3 0x%x S4 0x%x S5 0x%x, first of %u), %d edits / %d bytes applied in %d re-lifted functions, row-step byte 0x%x %s (0x%02x / 0x%02x)\n",
            kh_mod_nf_sigs[0].first,kh_mod_nf_sigs[1].first,kh_mod_nf_sigs[2].first,kh_mod_nf_sigs[3].first,kh_mod_nf_sigs[4].first,kh_mod_nf_sigs[4].nhits,
            KH_MOD_NF_NEDITS,KH_MOD_NF_BYTES,KH_MOD_NF_NFUNCS,KH_MOD_NF_IMM_RVA,by_vblank?"stepped at init and every 15 virtual vblanks (KHNMC_CLOCK=frames: 250 ms of virtual time, no host thread)":"polled every 250 ms by a host thread",KH_MOD_NF_IMM_EQ,KH_MOD_NF_IMM_NE);
}

/* exit report (kh_mod_hooks_dump) */
void kh_mod_neverland_dump(void){
    if(!applied){ fprintf(stderr,"kh: mod neverland: NOT applied\n"); return; }
    int kept=0; for(int i=0;i<KH_MOD_NF_NEDITS;i++){ const struct kh_mod_nf_edit *e=&kh_mod_nf_edits[i]; kept+=!memcmp((const void*)(gs_host_base+e->rva),e->new_,e->len); }
    for(int i=0;i<H_N;i++) fprintf(stderr,"kh: mod neverland %s calls=%llu\n",hook_names[i],(unsigned long long)atomic_load(&hits[i]));
    fprintf(stderr,"kh: mod neverland: 15b6d0 entered with row step 0x%02x: %llu, 0x%02x: %llu, other: %llu; %s: %llu polls, stored 0x%02x %llu times and 0x%02x %llu times, %llu changes of the byte, now 0x%02x; %d of %d edits still in the image\n",
            KH_MOD_NF_IMM_EQ,(unsigned long long)atomic_load(&draw_imm[0]),KH_MOD_NF_IMM_NE,(unsigned long long)atomic_load(&draw_imm[1]),(unsigned long long)atomic_load(&draw_imm[2]),by_vblank?"stepped by the virtual display":"thread",
            (unsigned long long)atomic_load(&polls),KH_MOD_NF_IMM_EQ,(unsigned long long)atomic_load(&stores[0]),KH_MOD_NF_IMM_NE,(unsigned long long)atomic_load(&stores[1]),
            (unsigned long long)atomic_load(&changes),*imm_ptr(),kept,KH_MOD_NF_NEDITS);
    if(by_vblank) fprintf(stderr,"kh: mod neverland: by the virtual display: the byte changed first at virtual vblank %llu, last at %llu (0 = only at init or never)\n",(unsigned long long)atomic_load(&first_change_vblank),(unsigned long long)atomic_load(&last_change_vblank));
}
