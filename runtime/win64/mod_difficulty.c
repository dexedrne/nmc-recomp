/* DLL 32d30..33388, a full replacement of exe 3c12f0 (not a block detour).  The handler never reaches its trampoline 0x82830 (the
 * only use of that slot is the CalcDamage syscall, DLL 0x14aff), so no state runs the vanilla body: tools/check_mod_static_link.py
 * --dll-replaces fails the build if this object references orig_f_3c12f0 (or the original of any other full replacement). */
#include "mod_config.h"
#include <math.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
static _Atomic u64 damage_calls[KH_DIFF_SLOTS], damage_zero[KH_DIFF_SLOTS], damage_results[KH_DIFF_SLOTS];
static _Atomic s32 damage_last[KH_DIFF_SLOTS];
static float damage_rf(KCtx *g,u64 p){ u32 v=rd32(g,p); float f; memcpy(&f,&v,4); return f; }
static s32 damage_cvtt(float f){ return !isfinite(f) || f>=2147483648.0f || f< -2147483648.0f ? INT32_MIN : (s32)f; }
static u64 damage_call(KCtx *g,u32 f,u64 a,u64 b,u64 c,u64 d){ return kh_call(g,GS_BASE+f,a,b,c,d); }
static u64 damage_decode(KCtx *g,u32 id){ return damage_call(g,0x4ad270,id,0,0,0); }
static int damage_actor(KCtx *g,u64 p){ return (u8)damage_call(g,0x3ba720,p,0,0,0)!=0; }
static s32 damage_mul(s32 a,s32 b){ return (s32)((u32)a*(u32)b); }
static s32 damage_add(s32 a,s32 b){ return (s32)((u32)a+(u32)b); }
static u64 damage_sstm(KCtx *g){
    static u64 name;
    if(!name) name=ga_strdup("sstm");
    if(!name) kh_fatal(g,"difficulty: cannot allocate sstm key");
    return damage_call(g,0x3e9300,2,name,0,0);
}
void f_3c12f0(KCtx *g){
    u32 difficulty=rd32(g,GS_BASE+0x9abd48); unsigned slot=kh_diff_slot(difficulty);
    damage_calls[slot]++;
    extern void kh_phase_damage_entry(void); kh_phase_damage_entry();
    u64 victim=A0(g), packet=A1(g), attack=A2(g);
    s32 lane=A3(g), home=(s32)rd32(g,KH_RSP(g)+0x10);
    /* DLL stores r9d at entry+20 and later reads it at frame+78. */
    wr32(g,KH_RSP(g)+0x20,lane);
    u64 attacker=damage_decode(g,rd32(g,packet+0x10));
    u64 sheet=rd64(g,attacker+0x5c0);
    s32 base, upper, lower, result;
    if(sheet){
        if(!attack){ base=upper=lower=home; }
        else {
            u8 kind=rd8(g,attack+5);
            if(kind==0){
                base=(s16)rd16(g,sheet+0x188);
                if(damage_rf(g,sheet+0x1c0)>0) base=damage_add(base,(s16)rd16(g,sheet+0x226));
            } else if(kind==1){
                base=(s16)rd16(g,sheet+0x18a);
                u8 type=rd8(g,attack+4);
                if(type==5){
                    s32 n=damage_mul((s16)rd16(g,attack+6),(s32)rd32(g,victim+(s64)lane*12+4));
                    result=damage_add(damage_mul(n,base),9999)/10000;
                    goto done;
                }
                if(type==6){ result=damage_add(damage_mul((s16)rd16(g,attack+6),base),99)/100; goto done; }
            } else if(kind==2) base=(s16)rd16(g,sheet+0x18c);
            else base=home;
            upper=rd32(g,sheet+0x198); lower=rd32(g,sheet+0x19c);
        }
    } else {
        u64 params=damage_call(g,0x3ee8c0,0,0,0,0);
        base=(s16)rd16(g,params+2); upper=(s16)rd16(g,params+6); lower=(s16)rd16(g,params+8);
    }
    base=damage_add(base,-(s32)(s16)rd16(g,victim+0x18c));
    u64 stat=victim+(s64)lane*12;
    if((s32)rd32(g,stat)<=(s32)rd32(g,stat+4)/4) base=damage_add(base,-(s32)(s8)rd8(g,victim+0x204));
    s32 cap=rd32(g,victim+0x1a0); if(upper>=cap) upper=cap;
    if(base>=upper) base=upper;
    cap=rd32(g,victim+0x1a4); if(lower<=cap) lower=cap;
    if(base<=lower) base=lower;
    s32 n=damage_mul(rd8(g,victim+0x1a8+(s8)rd8(g,attack+9)),rd8(g,victim+0x1ae));
    n=damage_mul(n,(s16)rd16(g,attack+6)); n=damage_mul(n,base);
    result=damage_add(n,999999)/1000000;
    u64 save=damage_call(g,0x152380,0,0,0,0);
    if(save){
        u64 victim_actor=damage_decode(g,rd32(g,victim+0x268));
        if(damage_actor(g,victim_actor)){
            /* DLL 330d1..33151: the values 0, 1, 2, 3 of [save+2498] load the ini pairs; every other value takes 100 / 100
             * (330ea `mov r14d,0x64; mov esi,r14d`) and the scaling below still runs. */
            s32 enemy=100,party=100;
            if(difficulty<=3){ enemy=kh_mod_difficulty_config[difficulty].enemy_damage; party=kh_mod_difficulty_config[difficulty].party_damage; }
            if(rd8(g,victim_actor+0x6c8)&4) result=damage_add(damage_mul(result,enemy),99)/100;
            else if(damage_actor(g,attacker) && (rd8(g,attacker+0x6c8)&4)) result=damage_add(damage_mul(result,party),99)/100;
        }
    }
    result=damage_call(g,0x3c10e0,victim,(u32)result,0,0);
    if(sheet){
        s32 element=(s8)rd8(g,attack+9), bonus;
        if(element>=1 && element<=3){
            bonus=(s16)rd16(g,sheet+0x218+2*(element-1));
            /* As shipped: +99 precedes the multiplication, unlike vanilla. */
            result=damage_mul(damage_add(bonus,99),result)/100;
        }
        result=damage_call(g,0x3c0eb0,victim,(u32)result,sheet,attack);
    }
    if((s32)rd32(g,stat)<=damage_cvtt((float)(s32)rd32(g,stat+4)*damage_rf(g,victim+0x248)))
        result=damage_add(damage_mul(result,(s32)rd32(g,damage_sstm(g)+0x180)),99)/100;
done:
    damage_last[slot]=result;
    damage_results[slot]+=(u32)result;
    if(!result) damage_zero[slot]++;
    KH_RAX(g)=(u32)result;
    KH_RET(g);
}
void kh_mod_difficulty_damage_dump(void){
    for(int i=0;i<4;i++) fprintf(stderr,"kh: difficulty damage slot=%d calls=%llu zero=%llu result_sum_u32=%llu last=%d\n",i,
        (unsigned long long)damage_calls[i],(unsigned long long)damage_zero[i],
        (unsigned long long)damage_results[i],(int)damage_last[i]);
}

