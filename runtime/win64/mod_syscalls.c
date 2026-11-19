/* KH2Tweaks_Refactor table 3, independently transcribed from DLL 15120..15cd1.
 * Descriptors are {guest fn:u64, argc/return flags:u64}; RCX points at u64 VM slots.
 * The vanilla slot is empty: the 114 entries start at index zero, not after another table.
 */
#include "khnmc_rt.h"
#include "xion_swap.h"
#include <math.h>

static const struct { const char *name; u32 flags, dll_rva; } entries[] = {
    {"Set_Action", 0x00000002u, 0x115c0}, /* 0 */
    {"AddExp", 0x00000002u, 0x115f0}, /* 1 */
    {"AddFormExp", 0x00000001u, 0x11730}, /* 2 */
    {"ConvertPointer", 0x40000002u, 0x11760}, /* 3 */
    {"Get_Datafile_Objentry", 0x40000000u, 0x11790}, /* 4 */
    {"Get_Datafile_System", 0x40000000u, 0x117c0}, /* 5 */
    {"Get_Datafile_Battle", 0x40000000u, 0x117f0}, /* 6 */
    {"PlaySystemPax", 0x40000006u, 0x11820}, /* 7 */
    {"MapJump", 0x00000009u, 0x118b0}, /* 8 */
    {"CheckIsHeartless", 0x40000001u, 0x11930}, /* 9 */
    {"CheckIsNobody", 0x40000001u, 0x119a0}, /* 10 */
    {"CallCampMenu", 0x00000002u, 0x11a10}, /* 11 */
    {"AddDrive", 0x00000002u, 0x11a20}, /* 12 */
    {"AddMP", 0x00000002u, 0x11a50}, /* 13 */
    {"UseMP", 0x00000002u, 0x11a80}, /* 14 */
    {"GetAttackParam", 0x40000002u, 0x11ab0}, /* 15 */
    {"GetCommandElem", 0x40000001u, 0x11af0}, /* 16 */
    {"GetItemParam", 0x40000001u, 0x11b30}, /* 17 */
    {"GetItemTable", 0x40000001u, 0x11b80}, /* 18 */
    {"GetPartyAttackTable", 0x40000001u, 0x11bc0}, /* 19 */
    {"LoadObjentry", 0x40000003u, 0x11c00}, /* 20 */
    {"SpawnObjentry", 0x40000002u, 0x11c20}, /* 21 */
    {"OPENBOXINFO", 0x00000003u, 0x115b0}, /* 22 */
    {"OPENBOXINFO", 0x40000002u, 0x115b0}, /* 23 */
    {"OPENBOXINFO", 0x40000003u, 0x115b0}, /* 24 */
    {"GetCommandMenuSlot", 0x40000000u, 0x11c80}, /* 25 */
    {"GetCommandMenuCurrent", 0x40000000u, 0x11cb0}, /* 26 */
    {"GetCommandMenuCurrentID", 0x40000000u, 0x11ce0}, /* 27 */
    {"GetMessageData", 0x40000001u, 0x11d10}, /* 28 */
    {"OpenLevelWindow", 0x00000005u, 0x11d50}, /* 29 */
    {"OpenBonusWindow", 0x00000005u, 0x11dc0}, /* 30 */
    {"OpenFormLevelWindow", 0x00000005u, 0x11e30}, /* 31 */
    {"OpenSummonLevelWindow", 0x00000003u, 0x11ea0}, /* 32 */
    {"SheetReload", 0x00000001u, 0x11ef0}, /* 33 */
    {"PadIsTrigger", 0x40000001u, 0x11f20}, /* 34 */
    {"PadIsButton", 0x40000001u, 0x11f60}, /* 35 */
    {"PartramUnequipAbility", 0x00000002u, 0x11fb0}, /* 36 */
    {"PartramEquipAbility", 0x00000002u, 0x12030}, /* 37 */
    {"PartramDeleteAbility", 0x00000002u, 0x120c0}, /* 38 */
    {"FormramUnequipAbility", 0x00000002u, 0x12140}, /* 39 */
    {"FormramEquipAbility", 0x00000002u, 0x121c0}, /* 40 */
    {"FormramDeleteAbility", 0x00000002u, 0x12250}, /* 41 */
    {"GlowOn", 0x00000000u, 0x122d0}, /* 42 */
    {"GlowOff", 0x00000000u, 0x122e0}, /* 43 */
    {"BgmFadeOut", 0x00000002u, 0x122f0}, /* 44 */
    {"BgmPlay", 0x00000006u, 0x12300}, /* 45 */
    {"AbilityCount", 0x40000002u, 0x12330}, /* 46 */
    {"GetGrowthTable", 0x40000000u, 0x128f0}, /* 47 */
    {"GetCurrentRC_MEM", 0x40000000u, 0x12920}, /* 48 */
    {"IsRCBlocked", 0x40000000u, 0x12950}, /* 49 */
    {"GetCurrentRC_MEM", 0x40000000u, 0x12920}, /* 50 */
    {"SetCommandMenuType", 0x40000001u, 0x12980}, /* 51 */
    {"SetCommandMenuSubMenu", 0x00000001u, 0x129b0}, /* 52 */
    {"GetCameraEye", 0x40000000u, 0x129e0}, /* 53 */
    {"GetCameraAt", 0x40000000u, 0x12a10}, /* 54 */
    {"SetCameraMode", 0x00000001u, 0x12a40}, /* 55 */
    {"SetCameraFocus", 0x00000001u, 0x12a50}, /* 56 */
    {"SetCameraLockonFocus", 0x00000001u, 0x12aa0}, /* 57 */
    {"OpenMissionWindow_KH2SCII", 0x00000001u, 0x12af0}, /* 58 */
    {"OpenLevelWindow_KH2SCII", 0x00000005u, 0x12c70}, /* 59 */
    {"OpenBonusWindow_KH2SCII", 0x00000005u, 0x13020}, /* 60 */
    {"OpenFormLevelWindow_KH2SCII", 0x00000005u, 0x133d0}, /* 61 */
    {"OpenSummonLevelWindow_KH2SCII", 0x00000003u, 0x13780}, /* 62 */
    {"OpenDisableReason_KH2SCII", 0x00000002u, 0x13a20}, /* 63 */
    {"OpenRoomText_KH2SCII", 0x00000001u, 0x13ba0}, /* 64 */
    {"ResetRoomText_KH2SCII", 0x00000000u, 0x13d40}, /* 65 */
    {"SetPauseModeSelector", 0x00000001u, 0x13d60}, /* 66 */
    {"CHANGEBGM", 0x00000006u, 0x13e60}, /* 67 */
    {"PLAYVSB", 0x00000002u, 0x13ee0}, /* 68 */
    {"WriteByte", 0x00000002u, 0x14040}, /* 69 */
    {"WriteShort", 0x00000002u, 0x14060}, /* 70 */
    {"ReadByte", 0x40000001u, 0x14080}, /* 71 */
    {"ReadShort", 0x40000001u, 0x140a0}, /* 72 */
    {"GetTreasureEvent", 0x00000001u, 0x140c0}, /* 73 */
    {"GetTreasure", 0x00000003u, 0x140f0}, /* 74 */
    {"PlayMovie", 0x00000001u, 0x14170}, /* 75 */
    {"SpawnObjentry", 0x00000006u, 0x11c20}, /* 76 */
    {"CallCommon", 0x00000007u, 0x142d0}, /* 77 */
    {"ItemGetNumBackyard", 0x40000001u, 0x14520}, /* 78 */
    {"HasBonusLV", 0x40000001u, 0x14540}, /* 79 */
    {"PlayerParamApply", 0x00000001u, 0x14560}, /* 80 */
    {"SetDamageMultiplierParty", 0x00000002u, 0x14570}, /* 81 */
    {"SetDamageMultiplierEnemy", 0x00000002u, 0x145b0}, /* 82 */
    {"OpenPrizeInfo_KH2SCII", 0x00000001u, 0x145f0}, /* 83 */
    {"OpenPrizeInfo", 0x00000001u, 0x14770}, /* 84 */
    {"OpenDisableReason", 0x00000002u, 0x147a0}, /* 85 */
    {"ItemGetMultiple", 0x00000003u, 0x147e0}, /* 86 */
    {"SetDriveMultiplier", 0x00000001u, 0x14820}, /* 87 */
    {"GetRandom", 0x40000001u, 0x14830}, /* 88 */
    {"GetRandomFloat", 0x40000001u, 0x148d0}, /* 89 */
    {"GetRandomFloatRange", 0x40000002u, 0x14980}, /* 90 */
    {"GetBonusLV", 0x40000000u, 0x14a40}, /* 91 */
    {"GetSaveram", 0x40000000u, 0x14a60}, /* 92 */
    {"PartyJump", 0x00000000u, 0x14a90}, /* 93 */
    {"CalcDamage", 0x40000003u, 0x14ac0}, /* 94 */
    {"PrintInt", 0x00000002u, 0x14ba0}, /* 95 */
    {"PrintFloat", 0x00000002u, 0x14c30}, /* 96 */
    {"PrintString", 0x00000002u, 0x14cc0}, /* 97 */
    {"PrintLong", 0x00000002u, 0x14d60}, /* 98 */
    {"WriteArray", 0x00000003u, 0x14e00}, /* 99 */
    {"SetSystemPax", 0x00000002u, 0x14e50}, /* 100 */
    {"GetSystemPax", 0x40000001u, 0x14ec0}, /* 101 */
    {"CastMagic", 0x00000001u, 0x14f30}, /* 102 */
    {"CastMagicCommand", 0x00000001u, 0x14f60}, /* 103 */
    {"GetComboPosition", 0x40000000u, 0x14f80}, /* 104 */
    {"GetFinisherPosition", 0x40000000u, 0x14fa0}, /* 105 */
    {"IsComboFinisher", 0x40000000u, 0x14fc0}, /* 106 */
    {"SetComboPosition", 0x00000001u, 0x15040}, /* 107 */
    {"SetFinisherPosition", 0x00000001u, 0x15060}, /* 108 */
    {"SetEnemyKarma", 0x00000002u, 0x15080}, /* 109 */
    {"GetKarmaFillRate", 0x40000001u, 0x150b0}, /* 110 */
    {"BlowRecov", 0x00000001u, 0x150d0}, /* 111 */
    {"GetLockonType", 0x40000000u, 0x150e0}, /* 112 */
    {"AttackGetHitObject", 0x40000001u, 0x15100}, /* 113 */
};
_Static_assert(sizeof entries / sizeof *entries == 114, "DLL table size");
static u64 table, drive_multiplier, sstm_name;
static u64 targets[114];
static _Atomic unsigned hits[114];

