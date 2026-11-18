/* warp.c : the DEBUG WARP of the scripted controller (task 33).  A TEST TOOL, not play.
 *
 * `@warp WORLD ROOM [door N] [map N] [btl N] [evt N] [settle S] [timeout T]` (and `@warp list FILE [settle S] [timeout T]`, the
 * sweep's list mode; runtime/null_input.c parses the route line and runs the step) asks the GAME for a map jump through its own
 * function, the way the mod's script call MapJump (runtime/mod_syscalls.c, syscall 8) and the camp menu's place jump (0x151069..)
 * do: a 10-byte place record {world, room, door, 0, map program, btl program, evt program} handed to exe 0x152990(record, 1, 0, 0, 0)
 * (fade type 1, no delay, no flag, no fifth argument: the arguments of the menu's explicit-record jump at 0x15112c).  0xffff in a
 * program word keeps the save's program of that room (0x152be0).  The call is made on the game thread at the end of a game frame
 * (the 0x105e10 hook), only at a safe moment: the field is up (FIELD_COMMAND.execute ran this frame, a world is set), no system menu
 * runs, no message window waits and no jump is pending ([0x1409ba928] = the jump task 0x152a90, [0x1409ba8d1]).  Nothing else is
 * written to the game: the record lives in a runtime block of guest memory and 0x152990 copies it at once (to 0x140717018, or to
 * 0x140717120 when a jump is already running).
 *
 * After the request the warp watches, without touching anything:
 *   jump done   the jump task has run (0x152a90 cleared [0x1409ba928] after 0x152be0 wrote the place 0x140717008..0x140717011)
 *               and the place is the requested world and room;
 *   field       the field is up again in that place and stayed up for `settle` game frames in a row (default 60).
 * Everything the game opens meanwhile is attributed to the warp (runtime/file_layer.c calls kh_warp_file_event): the map files
 * (`map/...`), the mission file (`msn/...`: a mission started), event data (`anm/`, `event/`: a scene ran) and every file MISS that
 * is not one of the numbered probes every run makes (NNcommon.bdx, NNeffect.bar), with the return sites found on the loading
 * thread's guest stack (the caller chain, innermost first).  Every line this prints starts with `kh: warp:`; a fault or a named
 * fatal while a warp is in progress also prints which warp it was (fault diagnostics callback).
 *
 * A RESULT AFTER A WARP SAYS "THIS ROOM LOADS FROM THIS STATE", NOT "THE GAME REACHES IT": the story may never jump there with these
 * programs, and the state the room expects (flags, party, items, an earlier room) is whatever the run had when it warped.
 */
#include "khnmc_rt.h"
#include "route_state.h"
#include "warp.h"
#include <errno.h>
#include <limits.h>
#include <strings.h>
#include <ctype.h>

/* the world names of the exe's table 0x140717210 (pointers to "zz" .. "eh"; 0x153200 / 0x152ed0 use it); the selftest compares */
const char *const kh_warp_world_names[KH_WARP_WORLDS]={"zz","es","tt","di","hb","bb","he","al","mu","po","lk","lm","dc","wi","nm","wm","ca","tr","eh"};
#define JUMP_TASK   (GS_BASE+0x9ba928ull)   /* the jump task 0x152a90 (0x152990 creates it, 0x152a90 clears it after the jump) */
#define JUMP_BUSY   (GS_BASE+0x9ba8d1ull)   /* 0x152990: a jump is already running -> the record goes to 0x140717120 */
#define PLACE       (GS_BASE+0x717008ull)   /* world, room, door, -, map, btl, evt program (0x152be0) */
#define JUMP_FN     0x152990u
void kh_register_fault_diag(void (*fn)(FILE*));

static int num(const char *s,long lo,long hi,long *v){
    char *e; errno=0; long x=strtol(s,&e,0); if(errno||e==s||*e||x<lo||x>hi) return 0; *v=x; return 1;
}
int kh_warp_world_lookup(const char *s){
    long v; if(num(s,0,KH_WARP_WORLDS-1,&v)) return (int)v;
    for(int i=0;i<KH_WARP_WORLDS;i++) if(!strcasecmp(s,kh_warp_world_names[i])) return i;
    return -1;
}
/* WORLD ROOM [door N] [map N] [btl N] [evt N] [settle S] [timeout T] from words[*i]; options=0 refuses settle / timeout (list lines) */
int kh_warp_parse(char **w,int n,int *i,KhWarpTarget *t,int options,u64 *settle,u64 *timeout,const char **why){
    memset(t,0,sizeof *t); t->door=0; t->prog[0]=t->prog[1]=t->prog[2]=0xffff;
    long v; int seen=0;
    if(*i+1>=n){ *why="@warp WORLD ROOM [door N] [map N] [btl N] [evt N]: a world and a room are needed"; return 0; }
    t->world=kh_warp_world_lookup(w[*i]);
    if(t->world<0){ *why="@warp: unknown world (a name zz es tt di hb bb he al mu po lk lm dc wi nm wm ca tr eh, or 0..18)"; return 0; }
    if(!num(w[*i+1],0,KH_WARP_ROOMS-1,&v)){ *why="@warp: the room is a number 0..63 (the save keeps 64 rooms per world)"; return 0; }
    t->room=(int)v; *i+=2;
    while(*i<n){
        static const char *opt[]={"door","map","btl","evt","settle","timeout"};
        int k; for(k=0;k<6;k++) if(!strcmp(w[*i],opt[k])) break;
        if(k==6){ *why=options?"unknown option for @warp (door, map, btl, evt, settle, timeout)":"unknown option in a warp list line (door, map, btl, evt)"; return 0; }
        if(k>=4 && !options){ *why="settle and timeout belong on the `@warp list` line, not on a list line"; return 0; }
        if(seen&(1<<k)){ *why="@warp: an option is given twice"; return 0; }
        seen|=1<<k;
        if(*i+1>=n){ *why="@warp: door, map, btl, evt, settle and timeout take a number"; return 0; }
        long lo=0,hi=k==0?255:k<4?0xfffe:100000000; if(k>=4) lo=1;
        if(!num(w[*i+1],lo,hi,&v)){ *why=k==0?"@warp: door is 0..255":k<4?"@warp: a program is 0..0xfffe (0xffff = the save's program: leave the option out)":"@warp: settle and timeout are 1..100000000 game frames"; return 0; }
        if(k==0) t->door=(int)v; else if(k<4) t->prog[k-1]=(int)v; else if(k==4){ if(settle) *settle=(u64)v; } else { if(timeout) *timeout=(u64)v; }
        *i+=2;
    }
    return 1;
}
int kh_warp_list_load(const char *path,KhWarpTarget **out,size_t *count,char *why,size_t cap){
    *out=NULL; *count=0;
    FILE *f=fopen(path,"r"); if(!f){ snprintf(why,cap,"cannot open the warp list %s",path); return 0; }
    char line[512]; unsigned ln=0; KhWarpTarget *v=NULL; size_t nv=0;
    while(fgets(line,sizeof line,f)){
        ln++;
        if(strlen(line)==sizeof line-1 && line[sizeof line-2]!='\n'){ snprintf(why,cap,"%s line %u: line too long",path,ln); goto bad; }
        char *c=strchr(line,'#'); if(c) *c=0;
        char text[160]; snprintf(text,sizeof text,"%s",line); { char *e=text+strlen(text); while(e>text && isspace((unsigned char)e[-1])) *--e=0; }
        char *t0=text; while(isspace((unsigned char)*t0)) t0++;
        char *wd[16],*sv; int n=0;
        for(char *x=strtok_r(line," \t\r\n",&sv);x;x=strtok_r(NULL," \t\r\n",&sv)){ if(n==16){ snprintf(why,cap,"%s line %u: too many words",path,ln); goto bad; } wd[n++]=x; }
        if(!n) continue;
        KhWarpTarget t; const char *w=""; int i=0;
        if(!kh_warp_parse(wd,n,&i,&t,0,NULL,NULL,&w)){ snprintf(why,cap,"%s line %u: %s",path,ln,w); goto bad; }
        t.line=ln; snprintf(t.text,sizeof t.text,"%s",t0);
        if(nv==KH_WARP_LIST_MAX){ snprintf(why,cap,"%s: more than %d entries",path,KH_WARP_LIST_MAX); goto bad; }
        KhWarpTarget *nx=realloc(v,(nv+1)*sizeof *v); if(!nx){ snprintf(why,cap,"%s: allocation failed",path); goto bad; }
        v=nx; v[nv++]=t;
    }
    if(ferror(f)){ snprintf(why,cap,"%s: read error",path); goto bad; }
    fclose(f);
    if(!nv){ free(v); snprintf(why,cap,"%s: the warp list has no entries",path); return 0; }
    *out=v; *count=nv; return 1;
bad: fclose(f); free(v); return 0;
}
void kh_warp_record(const KhWarpTarget *t,u8 rec[10]){
    rec[0]=(u8)t->world; rec[1]=(u8)t->room; rec[2]=(u8)t->door; rec[3]=0;
    for(int k=0;k<3;k++){ rec[4+2*k]=(u8)(t->prog[k]&0xff); rec[5+2*k]=(u8)(t->prog[k]>>8); }
}
void kh_warp_describe(const KhWarpTarget *t,char *out,size_t cap){
    char p[3][12]; for(int k=0;k<3;k++){ if(t->prog[k]==0xffff) snprintf(p[k],sizeof p[k],"save"); else snprintf(p[k],sizeof p[k],"%#x",t->prog[k]); }
    snprintf(out,cap,"%s%02d (world %d room %d) door %d programs map %s btl %s evt %s",kh_warp_world_names[t->world],t->room,t->world,t->room,t->door,p[0],p[1],p[2]);
}

