/* route_objective.c (Task 31): what the game TELLS the player, read from the game's own memory, so that a route is written from the
 * game's text and not from a guess: the mission's objective (the text of its pause screen), the INFORMATION bar, the reaction
 * command the command menu offers, and the objects of the field by their names.  Readers only: nothing here writes game memory; the
 * one hook (0x17b710) passes its call through unchanged.  Used by runtime/route_state.c (conditions, trace) and runtime/null_input.c
 * (`@react`, `@goto object NAME`).  Where each thing comes from (exe RVAs; read from the disassembly of the functions named and
 * checked against traced runs, run-Q31/):
 *
 *   messages         0x3ad9a0(id) turns a message id into a pointer to the message's bytes: three tables, [0x142a11668 + 0x10 * i]
 *                    (i = 0, 1, 2; the game loads them from msg/<lang>/sys.bar and the world's msg/<lang>/<world>.bar), each with its
 *                    entry count at +4 and 8-byte entries from +8 (id word at +0, offset of the text from the table's start at +4),
 *                    searched with bsearch (compare 0x3ad950: key - the entry's id WORD; the key is the id with bit 15 cleared, the
 *                    flag the command and menu tables set on their ids).  The first table that has the id wins; an id in none of
 *                    them gives message 0xadc of the table at 0x142a11678.  A message is a byte string: 0 ends it, 1 a space, 2 a new
 *                    line, 3..0x1f control codes with fixed argument counts (7 = colour + 4 bytes, 9 = a button icon + 1 byte, 3 =
 *                    back to the default colour), 0x2e.. the letters (table below; the mod's English texts use nothing else).
 *   the mission      the area's btl program names it (`Mission "TT07_MS103"`): 0x3a3fa0 -> 0x3a2c30 builds the one static mission
 *                    object 0x142a0f8b0 and stores it at [0x142a0ff68] (0x3a3526 clears that pointer when the mission ends).  The
 *                    object: +0x40 the path "msn/<lang>/<NAME>.bar", +8 the pointer to the file's first entry, the mission's header:
 *                    word +2 the mission id, word +4 flags, word +6 the INFORMATION bar's message (0 = none), byte +8 the kind of
 *                    pause screen START opens during it (a record id of 14mission.bar "slct"), word +0xa the message of that
 *                    pause screen: the OBJECTIVE.
 *   pause screen     0x3a3fd0 (mission start): 0x3ad610(header byte +8) copies the 0x40-byte "slct" record of that kind into one of
 *                    the six pause records at 0x142a114a0 (index = the record's byte +0x24; these are what Task 29 called "mission
 *                    records": 0x3ad500 / 0x3ad540 pick the highest one set) and stores the objective there: [record+0x18] = the id
 *                    (0x4ad240) of the message pointer 0x3ad9a0(header word +0xa) returned.  START (0x1572b0) hands that record to
 *                    0x2e4c20, which opens menu 50 on it.  A "slct" record: id word +0, row count word +2, rows of (row id word,
 *                    message word) from +4.
 *   INFORMATION bar  0x3a3f60: 0x17b710(0x3ad9a0(header word +6)) when the mission starts; scripts show other lines through the same
 *                    0x17b710 (the queue 0x42ae10).  0x17b710(message pointer) opens the window [0x140abe828] (0x18dcd0) or replaces
 *                    its text (0x18deb0).  The hook below logs each call's text (`kh: information:`) and passes it on.
 *   reaction command YS::FIELD_COMMAND (0x142a10620) collects the reaction commands the field's objects register during a game frame in
 *                    seven rows of 0x24 bytes at +0x9b0 (0x3b2850 / 0x3b25c0 fill a row, 0x3b2660 reads one); its per-frame update
 *                    0x3b2340 EMPTIES those rows again at its end (0x3b20b0), so at the end of a game frame they are always empty
 *                    (run-Q31/x3: all zero while "Talk" was offered and answered).  What stays is the command the reaction handler
 *                    0x408520 (this = FC+0xb10, called from the update) picked from its candidates (row 1, the lock-on target,
 *                    the nearby objects: 0x4081c0): a 12-byte entry at FC+0xb40 = flags word +0, command id word +2 (0 = none:
 *                    0x402120), state byte +5 (below 4 = can be answered, 5 / 6 = shown but refused: 0x402110), target object id
 *                    +8.  The update then answers it when the pad's reaction trigger is set ([[FC+0xae8]+8] bit 10: triangle on the
 *                    PS2, Y on an XInput pad): 0x3b22d0(FC, the command's record, the target).  A command's name: its record
 *                    (0x3b68b0: bsearch by id over the 0x30-byte records of the table [0x142a161e8], count at +4, records from +8)
 *                    has the name's message at word +8.
 *   objects by name  an object's first three dwords are ids (0x3b3200): +0 its class record, +4 itself, +8 its objentry record
 *                    (00objentry.bin: 0x60 bytes, the model name at +8, e.g. "N_EX760_TT_SHOP_ACCE").  The object list is the one
 *                    `enemies-at-most` walks ([0x142a171c8], next = the id at +0xa90); position at +0x670. */
#include "route_state.h"
#include "xion_swap.h"
#include "kh_look.h"
#include <math.h>
u64 kh_input_game_frames(void);
#define NOW_WORLD   (GS_BASE+0x717008ull)
#define ID_TABLE    (GS_BASE+0x2b0d720ull)
#define MSG_TABLES  (GS_BASE+0x2a11668ull)   /* [MSG_TABLES + 0x10 * i], i = 0..2 */
#define MISSION_PTR (GS_BASE+0x2a0ff68ull)
#define PAUSE_RECS  (GS_BASE+0x2a114a0ull)
#define CMD_TABLE   (GS_BASE+0x2a161e8ull)
#define FC          (GS_BASE+0x2a10620ull)
#define OBJ_HEAD    (GS_BASE+0x2a171c8ull)
int kh_route_object_name(KCtx *g,u64 obj,char *out,size_t cap);
static int readable(u64 a,u64 n){ return a && GA_OK(a) && GA_OK(a+n-1); }
static u64 decode_id(KCtx *g,u32 id){ return rd64(g,ID_TABLE+8ull*((id&0x7fffffffu)>>25))|(id&0x1ffffffu); }
static float rdf(KCtx *g,u64 a){ u32 v=rd32(g,a); float f; memcpy(&f,&v,4); return f; }

