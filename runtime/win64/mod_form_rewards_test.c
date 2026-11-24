/* T101 selftest: the PS2 growth rewards of Bladeless and Light & Dark (mod_form_rewards.c).
 * Expected values are written from the pnach (lines 6317-6329) and from the exe listing of f_3e8380, not from the code under test:
 *   - the row the rule leaves in the exe table (0x5c9030) for every form level 0..9;
 *   - a transcription of the engine's two find-and-replace loops (0x3e852f..0x3e8569 form, 0x3e8588..0x3e85c9 base) run over the
 *     levels 1 -> 7 with those rows: the three rewards arrive at LV3 / LV5 / LV7 and STAY, the form list is untouched, the form's
 *     message id is 0; with the mod's zero rows (the rule off) nothing arrives; with a static row the engine rewrites one slot;
 *   - rows of Valor / Master / Final and every other byte around the table are not written; off = nothing is written. */
#include "khnmc_rt.h"
#include <stdio.h>
#include <string.h>
static int checks, failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"form rewards selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)

#define TABLE (GS_BASE+0x5c9030)
#define SAVE  (GS_BASE+0x9abda0)
#define LEVEL(form) (SAVE+0xe04+0x38*(u64)((form)-1)+2)
extern int kh_form_rewards_force;
void kh_form_rewards_frame(KCtx *);
void kh_form_rewards_row(unsigned level,const u16 *rewards,u16 row[5]);

/* exe 0x3e852f..0x3e85cf: `find`, then low 15 bits replaced, bit 15 kept; returns the message id (the "replace" word) */
static u16 engine_replace(u16 *slots,int n,u16 find,u16 with){
    for(int i=0;i<n;i++) if((slots[i]&0x7fff)==find){ slots[i]=(u16)((slots[i]&0x8000)|(with&0x7fff)); break; }
    return with;
}
static const u8 gal[8]={0,1,1,2,2,3,3,4};        /* fmlv growth level of form levels 1..7 (forms 2 and 3, PC data) */
struct run { u16 base[80], form[24], base_msg[8], form_msg[8]; };
/* levels 1 -> 7 of one form; rule = 1: the row is rewritten before each level-up as the frame hook does, 0: `fixed` stays */
static void level_to_7(struct run *r,const u16 *rewards,const u16 *fixed){
    for(unsigned level=1;level<7;level++){
        u16 row[5]; if(fixed) memcpy(row,fixed,sizeof row); else kh_form_rewards_row(level,rewards,row);
        unsigned n=gal[level+1];
        if(n<=gal[level]) continue;
        r->form_msg[level+1]=engine_replace(r->form,24,row[n-1],row[n]);
        r->base_msg[level+1]=engine_replace(r->base,80,row[n-2],row[n-1]);
    }
}
static int count(const u16 *slots,int n,u16 id){ int c=0; for(int i=0;i<n;i++) c+=(slots[i]&0x7fff)==id; return c; }