/* DLL 31ee0 and 2f9b0: full replacements, including signed field reads and dormant flags.
 * Neither handler references its trampoline (0x82390, 0x820f8).  Both compare [save+2498] with 0, 1, 2, 3 in turn (31f4c..31fbd,
 * 2f9e3..2fa55); for any other value the two multipliers stay what `movss xmm7,[rsp+20]; movss xmm6,[rsp+20]` loaded at
 * 31f66 / 31f6c (2f9fe / 2fa04): a slot of the handler's own frame that nothing has written, i.e. residue of earlier calls.
 * 2f9b0 also indexes its 4-byte mask table 0x1800749e4 with the value (2fa9c..2faaa).  What the PC game does with such a value is
 * therefore not determined by the DLL's code (and it is not the vanilla body): a named fatal, for the whole hook. */
static _Atomic u64 reward_hits[2][4];
static void reward_range(KCtx *g,u32 d,u32 exe,u32 dll,const char *what){
    if(d>3) kh_fatal(g,"mod-difficulty-out-of-range: difficulty dword [0x1409abd48] = 0x%x (the game writes 0..3) in the hook on exe 0x%x = DLL 0x%x, "
                       "a full replacement (the vanilla body never runs on the PC): %s; what the PC game does here is not defined by the DLL's code, not ported",
                     (unsigned)d,(unsigned)exe,(unsigned)dll,what);
}
static u64 reward_call(KCtx *g,u32 r,u64 a,u64 b,u64 c){ return kh_call(g,GS_BASE+r,a,b,c,0); }
static void reward_message(KCtx *g,u64 list,int *n,u32 id){
    wr64(g,list+8*(*n)++,reward_call(g,0x3ad9a0,id,0,0));
}
void f_3ed7d0(KCtx *g){
    u32 d=rd32(g,GS_BASE+0x9abd48);
    reward_range(g,d,0x3ed7d0,0x31ee0,"the DLL takes its HP and AP multipliers from an unwritten slot of its own stack frame (movss xmm7 / xmm6,[rsp+0x20] at 0x31f66 / 0x31f6c)");
    reward_hits[0][d]++;
    u64 row=A0(g); u32 character=A1(g),level=A2(g); s32 path=(s32)A3(g);
    u64 stack=KH_RSP(g); KH_RSP(g)=(stack-0x180)&~15ull;
    u64 list=KH_RSP(g)+0x40; memset(hp(g,list),0,0x58);
    u64 sheet=reward_call(g,0x3c04c0,character,0xffffffff,0);
    int n=0; u8 flags=rd8(g,row+14),packed=rd8(g,row+15);
    if(flags&1){
        reward_message(g,list,&n,0x8ae2);
        s32 amount=(s32)(((float)(packed>>4)*kh_mod_difficulty_config[d].hp)*0.5f);
        wr32(g,sheet+4,rd32(g,sheet+4)+(u32)amount);
    }
    if(flags&2){
        reward_message(g,list,&n,0x8ae3);
        /* The DLL deliberately uses HP's multiplier for both level-up grants. */
        s32 amount=(s32)(((float)(packed&15)*kh_mod_difficulty_config[d].hp)*0.5f);
        wr32(g,sheet+0x184,rd32(g,sheet+0x184)+(u32)amount);
    }
    if(flags&4){ reward_message(g,list,&n,0x8ae4); wr8(g,sheet+0x1b2,rd8(g,sheet+0x1b2)+1); }
    const u8 slot_flags[]={8,16,32},slot_offsets[]={0x12,0x11,0x10};
    for(int k=0;k<3;k++) if(flags&slot_flags[k]){
        reward_message(g,list,&n,0x8ae5+k);
        u64 party=reward_call(g,0x3a14e0,GS_BASE+0x9abda0,character,0);
        wr8(g,party+slot_offsets[k],rd8(g,party+slot_offsets[k])+1);
    }
    for(int k=0;k<3;k++) if((s8)rd8(g,row+4+k)>(s8)rd8(g,row-12+k)) reward_message(g,list,&n,0x8ade + k);
    s32 before=(s32)((float)(s8)rd8(g,row-9)*kh_mod_difficulty_config[d].ap);
    s32 after=(s32)((float)(s8)rd8(g,row+7)*kh_mod_difficulty_config[d].ap);
    if(after>before){
        /* DLL 322ce writes AP's message without incrementing the list index. */
        wr64(g,list+8*n,reward_call(g,0x3ad9a0,0x8ae1,0,0));
    }
    s32 ability=(s16)rd16(g,row+8+2*(s64)path);
    if(ability){
        reward_call(g,0x3c3f00,(u32)ability,character,0);
        reward_call(g,0x3ed400,list,(u32)ability,0);
    }
    u64 party=reward_call(g,0x3e2e20,0xffffffff,0,0);
    if((u8)reward_call(g,0x3e37a0,party,character,0)) reward_call(g,0x3eda60,list,character,level);
    reward_call(g,0x3c07e0,sheet,0,0);
    KH_RSP(g)=stack; KH_RET(g);
}
void f_3edf90(KCtx *g){
    u32 d=rd32(g,GS_BASE+0x9abd48);
    reward_range(g,d,0x3edf90,0x2f9b0,"the DLL takes its HP and MP multipliers from an unwritten slot of its own stack frame (movss xmm6 / xmm7,[rsp+0x20] at 0x2f9fe / 0x2fa04) "
                                       "and indexes its 4-byte mask table 0x1800749e4 with the value (0x2faaa)");
    reward_hits[1][d]++;
    u64 row=A0(g),sheet=A1(g); s32 param=(s8)rd8(g,row+13);
    if(param) reward_call(g,0x3edca0,(u32)param,0,0);
    if(rd8(g,row+12) && !(rd8(g,row+12)&(1u<<d))){ KH_RET(g); return; }
    u64 stack=KH_RSP(g); KH_RSP(g)=(stack-0x180)&~15ull;
    u64 list=KH_RSP(g)+0x40; reward_call(g,0x3ecf20,list,0,0);
    s32 hp_grant=(s8)rd8(g,row+2),mp_grant=(s8)rd8(g,row+3);
    u8 character=rd8(g,row+1);
    if((u8)(character-1)<=2 || character==14){
        hp_grant=(s32)((float)hp_grant*kh_mod_difficulty_config[d].hp);
        mp_grant=(s32)((float)mp_grant*kh_mod_difficulty_config[d].mp);
    }
    wr32(g,sheet+4,rd32(g,sheet+4)+(u32)hp_grant);
    wr32(g,sheet+0x184,rd32(g,sheet+0x184)+(u32)mp_grant);
    kh_call(g,GS_BASE+0x3c0c30,sheet,(u32)hp_grant,0,0);
    reward_call(g,0x3c0cd0,sheet,(u32)mp_grant,0);
    s32 drive=(s8)rd8(g,row+4)+rd8(g,sheet+0x1b2);
    wr8(g,sheet+0x1b2,drive<9?drive:9);
    reward_call(g,0x3c0ad0,sheet,0,0);
    u32 id=rd32(g,sheet+0x250);
    if(id){
        u64 party=reward_call(g,0x4ad270,id,0,0);
        reward_call(g,0x3a16c0,party,(u32)((s8)rd8(g,party+0x12)+(s8)rd8(g,row+5)),0);
        wr8(g,party+0x11,rd8(g,party+0x11)+rd8(g,row+6));
        wr8(g,party+0x10,rd8(g,party+0x10)+rd8(g,row+7));
    }
    reward_call(g,0x3ed3d0,list,(u32)hp_grant,0x8ae2);
    reward_call(g,0x3ed3d0,list,(u32)mp_grant,0x8ae3);
    for(int k=4;k<8;k++) reward_call(g,0x3ed3d0,list,(u32)(s32)(s8)rd8(g,row+k),0x8ae0+k);
    for(int k=8;k<=10;k+=2){
        s32 ability=(s16)rd16(g,row+k);
        if(ability){ reward_call(g,0x3c3f00,(u32)ability,rd32(g,sheet+0x260),0); reward_call(g,0x3ed400,list,(u32)ability,0); }
    }
    u64 actor=reward_call(g,0x4ad270,rd32(g,sheet+0x268),0,0);
    if(actor) reward_call(g,0x3ed350,actor,0,0);
    reward_call(g,0x3edb20,list,(u32)(s32)(s8)character,(u32)(s32)(s8)rd8(g,GS_BASE+0x9acfb0));
    KH_RSP(g)=stack; KH_RET(g);
}