/* ---- messages ------------------------------------------------------------------------------------------------------------------ */
u64 kh_route_message(KCtx *g,int id,int *table){
    u32 key=(u32)id&~0x8000u;
    for(int i=0;i<3;i++){
        u64 t=rd64(g,MSG_TABLES+0x10ull*(u64)i); if(!readable(t,8)) continue;
        s32 n=(s32)rd32(g,t+4); if(n<=0 || n>1000000 || !readable(t+8,8ull*(u64)n)) continue;
        s32 lo=0,hi=n-1;
        while(lo<=hi){ s32 m=lo+(hi-lo)/2; u32 e=rd16(g,t+8+8ull*(u64)m);
            if(e==key){ if(table) *table=i; return t+rd32(g,t+8+8ull*(u64)m+4); }
            if(e<key) lo=m+1; else hi=m-1; }
    }
    return 0;
}
/* the id of the message whose text starts at `p` (the reverse of the lookup: the tables are scanned), -1 = in none of them.  The FIRST
 * such id: a table may give several ids one text (msg/us/eh.bar 0x3f56 = 0x3f59, msg/us/tt.bar 0x375b = 0x375c), and then this is not
 * necessarily the id the game named (kh_route_message_ids lists them all) */
int kh_route_message_id(KCtx *g,u64 p){
    int overlay=kh_look_message_id(g,p); if(overlay>=0) return overlay;
    for(int i=0;i<3;i++){
        u64 t=rd64(g,MSG_TABLES+0x10ull*(u64)i); if(!readable(t,8) || p<=t) continue;
        s32 n=(s32)rd32(g,t+4); if(n<=0 || n>1000000 || !readable(t+8,8ull*(u64)n)) continue;
        for(s32 k=0;k<n;k++) if(t+rd32(g,t+8+8ull*(u64)k+4)==p) return (int)rd16(g,t+8+8ull*(u64)k);
    }
    return -1;
}
/* every id whose text starts at `p` (up to `max` of them; the count of all is returned) */
int kh_route_message_ids(KCtx *g,u64 p,int *ids,int max){
    int overlay=kh_look_message_id(g,p); if(overlay>=0) p=kh_route_message(g,overlay,NULL);
    int c=0;
    for(int i=0;i<3;i++){
        u64 t=rd64(g,MSG_TABLES+0x10ull*(u64)i); if(!readable(t,8) || p<=t) continue;
        s32 n=(s32)rd32(g,t+4); if(n<=0 || n>1000000 || !readable(t+8,8ull*(u64)n)) continue;
        for(s32 k=0;k<n;k++) if(t+rd32(g,t+8+8ull*(u64)k+4)==p){ if(c<max) ids[c]=(int)rd16(g,t+8+8ull*(u64)k); c++; }
    }
    return c;
}
/* "0x3f56 = 0x3f59" for a text several ids share, "0x3f5c" for one, "0" for none */
static void ids_text(KCtx *g,u64 p,char *out,size_t cap){
    int ids[8], c=kh_route_message_ids(g,p,ids,8); size_t o=0; if(cap) out[0]=0;
    if(!c){ snprintf(out,cap,"0"); return; }
    for(int i=0;i<c && i<8 && o<cap;i++) o+=(size_t)snprintf(out+o,cap-o,"%s%#x",i?" = ":"",ids[i]);
    if(c>8 && o<cap) snprintf(out+o,cap-o," = ... (%d ids)",c);
}
/* argument bytes of the control codes 0x03..0x1f (0x00 ends the text, 0x01 is a space, 0x02 a new line) */
static const u8 ctl_args[0x20]={0,0,0,0,1,6,1,4,3,1,1,1,1,0,1,5,0,4,2,4,2,2,1,2,2,1,1,1,1,1,1,1};
int kh_route_message_ctl_args(int code){ return code>=0&&code<0x20?ctl_args[code]:-1; }   /* selftest: compared with the game's own length table */
static const char *const roman[]={"I","II","III","IV","V","VI","VII","VIII","IX","X","XIII"};
/* One message as plain ASCII: a new line becomes a space, a colour change `<<`, the return to the default colour `>>` (the game
 * highlights the line that says what to do that way), a button icon `(icon 0xNN)`, any other control code is dropped with its
 * arguments, a character outside the table below is `?`.  Returns the number of message bytes read, 0 when `p` is not readable. */