static void require_mem(KCtx *g,u64 p,u64 n){
    if(!GA_OK(p) || n>GS_BASE+gs_size-p)
        kh_fatal(g,"mod syscall invalid guest memory 0x%llx + 0x%llx (return RVA 0x%llx)",
                 (unsigned long long)p,(unsigned long long)n,(unsigned long long)(guest_return_addr(g)-GS_BASE));
}
static u64 call(KCtx *g,u32 rva,u64 a,u64 b){ return kh_call(g,GS_BASE+rva,a,b,0,0); }
static u64 decode(KCtx *g,u32 id){ return call(g,0x4ad270,id,0); }
static u32 encode(KCtx *g,u64 p){ return (u32)call(g,0x4ad240,p,0); }
static float rf(KCtx *g,u64 p){ u32 v=rd32(g,p); float f; memcpy(&f,&v,4); return f; }
static float xf(KCtx *g,int i){ float f; memcpy(&f,&KH_XMM(g,i),4); return f; }

static int item_ability(KCtx *g,s16 id,u16 ability,int skip_null){
    u64 item=call(g,0x3e39a0,(u32)(s32)id,0);
    if(skip_null && !item) return 0;
    require_mem(g,item,8);
    u64 info=call(g,0x3e3ef0,item,0);
    if(!info) return 0;
    require_mem(g,info,4);
    return rd16(g,info+2)==ability;
}
static int enabled_ability(u16 v,u16 ability){
    /* DLL clears bit 15 by a 16-bit shift pair, preserving the sign of bit 14. */
    return (v&0x8000) && (u16)((s16)(u16)(v<<1)>>1)==ability;
}
static void ability_count(KCtx *g,u64 args){
    u16 ability=rd16(g,args+8);
    u64 actor=decode(g,rd32(g,args)); require_mem(g,actor,0x5c8);
    u64 sheet=rd64(g,actor+0x5c0); require_mem(g,sheet,0x258);
    u32 party_id=rd32(g,sheet+0x250);
    if(!party_id) return; /* DLL leaves slot 0 unchanged on this path. */
    u64 party=decode(g,party_id), form=decode(g,rd32(g,sheet+0x254));
    require_mem(g,party,0xf4);
    int count=item_ability(g,(s16)rd16(g,party),ability,0);
    for(int k=0;k<2;k++)
        for(int i=0;i<(s8)rd8(g,party+0x10+k);i++){
            u64 p=party+0x14+0x10*k+2*i; require_mem(g,p,2);
            s16 id=(s16)rd16(g,p);
            if(id) count+=item_ability(g,id,ability,1);
        }
    for(int i=0;i<80;i++) count+=enabled_ability(rd16(g,party+0x54+2*i),ability);
    if(form){
        require_mem(g,form,0x38);
        /* DLL tests form's weapon but deliberately looks up party's weapon again. */
        if(rd16(g,form)) count+=item_ability(g,(s16)rd16(g,party),ability,0);
        for(int i=0;i<24;i++) count+=enabled_ability(rd16(g,form+8+2*i),ability);
    }
    wr32(g,args,count);
}
/* Task 23: the engine calls of the five syscalls added there (set action, map jump, party jump) need a live game behind them, so
 * they go through one seam that the selftest can replace by a recorder; NULL in normal runs. */
