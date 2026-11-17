/* Task 23: the function detours of KH2Tweaks_Refactor.dll that were still missing after Tasks 6..19.  Everything here was read from a
 * static `objdump -d -Mintel` of the shipped DLL (sha256 84b43f08...ca92; .text RVA 0x1000 = file offset 0x400) against the pinned exe;
 * no DLL code was executed, natively or emulated.  Mechanism as in mod_hooks_a.c: a strong f_<rva> replaces the lifted function through
 * the archive alias list (Makefile MOD_B_HOOKS / PHASE_HOOKS), orig_f_<rva> stays the vanilla lifted body.
 *
 *   DLL 0x306c0 -> exe 0x3f29e0  FIELD_COMMAND.execute, the command-menu builder.  FULL REPLACEMENT: the handler never reaches its
 *                                Detours trampoline (slot 0x82260 has no reference inside 0x306c0..0x30a73 or its two local helpers
 *                                0x30290 / 0x30510).  It is a re-implementation of the exe body with these differences:
 *                                  - Limit Form (`[obj+0xde0]==3`): the sub-list at this+0x628 holds only the limits whose flag bit is
 *                                    set in `[[obj+0x5c0]+0x1e7]` (0x04 -> 0x2ba, 0x08 -> 0x2bd, 0x10 -> 0x2c0, 0x20 -> 0x2ab), built in
 *                                    the DLL's own 16-byte word list 0x1800819e8; the four shortcut slots use the same list (the exe
 *                                    has two fixed tables, 0x74ebe8 and 0x5c9678);
 *                                  - when the DLL's word list 0x1800819d8 is not empty, command 0x3e7 is added to the list at
 *                                    this+0xa8 and the word list becomes the list at this+0x6a8;
 *                                  - the party entries take the command from 0x3dfeb0([member+0x914]) (the exe: 0x3b5b20 = decode of
 *                                    [member+8]), sign-extending the word at +0x4c (the exe zero-extends);
 *                                  - shortcut slots: ids are sign-extended words, the ability count byte is signed, the compare is
 *                                    16-bit (the exe: zero-extended id, unsigned count, 32-bit compare of a sign-extended word);
 *                                  - the pending-menu tail jump passes [this+0xbe0] as 64 bits (the exe sign-extends 32);
 *                                  - the sub-lists at +0x428 / +0x128 / +0x1a8 are built by the exe's own 0x3f3000 / 0x3f3300 /
 *                                    0x3f3110 (the same code the exe body has inline; 0x3f3110 carries the static edits and is
 *                                    re-lifted from the patched image).
 *                                The body re-lifted from the patched image (mod_f_3f29e0) is therefore dead on the PC and here:
 *                                Makefile MOD_STATIC_REPLACED.
 *   DLL 0x30a80 -> exe 0x3f27d0  FIELD_COMMAND reset.  FULL REPLACEMENT (slot 0x82258 has no reference in the handler).  As the exe
 *                                body, but the drive-form list at this+0xb68 is followed by a second pass: 0x3c5900(6..13) fills
 *                                the DLL's word list 0x1800819d8 (the list the 0x3e7 command shows).  8 entries would not leave room
 *                                for the terminator: the DLL then calls __report_rangecheckfailure (int 0x29, code 8) = named fatal.
 *
 *   DLL 0x31c60 -> exe 0x3c4780  item predicate.  CHAINS (tail jump through slot 0x82308 unless it answers itself): true when the item's
 *                                command (word +0x10 of its 0x3e39a0 record) is a command of kind 0xb (byte +6 of the 0x3b68b0 record).
 *   DLL 0x2d5c0 -> exe 0x351390  item-menu row predicate.  FULL REPLACEMENT (no reference to slot 0x81e68): true for ids 0x112 / 0x113
 *                                and for item kinds (byte +5) 4..8, 20, 21; the exe compares the id with a fixed list.
 *   DLL 0x2da70 -> exe 0x368820  item effect category.  CHAINS for kind 0, 22 and > 24; kinds 1..3, 9..15, 23, 24 -> 0, 4..8 and 20, 21 -> 5,
 *                                16 -> 1, 17 -> 2, 18 -> 3, 19 -> 4.
 *   DLL 0x33550 -> exe 0x4060e0  LMPLAYER command list.  CHAINS, behind its own gate: 0x3ba720(arg 2) must be true (the exe tests
 *                                [this+8], and only when it is not null), `[this]==0xa` answers 1 at once, and list types >= 0x10 would
 *                                be looked up in the DLL's special-command vector (tweaks/special_command_menu.bin: not shipped, the
 *                                vector is empty; kh_mod_hooks_b_init stops with a named fatal if that file exists).
 *   DLL 0x2f970 -> exe 0x3d1730  CHAINS: `[[this]+0x2c] = 0x4ad240(arg 2)` first.
 *   DLL 0x2fd20 -> exe 0x3f65a0  effect init.  CHAINS: runs trigger 0x66 of the primary common script (new / run / reset / free, like
 *                                the 0x65 trigger of DLL 0x32710), then would do the same for 01..05common.bdx and initialise the
 *                                NNeffect.bar files found by DLL 0x300a0.
 *   DLL 0x300a0 -> exe 0x3f6620  effect load.  CHAINS: probes 01effect.bar..20effect.bar with exe 0x3f95f0 and queues the ones found.
 *   DLL 0x30180 -> exe 0x3f6510  effect release.  CHAINS: releases the extra effect files (none: the count is 0).
 *   DLL 0x33610 -> exe 0x3a17f0  common-script load.  CHAINS: probes 01..05common.bdx with exe 0x3f95f0 and loads the ones found.
 *                                None of these 25 files exists in the mod as shipped.  The probes are made exactly as the DLL makes
 *                                them (the same exe function, the same names, so they show up as MISS lines in the file log);
 *                                a probe that FINDS a file is a named fatal (`mod-extra-file-not-ported`): the load/run/release code
 *                                behind it is not ported because nothing shipped can reach it.
 *   DLL 0x10340 -> exe 0x153850  FULL REPLACEMENT.  Icon byte of the loaded command-menu layout: the name at exe 0x9babb0 is compared
 *   DLL 0x10590 -> exe 0x15aab0  with `<name>command.2dd` for every 16-byte entry of dll/tweaks/command_menu_offsets.bin (29 entries
 *   DLL 0x10100 -> exe 0x15aba0  as shipped); entry byte +0xe (0x153850) / +0xc (0x15aab0), 0xe2 / 0xe7 when no entry matches.
 *                                0x15aab0 and 0x15aba0 then build the row text (0x11, icon, ff 1c 00 09, colour, 11 02 00 1c, 00, the
 *                                message) like the exe.  0x15aba0 takes its icon from the list's menu type `[[arg5+8]+0xab0]`
 *                                (1..12 as the exe's predicates; 100..110 from the DLL's 11 config bytes, all 0 as shipped; every
 *                                other type KEEPS the icon of the previous call, where the exe uses 0xff).  The three icon bytes
 *                                are DLL globals (0x1800817c8..ca, initially 0) and are kept here as such.
 *   DLL 0x2d670 -> exe 0x31dd70  menu input.  CHAINS: calls 0x2f5f30 / 0x2f6230 itself first (the original then calls them again),
 *                                and, when the confirm trigger (bit 24 of [pad+8]) is set on a row whose id is 0x200..0x204, runs
 *                                the DLL's own actions (map jumps through exe 0x152990 etc.) instead of / before the original.
 *
 * Slot -> exe RVA for every engine function used here: tools/mod_dll_slots.py with tools/mod_dll_slots_h23.txt (pattern read from the
 * DLL's scanner, scanned over the exe file image; all listed in implementation notes).  The DLL's two word lists live in one 32-byte guest
 * block (exe 0x3b1840 reads them through a guest pointer): +0 = 0x1800819d8, +0x10 = 0x1800819e8, adjacent as in the DLL's .bss.
 * A DLL path that would fault on the PC (it never checks a pointer) is a named fatal. */
