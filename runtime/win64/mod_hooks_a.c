/* Task 9 batch A: shipped KH2Tweaks_Refactor function detours, derived from a fresh static objdump of the shipped DLL
 * (sha256 84b43f08...ca92) against the original PE.  Mechanism: strong f_<rva> replaces the lifted function (archive alias hooks,
 * Makefile MOD_A_HOOKS / PHASE_HOOKS) and orig_f_<rva> is the untouched lifted body.  DLL .text RVA 0x1000 = file offset 0x400.
 *
 *   DLL 0x2da30 -> exe 0x2fc860  dual-wield form predicate (ini dualwield_forms=0,2,3,0,5,0)
 *   DLL 0x2d8f0 -> exe 0x2fc4e0  dual-wield form mask
 *   DLL 0x2de50 -> exe 0x4014a0  form support-ability list (full replacement; local helper DLL 0x2dd60)
 *   DLL 0x32780 -> exe 0x3a7cb0  solo_summon_fix placement wrapper
 *   DLL 0x2d190 -> exe 0x29efa0  shopface_fix path builder (full replacement)
 *   DLL 0x333b0 -> exe 0x405440  map-event types 0x7f / 0x3e8 / 0x3e9, then the original
 *   DLL 0x32710 -> exe 0x3a89a0  common script trigger 0x65, then the original
 *   DLL 0x32580 -> exe 0x3d61d0  script trigger 0x13, then the original
 *   DLL 0x32680 -> exe 0x3d60c0  script trigger 6, then the original
 * The DLL reaches the engine through signature-scanned function slots; every slot was re-derived by scanning the original PE with
 * the DLL's own pattern/mask bytes (see implementation notes for the slot -> exe RVA table).  Logging calls in the DLL carry no game state and
 * are not reproduced.  A DLL path that would fault on a real PC (null script, null ability entry, null name entry) is a named fatal. */
#include "mod_config.h"

enum { H_2FC860, H_2FC4E0, H_4014A0, H_3A7CB0, H_29EFA0, H_405440, H_3A89A0, H_3D61D0, H_3D60C0, H_N };
static _Atomic u64 hits[H_N], extensions[H_N], shopface_missing;
static const char *const hook_names[H_N]={"2fc860","2fc4e0","4014a0","3a7cb0","29efa0","405440","3a89a0","3d61d0","3d60c0"};
/* one stderr line on the first call of each hook, so a fault dump shows which detours had run */
static void enter(int h){ if(atomic_fetch_add(&hits[h],1)==0) fprintf(stderr,"kh: mod A %s first call\n",hook_names[h]); }

void orig_f_405440(KCtx *); void orig_f_3d61d0(KCtx *);
void mod_f_3a89a0(KCtx *);   /* Task 12: static edit 0x3a8a4b; the Detours trampoline of the DLL runs the PATCHED body */
void orig_f_3d60c0(KCtx *); void orig_f_3a7cb0(KCtx *);
static KhFn fwd_override[KHF_N];
static KhFn fwd_default[KHF_N]={orig_f_405440,mod_f_3a89a0,orig_f_3d61d0,orig_f_3d60c0,orig_f_3a7cb0};
static void forward(KCtx *g,int which){ (fwd_override[which]?fwd_override[which]:fwd_default[which])(g); }

static const u32 ext_rva[KHA_N]={
    0x4ad240,0x4ad270,0x3eafc0,0x3e1840,0x3e1730,0x3e1410,0x3e1510,0x3b4f80,0x152380,0x3ddf00,0x3b3e80,0x3bde30,
    0x3d74e0,0x3d78d0,0x3ba9a0,0x409b00,0x3c6dc0,0x3c8310,0x3b5fa0,0x1518e0,0x3e39a0,0x401730,0x3e3180,0x3dfeb0};