u64 (*kh_mod_syscalls_call_hook)(KCtx *g,u32 rva,const u64 *a5);
static u64 ecall5(KCtx *g,u32 rva,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){
    if(kh_mod_syscalls_call_hook){ u64 a[5]={a0,a1,a2,a3,a4}; u64 r=kh_mod_syscalls_call_hook(g,rva,a); KH_RAX(g)=r; return r; }
    return kh_call5(g,GS_BASE+rva,a0,a1,a2,a3,a4);
}
static void syscall_handler(KCtx *g){
    u64 target=g->aux[0]; int idx;
    for(idx=0;idx<114 && targets[idx]!=target;idx++);
    if(idx==114) kh_fatal(g,"unknown mod syscall target 0x%llx",(unsigned long long)target);
    if(!atomic_fetch_add(&hits[idx],1))
        fprintf(stderr,"kh: mod syscall table=3 index=%d name=%s call-site RVA=0x%llx\n",idx,entries[idx].name,
               (unsigned long long)(guest_return_addr(g)-GS_BASE-2));
    u64 args=A0(g),p,q; u32 a,b;
    require_mem(g,args,8ull*((entries[idx].flags&0xffff)?(entries[idx].flags&0xffff):1));
    a=rd32(g,args); b=(entries[idx].flags&0xffff)>1?rd32(g,args+8):0;
    switch(idx){
    case 0: /* 115c0 Set_Action: 0x3b4da0(0x4ad270(arg0), arg1), a tail jump in the DLL (the VM gets 0x3b4da0's return). 0x3b4da0 reads
             * [object+0xc] at once, so a handle that decodes to no object faults on the PC: named fatal. */
        p=decode(g,a); require_mem(g,p,0xbac); ecall5(g,0x3b4da0,p,b,0,0,0); break;
    case 8: { /* 118b0 MapJump: nine arguments.  A 10-byte record in the handler's stack frame: the low BYTE of arguments 0..2, a zero
               * byte, the low WORD of arguments 3..5; then exe 0x152990(record, arg6, arg7, 0, arg8) (edx, r8d, r9 = 0, 5th = a dword). */
        u64 sp=KH_RSP(g), rec=sp-0x80+0x30;            /* scratch below the handler's stack pointer, clear of kh_call's frame */
        wr8(g,rec,rd8(g,args)); wr8(g,rec+1,rd8(g,args+8)); wr8(g,rec+2,rd8(g,args+0x10)); wr8(g,rec+3,0);
        wr16(g,rec+4,rd16(g,args+0x18)); wr16(g,rec+6,rd16(g,args+0x20)); wr16(g,rec+8,rd16(g,args+0x28));
        KH_RSP(g)=sp-0x80;
        ecall5(g,0x152990,rec,rd32(g,args+0x30),rd32(g,args+0x38),0,rd32(g,args+0x40));
        KH_RSP(g)=sp; break; }
    case 47: /* 128f0 GetGrowthTable: the handle (0x4ad240) of the exe's form-growth table.  The DLL's pointer 0x180081898 is the
              * rip-relative operand at exe 0x3e8380+0x1a2 = 0x1405c9030 (the table the mod's form_growth edits rewrite). */
        wr32(g,args,encode(g,GS_BASE+0x5c9030)); break;
    case 55: /* 12a40 SetCameraMode: `*p = (u8)arg0` with p = [0x180081870] = exe 0x140718ca8 (the low byte of the camera mode dword).
              * That pointer is computed by the DLL's worker thread (0x342a0) only once the game global [0x142ae5768] (the pointer
              * exe 0x3e9300 reads) is non-zero, polled every 50 ms; until then it is NULL and the store faults on the PC. */
        if(!rd64(g,GS_BASE+0x2ae5768))
            kh_fatal(g,"mod syscall SetCameraMode (table 3 index 55, DLL 0x12a40) before the DLL's worker thread can have set its pointer: the game global "
                       "[0x142ae5768] is still 0, the DLL's pointer 0x180081870 is NULL and the store faults on the PC (call-site RVA=0x%llx)",
                     (unsigned long long)(guest_return_addr(g)-GS_BASE-2));
        wr8(g,GS_BASE+0x718ca8,(u8)a); break;
    case 93: /* 14a90 PartyJump: exe 0x3cb9f0(0, 0x3b3e80(), 1) as a tail jump (r8b = 1). */
        p=ecall5(g,0x3b3e80,0,0,0,0,0); ecall5(g,0x3cb9f0,0,p,1,0,0); break;
    case 1: { extern void kh_mod_add_exp(KCtx *,u64); kh_mod_add_exp(g,args); break; }
    case 3: /* 11760: both halves are sign extended before OR, including the low half. */
        p=((u64)(s64)(s32)a<<32)|(u64)(s64)(s32)b;
        wr32(g,args,encode(g,p)); break;
    case 13: p=decode(g,a); require_mem(g,p,0x26c); call(g,0x3c0cd0,p,b); break;
    case 15: p=call(g,0x3eafc0,a,b); wr32(g,args,encode(g,p)); break;
    case 33: p=decode(g,a); require_mem(g,p,0x26c); call(g,0x3c2100,p,0); break;
    case 34: case 35:
        p=call(g,0x39b760,0,0); q=(u64)(s64)(s32)(1u<<(a&31));
        if(p) require_mem(g,p,16);
        wr32(g,args,p && (rd64(g,p+(idx==34?8:0))&q)!=0); break;
    case 46: ability_count(g,args); break;
    case 48: case 50:   /* task 39 (review c1/c2, both families): descriptor 50 is the DLL's second entry for the SAME handler 0x12920
                         * (GetCurrentRC_MEM, flags 0x40000000 as 48): on the PC both run that code, so 50 is an alias, not a fatal */
        p=call(g,0x3b3e80,0,0); require_mem(g,p,0xdd8);
        p=rd64(g,p+0xdd0); require_mem(g,p,0xb43);
        wr32(g,args,rd8(g,p+0xb42)); break;
    case 69: p=decode(g,a); require_mem(g,p,1);
        { extern void kh_mod_item_writebyte(KCtx *,u64,u8); kh_mod_item_writebyte(g,p,(u8)b); } /* Task 16 counter only */
        wr8(g,p,(u8)b); break;
    case 78: wr32(g,args,(u32)call(g,0x3c4720,a,0)); break;
    case 80: call(g,0x3e4530,a,0); break;
    case 87: wr32(g,drive_multiplier,a); break;
    default:
        kh_fatal(g,"unimplemented mod syscall table=3 index=%d name=%s DLL RVA=0x%x call-site RVA=0x%llx",
                 idx,entries[idx].name,entries[idx].dll_rva,(unsigned long long)(guest_return_addr(g)-GS_BASE-2));
    }
    KH_RET(g);
}