#include "mod_config.h"
#include <string.h>
#include <strings.h>

int kh_moose_continue_on(void);   /* moose.c: the ini turns KH2Moose's exception handler on */
struct kh_modb_mock *kh_modb_sink;
static u64 lists_va;
#define L_EXTRA (lists_va)            /* DLL 0x1800819d8: up to 7 command ids + terminator */
#define L_LIMIT (lists_va+0x10)       /* DLL 0x1800819e8: up to 4 limit command ids, zero-filled to 16 bytes on every build */

enum { B_3F29E0, B_3F27D0, B_3C4780, B_351390, B_368820, B_4060E0, B_3D1730, B_3F65A0, B_3F6620, B_3F6510, B_3A17F0, B_153850, B_15AAB0, B_15ABA0, B_31DD70, B_N };
static const char *const hook_names[B_N]={"3f29e0","3f27d0","3c4780","351390","368820","4060e0","3d1730","3f65a0","3f6620","3f6510","3a17f0","153850","15aab0","15aba0","31dd70"};
static _Atomic u64 hits[B_N], ext[B_N];              /* ext: calls in which the DLL's own code decided the result (no forward to the original) */
static _Atomic u64 c_probes, c_icon_match[3], c_icon_default[3], c_icon_stale, c_menu_custom[5];
static _Atomic u64 c_tail, c_early, c_limit_lists, c_limit_slots, c_extra_cmd, c_reset_extra, c_party_rows;
static void enter(int h){ if(atomic_fetch_add(&hits[h],1)==0) fprintf(stderr,"kh: mod B %s first call\n",hook_names[h]); }

/* Every outgoing guest call goes through here so that a test can record it instead (kh_modb_sink).  Argument registers the DLL
 * leaves unset are passed as 0. */
static u64 bcall5(KCtx *g,u32 rva,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){
    struct kh_modb_mock *k=kh_modb_sink;
    if(k){
        if(k->ncalls>=KH_MODB_MAXCALLS){ k->overflow=1; return 0; }
        int idx=k->ncalls++; struct kh_modb_call *c=&k->c[idx];
        c->rva=rva; c->tail=0; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3; c->a[4]=a4; c->memok=0;
        if((rva==0x152990||rva==0x3f95f0)&&GA_OK(a0)&&GA_OK(a0+15)){ memcpy(c->mem,hp(g,a0),16); c->memok=1; }   /* the jump record / the probed name */
        u64 r=k->fake?k->fake(k->user,c,idx):0; KH_RAX(g)=r; return r;
    }
    return kh_call5(g,GS_BASE+rva,a0,a1,a2,a3,a4);
}
/* forward to the original function: a tail jump with the guest stack as at entry (recorded as {own rva, tail = 2}) */
static void borig(KCtx *g,u32 rva,KhFn orig,u64 a0,u64 a1,u64 a2,u64 a3){
    struct kh_modb_mock *k=kh_modb_sink;
    KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    if(k){
        if(k->ncalls>=KH_MODB_MAXCALLS){ k->overflow=1; KH_RET(g); return; }
        int idx=k->ncalls++; struct kh_modb_call *c=&k->c[idx];
        c->rva=rva; c->tail=2; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3; c->a[4]=0; c->memok=0;
        KH_RAX(g)=k->fake?k->fake(k->user,c,idx):0; KH_RET(g); return;
    }
    orig(g);
}
#define bcall(g,rva,a0,a1,a2,a3) bcall5((g),(rva),(u64)(a0),(u64)(a1),(u64)(a2),(u64)(a3),0)
/* tail jump: the callee returns to the hook's caller (its `ret` pops the return address the caller pushed) */
static void btail(KCtx *g,u32 rva,u64 a0,u64 a1,u64 a2,u64 a3){
    struct kh_modb_mock *k=kh_modb_sink;
    if(k){
        if(k->ncalls>=KH_MODB_MAXCALLS){ k->overflow=1; KH_RET(g); return; }
        int idx=k->ncalls++; struct kh_modb_call *c=&k->c[idx];
        c->rva=rva; c->tail=1; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3; c->a[4]=0; c->memok=0;
        KH_RAX(g)=k->fake?k->fake(k->user,c,idx):0; KH_RET(g); return;
    }
    KH_RCX(g)=a0; KH_RDX(g)=a1; KH_R8(g)=a2; KH_R9(g)=a3;
    kh_dispatch(g,GS_BASE+rva);
}
static u64 need(KCtx *g,u64 p,const char *hook,const char *what){
    if(!GA_OK(p)) kh_fatal(g,"mod %s: %s is not a valid guest pointer (0x%llx); the DLL does not check this and would fault%s",hook,what,(unsigned long long)p,
        kh_moose_continue_on()?" (KH2Moose's exception handler is on in the ini: the modded PC game would have skipped the faulting read or write, with 0 in its destination register, and gone on; not emulated)":"");
    return p;
}
static u64 lists(KCtx *g){ if(!lists_va) kh_fatal(g,"mod-b-lists: the DLL's command word lists are not allocated (kh_mod_hooks_b_init has not run)"); return lists_va; }