static size_t message_decode(KCtx *g,u64 p,char *out,size_t cap,int page){
    size_t o=0,i=0; int colour=0; if(cap) out[0]=0;
    if(!cap || !readable(p,1)) return 0;
#define PUT(s) do{ const char *s_=(s); size_t l_=strlen(s_); if(o+l_<cap){ memcpy(out+o,s_,l_); o+=l_; } }while(0)
    for(;i<4096 && readable(p+i,1);){
        u8 b=rd8(g,p+i); i++; char one[2]={0,0};
        if(!b) break;
        if(b==0x10 && page) break;                                       /* the page break (0x3aec80 looks for it) */
        if(b==1 || b==2 || b==0x73){ if(o && out[o-1]!=' ') PUT(" "); continue; }
        if(b<0x20){
            u8 na=ctl_args[b]; u8 a0=na&&readable(p+i,1)?rd8(g,p+i):0; i+=na;
            if(b==7){ PUT("<<"); colour=1; } else if(b==3){ if(colour) PUT(">>"); colour=0; }
            else if(b==9){ char ic[20]; snprintf(ic,sizeof ic,"(icon 0x%02x)",a0); PUT(ic); }
            else if(b>=0x19) PUT("?");
            continue; }
        if(b>=0x21 && b<=0x2a) one[0]=(char)('0'+b-0x21);
        else if(b>=0x2e && b<=0x47) one[0]=(char)('A'+b-0x2e);
        else if(b>=0x90 && b<=0x99) one[0]=(char)('0'+b-0x90);
        else if(b>=0x9a && b<=0xb3) one[0]=(char)('a'+b-0x9a);
        else if(b>=0x74 && b<=0x7e){ PUT(roman[b-0x74]); continue; }
        else switch(b){
            case 0x2b: one[0]='+'; break; case 0x2c: case 0x54: case 0x55: case 0x66: case 0x67: case 0xcd: one[0]='-'; break; case 0x2d: one[0]='x'; break;
            case 0x48: one[0]='!'; break; case 0x49: one[0]='?'; break; case 0x4a: one[0]='%'; break; case 0x4b: one[0]='/'; break; case 0x4c: one[0]='*'; break;
            case 0x4d: case 0x50: one[0]=','; break; case 0x4e: case 0x4f: one[0]='.'; break; case 0x51: one[0]=';'; break; case 0x52: one[0]=':'; break;
            case 0x53: PUT("..."); continue; case 0x56: one[0]='~'; break; case 0x57: case 0xe9: case 0xea: case 0xec: case 0xee: one[0]='\''; break;
            case 0x5a: one[0]='('; break; case 0x5b: one[0]=')'; break; case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0x60: case 0x61: case 0xed: one[0]='"'; break;
            case 0x62: one[0]='['; break; case 0x63: one[0]=']'; break; case 0x64: one[0]='<'; break; case 0x65: one[0]='>'; break;
            case 0x84: PUT("XI"); continue; case 0x85: PUT("XII"); continue; case 0x86: one[0]='&'; break; case 0x87: one[0]='#'; break; case 0xeb: one[0]='`'; break;
            default: one[0]='?'; }
        PUT(one);
    }
    if(colour) PUT(">>");
#undef PUT
    while(o && out[o-1]==' ') o--;
    out[o]=0; return i;
}
size_t kh_route_message_text(KCtx *g,u64 p,char *out,size_t cap){ return message_decode(g,p,out,cap,0); }
size_t kh_route_message_page(KCtx *g,u64 p,char *out,size_t cap){ return message_decode(g,p,out,cap,1); }
/* the id of the message that CONTAINS the byte at `p` (a page pointer lies inside its message), -1 = in none of the tables */
int kh_route_message_id_of(KCtx *g,u64 p){
    int overlay=kh_look_message_id(g,p); if(overlay>=0) return overlay;
    for(int i=0;i<3;i++){
        u64 t=rd64(g,MSG_TABLES+0x10ull*(u64)i); if(!readable(t,8) || p<=t) continue;
        s32 n=(s32)rd32(g,t+4); if(n<=0 || n>1000000 || !readable(t+8,8ull*(u64)n)) continue;
        u64 best=0; int id=-1, in=0;
        for(s32 k=0;k<n;k++){ u64 a=t+rd32(g,t+8+8ull*(u64)k+4); if(a<=p && a>=best){ best=a; id=(int)rd16(g,t+8+8ull*(u64)k); } if(a>p) in=1; }
        if(id>=0 && (in || p-best<4096)) return id;                    /* inside the table's text (or in its last message) */
    }
    return -1;
}
/* the text of message `id` as the game would look it up now; "" and 0 when no table has it */
int kh_route_message_by_id(KCtx *g,int id,char *out,size_t cap){
    u64 p=kh_route_message(g,id,NULL); if(cap) out[0]=0;
    return p && kh_route_message_text(g,p,out,cap);
}

/* ---- the mission and its objective ----------------------------------------------------------------------------------------------- */
int kh_route_objective(KCtx *g,KhObjective *o){
    memset(o,0,sizeof *o);
    if(rd8(g,NOW_WORLD)==0xff) return 0;
    u64 m=rd64(g,MISSION_PTR); if(!readable(m,0x6b0)) return 0;
    u64 h=rd64(g,m+8); if(!readable(h,0x20)) return 0;                    /* the file is not loaded yet: no header */
    o->mission=m; o->header=h; o->id=rd16(g,h+2); o->flags=rd16(g,h+4); o->information=rd16(g,h+6); o->pause_kind=rd8(g,h+8); o->objective=rd16(g,h+0xa);
    for(int i=0;i<(int)sizeof o->path-1;i++){ u8 c=rd8(g,m+0x40+(u64)i); if(!c) break; o->path[i]=c>=32&&c<127?(char)c:'?'; }
    /* the pause record of the mission's kind (0x3ad610 copied the "slct" record there: its first word is the kind) and the text in it */
    for(int i=0;i<6 && o->pause_kind;i++){ u64 r=PAUSE_RECS+0x40ull*(u64)i; if(rd16(g,r)==o->pause_kind){ o->record=r; u32 id=rd32(g,r+0x18); o->text=id&0x80000000u?decode_id(g,id):0; break; } }
    o->ok=1; return 1;
}
/* The mission's counters.  0x3a2c30 builds three gauge objects of 0x50 bytes at mission+0x160 (vtable 0x1405c4248); TT04_MS107
 * (the guard-and-reversal objective, header records `05 00 04 00 .. 03 00 00 40`: two counters with target 3) uses two:
 * traced (run-Q31/x13, x14, KHNMC_TRACE_MISSION=1) the dword at +0x38 of the first went 0 -> 1 -> 2 -> 3 with the perfect guards and
 * that of the second with the perfect reversals, and the mission ended when both were 3; +0x24 holds the target (3 and 3), +8 the
 * gauge's layout pointer (0 while the gauge is not in use). */