/* ---- the game's side: real readers and the call (the selftest replaces them) ------------------------------------------------- */
static int real_cond(KCtx *g,int kind,u64 n){ KhRouteBase b={n,0,0}; return kh_route_cond_eval(g,kind,-1,n,&b); }
static int real_jump_busy(KCtx *g){ return rd64(g,JUMP_TASK)!=0 || rd8(g,JUMP_BUSY)!=0; }
static int real_place(KCtx *g,int *w,int *r){ u8 a=rd8(g,PLACE),b=rd8(g,PLACE+1); if(a==0xff) return 0; *w=a; *r=b; return 1; }
static u64 rec_block;
/* the call into the game; the selftest swaps it for a recorder to check the target and the arguments without running a jump */
static u64 (*warp_call5)(KCtx *g,u64 target,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4)=kh_call5;
static void real_request(KCtx *g,const u8 rec[10]){
    if(!rec_block){ rec_block=ga_calloc(1,16); if(!rec_block) kh_fatal(g,"warp: cannot allocate the place record"); }
    memcpy(hp(g,rec_block),rec,10);
    warp_call5(g,GS_BASE+JUMP_FN,rec_block,1,0,0,0);
}
static void real_parts(KCtx *g,u64 *fc,int *t,int *wt){ kh_route_field_parts(g,fc,t,wt); }
static KhWarpProbe probe={real_cond,real_jump_busy,real_place,real_request,real_parts};
void kh_warp_set_probe(const KhWarpProbe *p){ if(p) probe=*p; else probe=(KhWarpProbe){real_cond,real_jump_busy,real_place,real_request,real_parts}; }