/* ---- DLL 0x306c0 -> exe 0x3f29e0 (full replacement) --------------------------------------------------------------------------- */
#define H29 "3f29e0 (DLL 0x306c0)"
/* The first row of the two top-level menus (DLL 0x3030b..0x30366 and 0x30582..0x305e2: the same code twice; exe 0x3f2600). */
static u32 first_command(KCtx *g,u64 self){
    if(!(u8)bcall(g,0x39a7e0,0,0,0,0)) return 0x68;
    if(rd8(g,GS_BASE+0x717008)==9) return 0x88;                       /* DLL data pointer 0x1800817a0 = exe 0x140717008 */
    u64 obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
    if(rd32(g,obj+0xde0)==2) return 0x16d;
    return (u8)bcall(g,0x3d7820,obj,0,0,0)?0x21d:1;
}
/* The "reaction list" form of both menus (DLL 0x302b3..0x302e7 and 0x30539..0x3056a): rows 3..6 of the table at this+0x9b0. */
static void rows_3_to_6(KCtx *g,u64 self,u64 list){
    bcall(g,0x3b11d0,list,0,0,0);
    for(u32 i=3;i<7;i++){
        u64 e=need(g,bcall(g,0x3b2660,self,i,0,0),H29,"the 0x3b2660 row (null when its id word is 0)");
        bcall(g,0x3b0f20,list,(u32)(s32)(s16)rd16(g,e+2),e+0xc,(u32)(s32)(s8)rd8(g,e+4));
    }
}
/* DLL 0x30290: the list at this+0x28 */
static void menu_main(KCtx *g,u64 self){
    u64 list=self+0x28;
    if((u8)bcall(g,0x3b27e0,self,0,0,0)){ rows_3_to_6(g,self,list); return; }
    bcall(g,0x3b11d0,list,0,0,0);
    u32 id=first_command(g,self);
    bcall(g,0x3b0f20,list,id,0,1);
    u64 obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
    if(rd32(g,obj+0xde0)==3){
        bcall(g,0x3b11d0,self+0x628,0,0,0);
        obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
        u64 sheet=need(g,rd64(g,obj+0x5c0),H29,"[[this+0xae0]+0x5c0]");
        u8 f=rd8(g,sheet+0x1e7); u64 L=lists(g)+0x10; u32 n=0;
        memset(hp(g,L),0,16);
        if(f&0x04){ wr16(g,L,0x2ba); n=1; }
        if(f&0x08){ wr16(g,L+2*(u64)n,0x2bd); n++; }
        if(f&0x10){ wr16(g,L+2*(u64)n,0x2c0); n++; }
        if(f&0x20) wr16(g,L+2*(u64)n,0x2ab);
        atomic_fetch_add(&c_limit_lists,1);
        bcall(g,0x3b1840,self+0x628,L,0,0);
        id=0x2c3;
    } else id=rd16(g,self+0xb68)?2:0x68;
    bcall(g,0x3b0f20,list,id,0,1);
    id=(u8)bcall(g,0x3b27c0,self,0,0,0)?0x68:3;
    bcall(g,0x3b0f20,list,id,0,1);
    if(rd8(g,self+0x24)&0x20){ bcall(g,0x3b0f20,list,0x68,0,1); return; }
    obj=rd64(g,self+0xae0);
    if((u8)bcall(g,0x3aa4f0,obj,0,0,0)){
        u64 e=bcall(g,0x3b0f20,list,5,0,1);
        obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
        if(rd32(g,obj+0xde0)==6 && (u8)bcall(g,0x3abb20,obj,0,0,0)) wr8(g,need(g,e,H29,"the row returned by 0x3b0f20")+5,5);
    } else {
        u64 e=bcall(g,0x3b0f20,list,4,0,1);
        obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
        if(rd32(g,obj+0xde0)!=0) wr8(g,need(g,e,H29,"the row returned by 0x3b0f20")+5,5);
    }
}
/* DLL 0x30510: the list at this+0x328 (the exe's 0x3f3860) */
static void menu_sub(KCtx *g,u64 self){
    u64 list=self+0x328;
    if((u8)bcall(g,0x3b27e0,self,0,0,0)){ rows_3_to_6(g,self,list); return; }
    bcall(g,0x3b11d0,list,0,0,0);
    u32 id=first_command(g,self);
    bcall(g,0x3b0f20,list,id,0,1);
    if((u32)bcall(g,0x3de250,0,0,0,0)!=0) id=0x13c;
    else id=((s32)rd32(g,self+0x3b8)<=0)?0x68:0x48;
    bcall(g,0x3b0f20,list,id,0,1);
    if((u32)bcall(g,0x3de250,0,0,0,0)==2 && (u32)bcall(g,0x3de130,0,0,0,0)==0xc9){ bcall(g,0x3f3230,self,0,0,0); id=0x25d; }
    else if(rd32(g,GS_BASE+0x749804)&0x2000000u) id=0x68;            /* DLL data pointer 0x180081960 = exe 0x140749804 */
    else id=(u8)bcall(g,0x39a800,0,0,0,0)?0x49:0x68;
    bcall(g,0x3b0f20,list,id,0,1);
    id=(u8)bcall(g,0x39a870,0,0,0,0)?0x51:0x68;
    bcall(g,0x3b0f20,list,id,0,1);                                    /* a tail jump in the DLL: returns into 0x306c0 */
}
void kh_phase_field_cmd_entry(void);
void f_3f29e0(KCtx *g){
    kh_phase_field_cmd_entry();
    enter(B_3F29E0);
    u64 self=A0(g);
    u64 pending=rd64(g,self+0xbd8);
    if(pending){ atomic_fetch_add(&c_tail,1); btail(g,0x3b19b0,self+0x528,pending,rd64(g,self+0xbe0),0); return; }
    u64 obj=need(g,rd64(g,self+0xae0),H29,"[this+0xae0]");
    if(!rd64(g,obj+0x5c0)){ atomic_fetch_add(&c_early,1); KH_RET(g); return; }       /* RAX is not written on this path */
    u64 Lx=lists(g);
    bcall(g,0x3b11d0,self+0x228,0,0,0);
    for(u32 i=0;i<3;i++){
        u64 m=bcall(g,0x3d5c50,i,0,0,0);
        if(!(u8)bcall(g,0x3ba720,m,0,0,0)) continue;
        need(g,m,H29,"the party member returned by 0x3d5c50");
        if(rd32(g,m+0x6c8)&0x400000u) continue;
        if(!rd64(g,m+0x5c0)) continue;
        wr64(g,self+0xb80+8*(u64)(s64)(s32)rd32(g,self+0x238),m);
        u64 ent=need(g,bcall(g,0x3dfeb0,rd32(g,m+0x914),0,0,0),H29,"the 0x3dfeb0 record of [member+0x914]");
        u32 cmd=(u32)bcall(g,0x3e60f0,(u32)(s32)(s16)rd16(g,ent+0x4c),0,0,0);
        bcall(g,0x3b0f20,self+0x228,cmd,0,1);
        atomic_fetch_add(&c_party_rows,1);
    }
    bcall(g,0x3f3000,self,0,0,0);
    bcall(g,0x3b11d0,self+0xa8,0,0,0);
    bcall(g,0x3b1840,self+0xa8,self+0xb68,0,0);
    if(rd16(g,Lx)){
        atomic_fetch_add(&c_extra_cmd,1);
        bcall(g,0x3b0f20,self+0xa8,0x3e7,0,1);
        bcall(g,0x3b11d0,self+0x6a8,0,0,0);
        bcall(g,0x3b1840,self+0x6a8,Lx,0,0);
    }
    bcall(g,0x3f3300,self,0,0,0);
    bcall(g,0x3f3110,self,0,0,0);
    bcall(g,0x3e7b30,self+0x4a8,0,0,0);
    if((u8)bcall(g,0x3bfb30,obj,0,0,0)) bcall(g,0x3e6d10,self+0xb76,0,0,0);           /* the DLL keeps the object pointer it loaded at entry */
    bcall(g,0x3b11d0,self+0x3a8,0,0,0);
    bcall(g,0x3b1840,self+0x3a8,self+0xb76,0,0);
    menu_main(g,self);
    menu_sub(g,self);
    bcall(g,0x3b11d0,self+0x2a8,0,0,0);
    u64 save=need(g,bcall(g,0x152380,0,0,0,0),H29,"the save block returned by 0x152380");
    for(u32 k=0;k<4;k++){
        s32 id=(s16)rd16(g,save+0x36f8+2*(u64)k);
        u64 cmd=need(g,bcall(g,0x3b68b0,(u32)id,0,0,0),H29,"the command record returned by 0x3b68b0");
        u64 cur=rd64(g,self+0xae0); u32 flag;
        if(rd16(g,cmd+2)==3){
            flag=0;
            u64 tab=need(g,bcall(g,0x3d74c0,cur,0,0,0),H29,"the ability table returned by 0x3d74c0");
            s32 n=(s8)rd8(g,tab+0x12);
            if(n>0){ u16 want=rd16(g,cmd+4); for(s32 j=0;j<n;j++) if(rd16(g,tab+0x34+2*(u64)j)==want) flag++; }
        } else {
            need(g,cur,H29,"[this+0xae0]");
            if(rd32(g,cur+0xde0)==3){ id=(s16)rd16(g,Lx+0x10+2*(u64)k); atomic_fetch_add(&c_limit_slots,1); }
            flag=1;
        }
        u64 e=need(g,bcall(g,0x3b0f20,self+0x2a8,(u32)id,0,flag),H29,"the row returned by 0x3b0f20");
        wr8(g,e+6,(u8)(k+1));
    }
    KH_RET(g);
}