int kh_route_mission_counters(KCtx *g,int value[3],int target[3]){
    int n=0; for(int i=0;i<3;i++){ value[i]=target[i]=0; }
    KhObjective o; if(!kh_route_objective(g,&o)) return 0;
    for(int i=0;i<3;i++){ u64 c=o.mission+0x160+0x50ull*(u64)i; if(!rd64(g,c+8)) break; value[i]=(int)(s32)rd32(g,c+0x38); target[i]=(int)(s32)rd32(g,c+0x24); n++; }
    return n;
}
/* `kh: objective:` whenever a mission starts, ends, its texts change, or one of its two messages becomes readable (or stops being
 * readable): at a mission's start its message table may not be loaded yet, and the line then says `(message 0x.. is in no loaded
 * message table)`; it is printed again when the text comes (review of task 31: the change key had no "readable" in it, so a mission
 * whose pause record never changed, EH03_INTRO1 / TT14_MS109 / TT14_MS110 with objective 0, never got its text into a line).  A
 * handful of reads per game frame (two binary searches while a mission runs); the decoding and the line happen only on a change. */
static void message_tick(KCtx *g,u64 n);
static struct { u64 mission, header, text; int id, information, objective, kind, obj_read, info_read; int cn, cv[3], ct[3]; } obj_prev;
static char obj_last[1500]; static u64 obj_lines;                /* the last mission line printed (without the newline): for the selftest */
u64 kh_route_objective_lines(const char **last){ if(last) *last=obj_last; return obj_lines; }
/* the `kh: objective:` line of a mission (without the newline), as kh_route_objective_tick prints it */
size_t kh_route_objective_format(KCtx *g,u64 n,const KhObjective *o,char *out,size_t cap){
    char t[700], it[300]; const char *name=o->path; { const char *s=strrchr(o->path,'/'); if(s) name=s+1; }
    char nm[40]; snprintf(nm,sizeof nm,"%s",name); { char *d=strstr(nm,".bar"); if(d) *d=0; }
    if(!o->objective) snprintf(t,sizeof t,"none"); else if(!kh_route_message_by_id(g,o->objective,t,sizeof t)) snprintf(t,sizeof t,"(message %#x is in no loaded message table)",o->objective);
    if(!o->information) snprintf(it,sizeof it,"none"); else if(!kh_route_message_by_id(g,o->information,it,sizeof it)) snprintf(it,sizeof it,"(message %#x is in no loaded message table)",o->information);
    return (size_t)snprintf(out,cap,"kh: objective: game frame %llu: mission %s (id %#x, flags %#x, pause kind %#x): objective message %#x: %s%s%s; INFORMATION message %#x: %s%s%s",
            (unsigned long long)n,nm,o->id,o->flags,o->pause_kind,o->objective,o->objective?"\"":"",t,o->objective?"\"":"",o->information,o->information?"\"":"",it,o->information?"\"":"");
}
static void objective_line(KCtx *g,u64 n,const KhObjective *o){
    char line[1500], pt[700]; kh_route_objective_format(g,n,o,line,sizeof line); fprintf(stderr,"%s\n",line);
    snprintf(obj_last,sizeof obj_last,"%s",line); obj_lines++;
    /* what the pause screen itself holds: it must be the same message (0x3a3fd0 stored it); a mission's script may replace it later.
     * The record holds a text pointer, not an id: every id whose text starts there is named */
    if(o->record && o->text){ char id2[80]; ids_text(g,o->text,id2,sizeof id2); u64 want=o->objective?kh_route_message(g,o->objective,NULL):0;
        if(o->text!=want && kh_route_message_text(g,o->text,pt,sizeof pt))
            fprintf(stderr,"kh: objective: game frame %llu: the pause screen's record (kind %#x) holds another text than the mission's header names: message %s: \"%s\"\n",(unsigned long long)n,o->pause_kind,id2,pt); }
}
/* KHNMC_TRACE_MISSION=1 (diagnostic, read-only): while a mission runs, every dword of its object (0x6b8 bytes at [0x142a0ff68]) that
 * changed during the game frame; an offset that changed more than 48 times is not printed again.  How the mission counters were found. */
