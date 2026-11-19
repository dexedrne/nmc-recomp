/* idtable.c : the engine's 32-bit object ids: report, guard on the registration function, fault diagnostics, selftest.
 *
 * What the original does (kh2fm.exe, read from its disassembly):
 *   The game keeps pointers in 32-bit fields (the PS2 code's structures were kept) as ids:
 *     id = 0x80000000 | slot<<25 | (ptr & 0x1ffffff)          slot = index into a table of 64 window bases, 8 bytes each
 *   table:  image RVA 0x2b0d720 .. 0x2b0d920 (64 x u64, -1 = unused), thread-safe-static guard at 0x2b0d920, filled with -1 by
 *           0x4ad2c0 on first use
 *   0x4ad240 / 0x4ad280  pointer -> id   (1,854 call sites): NULL -> 0; else 0x4ad2c0, then tail-jump to 0x4ad390
 *   0x4ad270 / 0x4ad2b0  id -> pointer   (5,648 call sites): 0 -> NULL; else tail-jump to 0x4ad3f0
 *   0x4ad390  registration + encode (rdx = ptr):
 *       4c 8b ca                 mov  r9,rdx
 *       4c 8d 15 86 03 66 02     lea  r10,[table]
 *       4c 8b c2                 mov  r8,rdx
 *       41 81 e1 ff ff ff 01     and  r9d,0x1ffffff               ; low 25 bits
 *       49 81 e0 00 00 00 fe     and  r8,0xfffffffffe000000       ; REX.W + sign-extended imm32: the full 64-bit window base
 *       ... for eax = 0..63:  rdx = [rcx]; if rdx == -1 goto take; if rdx == r8 goto found; rcx += 8
 *       c1 e0 19 / 41 0b c1 / 0f ba e8 1f / c3     after the loop eax = 64: 64<<25 = 0x80000000, so the id is 0x80000000 | low
 *                                                  = an id of SLOT 0: no check, no error: the 65th window aliases the first
 *       take:  48 63 c8  movsxd rcx,eax ;  4d 89 04 ca  mov [r10+rcx*8],r8     ; a 64-bit store of the base
 *   0x4ad3f0  decode (ecx = id):
 *       0f ba f3 1f              btr  ebx,31
 *       8b c3 / 48 c1 e8 19      mov  eax,ebx ; shr rax,25        ; slot 0..63
 *       81 e1 ff ff ff 01        and  ecx,0x1ffffff
 *       48 8b 04 c2              mov  rax,[rdx+rax*8]             ; a 64-bit load of the base
 *       48 0b c1                 or   rax,rcx                     ; 64-bit or
 *   So the scheme is 64-bit clean: bases are stored, compared and loaded as full 64-bit values and nothing assumes an address
 *   below 4 GiB (every base of this port is above 4 GiB already: the image base is 0x140000000).  What it cannot do is hold more
 *   than 64 windows, and the table has no lock: two threads that register two new windows at the same moment can take one slot.
 *
 * What this file does:
 *   guard   a wrapper hook on 0x4ad390 (the lifted body runs unchanged through orig_f_4ad390).  The common case (the window is
 *           already in the table) is one table read through a 64-entry hint cache.  A new window is registered under a host lock
 *           (closes the two-threads-one-slot race) and recorded (pointer, owner, call site, thread, game frame).  A window that
 *           would be the 65th is a named fatal with the pointer, the call-site RVA and all 64 bases, raised BEFORE the lifted
 *           body runs: no aliased id is ever produced.  KHNMC_IDTABLE_GUARD=0 restores the original behaviour (no lock, no fatal).
 *   report  KHNMC_IDTABLE_REPORT=1: at exit, and with KHNMC_IDTABLE_REPORT_EVERY=n also every n game frames: every slot with its
 *           base, what owns that window (image / id zone / far zone / not guest memory, live bytes per allocation source), who
 *           registered it first; the high-water marks of the table and of the arena zones.  A new window is also logged when it
 *           is registered.
 *   The arena that keeps the number of windows bounded is in core.c (layout comment there). */
#include "khnmc_rt.h"
#include <time.h>

#define PTAB        (GS_BASE+0x2b0d720ull)
#define PTAB_GUARD  (GS_BASE+0x2b0d920ull)
#define NSLOT 64
#define F_PTR2ID    (GS_BASE+0x4ad240ull)
#define F_ID2PTR    (GS_BASE+0x4ad270ull)
void orig_f_4ad390(KCtx *g);
void kh_register_fault_diag(void (*fn)(FILE*));
int ga_zone_get(int zi,u64 *lo,u64 *hi,u64 *bump,u64 *commit,u64 *live);
int ga_spread_on(void);

