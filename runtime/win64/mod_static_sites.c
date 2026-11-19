/* mod_static_sites.c : Task 12 - the two INLINE Detours sites of KH2Tweaks_Refactor.dll (sha256 84b43f08...ca92) as C.
 *
 * The DLL attaches Detours in the MIDDLE of two exe functions.  Each handler is a short compare-and-branch fragment that ends in
 * one of two indirect jumps back into the function (to pbRemain + constant).  The re-lifted function (gen/mod_static_lifted.c, made
 * by tools/mod_static_lift.py) has one spliced block at the site address:
 *
 *     L<site>:{ if(kh_mod_site_<site>(g)) goto L<ok>; goto L<skip>; }
 *
 * so the handler below stands exactly where the DLL handler runs: after the instruction before the site, instead of the bytes
 * Detours moved, with the registers, flags and memory the handler leaves behind.  Nothing is pushed on the guest stack.
 *
 * This file depends on khrt.h only: the lifter differential harness (tools/mod_static_difftest.py) compiles the same source.
 * Transcribed from a fresh `objdump -d -Mintel` of the DLL; no DLL code was executed.
 *
 *   exe 0x3c40a9 (in f_3c3fe0)  DLL handler 0x34490  installer 0x1bdd7..0x1be42  ok = site+6+0x10 = 0x3c40bf   skip = site+6+0xf6  = 0x3c41a5
 *   exe 0x360a5a (in f_360920)  DLL handler 0x34460  installer 0x1ee9d..0x1eeff  ok = site+10+0x9 = 0x360a6d   skip = site+10+0x141 = 0x360ba5
 */
#include "khrt.h"

/* VEX lazy flags after `cmp a,b` of the given width (0 = byte, 1 = word, 2 = dword): the same four puts the lifter emits. */
static inline void cmp_flags(KCtx *g,int width,u64 a,u64 b){
    PUT(u64,KO_CC_OP,(u64)(CC_SUB+width)); PUT(u64,KO_CC_DEP1,a); PUT(u64,KO_CC_DEP2,b); PUT(u64,KO_CC_NDEP,(u64)0);
}

/* ---- exe 0x3c40a9: dualwield_forms ---------------------------------------------------------------------------------------------
 * The DLL keeps the six ini values (dualwield_forms=0,2,3,0,5,0) in six dword slots at DLL 0x819c0..0x819d4; its init stores
 * ini[0]->819d4 ini[1]->819d0 ini[2]->819cc ini[3]->819c4 ini[4]->819c8 ini[5]->819c0 (DLL 0x1bd02..0x1bd31).  The handler is
 *     mov rax,[slot pointer]; cmp r8d,[rax]; je ok      six times, slots 819d4 819d0 819cc 819c4 819c8 819c0 (= ini order)
 *     jmp skip
 * It leaves RAX = address of the last slot it compared and the flags of that compare; r8 keeps the form id (vanilla subtracts 4).
 * The DLL is not mapped here, so the runtime keeps the slot block in guest memory (same layout by address) and RAX points there. */
u64 kh_mod_dual_slots_va;                                   /* guest address of the 24-byte slot block; set by kh_mod_static_init */
const u32 kh_mod_dual_slots_image[6]={0,0,5,3,2,0};         /* by address: 819c0=ini[5] 819c4=ini[3] 819c8=ini[4] 819cc=ini[2] 819d0=ini[1] 819d4=ini[0] */
static const u8 dual_probe[6]={0x14,0x10,0x0c,0x04,0x08,0x00};

int kh_mod_site_3c40a9(KCtx *g){
    u64 base=kh_mod_dual_slots_va;
    if(!base) kh_fault(g,KH_IMAGE_BASE+0x3c40a9,"mod-static-sites: dual-wield slot block not initialised");
    u32 form=(u32)KH_R8(g);
    for(int i=0;i<6;i++){
        u64 slot=base+dual_probe[i]; u32 v=LD(u32,slot);
        KH_RAX(g)=slot; cmp_flags(g,2,(u64)form,(u64)v);
        if(form==v) return 1;
    }
    return 0;
}

/* ---- exe 0x360a5a: patch_ability_categories ------------------------------------------------------------------------------------
 *     cmp byte [rax+6],dil ; jne B          A: category byte equals dil ...
 *     cmp word [rbx],di    ; jge B             ... and the list word is below di (signed): ok
 *     jmp ok
 *  B: cmp word [rax+0xc],1 ; jne skip       B: table word == 1 (the extension) ...
 *     cmp word [rbx],di    ; jge skip          ... and the list word is below di (signed): ok
 *     jmp ok
 * Registers are not written; the flags are those of the last compare executed.  Vanilla has only path A. */
int kh_mod_site_360a5a(KCtx *g){
    u64 info=KH_RAX(g), ent=KH_RBX(g); u16 di=(u16)KH_RDI(g); u8 dil=(u8)di;
    u8 cat=LD(u8,info+6);
    cmp_flags(g,0,(u64)cat,(u64)dil);
    if(cat==dil){
        u16 w=LD(u16,ent); cmp_flags(g,1,(u64)w,(u64)di);
        if((s16)w<(s16)di) return 1;
    }
    extern int kh_patch_ability_categories;
    if(!kh_patch_ability_categories) return 0;
    u16 k=LD(u16,info+0xc);
    cmp_flags(g,1,(u64)k,(u64)1);
    if(k!=1) return 0;
    u16 w=LD(u16,ent); cmp_flags(g,1,(u64)w,(u64)di);
    return (s16)w<(s16)di;
}

/* ---- exe 0x15b960 (in f_15b6d0): Task 19, NeverlandFly.dll (sha256 1fca202b...7dfb) ---------------------------------------------
 * Not a Detours site: the DLL never touches the instruction `83 c5 16  add ebp,0x16`, its THREAD (DLL 0x1290) rewrites the
 * immediate byte at exe 0x15b962 every 250 ms:  [S5+0x292] = (S1()==1) ? 0x16 : 0x1c   (S5 = 0x15b6d0, S1 = exe 0x2e5380).
 * A lifted function has its immediates baked in, so this one instruction is a site whose handler fetches the immediate from the
 * image byte each time it runs, as the CPU does; runtime/mod_neverland.c is the thread that writes the byte.
 *     L15b960:{ kh_mod_site_15b960(g); goto L15b963; }
 * `add r/m32,imm8` sign-extends the immediate, writes the 32-bit sum (upper half of RBP cleared) and sets the flags of a 32-bit
 * add (the next instruction, `inc edi`, keeps CF from here).  The two opcode bytes are checked: if anything ever rewrote them
 * this model would be wrong, and that is a fault, not a guess.  The result is unused (one continuation address). */
int kh_mod_site_15b960(KCtx *g){
    if(LD(u16,KH_IMAGE_BASE+0x15b960ull)!=0xc583) kh_fault(g,KH_IMAGE_BASE+0x15b960,"mod-static-sites: exe 0x15b960 is no longer `add ebp,imm8`");
    u32 imm=(u32)(s32)(s8)LD(u8,KH_IMAGE_BASE+0x15b962ull), a=(u32)KH_RBP(g);
    PUT(u64,KO_CC_OP,(u64)(CC_ADD+2)); PUT(u64,KO_CC_DEP1,(u64)a); PUT(u64,KO_CC_DEP2,(u64)imm); PUT(u64,KO_CC_NDEP,(u64)0);
    KH_RBP(g)=(u64)(u32)(a+imm);
    return 1;
}
