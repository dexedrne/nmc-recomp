/* mod_form_rewards.c : T101 - the PS2 version's growth rewards of Bladeless (Wisdom slot) and Light & Dark (Limit slot).
 *
 * PS2 NMC V0.20 lines 6317-6329 rewrite the executable's form-growth table (PS2 0x36E5A0, PC exe 0x5c9030: five words per form,
 * row[form-1] = {0, LV1, LV2, LV3, MAX}) as the form's level byte passes 2 and 4.  The PC mod zeroes both rows instead (ini
 * wisdom_growth / limit_growth = 0,0,0,0,0): no reward.
 *
 * The level-up (exe f_3e8380, same algorithm as PS2 FUN_001d5ec0) runs when the growth-ability level n of the new form level is
 * above the one before (fmlv byte +1: 1,1,2,2,3,3,4 for forms 2 and 3, so at LV3 / LV5 / LV7).  In the base character's 80 slots
 * (save +0x54) it finds row[n-2] and writes row[n-1] over it (0x3e8588..0x3e85c9) and names row[n-1] in the level-up window; in
 * the form's 24 slots it finds row[n-1] and writes row[n] (0x3e852f..0x3e8569); id 0 gives no line (0x3ed400).  A zero "find"
 * value matches the first EMPTY slot, so a row whose only non-zero word is the reward of the NEXT step makes the engine ADD that
 * reward and keep the earlier ones (a static row {0,A,B,C,0} would turn A into B into C in one slot, as it did on PS2):
 *     level 0..2 -> {0,R1,0,0,0}    3..4 -> {0,0,R2,0,0}    5..6 -> {0,0,0,R3,0}    7.. -> {0,0,0,0,0}
 * This file only writes those two rows once per game frame; the grant itself and its message are the engine's.  A form whose row
 * was set by the user (KHNMC_TWEAK_FORMS_WISDOM_GROWTH / _LIMIT_GROWTH not all zero) is left alone.
 * Switch: KHNMC_PS2_FORM_REWARDS (on / off); unset = on when the Roxas May Cry package is installed, as KHNMC_PS2_CASTLE. */
#include "rmc.h"
#include "khnmc_rt.h"
#include <stdlib.h>
#include <string.h>

#define GROWTH_TABLE (GS_BASE+0x5c9030)          /* exe .rdata; the operand of 0x3e851f */
#define SAVE_PARTY   (GS_BASE+0x9abda0)          /* rcx of exe 0x3a1410: form block f at +0xe04+0x38*(f-1), level byte at +2 */
#define FMLV_POINTER (GS_BASE+0x2ae5708)         /* the loaded fmlv table (00battle.bin): count at +4, 8-byte records at +8 */

/* {form, reward of the step to LV3, LV5, LV7}: Reaction Boost, Item Boost, Air Combo Boost; Drive Boost, Draw, Combo Boost */
const u16 kh_form_rewards[2][4]={{2,0x188,0x19b,0x187},{3,0x18d,0x195,0x186}};

void kh_form_rewards_row(unsigned level,const u16 *rewards,u16 row[5]){
    memset(row,0,5*sizeof *row);
    unsigned step=level<3?1:level<5?2:level<7?3:0;
    if(step) row[step]=rewards[step-1];
}

/* 1 = this form's row is the mod's "no reward" row (unset or all zero), 0 = the user configured one */
static int row_is_default(const char *env){
    const char *p=getenv(env);
    if(p) for(int k=0;k<5;k++){ char *end; if(strtod(p,&end)!=0) return 0; p=*end?end+1:end; }
    return 1;
}

/* The rule relies on the growth-ability levels of the loaded fmlv table.  Other data would move the steps: say so and stop. */
static void check_fmlv(KCtx *g,u64 table){
    static const u8 want[7]={1,1,2,2,3,3,4};
    u32 count=rd32(g,table+4);
    for(int i=0;i<2;i++) for(unsigned level=1;level<=7;level++){
        u32 key=(u32)kh_form_rewards[i][0]<<4|level, k=0;
        while(k<count && rd8(g,table+8+8*(u64)k)!=key) k++;
        if(k==count || rd8(g,table+8+8*(u64)k+1)!=want[level-1])
            kh_fatal(g,"ps2-form-rewards-fmlv: form %u level %u has growth level %d (expected %u); set KHNMC_PS2_FORM_REWARDS=off for this data",
                     kh_form_rewards[i][0],level,k==count?-1:(int)rd8(g,table+8+8*(u64)k+1),want[level-1]);
    }
}

int kh_form_rewards_force=-1;                    /* selftest: 0 / 1 instead of the environment */
void kh_form_rewards_frame(KCtx *g){
    static int enabled=-1, mine[2]; static u64 checked;
    if(kh_form_rewards_force>=0){ enabled=kh_form_rewards_force; mine[0]=mine[1]=1; }
    else if(enabled<0){
        extern int kh_tweak_boolean(KCtx *,const char *,int);
        enabled=kh_tweak_boolean(g,"KHNMC_PS2_FORM_REWARDS",kh_rmc_find("rmc-package-1") != NULL);
        mine[0]=row_is_default("KHNMC_TWEAK_FORMS_WISDOM_GROWTH");
        mine[1]=row_is_default("KHNMC_TWEAK_FORMS_LIMIT_GROWTH");
        if(enabled) kh_warn("ps2 form rewards: on (Bladeless row %s, Light & Dark row %s)",mine[0]?"by form level":"as configured",mine[1]?"by form level":"as configured");
    }
    if(!enabled) return;
    u64 fmlv=rd64(g,FMLV_POINTER);
    if(fmlv && fmlv!=checked && GA_OK(fmlv+7)){ check_fmlv(g,fmlv); checked=fmlv; }
    for(int i=0;i<2;i++){
        if(!mine[i]) continue;
        unsigned form=kh_form_rewards[i][0];
        u16 row[5]; kh_form_rewards_row(rd8(g,SAVE_PARTY+0xe04+0x38*(u64)(form-1)+2),kh_form_rewards[i]+1,row);
        for(int k=0;k<5;k++){ u64 p=GROWTH_TABLE+10*(u64)(form-1)+2*(u64)k; if(rd16(g,p)!=row[k]) wr16(g,p,row[k]); }
    }
}