/* ---- what the game does while a warp is watched ------------------------------------------------------------------------------ */
static pthread_mutex_t wlock=PTHREAD_MUTEX_INITIALIZER;
typedef struct { char name[72]; char src[12]; int count; } Named;
typedef struct {
    int active, phase;                  /* phase 1 waiting for a safe moment, 2 requested (jump not done), 3 jump done (waiting for the field), 4 done */
    int index; KhWarpTarget t; u64 started, requested, jumped, field_first, field_run, deadline_from;
    int prev_w, prev_r; int busy_seen; u64 safe_run;
    Named map[12], msn[4]; int nmaps, nmsn;   /* distinct names (a file is asked for several times: prefetch, size, load) */
    int scene_files; char scene_first[120];
    int misses, probes; char miss_lines[8][400];
    u64 files;
    char own_map[24]; int own_map_miss;     /* map/<world><room>.map of the target (0x152ed0 + "map/%s.map"): asked for and MISSed */
    int moved, cur_w, cur_r; char moved_to[24];   /* the room's own program jumped on: the place left the target after the jump */
} WarpWatch;
static WarpWatch W;
static u64 warps_requested, warps_field, warps_jumped, warps_failed;
u64 kh_warp_count(void){ return warps_requested; }
void kh_warp_reset_counts(void){ warps_requested=warps_field=warps_jumped=warps_failed=0; }      /* a route (re)loaded: its own warps */
void kh_warp_counts(u64 *req,u64 *field,u64 *jumped,u64 *failed){ if(req) *req=warps_requested; if(field) *field=warps_field; if(jumped) *jumped=warps_jumped; if(failed) *failed=warps_failed; }
static const char *strip_root(const char *p){ const char *k=strstr(p,"KH2ReSource/"); if(!k) k=strstr(p,"KH2ReSource\\"); return k?k+12:p; }
/* the numbered probes every run makes (`NNcommon.bdx`, `NNeffect.bar` in the root: misses on every boot, not game data) */
int kh_warp_is_probe(const char *rel){
    if(strchr(rel,'/')||strchr(rel,'\\')) return 0;
    return isdigit((unsigned char)rel[0]) && isdigit((unsigned char)rel[1]) && (!strcmp(rel+2,"common.bdx") || !strcmp(rel+2,"effect.bar"));
}
/* return sites on the guest stack of the thread that asked for the file (as kh_guest_bt finds them: a value in .text preceded by a call) */
static void callers(KCtx *g,char *out,size_t cap){
    size_t o=0; out[0]=0; if(!g) return; u64 rsp=KH_RSP(g); if(!GA_OK(rsp)) return; int k=0;
    for(u64 p=rsp;p<rsp+0x2000 && GA_OK(p+8) && k<6;p+=8){ u64 v=rd64(g,p); if(v<GS_BASE+0x1006||v>=GS_BASE+0x5784f3) continue;
        const u8 *c=(const u8*)XL(g,v);
        int ok=(c[-5]==0xE8)||(c[-6]==0xFF&&c[-5]==0x15)||(c[-2]==0xFF&&(c[-1]&0xF8)==0xD0)||(c[-3]==0xFF&&(c[-2]&0xF8)==0x50)||(c[-2]==0xFF&&(c[-1]&0xF8)==0x10);
        if(ok && o<cap){ o+=(size_t)snprintf(out+o,cap-o,"%s%llx",k?" <- ":"",(unsigned long long)(v-GS_BASE)); k++; } }
}
static void note(void *arr,int *n,int cap,const char *rel,const char *src){
    Named *a=arr; for(int i=0;i<*n;i++) if(!strcmp(a[i].name,rel) && !strcmp(a[i].src,src)){ a[i].count++; return; }
    if(*n<cap){ snprintf(a[*n].name,sizeof a[*n].name,"%s",rel); snprintf(a[*n].src,sizeof a[*n].src,"%s",src); a[*n].count=1; (*n)++; }
}
static void names(const void *arr,int n,char *out,size_t cap){
    const Named *a=arr; size_t o=0; out[0]=0; if(!n){ snprintf(out,cap,"none"); return; }
    for(int i=0;i<n && o<cap;i++) o+=(size_t)snprintf(out+o,cap-o,"%s%s(%s x%d)",i?" ":"",a[i].name,a[i].src,a[i].count);
}
void kh_warp_file_event(KCtx *g,const char *path,const char *source){
    if(!W.active) return;
    const char *rel=strip_root(path); int hit=source!=NULL;
    pthread_mutex_lock(&wlock);
    if(W.active && W.phase>=2){
        W.files++;
        if(!strncasecmp(rel,"map/",4)) note(W.map,&W.nmaps,12,rel,hit?source:"MISS");
        if(!hit && !strcasecmp(rel,W.own_map)) W.own_map_miss++;
        if(!strncasecmp(rel,"msn/",4)) note(W.msn,&W.nmsn,4,rel,hit?source:"MISS");
        if(!strncasecmp(rel,"anm/",4)||!strncasecmp(rel,"event/",6)){ if(!W.scene_files++) snprintf(W.scene_first,sizeof W.scene_first,"%s",rel); }
        if(!hit){
            if(kh_warp_is_probe(rel)) W.probes++;
            else { if(W.misses<8){ char c[200]; callers(g,c,sizeof c); snprintf(W.miss_lines[W.misses],sizeof W.miss_lines[0],"%s (callers %s)",rel,c[0]?c:"none found"); } W.misses++; }
        }
    }
    pthread_mutex_unlock(&wlock);
}
static void fault_diag(FILE *f){
    if(!W.active) return; char d[160]; kh_warp_describe(&W.t,d,sizeof d);
    char mp[700]; names(W.map,W.nmaps,mp,sizeof mp);
    fprintf(f,"kh: warp: FAULT OR FATAL DURING DEBUG WARP #%d to %s (list line %u): phase %d (%s), requested at game frame %llu, jump done at %llu; "
              "map files so far: %s; data misses so far: %d\n",W.index,d,W.t.line,W.phase,W.phase==1?"waiting for a safe moment":W.phase==2?"jump requested, not done":W.phase==3?"jump done, waiting for the field":"done",
            (unsigned long long)W.requested,(unsigned long long)W.jumped,mp,W.misses);
    for(int i=0;i<W.misses && i<8;i++) fprintf(f,"kh: warp: #%d miss %s\n",W.index,W.miss_lines[i]);
}
/* Start watching warp `index` toward t. */
void kh_warp_begin(KCtx *g,int index,const KhWarpTarget *t,u64 n){
    static int diag; if(!diag){ diag=1; kh_register_fault_diag(fault_diag); }
    pthread_mutex_lock(&wlock); memset(&W,0,sizeof W); W.active=1; W.phase=1; W.index=index; W.t=*t; W.started=n; W.prev_w=W.prev_r=-1; W.cur_w=W.cur_r=-1;
    snprintf(W.own_map,sizeof W.own_map,"map/%s%02d.map",kh_warp_world_names[t->world],t->room);   /* the name the exe builds for the room (0x161c50) */
    pthread_mutex_unlock(&wlock);
    (void)g;
}
static void result_line(KCtx *g,u64 n,const char *outcome,const char *detail){
    char d[160]; kh_warp_describe(&W.t,d,sizeof d); int door=-1,mp_=-1,bp=-1,ep=-1; int pw=-1,pr=-1; probe.place(g,&pw,&pr);
    if(probe.place==real_place) kh_route_programs(g,&door,&mp_,&bp,&ep);
    float x=0,y=0,z=0,h=0; int pp=probe.place==real_place && kh_route_player_pos(g,&x,&y,&z,&h);
    char pos[80]; if(pp) snprintf(pos,sizeof pos,"(%.0f, %.0f, %.0f)",x,y,z); else snprintf(pos,sizeof pos,"none");
    char mp[700], ms[300]; names(W.map,W.nmaps,mp,sizeof mp); names(W.msn,W.nmsn,ms,sizeof ms);
    /* the parts of `field` at the end (what tells LOADING, MESSAGE-WAITING and JUMP-PENDING apart) */
    u64 fc=0; int nw=-1,nwt=-1; long long fcs=-1; if(probe.parts){ probe.parts(g,&fc,&nw,&nwt); fcs=(long long)fc; }
    fprintf(stderr,"kh: warp: #%d result %s%02d %s: %s; requested at game frame %llu, jump done at %llu, field at %llu (%llu game frames after the request); "
                   "place now %02x:%02x door %d programs map %#x btl %#x evt %#x; player at %s; "
                   "at the end: field commands this frame %lld, message windows %d (waiting %d), menu-open %d, jump pending %d; mission file %s; scene files %d%s%s; map files: %s; "
                   "data misses %d (probes %d); files opened %llu; target %s (list line %u)\n",
            W.index,kh_warp_world_names[W.t.world],W.t.room,outcome,detail,(unsigned long long)W.requested,(unsigned long long)W.jumped,(unsigned long long)W.field_first,
            (unsigned long long)(W.field_first&&W.requested?W.field_first-W.requested:0),pw&0xff,pr&0xff,door,mp_,bp,ep,pos,
            fcs,nw,nwt,probe.cond(g,KR_MENU_OPEN,n),probe.jump_busy(g),ms,W.scene_files,W.scene_files?" first ":"",W.scene_files?W.scene_first:"",
            mp,W.misses,W.probes,(unsigned long long)W.files,d,W.t.line);
    for(int i=0;i<W.misses && i<8;i++) fprintf(stderr,"kh: warp: #%d miss %s\n",W.index,W.miss_lines[i]);
    if(W.misses>8) fprintf(stderr,"kh: warp: #%d miss ... %d more\n",W.index,W.misses-8);
    if(probe.place==real_place){ char what[64]; snprintf(what,sizeof what,"warp #%d result",W.index); kh_route_map_report(g,stderr,what); }
}
/* One game frame of the watched warp.  0 = still going, 1 = the field is up in the requested room (result printed), -1 = it did not
 * come within `timeout` game frames of the request (or no safe moment came within `timeout`): result printed, `what` says why. */