/* ---- DLL 0x30a80 -> exe 0x3f27d0 (full replacement) --------------------------------------------------------------------------- */
#define H27 "3f27d0 (DLL 0x30a80)"
void f_3f27d0(KCtx *g){
    enter(B_3F27D0);
    u64 self=A0(g); u64 Lx=lists(g);
    bcall(g,0x3b2080,self,0,0,0);
    bcall(g,0x3b1b70,self+0x28,0,0,0);
    bcall(g,0x3b1b70,self+0x328,0,0,0);
    wr64(g,self+0xae0,0);
    bcall(g,0x3b20b0,self,0,0,0);
    u32 n=0;
    for(u32 i=0;i<6;i++){ u32 v=(u32)bcall(g,0x3c5900,i,0,0,0); if(v){ wr16(g,self+0xb68+2*(u64)n,(u16)v); n++; } }
    wr16(g,self+0xb68+2*(u64)n,0);
    n=0;
    for(u32 i=6;i<14;i++){ u32 v=(u32)bcall(g,0x3c5900,i,0,0,0); if(v){ wr16(g,Lx+2*(u64)n,(u16)v); n++; } }
    if(2*(u64)n>=0x10)
        kh_fatal(g,"mod-3f27d0-extra-list-overflow: all 8 extra entries (0x3c5900 indices 6..13) are present, the DLL's 16-byte word list 0x1800819d8 "
                   "has no room for the terminator: the DLL calls __report_rangecheckfailure here (int 0x29, code 8) and the PC process ends");
    wr16(g,Lx+2*(u64)n,0);
    if(n) atomic_fetch_add(&c_reset_extra,1);
    u64 save=need(g,bcall(g,0x152380,0,0,0,0),H27,"the save block returned by 0x152380");
    for(u32 k=0;k<4;k++){
        u64 slot=save+0x36f8+2*(u64)k;
        u64 cmd=need(g,bcall(g,0x3b68b0,(u32)(s32)(s16)rd16(g,slot),0,0,0),H27,"the command record returned by 0x3b68b0");
        if(rd16(g,cmd+2)==2) wr16(g,slot,(u16)bcall(g,0x3c5900,(u32)(s32)(s16)rd16(g,cmd+4),0,0,0));
    }
    bcall(g,0x3e6d10,self+0xb76,0,0,0);
    wr64(g,self+0xbd8,0); wr32(g,self+0xbe4,0);
    KH_RET(g);
}

/* ---- DLL 0x31c60 -> exe 0x3c4780 (chains) --------------------------------------------------------------------------------------- */
void orig_f_3c4780(KCtx *); void orig_f_368820(KCtx *); void orig_f_4060e0(KCtx *); void orig_f_3d1730(KCtx *);
void orig_f_3f65a0(KCtx *); void orig_f_3f6620(KCtx *); void orig_f_3f6510(KCtx *); void orig_f_3a17f0(KCtx *); void orig_f_31dd70(KCtx *);
void f_3c4780(KCtx *g){
    enter(B_3C4780);
    u32 id=(u32)A0(g); u64 rdx=A1(g),r8=A2(g),r9=A3(g);
    u64 item=need(g,bcall(g,0x3e39a0,id,0,0,0),"3c4780 (DLL 0x31c60)","the item record returned by 0x3e39a0");
    s32 cmd=(s16)rd16(g,item+0x10);
    if((u16)cmd){
        u64 rec=need(g,bcall(g,0x3b68b0,(u32)cmd,0,0,0),"3c4780 (DLL 0x31c60)","the command record returned by 0x3b68b0");
        if(rd8(g,rec+6)==0xb){ atomic_fetch_add(&ext[B_3C4780],1); KH_RAX(g)=(rec&~0xffull)|1; KH_RET(g); return; }   /* mov al,1 */
    }
    borig(g,0x3c4780,orig_f_3c4780,id,rdx,r8,r9);
}

/* ---- DLL 0x2d5c0 -> exe 0x351390 (full replacement) ----------------------------------------------------------------------------- */
void f_351390(KCtx *g){
    enter(B_351390);
    s64 row=(s32)A0(g);
    u64 menu=bcall(g,0x304df0,0,0,0,0);
    u64 n=bcall(g,0x34bc50,menu,0,0,0);
    if(!(u32)n){ KH_RAX(g)=n&~0xffull; KH_RET(g); return; }                                   /* xor al,al */
    u64 list=bcall(g,0x34bc40,menu,0,0,0);
    u64 at=list+6*(u64)row; need(g,at,"351390 (DLL 0x2d5c0)","the item-menu row"); need(g,at+1,"351390 (DLL 0x2d5c0)","the item-menu row");
    u32 id=rd16(g,at);
    u64 item=bcall(g,0x3e39a0,id,0,0,0);
    if(id==0x112||id==0x113){ atomic_fetch_add(&ext[B_351390],1); KH_RAX(g)=(item&~0xffull)|1; KH_RET(g); return; }
    need(g,item,"351390 (DLL 0x2d5c0)","the item record returned by 0x3e39a0");
    u32 k=(u32)rd8(g,item+5)-4;
    static const u8 no[0x12]={0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0};                           /* DLL byte table 0x2d65c: 0 -> `mov al,1`, 1 -> `xor al,al` */
    if(k>0x11){ KH_RAX(g)=k&0xffffff00u; KH_RET(g); return; }
    if(!no[k]) atomic_fetch_add(&ext[B_351390],1);
    KH_RAX(g)=no[k]?0:1; KH_RET(g);
}

