/* Task 12 selftest: static-edit application (live image + scratch images), the buffer, the re-lifted function table and the two
 * inline detour-site handlers.  Expected handler results are written from the DLL listing (DLL 0x34490 / 0x34460), not from the
 * handlers; the function-level proof of the re-lifted code is the lifter differential (tools/mod_static_difftest.py). */
#include "mod_config.h"
#include <unistd.h>
#include <sys/wait.h>
static int checks, failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod static selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)

KhFn kh_lookup(u64 target);
struct kh_mod_static_fn { u32 rva; KhFn mod, orig; int chained; };
extern const struct kh_mod_static_fn kh_mod_static_fn_table[]; extern const u32 kh_mod_static_fn_count;
extern const u32 kh_mod_dual_slots_image[6];

static int run_fatal(KCtx *g,void (*body)(KCtx *),char *out,size_t cap){
    FILE *log=kh_tmpfile(); if(!log) kh_fatal(g,"mod static selftest: tmpfile failed");
    pid_t child=kh_fork(); if(child<0) kh_fatal(g,"mod static selftest: fork failed");
    if(!child){ if(dup2(fileno(log),STDERR_FILENO)<0) _exit(98); body(g); _exit(99); }
    int status=0; if(waitpid(child,&status,0)!=child) kh_fatal(g,"mod static selftest: waitpid failed");
    rewind(log); size_t n=fread(out,1,cap-1,log); out[n]=0; fclose(log);
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
}
/* call a host function with the guest ABI (return address pushed), like a guest `call` */
static u64 call_fn(KCtx *g,KhFn fn,u64 a0,u64 a1,u64 a2,u64 a3){
    u64 save=KH_RSP(g), sp=((save-0x100)&~0xFull)-8;
    wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    fn(g);
    T(KH_RSP(g)==sp+8);
    u64 r=KH_RAX(g); KH_RSP(g)=save; return r;
}

/* ---- scratch image -------------------------------------------------------------------------------------------------------------- */
static u64 scratch, scratch_nmc, span;
static void scratch_fill_old(KCtx *g){
    u32 rva,len; int kind,rel; const u8 *o,*n;
    for(int i=0;kh_mod_static_edit(i,&rva,&len,&kind,&rel,&o,&n);i++) memcpy(hp(g,scratch+rva),o,len);
    memset(hp(g,scratch_nmc),0,0x1000);
}
static int fatal_edit;
static void fatal_old(KCtx *g){ u32 rva,len; int kind,rel; const u8 *o,*n; kh_mod_static_edit(fatal_edit,&rva,&len,&kind,&rel,&o,&n);
    scratch_fill_old(g); wr8(g,scratch+rva+len-1,(u8)(o[len-1]^0x40)); kh_mod_static_apply(g,scratch,scratch_nmc); }
static void fatal_busy(KCtx *g){ scratch_fill_old(g); wr8(g,scratch_nmc+0xfff,1); kh_mod_static_apply(g,scratch,scratch_nmc); }
static void fatal_reinit(KCtx *g){ kh_mod_static_init(g); }
static void fatal_noslots(KCtx *g){ kh_mod_dual_slots_va=0; kh_mod_site_3c40a9(g); }