static void mission_diff(KCtx *g,u64 n){
    static int on=-1; static u32 prev[0x6b8/4]; static u8 seen[0x6b8/4]; static u64 prev_m;
    if(on<0){ const char *e=getenv("KHNMC_TRACE_MISSION"); on=e&&*e=='1'; }
    if(!on) return;
    u64 m=rd64(g,MISSION_PTR); if(!readable(m,0x6b8)){ prev_m=0; return; }
    if(m!=prev_m || !rd64(g,m+8)){ for(u64 i=0;i<0x6b8/4;i++) prev[i]=rd32(g,m+4*i); if(m!=prev_m) memset(seen,0,sizeof seen); prev_m=m; return; }
    char line[900]; size_t o=(size_t)snprintf(line,sizeof line,"kh: mission diff game-frame=%llu:",(unsigned long long)n); int k=0;
    for(u64 i=0;i<0x6b8/4;i++){ u32 v=rd32(g,m+4*i); if(v==prev[i]) continue;
        if(seen[i]<48){ seen[i]++; if(k<24 && o<sizeof line-40) o+=(size_t)snprintf(line+o,sizeof line-o," +%#llx:%x->%x%s",(unsigned long long)(4*i),prev[i],v,seen[i]==48?"(muted)":""); k++; }
        prev[i]=v; }
    if(k) fprintf(stderr,"%s\n",line);
}
void kh_route_objective_tick(KCtx *g,u64 n){
    message_tick(g,n); mission_diff(g,n);
    KhObjective o; kh_route_objective(g,&o);
    { int v[3],t[3],cn=kh_route_mission_counters(g,v,t);                 /* `kh: objective: ... counters` when a counter of the mission changes */
      if(cn!=obj_prev.cn || memcmp(v,obj_prev.cv,sizeof v) || memcmp(t,obj_prev.ct,sizeof t)){
          if(cn){ char b[120]; size_t q=0; for(int i=0;i<cn;i++) q+=(size_t)snprintf(b+q,sizeof b-q,"%s%d of %d",i?", ":"",v[i],t[i]);
              fprintf(stderr,"kh: objective: game frame %llu: mission id %#x counters: %s\n",(unsigned long long)n,o.id,b); }
          obj_prev.cn=cn; memcpy(obj_prev.cv,v,sizeof v); memcpy(obj_prev.ct,t,sizeof t); } }
    int ord=o.ok && o.objective && kh_route_message(g,o.objective,NULL)!=0, ird=o.ok && o.information && kh_route_message(g,o.information,NULL)!=0;
    if(o.mission==obj_prev.mission && o.header==obj_prev.header && o.text==obj_prev.text && o.id==obj_prev.id && o.information==obj_prev.information && o.objective==obj_prev.objective && o.pause_kind==obj_prev.kind &&
       ord==obj_prev.obj_read && ird==obj_prev.info_read) return;
    if(o.ok) objective_line(g,n,&o);
    else if(obj_prev.mission) fprintf(stderr,"kh: objective: game frame %llu: no mission (the mission with id %#x ended)\n",(unsigned long long)n,obj_prev.id);
    obj_prev.mission=o.mission; obj_prev.header=o.header; obj_prev.text=o.text; obj_prev.id=o.id; obj_prev.information=o.information; obj_prev.objective=o.objective; obj_prev.kind=o.pause_kind;
    obj_prev.obj_read=ord; obj_prev.info_read=ird;
}
void kh_route_objective_test_reset(void){ memset(&obj_prev,0,sizeof obj_prev); }      /* selftest only */

/* ---- the INFORMATION bar: every text 0x17b710 is asked to show (the mission's own line and the lines scripts queue) ------------------ */
/* The hook gets only the text's pointer.  Texts are deduplicated (msg/us/eh.bar: 0x3f56 and 0x3f59 are one text, tt.bar 0x375b and
 * 0x375c), so the pointer alone does not say which id the game named (review of task 31: EH03_INTRO1's header names 0x3f59, the
 * first table entry with that text is 0x3f56).  The mission's own line comes from 0x3a3f60 / 0x3a41c1: 0x17b710(0x3ad9a0(header word
 * +6)), the lookup right before the call on the same thread; the 0x3ad9a0 hook records each thread's last lookup (id, pointer, game
 * frame), and a lookup in the same game frame that gave this very pointer is the id the game named.  Otherwise (the script queue
 * 0x42ae10 shows a pointer looked up earlier) every id with that text is printed and none is claimed as the game's. */
static _Atomic u64 info_shown; static _Atomic int info_last_id=-1;
static _Thread_local int look_id=-1; static _Thread_local u64 look_ptr, look_frame;
void kh_route_message_lookup_note(int id,u64 p){ look_id=id<0?-1:id&0x7fff; look_ptr=p; look_frame=kh_input_game_frames(); }   /* bit 15: a flag of the caller's, the same message */
static char info_last[700];
const char *kh_route_information_line(void){ return info_last; }
u64 kh_route_information_shown(int *last_id){ if(last_id) *last_id=atomic_load(&info_last_id); return atomic_load(&info_shown); }
void kh_route_information_note(KCtx *g,u64 p){
    char t[400], all[80]; int ids[8], c=kh_route_message_ids(g,p,ids,8), id=-1; u64 now=kh_input_game_frames(); ids_text(g,p,all,sizeof all);
    if(!kh_route_message_text(g,p,t,sizeof t)) snprintf(t,sizeof t,"(unreadable message pointer %#llx)",(unsigned long long)p);
    int looked=p && look_ptr==p && look_frame==now && look_id>=0;
    if(looked) id=look_id; else if(c==1) id=ids[0];
    atomic_store(&info_last_id,id); atomic_fetch_add(&info_shown,1);
    if(looked && c>1) snprintf(info_last,sizeof info_last,"kh: information: game frame %llu: \"%s\" (message %#x, the id the game looked up; the same text is message %s)",(unsigned long long)now,t,id,all);
    else if(looked || c==1) snprintf(info_last,sizeof info_last,"kh: information: game frame %llu: \"%s\" (message %#x)",(unsigned long long)now,t,id);
    else if(c>1) snprintf(info_last,sizeof info_last,"kh: information: game frame %llu: \"%s\" (message %s: the text of each of these ids; the game's lookup was not seen, which one it named is not known)",(unsigned long long)now,t,all);
    else snprintf(info_last,sizeof info_last,"kh: information: game frame %llu: \"%s\" (message 0: the pointer is in no loaded message table)",(unsigned long long)now,t);
    fprintf(stderr,"%s\n",info_last);
}
void orig_f_17b710(KCtx *); void f_17b710(KCtx *g){ kh_route_information_note(g,KH_RCX(g)); orig_f_17b710(g); }

/* ---- the texts a system menu looks up (`kh: menu text:`) ---------------------------------------------------------------------------
 * The camp menu's rows and pages carry no names of their own in memory the route can read; their labels are messages the menu
 * looks up with 0x3ad9a0(id) while it builds a page.  A pass-through hook on 0x3ad9a0 notes, while a system menu runs (MENU::isRunning
 * [0x1409006b0]), each message id once per (menu id, camp menu page): `kh: menu text: game frame N: menu M page P: message 0xID "..."`.
 * The game's call is made first and unchanged; only its result is read. */