int kh_form_rewards_selftest(KCtx *g){
    static const u16 want[2][4]={{2,0x188,0x19b,0x187},{3,0x18d,0x195,0x186}};   /* pnach 6318/6320/6322 and 6325/6327/6329 */
    u8 table[50+16], levels[2]; memcpy(table,hp(g,TABLE-8),sizeof table);
    levels[0]=rd8(g,LEVEL(2)); levels[1]=rd8(g,LEVEL(3));
    int force=kh_form_rewards_force;

    /* off: not one byte of the table changes, whatever the levels are */
    kh_form_rewards_force=0;
    wr8(g,LEVEL(2),3); wr8(g,LEVEL(3),5); kh_form_rewards_frame(g);
    T(!memcmp(table,hp(g,TABLE-8),sizeof table));

    /* on: every level 0..9 of each form, the other form at a different level; only that form's five words follow the level */
    kh_form_rewards_force=1;
    for(int i=0;i<2;i++) for(unsigned level=0;level<10;level++){
        unsigned form=want[i][0], other=want[1-i][0];
        wr8(g,LEVEL(form),(u8)level); wr8(g,LEVEL(other),7);
        kh_form_rewards_frame(g);
        unsigned step=level<=2?1:level<=4?2:level<=6?3:0;
        for(unsigned k=0;k<5;k++){
            T(rd16(g,TABLE+10*(form-1)+2*k)==(k&&k==step?want[i][step]:0));
            T(rd16(g,TABLE+10*(other-1)+2*k)==0);
        }
        /* Valor, Master, Final rows and the 8 bytes on each side: as before */
        T(!memcmp(table,hp(g,TABLE-8),8+10) && !memcmp(table+8+30,hp(g,TABLE+30),20+8));
    }
    /* a row made stale (a state load, another writer) is put right by the next frame; the call from the game-frame step
     * (null_input.c kh_game_frame_step) is not run here (it advances the frame clock): a run shows `kh: ps2 form rewards:` */
    wr8(g,LEVEL(2),4); wr8(g,LEVEL(3),1);
    wr16(g,TABLE+10+2,0x1234); wr16(g,TABLE+10+4,0); wr16(g,TABLE+20+2,0);
    kh_form_rewards_frame(g);
    T(rd16(g,TABLE+10+2)==0 && rd16(g,TABLE+10+4)==0x19b && rd16(g,TABLE+20+2)==0x18d);

    /* the engine's loops over LV1 -> LV7 */
    for(int i=0;i<2;i++){
        struct run r; memset(&r,0,sizeof r);
        r.base[0]=0x8189; r.base[1]=0x008a; r.base[3]=0x8052;                    /* slot 2 and 4.. are free */
        r.form[1]=0x80dc; r.form[2]=0x80dd; r.form[5]=0x81a7;
        struct run before=r;
        level_to_7(&r,want[i]+1,NULL);
        T(r.base[2]==want[i][1] && r.base[4]==want[i][2] && r.base[5]==want[i][3]);   /* added in order, unequipped, all kept */
        T(r.base_msg[3]==want[i][1] && r.base_msg[5]==want[i][2] && r.base_msg[7]==want[i][3]);   /* named at LV3, LV5, LV7 */
        T(!r.base_msg[2] && !r.base_msg[4] && !r.base_msg[6]);
        T(!memcmp(r.form,before.form,sizeof r.form));
        for(int l=0;l<8;l++) T(r.form_msg[l]==0);
        /* a save that starts with one copy (Critical: plrp id 7) gets a second one; the first copy is not rewritten */
        struct run c=before; c.base[2]=(u16)(0x8000|want[i][1]);
        level_to_7(&c,want[i]+1,NULL);
        T(count(c.base,80,want[i][1])==2 && c.base[2]==(u16)(0x8000|want[i][1]) && count(c.base,80,want[i][2])==1 && count(c.base,80,want[i][3])==1);
        /* without the rule: the mod's zero row gives nothing ... */
        static const u16 zero[5]; struct run z=before;
        level_to_7(&z,NULL,zero);
        T(!memcmp(z.base,before.base,sizeof z.base));
        /* ... and a static row is rewritten in one slot (the PS2 Bladeless result), with the next reward as the form's line */
        u16 chain[5]={0,want[i][1],want[i][2],want[i][3],0}; struct run s=before;
        level_to_7(&s,NULL,chain);
        T(s.base[2]==want[i][3] && !count(s.base,80,want[i][1]) && !count(s.base,80,want[i][2]) && s.form_msg[3]==want[i][2]);
    }

    kh_form_rewards_force=force;
    memcpy(hp(g,TABLE-8),table,sizeof table); wr8(g,LEVEL(2),levels[0]); wr8(g,LEVEL(3),levels[1]);
    fprintf(stderr,"kh: form rewards selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