static int guard_on=1, report_on; static u64 report_every;
static pthread_mutex_t reg_m=PTHREAD_MUTEX_INITIALIZER;
typedef struct { u64 base, ptr, site, frame; u32 tid; int set; char owner[160]; } SlotInfo;
static SlotInfo slot_info[NSLOT];
static u8 hint[NSLOT];                                   /* (base>>25)&63 -> a slot that held that base when last seen; always re-checked against the table */
static _Atomic u64 c_slow, c_new, c_overflow, c_game_frame;
static _Atomic int hw_slots;                             /* most slots ever seen in use */

static u64 tq(u64 a){ u64 v; memcpy(&v,(void*)((uintptr_t)a+gs_bias),8); return v; }
static int slots_used(void){ int n=0; for(int i=0;i<NSLOT;i++) if(tq(PTAB+8ull*i)!=~0ull) n++; return n; }
static int table_live(void){ u32 gd; memcpy(&gd,(void*)((uintptr_t)PTAB_GUARD+gs_bias),4); return gd!=0; }   /* 0x4ad2c0 has run (or a selftest set the table up) */

/* the message of the 65th window: which pointer, from where, and the 64 bases it does not belong to */
static void overflow_text(KCtx *g,u64 ptr,u64 site,char *out,size_t cap){
    char own[200]; ga_describe(ptr,own,sizeof own); u64 b0=tq(PTAB); size_t o=0;
    o+=(size_t)snprintf(out+o,cap-o,"object-id table overflow: pointer 0x%llx (%s) lies in the 32 MiB window 0x%llx, which would be the 65th window of the game's 64-slot "
        "id table (image rva 0x2b0d720). Call-site rva 0x%llx (the caller of the pointer->id function 0x4ad240). The original registration 0x4ad390 has no check: it would return id 0x%08x, "
        "an id of slot 0, which decodes to 0x%llx instead: silent corruption. The 64 bases:",
        (unsigned long long)ptr,own,(unsigned long long)(ptr&~(GA_WIN-1)),(unsigned long long)(site-GS_BASE),(unsigned)(0x80000000u|(u32)(ptr&(GA_WIN-1))),(unsigned long long)(b0|(ptr&(GA_WIN-1))));
    for(int i=0;i<NSLOT&&o+32<cap;i++) o+=(size_t)snprintf(out+o,cap-o," [%d]=%llx",i,(unsigned long long)tq(PTAB+8ull*i));
    if(o+160<cap) snprintf(out+o,cap-o,". KHNMC_IDTABLE_REPORT=1 shows who registered each window; KHNMC_IDTABLE_GUARD=0 restores the original (aliasing) behaviour.");
    (void)g;
}
static void overflow_fatal(KCtx *g,u64 ptr,u64 site){ static char msg[4096]; overflow_text(g,ptr,site,msg,sizeof msg); kh_fatal(g,"%s",msg); }
static void (*overflow_fn)(KCtx *g,u64 ptr,u64 site)=overflow_fatal;     /* the selftest installs a recorder that returns */