#define MT_MENU_RUN (GS_BASE+0x9006b0ull)
#define MT_MENU_ID  (GS_BASE+0x900724ull)
static pthread_mutex_t mt_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { int menu, page, n, full; u16 ids[128]; u64 lines; } mt={-1,-2,0,0,{0},0};
static char mt_last[760];                                       /* the last line printed (without the newline): for the selftest */
u64 kh_route_menu_text_lines(const char **last){ if(last) *last=mt_last; return mt.lines; }
void kh_route_menu_text_note(KCtx *g,int id,u64 p){
    if(!rd8(g,MT_MENU_RUN)){ if(mt.menu!=-1){ pthread_mutex_lock(&mt_lock); mt.menu=-1; mt.page=-2; mt.n=0; mt.full=0; pthread_mutex_unlock(&mt_lock); } return; }
    int menu=(int)(s32)rd32(g,MT_MENU_ID), page=menu==8?(int)(s32)rd32(g,GS_BASE+0xbee64cull):-1;
    pthread_mutex_lock(&mt_lock);
    if(menu!=mt.menu || page!=mt.page){ mt.menu=menu; mt.page=page; mt.n=0; mt.full=0; }
    u16 key=(u16)(id&0x7fff); int seen=0; for(int i=0;i<mt.n;i++) if(mt.ids[i]==key){ seen=1; break; }
    if(seen || mt.full){ pthread_mutex_unlock(&mt_lock); return; }
    if(mt.n==(int)(sizeof mt.ids/sizeof *mt.ids)){ mt.full=1; pthread_mutex_unlock(&mt_lock);
        snprintf(mt_last,sizeof mt_last,"kh: menu text: game frame %llu: menu %d page %d: more than %d different messages; no more lines for this page",(unsigned long long)kh_input_game_frames(),menu,page,(int)(sizeof mt.ids/sizeof *mt.ids));
        fprintf(stderr,"%s\n",mt_last); return; }
    mt.ids[mt.n++]=key; mt.lines++; pthread_mutex_unlock(&mt_lock);
    char t[600]; if(!p || !kh_route_message_text(g,p,t,sizeof t)) snprintf(t,sizeof t,"(no text: the lookup gave %#llx)",(unsigned long long)p);
    snprintf(mt_last,sizeof mt_last,"kh: menu text: game frame %llu: menu %d page %d: message %#x \"%s\"",(unsigned long long)kh_input_game_frames(),menu,page,key,t);
    fprintf(stderr,"%s\n",mt_last);
}
int kh_route_menu_text_seen(KCtx *g,int id){
    if(!rd8(g,MT_MENU_RUN)) return 0;
    int menu=(int)(s32)rd32(g,MT_MENU_ID), page=menu==8?(int)(s32)rd32(g,GS_BASE+0xbee64cull):-1, r=0; u16 key=(u16)(id&0x7fff);
    pthread_mutex_lock(&mt_lock);
    if(menu==mt.menu && page==mt.page) for(int i=0;i<mt.n;i++) if(mt.ids[i]==key){ r=1; break; }   /* the notes of another page say nothing about this one */
    pthread_mutex_unlock(&mt_lock); return r;
}
void kh_route_menu_text_test_reset(void){ pthread_mutex_lock(&mt_lock); mt.menu=-1; mt.page=-2; mt.n=0; mt.full=0; pthread_mutex_unlock(&mt_lock); }   /* selftest only */
/* (A temporary caller trace of this hook, run-Q31/x47b, found the camp menu's labels looked up by the row helper 0x2f74a0 for rows
 * built from the descriptors at 0x1405badb0; the trace is not kept: route_state.c, kh_route_menu_flag, says what decides the rows.) */
void orig_f_3ad9a0(KCtx *);
void f_3ad9a0(KCtx *g){
    int id=(s32)A0(g);
    u64 overlay=kh_xkb_message(id);
    if(!overlay) overlay=kh_save_character_text(id);
    if(overlay){ SET_RAX(g,overlay); KH_RET(g); }
    else {
        orig_f_3ad9a0(g);
        KH_RAX(g)=kh_look_name(g,id,KH_RAX(g));
    }
    kh_route_message_lookup_note(id,KH_RAX(g));
    kh_route_menu_text_note(g,id,KH_RAX(g));
}

/* ---- reaction commands ------------------------------------------------------------------------------------------------------------ */
/* Review of task 31: "usable" is what the field command's update 0x3b2340 would answer in the next frame as far as memory says (read in
 * the exe: 0x1403b2346..0x1403b24d2).  It returns at once without the player [FC+0xae0] or the pad record [FC+0xae8]; it calls the
 * reaction handler 0x408520(FC+0xb10), which RETURNS WITHOUT TOUCHING the entry unless 0x39a8c0 is true (false = [0x1409ab580] has bit 23,
 * progress flag 0x817, and lacks bit 28, 0x81c) and [FC+0xb3c] bit 0 is set: the entry would be stale; then it answers only when the
 * player class's virtual +0x78 is true (code: not read here), FC+0x24 bit 0 is clear, 0x3a57f0(0x142a10500, 0) = [0x142a10500] is 0,
 * FC+0x24 bit 1 is clear (set: state 5, no answer), the pad's trigger bit 10 is set (the press), the command is not 0 and its state
 * byte below 4.  `closed` names the first gate that is shut. */