/* ---- DLL 0x2da70 -> exe 0x368820 (chains for kinds 0, 22, > 24) ------------------------------------------------------------------- */
void f_368820(KCtx *g){
    enter(B_368820);
    u32 id=(u32)A0(g); u64 rdx=A1(g),r8=A2(g),r9=A3(g);
    u64 item=need(g,bcall(g,0x3e39a0,id,0,0,0),"368820 (DLL 0x2da70)","the item record returned by 0x3e39a0");
    u32 k=(u32)rd8(g,item+5)-1;
    static const u8 sel[0x18]={0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,2,3,4,5,1,1,6,0,0};               /* DLL byte table 0x2db18 */
    static const u32 val[6]={0,5,1,2,3,4};                                                    /* DLL targets 0x2daaa, 0x2dab2, 0x2dabd, 0x2dac8, 0x2dad3, 0x2dade */
    if(k<=0x17&&sel[k]!=6){ atomic_fetch_add(&ext[B_368820],1); KH_RAX(g)=val[sel[k]]; KH_RET(g); return; }
    borig(g,0x368820,orig_f_368820,id,rdx,r8,r9);
}

/* ---- DLL 0x33550 -> exe 0x4060e0 (chains behind its own gate) --------------------------------------------------------------------- */
void f_4060e0(KCtx *g){
    enter(B_4060E0);
    u64 self=A0(g),who=A1(g),list=A2(g),r9=A3(g);
    u64 r=bcall(g,0x3ba720,who,0,0,0);
    if(!(u8)r){ atomic_fetch_add(&ext[B_4060E0],1); KH_RAX(g)=r; KH_RET(g); return; }           /* the DLL returns what 0x3ba720 returned */
    need(g,self,"4060e0 (DLL 0x33550)","this");
    if(rd32(g,self)==0xa){ atomic_fetch_add(&ext[B_4060E0],1); KH_RAX(g)=(r&~0xffull)|1; KH_RET(g); return; }
    bcall(g,0x3b11d0,list,0,0,0);
    /* list types >= 0x10: the DLL's special-command vector [0x180082a78, 0x180082a80) is empty as shipped (see kh_mod_hooks_b_init) */
    borig(g,0x4060e0,orig_f_4060e0,self,who,list,r9);
}

/* ---- DLL 0x2f970 -> exe 0x3d1730 (chains) --------------------------------------------------------------------------------------- */
void f_3d1730(KCtx *g){
    enter(B_3D1730);
    u64 self=need(g,A0(g),"3d1730 (DLL 0x2f970)","this"),b=A1(g),c=A2(g),r9=A3(g);
    u64 inner=rd64(g,self);                                                                   /* loaded before the call */
    u32 id=(u32)bcall(g,0x4ad240,b,0,0,0);
    wr32(g,need(g,inner,"3d1730 (DLL 0x2f970)","[this]")+0x2c,id);
    borig(g,0x3d1730,orig_f_3d1730,self,b,c,r9);
}

/* ---- the extra-file chain: DLL 0x33610 -> exe 0x3a17f0, DLL 0x300a0 -> exe 0x3f6620, DLL 0x2fd20 -> exe 0x3f65a0,
 *      DLL 0x30180 -> exe 0x3f6510 (all chain) ------------------------------------------------------------------------------------ */
static u64 names_va;                                  /* 25 guest strings, 16 bytes each: 01..05common.bdx, 01..20effect.bar */
#define NAME_COMMON(k) (names_va+16*(u64)(k))         /* k = 0..4 */
#define NAME_EFFECT(n) (names_va+16*(u64)(5+(n)-1))   /* n = 1..20 */
static void extra_found(KCtx *g,const char *hook,u64 name,const char *what){
    kh_fatal(g,"mod-extra-file-not-ported: %s: the DLL's probe (exe 0x3f95f0) found \"%s\"; the mod as shipped has no such file, so the code that %s is not ported",hook,gstr(g,name),what);
}
void f_3a17f0(KCtx *g){
    enter(B_3A17F0);
    u64 rcx=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    if(!names_va) kh_fatal(g,"mod-b-lists: kh_mod_hooks_b_init has not run");
    for(int k=0;k<5;k++){ atomic_fetch_add(&c_probes,1);
        if(bcall(g,0x3f95f0,NAME_COMMON(k),0,0,0)) extra_found(g,"3a17f0 (DLL 0x33610)",NAME_COMMON(k),"loads an extra common script (exe 0x3e5fa0) into the DLL's pointer"); }
    borig(g,0x3a17f0,orig_f_3a17f0,rcx,rdx,r8,r9);
}
void f_3f6620(KCtx *g){
    enter(B_3F6620);
    u64 rcx=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    if(!names_va) kh_fatal(g,"mod-b-lists: kh_mod_hooks_b_init has not run");
    for(int n=1;n<=20;n++){ atomic_fetch_add(&c_probes,1);                                     /* the DLL's count 0x180082b58 = 0, then 20 probes */
        if((u32)bcall(g,0x3f95f0,NAME_EFFECT(n),0,0,0)) extra_found(g,"3f6620 (DLL 0x300a0)",NAME_EFFECT(n),"queues an extra effect file (exe 0x39df00, 0x3e9) and records its number"); }
    borig(g,0x3f6620,orig_f_3f6620,rcx,rdx,r8,r9);
}
void f_3f65a0(KCtx *g){
    enter(B_3F65A0);
    u64 rcx=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    if(!names_va) kh_fatal(g,"mod-b-lists: kh_mod_hooks_b_init has not run");
    u64 script=bcall(g,0x3e1730,rd64(g,GS_BASE+0x2a0f688),0,0,0);                             /* DLL data pointer 0x1800818b8 = exe 0x142a0f688 */
    if(!script) kh_fatal(g,"mod 3f65a0 (DLL 0x2fd20): common script allocation (0x3e1730) returned null; the DLL would fault in trigger 0x66");
    atomic_fetch_add(&ext[B_3F65A0],1);
    bcall5(g,0x3e1840,script,0x66,0,0,0);
    bcall(g,0x3e1410,script,0,0,0);
    bcall(g,0x3e1510,script,0,0,0);
    for(int k=0;k<5;k++){ atomic_fetch_add(&c_probes,1);
        if(bcall(g,0x3f95f0,NAME_COMMON(k),0,0,0)) extra_found(g,"3f65a0 (DLL 0x2fd20)",NAME_COMMON(k),"runs trigger 0x66 of an extra common script"); }
    /* the DLL's extra effect files: its count 0x180082b58 is 0 (set by DLL 0x300a0, whose probes found nothing or stopped above) */
    borig(g,0x3f65a0,orig_f_3f65a0,rcx,rdx,r8,r9);
}
void f_3f6510(KCtx *g){
    enter(B_3F6510);
    u64 rcx=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g);
    borig(g,0x3f6510,orig_f_3f6510,rcx,rdx,r8,r9);                                             /* nothing to release: the DLL's count is 0 */
}