/* DLL 31260/30f60 (replacing exe 3c4b80/3c3da0).  Ported: kinds 11/12 (the max-HP/MP caps, Task 7), kind 0 (the stock item
 * path with the command-byte reversal, Task 16), the DLL's no-op apply kinds and its usability fall-through.  Every other
 * active kind is a named fatal.  DLL 31569 is an internal item-0x83 branch, not another detour. */
#include "khnmc_rt.h"
void orig_f_3c3da0(KCtx *g);      /* DLL 30f60 chains for kind 0, 22, > 24, a null sheet and a reached cap (310b9); DLL 31260 never does */
static _Atomic u64 caps_apply[KH_DIFF_SLOTS],caps_usable[KH_DIFF_SLOTS];
/* A nonzero kind makes the DLL skip the id switch for every id (31433 jne 31618), stock ids included, so the vanilla function
 * is never an answer for an active kind: no silent success, whatever the id. */
static void caps_require_scope(KCtx *g,u64 effect,unsigned kind,int apply){
    unsigned id=rd16(g,effect);
    int active=apply?((kind>=1&&kind<=24)||kind==30||kind==31):((kind>=1&&kind<=24)&&kind!=22);
    if(active) kh_fatal(g,"unimplemented mod item effect DLL RVA=0x%x exe RVA=0x%x id=0x%x kind=%u",apply?0x31260:0x30f60,apply?0x3c4b80:0x3c3da0,id,kind);
}