static u64 ext_target[KHA_N];
void kh_mod_a_set_target(int ext,u64 target){ if(ext>=0&&ext<KHA_N) ext_target[ext]=target; }
void kh_mod_a_set_fwd(int fwd,KhFn fn){ if(fwd>=0&&fwd<KHF_N) fwd_override[fwd]=fn; }
static u64 xaddr(int e){ return ext_target[e]?ext_target[e]:GS_BASE+ext_rva[e]; }
static u64 xcall(KCtx *g,int e,u64 a,u64 b,u64 c,u64 d){ return kh_call(g,xaddr(e),a,b,c,d); }
static u64 xcall5(KCtx *g,int e,u64 a,u64 b,u64 c,u64 d,u64 f){ return kh_call5(g,xaddr(e),a,b,c,d,f); }
static void xmm_f(KCtx *g,int i,float f){ v128 o=VZ; memcpy(&o,&f,4); KH_XMM(g,i)=o; }
static void xmm_d(KCtx *g,int i,double f){ v128 o=VZ; memcpy(&o,&f,8); KH_XMM(g,i)=o; }
/* Scratch frame below the hook's own stack pointer: the DLL keeps its argument slots in its local frame.  kh_call places its own
 * frame 0x100 below the current rsp, so lowering rsp by 0x80 keeps the slots clear of every nested call. */
#define FRAME_DROP 0x80ull
#define FRAME_BUF(sp) ((sp)-FRAME_DROP+0x30)

/* ---- DLL 2da30 -> exe 2fc860 ------------------------------------------------------------------------------------------------ */
/* cmp ecx,[six ini slots]; sete al / mov al,1.  Only AL is written: RAX bits 8..63 survive.  Slots (dualwield_forms=0,2,3,0,5,0). */
u32 kh_dual_forms[6]={0,2,3,0,5,0};
void f_2fc860(KCtx *g){
    enter(H_2FC860);
    u32 form=(u32)A0(g); u32 in=0;
    for(int i=0;i<6;i++) if(form==kh_dual_forms[i]) in=1;
    if(in) extensions[H_2FC860]++;
    KH_RAX(g)=(KH_RAX(g)&~255ull)|in;
    KH_RET(g);
}
/* bit0 when any slot is 0, bit k (1..6) when any slot equals k; RAX = 32-bit result (upper half cleared by the DLL's eax writes). */
void f_2fc4e0(KCtx *g){
    enter(H_2FC4E0);
    u32 mask=0;
    for(int i=0;i<6;i++) if(kh_dual_forms[i]==0) mask|=1;
    for(u32 k=1;k<=6;k++) for(int i=0;i<6;i++) if(kh_dual_forms[i]==k) mask|=1u<<k;
    KH_RAX(g)=mask;
    KH_RET(g);
}

/* ---- DLL 2de50 -> exe 4014a0 (full replacement) ------------------------------------------------------------------------------ */
/* DLL 2dd60: for each of five 4-ability groups store the first present level (lowest bit first, NOT the highest as in the
 * vanilla table walk) at this+0x20..0x24; a group with no ability present leaves its byte untouched. */