int kh_route_reaction(KCtx *g,KhReaction *o){
    memset(o,0,sizeof *o);
    if(rd8(g,NOW_WORLD)==0xff) return 0;
    u64 obj=rd64(g,FC+0xae0); if(!readable(obj,0xe00)) return 0;         /* no field player: the entry is stale */
    o->cmd=rd16(g,FC+0xb42); o->state=rd8(g,FC+0xb45); o->target_id=rd32(g,FC+0xb28);   /* the entry's parameters (0x14 bytes at FC+0xb28) start with the target object's id */
    o->offered=o->cmd!=0;
    if(!o->offered) return 0;
    u32 f=rd32(g,GS_BASE+0x9ab580ull), fc24=rd32(g,FC+0x24);
    if(o->state>=4) o->closed="its state byte is 4 or above (shown but refused)";
    else if(!rd64(g,FC+0xae8)) o->closed="no pad record [FC+0xae8]: 0x3b2340 returns at once";
    else if(((f>>23)&1) && !((f>>28)&1)) o->closed="stale entry: 0x39a8c0 is false (progress flag 0x817 set, 0x81c clear), the reaction handler 0x408520 picks nothing";
    else if(!(rd8(g,FC+0xb3c)&1)) o->closed="stale entry: [FC+0xb3c] bit 0 is clear, the reaction handler 0x408520 picks nothing";
    else if(fc24&1) o->closed="FC+0x24 bit 0 is set: 0x3b2340 answers nothing";
    else if(rd32(g,GS_BASE+0x2a10500ull)) o->closed="[0x142a10500] is not 0 (0x3a57f0): 0x3b2340 answers nothing";
    else if(fc24&2) o->closed="FC+0x24 bit 1 is set: 0x3b2340 sets the state to 5 and answers nothing";
    o->usable=!o->closed;
    return o->usable;
}
int kh_route_reaction_target(KCtx *g,const KhReaction *r,char *out,size_t cap){
    if(cap) out[0]=0;
    return r->offered && (r->target_id&0x80000000u) && kh_route_object_name(g,decode_id(g,r->target_id),out,cap);
}
/* the lock-on: the player object's target record [player+0xdc8] (kind dword +0, target id +4).  Kind 2 came with the lock-on button
 * in TT07_MS105 ("lock on to the cat": run-Q31/x6, kind 0 -> 2 in the frame after RB, back to 0 when the mission ended), and the
 * reaction handler 0x4081c0 takes the reaction candidates of exactly that target (0x408343: `cmp dword [rec], 2`, id at +4). */
int kh_route_lock_on(KCtx *g,u32 *target_id){
    if(target_id) *target_id=0;
    if(rd8(g,NOW_WORLD)==0xff) return 0;
    u64 obj=rd64(g,FC+0xae0); if(!readable(obj,0xe00)) return 0;
    u64 rec=rd64(g,obj+0xdc8); if(!readable(rec,8)) return 0;
    if(rd32(g,rec)!=2 || !(rd32(g,rec+4)&0x80000000u)) return 0;
    if(target_id) *target_id=rd32(g,rec+4);
    return 1;
}
/* ---- field message windows ------------------------------------------------------------------------------------------------------------
 * The YS::WINDOW_BASE list (head [0x142a16310], next = the id at +0xac; runtime/route_state.c `message`).  A window's state machine
 * (vfn4 0x3bce30): state [w+0xa0] 2 = waiting for the player, with mode [w+0x260] 1 (a page: the confirm bits 0x40c of the pad
 * trigger word go to the next page, 0x3b0950) or 2 (a choice: phase [w+0x408] 1 = takes input, cursor [w+0x40c], pad trigger bit 0x10
 * = one row up, 0x20 = one row down, no wrap; bit 0x4 confirms and the phase becomes 2).  The text object is at w+0x10: +0x10 the
 * message's first byte, +0x18 the first byte of the page shown, +0x3c the number of choice rows (mode 2 when it is above 0). */
#define WIN_HEAD (GS_BASE+0x2a16310ull)
int kh_route_message_window(KCtx *g,KhMessageWindow *o){
    memset(o,0,sizeof *o); o->cursor=-1;
    u64 w=rd64(g,WIN_HEAD);
    for(int k=0;k<16 && readable(w,0x5c8);k++){
        s32 st=(s32)rd32(g,w+0xa0), mode=(s32)rd32(g,w+0x260);
        if(st==2 && (mode==1||mode==2) && !rd64(g,w+0x5c0)){
            o->w=w; o->mode=mode; o->text=rd64(g,w+0x20); o->page=rd64(g,w+0x28);
            if(mode==2){ s32 rows=(s32)rd32(g,w+0x4c), cur=(s32)rd32(g,w+0x40c); o->rows=rows>0&&rows<=16?rows:0; o->cursor=cur>=0&&cur<o->rows?cur:-1; o->takes=rd32(g,w+0x408)==1; }
            return 1; }
        u32 id=rd32(g,w+0xac); if(!id) break; w=decode_id(g,id);
    }
    return 0;
}
/* `kh: message:` whenever a window starts to wait with another page (the decoding only then) */
static struct { u64 w, page; int mode; } msg_prev;
static void message_tick(KCtx *g,u64 n){
    KhMessageWindow m;
    if(!kh_route_message_window(g,&m)){ msg_prev.w=0; return; }
    if(m.w==msg_prev.w && m.page==msg_prev.page && m.mode==msg_prev.mode) return;
    msg_prev.w=m.w; msg_prev.page=m.page; msg_prev.mode=m.mode;
    char t[900]; int id=kh_route_message_id_of(g,m.page);
    if(!kh_route_message_page(g,m.page,t,sizeof t)) snprintf(t,sizeof t,"(unreadable page pointer %#llx)",(unsigned long long)m.page);
    if(m.mode==2) fprintf(stderr,"kh: message: game frame %llu: \"%s\" (message %#x, page at +%llu; a choice of %d rows, cursor on row %d)\n",(unsigned long long)n,t,id<0?0:id,(unsigned long long)(m.page-m.text),m.rows,m.cursor);
    else fprintf(stderr,"kh: message: game frame %llu: \"%s\" (message %#x, page at +%llu)\n",(unsigned long long)n,t,id<0?0:id,(unsigned long long)(m.page-m.text));
}
void kh_route_message_test_reset(void){ memset(&msg_prev,0,sizeof msg_prev); }        /* selftest only */
/* the name of a command (the message of its record's word +8), "" when the record or the message is not there */
int kh_route_command_name(KCtx *g,int cmd,char *out,size_t cap){
    if(cap) out[0]=0;
    u64 t=rd64(g,CMD_TABLE); if(!readable(t,8)) return 0;
    s32 n=(s32)rd32(g,t+4); if(n<=0 || n>100000 || !readable(t+8,0x30ull*(u64)n)) return 0;
    s32 lo=0,hi=n-1;
    while(lo<=hi){ s32 m=lo+(hi-lo)/2; u32 e=rd16(g,t+8+0x30ull*(u64)m);
        if(e==(u32)cmd) return kh_route_message_by_id(g,rd16(g,t+8+0x30ull*(u64)m+8),out,cap);
        if(e<(u32)cmd) lo=m+1; else hi=m-1; }
    return 0;
}