/* ---- Task 16: DLL 31260 kind 0, its stock path 3143b..31618 (the replacement of the vanilla id switch of exe 3c4b80).
 * Arguments: rcx item record (id word +0, percent byte +4, kind byte +5, command word +10), edx target character, r8d using
 * character (0 from the menus), r9d multiplier in percent (the DLL's console calls it "RATIO": 100 from the menus,
 * 100 + u16[user sheet+224] from the battle command 3f6d00).
 * It is not the vanilla function even when the command byte is 0:
 *   - the "used by another character" flag is r8d!=0 (31445/31457); vanilla also requires r8d!=edx (3c4bbd..3c4bca);
 *   - every kind-0 item looks up its command through 3b68b0 and reads byte +2f of the result (3147c/31482);
 *   - byte==2 divides both amounts by -10000 (magic 97247453), anything else by 10000 (magic 68db8bad);
 *   - item 83 passes flag 1 and an MP amount that is NOT divided (31569..31590);
 *   - a null sheet is passed to 3c07e0 (31367 -> 31bb6), which faults; vanilla returns.
 * The DLL's console output (RATIO, TEST! Battle Type Item, CureRate, Effect, ACTOR FAKE/REAL POINTER) is dropped. */
struct kh_item_mock *kh_item_mock_sink;
static _Atomic u64 item_apply[KH_DIFF_SLOTS][KH_ITEM_SLOTS],item_reversed[KH_DIFF_SLOTS],item_noop[KH_DIFF_SLOTS],item_usable[KH_DIFF_SLOTS],item_hp_calls,item_mp_calls;
static _Atomic s32 item_last_hp,item_last_mp;
static _Atomic u64 cmd2f_writes,cmd2f_two,cmd2f_zero,cmd2f_other,cmd2f_unlisted;
static struct { _Atomic u32 id_plus1,last; _Atomic u64 n; } cmd2f_seen[16];
static int item_slot(unsigned id){ return id>=1&&id<=7?(int)id-1:id==0x83?7:id>=0x112&&id<=0x117?8+(int)(id-0x112):KH_ITEM_SLOTS-1; }
static u64 item_call(KCtx *g,u32 rva,u64 a0,u64 a1,u64 a2,u64 a3){
    struct kh_item_mock *m=kh_item_mock_sink;
    if(!m) return kh_call(g,GS_BASE+rva,a0,a1,a2,a3);
    if(m->ncalls>=KH_ITEM_MAXCALLS) kh_fatal(g,"item call sink overflow at exe RVA=0x%x",rva);
    struct kh_item_call *c=&m->c[m->ncalls++];
    c->rva=rva; c->a[0]=a0; c->a[1]=a1; c->a[2]=a2; c->a[3]=a3;
    return m->fake(m->user,c);
}
/* DLL 31488..314b3 and 314b5..314dd: imul by the magic, high dword, sar 12, add the sign bit of the shifted quotient. */
static s32 item_div(s32 n,int reversed){
    s32 q=(s32)(((s64)(reversed?(s32)0x97247453:(s32)0x68db8bad)*(s64)n)>>32)>>12;
    return (s32)((u32)q+((u32)q>>31));
}
/* DLL 31351..31436 without the console: acquire, decode the sheet's actor id, fetch the save block. */
static u64 item_enter(KCtx *g){
    u32 target=(u32)A1(g);
    u64 sheet=item_call(g,0x3c04c0,target,0xffffffffu,0,0);
    if(!sheet) kh_fatal(g,"mod item effect: no sheet for character %u; shipped DLL 0x31367 jumps to 0x31bb6 and faults in exe 0x3c07e0(null)",target);
    item_call(g,0x4ad270,rd32(g,sheet+0x268),0,0,0);
    item_call(g,0x152380,0,0,0,0);
    return sheet;
}
/* Task 29: the last application as the target's sheet saw it (read-only; kh_mod_item_obs).  Written on the game thread only. */
static struct kh_item_obs item_obs; static pthread_mutex_t item_obs_lock=PTHREAD_MUTEX_INITIALIZER;
u64 kh_mod_item_obs(struct kh_item_obs *out){ pthread_mutex_lock(&item_obs_lock); if(out) *out=item_obs; u64 s=item_obs.seq; pthread_mutex_unlock(&item_obs_lock); return s; }
void kh_mod_item_obs_test_set(const struct kh_item_obs *in){ pthread_mutex_lock(&item_obs_lock); item_obs=*in; pthread_mutex_unlock(&item_obs_lock); }   /* selftest only */
u64 kh_input_game_frames(void);
static void item_hp(KCtx *g,u64 sheet,s32 amount,int flag){ item_hp_calls++; item_last_hp=amount; item_call(g,0x3c0c30,sheet,(u32)amount,0,(u64)flag); }
static void item_mp(KCtx *g,u64 sheet,s32 amount,int flag){ item_mp_calls++; item_last_mp=amount; item_call(g,0x3c0cd0,sheet,(u32)amount,(u64)flag,0); }
static void item_stock_apply(KCtx *g,int difficulty){
    u64 rec=A0(g); u32 user=(u32)A2(g),mult=(u32)A3(g);
    u64 sheet=item_enter(g);
    unsigned id=rd16(g,rec),pct=rd8(g,rec+4);
    int flag=user!=0;
    /* Both numerators are formed before the command lookup, from the maxima as they are at that point. */
    s32 mp_n=damage_add(damage_mul(damage_mul((s32)pct,(s32)rd32(g,sheet+0x184)),(s32)mult),9999);
    s32 hp_n=damage_add(damage_mul(damage_mul((s32)pct,(s32)mult),(s32)rd32(g,sheet+4)),9999);
    u64 cmd=item_call(g,0x3b68b0,(u32)(s32)(s16)rd16(g,rec+0x10),0,0,0);
    if(!cmd) kh_fatal(g,"mod item effect: no cmd record for command %d of item 0x%x; shipped DLL 0x31482 reads byte [0+0x2f]",(int)(s16)rd16(g,rec+0x10),id);
    int reversed=rd8(g,cmd+0x2f)==2;
    s32 hp_amount=item_div(hp_n,reversed),mp_amount=item_div(mp_n,reversed);
    struct kh_item_obs ob={0};
    ob.id=id; ob.target=(u32)A1(g); ob.user=user; ob.mult=mult; ob.pct=pct; ob.cmd=rd16(g,rec+0x10); ob.byte2f=rd8(g,cmd+0x2f); ob.reversed=reversed;
    ob.hp_before=(s32)rd32(g,sheet); ob.hp_max=(s32)rd32(g,sheet+4); ob.mp_before=(s32)rd32(g,sheet+0x180); ob.mp_max=(s32)rd32(g,sheet+0x184);
    ob.hp_call=(!((id-1)&0xfffa) && id!=6) || id==4 || id==7 || id==0x83; ob.mp_call=id==3 || id==6 || id==4 || id==7 || id==0x83;
    ob.hp_amount=ob.hp_call?hp_amount:0; ob.mp_amount=ob.mp_call?mp_amount:0;
    item_apply[difficulty][item_slot(id)]++;
    if(reversed && id>=1 && id<=7) item_reversed[difficulty]++;
    /* 314e0..31557: (id-1)&0xfffa==0 selects 1,2,5,6 and 6 is then excluded. */
    if(!((id-1)&0xfffa) && id!=6) item_hp(g,sheet,hp_amount,flag);
    if(id==3 || id==6) item_mp(g,sheet,mp_amount,flag);
    if(id==4 || id==7){ item_hp(g,sheet,hp_amount,flag); item_mp(g,sheet,mp_amount,0); }
    if(id==0x83){
        item_hp(g,sheet,(s32)rd32(g,sheet+4),1);
        /* As shipped: no +9999 and no division, and max MP is read again after the HP call. */
        item_mp(g,sheet,damage_mul(damage_mul((s32)pct,(s32)rd32(g,sheet+0x184)),(s32)mult),0);
    }
    if(id==0x112 || id==0x113) item_call(g,0x3c0ad0,sheet,(u32)damage_mul((s32)pct,100),0,0);
    if(id==0x114) item_call(g,0x3c09d0,sheet,pct,0,0);
    if(id==0x115) item_call(g,0x3c0e30,sheet,pct,0,0);
    if(id==0x116) item_call(g,0x3c0a50,sheet,pct,0,0);
    if(id==0x117) item_call(g,0x3c0950,sheet,pct,0,0);
    /* the sheet after the adds, before 0x3c07e0 drops the reference (the engine's add writes the sheet itself) */
    ob.hp_after=(s32)rd32(g,sheet); ob.mp_after=(s32)rd32(g,sheet+0x180); ob.game_frame=kh_input_game_frames();
    pthread_mutex_lock(&item_obs_lock); ob.seq=item_obs.seq+1; item_obs=ob; pthread_mutex_unlock(&item_obs_lock);
    if(!kh_item_mock_sink)
        fprintf(stderr,"kh: item apply #%llu game-frame=%llu id=%u cmd=%u cmd+0x2f=%u (%s) target=%u user=%u mult=%u pct=%u: HP %d -> %d of %d (amount %d%s), MP %d -> %d of %d (amount %d%s)\n",
                (unsigned long long)ob.seq,(unsigned long long)ob.game_frame,id,ob.cmd,ob.byte2f,reversed?"REVERSED: / -10000":"/ 10000",ob.target,user,mult,pct,
                ob.hp_before,ob.hp_after,ob.hp_max,ob.hp_amount,ob.hp_call?"":", no HP call",ob.mp_before,ob.mp_after,ob.mp_max,ob.mp_amount,ob.mp_call?"":", no MP call");
    KH_RAX(g)=item_call(g,0x3c07e0,sheet,0,0,0);
    KH_RET(g);
}
/* The WriteByte syscall (mod_syscalls.c index 69) is how the scripts arm the reversal: count stores to +2f of a cmd record. */
void kh_mod_item_writebyte(KCtx *g,u64 p,u8 value){
    u64 table=rd64(g,GS_BASE+0x2a161e8);
    if(!GA_OK(table)) return;
    s64 count=(s32)rd32(g,table+4); u64 first=table+8;
    if(count<=0 || p<first || p-first>=0x30ull*(u64)count || (p-first)%0x30!=0x2f) return;
    cmd2f_writes++;
    if(value==2) cmd2f_two++; else if(!value) cmd2f_zero++; else cmd2f_other++;
    u32 key=(u32)rd16(g,p-0x2f)+1;
    for(int i=0;i<16;i++){
        u32 cur=atomic_load(&cmd2f_seen[i].id_plus1);
        if(!cur){ u32 expect=0; cur=atomic_compare_exchange_strong(&cmd2f_seen[i].id_plus1,&expect,key)?key:expect; }
        if(cur==key){ cmd2f_seen[i].n++; cmd2f_seen[i].last=value; return; }
    }
    cmd2f_unlisted++;
}
void kh_mod_items_counts(struct kh_item_counts *out){
    for(int d=0;d<KH_DIFF_SLOTS;d++){
        for(int s=0;s<KH_ITEM_SLOTS;s++) out->apply[d][s]=item_apply[d][s];
        out->reversed[d]=item_reversed[d]; out->noop[d]=item_noop[d]; out->usable[d]=item_usable[d];
    }
    out->hp_calls=item_hp_calls; out->mp_calls=item_mp_calls;
    out->cmd2f_writes=cmd2f_writes; out->cmd2f_two=cmd2f_two; out->cmd2f_zero=cmd2f_zero; out->cmd2f_other=cmd2f_other;
}
void kh_mod_items_reset(void){
    for(int d=0;d<KH_DIFF_SLOTS;d++){
        for(int s=0;s<KH_ITEM_SLOTS;s++) item_apply[d][s]=0;
        item_reversed[d]=item_noop[d]=item_usable[d]=0;
    }
    item_hp_calls=item_mp_calls=0; item_last_hp=item_last_mp=0;
    cmd2f_writes=cmd2f_two=cmd2f_zero=cmd2f_other=cmd2f_unlisted=0;
    for(int i=0;i<16;i++){ cmd2f_seen[i].id_plus1=0; cmd2f_seen[i].last=0; cmd2f_seen[i].n=0; }
}
void kh_mod_items_dump(void){
    for(int d=0;d<4;d++){
        fprintf(stderr,"kh: item stock slot=%d uses",d);
        for(int s=0;s<7;s++) fprintf(stderr," id%d=%llu",s+1,(unsigned long long)item_apply[d][s]);
        fprintf(stderr," id83=%llu",(unsigned long long)item_apply[d][7]);
        for(int s=8;s<14;s++) fprintf(stderr," id%x=%llu",0x112+s-8,(unsigned long long)item_apply[d][s]);
        fprintf(stderr," other=%llu reversed=%llu noop_kinds=%llu usable_fallthrough=%llu\n",(unsigned long long)item_apply[d][14],
            (unsigned long long)item_reversed[d],(unsigned long long)item_noop[d],(unsigned long long)item_usable[d]);
    }
    fprintf(stderr,"kh: item stock helper calls hp=%llu mp=%llu last_hp=%d last_mp=%d\n",(unsigned long long)item_hp_calls,
        (unsigned long long)item_mp_calls,(int)item_last_hp,(int)item_last_mp);
    fprintf(stderr,"kh: WriteByte cmd+0x2f writes=%llu value2=%llu value0=%llu other=%llu unlisted=%llu commands(id=last_value x writes):",
        (unsigned long long)cmd2f_writes,(unsigned long long)cmd2f_two,(unsigned long long)cmd2f_zero,(unsigned long long)cmd2f_other,
        (unsigned long long)cmd2f_unlisted);
    for(int i=0;i<16;i++) if(cmd2f_seen[i].id_plus1)
        fprintf(stderr," %u=%ux%llu",(unsigned)cmd2f_seen[i].id_plus1-1,(unsigned)cmd2f_seen[i].last,(unsigned long long)cmd2f_seen[i].n);
    fprintf(stderr,"\n");
}