static void note_new(KCtx *g,int slot,u64 ptr,u64 base,u64 site){
    SlotInfo *s=&slot_info[slot]; KThread *t=cur_thread();
    s->base=base; s->ptr=ptr; s->site=site; s->frame=atomic_load(&c_game_frame); s->tid=t?t->tid:0; ga_describe(ptr,s->owner,sizeof s->owner); s->set=1;
    atomic_fetch_add(&c_new,1);
    int used=slots_used()+1, hw=atomic_load(&hw_slots); while(used>hw && !atomic_compare_exchange_weak(&hw_slots,&hw,used)){}
    if(report_on) fprintf(stderr,"kh: idtable: slot %d = window 0x%llx, first pointer 0x%llx (%s), call-site rva 0x%llx, thread %u, game frame %llu: %d of 64 slots in use\n",
                          slot,(unsigned long long)base,(unsigned long long)ptr,s->owner,(unsigned long long)(site-GS_BASE),s->tid,(unsigned long long)s->frame,used);
}
static __attribute__((noinline)) void slow_path(KCtx *g,u64 ptr,u64 base){
    u64 site=rd64(g,KH_RSP(g));                          /* 0x4ad240 tail-jumps here: the return address is its caller's */
    atomic_fetch_add(&c_slow,1);
    if(guard_on) pthread_mutex_lock(&reg_m);             /* registrations are serialised: the table write of the lifted body is atomic against other registering threads */
    int slot=-1, freeslot=-1;
    for(int i=0;i<NSLOT;i++){ u64 b=rd64(g,PTAB+8ull*i); if(b==~0ull){ freeslot=i; break; } if(b==base){ slot=i; break; } }   /* the original's scan: first free or first match */
    if(slot>=0) hint[(base>>GA_WIN_SHIFT)&63]=(u8)slot;
    else if(freeslot>=0){ hint[(base>>GA_WIN_SHIFT)&63]=(u8)freeslot; note_new(g,freeslot,ptr,base,site); }
    else { atomic_fetch_add(&c_overflow,1); if(guard_on) overflow_fn(g,ptr,site); }   /* does not return, except for the selftest's recorder */
    orig_f_4ad390(g);
    if(guard_on) pthread_mutex_unlock(&reg_m);
}
void f_4ad390(KCtx *g){                                  /* rdx = pointer (never NULL here); returns the id in eax */
    u64 ptr=KH_RDX(g), base=ptr&~(GA_WIN-1);
    if(__builtin_expect(rd64(g,PTAB+8ull*hint[(base>>GA_WIN_SHIFT)&63])==base,1)){ orig_f_4ad390(g); return; }
    slow_path(g,ptr,base);
}

/* ---- report -------------------------------------------------------------------------------------------------------------------- */
static void report(FILE *f,const char *when){
    int used=table_live()?slots_used():0; u64 lo0=0,hi0=0,bump0=0;
    ga_zone_get(0,&lo0,&hi0,&bump0,NULL,NULL);
    u64 id_end=GS_BASE+(u64)GA_ID_WINDOWS*GA_WIN; int in_range=0, foreign=0;
    fprintf(f,"kh: idtable report (%s): %d of 64 slots in use (most ever %d), guard=%s, registrations through the slow path=%llu, new windows=%llu, windows refused or aliased=%llu, game frame %llu\n",
            when,used,atomic_load(&hw_slots)>used?atomic_load(&hw_slots):used,guard_on?"on":"OFF (original behaviour)",(unsigned long long)c_slow,(unsigned long long)c_new,(unsigned long long)c_overflow,(unsigned long long)c_game_frame);
    if(!table_live()){ fprintf(f,"kh: idtable:   the game has not initialised the table yet\n"); ga_report(f); return; }
    for(int i=0;i<NSLOT;i++){ u64 b=tq(PTAB+8ull*i); if(b==~0ull) continue;
        char where[200], src[200]; const char *kind;
        if(b>=GS_BASE&&b<id_end){ in_range++; kind=b<GS_BASE+((IMG_SIZE+GA_WIN-1)&~(GA_WIN-1))?(b+GA_WIN<=lo0?"image":"image + id zone"):"id zone"; }
        else { foreign++; kind=(b>=id_end&&b<GS_BASE+gs_size)?"far zone":b==0?"not memory (values below 32 MiB)":"not guest memory"; }
        ga_window_sources(b,src,sizeof src);
        const SlotInfo *s=&slot_info[i];
        if(s->set&&s->base==b) snprintf(where,sizeof where,"first: 0x%llx (%s) from rva 0x%llx, thread %u, frame %llu",(unsigned long long)s->ptr,s->owner,(unsigned long long)(s->site-GS_BASE),s->tid,(unsigned long long)s->frame);
        else snprintf(where,sizeof where,"first registration not seen by the guard");
        fprintf(f,"kh: idtable:   slot %2d  base 0x%09llx  %-16s live: %s | %s\n",i,(unsigned long long)b,kind,src[0]?src:"-",where); }
    fprintf(f,"kh: idtable:   %d windows inside [image + id zone] (at most %d by construction), %d outside it (%d slots are spare for those)\n",in_range,GA_ID_WINDOWS,foreign,NSLOT-GA_ID_WINDOWS);
    ga_report(f);
}
/* task 29: the map reports of runtime/route_state.c */
int kh_idtable_slots(int *most){ int used=table_live()?slots_used():0, hw=atomic_load(&hw_slots); if(most) *most=hw>used?hw:used; return used; }
void kh_idtable_report_now(FILE *f,const char *when){ report(f,when); }
void kh_idtable_exit_report(FILE *f){
    if(report_on){ report(f,"exit"); return; }
    if(atomic_load(&c_overflow)) fprintf(f,"kh: idtable: %llu registrations of a 65th window were %s\n",(unsigned long long)c_overflow,guard_on?"refused":"ALIASED to slot 0 (guard off): ids of those windows decode to wrong addresses");
    ga_report(f);                                        /* the zones' high-water marks belong to every exit report */
}
void kh_idtable_game_frame(u64 n){                       /* once per game frame (null_input.c, the 0x105e10 hook) */
    atomic_store(&c_game_frame,n);
    if(report_every && n%report_every==0){ char w[48]; snprintf(w,sizeof w,"game frame %llu",(unsigned long long)n); report(stderr,w); }
}
static void fault_diag(FILE *f){
    if(!table_live()) return;
    fprintf(f,"---- object-id table: %d of 64 slots in use, guard=%s, registrations of a 65th window so far: %llu%s ----\n",slots_used(),guard_on?"on":"OFF",(unsigned long long)c_overflow,
            (!guard_on&&c_overflow)?" (ALIASED to slot 0: ids of those windows decode to wrong addresses)":"");
    for(int i=0;i<NSLOT;i++){ u64 b=tq(PTAB+8ull*i); if(b==~0ull) continue; const SlotInfo *s=&slot_info[i];
        fprintf(f,"  [%2d] %llx",i,(unsigned long long)b); if(s->set&&s->base==b) fprintf(f,"  first 0x%llx (%s) from rva 0x%llx",(unsigned long long)s->ptr,s->owner,(unsigned long long)(s->site-GS_BASE)); fputc('\n',f); }
}
void kh_idtable_init(void){
    const char *e=getenv("KHNMC_IDTABLE_GUARD"); if(e&&*e=='0') guard_on=0;
    e=getenv("KHNMC_IDTABLE_REPORT"); report_on=e&&*e&&*e!='0';
    e=getenv("KHNMC_IDTABLE_REPORT_EVERY"); if(e&&*e){ report_every=strtoull(e,0,0); if(report_every) report_on=1; }
    if(!guard_on) fprintf(stderr,"kh: idtable: guard OFF (original behaviour: no lock on registration, a 65th window aliases slot 0)\n");
    kh_register_fault_diag(fault_diag);
}