static void ability_levels(KCtx *g,u64 t){
    u8 b0=rd8(g,t),b1=rd8(g,t+1),b2=rd8(g,t+2),b16=rd8(g,t+0x16),b17=rd8(g,t+0x17);
    int v;
    v=(b0&0x04)?1:(b0&0x08)?2:(b0&0x10)?3:(b0&0x20)?4:0; if(v) wr8(g,t+0x20,v);
    v=(b0&0x40)?1:(b0&0x80)?2:(b1&0x01)?3:(b1&0x02)?4:0; if(v) wr8(g,t+0x21,v);
    v=(b1&0x04)?1:(b1&0x08)?2:(b1&0x10)?3:(b1&0x20)?4:0; if(v) wr8(g,t+0x22,v);
    v=(b1&0x40)?1:(b1&0x80)?2:(b2&0x01)?3:(b2&0x02)?4:0; if(v) wr8(g,t+0x23,v);
    v=(b16&0x20)?1:(b16&0x40)?2:(b16&0x80)?3:(b17&0x01)?4:0; if(v) wr8(g,t+0x24,v);
}
/* 15-bit sign decode of an entry with bit 15 set: add cx,cx / movsx / sar 1. */
static s32 ability_decode(u16 e){ return (s32)(s16)(u16)(e<<1)>>1; }
void f_4014a0(KCtx *g){
    extern int kh_patch_ability_categories; void orig_f_4014a0(KCtx *);
    if(!kh_patch_ability_categories){orig_f_4014a0(g);return;}
    enter(H_4014A0);
    u64 list=A0(g), sheet=rd64(g,list+0x28);
    if(!sheet) kh_fatal(g,"mod 4014a0: ability list %llx has no actor block at +28 (the DLL would fault)",(unsigned long long)list);
    u64 first=xcall(g,KHA_DECODE,rd32(g,sheet+0x250),0,0,0);
    u64 second=xcall(g,KHA_DECODE,rd32(g,sheet+0x254),0,0,0);
    if(first) for(int i=0;i<0x50;i++){
        u16 e=rd16(g,first+0x54+2*(u64)i);
        if(!(e&0x8000)) continue;
        s32 id=ability_decode(e);
        /* The DLL looks the entry up twice and discards the first result: a pure table search. */
        u64 info=xcall(g,KHA_ABILITY_INFO,(u32)id,0,0,0);
        if(!info) kh_fatal(g,"mod 4014a0: no ability table entry for decoded id %d (entry 0x%04x); the DLL would fault",id,e);
        int add=rd16(g,info+0xc)==1;
        if(add) extensions[H_4014A0]++;
        else add=rd8(g,info+6)==0 || !second;
        if(add) xcall(g,KHA_ABILITY_ADD,list,(u32)id,0,0);
    }
    if(second) for(int i=0;i<0x18;i++){
        u16 e=rd16(g,second+8+2*(u64)i);
        if(!(e&0x8000)) continue;
        s32 id=ability_decode(e);
        if(id==0) continue;
        xcall(g,KHA_ABILITY_ADD,list,(u32)id,0,0);
    }
    ability_levels(g,list);
    KH_RET(g);   /* full replacement: pop the return address like the original's ret */
}

/* ---- DLL 32780 -> exe 3a7cb0 (solo_summon_fix) --------------------------------------------------------------------------------- */
void f_3a7cb0(KCtx *g){
    extern int kh_solo_summon_fix; void orig_f_3a7cb0(KCtx *);
    if(!kh_solo_summon_fix){orig_f_3a7cb0(g);return;}
    enter(H_3A7CB0);
    u64 rcx=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    v128 xs[4]; for(int i=0;i<4;i++) xs[i]=KH_XMM(g,i);
    u64 save=xcall(g,KHA_SAVE,0,0,0,0);
    if(rd8(g,save+0x3525)){
        extensions[H_3A7CB0]++;
        KH_XMM(g,1)=xs[3];
        xcall(g,KHA_SUMMON,r8,0,0,0);
    }
    for(int i=0;i<4;i++) KH_XMM(g,i)=xs[i];
    KH_RCX(g)=rcx; KH_RDX(g)=(u32)rdx; KH_R8(g)=r8; KH_R9(g)=r9;
    forward(g,KHF_3A7CB0);
}

/* ---- DLL 2d190 -> exe 29efa0 (shopface_fix, full replacement) ------------------------------------------------------------------ */
void f_29efa0(KCtx *g){
    extern int kh_shopface_fix; void orig_f_29efa0(KCtx *);
    if(!kh_shopface_fix){orig_f_29efa0(g);return;}
    enter(H_29EFA0);
    u32 id=(u32)A0(g); u64 out=A1(g);
    u32 selector=(u32)xcall(g,KHA_SHOP_SEL,id,0,0,0);
    u64 entry=xcall(g,KHA_NAME_ENTRY,selector,0,0,0);
    if(!entry) kh_fatal(g,"mod 29efa0: no shop-face name entry for id 0x%x (selector 0x%x); the DLL would fault",id,selector);
    char name[33];
    for(int i=0;i<32;i++){ u8 c=rd8(g,entry+8+(u64)i); if((u8)(c-0x41)<=0x19) c+=0x20; name[i]=(char)c; }
    name[32]=0;
    char path[0x104];
    int n=snprintf(path,sizeof path,"menu/shopface/%s.bin",name);
    if(n<0||n>=(int)sizeof path) kh_fatal(g,"mod 29efa0: shop-face path overflows 0x104 bytes");
    memcpy(hp(g,out),path,(size_t)n+1);
    extensions[H_29EFA0]++;
    /* Counter only: the file layer resolves the same relative path loose-first (data/menu/shopface/<name>.bin). */
    const char *root=getenv("KHNMC_DATA"); char host[4096];
    if(!kh_resolve_ci(root?root:"data",path,host,sizeof host)) shopface_missing++;
    KH_RET(g);   /* full replacement: pop the return address like the original's ret */
}