/* ---- the command-menu icon hooks: DLL 0x10340 -> exe 0x153850, DLL 0x10590 -> exe 0x15aab0, DLL 0x10100 -> exe 0x15aba0
 *      (all three full replacements) ------------------------------------------------------------------------------------------------ */
static u8 *cm_entries; static u32 cm_count; static char cm_state[160]="not loaded";           /* tweaks/command_menu_offsets.bin: 16-byte entries */
static u8 icon_c8, icon_c9, icon_ca;                                                          /* DLL globals 0x1800817c8 / c9 / ca (.bss: 0) */
static u8 icon_cfg[11];                                                                       /* DLL 0x180081860..6a: KH2_Tweaks_SpecialCommandList.ini (not shipped): 0 */
/* the loop of DLL 0x103a1..0x104c1 / 0x10600..0x10736: *icon = entry[off] for the first entry whose name + "command.2dd" equals the
 * loaded layout name (exe 0x9babb0), `miss` after every entry that does not match; untouched when there is no entry at all */
static void layout_icon(KCtx *g,u8 *icon,int off,u8 miss,int which){
    const char *cur=gstr(g,GS_BASE+0x9babb0);                                                 /* DLL data pointer 0x1800817d0 */
    for(u32 i=0;i<cm_count;i++){
        const u8 *e=cm_entries+16*(size_t)i; char name[12+11+1]; size_t n=strnlen((const char *)e,12);
        memcpy(name,e,n); memcpy(name+n,"command.2dd",12);
        if(strcmp(cur,name)){ *icon=miss; continue; }
        *icon=e[off]; atomic_fetch_add(&c_icon_match[which],1); return;
    }
    if(cm_count) atomic_fetch_add(&c_icon_default[which],1);
}
static void row_text(KCtx *g,u64 out,u8 icon,u8 colour,u32 msg,const char *hook){
    need(g,out,hook,"the text buffer"); need(g,out+0xb,hook,"the text buffer");
    wr8(g,out+1,icon); wr8(g,out,0x11); wr32(g,out+2,0x09001cffu); wr8(g,out+6,colour); wr32(g,out+7,0x1c000211u); wr8(g,out+0xb,0);
    u64 text=bcall(g,0x3ad9a0,msg,0,0,0);
    bcall(g,0x3ae260,text,out+0xc,0,0);
    KH_RAX(g)=out;
}
void f_153850(KCtx *g){
    enter(B_153850);
    layout_icon(g,&icon_ca,0xe,0xe2,0);
    KH_RAX(g)=(u64)(u32)(s32)(s8)icon_ca;                                                     /* movsx eax,byte */
    KH_RET(g);
}
void f_15aab0(KCtx *g){
    enter(B_15AAB0);
    u64 out=A1(g); u8 colour=(u8)A2(g); u32 msg=(u32)A3(g);
    layout_icon(g,&icon_c9,0xc,0xe7,1);
    row_text(g,out,icon_c9,colour,msg,"15aab0 (DLL 0x10590)");
    KH_RET(g);
}
void f_15aba0(KCtx *g){
    enter(B_15ABA0);
    u64 out=A1(g); u8 colour=(u8)A2(g); u32 msg=(u32)A3(g);
    u64 list=need(g,A4(g),"15aba0 (DLL 0x10100)","the list (argument 5)");
    u64 owner=rd64(g,list+8);
    if(!GA_OK(owner))
        kh_fatal(g,"mod 15aba0 (DLL 0x10100): the list's owner [arg5+8] is not a valid guest pointer (0x%llx).  The exe tests it for null; the DLL reads "
                   "[owner+0xab0] unchecked and would fault%s",(unsigned long long)owner,
                 kh_moose_continue_on()?" (with KH2Moose's exception handler on, as the ini has it, the modded PC game would skip that read with r11 = 0, keep the previous icon and go on)":"");
    s32 type=(s32)rd32(g,owner+0xab0);
    /* types >= 0x10: the DLL's special-command vector first (empty as shipped) */
    static const u8 fixed[13]={0,0xde,0xdd,0xe2,0xe2,0xe3,0xdc,0xdc,0xe1,0xde,0xe2,0xe4,0xe0};   /* DLL tables 0x102d0 / 0x10280, types 1..12 */
    if(type>=1&&type<=12) icon_c8=fixed[type];
    else if(type>=0x64&&type<=0x6e) icon_c8=icon_cfg[type-0x64];
    else atomic_fetch_add(&c_icon_stale,1);                                                   /* every other type keeps the icon of the previous call */
    row_text(g,out,icon_c8,colour,msg,"15aba0 (DLL 0x10100)");
    KH_RET(g);
}