/* The difficulty dword only picks the counter slot in these two hooks.  DLL 31260 and 30f60 read [save+2498] in their kind 11 / 12
 * cap tests alone (317a2 / 317d7, 310e0 / 310fa: `cmp ...,3`), so every value other than 3, legal or not, is the cap 240 there and
 * plays no part anywhere else; neither handler has a path that depends on the dword being 0..3. */
void f_3c4b80(KCtx *g){
    int difficulty=(int)kh_diff_slot(rd32(g,GS_BASE+0x9abd48));
    u64 effect=A0(g); unsigned kind=rd8(g,effect+5);
    if(!kind){ item_stock_apply(g,difficulty); return; }
    if(kind!=11 && kind!=12){
        caps_require_scope(g,effect,kind,1);
        /* DLL table 31bdc kinds 25..29 and 3161e kind>31: acquire, release, nothing else.  The vanilla id switch does not run. */
        item_noop[difficulty]++;
        u64 held=item_enter(g);
        KH_RAX(g)=item_call(g,0x3c07e0,held,0,0,0);
        KH_RET(g); return;
    }
    caps_apply[difficulty]++;
    u64 sheet=kh_call(g,GS_BASE+0x3c04c0,(u32)A1(g),0xffffffffu,0,0);
    if(sheet){
        kh_call(g,GS_BASE+0x4ad270,rd32(g,sheet+0x268),0,0,0);
        u64 save=kh_call(g,GS_BASE+0x152380,0,0,0,0);
        int d=(int)rd32(g,save+0x2498),cap=d==3?(kind==11?120:220):240;
        u64 field=sheet+(kind==11?4:0x184); int value=(int)rd32(g,field);
        /* DLL gates before adding the raw byte; grants can overshoot the cap. */
        if(value<cap) wr32(g,field,(u32)value+rd8(g,effect+4));
    }
    kh_call(g,GS_BASE+0x3c07e0,sheet,0,0,0);
    KH_RET(g);
}
void f_3c3da0(KCtx *g){
    int difficulty=(int)kh_diff_slot(rd32(g,GS_BASE+0x9abd48));
    u64 effect=A0(g); unsigned kind=rd8(g,effect+5);
    if(kind!=11 && kind!=12){
        caps_require_scope(g,effect,kind,0);
        /* Task 16.  DLL 30f60 never calls the command lookup 3b68b0 (slot 82150) and has no +2f test: usability of a reversed
         * item is the vanilla predicate.  Kind 0, 22 and >24 (30fb7..30fc0, table 311f4) tail-call the original at 310b9, and
         * as on every path of this handler the sheet it acquired at 30fa9 is not released; a null sheet goes there as well. */
        item_usable[difficulty]++;
        kh_call(g,GS_BASE+0x3c04c0,(u32)A1(g),0xffffffffu,0,0);
        orig_f_3c3da0(g); return;
    }
    caps_usable[difficulty]++;
    u64 sheet=kh_call(g,GS_BASE+0x3c04c0,(u32)A1(g),0xffffffffu,0,0);
    if(sheet){
        u64 save=kh_call(g,GS_BASE+0x152380,0,0,0,0);
        int d=(int)rd32(g,save+0x2498),cap=d==3?(kind==11?120:220):240;
        if((int)rd32(g,sheet+(kind==11?4:0x184))<cap){
            /* DLL returns AL=1 without releasing its acquired sheet. */
            KH_RAX(g)=(KH_RAX(g)&~255ull)|1; KH_RET(g); return;
        }
    }
    /* Cap-hit and absent-sheet paths tail-call the original, retaining acquisition. */
    orig_f_3c3da0(g);
}
void kh_mod_caps_dump(void){
    for(int d=0;d<4;d++) fprintf(stderr,"kh: difficulty caps[%d] apply=%llu usable=%llu\n",d,
        (unsigned long long)caps_apply[d],(unsigned long long)caps_usable[d]);
}
int kh_mod_caps_selftest(KCtx *g){
    int checks=0,failures=0;
#define CAP_T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"caps selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)
    u64 sheet=GS_BASE+0x2a17290, count=GS_BASE+0x2a23950;
    u8 backup[0x278]; memcpy(backup,hp(g,sheet),sizeof backup);
    u32 oldcount=rd32(g,count),olddiff=rd32(g,GS_BASE+0x9abd48);
    u8 codec[512]; memcpy(codec,hp(g,GS_BASE+0x2b0d720),512);
    u32 guard=rd32(g,GS_BASE+0x2b0d920);
    memset(hp(g,GS_BASE+0x2b0d720),0xff,512); wr32(g,GS_BASE+0x2b0d920,0x80000000);
    /* Task 16: every kind-0 apply now performs the DLL's command lookup, so the fixture needs a cmd table. */
    u64 oldcmds=rd64(g,GS_BASE+0x2a161e8),cmds=kh_mod_items_test_cmds(g); wr64(g,GS_BASE+0x2a161e8,cmds);
    u64 effect=ga_calloc(1,0x18),sp=KH_RSP(g);
    /* The dword is compared with 3 and nothing else (DLL 317a2 / 317d7, 310e0 / 310fa): the values the game never writes take the
     * cap 240 like 0, 1, 2 do, in the DLL's own body (there is no path to the vanilla function for them). */
    static const s32 diffs[]={0,1,2,3,-1,4,INT32_MAX,INT32_MIN};
    u64 apply0[KH_DIFF_SLOTS],usable0[KH_DIFF_SLOTS]; for(int s=0;s<KH_DIFF_SLOTS;s++){ apply0[s]=caps_apply[s]; usable0[s]=caps_usable[s]; }
    for(unsigned di=0;di<sizeof diffs/sizeof *diffs;di++) for(int kind=11;kind<=12;kind++){
        s32 d=diffs[di];
        int cap=d==3?(kind==11?120:220):240;
        const int values[]={0,1,cap-1,cap,cap+1,255};
        const int grants[]={0,1,255};
        wr32(g,GS_BASE+0x9abd48,d); wr8(g,effect+5,kind); wr16(g,effect,0);
        for(unsigned i=0;i<sizeof values/sizeof *values;i++) for(unsigned j=0;j<sizeof grants/sizeof *grants;j++){
            memset(hp(g,sheet),0,sizeof backup); wr32(g,count,0); wr32(g,sheet+0x260,2); wr32(g,sheet+0x264,100);
            u64 field=sheet+(kind==11?4:0x184); wr32(g,field,values[i]); wr8(g,effect+4,grants[j]);
            kh_call(g,GS_BASE+0x3c4b80,effect,2,0,17);
            CAP_T(rd32(g,field)==values[i]+(values[i]<cap?grants[j]:0));
            CAP_T(rd32(g,sheet+0x264)==100); CAP_T(KH_RSP(g)==sp);
            wr32(g,field,values[i]);
            u64 r=kh_call(g,GS_BASE+0x3c3da0,effect,2,0,0);
            CAP_T((r&255)==(values[i]<cap)); CAP_T(rd32(g,sheet+0x264)==101);
        }
        /* At the cap, vanilla item-id fallback may still report usable. */
        memset(hp(g,sheet),0,sizeof backup); wr32(g,count,0); wr32(g,sheet+0x260,2); wr32(g,sheet+0x264,100);
        wr32(g,sheet+4,cap); wr32(g,sheet+0x184,cap); wr16(g,effect,1);
        CAP_T((kh_call(g,GS_BASE+0x3c3da0,effect,2,0,0)&255)==1);
    }
    /* The counters of those calls: per kind and value of the dword 18 applications and 19 usability questions; the four values
     * outside 0..3 share slot 4. */
    for(int s=0;s<KH_DIFF_SLOTS;s++){ CAP_T(caps_apply[s]-apply0[s]==(s==4?4u:1u)*36u); CAP_T(caps_usable[s]-usable0[s]==(s==4?4u:1u)*38u); }
    /* Kind 0 with an id the stock path only looks the command up for: nothing is granted. */
    wr32(g,GS_BASE+0x9abd48,1); wr8(g,effect+5,0); wr16(g,effect,0);
    memset(hp(g,sheet),0,sizeof backup); wr32(g,count,0); wr32(g,sheet+0x260,2); wr32(g,sheet+0x264,100); wr32(g,sheet+4,119);
    kh_call(g,GS_BASE+0x3c4b80,effect,2,0,100); CAP_T(rd32(g,sheet+4)==119); CAP_T(rd32(g,sheet+0x264)==100);
    wr16(g,effect,0x777); wr8(g,effect+5,13);
    if(!kh_fork_ok()) kh_selftest_not_run("difficulty caps","unimplemented item effect in the usable and the apply hook (named fatal)",4);   /* iOS: no fork (host_env.c) */
    else for(int apply=0;apply<2;apply++){
        FILE *log=kh_tmpfile(); if(!log) kh_fatal(g,"caps selftest cannot open fatal log");
        pid_t child=kh_fork();
        if(!child){ dup2(fileno(log),2); kh_call(g,GS_BASE+(apply?0x3c4b80:0x3c3da0),effect,2,0,100); _exit(99); }
        int status=0; if(child<0) kh_fatal(g,"caps selftest fork failed"); waitpid(child,&status,0);
        rewind(log); char output[2048]; size_t len=fread(output,1,sizeof output-1,log); output[len]=0; fclose(log);
        CAP_T(WIFEXITED(status)&&WEXITSTATUS(status)!=0&&WEXITSTATUS(status)!=99);
        CAP_T(strstr(output,"unimplemented mod item effect")&&strstr(output,"id=0x777 kind=13"));
    }
    extern int kh_mod_rewards_selftest(KCtx *,u64);
    failures+=kh_mod_rewards_selftest(g,sheet);
    failures+=kh_mod_items_selftest(g,sheet);
    memcpy(hp(g,sheet),backup,sizeof backup); wr32(g,count,oldcount); wr32(g,GS_BASE+0x9abd48,olddiff);
    memcpy(hp(g,GS_BASE+0x2b0d720),codec,512); wr32(g,GS_BASE+0x2b0d920,guard); ga_free(effect);
    wr64(g,GS_BASE+0x2a161e8,oldcmds); ga_free(cmds);
    for(int d=0;d<KH_DIFF_SLOTS;d++) caps_apply[d]=caps_usable[d]=0;
    kh_mod_items_reset();
    fprintf(stderr,"kh: difficulty caps selftest: %d checks, %d failures\n",checks,failures);
#undef CAP_T
    return failures;
}