static void test_tables(KCtx *g){
    int n=kh_mod_static_nedits(), code,rdata,data; kh_mod_static_counts(&code,&rdata,&data);
    u32 rva,len,nmc_size,init_len; int kind,rel; const u8 *o,*nw,*init;
    u32 nmc_rva=kh_mod_static_nmc(&nmc_size,&init,&init_len); u64 nmc=GS_BASE+nmc_rva;
    T(n==code+rdata+data); T(nmc_rva==IMG_SIZE&&nmc_size==0x1000&&init_len<=nmc_size);
    /* live image: every edit shows its new bytes; the rel32 code edits point into the buffer */
    int seen[3]={0,0,0}, nrel=0; u32 prev_end=0;
    for(int i=0;kh_mod_static_edit(i,&rva,&len,&kind,&rel,&o,&nw);i++){
        T(len>=1&&len<=32&&kind>=0&&kind<=2); if(kind>=0&&kind<=2) seen[kind]++;
        T(rva>=prev_end); prev_end=rva+len;
        T(memcmp(o,nw,len)!=0);
        T(!memcmp(hp(g,GS_BASE+rva),nw,len));
        if(kind==0) T(rva>=0x1000&&rva+len<=0x1000+0x5773f3);           /* .text */
        if(kind==1) T(rva>=0x57b000&&rva+len<=0x57b000+0x19773e);         /* .rdata */
        if(kind==2) T(rva>=0x713000&&rva+len<=0x713000+0x89c00);          /* file-backed part of .data */
        if(rel){ nrel++; T(kind==0&&len==4); s32 d=(s32)rd32(g,GS_BASE+rva); u64 target=GS_BASE+rva+4+(s64)d; T(target>=nmc&&target<nmc+nmc_size); }
    }
    T(seen[0]==code&&seen[1]==rdata&&seen[2]==data); T(nrel==8);
    T(kh_mod_static_check(g,GS_BASE)==0);                                /* applied: the first edit no longer shows its old bytes */
    /* buffer: init bytes, then zero (nothing has run yet that writes it) */
    T(!memcmp(hp(g,nmc),init,init_len));
    { int z=1; for(u32 i=init_len;i<nmc_size;i++) if(rd8(g,nmc+i)) z=0; T(z); }
    T(nmc+nmc_size<=((u64)kh_mod_dual_slots_va)&&kh_mod_dual_slots_va!=0);   /* the slot block lives in the guest arena above the buffer */
    T(!memcmp(hp(g,kh_mod_dual_slots_va),kh_mod_dual_slots_image,24));

    /* scratch image: old bytes -> check passes -> apply writes exactly the edit bytes and the buffer */
    span=((u64)prev_end+0xfff)&~0xfffull; scratch=ga_calloc(1,span); scratch_nmc=ga_calloc(1,0x1000+32)+16;
    T(scratch!=0&&scratch_nmc!=16);
    scratch_fill_old(g);
    T(kh_mod_static_check(g,scratch)==-1);
    kh_mod_static_apply(g,scratch,scratch_nmc);
    u64 written=0;
    for(int i=0;kh_mod_static_edit(i,&rva,&len,&kind,&rel,&o,&nw);i++){ T(!memcmp(hp(g,scratch+rva),nw,len)); written+=len; }
    { u64 nz=0; const u8 *p=hp(g,scratch); for(u64 i=0;i<span;i++) if(p[i]) nz++;          /* nothing outside the edit ranges was touched */
      u64 nz_new=0; for(int i=0;kh_mod_static_edit(i,&rva,&len,&kind,&rel,&o,&nw);i++) for(u32 k=0;k<len;k++) if(nw[k]) nz_new++;
      T(nz==nz_new); T(written==182); }
    T(!memcmp(hp(g,scratch_nmc),init,init_len)); T(rd64(g,scratch_nmc-8)==0&&rd64(g,scratch_nmc+0x1000)==0);
    { int z=1; for(u32 i=init_len;i<nmc_size;i++) if(rd8(g,scratch_nmc+i)) z=0; T(z); }
    T(kh_mod_static_check(g,scratch)==0);                                /* a second application would be refused at edit 0 */
    /* every edit is individually verified: corrupt one byte of edit i (first / last byte alternately) -> check reports i */
    scratch_fill_old(g);
    for(int i=0;kh_mod_static_edit(i,&rva,&len,&kind,&rel,&o,&nw);i++){
        u32 at=(i&1)?len-1:0; u8 keep=rd8(g,scratch+rva+at);
        wr8(g,scratch+rva+at,(u8)(keep^0x01)); T(kh_mod_static_check(g,scratch)==i); wr8(g,scratch+rva+at,keep);
    }
    T(kh_mod_static_check(g,scratch)==-1);
    /* named fatals (child processes; exit status 3 = kh_fatal) */
    char out[8192];
    const int probe[]={0,n/2,n-1};
    if(kh_fork_ok()){
    for(unsigned k=0;k<3;k++){
        fatal_edit=probe[k]; kh_mod_static_edit(fatal_edit,&rva,&len,&kind,&rel,&o,&nw);
        char want[64]; snprintf(want,sizeof want,"mod-static-old-bytes: edit %d of %d at rva 0x%x ",fatal_edit,n,rva);
        T(run_fatal(g,fatal_old,out,sizeof out)==3); T(strstr(out,want)!=NULL); T(strstr(out,"nothing was written")!=NULL);
    }
    T(run_fatal(g,fatal_busy,out,sizeof out)==3); T(strstr(out,"mod-static-nmc-busy:")!=NULL&&strstr(out,"+0xfff")!=NULL);
    T(run_fatal(g,fatal_reinit,out,sizeof out)==3); T(strstr(out,"mod-static-reinit:")!=NULL);
    } else kh_selftest_not_run("mod static","named fatals of the image check: old bytes at three edits (9), busy buffer (2), second init (2)",13);   /* iOS: no fork (host_env.c) */
    ga_free(scratch); ga_free(scratch_nmc-16);
}