/* ---- DLL 0x2d670 -> exe 0x31dd70 (chains) --------------------------------------------------------------------------------------- */
#define H31 "31dd70 (DLL 0x2d670)"
/* the 10-byte record exe 0x152990 takes: three bytes, a zero byte, three words; in the DLL's stack frame */
static void jump_record(KCtx *g,u64 rec,u64 src,int o0,int o1,int o2,int o4,int o6,int o8,int sx){
    need(g,src,H31,"the record the jump is built from"); need(g,src+0x38,H31,"the record the jump is built from");
    wr8(g,rec,rd8(g,src+o0)); wr8(g,rec+1,rd8(g,src+o1)); wr8(g,rec+2,rd8(g,src+o2)); wr8(g,rec+3,0);
    u8 a=rd8(g,src+o4),b=rd8(g,src+o6),c=rd8(g,src+o8);
    wr16(g,rec+4,sx?(u16)(s16)(s8)a:a); wr16(g,rec+6,sx?(u16)(s16)(s8)b:b); wr16(g,rec+8,sx?(u16)(s16)(s8)c:c);
}
void f_31dd70(KCtx *g){
    enter(B_31DD70);
    u64 self=A0(g),rdx=A1(g),r8=A2(g),r9=A3(g),sp=KH_RSP(g);
    bcall(g,0x2f5f30,self,0,0,0);                                                             /* result unused; rcx is still the argument */
    u64 r=bcall(g,0x2f6230,0,0,0,0);
    if((u8)r){ atomic_fetch_add(&ext[B_31DD70],1); r=bcall(g,0x151730,0,0,0,0); KH_RAX(g)=(r&~0xffull)|1; KH_RET(g); return; }
    u64 cur=need(g,rd64(g,GS_BASE+0xbef528),H31,"the menu cursor pointer [0x140bef528]");     /* DLL data pointer 0x1800819a0 */
    u32 row=rd16(g,cur);
    u64 ids=need(g,rd64(g,GS_BASE+0xbef538),H31,"the menu row-id list [0x140bef538]");        /* DLL data pointer 0x1800819a8 */
    need(g,ids+2*(u64)row,H31,"the menu row id");
    s32 id=(s16)rd16(g,ids+2*(u64)row);
    u64 pad=bcall(g,0x39b760,0,0,0,0);
    if(pad&&(rd32(g,need(g,pad,H31,"the pad state returned by 0x39b760")+8)&0x1000000u)&&id>=0x200&&id<=0x204){
        atomic_fetch_add(&c_menu_custom[id-0x200],1);
        u64 rec=sp-0x80+0x30;                                                                 /* scratch below the hook's stack pointer, clear of kh_call's frame */
        KH_RSP(g)=sp-0x80;
        if(id==0x203){                                                                        /* DLL 0x2d7b7 */
            u64 t=need(g,bcall(g,0x3ad500,0,0,0,0),H31,"the record returned by 0x3ad500"); need(g,t+row+0x38,H31,"the record returned by 0x3ad500");
            bcall(g,0x31d850,(u32)(s32)(s8)rd8(g,t+row+0x38),0,0,0);
        } else {
            bcall(g,0x2eb790,0,0,0,0); bcall(g,0x1dd170,2,0,0,0); bcall(g,0x2f6500,0,0,0,0);
            if(id==0x202){                                                                    /* DLL 0x2d7cc */
                u64 t=need(g,bcall(g,0x3ad500,0,0,0,0),H31,"the record returned by 0x3ad500"); need(g,t+0x2b,H31,"the record returned by 0x3ad500");
                bcall(g,0x3aabe0,(u32)((s32)(s8)rd8(g,t+0x27)+(s32)row),(u32)((s32)(s8)rd8(g,t+0x2b)+(s32)row),0,0);
            } else if(id==0x204){                                                             /* DLL 0x2d718 */
                u64 t=bcall(g,0x3ad500,0,0,0,0);
                jump_record(g,rec,t,0x30,0x31,0x32,0x33,0x35,0x37,1);
                bcall5(g,0x152990,rec,1,1,1,0);
            } else {                                                                          /* 0x201: DLL 0x2d801 (+0x20), 0x200: DLL 0x2d876 (+0) */
                u64 w=GS_BASE+0x717008+(id==0x201?0x20:0);
                jump_record(g,rec,w,0,1,2,4,6,8,0);
                bcall5(g,0x152990,rec,1,1,1,0);
            }
        }
        KH_RSP(g)=sp;
        if(id!=0x204){ atomic_fetch_add(&ext[B_31DD70],1); KH_RAX(g)&=~0xffull; KH_RET(g); return; }   /* xor al,al */
    }
    borig(g,0x31dd70,orig_f_31dd70,self,rdx,r8,r9);                                            /* 0x204 falls through to the original as well */
}

/* ---- init, test access, exit report ---------------------------------------------------------------------------------------------- */
/* dll/tweaks/command_menu_offsets.bin as the DLL's loader 0x1f440 reads it: 16 header bytes (the entry count is the dword at +4), then
 * count 16-byte entries read in one go; a short read leaves the vector resized and zero-filled behind what was read. */
int kh_mod_b_load_command_menu(const char *path,char *err,size_t cap){
    free(cm_entries); cm_entries=NULL; cm_count=0; snprintf(cm_state,sizeof cm_state,"no file");
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    u8 hdr[16]; size_t got=fread(hdr,1,16,f);
    if(got!=16){ fclose(f); snprintf(cm_state,sizeof cm_state,"header short (%zu bytes): no entries, as the DLL",got); return 0; }
    u32 n; memcpy(&n,hdr+4,4);
    if(n>0x10000u){ fclose(f); snprintf(err,cap,"entry count %u is not plausible",n); return -1; }
    cm_entries=calloc(n?n:1,16); if(!cm_entries){ fclose(f); snprintf(err,cap,"out of memory"); return -1; }
    size_t rd=fread(cm_entries,1,16*(size_t)n,f); fclose(f);
    cm_count=n; snprintf(cm_state,sizeof cm_state,"%u entries%s",n,rd==16*(size_t)n?"":" (file shorter than its count: the rest is zero)");
    return 1;
}
/* The seven DetourAttach calls of the DLL that the shipped ini files leave out (each sits behind an option that is off as shipped,
 * in an option initialiser; the reader is called for retry at 0x1d2f5, patch_gummiskip at 0x1ffa5, savepoint_qol at 0x20165).
 * Not ported: a data tree that turns one of them on stops at start-up instead of silently running without it.
 * -1 = no such ini / key under <root>/dll, 0 = present and not on, 1 = on.
 * "On" is what the DLL's boolean option reader 0x2a840 answers TRUE for.  Its string reader 0x2a540 calls
 * GetPrivateProfileStringA(section, key, "", buffer, 0x200, <DLL directory>/<ini>) (the API trims the value and drops one pair of
 * enclosing quotes), trims " \t\r\n" (the set at 0x74364; 0x2b9b0 / 0x2b8a0) and hands back the empty default for an empty value
 * or one that starts with '#' or ';'.  0x2a840 then lower-cases every character (0x2a8f0..0x2a905, tolower 0x4a580) and returns
 * TRUE for exactly "on" (0x7436c, test at 0x2a9ad), "true" (0x72db0, 0x2a9d3) or "1" (0x74370, 0x2a9f9); "off", "false", "0", the
 * empty string and everything else are FALSE (0x2aa85).  An inline comment is part of the value and so makes it FALSE. */
static int option_value_on(char *p){
    static const char ws[]=" \t\r\n";
    size_t n=strlen(p);
    while(n&&strchr(ws,p[n-1])) p[--n]=0;                                                    /* the API: no trailing blanks */
    if(n>=2&&(*p=='"'||*p=='\'')&&p[n-1]==*p){ p[--n]=0; p++; n--; }                          /* the API: one pair of enclosing quotes */
    while(n&&strchr(ws,p[n-1])) p[--n]=0;                                                    /* the DLL's own trim, both ends */
    while(*p&&strchr(ws,*p)) p++;
    return !strcasecmp(p,"on")||!strcasecmp(p,"true")||!strcmp(p,"1");
}
static const struct { const char *ini, *key, *what; } off_options[]={
    {"KH2_Tweaks_Retry.ini","retry","5 detours (DLL handlers 0x30d70, 0x30df0, 0x2fce0, 0x324a0, 0x32550: retry / continue menu)"},
    {"KH2_Tweaks_Savepoint.ini","savepoint_qol","1 detour (DLL handler 0x329a0)"},
    {"KH2_Tweaks_Worlds.ini","patch_gummiskip","1 detour (DLL handler 0x33870)"}};