static _Atomic u64 reset_calls;                  /* calls of the 0x1f9a80 hook below */
/* exit report (kh_mod_hooks_dump): every table-3 syscall the run called, and the five added by Task 23 even when they were not */
void kh_mod_syscalls_dump(void){
    fprintf(stderr,"kh: mod syscalls (table 3): calls by index:");
    int any=0; for(int i=0;i<114;i++){ unsigned n=atomic_load(&hits[i]); if(n){ fprintf(stderr," %d %s=%u",i,entries[i].name,n); any=1; } }
    if(!any) fprintf(stderr," none");
    fprintf(stderr,"; Task 23 additions: 0 Set_Action=%u, 8 MapJump=%u, 47 GetGrowthTable=%u, 55 SetCameraMode=%u, 93 PartyJump=%u; 1f9a80 (DLL 0x2d110) resets=%llu\n",
            atomic_load(&hits[0]),atomic_load(&hits[8]),atomic_load(&hits[47]),atomic_load(&hits[55]),atomic_load(&hits[93]),(unsigned long long)atomic_load(&reset_calls));
}
void kh_mod_syscalls_init(KCtx *g){
    if(table) kh_fatal(g,"mod syscall table initialized twice");
    if(rd64(g,GS_BASE+0x7534a8)) kh_fatal(g,"expected empty vanilla syscall table 3");
    table=ga_calloc(114,16); drive_multiplier=ga_alloc(4,16); sstm_name=ga_strdup("sstm");
    if(!table || !drive_multiplier || !sstm_name) kh_fatal(g,"mod syscall allocation failed");
    wr32(g,drive_multiplier,0x3f800000); /* shipped DLL drive_add_mult=1.0 */
    for(int i=0;i<114;i++){
        char name[128]; snprintf(name,sizeof name,"mod.syscall.3.%d.%s",i,entries[i].name);
        targets[i]=kh_host_fn(name,syscall_handler);
        wr64(g,table+16*i,targets[i]); wr64(g,table+16*i+8,entries[i].flags);
    }
    /* The two four-byte static edits are halves of this one pointer. No count field exists. */
    wr64(g,GS_BASE+0x7534a8,table);
    fprintf(stderr,"kh: mod syscall table 3 installed: 114 descriptors at 0x%llx (19 implemented, 95 named fatals)\n",(unsigned long long)table);
}