/* ---- re-lifted functions: dispatch table and two behavioural probes -------------------------------------------------------------- */
static void test_functions(KCtx *g){
    u64 sp=KH_RSP(g);
    T(kh_mod_static_fn_count==30);
    int chained=0;
    for(u32 i=0;i<kh_mod_static_fn_count;i++){
        const struct kh_mod_static_fn *f=&kh_mod_static_fn_table[i]; KhFn cur=kh_lookup(GS_BASE+f->rva);
        T(f->mod!=NULL&&f->orig!=NULL&&f->mod!=f->orig);
        if(f->chained){ chained++; T(cur!=f->mod&&cur!=f->orig&&cur!=NULL); T(f->rva==0x3a89a0||f->rva==0x3f29e0); }
        else T(cur==f->mod);
        if(i) T(f->rva>kh_mod_static_fn_table[i-1].rva);
    }
    T(chained==2);
    const struct kh_mod_static_fn *f3b1780=NULL,*f3c5900=NULL;
    for(u32 i=0;i<kh_mod_static_fn_count;i++){ if(kh_mod_static_fn_table[i].rva==0x3b1780) f3b1780=&kh_mod_static_fn_table[i]; if(kh_mod_static_fn_table[i].rva==0x3c5900) f3c5900=&kh_mod_static_fn_table[i]; }
    T(f3b1780&&f3c5900); if(!f3b1780||!f3c5900) return;
    /* 0x3b1780: `bt mask,[rcx]` membership, mask 0x7e420 -> 0x74420 (edit 0x3b1789): bits 13 and 15 leave the set.  Through the
     * dispatch table (what every guest caller reaches) and against the vanilla body. */
    u64 cell=ga_calloc(1,16);
    for(u32 v=0;v<24;v++){
        wr32(g,cell,v);
        int mod_want=v<=0x12&&((0x74420u>>v)&1), van_want=v<=0x12&&((0x7e420u>>v)&1);
        KH_RAX(g)=0xaaaaaaaaaaaaaa00ull; u64 r=kh_call(g,GS_BASE+0x3b1780,cell,0,0,0); T((u8)r==(u8)mod_want);
        KH_RAX(g)=0xaaaaaaaaaaaaaa00ull; r=call_fn(g,f3b1780->orig,cell,0,0,0); T((u8)r==(u8)van_want);
        T((mod_want!=van_want)==(v==13||v==15));
    }
    ga_free(cell);
    /* 0x3c5900: `lea rax,[table]; mov rax,[rax+idx*80]; word [rax+0x26]`; the edit (0x3c590d, rel32) moves the table from the
     * vanilla BSS block to buffer+0x148.  Plant an object pointer in the buffer slot: the replacement must find it, the vanilla
     * body must not. */
    u32 nmc_size,init_len; const u8 *init; u64 nmc=GS_BASE+kh_mod_static_nmc(&nmc_size,&init,&init_len);
    u64 obj=ga_calloc(1,0x40); wr16(g,obj+0x26,0xbeef);
    for(u32 idx=0;idx<3;idx++){
        u64 slot=nmc+0x148+80ull*idx, vslot=GS_BASE+0x2a23ac8+80ull*idx, keep=rd64(g,slot), vkeep=rd64(g,vslot);
        wr64(g,slot,obj); wr64(g,vslot,0);
        T(kh_call(g,GS_BASE+0x3c5900,idx,0,0,0)==0xbeef); T(call_fn(g,f3c5900->orig,idx,0,0,0)==0);
        wr64(g,slot,0); wr64(g,vslot,obj);
        T(kh_call(g,GS_BASE+0x3c5900,idx,0,0,0)==0); T(call_fn(g,f3c5900->orig,idx,0,0,0)==0xbeef);
        wr64(g,slot,keep); wr64(g,vslot,vkeep);
    }
    ga_free(obj);
    T(KH_RSP(g)==sp);
}