/* ---- objects by name ---------------------------------------------------------------------------------------------------------------- */
int kh_route_object_name(KCtx *g,u64 obj,char *out,size_t cap){
    if(cap) out[0]=0;
    if(!readable(obj,0xe00)) return 0;
    u32 eid=rd32(g,obj+8); u64 e=eid&0x80000000u?decode_id(g,eid):0; if(!readable(e,0x60)) return 0;
    size_t i=0; for(;i<32 && i+1<cap;i++){ u8 c=rd8(g,e+8+i); if(!c) break; out[i]=c>=33&&c<127?(char)c:'?'; }
    out[i]=0; return i>0;
}
/* objects of the list whose objentry model name is `name` (the nearest one to (px, pz) through nx / nz / nd); -1 = no map */
static int objects_named(KCtx *g,const char *name,float px,const float *py,float pz,float *nx,float *nz,float *nd){
    if(rd8(g,NOW_WORLD)==0xff) return -1;
    u64 o=rd64(g,OBJ_HEAD); int n=0; float best=1e30f; char nm[40];
    for(int k=0;k<512 && o && readable(o,0xe00);k++){
        if(kh_route_object_name(g,o,nm,sizeof nm) && !strcmp(nm,name)){
            float x=rdf(g,o+0x670),z=rdf(g,o+0x678);
            if(isfinite(x)&&isfinite(z)){ n++; float d=hypotf(x-px,z-pz); if(py) d=hypotf(d,rdf(g,o+0x674)-*py); if(d<best){ best=d; if(nx) *nx=x; if(nz) *nz=z; } } }
        u32 id=rd32(g,o+0xa90); if(!id) break; o=decode_id(g,id);
    }
    if(nd) *nd=best; return n;
}
int kh_route_objects_named(KCtx *g,const char *name,float px,float pz,float *nx,float *nz,float *nd){
    return objects_named(g,name,px,NULL,pz,nx,nz,nd);
}
int kh_route_at_save_point(KCtx *g){
    float x,y,z,yaw,d;
    return kh_route_player_pos(g,&x,&y,&z,&yaw)&&objects_named(g,"SAVE_POINT",x,&y,z,NULL,NULL,&d)>0&&d<=60;
}
/* KHNMC_ROUTE_TRACE=2: the reaction command when it changes (with its name and its target's name), and every 64 game frames the
 * objects with their names and positions */
void kh_route_objective_trace(KCtx *g,u64 n){
    static char prev[300]; char cur[300]; KhReaction r;
    kh_route_reaction(g,&r);
    u32 lk=0; int locked=kh_route_lock_on(g,&lk);
    snprintf(cur,sizeof cur,"reaction command %d state %d target id %#x; lock-on %s (target id %#x)%s%s",r.cmd,r.state,r.target_id,locked?"yes":"no",lk,r.offered&&!r.usable?"; cannot be answered: ":"",r.offered&&!r.usable?r.closed:"");
    if(strcmp(cur,prev)){ char nm[80]="",tn[40]="";
        if(r.offered){ kh_route_command_name(g,r.cmd,nm,sizeof nm); kh_route_reaction_target(g,&r,tn,sizeof tn); }
        fprintf(stderr,"kh: route probe game-frame=%llu %s%s%s%s%s%s\n",(unsigned long long)n,cur,r.offered?" offered: \"":"",nm,r.offered?"\"":"",tn[0]?" on ":"",tn); strcpy(prev,cur); }
    { static u64 ph0,ph1; u64 h0=rd64(g,GS_BASE+0xbf3270ull), h1=rd64(g,GS_BASE+0xbf31a0ull);      /* the two pad records (0x39b760 / 0x39b780): the held words */
      if(h0!=ph0 || h1!=ph1){ fprintf(stderr,"kh: route probe game-frame=%llu pad held: record 0x140bf3270 %#llx (trigger %#llx), record 0x140bf31a0 %#llx (trigger %#llx)\n",(unsigned long long)n,
          (unsigned long long)h0,(unsigned long long)rd64(g,GS_BASE+0xbf3278ull),(unsigned long long)h1,(unsigned long long)rd64(g,GS_BASE+0xbf31a8ull)); ph0=h0; ph1=h1; } }
    if(n%64 || rd8(g,NOW_WORLD)==0xff) return;
    u64 ob=rd64(g,OBJ_HEAD); float px=0,py=0,pz=0,ph=0; kh_route_player_pos(g,&px,&py,&pz,&ph);
    char buf[6000]; size_t b=(size_t)snprintf(buf,sizeof buf,"kh: route names game-frame=%llu:",(unsigned long long)n); int k=0;
    while(ob && readable(ob,0xe00) && k<128 && b<sizeof buf-120){ char nm[40];
        kh_route_object_name(g,ob,nm,sizeof nm); float x=rdf(g,ob+0x670),z=rdf(g,ob+0x678);
        b+=(size_t)snprintf(buf+b,sizeof buf-b," [%s %.0f,%.0f d%.0f]",nm[0]?nm:"?",x,z,hypotf(x-px,z-pz));
        u32 id=rd32(g,ob+0xa90); k++; if(!id) break; ob=decode_id(g,id); }
    fprintf(stderr,"%s (%d objects)\n",buf,k);
}