/* Init-save defaults to Standard; new-game commit later uses the setter again. */
void orig_f_1523a0(KCtx *); void orig_f_152370(KCtx *);
static int forced_difficulty=-1;
void kh_mod_difficulty_init(KCtx *g){
    const char *v=getenv("KHNMC_DIFFICULTY");
    if(v){
        if(v[0]<'0'||v[0]>'3'||v[1]) kh_fatal(g,"KHNMC_DIFFICULTY must be one of 0,1,2,3");
        forced_difficulty=v[0]-'0';
    }
}
void f_1523a0(KCtx *g){
    orig_f_1523a0(g);
    if(forced_difficulty>=0){ wr32(g,GS_BASE+0x9abd48,forced_difficulty); fprintf(stderr,"kh: difficulty forced=%d after init-save\n",forced_difficulty); }
}
void f_152370(KCtx *g){
    orig_f_152370(g);
    if(forced_difficulty>=0) wr32(g,GS_BASE+0x9abd48,forced_difficulty);
}
static _Atomic u64 exp_hits[4];
void kh_mod_add_exp(KCtx *g,u64 args){
    u32 entry_d=rd32(g,GS_BASE+0x9abd48); if(entry_d<4) exp_hits[entry_d]++;
    u64 actor=damage_decode(g,rd32(g,args));
    s32 input=rd32(g,args+8);
    if(!(u8)damage_call(g,0x3ed110,0,0,0,0)) return;
    u64 stack=KH_RSP(g); KH_RSP(g)=(stack-0x100)&~15ull;
    u64 vec=KH_RSP(g)+0x40; memset(hp(g,vec),0,16);
    damage_call(g,0x3b58c0,actor,vec,0,0);
    u64 params=damage_call(g,0x3ee8c0,0,0,0,0);
    s32 award=damage_mul(damage_mul((s16)rd16(g,params+10),input),100)/10000;
    u64 player=damage_call(g,0x3b3e80,0,0,0,0),sheet=rd64(g,player+0x5c0);
    float ratio=(float)(s32)rd32(g,sheet)/(float)(s32)rd32(g,sheet+4);
    if(damage_rf(g,damage_sstm(g)+0x14c)>=ratio) award=damage_mul(award,(s8)rd8(g,sheet+0x203));
    u64 save=damage_call(g,0x152380,0,0,0,0); u32 d=rd32(g,save+0x2498);
    if(d==3){ award=damage_mul(award,75)/100; if(!award) award=1; }
    s32 levels=damage_call(g,0x3ecf30,(u32)award,0,0,0);
    if(levels>0) damage_call(g,0x17ca70,vec,(u32)award,(u32)levels,0);
    KH_RSP(g)=stack;
}
void kh_mod_difficulty_dump(void){
    kh_mod_difficulty_damage_dump(); kh_mod_caps_dump(); kh_mod_items_dump();
    /* Slot 4 of the counters above: calls made while the dword was not 0..3.  The damage and item hooks run the DLL's code for such
     * a value (100 % / 100 %, cap 240); the two reward hooks stop with mod-difficulty-out-of-range instead. */
    { u64 items=0; for(int s=0;s<KH_ITEM_SLOTS;s++) items+=item_apply[4][s];
      fprintf(stderr,"kh: difficulty out-of-range (dword [0x1409abd48] not 0..3) damage calls=%llu zero=%llu result_sum_u32=%llu last=%d item uses=%llu reversed=%llu noop_kinds=%llu usable_fallthrough=%llu caps apply=%llu usable=%llu\n",
        (unsigned long long)damage_calls[4],(unsigned long long)damage_zero[4],(unsigned long long)damage_results[4],(int)damage_last[4],
        (unsigned long long)items,(unsigned long long)item_reversed[4],(unsigned long long)item_noop[4],(unsigned long long)item_usable[4],
        (unsigned long long)caps_apply[4],(unsigned long long)caps_usable[4]); }
    for(int d=0;d<4;d++) fprintf(stderr,"kh: difficulty rewards slot=%d popup=%llu bons=%llu AddExp=%llu\n",d,
        (unsigned long long)reward_hits[0][d],(unsigned long long)reward_hits[1][d],(unsigned long long)exp_hits[d]);
}