/* ---- DLL 333b0 -> exe 405440 (map-event types) ------------------------------------------------------------------------------- */
void kh_phase_mapevent_entry(void);
void f_405440(KCtx *g){
    enter(H_405440);
    kh_phase_mapevent_entry();
    u64 self=A0(g),ev=A1(g),r8=A2(g),r9=A3(g);
    s32 type=(s16)rd16(g,ev+2);
    if(type==0x7f){
        s32 arg=(s16)rd16(g,ev+4);
        if(arg>=0x7d00){ extensions[H_405440]++; xcall(g,KHA_SHOP_ARG,(u32)arg,0,0,0); }
    } else if(type==0x3e9){
        extensions[H_405440]++;
        u64 globals=xcall(g,KHA_GLOBALS,self,0,0,0);
        u64 vec=rd64(g,globals+0xdc8);
        xcall(g,KHA_VEC_PREP,vec+4,0,0,0);
        u64 entry=xcall(g,KHA_FIND,self,(u32)(s32)(s16)rd16(g,ev+4),0,0);
        if(entry) xcall(g,KHA_APPLY,self,entry,vec+4,0);
    } else if(type==0x3e8){
        u8 b18c=rd8(g,self+0x18c),b18d=rd8(g,self+0x18d),b194=rd8(g,self+0x194);
        if(((b18c|b18d)&4)||(b18c&8)||(b194&0x20)){
            extensions[H_405440]++;
            u64 gate=xcall(g,KHA_GATE,self,0,0,0);          /* the DLL tests the whole RAX, not AL */
            xcall(g,KHA_ENCODE,self,0,0,0);                  /* result unused */
            s32 w=(s16)rd16(g,ev+4);
            if(w!=0){
                s32 op_a,op_b;
                if(gate){ xmm_d(g,0,30.0); xcall(g,KHA_MOTION,0,self,0,0); op_a=(s32)((u32)w+1); op_b=4; }
                else { op_a=w; op_b=0; }
                xmm_f(g,2,0); xmm_f(g,3,0);
                xcall(g,KHA_OP_A,self+0x158,(u32)op_a,0,0);
                xmm_f(g,2,0); xmm_f(g,3,0);
                xcall5(g,KHA_OP_B,self+0x158,(u32)op_b,0,0,0);
            }
            if(rd8(g,ev+0x2f)==1) xcall(g,KHA_SHOP_ACT,self,self,(u32)(s32)(s16)rd16(g,ev),0);
        }
    }
    KH_RCX(g)=self; KH_RDX(g)=ev; KH_R8(g)=r8; KH_R9(g)=r9;
    forward(g,KHF_405440);
}

/* ---- DLL 32710 -> exe 3a89a0 (script trigger 0x65) ---------------------------------------------------------------------------- */
void f_3a89a0(KCtx *g){
    enter(H_3A89A0);
    u64 self=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    xcall(g,KHA_ACTOR_PRE,self,rdx,r8,r9);
    u64 script=xcall(g,KHA_SCRIPT_NEW,rd64(g,GS_BASE+0x2a0f688),0,0,0);
    if(!script) kh_fatal(g,"mod 3a89a0: common script allocation (3e1730) returned null; the DLL would fault in trigger 0x65");
    extensions[H_3A89A0]++;
    xcall5(g,KHA_SCRIPT_RUN,script,0x65,0,0,0);
    xcall(g,KHA_SCRIPT_RESET,script,0,0,0);
    xcall(g,KHA_SCRIPT_FREE,script,0,0,0);
    KH_RCX(g)=self; KH_RDX(g)=rdx; KH_R8(g)=r8; KH_R9(g)=r9;
    forward(g,KHF_3A89A0);
}

