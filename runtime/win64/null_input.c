#include "khnmc_rt.h"
/* Scripted XInput pad 0; other input backends remain empty. Ranges are inclusive;
 * gaps release all controls, and later lines win when ranges overlap.
 * Two kinds of script (input/README.md):
 *   frame-numbered  `FROM TO BUTTONS [lx ly]` lines, optionally `@hold FRAME until initial-state N` and `@clock game-frames`
 *                   (tasks 5, 14): presses at fixed frame numbers.  Unchanged by task 27.
 *   route           (task 27) steps that follow the GAME's state: `@wait COND`, `@press BUTTONS until COND`, `@select CURSOR N`,
 *                   `@expect COND`, each followed by presses relative to the game frame in which its condition was met
 *                   (`+A +B BUTTONS [lx ly]`).  Conditions are read by runtime/route_state.c at the end of every game frame; a
 *                   condition that does not come within its time-out ends the run with a named fatal that says which state never
 *                   came.  Everything is counted in game frames (calls of 0x105e10), nothing in wall time or presents.
 *   The two kinds cannot be mixed in one file (a named fatal at load). */
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
#include "route_state.h"
typedef struct { u16 buttons; u8 lt,rt; s16 lx,ly,rx,ry; } ScriptPad;
_Static_assert(sizeof(ScriptPad)==12,"XINPUT_GAMEPAD layout");
typedef struct { u64 start,end; ScriptPad pad; } InputRange;
static InputRange *ranges;
static size_t nranges;
static int scripted;
static ScriptPad last_pad;
static u32 packet;
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
extern _Atomic u64 gfx_presents;
static const char *button_names[]={"DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT","START","BACK","LSTICK","RSTICK","LB","RB",NULL,NULL,"A","B","X","Y","LT","RT"};
static int number(const char *s,long long lo,long long hi,long long *v){
    char *end; errno=0; *v=strtoll(s,&end,10); return !errno && end!=s && !*end && *v>=lo && *v<=hi;
}
/* `@hold FRAME until initial-state N` : script frames >= FRAME are not sampled before the Initial Settings screen has reached state N.
 * The script clock is the present counter, but the game thread's progress per present depends on the machine's load (the render
 * thread keeps presenting while the game thread boots): a confirm press scripted for present 180 misses a screen that only reaches
 * its prompt at present 186, and the route then loops in the settings menu for the whole run.  With a hold the script waits at
 * FRAME-1 and is delayed by exactly the lateness; a run that reaches the state in time is sampled exactly as without the line. */
static long long hold_frame=-1; static int hold_state=-1; static _Atomic long long hold_shift; static _Atomic int hold_released;
/* `@clock game-frames` (needs a hold): after the hold is released the script advances with the GAME thread's frames instead of
 * presents.  A game frame = one call of 0x105e10, which the end-of-frame task 0x1548e0 / 0x154740 makes once per frame and the
 * settings menu's own loop (0x4edef9, nested inside one such call) once per iteration.  One game frame is one present on
 * an idle machine, so a script written in presents keeps its meaning; on a loaded machine the game thread makes fewer frames than
 * the render thread presents, and a script clocked by presents runs ahead of the menus it is driving. */
static int clock_game; static _Atomic u64 game_frames; static _Atomic long long clock_sf0, clock_gf0; static _Atomic u64 last_script_frame;
/* KH_RUN_GAME_FRAMES=n ends the run (exit 0, with the reports) when the game thread has made n frames: a route length that does not
 * depend on the machine's load, unlike KH_RUN_SECONDS, which then only serves as the watchdog. */
static u64 run_game_frames;
u64 kh_game_frames(void){ return atomic_load(&game_frames); }      /* frames the game thread has made (diagnostic logs) */
static void route_tick(KCtx *g,u64 n);                                /* task 27: the route's steps, once per game frame */
void orig_f_105e10(KCtx *g);
void f_105e10(KCtx *g){
    u64 n=atomic_fetch_add(&game_frames,1)+1;
    { extern void kh_idtable_game_frame(u64); kh_idtable_game_frame(n); extern void kh_moose_game_frame(KCtx *); kh_moose_game_frame(g); }
    kh_route_state_tick(g,n); route_tick(g,n);
    if(run_game_frames && n>=run_game_frames){ extern void kh_exit_process(int); kh_warn("input: KH_RUN_GAME_FRAMES=%llu reached, ending the run",(unsigned long long)run_game_frames); kh_exit_process(0); }
    orig_f_105e10(g);
}
long long kh_phase_initial_first(int state);
static u64 script_frame(u64 present){
    if(hold_frame<0) return present;
    if(!atomic_load(&hold_released)){
        long long first=kh_phase_initial_first(hold_state);
        if(first<0) return present<(u64)hold_frame?present:(u64)hold_frame-1;
        long long shift=first>hold_frame-1?first-(hold_frame-1):0; int z=0;
        atomic_store(&hold_shift,shift);
        atomic_store(&clock_sf0,(long long)present-shift>0?(long long)present-shift:0); atomic_store(&clock_gf0,(long long)atomic_load(&game_frames));
        if(atomic_compare_exchange_strong(&hold_released,&z,1))
            fprintf(stderr,"kh: scripted XInput: hold at script frame %lld released: InitialSetting state %d first seen at present %lld, script delayed by %lld presents%s\n",hold_frame,hold_state,first,shift,clock_game?"; from here the script follows game frames":"");
    }
    if(clock_game){ long long f=atomic_load(&clock_sf0)+((long long)atomic_load(&game_frames)-atomic_load(&clock_gf0)); return f>0?(u64)f:0; }
    long long sh=atomic_load(&hold_shift); return present>(u64)sh?present-(u64)sh:0;
}
u64 kh_input_game_frames(void){ return atomic_load(&game_frames); }
/* ---- task 27: route steps ----------------------------------------------------------------------------------------------------
 * A step starts at the end of a game frame (a "tick", the 0x105e10 hook) and reads its condition there.  Game frame numbers: the
 * tick that ends frame F carries n = F+1 (the counter after the increment); the pad that tick computes is the pad of the NEXT frame,
 * counter n.  A step met at tick n has its anchor at counter n-1 (the frame in which the state was seen): `+A +B` presses counters
 * anchor+A .. anchor+B, so `+1` is the very next frame (A >= 1 is required).  The next step starts at the tick that ends the first
 * frame after the last relative press, a frame with nothing pressed: two steps' presses are always separated by at least one
 * released frame, so the game sees every press as a new one.  A step without relative lines hands over in the same tick. */
typedef struct { int kind, arg, neg; } Cond;
enum { ST_WAIT, ST_PRESS, ST_SELECT, ST_EXPECT };
static const char *st_name[]={"@wait","@press","@select","@expect"};
typedef struct { u64 a,b; ScriptPad pad; } RelRange;
typedef struct { int type; Cond c; ScriptPad pad; u64 every, hold, timeout; unsigned line; char text[160]; RelRange *rel; size_t nrel; u64 maxb;
                 u64 started, met, waited; } Step;