int kh_warp_tick(KCtx *g,u64 n,u64 settle,u64 timeout,char *what,size_t cap){
    if(!W.active) return 0;
    if(W.phase==1){
        int safe=probe.cond(g,KR_FIELD,n) && !probe.cond(g,KR_MENU_OPEN,n) && !probe.cond(g,KR_MESSAGE,n) && !probe.jump_busy(g);
        W.safe_run=safe?W.safe_run+1:0;
        /* the field must have been up for `settle` game frames in a row: a jump requested in the first field frame of a map, while that
         * map's own set-up still runs, faulted in the jump task's unload (0x152a90 -> 0x39c800 -> 0x3ec3e0 -> 0x39da10, run-S33/try1) */
        if(W.safe_run<settle){
            if(n-W.started>=timeout){
                snprintf(what,cap,"NO-SAFE-MOMENT: no safe moment for the debug warp came within %llu game frames (field %d, menu-open %d, message %d, jump pending %d)",(unsigned long long)timeout,
                         probe.cond(g,KR_FIELD,n),probe.cond(g,KR_MENU_OPEN,n),probe.cond(g,KR_MESSAGE,n),probe.jump_busy(g));
                pthread_mutex_lock(&wlock); W.phase=4; pthread_mutex_unlock(&wlock); result_line(g,n,"NO-SAFE-MOMENT",what+16); W.active=0; warps_failed++; return -1; }
            return 0;
        }
        u8 rec[10]; kh_warp_record(&W.t,rec); char d[160]; kh_warp_describe(&W.t,d,sizeof d);
        probe.place(g,&W.prev_w,&W.prev_r);
        fprintf(stderr,"kh: warp: #%d DEBUG WARP (a test tool, not play) to %s: exe 0x152990({%02x %02x %02x 00 %02x%02x %02x%02x %02x%02x}, 1, 0, 0, 0) at game frame %llu, from %02x:%02x\n",
                W.index,d,rec[0],rec[1],rec[2],rec[5],rec[4],rec[7],rec[6],rec[9],rec[8],(unsigned long long)n,W.prev_w&0xff,W.prev_r&0xff);
        pthread_mutex_lock(&wlock); W.phase=2; W.requested=n; pthread_mutex_unlock(&wlock);
        warps_requested++;
        probe.request(g,rec);
        W.busy_seen=probe.jump_busy(g);
        return 0;
    }
    if(W.phase==2){
        int busy=probe.jump_busy(g); if(busy) W.busy_seen=1;
        int w=-1,r=-1; int placed=probe.place(g,&w,&r);
        int here=placed && w==W.t.world && r==W.t.room, elsewhere=placed && !here && (w!=W.prev_w || r!=W.prev_r);
        /* done: the jump task has come and gone and the place is the target, or already another new room (its program jumped on at once) */
        if(W.busy_seen && !busy && (here||elsewhere)){ pthread_mutex_lock(&wlock); W.phase=3; W.jumped=n; pthread_mutex_unlock(&wlock); }
        else if(!W.busy_seen && !busy && n>W.requested+2){       /* 0x152990 always creates the jump task or queues: never seeing it is a defect */
            snprintf(what,cap,"JUMP-NOT-STARTED: the jump task never appeared after the request (0x152990 did not create it)");
            W.phase=4; result_line(g,n,"JUMP-NOT-STARTED",what+18); W.active=0; warps_failed++; return -1; }
    }
    if(W.phase==3){
        /* the field in the target, or (when the room's own program jumped on: an event, a door or a jump of its programs) in the room it
         * went on to; the place must not change and no jump may be pending while the field stays up `settle` frames */
        int w=-1,r=-1; int placed=probe.place(g,&w,&r);
        int here=placed && w==W.t.world && r==W.t.room;
        if(placed && (w!=W.cur_w || r!=W.cur_r)){
            W.cur_w=w; W.cur_r=r; W.field_run=0;
            if(!here){ W.moved++; snprintf(W.moved_to,sizeof W.moved_to,"%s%02d",w<KH_WARP_WORLDS?kh_warp_world_names[w]:"??",r&0xff); }
        }
        if(placed && (here||W.moved) && !probe.jump_busy(g) && probe.cond(g,KR_FIELD,n)){ if(!W.field_run++) W.field_first=n; }
        else W.field_run=0;
        if(W.field_run>=settle){
            char det[300];
            if(here){
                snprintf(det,sizeof det,"the field is up and stayed up for %llu game frames%s%s%s%s",(unsigned long long)settle,W.nmsn?"; a mission started":"",W.scene_files?"; event data was read (a scene ran)":"",
                         W.moved?"; after jumping on to ":"",W.moved?W.moved_to:"");
                W.phase=4; result_line(g,n,W.nmsn?"FIELD+MISSION":"FIELD",det); W.active=0; warps_field++; return 1;
            }
            snprintf(det,sizeof det,"the room loaded and its own program jumped on to %s (%02x:%02x); the field is up there and stayed up for %llu game frames%s%s",
                     W.moved_to,w&0xff,r&0xff,(unsigned long long)settle,W.nmsn?"; a mission started":"",W.scene_files?"; event data was read (a scene ran)":"");
            W.phase=4; result_line(g,n,"JUMPED-ON",det); W.active=0; warps_jumped++; return 1;
        }
    }
    if(n-W.requested>=timeout){
        const char *o; char det[400];
        if(W.phase==2){ o="JUMP-NOT-DONE"; snprintf(det,sizeof det,"the jump was not carried out within %llu game frames (jump task pending %d)",(unsigned long long)timeout,probe.jump_busy(g)); }
        else if(W.moved){ o="JUMPED-ON-NO-FIELD"; snprintf(det,sizeof det,"the room's own program jumped on to %s, and no field came there within %llu game frames of the request",W.moved_to,(unsigned long long)timeout); }
        else if(W.own_map_miss){ o="MAP-MISSING"; snprintf(det,sizeof det,"the place is set, the exe asked for the room's map file %s (named after the room by 0x152ed0) and neither the loose tree nor "
                                                          "the packages have it (MISS x%d); no field within %llu game frames",W.own_map,W.own_map_miss,(unsigned long long)timeout); }
        else if(W.scene_files){ o="SCENE"; snprintf(det,sizeof det,"the place is set but no field within %llu game frames; event data was read (a scene ran or runs: %d files, first %s)",(unsigned long long)timeout,W.scene_files,W.scene_first); }
        /* what keeps `field` false at the end (the parts are on the result line): a jump the room's own program asked for and the game
         * has not carried out, a message window waiting for a button (the warp presses nothing), or neither: still loading / no control */
        else if(probe.jump_busy(g)){ o="JUMP-PENDING"; snprintf(det,sizeof det,"the place is set but a jump is pending at the time-out (the jump task exists or a jump runs: a jump the room's own "
                                                                    "program asked for was not carried out); no field within %llu game frames",(unsigned long long)timeout); }
        else if(probe.cond(g,KR_MESSAGE,n)){ o="MESSAGE-WAITING"; snprintf(det,sizeof det,"the place is set but a field message window waits for a button at the time-out (the warp presses "
                                                                    "nothing); no field within %llu game frames",(unsigned long long)timeout); }
        else if(probe.cond(g,KR_MENU_OPEN,n)){ o="MENU-OPEN"; snprintf(det,sizeof det,"the place is set but a system menu runs at the time-out (MENU::isRunning; the warp presses nothing); "
                                                                    "no field within %llu game frames",(unsigned long long)timeout); }
        else { o="LOADING"; snprintf(det,sizeof det,"the place is set but the field did not stay up within %llu game frames, with no jump pending, no message window waiting and no "
                                                       "system menu (still loading, or no player control)",(unsigned long long)timeout); }
        snprintf(what,cap,"%s: %s",o,det);
        W.phase=4; result_line(g,n,o,det); W.active=0; warps_failed++; return -1;
    }
    return 0;
}
int kh_warp_active(void){ return W.active; }