/* ==============================================================================================================================
 * selftest: the REAL lifted 0x4ad240 (pointer -> id, which calls the hooked 0x4ad390) and 0x4ad270 (id -> pointer) on a table the
 * test sets up; the game's own table state is saved and restored.
 * ============================================================================================================================ */
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"idtable selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
static struct { int calls; u64 ptr, site; int ok_before; } ovr;
static KCtx *ovr_g; static u64 ovr_seen[NSLOT]; static u32 ovr_ids[NSLOT]; static int ovr_n;
static u32 enc(KCtx *g,u64 p){ return (u32)kh_call(g,F_PTR2ID,p,0,0,0); }
static u64 dec(KCtx *g,u32 id){ return kh_call(g,F_ID2PTR,id,0,0,0); }
static void ov_record(KCtx *g,u64 ptr,u64 site){
    /* at the moment the guard fires nothing is damaged yet: every id handed out so far still decodes to its pointer */
    int ok=1; for(int i=0;i<ovr_n;i++) if(dec(ovr_g,ovr_ids[i])!=ovr_seen[i]) ok=0;
    if(!ovr.calls){ ovr.ptr=ptr; ovr.site=site; ovr.ok_before=ok; }                  /* the FIRST firing is what the checks look at */
    ovr.calls++; (void)g;
}
static void fresh_table(KCtx *g){ for(int i=0;i<NSLOT;i++) wr64(g,PTAB+8ull*i,~0ull); wr32(g,PTAB_GUARD,0x80000000u); }   /* as 0x4ad2c0 leaves it; guard = INT_MIN: every thread takes the fast path */
#define MT_THREADS 8
#define MT_ROUNDS 25
static struct { _Atomic int round, done; u64 ptr[MT_THREADS]; u32 id[MT_THREADS]; } mt;
static void mt_proc(KCtx *g){ int k=(int)A0(g);
    for(int r=1;r<=MT_ROUNDS;r++){ while(atomic_load(&mt.round)<r){ struct timespec ts={0,20000}; nanosleep(&ts,0); } mt.id[k]=enc(g,mt.ptr[k]); atomic_fetch_add(&mt.done,1); }
    KH_RET(g); }