/* ---- DLL 32580 -> exe 3d61d0 (script trigger 0x13) ---------------------------------------------------------------------------- */
void f_3d61d0(KCtx *g){
    enter(H_3D61D0);
    u64 self=A0(g),p1=A1(g),p2=A2(g),r9=A3(g),sp=KH_RSP(g);
    u64 ref1=rd64(g,p1),ref2=rd64(g,p2);
    u64 slots=FRAME_BUF(sp);
    KH_RSP(g)=sp-FRAME_DROP;
    memset(hp(g,slots),0,32);
    wr32(g,slots,(u32)xcall(g,KHA_ENCODE,ref1,0,0,0));
    u64 a=xcall(g,KHA_REF_ACTOR,rd32(g,ref1+0x18),rd32(g,ref1+0x1c),0,0);
    wr32(g,slots+8,(u32)xcall(g,KHA_ENCODE,a,0,0,0));
    wr32(g,slots+16,(u32)xcall(g,KHA_ENCODE,ref2,0,0,0));
    u64 b=xcall(g,KHA_REF_ACTOR,rd32(g,ref2+0x18),rd32(g,ref2+0x1c),0,0);
    wr32(g,slots+24,(u32)xcall(g,KHA_ENCODE,b,0,0,0));
    u64 script=rd64(g,self+0x5b0);
    if(script){ extensions[H_3D61D0]++; xcall5(g,KHA_SCRIPT_RUN,script,0x13,slots,4,0); }
    KH_RSP(g)=sp;
    KH_RCX(g)=self; KH_RDX(g)=p1; KH_R8(g)=p2; KH_R9(g)=r9;
    forward(g,KHF_3D61D0);
}

/* ---- DLL 32680 -> exe 3d60c0 (script trigger 6) ------------------------------------------------------------------------------- */
void f_3d60c0(KCtx *g){
    enter(H_3D60C0);
    u64 self=A0(g),arg=A1(g),r8=A2(g),r9=A3(g),sp=KH_RSP(g);
    if(rd64(g,self+0x5b0)){
        u64 slot=FRAME_BUF(sp);
        KH_RSP(g)=sp-FRAME_DROP;
        wr64(g,slot,0);
        wr32(g,slot,(u32)xcall(g,KHA_ENCODE,arg,0,0,0));
        u64 script=rd64(g,self+0x5b0);
        extensions[H_3D60C0]++;
        xcall5(g,KHA_SCRIPT_RUN,script,6,slot,1,0);
        KH_RSP(g)=sp;
    }
    KH_RCX(g)=self; KH_RDX(g)=arg; KH_R8(g)=r8; KH_R9(g)=r9;
    forward(g,KHF_3D60C0);
}

int kh_mod_a_counts(int hook,u64 *calls,u64 *ext){
    if(hook==H_N){ *calls=atomic_load(&shopface_missing); *ext=0; return 1; }   /* pseudo hook 9: shop-face files missing from the loose tree */
    if(hook<0||hook>=H_N) return 0;
    *calls=atomic_load(&hits[hook]); *ext=atomic_load(&extensions[hook]); return 1;
}
void kh_mod_hooks_a_dump(void){
    for(int i=0;i<H_N;i++)
        fprintf(stderr,"kh: mod A %s calls=%llu extensions=%llu\n",hook_names[i],
            (unsigned long long)atomic_load(&hits[i]),(unsigned long long)atomic_load(&extensions[i]));
    fprintf(stderr,"kh: mod A 29efa0 loose-missing=%llu\n",(unsigned long long)atomic_load(&shopface_missing));
}