/* DLL 0x2d110 -> exe 0x1f9a80.  FULL REPLACEMENT (Task 23 audit: the handler never reaches its trampoline slot 0x81d90): it stores the
 * DLL's table pointer into directory slot 3 (`[[0x180081a08]+0x18] = [0x1800817f8]`, 0x180081a08 = exe 0x140753490) and tail-jumps
 * to exe 0x3e1620 (slot 0x829c8), the function the exe body tail-jumps to after its `0x3e1710(0)`.  The earlier port called the
 * original first and restored the pointer afterwards: 0x3e1620 (which resets every live script through 0x3e1410, i.e. can run
 * script code) then ran with slot 3 = NULL.  Now the order is the DLL's: the table is in place while 0x3e1620 runs. */
u64 kh_mod_syscalls_reset_calls(void){ return atomic_load(&reset_calls); }
KhFn kh_mod_syscalls_reset_tail;              /* test only: stands in for exe 0x3e1620 */
void f_1f9a80(KCtx *g){
    atomic_fetch_add(&reset_calls,1);
    wr64(g,GS_BASE+0x7534a8,table);
    if(kh_mod_syscalls_reset_tail){ kh_mod_syscalls_reset_tail(g); return; }
    kh_dispatch(g,GS_BASE+0x3e1620);          /* tail jump: its ret pops our caller's return address */
}