int kh_idtable_selftest(KCtx *g){
    int checks=0,fails=0;
    u64 sv_tab[NSLOT]; memcpy(sv_tab,hp(g,PTAB),sizeof sv_tab); u32 sv_guard=rd32(g,PTAB_GUARD);
    SlotInfo sv_info[NSLOT]; memcpy(sv_info,slot_info,sizeof sv_info); u8 sv_hint[NSLOT]; memcpy(sv_hint,hint,sizeof sv_hint);
    int sv_guard_on=guard_on, sv_report=report_on, sv_hw=atomic_load(&hw_slots); void (*sv_fn)(KCtx*,u64,u64)=overflow_fn;
    u64 c0_new=c_new, c0_ovf=c_overflow, c0_slow=c_slow;
    guard_on=1; report_on=0; overflow_fn=ov_record; ovr_g=g;
    u64 id_lo=0,id_hi=0; ga_zone_get(0,&id_lo,&id_hi,NULL,NULL,NULL);

    /* 1. round trip through the real lifted code at window boundaries: image, both ends of the id zone, the far zone, the top of
     *    the address space.  Every base is above 4 GiB: the test fails on any 32-bit truncation of a base. */
    fresh_table(g);
    CHECK(enc(g,0)==0 && dec(g,0)==0);                                               /* NULL <-> 0, no slot used */
    CHECK(slots_used()==0);
    u64 heap=ga_alloc_src(64,16,GA_SRC_CRT);
    const u64 pts[]={ GS_BASE+0x1000, GS_BASE+GA_WIN-1, GS_BASE+GA_WIN, GS_BASE+0x2b0d720, heap, heap+63,
                      id_hi-GA_WIN, id_hi-1, id_hi, id_hi+GA_WIN-1, id_hi+GA_WIN, GS_BASE+gs_size-1,
                      0x1ull, 0x1ffffffull, 0x2000000ull, 0xffffffffull, 0x100000000ull, 0x7ff7f8000000ull, 0x7fffffffffffull, 0xffff800000000000ull, 0xfffffffffe000000ull, 0xffffffffffffffffull };
    int rt_ok=1, bit_ok=1, low_ok=1, nb=0; u64 bases[32];
    for(unsigned i=0;i<sizeof pts/sizeof*pts;i++){
        u32 id=enc(g,pts[i]); u64 back=dec(g,id); if(back!=pts[i]) rt_ok=0;
        if(!(id&0x80000000u)) bit_ok=0; if((id&0x1ffffffu)!=(u32)(pts[i]&0x1ffffffu)) low_ok=0;
        u32 slot=(id&0x7fffffffu)>>25; if(rd64(g,PTAB+8ull*slot)!=(pts[i]&~(GA_WIN-1))) rt_ok=0;      /* the slot holds the full 64-bit base */
        int seen=0; for(int k=0;k<nb;k++) if(bases[k]==(pts[i]&~(GA_WIN-1))) seen=1; if(!seen) bases[nb++]=pts[i]&~(GA_WIN-1);
        if(enc(g,pts[i])!=id) rt_ok=0; }                                             /* stable: the same pointer gives the same id */
    CHECK(rt_ok); CHECK(bit_ok); CHECK(low_ok);
    CHECK(slots_used()==nb && nb>=14);                                               /* one slot per distinct window, in order of first use */
    CHECK(rd64(g,PTAB)==GS_BASE && rd64(g,PTAB+8)==GS_BASE+GA_WIN && (rd64(g,PTAB)>>32)==1);
    CHECK(enc(g,GS_BASE+0x1000)==(0x80000000u|0x1000u) && enc(g,GS_BASE+GA_WIN+5)==(0x80000000u|(1u<<25)|5u));
    CHECK(c_new-c0_new==(u64)nb && c_overflow==c0_ovf);                              /* the hook saw every new window: lifted 0x4ad240 reaches f_4ad390 */
    { SlotInfo *s=&slot_info[(enc(g,heap)&0x7fffffffu)>>25];                         /* who registered the heap block's window (the block itself unless it shares the image's second window) */
      CHECK(s->set && s->base==(heap&~(GA_WIN-1)) && s->site==0x7ff7ffff0000ull && (s->ptr!=heap || strstr(s->owner,"crt-heap block")!=NULL)); }
    { SlotInfo *s=&slot_info[0]; CHECK(s->set && s->ptr==GS_BASE+0x1000 && strstr(s->owner,"image .text")!=NULL); }
    ga_free(heap);

    /* 2. exactly 64 windows are fine; the 65th is reported with pointer and call site before any id is aliased */
    fresh_table(g); memset(&ovr,0,sizeof ovr); ovr_n=0;
    for(int i=0;i<NSLOT;i++){ u64 p=id_hi+(u64)i*GA_WIN+0x1230+(u64)i; ovr_seen[i]=p; ovr_ids[i]=enc(g,p); ovr_n=i+1; }
    CHECK(slots_used()==64 && ovr.calls==0 && c_overflow==c0_ovf);
    { int ok=1; for(int i=0;i<NSLOT;i++) if(dec(g,ovr_ids[i])!=ovr_seen[i]||((ovr_ids[i]&0x7fffffffu)>>25)!=(u32)i) ok=0; CHECK(ok); }
    CHECK(enc(g,ovr_seen[63]+8)==ovr_ids[63]+8 && enc(g,ovr_seen[0])==ovr_ids[0] && ovr.calls==0);          /* known windows: no report, also when full */
    u64 p65=id_hi+64ull*GA_WIN+0x4560;
    u32 alias=enc(g,p65);
    CHECK(ovr.calls==1 && ovr.ptr==p65 && ovr.site==0x7ff7ffff0000ull && ovr.ok_before==1 && c_overflow==c0_ovf+1);
    CHECK(alias==(0x80000000u|0x4560u) && dec(g,alias)==(ovr_seen[0]&~(GA_WIN-1))+0x4560 && dec(g,alias)!=p65);   /* what the original returns: an id of slot 0, a wrong pointer */
    { char msg[4096]; overflow_text(g,p65,GS_BASE+0x3a0733,msg,sizeof msg); char want[64]; int all=1;
      snprintf(want,sizeof want,"pointer 0x%llx",(unsigned long long)p65); CHECK(strstr(msg,want)!=NULL);
      char own[200]; ga_describe(p65,own,sizeof own);
      CHECK(strstr(msg,"Call-site rva 0x3a0733")!=NULL && strstr(msg,"65th window")!=NULL && strstr(msg,own)!=NULL);
      for(int i=0;i<NSLOT;i++){ snprintf(want,sizeof want," [%d]=%llx",i,(unsigned long long)(id_hi+(u64)i*GA_WIN)); if(!strstr(msg,want)) all=0; } CHECK(all); }
    /* the same with the guard off = the original: no report, the alias is produced silently (this is what the guard prevents) */
    guard_on=0; ovr.calls=0; CHECK(enc(g,p65+0x10)==(0x80000000u|0x4570u) && ovr.calls==0 && c_overflow==c0_ovf+2 && dec(g,0x80000000u|0x4570u)!=p65+0x10); guard_on=1;

    /* 3. the hint cache cannot outlive a rewritten table: the same window is registered again in the fresh table */
    fresh_table(g); u64 wA=id_hi+3*GA_WIN, wB=wA+64ull*GA_WIN;                       /* A and B share a hint entry */
    CHECK(enc(g,wA+1)==(0x80000000u|1u) && enc(g,wB+2)==(0x80000000u|(1u<<25)|2u) && enc(g,wA+3)==(0x80000000u|3u) && enc(g,wB+4)==(0x80000000u|(1u<<25)|4u));
    fresh_table(g);
    CHECK(enc(g,wB+2)==(0x80000000u|2u) && enc(g,wA+1)==(0x80000000u|(1u<<25)|1u) && slots_used()==2 && dec(g,0x80000000u|(1u<<25)|1u)==wA+1);

    /* 4. eight threads register eight new windows at the same moment: eight slots, every id decodes to its pointer */
    atomic_store(&mt.round,0);
    for(int k=0;k<MT_THREADS;k++) kh_spawn_hostfn_thread(g,"idtable_selftest",mt_proc,(u64)k);
    for(int round=1;round<=MT_ROUNDS;round++){
        fresh_table(g); atomic_store(&mt.done,0);
        for(int k=0;k<MT_THREADS;k++){ mt.ptr[k]=id_hi+(u64)(8+k+8*round)*GA_WIN+0x100u*(u64)(k+1); mt.id[k]=0; }
        atomic_store(&mt.round,round);
        for(int i=0;i<50000&&atomic_load(&mt.done)<MT_THREADS;i++){ struct timespec t2={0,100000}; nanosleep(&t2,0); }
        int ok=atomic_load(&mt.done)==MT_THREADS && slots_used()==MT_THREADS; u32 seen=0;
        for(int k=0;k<MT_THREADS&&ok;k++){ if(dec(g,mt.id[k])!=mt.ptr[k]) ok=0; seen|=1u<<((mt.id[k]&0x7fffffffu)>>25); }
        CHECK(ok && seen==0xffu);
    }

    /* 5. KHNMC_HEAP_SPREAD's placement (a window per large block and per heap segment of small blocks).  Large: 70 blocks, 70 windows.
     *    Small: blocks of 60,000 bytes fill the heap segments, one block of each segment is encoded as the game would: the table
     *    fills, the guard fires at the 65th window, and at that moment all 64 earlier ids still decode to their blocks. */
    { extern int kh_ga_spread_test(int on); extern u64 kh_ga_spread_seg(void); u64 seg=kh_ga_spread_seg(); int was=kh_ga_spread_test(1); u64 blk[70]; int distinct=1;
      for(int i=0;i<70;i++){ blk[i]=ga_alloc_src(1100u<<10,16,GA_SRC_CRT); for(int k=0;k<i;k++) if((blk[k]>>GA_WIN_SHIFT)==(blk[i]>>GA_WIN_SHIFT)) distinct=0; }
      enum { NSMALL=40000 }; static u64 small[NSMALL]; u64 rep[80]; int nrep=0, nsmall=0;        /* 5 blocks of 61,440+16 bytes per 320 KiB segment */
      while(nrep<70&&nsmall<NSMALL){ u64 p=small[nsmall++]=ga_alloc_src(60000,16,GA_SRC_CRT); int seen=0; for(int k=0;k<nrep;k++) if((rep[k]>>GA_WIN_SHIFT)==(p>>GA_WIN_SHIFT)) seen=1; if(!seen) rep[nrep++]=p; }
      kh_ga_spread_test(was);
      if(ga_deterministic) fprintf(stderr,"kh: idtable selftest: KH_DET allocates without the stress placement: its checks are skipped\n");
      else {
      CHECK(distinct);
      CHECK(nrep==70 && (u64)nsmall<=70*(seg/61456+1));                                          /* 70 segments, 70 windows: more than the table has slots */
      for(int pass=0;pass<2&&nrep==70;pass++){ const u64 *src=pass?rep:blk; int n=70;
          fresh_table(g); memset(&ovr,0,sizeof ovr); ovr_n=0; int fired_at=-1;
          for(int i=0;i<n;i++){ wr64(g,src[i],0xfeed0000u+(u64)i); u32 id=enc(g,src[i]); if(ovr.calls&&fired_at<0) fired_at=i; if(fired_at<0){ ovr_seen[ovr_n]=src[i]; ovr_ids[ovr_n]=id; ovr_n++; } }
          CHECK(fired_at==64 && ovr.ptr==src[64] && ovr.ok_before==1 && ovr.calls==n-64);
          { int ok=ovr_n==64; for(int i=0;i<64&&ok;i++) if(rd64(g,dec(g,ovr_ids[i]))!=0xfeed0000u+(u64)i) ok=0; CHECK(ok); } }
      }
      for(int i=0;i<70;i++) ga_free(blk[i]); for(int i=0;i<nsmall;i++) ga_free(small[i]); }

    /* restore the game's table and the guard's own state */
    memcpy(hp(g,PTAB),sv_tab,sizeof sv_tab); wr32(g,PTAB_GUARD,sv_guard);
    memcpy(slot_info,sv_info,sizeof sv_info); memcpy(hint,sv_hint,sizeof sv_hint);
    guard_on=sv_guard_on; report_on=sv_report; overflow_fn=sv_fn; atomic_store(&hw_slots,sv_hw);
    atomic_store(&c_new,c0_new); atomic_store(&c_overflow,c0_ovf); atomic_store(&c_slow,c0_slow);
    CHECK(sv_fn==overflow_fatal);                                                    /* outside this test the 65th window is the named fatal */
    fprintf(stderr,"kh: idtable selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

#include "rewind.h"
void kh_rewind_ids(KhRecords *r){
    KH_RECORD(r,slot_info);
    KH_RECORD(r,hint);
    KH_RECORD(r,c_game_frame);
}