/* ---- inline detour-site handlers ------------------------------------------------------------------------------------------------- */
static int state_same_except(const KCtx *a,const KCtx *b,int rax_too){
    for(int off=0;off<KH_ST_SIZE;off++){
        if(off>=KO_CC_OP&&off<KO_CC_NDEP+8) continue;
        if(rax_too&&off>=KO_RAX&&off<KO_RAX+8) continue;
        if(a->st[off]!=b->st[off]) return 0;
    }
    return 1;
}
static void test_sites(KCtx *g){
    static KCtx before;
    /* DLL 0x34490: probes 819d4,819d0,819cc,819c4,819c8,819c0 holding ini 0,2,3,0,5,0; RAX = last probed slot.
     * Expected by form: 0 -> first probe (+0x14); 2 -> +0x10; 3 -> +0x0c; 5 -> +0x08 (fifth probe); anything else -> all six, +0x00. */
    const u64 forms[]={0,1,2,3,4,5,6,7,8,0x2d,0xff,0x100,0xffffffffull,0x80000000ull,0x700000002ull,0xffffffff00000005ull,0x100000001ull};
    for(unsigned i=0;i<sizeof forms/sizeof *forms;i++) for(int seed=0;seed<3;seed++){
        u32 f=(u32)forms[i]; int ok=f==0||f==2||f==3||f==5;
        u64 off=f==0?0x14:f==2?0x10:f==3?0x0c:f==5?0x08:0; u32 last=ok?f:0;
        KH_R8(g)=forms[i]; KH_RAX(g)=seed==0?0:seed==1?~0ull:0x1234567800000000ull;
        PUT(u64,KO_CC_OP,(u64)(seed*9)); PUT(u64,KO_CC_DEP1,(u64)0x1111); PUT(u64,KO_CC_DEP2,(u64)0x2222); PUT(u64,KO_CC_NDEP,(u64)0x3333);
        memcpy(&before,g,sizeof before);
        int r=kh_mod_site_3c40a9(g);
        T(r==ok); T(KH_RAX(g)==kh_mod_dual_slots_va+off); T(rd32(g,KH_RAX(g))==last);
        T(GET(u64,KO_CC_OP)==7&&GET(u64,KO_CC_DEP1)==(u64)f&&GET(u64,KO_CC_DEP2)==(u64)last&&GET(u64,KO_CC_NDEP)==0);   /* cmp r8d,[rax] */
        T(calc_cond(4,GET(u64,KO_CC_OP),GET(u64,KO_CC_DEP1),GET(u64,KO_CC_DEP2),GET(u64,KO_CC_NDEP))==(u64)ok);            /* ZF = matched */
        T(KH_R8(g)==forms[i]); T(state_same_except(&before,g,1));
    }
    T(!memcmp(hp(g,kh_mod_dual_slots_va),kh_mod_dual_slots_image,24));
    if(kh_fork_ok()){ char out[4096]; T(run_fatal(g,fatal_noslots,out,sizeof out)==5); T(strstr(out,"dual-wield slot block not initialised")!=NULL); }
    else kh_selftest_not_run("mod static","dual-wield site handler without its slot block (fault 5)",2);   /* iOS: no fork (host_env.c) */

    /* DLL 0x34460.  Expected: ok = (cat==dil && s16[rbx]<s16 di) || (word[rax+c]==1 && s16[rbx]<s16 di); flags = last compare run. */
    u64 info=ga_calloc(1,0x20), ent=ga_calloc(1,0x10);
    const u64 rdis[]={0,1,0x80,0xff,0x100,0x7fff,0x8000,0xffff,0xabcd0000ull,0xffffffffffff0001ull};
    const u8 cats[]={0,1,0x7f,0x80,0xff};
    const u16 words[]={0,1,2,0x7fff,0x8000,0x8001,0xfffe,0xffff};
    const u16 keys[]={0,1,2,0x101,0xffff};
    for(unsigned a=0;a<sizeof rdis/sizeof *rdis;a++) for(unsigned b=0;b<sizeof cats;b++) for(unsigned c=0;c<sizeof words/sizeof *words;c++) for(unsigned d=0;d<sizeof keys/sizeof *keys;d++){
        u16 di=(u16)rdis[a]; u8 dil=(u8)di, cat=cats[b]; u16 w=words[c], k=keys[d];
        memset(hp(g,info),0x5a,0x20); memset(hp(g,ent),0xa5,0x10);
        wr8(g,info+6,cat); wr16(g,info+0xc,k); wr16(g,ent,w);
        KH_RAX(g)=info; KH_RBX(g)=ent; KH_RDI(g)=rdis[a];
        PUT(u64,KO_CC_OP,(u64)0); PUT(u64,KO_CC_DEP1,(u64)0x1111); PUT(u64,KO_CC_DEP2,(u64)0x2222); PUT(u64,KO_CC_NDEP,(u64)0x3333);
        memcpy(&before,g,sizeof before);
        int r=kh_mod_site_360a5a(g);
        int below=(s16)w<(s16)di, pathA=cat==dil&&below, ok=pathA||(k==1&&below);
        T(r==ok);
        u64 op,d1,d2;                      /* last compare on each path of the listing */
        if(pathA){ op=6; d1=w; d2=di; }                          /* cmp word [rbx],di ; jmp ok */
        else if(k!=1){ op=6; d1=k; d2=1; }                        /* cmp word [rax+0xc],1 ; jne skip */
        else { op=6; d1=w; d2=di; }                               /* second cmp word [rbx],di */
        T(GET(u64,KO_CC_OP)==op&&GET(u64,KO_CC_DEP1)==d1&&GET(u64,KO_CC_DEP2)==d2&&GET(u64,KO_CC_NDEP)==0);
        T(state_same_except(&before,g,0));                         /* no register is written, RAX included */
        T(rd8(g,info+6)==cat&&rd16(g,info+0xc)==k&&rd16(g,ent)==w&&rd8(g,info+5)==0x5a&&rd8(g,ent+2)==0xa5);
        /* vanilla predicate (path A only) differs exactly when the extension fires */
        T((ok!=pathA)==(k==1&&below&&cat!=dil));
    }
    ga_free(info); ga_free(ent);
}

int kh_mod_static_selftest(KCtx *g){
    u64 sp=KH_RSP(g);
    test_tables(g);
    test_functions(g);
    test_sites(g);
    T(KH_RSP(g)==sp);
    fprintf(stderr,"kh: mod static selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