/* DLL 327e0: the sole game consumer of SetDriveMultiplier's private float. */
void f_3a9cf0(KCtx *g){
    u64 actor=A0(g); float amount=xf(g,1);
    // Drive orbs collected by prototype Xion belong to the canonical campaign sheet.
    if(kh_xion_enabled&&kh_xion_progress_player()&&actor==rd64(g,GS_BASE+0x2a105d0)&&rd64(g,KH_RSP(g))==GS_BASE+0x3dcae8)
        actor=kh_xion_progress_player();
    require_mem(g,actor,0xde4);
    u64 sheet=rd64(g,actor+0x5c0); require_mem(g,sheet,0x210);
    call(g,0x3a3990,actor,0); float rate=xf(g,0);
    u8 mode=rd8(g,sheet+0x1af);
    if(mode==1){
        if(rf(g,sheet+0x1c0)>0){
            u64 sstm=call(g,0x3e9300,2,sstm_name); require_mem(g,sstm,0x84);
            amount=amount*rf(g,sstm+0x80); amount=amount*rf(g,sheet+0x20c);
        }
        amount=amount*rf(g,drive_multiplier); amount=amount*rate;
        s32 n=(!isfinite(amount)||amount>=2147483648.0f||amount< -2147483648.0f)?INT32_MIN:(s32)amount;
        call(g,0x3c0ad0,sheet,(u32)n);
    }else if(mode==3 || (mode==2 && rd32(g,actor+0xde0)!=12)){
        u64 sstm=call(g,0x3e9300,2,sstm_name); require_mem(g,sstm,0x7c);
        float v=amount*rf(g,sstm+0x78); memcpy(&KH_XMM(g,1),&v,4);
        call(g,0x3c0b50,sheet,0);
    }
    KH_RET(g);
}