int kh_mod_b_option_on(const char *root,const char *ini,const char *key){
    char rel[300],host[4096],line[512]; snprintf(rel,sizeof rel,"dll/%s",ini);
    if(!kh_resolve_ci(root,rel,host,sizeof host)) return -1;
    FILE *f=fopen(host,"r"); if(!f) return -1;
    int res=-1, in_settings=0; size_t kn=strlen(key);
    while(fgets(line,sizeof line,f)){
        char *p=line; while(*p==' '||*p=='\t') p++;
        if(*p=='['){ in_settings=!strncasecmp(p,"[Settings]",10); continue; }
        if(!in_settings||strncasecmp(p,key,kn)) continue;
        p+=kn; while(*p==' '||*p=='\t') p++;
        if(*p!='=') continue;
        p++; while(*p==' '||*p=='\t') p++;
        res=option_value_on(p); break;                                                       /* the first occurrence counts */
    }
    fclose(f); return res;
}
void kh_mod_b_check_options(KCtx *g,const char *root){
    for(unsigned i=0;i<sizeof off_options/sizeof *off_options;i++)
        if(kh_mod_b_option_on(root,off_options[i].ini,off_options[i].key)==1)
            kh_fatal(g,"mod-option-not-ported: %s/dll/%s has %s set to on (the DLL's option reader 0x2a840 takes on, true and 1, in any letter case): "
                       "KH2Tweaks_Refactor.dll would attach %s; the shipped mod has this option off and the port does not implement it",
                     root,off_options[i].ini,off_options[i].key,off_options[i].what);
}
void kh_mod_hooks_b_init(KCtx *g){
    if(lists_va) kh_fatal(g,"mod-b-reinit: the DLL's command word lists are already allocated");
    lists_va=ga_calloc(1,32); names_va=ga_calloc(25,16);
    if(!lists_va||!names_va) kh_fatal(g,"mod-b-lists: allocation of the DLL's command word lists / file names failed");
    for(int k=0;k<5;k++) snprintf(hp(g,NAME_COMMON(k)),16,"%02dcommon.bdx",k+1);            /* DLL strings 0x74bd8, 0x74bc8, 0x74bf8, 0x74be8, 0x74c18 */
    for(int n=1;n<=20;n++) snprintf(hp(g,NAME_EFFECT(n)),16,"%02deffect.bar",n);             /* DLL format 0x74c08 */
    /* the DLL's data files sit next to it (it takes its own path from GetModuleHandleEx / GetModuleFileName): <data>/dll/tweaks/ */
    const char *root=getenv("KHNMC_DATA"); char host[4096],err[160]="";
    kh_mod_b_check_options(g,root?root:"data");
    if(kh_resolve_ci(root?root:"data","dll/tweaks/special_command_menu.bin",host,sizeof host))
        kh_fatal(g,"mod-b-special-command-menu: %s exists; the DLL would load its 32-byte entries into the special-command vector used by the detours on "
                   "exe 0x4060e0 and 0x15aba0, which this port implements for the empty vector of the shipped mod only",host);
    if(kh_resolve_ci(root?root:"data","dll/KH2_Tweaks_SpecialCommandList.ini",host,sizeof host))
        kh_fatal(g,"mod-b-special-command-list: %s exists; the DLL would read 11 command lists and icon bytes from it (0x180081860..6a), not ported: the shipped mod has no such file",host);
    if(kh_resolve_ci(root?root:"data","dll/tweaks/command_menu_offsets.bin",host,sizeof host)){
        if(kh_mod_b_load_command_menu(host,err,sizeof err)<0) kh_fatal(g,"mod-b-command-menu: %s: %s",host,err);
    } else snprintf(cm_state,sizeof cm_state,"no file");
    fprintf(stderr,"kh: mod B: command-menu icon table (dll/tweaks/command_menu_offsets.bin): %s\n",cm_state);
}
u64 kh_mod_b_lists(void){ return lists_va; }
u64 kh_mod_b_names(void){ return names_va; }
int kh_mod_b_counts(int hook,u64 *calls){ if(hook<0||hook>=B_N) return 0; *calls=atomic_load(&hits[hook]); return 1; }
void kh_mod_b_icons(u8 *c8,u8 *c9,u8 *ca,int set){ if(set){ icon_c8=*c8; icon_c9=*c9; icon_ca=*ca; } else { *c8=icon_c8; *c9=icon_c9; *ca=icon_ca; } }
u32 kh_mod_b_command_menu(const u8 **entries){ if(entries) *entries=cm_entries; return cm_count; }
void kh_mod_hooks_b_dump(void){
    fprintf(stderr,"kh: mod B 3f29e0 calls=%llu pending-menu tail jumps=%llu no-sheet returns=%llu party rows=%llu limit-form lists=%llu limit-form shortcut slots=%llu extra-command (0x3e7) lists=%llu\n",
        (unsigned long long)atomic_load(&hits[B_3F29E0]),(unsigned long long)atomic_load(&c_tail),(unsigned long long)atomic_load(&c_early),(unsigned long long)atomic_load(&c_party_rows),
        (unsigned long long)atomic_load(&c_limit_lists),(unsigned long long)atomic_load(&c_limit_slots),(unsigned long long)atomic_load(&c_extra_cmd));
    fprintf(stderr,"kh: mod B 3f27d0 calls=%llu with a non-empty extra list=%llu\n",(unsigned long long)atomic_load(&hits[B_3F27D0]),(unsigned long long)atomic_load(&c_reset_extra));
    for(int i=B_3C4780;i<B_N;i++)
        fprintf(stderr,"kh: mod B %s calls=%llu answered-by-the-DLL-code=%llu\n",hook_names[i],(unsigned long long)atomic_load(&hits[i]),(unsigned long long)atomic_load(&ext[i]));
    fprintf(stderr,"kh: mod B extra-file probes (01..05common.bdx, 01..20effect.bar through exe 0x3f95f0)=%llu, none found (a hit is a named fatal)\n",(unsigned long long)atomic_load(&c_probes));
    fprintf(stderr,"kh: mod B command-menu icons (%s): layout matched / not matched: 153850 %llu / %llu, 15aab0 %llu / %llu; 15aba0 rows that kept the previous icon=%llu; last icon bytes c8=%02x c9=%02x ca=%02x\n",cm_state,
        (unsigned long long)atomic_load(&c_icon_match[0]),(unsigned long long)atomic_load(&c_icon_default[0]),(unsigned long long)atomic_load(&c_icon_match[1]),(unsigned long long)atomic_load(&c_icon_default[1]),
        (unsigned long long)atomic_load(&c_icon_stale),icon_c8,icon_c9,icon_ca);
    fprintf(stderr,"kh: mod B 31dd70 custom rows confirmed: 0x200=%llu 0x201=%llu 0x202=%llu 0x203=%llu 0x204=%llu\n",(unsigned long long)atomic_load(&c_menu_custom[0]),(unsigned long long)atomic_load(&c_menu_custom[1]),
        (unsigned long long)atomic_load(&c_menu_custom[2]),(unsigned long long)atomic_load(&c_menu_custom[3]),(unsigned long long)atomic_load(&c_menu_custom[4]));
}