static Step *steps; static size_t nsteps;
static char route_path[512];
#define ARG_DIFFICULTY (-2)                /* a condition argument `difficulty`: the route's difficulty (@difficulty N or KHNMC_ROUTE_DIFFICULTY) */
static int route_difficulty=-1; static const char *route_difficulty_from="";
static size_t cur_step; static int step_phase;            /* 0 not started, 1 waiting / pressing, 2 relative presses */
static u64 step_t0, step_hits0, step_anchor; static int route_done;
static ScriptPad route_pad;                               /* the pad of the next frame (counter = game_frames), written by route_tick */
static long long sel_from=-1, sel_until=-1; static int sel_dir, sel_v0;
static int (*cond_fn)(KCtx *,int,int,u64,u64,u64)=kh_route_cond_eval;    /* the selftest replaces both with recorders */
static int (*cursor_fn)(KCtx *,int)=kh_route_cursor;
static int route_exit_at_end;                             /* KHNMC_ROUTE_END=exit: the run ends (exit 0, with the reports) when the route is done */
u64 kh_phase_damage_hits(void);
static int cond_true(KCtx *g,const Cond *c,u64 n){ int v=cond_fn(g,c->kind,c->arg,n,step_t0,step_hits0)!=0; return c->neg?!v:v; }
static void route_fatal_state(KCtx *g,u64 n,const char *name,const Step *s,const char *what){
    char st[700]; kh_route_state_summary(g,n,st,sizeof st);
    kh_fatal(g,"%s: %s line %u `%s`: %s (step %zu of %zu, started at game frame %llu, now game frame %llu, present %llu); game state now: %s",
             name,route_path,s->line,s->text,what,cur_step+1,nsteps,(unsigned long long)s->started,(unsigned long long)n,(unsigned long long)atomic_load(&gfx_presents),st);
}
static void step_met(Step *s,u64 n,u64 anchor){
    s->met=n; s->waited=n-s->started; step_anchor=anchor; step_phase=2;
    fprintf(stderr,"kh: route: step %zu/%zu line %u `%s` met at game frame %llu (present %llu, after %llu game frames)\n",cur_step+1,nsteps,s->line,s->text,
            (unsigned long long)n,(unsigned long long)atomic_load(&gfx_presents),(unsigned long long)s->waited);
}
static void route_tick(KCtx *g,u64 n){
    if(!nsteps || route_done) return;
    for(int guard=0;guard<=(int)nsteps+1;guard++){
        if(cur_step>=nsteps){
            route_done=1; memset(&route_pad,0,sizeof route_pad);
            fprintf(stderr,"kh: route: all %zu steps done at game frame %llu (present %llu)\n",nsteps,(unsigned long long)n,(unsigned long long)atomic_load(&gfx_presents));
            if(route_exit_at_end){ extern void kh_exit_process(int); kh_warn("input: KHNMC_ROUTE_END=exit: the route is done, ending the run"); kh_exit_process(0); }
            return;
        }
        Step *s=&steps[cur_step];
        if(step_phase==0){ step_phase=1; step_t0=n; step_hits0=kh_phase_damage_hits(); s->started=n; sel_from=sel_until=-1; }
        if(step_phase==1){
            int ok=cond_true(g,&s->c,n);
            if(s->type==ST_EXPECT){ if(!ok) route_fatal_state(g,n,"route-expect-failed",s,"the game is not in this state"); step_met(s,n,n-1); }
            else if(s->type==ST_WAIT){ if(ok) step_met(s,n,n-1); }
            else if(s->type==ST_PRESS){
                int pressed_last=n-1>=step_t0 && (n-1-step_t0)%s->every<s->hold;
                if(ok) step_met(s,n,pressed_last?n:n-1);        /* keep one released frame before the next press */
            } else {                                           /* ST_SELECT: move the cursor one row at a time until it is on the row */
                int pressing=sel_from>=0 && (long long)n-1<sel_until;
                int v=cursor_fn(g,s->c.kind);
                if(!pressing){
                    if(v==s->c.arg) step_met(s,n,n-1);
                    else if(v>=0 && (sel_from<0 || ((long long)n>=sel_until+2 && (v!=sel_v0 || (long long)n>=sel_until+20)))){
                        sel_from=(long long)n; sel_until=(long long)n+(long long)s->hold; sel_dir=v<s->c.arg?1:0; sel_v0=v;   /* bit 1 = DPAD_DOWN, bit 0 = DPAD_UP */
                    }
                }
            }
            if(step_phase==1){
                if(n-s->started>=s->timeout){
                    char what[400];
                    if(s->type==ST_SELECT) snprintf(what,sizeof what,"the cursor (%s) did not reach row %d within %llu game frames; it is on row %d (-1 = not up)",
                                                     kh_route_cond_info[s->c.kind].name,s->c.arg,(unsigned long long)s->timeout,cursor_fn(g,s->c.kind));
                    else snprintf(what,sizeof what,"%s%s never came within %llu game frames",s->c.neg?"NOT: ":"",kh_route_cond_info[s->c.kind].what,(unsigned long long)s->timeout);
                    route_fatal_state(g,n,"route-wait-timeout",s,what);
                }
                break;
            }
        }
        /* step_phase == 2: the relative presses; done once the frame after the last one (released) has ended */
        if(!s->nrel || n-1>=step_anchor+s->maxb+1){ cur_step++; step_phase=0; continue; }
        break;
    }
    ScriptPad pad={0};
    if(!route_done && cur_step<nsteps){
        Step *s=&steps[cur_step];
        if(step_phase==2){ for(size_t i=0;i<s->nrel;i++) if(n>=step_anchor+s->rel[i].a && n<=step_anchor+s->rel[i].b) pad=s->rel[i].pad; }
        else if(step_phase==1 && s->type==ST_PRESS && n>=step_t0 && (n-step_t0)%s->every<s->hold) pad=s->pad;
        else if(step_phase==1 && s->type==ST_SELECT && sel_from>=0 && (long long)n>=sel_from && (long long)n<sel_until) pad.buttons=sel_dir?0x2:0x1;
    }
    pthread_mutex_lock(&input_lock); route_pad=pad; pthread_mutex_unlock(&input_lock);
}
static void route_report(FILE *f){
    size_t done=0; for(size_t i=0;i<nsteps;i++) if(steps[i].met) done++;
    fprintf(f,"kh: route report: %zu of %zu steps done%s; difficulty %d%s; met at game frame:",done,nsteps,route_done?"":" -- ROUTE INCOMPLETE",route_difficulty,route_difficulty_from);
    for(size_t i=0;i<nsteps;i++) if(steps[i].met) fprintf(f," L%u:%s=%llu",steps[i].line,steps[i].type==ST_SELECT?"select":kh_route_cond_info[steps[i].c.kind].name,(unsigned long long)steps[i].met);
    if(!route_done && cur_step<nsteps) fprintf(f,"; waiting at line %u `%s` since game frame %llu",steps[cur_step].line,steps[cur_step].text,(unsigned long long)steps[cur_step].started);
    fputc('\n',f);
}
void kh_input_report(FILE *f){
    if(!scripted) return;
    fprintf(f,"kh: input report: presents=%llu game-frames=%llu last-script-frame=%llu clock=%s hold=%s\n",(unsigned long long)atomic_load(&gfx_presents),(unsigned long long)atomic_load(&game_frames),
            (unsigned long long)atomic_load(&last_script_frame),nsteps?"game-frames (route)":clock_game?"game-frames":"presents",hold_frame<0?"none":atomic_load(&hold_released)?"released":"NEVER RELEASED (the awaited state was not reached)");
    if(nsteps) route_report(f);
}
int kh_input_hold_pending(void){ return scripted && hold_frame>=0 && !atomic_load(&hold_released); }
static int parse_buttons(char *w,ScriptPad *pad){
    if(!strcmp(w,"-")) return 1;
    size_t len=strlen(w); if(!len || w[0]=='+'||w[len-1]=='+'||strstr(w,"++")) return 0;
    char *bs; for(char *b=strtok_r(w,"+",&bs);b;b=strtok_r(NULL,"+",&bs)){
        int bit; for(bit=0;bit<18;bit++) if(button_names[bit]&&!strcmp(b,button_names[bit])) break;
        if(bit==18) return 0;
        if(bit==16) pad->lt=255; else if(bit==17) pad->rt=255; else pad->buttons|=1u<<bit;
    }
    return 1;
}
/* `[not] NAME [ARG]` from words[*i]; ARG may be `difficulty`.  0 + a reason on error */
static int parse_cond(char **words,int n,int *i,Cond *c,const char **why){
    c->neg=0; c->arg=-1;
    if(*i<n && !strcmp(words[*i],"not")){ c->neg=1; (*i)++; }
    if(*i>=n){ *why="a condition is missing"; return 0; }
    c->kind=kh_route_cond_lookup(words[*i]); if(c->kind<0){ *why="unknown condition (known: initial-state title-screen title-menu title-choice newgame-dialog newgame-choice game-level field battle hits message frames)"; return 0; }
    (*i)++;
    const KhRouteCondInfo *ci=&kh_route_cond_info[c->kind];
    if(ci->has_arg){
        long long v;
        if(*i>=n){ *why="this condition takes a number"; return 0; }
        if(!strcmp(words[*i],"difficulty") && ci->hi>=3 && ci->lo==0) c->arg=ARG_DIFFICULTY;
        else if(number(words[*i],ci->lo,ci->hi,&v)) c->arg=(int)v;
        else { *why="the condition's number is out of range"; return 0; }
        (*i)++;
    }
    return 1;
}
static void route_free(void){ for(size_t i=0;i<nsteps;i++) free(steps[i].rel); free(steps); steps=NULL; nsteps=0; }
void kh_input_load(KCtx *g,const char *path){
    free(ranges); ranges=NULL; nranges=0; scripted=path!=NULL; packet=0; memset(&last_pad,0,sizeof last_pad);
    hold_frame=-1; hold_state=-1; atomic_store(&hold_shift,0); atomic_store(&hold_released,0); clock_game=0;
    route_free(); cur_step=0; step_phase=0; route_done=0; memset(&route_pad,0,sizeof route_pad); route_difficulty=-1; route_difficulty_from=""; sel_from=sel_until=-1;
    if(!path) return;
    snprintf(route_path,sizeof route_path,"%s",path);
    FILE *f=fopen(path,"r"); if(!f) kh_fatal(g,"scripted XInput: cannot open %s",path);
    char line[1024], text[1024]; unsigned lineno=0, first_frame_line=0; u64 timeout_default=3600; int uses_difficulty=0;
    const char *why="";
    while(fgets(line,sizeof line,f)){
        lineno++; why="";
        if(strlen(line)==sizeof line-1 && line[sizeof line-2]!='\n') goto invalid;
        char *comment=strchr(line,'#'); if(comment) *comment=0;
        snprintf(text,sizeof text,"%s",line); { char *e=text+strlen(text); while(e>text && (e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '||e[-1]=='\t')) *--e=0; }
        char *t0=text; while(*t0==' '||*t0=='\t') t0++;
        char *words[20],*save; int n=0;
        for(char *w=strtok_r(line," \t\r\n",&save);w;w=strtok_r(NULL," \t\r\n",&save)){
            if(n==20) goto invalid; words[n++]=w;
        }
        if(!n) continue;
        if(!strcmp(words[0],"@hold")){
            long long hf,hs;
            if(n!=5||strcmp(words[2],"until")||strcmp(words[3],"initial-state")||!number(words[1],1,LLONG_MAX,&hf)||!number(words[4],0,63,&hs)||hold_frame>=0) goto invalid;
            hold_frame=hf; hold_state=(int)hs; if(!first_frame_line) first_frame_line=lineno; continue;
        }
        if(!strcmp(words[0],"@clock")){ if(n!=2||strcmp(words[1],"game-frames")||clock_game) goto invalid; clock_game=1; if(!first_frame_line) first_frame_line=lineno; continue; }
        if(!strcmp(words[0],"@timeout")){ long long v; if(n!=2||!number(words[1],1,100000000,&v)){ why="@timeout takes a number of game frames"; goto invalid; } timeout_default=(u64)v; continue; }
        if(!strcmp(words[0],"@difficulty")){ long long v; if(n!=2||!number(words[1],0,3,&v)||route_difficulty>=0){ why="@difficulty takes 0..3, once"; goto invalid; } route_difficulty=(int)v; route_difficulty_from=" (@difficulty)"; continue; }
        if(!strcmp(words[0],"@wait")||!strcmp(words[0],"@expect")||!strcmp(words[0],"@press")||!strcmp(words[0],"@select")){
            Step st; memset(&st,0,sizeof st); st.line=lineno; st.timeout=timeout_default; st.every=8; st.hold=2;
            snprintf(st.text,sizeof st.text,"%s",t0);
            int i=1;
            if(!strcmp(words[0],"@wait")||!strcmp(words[0],"@expect")){
                st.type=words[0][1]=='w'?ST_WAIT:ST_EXPECT;
                if(!parse_cond(words,n,&i,&st.c,&why)) goto invalid;
            } else if(!strcmp(words[0],"@press")){
                st.type=ST_PRESS;
                if(n<4 || !parse_buttons(words[1],&st.pad) || !st.pad.buttons&&!st.pad.lt&&!st.pad.rt){ why="@press BUTTONS [lx ly] until COND: bad buttons"; goto invalid; }
                i=2; long long x,y;
                if(i+1<n && number(words[i],-32768,32767,&x) && number(words[i+1],-32768,32767,&y)){ st.pad.lx=(s16)x; st.pad.ly=(s16)y; i+=2; }
                if(i>=n||strcmp(words[i],"until")){ why="@press BUTTONS [lx ly] until COND"; goto invalid; }
                i++; if(!parse_cond(words,n,&i,&st.c,&why)) goto invalid;
            } else {
                st.type=ST_SELECT; st.hold=2;
                if(n<3){ why="@select title-choice|newgame-choice N"; goto invalid; }
                if(!parse_cond(words,n,&i,&st.c,&why)) goto invalid;
                if(st.c.neg || (st.c.kind!=KR_TITLE_CHOICE && st.c.kind!=KR_NEWGAME_CHOICE)){ why="@select moves title-choice or newgame-choice"; goto invalid; }
            }
            while(i<n){                                    /* options: every P, hold H (@press), timeout T (every step but @expect) */
                long long v; if(i+1>=n || !number(words[i+1],1,100000000,&v)){ why="an option takes a number"; goto invalid; }
                if(!strcmp(words[i],"timeout") && st.type!=ST_EXPECT) st.timeout=(u64)v;
                else if(!strcmp(words[i],"every") && st.type==ST_PRESS) st.every=(u64)v;
                else if(!strcmp(words[i],"hold") && st.type==ST_PRESS) st.hold=(u64)v;
                else { why="unknown option for this directive (timeout; @press also every, hold)"; goto invalid; }
                i+=2;
            }
            if(st.type==ST_PRESS && st.hold>=st.every){ why="@press: hold must be shorter than every (a press needs a release before the next)"; goto invalid; }
            if(st.c.arg==ARG_DIFFICULTY) uses_difficulty=1;
            Step *next=realloc(steps,(nsteps+1)*sizeof *steps); if(!next) kh_fatal(g,"scripted XInput: allocation failed");
            steps=next; steps[nsteps++]=st; continue;
        }
        if(words[0][0]=='@'){ why="unknown directive"; goto invalid; }
        if(words[0][0]=='+'){                              /* relative press of the last step */
            long long a,b,x=0,y=0;
            if(!nsteps){ why="a relative line (+A +B) needs a step (@wait, @press, @select, @expect) before it"; goto invalid; }
            if((n!=3 && n!=5) || words[1][0]!='+' || !number(words[0]+1,1,100000000,&a) || !number(words[1]+1,a,100000000,&b)){ why="+A +B BUTTONS [lx ly] with 1 <= A <= B"; goto invalid; }
            if(n==5 && (!number(words[3],-32768,32767,&x)||!number(words[4],-32768,32767,&y))) goto invalid;
            ScriptPad pad={.lx=(s16)x,.ly=(s16)y}; if(!parse_buttons(words[2],&pad)) goto invalid;
            Step *s=&steps[nsteps-1];
            RelRange *next=realloc(s->rel,(s->nrel+1)*sizeof *s->rel); if(!next) kh_fatal(g,"scripted XInput: allocation failed");
            s->rel=next; s->rel[s->nrel++]=(RelRange){(u64)a,(u64)b,pad}; if((u64)b>s->maxb) s->maxb=(u64)b;
            continue;
        }
        if(n!=3 && n!=5) goto invalid;
        long long a,b,x=0,y=0;
        if(!number(words[0],0,LLONG_MAX,&a)||!number(words[1],a,LLONG_MAX,&b)) goto invalid;
        if(n==5 && (!number(words[3],-32768,32767,&x)||!number(words[4],-32768,32767,&y))) goto invalid;
        ScriptPad pad={.lx=x,.ly=y};
        if(!parse_buttons(words[2],&pad)) goto invalid;
        InputRange *next=realloc(ranges,(nranges+1)*sizeof *ranges);
        if(!next) kh_fatal(g,"scripted XInput: allocation failed"); ranges=next;
        ranges[nranges++]=(InputRange){a,b,pad}; if(!first_frame_line) first_frame_line=lineno;
        continue;
invalid: kh_fatal(g,"scripted XInput: invalid %s line %u%s%s",path,lineno,*why?": ":"",why);
    }
    if(ferror(f)) kh_fatal(g,"scripted XInput: read failed %s",path);
    fclose(f);
    if(clock_game && hold_frame<0) kh_fatal(g,"scripted XInput: %s: @clock game-frames needs an @hold line (the point from which game frames are counted)",path);
    if(nsteps && first_frame_line) kh_fatal(g,"scripted XInput: %s: line %u is a frame-numbered line (FROM TO, @hold or @clock) in a route of @wait / @press / @select / @expect steps; "
                                          "a route presses relative to the state it waited for (+A +B BUTTONS): the two kinds cannot be mixed",path,first_frame_line);
    if(nsteps){
        const char *e=getenv("KHNMC_ROUTE_DIFFICULTY");
        if(e&&*e){ if(e[0]<'0'||e[0]>'3'||e[1]) kh_fatal(g,"KHNMC_ROUTE_DIFFICULTY must be one of 0,1,2,3 (0 Beginner, 1 Standard, 2 Critical, 3 Less Than Nothing)"); route_difficulty=e[0]-'0'; route_difficulty_from=" (KHNMC_ROUTE_DIFFICULTY)"; }
        if(uses_difficulty && route_difficulty<0) kh_fatal(g,"scripted XInput: %s uses `difficulty` in a condition but sets none: add `@difficulty N` or set KHNMC_ROUTE_DIFFICULTY",path);
        for(size_t i=0;i<nsteps;i++) if(steps[i].c.arg==ARG_DIFFICULTY) steps[i].c.arg=route_difficulty;
        { const char *x=getenv("KHNMC_ROUTE_END"); route_exit_at_end=0;
          if(x&&*x){ if(!strcmp(x,"exit")) route_exit_at_end=1; else if(strcmp(x,"idle")) kh_fatal(g,"KHNMC_ROUTE_END=%s: known values are idle (the default: after the last step the pad stays released) and exit (the run ends, exit 0)",x); } }
        kh_warn("scripted XInput pad 0: route of %zu steps from %s (difficulty %d%s; game-frame clock; after the last step: %s)",nsteps,path,route_difficulty,route_difficulty_from,route_exit_at_end?"exit":"idle");
    } else kh_warn("scripted XInput pad 0: %zu ranges from %s",nranges,path);
}
u32 kh_input_state(KCtx *g,u32 slot,u64 out,u64 frame){
    if(!scripted||slot) return 1167;
    if(!out) return 87;
    ScriptPad pad={0};
    pthread_mutex_lock(&input_lock);
    if(nsteps) pad=route_pad;                               /* a route: the pad route_tick computed for this game frame */
    else for(size_t i=0;i<nranges;i++) if(frame>=ranges[i].start && frame<=ranges[i].end) pad=ranges[i].pad;
    if(!packet || memcmp(&last_pad,&pad,sizeof pad)){ packet++; last_pad=pad; }
    wr32(g,out,packet); wr16(g,out+4,pad.buttons); wr8(g,out+6,pad.lt); wr8(g,out+7,pad.rt);
    wr16(g,out+8,(u16)pad.lx); wr16(g,out+10,(u16)pad.ly); wr16(g,out+12,0); wr16(g,out+14,0);
    pthread_mutex_unlock(&input_lock); return 0;
}
static void input_get(KCtx *g){ kh_cnt_input_calls++; u64 sf=nsteps?atomic_load(&game_frames):script_frame(atomic_load(&gfx_presents)); atomic_store(&last_script_frame,sf); SET_RAX(g,kh_input_state(g,(u32)A0(g),A1(g),sf)); }
IMP(xinput1_4,XInputGetState){ input_get(g); }
IMP(xinput1_4,XInputGetStateEx){ input_get(g); }
IMP(xinput1_4,ord100){ input_get(g); }
static int C_di,C_dev;
static int h_di(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==3){ wr64(g,A2(g),kh_com_obj(C_dev,16)); SET_RAX(g,0); return 1; }          /* CreateDevice(guid, **ppDevice, outer) */
    SET_RAX(g,0); return 1; }                                                          /* EnumDevices etc.: S_OK, no callbacks */
static int h_dev(KCtx *g,u64 o,int s,const char *c){ (void)o;(void)c;
    if(s==9){ memset(hp(g,A2(g)),0,(size_t)(u32)A1(g)); SET_RAX(g,0); return 1; }    /* GetDeviceState(cb, data): all zero */
    SET_RAX(g,0); return 1; }
void kh_seam_input_init(void){ kh_input_load(NULL,getenv("KHNMC_INPUT_SCRIPT"));
    { const char *e=getenv("KH_RUN_GAME_FRAMES"); if(e&&*e) run_game_frames=strtoull(e,0,0); }
    if(scripted){
        u64 get=kh_host_fn("XInputGetState",rt_imp_xinput1_4__XInputGetState);
        const char *aliases[]={"XInputGetState","XInputGetStateEx","#2","#100"};
        for(size_t i=0;i<sizeof aliases/sizeof *aliases;i++) kh_register_proc("xinput1_4.dll",aliases[i],get);
    }
    C_di=kh_com_class("di:IDirectInput8",16,0,h_di); C_dev=kh_com_class("di:IDirectInputDevice8",40,0,h_dev); }
IMP(xinput1_4,ord2){ input_get(g); }
IMP(xinput1_4,ord3){ kh_cnt_input_calls++; SET_RAX(g,scripted && (u32)A0(g)==0?0:1167); }
IMP(dinput8,DirectInput8Create){ kh_cnt_input_calls++; wr64(g,A3(g),kh_com_obj(C_di,16)); SET_RAX(g,0); }
IMP(setupapi,SetupDiGetClassDevsW){ kh_cnt_input_calls++; SET_RAX(g,(u64)-1); kh_set_last_error(13); }
IMP(setupapi,SetupDiEnumDeviceInterfaces){ kh_cnt_input_calls++; kh_set_last_error(259); SET_RAX(g,0); }
IMP(setupapi,SetupDiDestroyDeviceInfoList){ kh_cnt_input_calls++; SET_RAX(g,1); }
IMP(hid,HidD_GetHidGuid){ kh_cnt_input_calls++; memset(hp(g,A0(g)),0,16); }

/* The real probe is reached from DInput enumeration, absent on a null host.
 * Supply the known virtual candidate and bypass WMI only for that product id. */
#define SCRIPT_PRODUCT 0x028e045e
void orig_f_131ef0(KCtx *g);
void f_131ef0(KCtx *g){
    if(scripted && (u32)A1(g)==SCRIPT_PRODUCT){ SET_RAX(g,1); KH_RET(g); return; }
    orig_f_131ef0(g);
}
void kh_input_attach(KCtx *g){
    if(!scripted) return;
    u64 pad=GS_BASE+0x8bb2a8;
    wr32(g,pad+0x48,SCRIPT_PRODUCT);
    memset(hp(g,pad+0x50),0xff,24); /* identity profile for the virtual candidate */
    kh_call(g,GS_BASE+0x133970,GS_BASE+0x8bb290,pad,0,0);
    if((s32)rd32(g,pad+0x10)!=0 || !rd64(g,pad+0xb8)) kh_fatal(g,"scripted XInput: native backend installation failed");
    kh_warn("scripted XInput: native pad 0 backends installed");
}

/* selftest of the `@hold` directive: the script clock before, at and after the awaited state, late and early */
int kh_input_hold_selftest(KCtx *g){
    int checks=0,fails=0;
#define ICHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"input hold selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    void kh_phase_test_set_initial_first(int state,long long present);
    long long saved=kh_phase_initial_first(3);
    char path[KH_TMP_MAX]; kh_tmp_path(path,sizeof path,"khnmc-hold-XXXXXX"); int fd=mkstemp(path); ICHECK(fd>=0); if(fd<0) return fails;
    const char *txt="# hold test\n@hold 165 until initial-state 3\n180 182 A\n"; ICHECK(write(fd,txt,strlen(txt))==(ssize_t)strlen(txt)); close(fd);
    u64 out=ga_alloc(32,16);
    kh_phase_test_set_initial_first(3,-1); kh_input_load(g,path);
    ICHECK(kh_input_hold_pending()==1);
    ICHECK(script_frame(100)==100 && script_frame(164)==164 && script_frame(165)==164 && script_frame(5000)==164);   /* held */
    ICHECK(kh_input_state(g,0,out,script_frame(182))==0 && rd16(g,out+4)==0);                                         /* A at 180 is not sampled yet */
    kh_phase_test_set_initial_first(3,186);                                                                             /* the prompt comes up late */
    ICHECK(script_frame(186)==164 && script_frame(187)==165 && script_frame(202)==180 && script_frame(1000)==978);
    ICHECK(kh_input_hold_pending()==0);
    ICHECK(kh_input_state(g,0,out,script_frame(201))==0 && rd16(g,out+4)==0);
    ICHECK(kh_input_state(g,0,out,script_frame(202))==0 && rd16(g,out+4)==0x1000);                                    /* A, 16 presents after the prompt */
    kh_phase_test_set_initial_first(3,101); kh_input_load(g,path);                                                      /* the prompt is up in time: nothing changes */
    ICHECK(script_frame(101)==101 && script_frame(164)==164 && script_frame(165)==165 && script_frame(180)==180 && kh_input_hold_pending()==0);
    kh_input_load(g,NULL); ICHECK(script_frame(165)==165 && script_frame(9999)==9999);                                  /* no script, no hold */
    /* @clock game-frames: after the release the script follows the game thread's frames, whatever the present counter does */
    { char p2[KH_TMP_MAX]; kh_tmp_path(p2,sizeof p2,"khnmc-hold-XXXXXX"); int f2=mkstemp(p2); ICHECK(f2>=0);
      if(f2>=0){ const char *t2="@hold 165 until initial-state 3\n@clock game-frames\n180 182 A\n"; ICHECK(write(f2,t2,strlen(t2))==(ssize_t)strlen(t2)); close(f2);
        u64 sv_gf=atomic_load(&game_frames);
        kh_phase_test_set_initial_first(3,-1); kh_input_load(g,p2); atomic_store(&game_frames,1000);
        ICHECK(script_frame(100)==100 && script_frame(400)==164);                                                       /* before the release: presents, held */
        kh_phase_test_set_initial_first(3,304); ICHECK(script_frame(304)==164);                                         /* released at present 304, game frame 1000 */
        atomic_store(&game_frames,1010); ICHECK(script_frame(600)==174 && script_frame(9000)==174);                     /* 10 game frames later, however many presents */
        atomic_store(&game_frames,1016); ICHECK(script_frame(601)==180 && kh_input_state(g,0,out,script_frame(601))==0 && rd16(g,out+4)==0x1000);
        kh_phase_test_set_initial_first(3,101); kh_input_load(g,p2); atomic_store(&game_frames,50);                     /* in time: anchored at the present of the first poll after the sighting */
        ICHECK(script_frame(120)==120); atomic_store(&game_frames,110); ICHECK(script_frame(500)==180);
        atomic_store(&game_frames,sv_gf); unlink(p2); } }
    unlink(path); ga_free(out); kh_phase_test_set_initial_first(3,saved);
    kh_input_load(g,getenv("KHNMC_INPUT_SCRIPT"));
    fprintf(stderr,"kh: input hold selftest: %d checks, %d failures\n",checks,fails);
    return fails;
}

/* ---- task 27: selftest of the route steps and of the game-state readers ------------------------------------------------------------
 * Part 1 drives route_tick with a recorded condition table (the readers are replaced, as other suites replace guest helpers): a state
 * met early / late / never (the named fatal, in a child process), relative presses and the released frame between two steps, @press,
 * @select both ways, @expect, the difficulty parameter, the game-frame clock and the load-time fatals.  Part 2 runs the REAL readers
 * of runtime/route_state.c on synthetic game memory (the globals they read are written, checked and restored). */
static int t_val[KR_NCOND]; static int t_cursor[KR_NCOND]; static int t_cursor_step;   /* cursor moves by one per DPAD press seen */
static int t_cond(KCtx *g,int kind,int arg,u64 n,u64 sf,u64 sh){ (void)g;(void)sh;
    if(kind==KR_FRAMES) return n-sf>=(u64)arg;
    const KhRouteCondInfo *ci=&kh_route_cond_info[kind]; return ci->has_arg ? t_val[kind]==arg : t_val[kind]!=0; }
static int t_cur(KCtx *g,int kind){ (void)g; return t_cursor[kind]; }
static char t_path[KH_TMP_MAX];
static int t_load(KCtx *g,const char *txt){
    kh_tmp_path(t_path,sizeof t_path,"khnmc-route-XXXXXX"); int fd=mkstemp(t_path); if(fd<0) return 0;
    int ok=write(fd,txt,strlen(txt))==(ssize_t)strlen(txt); close(fd); kh_input_load(g,t_path); unlink(t_path); return ok; }
static u16 t_pad(void){ return route_pad.buttons; }
/* ticks n0..n1 (inclusive) with the recorded table; bits[k] = buttons of counter n0+k (the pad each tick computes for the next frame) */
static void t_run(KCtx *g,u64 n0,u64 n1,u16 *bits){ for(u64 n=n0;n<=n1;n++){ route_tick(g,n); if(bits) bits[n-n0]=t_pad();
        for(int k=0;k<KR_NCOND;k++) if(t_cursor_step && (k==KR_TITLE_CHOICE||k==KR_NEWGAME_CHOICE) && t_cursor[k]>=0){   /* the "game" moves the cursor on a new DPAD press */
            static u16 prev; u16 b=t_pad(); if((b&3) && !(prev&3)) t_cursor[k]+= (b&2)?1:-1; prev=b; } } }
static KCtx *t_g; static const char *t_fatal_txt; static u64 t_fatal_ticks; static char t_child_path[KH_TMP_MAX];
static void t_fatal_body(void){ kh_input_load(t_g,t_child_path); t_run(t_g,1,t_fatal_ticks,NULL); _exit(0); }
/* the script file is written and removed by the parent: a child that ends in a named fatal leaves nothing behind */
static int t_child(void (*body)(void),char *out,size_t cap){
    kh_tmp_path(t_child_path,sizeof t_child_path,"khnmc-route-XXXXXX"); int fd=mkstemp(t_child_path); if(fd<0) return -1;
    int ok=write(fd,t_fatal_txt,strlen(t_fatal_txt))==(ssize_t)strlen(t_fatal_txt); close(fd); if(!ok){ unlink(t_child_path); return -1; }
    FILE *log=kh_tmpfile(); if(!log){ unlink(t_child_path); return -1; } fflush(stderr);
    pid_t c=kh_fork(); if(c<0){ fclose(log); unlink(t_child_path); return -1; }
    if(!c){ dup2(fileno(log),2); body(); _exit(99); }
    int st=0; waitpid(c,&st,0); unlink(t_child_path); rewind(log); size_t n=fread(out,1,cap-1,log); out[n]=0; fclose(log);
    return WIFEXITED(st)?WEXITSTATUS(st):-1;
}
void kh_phase_test_set_alive(int initial,int title,int newgame); void kh_phase_test_set_initial_state(int st); void kh_phase_test_set_damage(u64 hits,u64 last_frame);
int kh_phase_initial_alive(void); int kh_phase_title_alive(void); int kh_phase_newgame_alive(void); int kh_phase_initial_state(void); u64 kh_phase_damage_last_frame(void);
void kh_route_state_test_set_field_cmds(u64 per_frame); void kh_route_state_test_reset(void);
int kh_input_route_selftest(KCtx *g){
    int checks=0,fails=0; t_g=g;
#define RCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"input route selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    int (*sv_cond)(KCtx *,int,int,u64,u64,u64)=cond_fn; int (*sv_cur)(KCtx *,int)=cursor_fn; cond_fn=t_cond; cursor_fn=t_cur;
    char *sv_env=getenv("KHNMC_ROUTE_DIFFICULTY"); char sv_envv[16]=""; if(sv_env) snprintf(sv_envv,sizeof sv_envv,"%s",sv_env); unsetenv("KHNMC_ROUTE_DIFFICULTY");
    u16 b[200]; u64 out=ga_alloc(32,16);
    /* 1. late: not met at ticks 1..9, met at 10 -> anchor 9, A on counters 10..12, released 13, next step starts at tick 14 */
    memset(t_val,0,sizeof t_val); memset(t_cursor,0xff,sizeof t_cursor); t_cursor_step=0;
    RCHECK(t_load(g,"@wait title-menu timeout 500\n+1 +3 A\n@wait field\n+2 +2 B\n") && nsteps==2 && steps[0].nrel==1 && steps[0].maxb==3 && steps[0].timeout==500);
    t_run(g,1,9,b); RCHECK(cur_step==0 && step_phase==1 && b[8]==0);
    t_val[KR_TITLE_MENU]=1; t_run(g,10,13,b);
    RCHECK(steps[0].met==10 && b[0]==0x1000 && b[1]==0x1000 && b[2]==0x1000 && b[3]==0 && cur_step==0);
    t_val[KR_FIELD]=1; t_run(g,14,16,b);                 /* tick 14: step 2 starts and is met (anchor 13): B on counter 15 only */
    RCHECK(cur_step==1 && steps[1].met==14 && b[0]==0 && b[1]==0x2000 && b[2]==0 && !route_done);
    t_run(g,17,17,NULL); RCHECK(route_done && t_pad()==0);   /* tick 17 ends counter 16, the released frame after +2: the route is done */
    /* the pad goes out through XInputGetState whatever frame number the caller passes: route mode is on the game-frame clock */
    RCHECK(kh_input_state(g,0,out,123456)==0 && rd16(g,out+4)==0);
    /* 2. early: the state is already there at the first tick -> A on counters 1..3 */
    memset(t_val,0,sizeof t_val); t_val[KR_TITLE_MENU]=1;
    RCHECK(t_load(g,"@wait title-menu\n+1 +3 A\n")); t_run(g,1,4,b);
    RCHECK(steps[0].met==1 && b[0]==0x1000 && b[2]==0x1000 && b[3]==0 && route_done==0); t_run(g,5,5,NULL); RCHECK(route_done);
    /* 3. the released frame between two steps whose states are both already there; a wait with no presses hands over in the same tick */
    memset(t_val,0,sizeof t_val); t_val[KR_TITLE_MENU]=t_val[KR_FIELD]=t_val[KR_BATTLE]=1;
    RCHECK(t_load(g,"@wait title-menu\n+1 +2 A\n@wait battle\n@wait field\n+1 +1 B\n")); t_run(g,1,6,b);
    RCHECK(b[0]==0x1000 && b[1]==0x1000 && b[2]==0 && b[3]==0x2000 && b[4]==0 && steps[1].met==4 && steps[2].met==4);
    /* 4. relative lines: later lines win, sticks, `-` */
    memset(t_val,0,sizeof t_val); t_val[KR_FIELD]=1;
    RCHECK(t_load(g,"@wait field\n+1 +4 A 100 -200\n+2 +2 B\n+3 +3 -\n")); t_run(g,1,4,b);
    RCHECK(b[0]==0x1000 && route_pad.lx==100 && b[1]==0x2000 && b[2]==0 && b[3]==0x1000);
    RCHECK(kh_input_state(g,0,out,0)==0 && rd16(g,out+4)==0x1000 && (s16)rd16(g,out+8)==100 && (s16)rd16(g,out+10)==-200);
    /* 5. @press: A for 2 of every 6 frames from the step's first frame until the state; then one released frame before +1 */
    memset(t_val,0,sizeof t_val);
    RCHECK(t_load(g,"@press A until field every 6 hold 2 timeout 100\n+1 +1 X\n") && steps[0].every==6 && steps[0].hold==2);
    t_run(g,1,13,b); RCHECK(b[0]==0x1000 && b[1]==0x1000 && b[2]==0 && b[5]==0 && b[6]==0x1000 && b[7]==0x1000 && b[8]==0 && b[12]==0x1000);
    t_val[KR_FIELD]=1; t_run(g,14,16,b);              /* tick 14: counter 13 was pressed -> anchor 14, X on counter 15, nothing on 14 */
    RCHECK(steps[0].met==14 && b[0]==0 && b[1]==0x4000 && b[2]==0);
    /* 6. @select down (1 -> 3) and up (3 -> 0): one DPAD press per row, released frames between, then the relative confirm */
    memset(t_val,0,sizeof t_val); t_cursor[KR_NEWGAME_CHOICE]=1; t_cursor_step=1;
    RCHECK(t_load(g,"@select newgame-choice 3 timeout 300\n+1 +3 A\n")); t_run(g,1,40,b);
    { int downs=0,ups=0,a=0; for(int i=1;i<40;i++){ if((b[i]&2)&&!(b[i-1]&2)) downs++; if((b[i]&1)&&!(b[i-1]&1)) ups++; if(b[i]==0x1000) a++; }
      if(b[0]&2) downs++;
      RCHECK(t_cursor[KR_NEWGAME_CHOICE]==3 && downs==2 && ups==0 && a==3 && steps[0].met>0); }
    t_cursor[KR_NEWGAME_CHOICE]=3; RCHECK(t_load(g,"@select newgame-choice 0\n")); t_run(g,1,60,b);
    { int ups=0; for(int i=0;i<60;i++) if((b[i]&1)&&(i==0||!(b[i-1]&1))) ups++; RCHECK(t_cursor[KR_NEWGAME_CHOICE]==0 && ups==3 && route_done); }
    t_cursor_step=0; t_cursor[KR_NEWGAME_CHOICE]=-1;
    /* 7. `difficulty`: @difficulty, overridden by KHNMC_ROUTE_DIFFICULTY; frames N counts from the step's start */
    RCHECK(t_load(g,"@difficulty 2\n@wait newgame-choice difficulty\n@expect game-level difficulty\n") && route_difficulty==2 && steps[0].c.arg==2 && steps[1].c.arg==2);
    setenv("KHNMC_ROUTE_DIFFICULTY","0",1); RCHECK(t_load(g,"@difficulty 2\n@select newgame-choice difficulty\n") && route_difficulty==0 && steps[0].c.arg==0); unsetenv("KHNMC_ROUTE_DIFFICULTY");
    memset(t_val,0,sizeof t_val); RCHECK(t_load(g,"@wait frames 5\n+1 +1 A\n")); t_run(g,10,16,b);
    RCHECK(steps[0].met==15 && b[5]==0x1000 && b[4]==0 && b[6]==0);
    RCHECK(t_load(g,"@wait not field\n") && steps[0].c.neg==1); t_val[KR_FIELD]=1; t_run(g,1,3,NULL); RCHECK(!steps[0].met); t_val[KR_FIELD]=0; t_run(g,4,4,NULL); RCHECK(steps[0].met==4);
    /* 8. named fatals in a child: a state that never comes, a failed @expect, a cursor that never gets there, bad route files */
    if(kh_fork_ok()){
        char o[16384];
        memset(t_val,0,sizeof t_val);
        t_fatal_txt="@wait title-menu\n+1 +1 A\n@wait battle timeout 50\n"; t_val[KR_TITLE_MENU]=1; t_fatal_ticks=200;
        RCHECK(t_child(t_fatal_body,o,sizeof o)==3 && strstr(o,"route-wait-timeout:") && strstr(o,"line 3 `@wait battle timeout 50`") &&
               strstr(o,"a battle is in progress (a hit computed in the last 300 game frames) never came within 50 game frames") && strstr(o,"step 2 of 2") && strstr(o,"game state now:"));
        t_val[KR_TITLE_MENU]=0; t_fatal_txt="@expect title-menu\n";
        RCHECK(t_child(t_fatal_body,o,sizeof o)==3 && strstr(o,"route-expect-failed:") && strstr(o,"`@expect title-menu`"));
        t_fatal_txt="@press A until field timeout 30\n";
        RCHECK(t_child(t_fatal_body,o,sizeof o)==3 && strstr(o,"route-wait-timeout:") && strstr(o,"never came within 30 game frames"));
        t_cursor[KR_TITLE_CHOICE]=1; t_fatal_txt="@select title-choice 0 timeout 40\n";       /* the cursor never moves (t_cursor_step=0) */
        RCHECK(t_child(t_fatal_body,o,sizeof o)==3 && strstr(o,"did not reach row 0 within 40 game frames; it is on row 1")); t_cursor[KR_TITLE_CHOICE]=-1;
        static const struct { const char *txt, *msg; } bad[]={
            {"@wait field\n+0 +2 A\n","line 2: +A +B BUTTONS [lx ly] with 1 <= A <= B"},
            {"+1 +2 A\n","line 1: a relative line (+A +B) needs a step"},
            {"@wait field\n10 12 A\n","line 2 is a frame-numbered line"},
            {"@hold 165 until initial-state 3\n@wait field\n","line 1 is a frame-numbered line"},
            {"@wait sunshine\n","line 1: unknown condition"},
            {"@wait initial-state 64\n","line 1: the condition's number is out of range"},
            {"@select field 1\n","line 1: @select moves title-choice or newgame-choice"},
            {"@select newgame-choice\n","line 1: @select title-choice|newgame-choice N"},
            {"@press A until field every 2 hold 2\n","line 1: @press: hold must be shorter than every"},
            {"@expect field timeout 5\n","line 1: unknown option"},
            {"@select newgame-choice difficulty\n","uses `difficulty` in a condition but sets none"},
            {"@frobnicate\n","line 1: unknown directive"},
        };
        for(size_t i=0;i<sizeof bad/sizeof *bad;i++){ t_fatal_txt=bad[i].txt; t_fatal_ticks=1;
            int st=t_child(t_fatal_body,o,sizeof o); RCHECK(st==3 && strstr(o,"scripted XInput: ") && strstr(o,bad[i].msg)); if(st!=3||!strstr(o,bad[i].msg)) fprintf(stderr,"  case %zu: status %d, %zu bytes: %.400s\n",i,st,strlen(o),o); }
        t_fatal_ticks=200;
    } else kh_selftest_not_run("input route","route time-out / expect / select / load fatals (named fatals in a child)",16);
    /* 9. old scripts are untouched by the route code: a frame-numbered script keeps its ranges and has no steps */
    RCHECK(t_load(g,"# old\n@hold 165 until initial-state 3\n@clock game-frames\n180 182 A\n240 242 DPAD_UP+DPAD_LEFT\n") && nsteps==0 && nranges==2 && hold_frame==165 && clock_game==1);
    kh_input_load(g,NULL); cond_fn=sv_cond; cursor_fn=sv_cur;
    /* Part 2: the real readers on synthetic memory */
    {
        int sa_i=kh_phase_initial_alive(), sa_t=kh_phase_title_alive(), sa_n=kh_phase_newgame_alive(), sa_s=kh_phase_initial_state(); u64 sa_h=kh_phase_damage_hits(), sa_l=kh_phase_damage_last_frame();
        static const u64 G[]={0xb1d5d0,0xb1d5d8,0xb1d5e4,0xb1d5ec,0x9006b0,0xb1d7e0,0xb1d800,0xb1d808,0x9abd48,0x717008,0x2a16310};
        u64 saved[sizeof G/sizeof *G]; for(size_t i=0;i<sizeof G/sizeof *G;i++) saved[i]=rd64(g,GS_BASE+G[i]);
        u64 lay=ga_calloc(1,0x100), menu=ga_calloc(1,0x10), ctx=ga_calloc(1,0x20), ngl=ga_calloc(1,0x620), list=ga_calloc(1,0x30), win=ga_calloc(1,0x600);
#define EV(k,a) kh_route_cond_eval(g,(k),(a),1000,990,0)
        kh_phase_test_set_alive(1,0,0); kh_phase_test_set_initial_state(14);
        RCHECK(EV(KR_INITIAL_STATE,14) && !EV(KR_INITIAL_STATE,3)); kh_phase_test_set_alive(0,0,0); RCHECK(!EV(KR_INITIAL_STATE,14));
        /* title */
        wr64(g,GS_BASE+0xb1d5d0,lay); wr64(g,GS_BASE+0xb1d5d8,0); wr32(g,GS_BASE+0xb1d5ec,0); wr8(g,GS_BASE+0x9006b0,0); wr32(g,lay+0xe0,12); wr32(g,GS_BASE+0xb1d5e4,0);
        RCHECK(!EV(KR_TITLE_SCREEN,-1));                                    /* the title task is not running */
        kh_phase_test_set_alive(0,1,0);
        RCHECK(EV(KR_TITLE_SCREEN,-1) && !EV(KR_TITLE_MENU,-1)); wr32(g,lay+0xe0,17); RCHECK(!EV(KR_TITLE_SCREEN,-1)); wr32(g,lay+0xe0,13); RCHECK(EV(KR_TITLE_SCREEN,-1));
        wr64(g,GS_BASE+0xb1d5d8,menu); RCHECK(!EV(KR_TITLE_SCREEN,-1) && EV(KR_TITLE_MENU,-1) && EV(KR_TITLE_CHOICE,0) && !EV(KR_TITLE_CHOICE,1) && kh_route_cursor(g,KR_TITLE_CHOICE)==0);
        wr8(g,GS_BASE+0x9006b0,1); RCHECK(!EV(KR_TITLE_MENU,-1) && kh_route_cursor(g,KR_TITLE_CHOICE)==-1); wr8(g,GS_BASE+0x9006b0,0);
        wr64(g,GS_BASE+0xb1d5d0,0); RCHECK(!EV(KR_TITLE_MENU,-1)); wr64(g,GS_BASE+0xb1d5d0,lay);
        /* New Game menu */
        wr32(g,GS_BASE+0xb1d5ec,1); RCHECK(!EV(KR_TITLE_MENU,-1));
        wr64(g,GS_BASE+0xb1d800,ctx); wr64(g,GS_BASE+0xb1d808,ngl); wr64(g,GS_BASE+0xb1d7e0,list);
        wr32(g,ctx,0); wr32(g,ctx+4,1); wr32(g,ngl+0x614,0x1d); wr16(g,list,0); wr16(g,list+0x12,1); wr16(g,list+0x16,4);
        RCHECK(!EV(KR_NEWGAME_DIALOG,0));                                   /* the menu task is not running */
        kh_phase_test_set_alive(0,1,1);
        RCHECK(EV(KR_NEWGAME_DIALOG,0) && !EV(KR_NEWGAME_DIALOG,1) && EV(KR_NEWGAME_CHOICE,1) && kh_route_cursor(g,KR_NEWGAME_CHOICE)==1);
        wr16(g,list,2); wr16(g,list+0x12,3); RCHECK(kh_route_cursor(g,KR_NEWGAME_CHOICE)==1);             /* (2+3) mod 4 */
        wr16(g,list+0x16,0); RCHECK(kh_route_cursor(g,KR_NEWGAME_CHOICE)==5 && EV(KR_NEWGAME_CHOICE,5)); wr16(g,list+0x16,4);   /* no modulo when +0x16 is 0 */
        wr32(g,ctx+4,0); RCHECK(!EV(KR_NEWGAME_DIALOG,0) && kh_route_cursor(g,KR_NEWGAME_CHOICE)==-1); wr32(g,ctx+4,1);
        wr32(g,ngl+0x614,0x1c); RCHECK(!EV(KR_NEWGAME_DIALOG,0)); wr32(g,ngl+0x614,0x1d);
        wr32(g,ctx,2); RCHECK(EV(KR_NEWGAME_DIALOG,2)); wr32(g,ctx,7); RCHECK(!EV(KR_NEWGAME_DIALOG,2) && kh_route_cursor(g,KR_NEWGAME_CHOICE)==-1);
        kh_phase_test_set_alive(0,0,0);
        /* save, field, windows, battle */
        wr32(g,GS_BASE+0x9abd48,3); RCHECK(EV(KR_GAME_LEVEL,3) && !EV(KR_GAME_LEVEL,1));
        wr64(g,GS_BASE+0x2a16310,0); wr8(g,GS_BASE+0x717008,0x12); kh_route_state_test_set_field_cmds(1);
        RCHECK(EV(KR_FIELD,-1) && !EV(KR_MESSAGE,-1));
        wr8(g,GS_BASE+0x717008,0xff); RCHECK(!EV(KR_FIELD,-1)); wr8(g,GS_BASE+0x717008,0x12);
        kh_route_state_test_set_field_cmds(0); RCHECK(!EV(KR_FIELD,-1)); kh_route_state_test_set_field_cmds(1);
        wr64(g,GS_BASE+0x2a16310,win); wr32(g,win+0xa0,2); wr32(g,win+0x260,1); wr64(g,win+0x5c0,0); wr32(g,win+0xac,0);
        RCHECK(EV(KR_MESSAGE,-1) && !EV(KR_FIELD,-1));
        wr32(g,win+0x260,2); RCHECK(EV(KR_MESSAGE,-1)); wr32(g,win+0xa0,1); RCHECK(!EV(KR_MESSAGE,-1) && EV(KR_FIELD,-1));
        wr32(g,win+0xa0,2); wr64(g,win+0x5c0,win); RCHECK(!EV(KR_MESSAGE,-1)); wr64(g,GS_BASE+0x2a16310,0);
        kh_phase_test_set_damage(5,800);
        RCHECK(kh_route_cond_eval(g,KR_BATTLE,-1,1000,0,0) && kh_route_cond_eval(g,KR_BATTLE,-1,1100,0,0) && !kh_route_cond_eval(g,KR_BATTLE,-1,1101,0,0));
        RCHECK(kh_route_cond_eval(g,KR_HITS,2,1000,0,3) && !kh_route_cond_eval(g,KR_HITS,3,1000,0,3));
        kh_phase_test_set_damage(0,0); RCHECK(!kh_route_cond_eval(g,KR_BATTLE,-1,10,0,0));
        RCHECK(kh_route_cond_eval(g,KR_FRAMES,10,1000,990,0) && !kh_route_cond_eval(g,KR_FRAMES,11,1000,990,0));
        { char sum[700]; kh_route_state_summary(g,1000,sum,sizeof sum); RCHECK(strstr(sum,"game-level=3") && strstr(sum,"world=12") && strstr(sum,"battle=0")); }
#undef EV
        for(size_t i=0;i<sizeof G/sizeof *G;i++) wr64(g,GS_BASE+G[i],saved[i]);
        ga_free(lay); ga_free(menu); ga_free(ctx); ga_free(ngl); ga_free(list); ga_free(win);
        kh_phase_test_set_alive(sa_i,sa_t,sa_n); kh_phase_test_set_initial_state(sa_s); kh_phase_test_set_damage(sa_h,sa_l); kh_route_state_test_reset();
    }
    ga_free(out);
    if(sv_env) setenv("KHNMC_ROUTE_DIFFICULTY",sv_envv,1);
    kh_input_load(g,getenv("KHNMC_INPUT_SCRIPT"));
    fprintf(stderr,"kh: input route selftest: %d checks, %d failures\n",checks,fails);
#undef RCHECK
    return fails;
}