/* ---- selftest (task 33): parsing, the list file, the record, the probe filter and the watch on a fake game ---------------------- */
static int f_field, f_menu, f_msg, f_busy, f_place_w=-1, f_place_r, f_requests, f_nw, f_nwt; static u64 f_fc; static u8 f_rec[10];
static int fake_cond(KCtx *g,int kind,u64 n){ (void)g; (void)n; return kind==KR_FIELD?f_field:kind==KR_MENU_OPEN?f_menu:kind==KR_MESSAGE?f_msg:0; }
static void fake_parts(KCtx *g,u64 *fc,int *t,int *wt){ (void)g; *fc=f_fc; *t=f_nw; *wt=f_nwt; }
/* the call into the game, recorded instead of made (the real-probe test) */
static u64 c_target, c_arg[5]; static int c_calls;
static u64 rec_call5(KCtx *g,u64 target,u64 a0,u64 a1,u64 a2,u64 a3,u64 a4){ (void)g; c_target=target; c_arg[0]=a0; c_arg[1]=a1; c_arg[2]=a2; c_arg[3]=a3; c_arg[4]=a4; c_calls++; return 0; }
void kh_route_state_test_set_field_cmds(u64 per_frame);
/* the game-side half of the warp on the real image: the three addresses the probes read (written with known values, then restored),
 * the target and the arguments of the call (recorded, not made).  The addresses are spelled out here, not taken from the macros above,
 * so a wrong macro fails the test. */
static int real_probe_selftest(KCtx *g,int *checks){
    int fails=0;
#define RCHECK(c) do{ (*checks)++; if(!(c)){ fails++; fprintf(stderr,"warp selftest FAIL (real probes): %s (line %d)\n",#c,__LINE__); } }while(0)
    const u64 task=GS_BASE+0x9ba928ull, busy=GS_BASE+0x9ba8d1ull, place=GS_BASE+0x717008ull;
    u64 sv_task=rd64(g,task); u8 sv_busy=rd8(g,busy), sv_w=rd8(g,place), sv_r=rd8(g,place+1);
    int w=-1,r=-1;
    wr64(g,task,0); wr8(g,busy,0);                RCHECK(!real_jump_busy(g));                    /* no jump task, no jump running */
    wr64(g,task,GS_BASE+0x1000ull);               RCHECK(real_jump_busy(g));                     /* 0x152990 created the jump task */
    wr64(g,task,0); wr8(g,busy,1);                RCHECK(real_jump_busy(g));                     /* a jump runs (the record would be queued) */
    wr8(g,busy,0);                                RCHECK(!real_jump_busy(g));
    wr8(g,place,0x0b); wr8(g,place+1,0x02);       RCHECK(real_place(g,&w,&r) && w==0x0b && r==0x02);   /* lm02 */
    wr8(g,place,0x12); wr8(g,place+1,0x1d);       RCHECK(real_place(g,&w,&r) && w==0x12 && r==0x1d);   /* eh29 */
    w=r=-7; wr8(g,place,0xff);                    RCHECK(!real_place(g,&w,&r) && w==-7 && r==-7);      /* world 0xff: no place */
    wr64(g,task,sv_task); wr8(g,busy,sv_busy); wr8(g,place,sv_w); wr8(g,place+1,sv_r);
    RCHECK(rd64(g,task)==sv_task && rd8(g,busy)==sv_busy && rd8(g,place)==sv_w && rd8(g,place+1)==sv_r);   /* restored */
    /* the request: exe 0x152990(record, 1, 0, 0, 0), the record in guest memory as kh_warp_record built it */
    u8 rec[10]={0x0b,0x02,0x00,0x00,0x34,0x00,0x34,0x00,0xff,0xff}; c_calls=0;
    warp_call5=rec_call5; real_request(g,rec); warp_call5=kh_call5;
    RCHECK(c_calls==1 && c_target==GS_BASE+0x152990ull);
    RCHECK(c_arg[0]==rec_block && GA_OK(rec_block) && !memcmp(hp(g,rec_block),rec,10));
    RCHECK(c_arg[1]==1 && c_arg[2]==0 && c_arg[3]==0 && c_arg[4]==0);
    /* the parts of `field` come from the route-state readers */
    { u64 fc=0; int t=-1,wt=-1; kh_route_state_test_set_field_cmds(7); real_parts(g,&fc,&t,&wt); RCHECK(fc==7 && t>=0 && wt>=0 && wt<=t); kh_route_state_test_set_field_cmds(0); }
#undef RCHECK
    return fails;
}
static int fake_busy(KCtx *g){ (void)g; return f_busy; }
static int fake_place(KCtx *g,int *w,int *r){ (void)g; if(f_place_w<0) return 0; *w=f_place_w; *r=f_place_r; return 1; }
static void fake_request(KCtx *g,const u8 rec[10]){ (void)g; memcpy(f_rec,rec,10); f_requests++; f_busy=1; }
static void fake_request_nobusy(KCtx *g,const u8 rec[10]){ (void)g; memcpy(f_rec,rec,10); f_requests++; }
/* stderr of a block into a buffer (the result lines) */
static FILE *cap_f; static int cap_fd=-1;
static void cap_begin(void){ fflush(stderr); cap_f=kh_tmpfile(); if(!cap_f) return; cap_fd=dup(2); dup2(fileno(cap_f),2); }
static void cap_end(char *out,size_t cap){ out[0]=0; fflush(stderr); if(!cap_f) return; dup2(cap_fd,2); close(cap_fd); cap_fd=-1; rewind(cap_f); size_t n=fread(out,1,cap-1,cap_f); out[n]=0; fclose(cap_f); cap_f=NULL; }
static int split(char *s,char **w,int max){ int n=0; char *sv; for(char *x=strtok_r(s," ",&sv);x&&n<max;x=strtok_r(NULL," ",&sv)) w[n++]=x; return n; }
static int parse_str(const char *line,KhWarpTarget *t,int options,u64 *settle,u64 *timeout,const char **why){
    char b[256]; snprintf(b,sizeof b,"%s",line); char *w[20]; int n=split(b,w,20), i=0; *why=""; return kh_warp_parse(w,n,&i,t,options,settle,timeout,why); }
int kh_warp_selftest(KCtx *g){
    int checks=0,fails=0;
#define WCHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr,"warp selftest FAIL: %s (line %d)\n",#c,__LINE__); } }while(0)
    /* 1. the world names are the exe's table 0x140717210 */
    for(int w=0;w<KH_WARP_WORLDS;w++){ u64 p=rd64(g,GS_BASE+0x717210ull+8ull*(u64)w); WCHECK(GA_OK(p) && !strcmp(gstr(g,p),kh_warp_world_names[w])); }
    WCHECK(kh_warp_world_lookup("mu")==8 && kh_warp_world_lookup("MU")==8 && kh_warp_world_lookup("18")==18 && kh_warp_world_lookup("0x12")==18 &&
           kh_warp_world_lookup("19")==-1 && kh_warp_world_lookup("qq")==-1 && kh_warp_world_lookup("")==-1);
    /* 2. a warp line */
    KhWarpTarget t; const char *why; u64 se=7,to=9;
    WCHECK(parse_str("mu 13",&t,1,&se,&to,&why) && t.world==8 && t.room==13 && t.door==0 && t.prog[0]==0xffff && t.prog[1]==0xffff && t.prog[2]==0xffff && se==7 && to==9);
    WCHECK(parse_str("8 0x0d door 2 map 0x4f btl 79 evt 0x4f settle 30 timeout 500",&t,1,&se,&to,&why) && t.world==8 && t.room==13 && t.door==2 &&
           t.prog[0]==0x4f && t.prog[1]==0x4f && t.prog[2]==0x4f && se==30 && to==500);
    WCHECK(parse_str("lk 16 evt 0xfffe",&t,0,NULL,NULL,&why) && t.world==10 && t.room==16 && t.prog[0]==0xffff && t.prog[2]==0xfffe);
    static const struct { const char *l; int opt; const char *msg; } bad[]={
        {"qq 1",1,"unknown world"}, {"mu",1,"a world and a room are needed"}, {"mu 64",1,"the room is a number 0..63"}, {"mu -1",1,"the room is a number"},
        {"mu x",1,"the room is a number"}, {"mu 1 door 256",1,"door is 0..255"}, {"mu 1 map 0xffff",1,"a program is 0..0xfffe"}, {"mu 1 map 1 map 2",1,"given twice"},
        {"mu 1 speed 3",1,"unknown option for @warp"}, {"mu 1 door",1,"take a number"}, {"mu 1 settle 0",1,"settle and timeout are 1.."}, {"mu 1 settle 5",0,"belong on the `@warp list` line"},
        {"mu 1 frob 2",0,"unknown option in a warp list line"}, {"20 1",1,"unknown world"},
    };
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++){ int ok=parse_str(bad[i].l,&t,bad[i].opt,&se,&to,&why); WCHECK(!ok && strstr(why,bad[i].msg)); if(ok||!strstr(why,bad[i].msg)) fprintf(stderr,"  case `%s`: %s\n",bad[i].l,why); }
    /* 3. the record handed to 0x152990 */
    { u8 r[10]; parse_str("mu 13 door 2 map 0x4f btl 0x1301",&t,1,&se,&to,&why); kh_warp_record(&t,r);
      static const u8 want[10]={0x08,0x0d,0x02,0x00,0x4f,0x00,0x01,0x13,0xff,0xff}; WCHECK(!memcmp(r,want,10)); }
    /* 4. the list file */
    { char p[KH_TMP_MAX]; kh_tmp_path(p,sizeof p,"khnmc-warplist-XXXXXX"); int fd=mkstemp(p); WCHECK(fd>=0);
      if(fd>=0){ const char *txt="# sweep\n\nmu 12\n  mu 13 map 0x4f btl 0x4f evt 0x4f   # Zexion\nlm 3 door 1\n"; WCHECK(write(fd,txt,strlen(txt))==(ssize_t)strlen(txt)); close(fd);
        KhWarpTarget *v=NULL; size_t nv=0; char e[300];
        WCHECK(kh_warp_list_load(p,&v,&nv,e,sizeof e) && nv==3 && v[0].line==3 && v[1].line==4 && v[1].prog[1]==0x4f && v[2].world==11 && v[2].door==1 &&
               !strcmp(v[1].text,"mu 13 map 0x4f btl 0x4f evt 0x4f"));
        free(v);
        static const struct { const char *txt, *msg; } lbad[]={{"mu 1\nmu 99\n","line 2: @warp: the room is a number"},{"# only a comment\n\n","no entries"},{"mu 1 settle 3\n","line 1: settle and timeout belong"}};
        for(size_t i=0;i<sizeof lbad/sizeof *lbad;i++){ FILE *f=fopen(p,"w"); if(f){ fputs(lbad[i].txt,f); fclose(f); } v=NULL; nv=1;
            int ok=kh_warp_list_load(p,&v,&nv,e,sizeof e); WCHECK(!ok && !v && nv==0 && strstr(e,lbad[i].msg)); if(ok||!strstr(e,lbad[i].msg)) fprintf(stderr,"  list case %zu: %s\n",i,e); }
        unlink(p);
        WCHECK(!kh_warp_list_load(p,&v,&nv,e,sizeof e) && strstr(e,"cannot open the warp list")); } }
    /* 5. the probes every run makes are not game data */
    WCHECK(kh_warp_is_probe("05common.bdx") && kh_warp_is_probe("20effect.bar") && !kh_warp_is_probe("map/05common.bdx") && !kh_warp_is_probe("5common.bdx") &&
           !kh_warp_is_probe("00effect.bar.x") && !kh_warp_is_probe("ab.bar") && !kh_warp_is_probe("map/mu13.map"));
    /* 6. the game-side half: the addresses, the call target and its arguments */
    fails+=real_probe_selftest(g,&checks);
    /* 7. the watch on a fake game */
    KhWarpProbe fp={fake_cond,fake_busy,fake_place,fake_request,fake_parts}; kh_warp_set_probe(&fp);
    f_fc=0; f_nw=0; f_nwt=0;
    u64 r0=0,fd0=0,jo0=0,fl0=0; kh_warp_counts(&r0,&fd0,&jo0,&fl0);
    char what[600], out[16384]; int rc;
    parse_str("mu 13 map 0x4f btl 0x4f evt 0x4f",&t,1,&se,&to,&why); t.line=7;
    f_field=0; f_menu=0; f_msg=0; f_busy=0; f_place_w=0x12; f_place_r=2; f_requests=0;
    cap_begin(); kh_warp_begin(g,1,&t,100); rc=0;
    for(u64 n=100;n<130;n++) rc|=kh_warp_tick(g,n,10,1000,what,sizeof what);                       /* no field: nothing asked */
    WCHECK(rc==0 && f_requests==0);
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/eh02.map",NULL);                        /* before the request: not this warp's */
    f_field=1; f_menu=1; for(u64 n=130;n<160;n++) kh_warp_tick(g,n,10,1000,what,sizeof what); WCHECK(f_requests==0);   /* a menu */
    f_menu=0; f_msg=1; for(u64 n=160;n<190;n++) kh_warp_tick(g,n,10,1000,what,sizeof what); WCHECK(f_requests==0);    /* a message */
    f_msg=0; f_busy=1; for(u64 n=190;n<220;n++) kh_warp_tick(g,n,10,1000,what,sizeof what); WCHECK(f_requests==0);    /* a jump pending */
    f_busy=0; for(u64 n=220;n<229;n++) kh_warp_tick(g,n,10,1000,what,sizeof what); WCHECK(f_requests==0);            /* 9 safe frames of 10 */
    f_msg=1; kh_warp_tick(g,229,10,1000,what,sizeof what); f_msg=0; for(u64 n=230;n<239;n++) kh_warp_tick(g,n,10,1000,what,sizeof what);
    WCHECK(f_requests==0);                                                                               /* a message reset the run of safe frames */
    kh_warp_tick(g,239,10,1000,what,sizeof what);                                                        /* the 10th safe frame in a row */
    { static const u8 want[10]={0x08,0x0d,0x00,0x00,0x4f,0x00,0x4f,0x00,0x4f,0x00}; WCHECK(f_requests==1 && !memcmp(f_rec,want,10)); }
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/mu13.map",NULL);
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/mu13.map",NULL);
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/us/mu13.bar","LOOSE");
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/07common.bdx",NULL);                       /* a probe */
    kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/msn/us/MU13_FM_ZEX.bar","LOOSE");
    for(u64 n=240;n<260;n++) kh_warp_tick(g,n,10,1000,what,sizeof what); WCHECK(f_requests==1);       /* busy: the jump is not done */
    f_place_w=8; f_place_r=13; for(u64 n=260;n<270;n++) kh_warp_tick(g,n,10,1000,what,sizeof what);   /* placed, still busy */
    f_busy=0; f_field=0; rc=kh_warp_tick(g,270,10,1000,what,sizeof what); WCHECK(rc==0 && kh_warp_active());   /* the jump is done: waiting for the field */
    f_field=1; for(u64 n=271;n<276;n++) WCHECK(kh_warp_tick(g,n,10,1000,what,sizeof what)==0);
    f_field=0; kh_warp_tick(g,276,10,1000,what,sizeof what); f_field=1;                                  /* the field went away: the run starts again */
    f_busy=1; WCHECK(kh_warp_tick(g,277,10,1000,what,sizeof what)==0); f_busy=0;                            /* a jump pending: the field does not count */
    for(u64 n=278;n<287;n++) WCHECK(kh_warp_tick(g,n,10,1000,what,sizeof what)==0);
    rc=kh_warp_tick(g,287,10,1000,what,sizeof what); cap_end(out,sizeof out);
    WCHECK(rc==1 && !kh_warp_active() && f_requests==1);
    WCHECK(strstr(out,"kh: warp: #1 DEBUG WARP (a test tool, not play) to mu13 (world 8 room 13) door 0 programs map 0x4f btl 0x4f evt 0x4f: exe 0x152990({08 0d 00 00 004f 004f 004f}, 1, 0, 0, 0) at game frame 239, from 12:02"));
    WCHECK(strstr(out,"kh: warp: #1 result mu13 FIELD+MISSION:") && strstr(out,"requested at game frame 239, jump done at 270, field at 278 (39 game frames after the request)"));
    WCHECK(strstr(out,"mission file msn/us/MU13_FM_ZEX.bar(LOOSE x1)") && strstr(out,"map files: map/mu13.map(MISS x2) map/us/mu13.bar(LOOSE x1);") && strstr(out,"data misses 2 (probes 1)") && strstr(out,"(list line 7)"));
    WCHECK(strstr(out,"kh: warp: #1 miss map/mu13.map (callers ") && !strstr(out,"eh02"));
    /* timeouts: still loading, a scene, the jump not done, never started, no safe moment, the room's map file missing (a MISS of
     * map/mu13.map: the name the exe builds; a MISS of another room's map or of the localized bar is not it), jumped on without a field */
    static const struct { int busy_after, place, scene, safe, map; const char *outcome; } T[]={
        {0,1,0,1,0,"LOADING"},{0,1,1,1,0,"SCENE"},{1,1,0,1,0,"JUMP-NOT-DONE"},{-1,0,0,1,0,"JUMP-NOT-STARTED"},{0,0,0,0,0,"NO-SAFE-MOMENT"},
        {0,1,1,1,1,"MAP-MISSING"},{0,1,0,1,2,"LOADING"},{0,2,0,1,1,"JUMPED-ON-NO-FIELD"}};
    for(size_t i=0;i<sizeof T/sizeof *T;i++){
        f_field=T[i].safe; f_menu=f_msg=0; f_busy=0; f_place_w=0x12; f_place_r=2; f_requests=0;
        KhWarpProbe q=fp; if(T[i].busy_after<0) q.request=fake_request_nobusy;     /* the game never shows a jump task */
        kh_warp_set_probe(&q);
        cap_begin(); kh_warp_begin(g,2+(int)i,&t,1000); rc=0; u64 n=1000;
        for(;n<1000+5 && !rc;n++) rc=kh_warp_tick(g,n,3,50,what,sizeof what);
        if(T[i].place){ f_place_w=8; f_place_r=13; } if(!T[i].busy_after) f_busy=0; f_field=0;
        if(T[i].scene) kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/anm/us/mu/b_mu100/x.anb","LOOSE");
        if(T[i].map==1) kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/mu13.map",NULL);
        if(T[i].map==2){ kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/mu12.map",NULL); kh_warp_file_event(g,"C:/hd28/STEAM/juefigs/KH2ReSource/map/us/mu13.bar",NULL); }
        if(T[i].place==2){ rc=kh_warp_tick(g,n++,3,50,what,sizeof what); f_place_w=8; f_place_r=12; }   /* in mu13 one frame, then mu12 */
        for(;n<1200 && !rc;n++) rc=kh_warp_tick(g,n,3,50,what,sizeof what);
        cap_end(out,sizeof out);
        WCHECK(rc==-1 && !kh_warp_active() && strstr(out,T[i].outcome) && !strncmp(what,T[i].outcome,strlen(T[i].outcome)) && what[strlen(T[i].outcome)]==':');
        if(rc!=-1 || !strstr(out,T[i].outcome) || what[strlen(T[i].outcome)]!=':') fprintf(stderr,"  timeout case %s: rc %d what %s\n",T[i].outcome,rc,what);
        if(T[i].map==1 && T[i].place==1) WCHECK(strstr(what,"map/mu13.map") && strstr(what,"MISS x1"));
        if(T[i].place==2) WCHECK(strstr(what,"jumped on to mu12"));
    }
    /* the room's own program jumps on: done in the room it went to (the field there), counted apart; straight away (the place is
     * already the next room when the jump task is gone) and after a frame in the target */
    for(int k=0;k<2;k++){
        f_field=1; f_menu=f_msg=0; f_busy=0; f_place_w=0x12; f_place_r=2; f_requests=0; kh_warp_set_probe(&fp);
        cap_begin(); kh_warp_begin(g,20+k,&t,2000); rc=0; u64 n=2000;
        for(;n<2003 && !rc;n++) rc=kh_warp_tick(g,n,3,500,what,sizeof what);            /* 3 safe frames: requested, busy */
        WCHECK(f_requests==1 && rc==0);
        if(k){ f_place_w=8; f_place_r=13; f_busy=0; f_field=0; rc=kh_warp_tick(g,n++,3,500,what,sizeof what); }             /* a frame in mu13 first */
        f_place_w=5; f_place_r=1; f_busy=0; f_field=1;                                       /* bb01: the field there */
        for(;n<2100 && !rc;n++) rc=kh_warp_tick(g,n,3,500,what,sizeof what);
        cap_end(out,sizeof out);
        WCHECK(rc==1 && !kh_warp_active() && strstr(out,"kh: warp: #") && strstr(out," result mu13 JUMPED-ON: the room loaded and its own program jumped on to bb01 (05:01)"));
        if(rc!=1 || !strstr(out,"JUMPED-ON: the room loaded")) fprintf(stderr,"  jumped-on case %d: rc %d: %.600s\n",k,rc,out);
    }
    kh_warp_set_probe(&fp);
    u64 r1=0,fd1=0,jo1=0,fl1=0; kh_warp_counts(&r1,&fd1,&jo1,&fl1);
    WCHECK(r1-r0==10 && fd1-fd0==1 && jo1-jo0==2 && fl1-fl0==8);         /* NO-SAFE-MOMENT never asked the game */
    /* what keeps the field false at the time-out, after the jump was done and the field came for a frame (lm02 0x34 in the sweep):
     * nothing known (LOADING), a jump the room's program asked for (JUMP-PENDING, first), a message window (MESSAGE-WAITING); the
     * parts of `field` are on the result line */
    static const struct { int busy_end, msg_end, menu_end; const char *outcome, *parts; } E[]={
        {0,0,0,"LOADING","at the end: field commands this frame 0, message windows 0 (waiting 0), menu-open 0, jump pending 0; "},
        {1,0,0,"JUMP-PENDING","at the end: field commands this frame 2, message windows 0 (waiting 0), menu-open 0, jump pending 1; "},
        {0,1,0,"MESSAGE-WAITING","at the end: field commands this frame 2, message windows 1 (waiting 1), menu-open 0, jump pending 0; "},
        {1,1,0,"JUMP-PENDING","at the end: field commands this frame 2, message windows 1 (waiting 1), menu-open 0, jump pending 1; "},
        {0,0,1,"MENU-OPEN","at the end: field commands this frame 0, message windows 0 (waiting 0), menu-open 1, jump pending 0; "},
        {0,1,1,"MESSAGE-WAITING","at the end: field commands this frame 2, message windows 1 (waiting 1), menu-open 1, jump pending 0; "}};
    for(size_t i=0;i<sizeof E/sizeof *E;i++){
        f_field=1; f_menu=f_msg=0; f_busy=0; f_place_w=0x12; f_place_r=2; f_requests=0; f_fc=2; f_nw=f_nwt=0; kh_warp_set_probe(&fp);
        cap_begin(); kh_warp_begin(g,30+(int)i,&t,3000); rc=0; u64 n=3000;
        for(;n<3003 && !rc;n++) rc=kh_warp_tick(g,n,3,50,what,sizeof what);                          /* requested, the jump task up */
        f_place_w=8; f_place_r=13; f_busy=0; rc|=kh_warp_tick(g,n++,3,50,what,sizeof what);             /* jump done, a field frame */
        f_busy=E[i].busy_end; f_msg=E[i].msg_end; f_nw=f_nwt=E[i].msg_end; f_menu=E[i].menu_end;
        f_field=E[i].busy_end&&!E[i].msg_end; if(!E[i].busy_end && !E[i].msg_end) f_fc=0;              /* the parts as the game shows them */
        for(;n<3200 && !rc;n++) rc=kh_warp_tick(g,n,3,50,what,sizeof what);
        cap_end(out,sizeof out);
        int ok=rc==-1 && !strncmp(what,E[i].outcome,strlen(E[i].outcome)) && what[strlen(E[i].outcome)]==':' && strstr(out,E[i].parts) &&
               strstr(out," field at 3003 (") && strstr(out,"place now 08:0d");
        WCHECK(ok); if(!ok) fprintf(stderr,"  end case %zu %s: rc %d what %s: %.900s\n",i,E[i].outcome,rc,what,out);
    }
    kh_warp_set_probe(NULL);
    fprintf(stderr,"kh: warp selftest: %d checks, %d failures\n",checks,fails);
#undef WCHECK
    return fails;
}
