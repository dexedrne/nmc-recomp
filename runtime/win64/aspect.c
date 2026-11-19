#include "route_state.h"
/* aspect.c : task 37, the first steps of the "fill" mode (NATIVE-FILL-PLAN.md sections 3, 4.3, 6 and 7; plan steps 0 to 3).
 *
 * The owner plays on a 1.42:1 screen (iPhone Duo inner screen, 2670x1878).  The exe already has a Vert+ projection for screens
 * narrower than 16:9; what keeps the PC picture at 16:9 is the size the runtime reports (KHNMC_SCREEN, runtime/null_window.c, seam S1,
 * and the DXGI output, runtime/null_d3d12.c, seam S2) and exe behaviours the plan's hooks H1..H6 change.  This file has H1, the part
 * of H2b that H1's condition needs, the screen-shape numbers r, o, b of the plan's section 3, the size records and the selftest.
 *
 * Switches (read once at start from runtime/null_window.c kh_seam_window_init; unset or empty = the default):
 *   KHNMC_ASPECT=pc|fill         pc (default): every hook here is a pass-through and the game is what it was before task 37.
 *                                fill: H1 below.  At 16:9 or wider H1 changes nothing (the plan's "fill neutrality", step 3).
 *   KHNMC_ASPECT_TEST_H1=unconditional    TEST AID ONLY (plan section 8 step 1, the "first test"): H1 without its "3D view drawn"
 *                                condition, i.e. on every frame, menus and title included.  Needs KHNMC_ASPECT=fill.  Not for play.
 *   (KHNMC_SCREEN, runtime/null_window.c: with it set, kh_screen_check_present below checks at every present that the game built
 *                                that size: the named, counted WARNING screen-not-applied otherwise, whatever the switches here say)
 *   KHNMC_ASPECT_LOG=1           the size records (the first part of the plan's measurement log M): the swap chain the game creates,
 *                                every render-target / depth-stencil / multisampled texture by kind (the RT census), the GX size fields
 *                                at each present where they changed (the VRAM guard and the 16:9 fit), and the H1 counts.  Null
 *                                graphics backend (runtime/null_d3d12.c); the vkd3d bridges print their own census.
 * Any other value of these three is a named fatal.
 *   KHNMC_FOV_ZOOM=<number>      task 49 (section "the zoom of the tall screen" below): the zoom of the 3D picture of a screen TALLER
 *                                THAN WIDE in fill mode, 1.0 .. 1.89 (plain decimal), default 1.3; 1.0 = no zoom, nothing written.  The
 *                                game's camera stays the exe's (only what is drawn is zoomed).  A screen that is not taller than wide
 *                                always has zoom 1.  With KHNMC_ASPECT unset or pc the value is checked, reported
 *                                as not applied (NOTE fov-zoom-not-applied) and changes nothing.  Malformed, or outside the range: the
 *                                named fatals fov-zoom-value-malformed / fov-zoom-value-range.
 *
 * H1 (post-call hook on f_11c1e0, plan section 4.3).  f_11c1e0(out) is the exe's "largest 16:9 rectangle in the screen"
 * (shard_0022.c:26728): s = minss(1.0, GX+0x28); W' = GX+0x18 * s, H' = GX+0x1c * s (floats); "narrow" when 9W' < 16H' (comiss; an
 * unordered compare also counts as narrow); narrow: (W', 9W' * 0.0625), otherwise (16H' / 9, H'); then x *= GX+0x20, y *= GX+0x24;
 * it returns out in rax.  Its callers draw black bars of (GX+0x10 - result) / 2 around it: f_14e050 (shard_0026.c:36792, after the
 * fades / overlay of 0x156940 -> 0x14e020) and the settings-menu copy f_4ed940 (shard_0105.c:38419).  On a narrow screen H1 replaces y
 * by H' * GX+0x24, the value of the exe's own not-narrow branch: the bars get zero height.  x is never changed; at 16:9 or wider the
 * exe's y already is that value, so H1 is an identity there.  H1 acts when fill is on and the screen is narrow, on EVERY frame (task
 * 46, H7: no bars on the title and the full-screen menus either).  Until task 46 it acted only when the 3D view was drawn in this game
 * frame or the previous one; KHNMC_ASPECT_TEST_OFF=h7 (a test aid) brings that gate back, and KHNMC_ASPECT_TEST_H1=unconditional (the
 * older test aid) drops it whatever else is set.  Each call is checked against a replica of the exe's arithmetic
 * (h1_model); a call whose result differs (for example GX changed by another thread between the exe's reads and ours) is left as the
 * exe made it and counted (reported at exit, with a WARNING line when not zero).
 *
 * The "3D view drawn" frame (hook on f_186920, the map pass; its only call site is shard_0029.c:13361 in f_170450, return address
 * 0x1401704a1): a pass-through that, when KHNMC_ASPECT=fill or KHNMC_ASPECT_LOG=1, stores the game frame (runtime/null_input.c
 * kh_game_frames) of its last call.  This is the flag half of the plan's H2b; H2b's m11 edit (cam+0x98 / r around the call) is plan
 * step 5 and joins this wrapper then.  Later steps add the hooks 2bd190 126500 156940 1569b0 156a70 156e20 126fd0 4b7530 (Makefile).
 *
 * GX is the GXDevice singleton: a static object at 0x1408a0970 (f_11c150, the accessor, returns that address: shard_0022.c block
 * L11c182; createDevices 0x124060 is called with this = 0x1408a0970).  Its size fields as the exe uses them: +0x8/+0xc the built size
 * (ints, after the VRAM guard 0x124b50 called from 0x1119b0), +0x10/+0x14 the same as floats, +0x18/+0x1c the screen size f_11c1e0
 * reads (floats), +0x20/+0x24 and +0x28 its scale factors, +0x438/+0x43c the requested size (floats, 0x1119b0 before the guard),
 * +0x408/+0x40c the 16:9 fit createDevices stores (0x1246ae; used only to compare settings, plan section 3), +0x41c/+0x420 the
 * settings' window size, byte +0x416 the fullscreen setting (0x117fd0 takes the fullscreen branch when it is not zero). */
#include "khnmc_rt.h"
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>

#define GX (GS_BASE+0x8a0970ull)
#define GX_GUARD (GS_BASE+0x8aeef0ull)      /* f_11c150's thread-safe-static guard (selftest only) */
u64 kh_game_frames(void);
extern _Atomic u64 gfx_presents;

static int asp_mode;                       /* 0 = pc, 1 = fill */
static int asp_h1_uncond;                  /* KHNMC_ASPECT_TEST_H1=unconditional */
int kh_aspect_log;                         /* KHNMC_ASPECT_LOG=1 (also read by runtime/null_d3d12.c) */

/* ---- the plan's section 3 numbers for a W x H screen ------------------------------------------------------------------------
 * narrow = 9W < 16H (the exe's test; exact in double for integer sizes); r = 16H / 9W when narrow, else exactly 1; o = (H - 9W/16) / 2
 * the 16:9 band's offset in pixels; b = 208 (r - 1) the extra GS rows on each side.  Exactly 1, 0, 0 at 16:9 or wider.  Used by the
 * later hooks (H2: m11 / r, H3: MS.w +- o, H4: rects +- ceil(b)); here by the selftest and the report. */
static int narrow_d(double W,double H){ return 9.0*W<16.0*H; }
double kh_aspect_r(double W,double H){ return narrow_d(W,H)?(16.0*H)/(9.0*W):1.0; }
double kh_aspect_o(double W,double H){ return narrow_d(W,H)?(H-9.0*W/16.0)/2.0:0.0; }
double kh_aspect_b(double W,double H){ return narrow_d(W,H)?208.0*(kh_aspect_r(W,H)-1.0):0.0; }

/* ---- configuration ---------------------------------------------------------------------------------------------------------- */
int kh_aspect_m;                           /* task 40: KHNMC_ASPECT_LOG=2, the measurement M (also read by runtime/null_d3d12.c) */
int kh_aspect_dk;                          /* task 42: KHNMC_ASPECT=fill or the measurement M: runtime/bgload.c's f_4ad270 hook reports the
                                              dk loop's next object (H3b, M); 0 with nothing set or pc without the log (one flag test) */
void kh_aspect_config(const char *mode,const char *log,const char *test_h1){
    if(mode&&*mode){ if(!strcmp(mode,"pc")) asp_mode=0; else if(!strcmp(mode,"fill")) asp_mode=1;
        else kh_fatal(NULL,"aspect-mode-unknown: KHNMC_ASPECT=%s (known: pc, fill)",mode); }
    if(log&&*log){ if(!strcmp(log,"1")) kh_aspect_log=1; else if(!strcmp(log,"2")) kh_aspect_log=kh_aspect_m=1; else if(!strcmp(log,"0")) kh_aspect_log=0;
        else kh_fatal(NULL,"aspect-log-value: KHNMC_ASPECT_LOG=%s (expected 2, 1 or 0)",log); }
    if(test_h1&&*test_h1){
        if(strcmp(test_h1,"unconditional")) kh_fatal(NULL,"aspect-test-h1-value: KHNMC_ASPECT_TEST_H1=%s (the only value is unconditional, a test aid)",test_h1);
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-h1-needs-fill: KHNMC_ASPECT_TEST_H1=unconditional changes H1, which runs only with KHNMC_ASPECT=fill");
        asp_h1_uncond=1; }
    kh_aspect_dk=asp_mode||kh_aspect_m;
}
/* the run's start line.  It is composed from the state BOTH configuration functions leave (task 46 review finding 7: the line used to
 * be printed before KHNMC_ASPECT_TEST_OFF was read and still said "on frames with the 3D view", the gate H7 removed), so
 * kh_aspect_test_config prints it */
static int asp_off_h7;                     /* KHNMC_ASPECT_TEST_OFF=h7 (task 46): the gate of tasks 37 / 42 again, band rectangles left alone */
static void asp_banner(char *out,size_t n){
    snprintf(out,n,"kh: aspect: KHNMC_ASPECT=%s%s%s",asp_mode?"fill (H1: no 16:9 bars on a screen narrower than 16:9":"pc",
             asp_mode?(asp_h1_uncond?", UNCONDITIONAL: every frame, KHNMC_ASPECT_TEST_H1 test aid)":
                       asp_off_h7?", only on frames with the 3D view: TEST AID KHNMC_ASPECT_TEST_OFF=h7, the gate of task 37)":", on every frame: task 46, H7)"):"",
             kh_aspect_m?"; KHNMC_ASPECT_LOG=2: size records and the measurement M on":kh_aspect_log?"; KHNMC_ASPECT_LOG=1: size records on":"");
}
/* task 40 test aids (plan step 5 / 7 tests; both need KHNMC_ASPECT=fill, any other value is a named fatal):
 *   KHNMC_ASPECT_TEST_OFF=list   comma-separated h2 (= h2b,h2c), h2b, h2c, h3 (task 42), h4, h5, h7 (task 46): those fill hooks pass straight
 *                                through (the picture "without its change"); the others stay on.
 *   KHNMC_ASPECT_TEST_NOCULL=1   H2b / H2c divide m11 by 2047/208 (the exe's guard band) instead of r: the "no-cull reference" of
 *                                plan step 5 (nothing is culled vertically inside the guard band; the tall picture must equal it). */
static int asp_off_h2b, asp_off_h2c, asp_off_h3, asp_off_h4, asp_off_h5, asp_nocull;
void kh_aspect_test_config(const char *off,const char *nocull){
    if(off&&*off){
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_OFF=%s turns fill hooks off, which run only with KHNMC_ASPECT=fill",off);
        const char *p=off;
        while(*p){ size_t n=strcspn(p,",");
            if(n==2&&!strncmp(p,"h2",2)) asp_off_h2b=asp_off_h2c=1; else if(n==3&&!strncmp(p,"h2b",3)) asp_off_h2b=1; else if(n==3&&!strncmp(p,"h2c",3)) asp_off_h2c=1;
            else if(n==2&&!strncmp(p,"h3",2)) asp_off_h3=1;
            else if(n==2&&!strncmp(p,"h4",2)) asp_off_h4=1; else if(n==2&&!strncmp(p,"h5",2)) asp_off_h5=1;
            else if(n==2&&!strncmp(p,"h7",2)) asp_off_h7=1;
            else kh_fatal(NULL,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=%s: '%.*s' is not one of h2, h2b, h2c, h3, h4, h5, h7 (comma-separated)",off,(int)n,p);
            p+=n; if(*p==','){ p++; if(!*p) kh_fatal(NULL,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=%s ends with a comma",off); } } }
    if(nocull&&*nocull){
        if(strcmp(nocull,"1")) kh_fatal(NULL,"aspect-test-nocull-value: KHNMC_ASPECT_TEST_NOCULL=%s (the only value is 1, a test aid)",nocull);
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_NOCULL=1 changes H2, which runs only with KHNMC_ASPECT=fill");
        asp_nocull=1; }
    if(asp_mode||kh_aspect_log){ char b[512]; asp_banner(b,sizeof b); fprintf(stderr,"%s\n",b); }
    if(asp_off_h2b||asp_off_h2c||asp_off_h3||asp_off_h4||asp_off_h5||asp_off_h7||asp_nocull)
        fprintf(stderr,"kh: aspect: TEST AID: %s%s%s%s%s%s%s(not for play)\n",asp_off_h2b?"H2b off; ":"",asp_off_h2c?"H2c off; ":"",asp_off_h3?"H3 off; ":"",asp_off_h4?"H4 off; ":"",asp_off_h5?"H5 off; ":"",
                asp_off_h7?"H7 off (the exe's bars on frames without the 3D view, band rectangles left in the band); ":"",
                asp_nocull?"H2 divides m11 by 2047/208 instead of r (the no-cull reference); ":"");
}
void kh_aspect_drawlog_config(const char *v);
void kh_aspect_zoom_config(const char *v);          /* task 49, below */
void kh_aspect_hud_config(const char *cmd,const char *hpv);   /* task 53, below */
void kh_aspect_title_config(const char *v);                   /* task 53, below */
static void zoom_storage_init(void);
static int test_event_bars;
static int hide_title_credit;
static _Atomic u64 hidden_credit_batches;
static void portrait_config(void);
void kh_aspect_init(void){
    const char *credit=getenv("KHNMC_TITLE_CREDIT");
    hide_title_credit=credit && !strcmp(credit,"hide");
    if(credit && *credit && !hide_title_credit && strcmp(credit,"show"))
        kh_fatal(NULL,"title-credit-value: KHNMC_TITLE_CREDIT=%s (expected show or hide)",credit);
    portrait_config(); kh_aspect_config(getenv("KHNMC_ASPECT"),getenv("KHNMC_ASPECT_LOG"),getenv("KHNMC_ASPECT_TEST_H1"));
    const char *bars=getenv("KHNMC_ASPECT_TEST_EVENT_BARS");
    if(bars&&*bars){
        if(strcmp(bars,"1")||!asp_mode) kh_fatal(NULL,"aspect-test-event-bars: expected 1 with KHNMC_ASPECT=fill");
        test_event_bars=1;
        fprintf(stderr,"kh: aspect: TEST AID: draw inactive cinematic bars during events\n");
    }
    zoom_storage_init();
    kh_aspect_zoom_config(getenv("KHNMC_FOV_ZOOM"));
    kh_aspect_hud_config(getenv("KHNMC_HUD_SCALE_CMD"),getenv("KHNMC_HUD_SCALE_HP"));
    kh_aspect_title_config(getenv("KHNMC_TITLE_LAYOUT"));
    kh_aspect_test_config(getenv("KHNMC_ASPECT_TEST_OFF"),getenv("KHNMC_ASPECT_TEST_NOCULL"));
    kh_aspect_drawlog_config(getenv("KHNMC_ASPECT_M_DRAWLOG")); }

static inline float rdf(KCtx *g,u64 a){ u32 u=rd32(g,a); float f; memcpy(&f,&u,4); return f; }
static inline void wrf(KCtx *g,u64 a,float f){ u32 u; memcpy(&u,&f,4); wr32(g,a,u); }
static inline u32 fbits(float f){ u32 u; memcpy(&u,&f,4); return u; }
static u32 cur_tid(void){ KThread *t=cur_thread(); return t?t->tid:0; }
#define P(x) ((unsigned long long)(x))

/* ---- the "3D view drawn" frame ------------------------------------------------------------------------------------------- */
static _Atomic u64 last3d_plus1;           /* game frame of the last map-pass call + 1; 0 = never */
static _Atomic u64 map_calls; static _Atomic u32 map_tid;
static void note_3d(u64 frame){ atomic_store(&last3d_plus1,frame+1); }
/* game frames since the 3D view was last drawn, seen at game frame `now`: -1 = never; 0 also when the note is from a later frame than
 * `now` (the counter moved on between the two reads) */
static s64 since_3d(u64 now){ u64 l=atomic_load(&last3d_plus1); if(!l) return -1; s64 d=(s64)now-(s64)(l-1); return d<0?0:d; }
static int gate_open_d(s64 d){ return d>=0&&d<=1; }                /* drawn in this game frame or the previous one */
static int gate_open_at(u64 now){ return gate_open_d(since_3d(now)); }
/* the hook on f_186920 itself (the frame note and, since task 40, H2b's m11 edit) is in the task-40 section below */

/* ---- H1 ---------------------------------------------------------------------------------------------------------------------- */
typedef struct { float x,y,yfull; int narrow; } H1Model;
/* the exe's arithmetic, operation by operation in single precision (SSE scalar, -ffp-contract=off as the Makefile compiles) */
static H1Model h1_model(float W,float H,float sx,float sy,float s){
    const float one=1.0f; float s1=(one<s)?one:s;                     /* MINSS 1.0, [GX+0x28]: the first operand when it is less, else the second (NaN too) */
    float Wp=W*s1, Hp=H*s1, a=Wp*9.0f, b=Hp*16.0f; H1Model m; m.narrow=!(a>=b);
    if(m.narrow){ m.x=Wp*sx; m.y=(a*0.0625f)*sy; } else { m.x=(b/9.0f)*sx; m.y=Hp*sy; }
    m.yfull=Hp*sy; return m;
}
static _Atomic u64 h1_calls, h1_wide, h1_narrow, h1_rewritten, h1_kept, h1_mismatch, h1_since[4]; static _Atomic u32 h1_tid;
static _Atomic int h1_logged_narrow, h1_logged_rewrite, h1_logged_mismatch;
static _Atomic u64 h7_h1_no3d, h7_h3_no3d;  /* task 46 (H7): H1 rewrites / dk passes anchored on frames without the 3D view (the gate of tasks 37 / 42 would have kept the bars) */
static void h1_post(KCtx *g,u64 out){
    h1_calls++; { u32 z=0; atomic_compare_exchange_strong(&h1_tid,&z,cur_tid()); }
    float W=rdf(g,GX+0x18), H=rdf(g,GX+0x1c), sx=rdf(g,GX+0x20), sy=rdf(g,GX+0x24), s=rdf(g,GX+0x28);
    float ox=rdf(g,out), oy=rdf(g,out+4); H1Model m=h1_model(W,H,sx,sy,s);
    if(fbits(ox)!=fbits(m.x)||fbits(oy)!=fbits(m.y)){
        h1_mismatch++;
        if(!atomic_exchange(&h1_logged_mismatch,1)) fprintf(stderr,"kh: aspect: WARNING: H1: the exe's 16:9 fit (%g x %g) differs from the model (%g x %g) for GX+0x18 %g x %g, GX+0x20/0x24 %g/%g, GX+0x28 %g: left as the exe made it (counted)\n",ox,oy,m.x,m.y,W,H,sx,sy,s);
        return; }
    if(!m.narrow){ h1_wide++; return; }
    h1_narrow++;
    u64 now=kh_game_frames(); s64 d=since_3d(now); h1_since[d<0?3:d>2?2:d]++;      /* [0] same frame, [1] previous, [2] 2 or more frames earlier, [3] never */
    if(kh_aspect_log&&!atomic_exchange(&h1_logged_narrow,1))
        fprintf(stderr,"kh: aspect: H1: first narrow screen: GX+0x18 %g x %g, GX+0x20/0x24 %g/%g, GX+0x28 %g -> the exe's 16:9 fit %g x %g, bars of %g rows above and below (GX+0x10 height %g); full height %g; game frame %llu, present %llu, guest thread %u\n",
                W,H,sx,sy,s,ox,oy,(rdf(g,GX+0x14)-oy)/2,rdf(g,GX+0x14),m.yfull,P(now),P(gfx_presents),cur_tid());
    if(!asp_mode) return;                                                           /* pc: counted only */
    if(asp_h1_uncond||gate_open_d(d)||!asp_off_h7){                                   /* task 46 (H7): no bars on any frame of a narrow screen */
        wrf(g,out+4,m.yfull); h1_rewritten++; if(!asp_h1_uncond&&!gate_open_d(d)) h7_h1_no3d++;
        if(kh_aspect_log&&!atomic_exchange(&h1_logged_rewrite,1))
            fprintf(stderr,"kh: aspect: H1: first rewrite: %g x %g -> %g x %g (%s) at game frame %llu, present %llu\n",ox,oy,ox,m.yfull,asp_h1_uncond?"unconditional":!gate_open_d(d)?"no 3D view in this frame: H7, no bars on any frame":d==0?"3D view drawn in this frame":"3D view drawn in the previous frame",P(now),P(gfx_presents));
    } else h1_kept++;
}
void orig_f_11c1e0(KCtx *);
void f_11c1e0(KCtx *g){
    u64 out=KH_RCX(g);
    orig_f_11c1e0(g);
    if(__builtin_expect(!asp_mode&&!kh_aspect_log,1)) return;                       /* pc without the log: exactly the exe */
    h1_post(g,out);
}

/* ---- size records (KHNMC_ASPECT_LOG=1, called by runtime/null_d3d12.c) --------------------------------------------------------- */
static pthread_mutex_t am=PTHREAD_MUTEX_INITIALIZER;
typedef struct { u32 dim,fmt,w,h,arr,mips,samples,flags; u64 n,first_present,first_frame; } RtKind;
static RtKind rtk[64]; static int n_rtk; static u64 rtk_more, rt_total;
static u64 sc_notes, sc_lines_cut; static u32 sc_w, sc_h;
static const char *rt_flags(u32 f){ static __thread char b[64]; snprintf(b,sizeof b,"%s%s%s%s",f&1?" RT":"",f&2?" DS":"",f&4?" UAV":"",f&8?" no-SRV":""); return b; }
void kh_aspect_note_swapchain(KCtx *g,const char *what,u64 d,u32 w,u32 h){
    pthread_mutex_lock(&am); sc_notes++;
    if(w) sc_w=w; if(h) sc_h=h;
    int print=sc_notes<=32; if(!print) sc_lines_cut++;
    pthread_mutex_unlock(&am);
    if(!print) return;
    if(d) fprintf(stderr,"kh: aspect: swap chain: %s %ux%u format %u buffers %u samples %u scaling %u swap-effect %u alpha %u flags 0x%x (present %llu, game frame %llu)\n",
                  what,w,h,rd32(g,d+8),rd32(g,d+28),rd32(g,d+16),rd32(g,d+32),rd32(g,d+36),rd32(g,d+40),rd32(g,d+44),P(gfx_presents),P(kh_game_frames()));
    else fprintf(stderr,"kh: aspect: swap chain: %s %ux%u (0 = keep that side) (present %llu, game frame %llu)\n",what,w,h,P(gfx_presents),P(kh_game_frames()));
}
/* every texture with the render-target or depth-stencil flag, and every multisampled texture (D3D12_RESOURCE_DESC: Dimension +0,
 * Width +16, Height +24, DepthOrArraySize +28, MipLevels +30, Format +32, SampleDesc.Count +36, Flags +48) */
void kh_aspect_note_texture(KCtx *g,u64 d){
    u32 dim=rd32(g,d), flags=rd32(g,d+48), samples=rd32(g,d+36);
    if(dim<2||dim>4||(!(flags&3)&&samples<=1)) return;
    RtKind k={dim,rd32(g,d+32),(u32)rd64(g,d+16),rd32(g,d+24),rd16(g,d+28),rd16(g,d+30),samples,flags,0,0,0};
    pthread_mutex_lock(&am); rt_total++; int i; for(i=0;i<n_rtk;i++){ RtKind *q=&rtk[i];
        if(q->dim==k.dim&&q->fmt==k.fmt&&q->w==k.w&&q->h==k.h&&q->arr==k.arr&&q->mips==k.mips&&q->samples==k.samples&&q->flags==k.flags) break; }
    int fresh=0; if(i==n_rtk&&n_rtk<64){ k.first_present=gfx_presents; k.first_frame=kh_game_frames(); rtk[i]=k; n_rtk++; fresh=1; }
    if(i<64) rtk[i].n++; else rtk_more++;
    pthread_mutex_unlock(&am);
    if(fresh) fprintf(stderr,"kh: aspect: rt census: new kind %d: %uD fmt%u %ux%u array %u mips %u samples %u flags 0x%x%s (present %llu, game frame %llu)\n",
                      i,k.dim-1,k.fmt,k.w,k.h,k.arr,k.mips,k.samples,k.flags,rt_flags(k.flags),P(k.first_present),P(k.first_frame));
}
typedef struct { s32 bw,bh; u32 rw,rh,W,H,sx,sy,s; s32 fw,fh; u32 qw,qh; s32 setw,seth; u32 fs; } GxSnap;
static GxSnap gx_last; static int gx_have; static u64 gx_lines, gx_lines_cut, gx_guard_presents;
static GxSnap gx_read(KCtx *g){ GxSnap x; memset(&x,0,sizeof x);
    x.bw=(s32)rd32(g,GX+0x8); x.bh=(s32)rd32(g,GX+0xc); x.rw=rd32(g,GX+0x10); x.rh=rd32(g,GX+0x14); x.W=rd32(g,GX+0x18); x.H=rd32(g,GX+0x1c);
    x.sx=rd32(g,GX+0x20); x.sy=rd32(g,GX+0x24); x.s=rd32(g,GX+0x28); x.fw=(s32)rd32(g,GX+0x408); x.fh=(s32)rd32(g,GX+0x40c);
    x.qw=rd32(g,GX+0x438); x.qh=rd32(g,GX+0x43c); x.setw=(s32)rd32(g,GX+0x41c); x.seth=(s32)rd32(g,GX+0x420); x.fs=rd8(g,GX+0x416); return x; }
static float bf(u32 u){ float f; memcpy(&f,&u,4); return f; }
static int guard_reduced(const GxSnap *x){ return x->qw&&((float)x->bw!=bf(x->qw)||(float)x->bh!=bf(x->qh)); }
static void gx_print(FILE *f,const char *when,const GxSnap *x){
    fprintf(f,"kh: aspect: GX sizes %s: built (GX+0x8) %dx%d, GX+0x10 %gx%g, GX+0x18 %gx%g, scale GX+0x20/0x24 %g/%g, GX+0x28 %g; requested (GX+0x438) %gx%g: %s; 16:9 fit (GX+0x408) %dx%d; settings window (GX+0x41c) %dx%d, fullscreen byte (GX+0x416) %u\n",
            when,x->bw,x->bh,bf(x->rw),bf(x->rh),bf(x->W),bf(x->H),bf(x->sx),bf(x->sy),bf(x->s),bf(x->qw),bf(x->qh),
            !x->qw?"not set yet":guard_reduced(x)?"REDUCED by the VRAM guard (0x124b50)":"built as requested (no VRAM-guard change)",x->fw,x->fh,x->setw,x->seth,x->fs);
}
void kh_aspect_note_present(KCtx *g,u64 present){
    GxSnap x=gx_read(g); int changed;
    pthread_mutex_lock(&am); changed=!gx_have||memcmp(&x,&gx_last,sizeof x); if(changed){ gx_last=x; gx_have=1; } if(guard_reduced(&x)) gx_guard_presents++;
    int print=changed&&gx_lines<64; if(changed){ if(print) gx_lines++; else gx_lines_cut++; } pthread_mutex_unlock(&am);
    if(print){ char when[96]; snprintf(when,sizeof when,"at present %llu (game frame %llu)",P(present),P(kh_game_frames())); gx_print(stderr,when,&x); }
}

/* ---- the size the game built against KHNMC_SCREEN (task 37 review, finding 1) --------------------------------------------------
 * The seam only REPORTS a W x H screen; what the game builds is its own choice.  With KHNMC_SCREEN set, every present (runtime/
 * null_d3d12.c; every backend, the swap chain is always the null DXGI object) checks the swap chain being presented (sc+32/+36), the
 * built size GX+0x8/+0xc (ints; the VRAM guard 0x124b50 lowers it below the requested GX+0x438) and the screen size GX+0x18/+0x1c
 * (floats; the 16:9 fit 0x11c1e0 and H1 read it).  Any of them not W x H is the named condition `screen-not-applied`: a WARNING line
 * at the first such present, every such present counted, the counts in the exit report (kh_screen_report, printed whenever
 * KHNMC_SCREEN is set), with or without KHNMC_ASPECT_LOG.  A known way there: 0x117fd0 takes its fullscreen branch (f_1119b0 with
 * GX+0x440 instead of IsZoomed / GetClientRect) when GX+0x416 is not 0; GX+0x416 is written by 0x117fd0 itself as !Windowed of the
 * swap chain's GetFullscreenDesc (0x140118494..0x14011849b; the null swap chain answers Windowed = TRUE) and by 0x124970
 * (0x1401249d2, from its argument).  Without KHNMC_SCREEN nothing is checked, counted or printed. */
static _Atomic u64 scr_chk_presents, scr_chk_bad; static _Atomic int scr_chk_logged;
int kh_screen_size(int *w,int *h);
int kh_screen_check_present(KCtx *g,u32 scw,u32 sch,u64 present){
    int W,H; if(!kh_screen_size(&W,&H)) return 0;
    scr_chk_presents++;
    s32 bw=(s32)rd32(g,GX+0x8), bh=(s32)rd32(g,GX+0xc); float fw=rdf(g,GX+0x18), fh=rdf(g,GX+0x1c);
    int sc_ok=scw==(u32)W&&sch==(u32)H, built_ok=bw==W&&bh==H, scr_ok=fw==(float)W&&fh==(float)H;
    if(sc_ok&&built_ok&&scr_ok) return 0;
    scr_chk_bad++;
    if(!atomic_exchange(&scr_chk_logged,1))
        fprintf(stderr,"kh: screen: WARNING: screen-not-applied: KHNMC_SCREEN=%dx%d, but at present %llu (game frame %llu) the game has: swap chain %ux%u%s, built size (GX+0x8) %dx%d%s, screen size (GX+0x18) %gx%g%s; requested (GX+0x438) %gx%g, fullscreen byte (GX+0x416) %u.  Every such present is counted (exit report)\n",
                W,H,P(present),P(kh_game_frames()),scw,sch,sc_ok?"":" (NOT the screen)",bw,bh,built_ok?"":" (NOT the screen)",fw,fh,scr_ok?"":" (NOT the screen)",
                rdf(g,GX+0x438),rdf(g,GX+0x43c),rd8(g,GX+0x416));
    return 1;
}
void kh_screen_check_counts(u64 *presents,u64 *bad){ if(presents) *presents=scr_chk_presents; if(bad) *bad=scr_chk_bad; }

/* ==== task 40: NATIVE-FILL-PLAN.md section 8 step 5 (H2), the H4 / H5 part of step 7, and the measurement M of step 4 =============
 *
 * Every fill hook below computes the screen's shape from GX+0x18 / +0x1c (floats) at the time of the call: r = 16H / 9W when 9W < 16H,
 * else exactly 1 (kh_aspect_r), and B = ceil(208 (r - 1)) extra GS rows above and below the 16:9 band (52 at 1920x1350, 53 at the
 * phone sizes).  At r = 1 (16:9 or wider) each of them writes nothing at all: an exact pass-through.  With KHNMC_ASPECT unset or pc
 * they are pass-throughs too (one flag test), and so are the M-only hooks without KHNMC_ASPECT_LOG=2.
 *
 * H2 (plan 4.4 as corrected by the verification, section 11 finding 1): the map pass f_186920 and the effect pass f_2bd190 cull with the
 * clip matrix cam+0x84 that f_19c820 builds with the 16:9 half-height 208; halfH reaches only m11 = cam+0x84+0x14 = cam+0x98
 * (0x19c8b0), so the Vert+ clip is the PC clip with that float divided by r.  Around each call: save the 4 bytes at cam+0x98, write
 * (float)(saved / r), call the lifted body, put the saved bytes back.  If the body itself wrote cam+0x98 (it is found holding neither
 * our value), the game's value is left and the event counted (WARNING line, exit report): no silent path.
 *   H2b f_186920 (one call site, f_170450 at 0x1401704a1): camera = f_19cfc0(1) = 0x140ac1840 + 0x184 * (s32)[0x140ac1838]
 *       (shard_0032.c L19cfd3 / L19cfda); f_186920 copies it to rctx+0x38 (f_186810 at 0x186962) and reads the copy, so the copy keeps
 *       the wide m11 for the render-side readers of rctx+0x488 (plan 4.4).  The frame note for H1 (task 37) stays here.
 *   H2c f_2bd190 (ryj::EFFECT, callers f_2bd4c0 and 0x300104): camera = guest rcx (0x2bd1b2 mov rdi,rcx).
 *   The kn / YS / Ti readers of cam+0x84 (gameplay: on-screen and line-of-sight tests) keep the PC value (plan 4.4, "not widened").
 *   Task 49 keeps that true on a zoomed tall screen: the camera stays the exe's build; only the drawing sites of f_19cfc0 (the map
 *   pass, the effect pass, ...) get a copy with zoomed cam+0x00 / +0x104, whose cam+0x84 is the exe's, so H2 divides the same m11
 *   (section "task 49" below).
 * H4 (plan section 5 "full-screen fades", "event letterbox bars"; the setter offsets [R] verified here): f_160cc0(obj, w, h) stores
 *   s16 w, h at obj+0x8 / +0xa and f_160cd0(obj, x, y) s16 x, y at obj+0xc / +0xe (shard_0028.c:1516 / :1538).  The three fades
 *   (f_156940 -> object 0x140abb3b0, f_1569b0 -> 0x140abb720, f_156a70 -> 0x140abb290) set y = -1, h = 0x1a2 and then DRAW at once:
 *   each ends in f_156f10(obj), whose last step is the object's vfn1 (shard_0027.c L15701f), so a hook after them would come one
 *   frame late and be overwritten.  H4a therefore sits at the entry of f_156f10 and acts on those three objects only when they hold
 *   the exe's rect (y = -1, h = 0x1a2): y = -1 - B, h = 0x1a2 + 2B.  The event letterbox bars: f_156e20 only SETS four objects (top
 *   0x140abb170 and 0x140abb600: y = -1, h = 0x24; bottom 0x140abb050 and 0x140abb4e0: y = 0x17d, h = 0x24; its last setter is a
 *   tail call, so no return address names it) and draws nothing; H4b runs after it: move each bar to its frame edge, keeping its PC height.
 *   H4a also applies the bar rewrite at their draw (f_156f10), in case another
 *   path set the exe's rect again.  An object found with another rect is left alone and counted.
 * H5 (plan section 5 "scissored 2D", f_126fd0): f_472b20 calls f_126fd0(x0, y0, w = x1 - x0 + 1, h = y1 - y0 + 1) from the GS rect
 *   table at 0x14075e7a0 (+0x58 x0, +0x5a x1, +0x5c y0, +0x5e y1, shard_0095.c L472ba9); f_126fd0 stores them at GX+0xf4 / +0xf8 /
 *   +0xfc / +0x100.  A full-height rect (y0 = 0, h = 416) becomes y0 = -B, h = 416 + 2B; any other rect is left.  (A rect that is also
 *   full width never gets here: 0x15fdd0 disables the scissor for {0,511,0,415}, plan section 2.)
 * M (KHNMC_ASPECT_LOG=2, plan step 4): the dk draw loop f_166640 (its setScreenMode f_126500 at return address 0x140166677, the next
 *   object from f_4ad270 at 0x140166693, the restore f_126500 at 0x1401666bf), per object class (vtable, RTTI name) its position in
 *   the pass, flags, the draws while it is current by vertex-shader kind (null backend: runtime/null_d3d12.c identifies the bound
 *   pipeline's VS by its DXBC checksum against the blobs in the image) and the GS-frame y range of the MagicScale draws, nested
 *   setScreenMode calls; the f_126fd0 scissors and the D3D12 scissor rects; the cam+0x84 readers (f_19d040, by return address) and
 *   the cameras they get; byte 0x140ac183c at the effect pass; the fade / bar draws.  The RT census and the VRAM guard are the size
 *   records above (KHNMC_ASPECT_LOG=1 and 2). */
#define CAM_CUR   (GS_BASE+0xac1840ull)    /* f_19cfc0(cl != 0): the cameras f_19cad0 rebuilds */
#define CAM_PREV  (GS_BASE+0xac1220ull)    /* f_19cfc0(0) when byte 0x140ac183c is 0: the copies f_19cba0 makes with f_186810 */
#define CAM_IDX   (GS_BASE+0xac1838ull)    /* s32 index into both arrays */
#define CAM_SEL   (GS_BASE+0xac183cull)    /* the byte f_19cfc0(0) compares with 0 */
#define CAM_SIZE  0x184ull
#define CAM_M11   0x98ull
#define DK_RA_MODE  (GS_BASE+0x166677ull)  /* f_166640: return address of its setScreenMode({1,0,0,0}) */
#define DK_RA_NEXT  (GS_BASE+0x166693ull)  /* f_166640: return address of f_4ad270 (the next object) */
#define DK_RA_END   (GS_BASE+0x1666bfull)  /* f_166640: return address of the restoring f_126500 */
static pthread_mutex_t mm=PTHREAD_MUTEX_INITIALIZER;
int kh_aspect_B(double W,double H){ double b=kh_aspect_b(W,H); return b>0?(int)__builtin_ceil(b):0; }
static double gx_r(KCtx *g){ float W=rdf(g,GX+0x18), H=rdf(g,GX+0x1c); return (W>0&&H>0)?kh_aspect_r(W,H):1.0; }
static int gx_B(KCtx *g){ float W=rdf(g,GX+0x18), H=rdf(g,GX+0x1c); return (W>0&&H>0)?kh_aspect_B(W,H):0; }

/* ---- H2 ---------------------------------------------------------------------------------------------------------------------- */
typedef struct { const char *name; _Atomic u64 calls, off, identity, edited, inside, badcam; _Atomic int warned; } H2Stat;
static __thread int h2_in;                 /* 1 / 2 while H2b / H2c's m11 edit is in force on this thread (M: who reads cam+0x84 then) */
static H2Stat h2b={"H2b (0x186920, map pass)"}, h2c={"H2c (0x2bd190, effect pass)"};
static const double H2_NOCULL=2047.0/208.0;
/* the m11 value written for a saved value at the ratio `div` (r, or 2047/208 for the no-cull reference) */
static u32 h2_value(u32 saved,double div){ return fbits((float)((double)bf(saved)/div)); }
int kh_aspect_zoom_shadow(u64 a);           /* task 49, below */
static void h2_around(KCtx *g,u64 cam,void (*fn)(KCtx *),H2Stat *st){
    st->calls++;
    double r=gx_r(g);
    if(r==1.0){ st->identity++; fn(g); return; }                                   /* 16:9 or wider: nothing is written */
    u64 a=cam+CAM_M11;
    if(!cam||((!GA_OK(a)||!GA_OK(a+3))&&!kh_aspect_zoom_shadow(a))){ st->badcam++;  /* (task 49: a drawing site's camera copy is host memory) */
        if(!atomic_exchange(&st->warned,1)) fprintf(stderr,"kh: aspect: WARNING: %s: camera pointer 0x%llx is not guest memory: m11 not edited (counted)\n",st->name,P(cam));
        fn(g); return; }
    u32 saved=rd32(g,a), w=h2_value(saved,asp_nocull?H2_NOCULL:r);
    wr32(g,a,w);
    int sv_in=h2_in; h2_in=st==&h2b?1:2;                                          /* M: readers of cam+0x84 inside the edited call */
    fn(g);
    h2_in=sv_in;
    u32 now=rd32(g,a);
    if(now==w){ wr32(g,a,saved); st->edited++; }
    else { st->inside++;
        if(!atomic_exchange(&st->warned,1)) fprintf(stderr,"kh: aspect: WARNING: %s: cam+0x98 of camera 0x%llx changed inside the call (we wrote 0x%08x, found 0x%08x): the game's value is kept (counted)\n",st->name,P(cam),w,now); }
}
/* M: the camera classes */
static int cam_class(u64 cam,int *idx){
    if(cam>=CAM_CUR&&cam<CAM_CUR+8*CAM_SIZE&&(cam-CAM_CUR)%CAM_SIZE==0){ *idx=(int)((cam-CAM_CUR)/CAM_SIZE); return 0; }
    if(cam>=CAM_PREV&&cam<CAM_PREV+4*CAM_SIZE&&(cam-CAM_PREV)%CAM_SIZE==0){ *idx=(int)((cam-CAM_PREV)/CAM_SIZE); return 1; }
    *idx=-1; return 2;
}
/* game-frame intervals (gaps of up to 30 frames are joined) */
#define NIV 10
typedef struct { u32 a,b; } Iv;
typedef struct { Iv v[NIV]; int n; u64 cut; } Ivs;
static void iv_add(Ivs *s,u32 f){
    if(s->n&&f+1>=s->v[s->n-1].a+1&&f<=s->v[s->n-1].b+30){ if(f>s->v[s->n-1].b) s->v[s->n-1].b=f; return; }
    if(s->n<NIV){ s->v[s->n].a=s->v[s->n].b=f; s->n++; } else { s->cut++; if(f>s->v[NIV-1].b) s->v[NIV-1].b=f; } }
static void iv_print(FILE *f,const Ivs *s){ fprintf(f," frames"); for(int i=0;i<s->n;i++) fprintf(f,"%s%u-%u",i?",":" ",s->v[i].a,s->v[i].b); if(s->cut) fprintf(f," (+%llu joined into the last)",P(s->cut)); }
/* dst = the union of dst's and src's intervals, in order, joined as iv_add joins (gaps of up to 30 frames); beyond NIV intervals the
 * rest is joined into the last and counted.  (Task 40 fix: the M group lines added each source interval's two ends with iv_add, which
 * assumes rising frames: two instances 930-6999 and 414-557 printed "930-930,6999-6999,414-414,...".) */
static void iv_merge(Ivs *dst,const Ivs *src){
    Iv all[2*NIV]; int n=0;
    for(int i=0;i<dst->n;i++) all[n++]=dst->v[i];
    for(int i=0;i<src->n;i++) all[n++]=src->v[i];
    for(int a=1;a<n;a++){ Iv x=all[a]; int b=a; while(b>0&&all[b-1].a>x.a){ all[b]=all[b-1]; b--; } all[b]=x; }
    Ivs o; memset(&o,0,sizeof o); o.cut=dst->cut+src->cut;
    for(int i=0;i<n;i++){ Iv x=all[i];
        if(o.n&&(u64)x.a<=(u64)o.v[o.n-1].b+30){ if(x.b>o.v[o.n-1].b) o.v[o.n-1].b=x.b; }
        else if(o.n<NIV) o.v[o.n++]=x;
        else { o.cut++; if(x.b>o.v[NIV-1].b) o.v[NIV-1].b=x.b; } }
    *dst=o;
}
typedef struct { u64 ra; int cls, idx; u64 ptr, n, inb, inc; Ivs fr; } C84;
static C84 c84[48]; static int n_c84; static u64 c84_cut;
static void m_cam(KCtx *g,u64 ra,u64 cam){ int idx, cls=cam_class(cam,&idx); u32 fr=(u32)kh_game_frames(); (void)g;
    pthread_mutex_lock(&mm); int i; for(i=0;i<n_c84;i++) if(c84[i].ra==ra&&c84[i].cls==cls&&c84[i].idx==idx&&(cls<2||c84[i].ptr==cam)) break;
    if(i==n_c84){ if(n_c84<48){ c84[i].ra=ra; c84[i].cls=cls; c84[i].idx=idx; c84[i].ptr=cam; n_c84++; } else { c84_cut++; pthread_mutex_unlock(&mm); return; } }
    c84[i].n++; if(h2_in==1) c84[i].inb++; else if(h2_in==2) c84[i].inc++; iv_add(&c84[i].fr,fr); pthread_mutex_unlock(&mm); }
static u64 sel_hist[256];                  /* byte 0x140ac183c at each effect-pass call (M) */
void orig_f_186920(KCtx *); void orig_f_2bd190(KCtx *); void orig_f_19d040(KCtx *);
static u64 h2b_cam(KCtx *g){ return CAM_CUR+CAM_SIZE*(u64)(s64)(s32)rd32(g,CAM_IDX); }   /* = f_19cfc0(1) (shard_0032.c L19cfd3 / L19cfda) */
static void m_occ(KCtx *g);                /* M: the occlusion volumes the previous frame's map pass left (below, "M: occlusion volumes") */
/* the two wrappers with the pass as a parameter (task 40 review, finding 1): the hooks below call them with the lifted bodies, the
 * selftest with a stand-in pass that records cam+0x98 of every camera H2 could edit, so that "H2b edits f_19cfc0(1)'s camera, H2c edits
 * rcx's, each only when on" is tested on the code the game runs */
static void h2b_wrap(KCtx *g,void (*body)(KCtx *)){
    map_calls++; { u32 z=0; atomic_compare_exchange_strong(&map_tid,&z,cur_tid()); } note_3d(kh_game_frames());
    u64 cam=h2b_cam(g);
    if(kh_aspect_m){ m_cam(g,1,cam); m_occ(g); }                                      /* "ra" 1 = the camera H2b edits */
    if(!asp_mode){ body(g); return; }
    if(asp_off_h2b){ h2b.calls++; h2b.off++; body(g); return; }
    h2_around(g,cam,body,&h2b);
}
static void h2c_wrap(KCtx *g,void (*body)(KCtx *)){
    u64 cam=KH_RCX(g);
    if(kh_aspect_m){ m_cam(g,2,cam); u8 s=rd8(g,CAM_SEL); pthread_mutex_lock(&mm); sel_hist[s]++; pthread_mutex_unlock(&mm); }   /* "ra" 2 = H2c's camera */
    if(!asp_mode){ body(g); return; }
    if(asp_off_h2c){ h2c.calls++; h2c.off++; body(g); return; }
    h2_around(g,cam,body,&h2c);
}
void f_186920(KCtx *g){
    if(__builtin_expect(!asp_mode&&!kh_aspect_log,1)){ orig_f_186920(g); return; }
    h2b_wrap(g,orig_f_186920);
}
void f_2bd190(KCtx *g){
    if(__builtin_expect(!asp_mode&&!kh_aspect_m,1)){ orig_f_2bd190(g); return; }
    h2c_wrap(g,orig_f_2bd190);
}
/* M only: f_19d040 = `lea rax,[rcx+0x84]`, the accessor every cam+0x84 reader calls (task 49 leaves it alone: the camera's own cam+0x84
 * is the exe's build, section "task 49" below) */
void f_19d040(KCtx *g){
    if(__builtin_expect(kh_aspect_m,0)) m_cam(g,rd64(g,KH_RSP(g)),KH_RCX(g));
    orig_f_19d040(g);
}

/* ==== task 49: the zoom of the tall (portrait) screen ===============================================================================
 *
 * Below 16:9 the exe keeps the HORIZONTAL view of a 16:9 screen and adds rows (Vert+, plan sections 2 and 4), so on a portrait screen
 * (the phone: 2007x2853, r = 2.53) the picture is very wide-angle and characters are small.  The zoom z (default 1.3 on a screen taller
 * than wide, KHNMC_FOV_ZOOM 1.0 .. 1.89; exactly 1 on every other screen, in pc and with nothing set) makes the 3D picture z times
 * bigger about the screen centre.  Owner rule: gameplay exactly PC NMC, only the picture changes.
 *
 * The camera.  A camera struct (0x184 bytes) holds its projection distance d at +0x40; the FOV angles at +0x44 / +0x48 are derived from
 * it against the fixed 512 x 416 frame (f_19dde0, f_19df00, f_19dc50).  f_19cad0(cam) (shard_0032.c:51918) builds every matrix of it
 * from d and fields that do not depend on d: f_4748b0 (0x14019cb3e) writes cam+0x104 = the projection (its x / y scales linear in d),
 * cam+0x144 = the screen matrix (no d) and cam+0x00 = their product (the camera half of the 3D vertex shaders' ViewProjection);
 * f_19c820 (0x14019cb78) cam+0x84 = the world-to-GS clip matrix (m00 = d cam+0x4c / halfW, m11 = d cam+0x50 / 208, cam+0x98: the
 * float H2 divides by r), f_19c820 (tail jump 0x14019cb97) cam+0xc4 = the guard band (2047).  Nothing else is written (f_142ec0 and
 * the f_11c150 getters it and f_4748b0 call only read).  So a build with d * z in cam+0x40 gives the zoomed matrices; d, the FOV
 * angles and every other field are the same.
 *
 * Who reads a scene camera's matrices (review of task 49, finding 1: the first version zoomed the camera itself, and gameplay code read
 * it: the on-screen tests of kn f_1b5e30 / YS f_3f3ed0 (script syscall f_42ee40, object-list scan f_41f730, the camera module) through
 * cam+0x84, the camera's framing f_176710 / f_176ec0 through cam+0x00; the lock-on camera took another path for 84 frames).  The
 * matrices are read only through the camera pointer f_19cfc0 returns (62 call sites; the arrays 0x140ac1840 / 0x140ac1220 are not
 * referenced outside the camera module 0x19cad0..0x19ddd0) and from copies of a camera made there (the map pass's rctx+0x38 / +0x280,
 * f_19cba0's previous-frame copy 0x140ac1220[idx], object copies).  The 62 sites, by the use of what they get (implementation notes, "the
 * readers"; zsites below):
 *   D, drawing (9): the map pass f_186920 (copies the camera to rctx: every 3D draw's ViewProjection rctx+0x240, the cull and guard
 *       frustums rctx+0x488 / +0x4c8, the render's own projections f_16fcd0 / f_170450 into rctx+0xa0c / +0xf4c..0xf54), the render
 *       frame f_170450, the effect pass f_2bd4c0 -> f_2bd190 (f_2c31e0: the effects' matrices 0x142b0ede0 / 0x142b0ef20; its cull
 *       context from cam+0x84), dk f_1666f0 / f_1673b0 (dk::COMMAND_DRAW: [0x140abcd20]+0x30), dk::ModelSprite f_18eee0, dk::SpotBlend
 *       f_18f240, the prize draw order f_411f30 (PRIZEOBJ slot 4: a depth sort, then f_2bebe0 draws) and f_3b7150 (only for f_3b1fa0 /
 *       f_3b2340: the command menu's fade while the player is in the lower left of the picture, [0x140abc508]+0x3bc).
 *   G, game logic and other readers (20): the camera module (distance f_19d010 in f_1c8d90 / f_1c9f70 / f_1cb360 / f_396b80; FOV get
 *       and set f_165700, f_177f20, f_1780d0, f_178580, f_38eb50, f_38ebc0, f_2d27a0, near plane f_1a1610), kn f_1b5e30, YS f_3f3ed0,
 *       the camera's framing f_176710, the script syscall f_430ea0 (screen position of a point, table 0x140756730), the object picker
 *       f_438070 (YS::ROOTWINDOW).
 *   M, play modes whose own code projects with the scene camera for play and picture alike (33): the gummi ship gb (f_1e2580 ..
 *       f_219900: camera init, lock-on guns, screen positions), the gummi garage gm (f_247420 .. f_2827d0) and the minigames Ti
 *       (f_543020 .. f_554a30, f_2e1110).
 * The design follows from that: THE CAMERA STRUCT STAYS THE EXE'S BUILD, and only the drawing sites get the zoomed matrices.
 *   - zoom_build (the hook on f_19cad0), scene camera 0x140ac1840 / 0x140ac19c4: the lifted body builds the camera as the exe does
 *     (1); then, from the guest state of the call, with cam+0x40 = (float)(d * z), it builds again (2) and the zoomed cam+0x00 / +0x104
 *     are noted, with the build-1 matrices they belong to (the last two builds per camera: the previous-frame copy holds the one
 *     before); then the camera (all 0x184 bytes), the guest registers and the stack below the call are put back to what build 1 left.
 *     So every field of the camera, the registers and the stack are bit for bit the exe's (selftest), and game logic, which reads
 *     only that (G above, and anything unknown), sees PC.
 *   - zoom_cam_post (the hook on f_19cfc0): at a D site, on a zoomed screen, a camera of the two arrays (current or previous-frame
 *     copy) whose cam+0x00 / +0x104 are a noted build 1 is handed out as a COPY: its 0x184 bytes with the zoomed cam+0x00 / +0x104 of
 *     that build, in host memory owned by the site (zshadow, given to the game by its guest address as runtime/null_audio.c GADDR
 *     does; nothing is allocated, the guest heap is as without the zoom); each site uses its pointer before it can call again (the
 *     map pass and f_170450 copy it at once, the effect pass reads it at its start).  Everything else gets the camera.  So what is
 *     drawn is zoomed consistently (3D, effects, the render's markers and the dk sprites all take the same matrices), the clip and
 *     guard band in the copy are the exe's: culling is over-inclusive (it keeps what the PC view keeps; H2 divides the copy's m11 as
 *     it does the camera's), never under.  A camera none of whose noted builds matches (built at zoom 1, or another camera) is handed
 *     out as it is (counted).
 *   - M sites: the gummi ship, its garage and the minigames project with the scene camera both for play (lock-on guns, targets) and
 *     for their 2D (sights, markers); a zoomed picture there would put their 2D off its 3D, and play must stay PC.  So while one of
 *     them runs (a call from an M site in this game frame or the last), the zoom is 1: builds are the exe's only and D sites get the
 *     camera.  The field, battles and events have no M call (routes: 0).
 *   - The map pass's per-object camera (f_188120: a stack copy of rctx+0x38 with the object's own distance, rebuilt at 0x14018830d,
 *     return address 0x140188312) is drawing only: it is built the same way and keeps the zoomed cam+0x00 / +0x104.
 *   - Other cameras (object-owned: menu models Tz 0x2ffbb0, gm::CAMERA 0x272480, gm::WORKAREACAMERA 0x290d50 / 0x2915f0, 0x535040)
 *     are not zoomed (counted, their return addresses listed).
 * f_19d010 (the distance) and f_19ce90 (the FOV angles) are not hooked: d and the FOV are put back, and they are the game's state.  The
 * lock-on camera's limit (f_178580: the target within half the FOV minus 10 degrees, 0x1405b2300) is game logic and stays PC; the
 * picture's left / right edge is at tan = (341.33 / 256) tan(fov_x / 2) / z, so the limit stays inside the picture while z <= (4 / 3)
 * tan(f) / tan(f - 10 deg), whose least value over all f is 1.894 (f = 50 deg; the routes' field camera, fov_x = 1.55 rad: 1.907);
 * hence the range 1.0 .. 1.89 (review finding 3).
 *
 * Identity: with KHNMC_ASPECT unset or pc the hooks are a flag test and the lifted body; in fill mode on a screen that is not taller
 * than wide, or with KHNMC_FOV_ZOOM=1, z is exactly 1: one build, nothing written, every site gets the camera (counted). */
#define CAM_D          0x40ull                     /* the projection distance */
#define CAM_VP         0x00ull                     /* projection x screen matrix: what is drawn with */
#define CAM_PROJ       0x104ull                    /* the projection */
#define ZOOM_NCAM      2                           /* the scene cameras f_19d050 builds: 0x140ac1840 and 0x140ac19c4 */
#define ZOOM_RA_MAPOBJ (GS_BASE+0x188312ull)       /* f_188120: return address of its f_19cad0 (a stack copy of the map pass's camera) */
#define ZOOM_DEFAULT   1.3                         /* owner 2026-11-18 ~22:15, playing portrait on the phone: "the zoom is still alot i
                                                    * want roxas a bit closer" (task 49's first default was 1.15) */
#define ZOOM_MAX       1.89                        /* review finding 3: f_178580's limit inside the picture for every FOV (above) */
#define ZOOM_STACK     0x1000ull                   /* the guest stack below the call that the two builds may write (put back) */
enum { ZC_SCENE0=0, ZC_SCENE1=1, ZC_MAPOBJ=2, ZC_OTHER=3 };
static double asp_zoom_tall=ZOOM_DEFAULT; static int asp_zoom_set;
/* a KHNMC_FOV_ZOOM value: DIGITS[.DIGITS] with at most 15 digits in all and nothing else (no sign, exponent, space; no locale).
 * 0 = *z set, 1 = malformed, 2 = a number outside 1.0 .. 1.89 */
int kh_fov_zoom_parse(const char *s,double *z){
    u64 n=0, den=1; int nd=0, ni=0, nf=0, dot=0;
    if(!s) return 1;
    for(const char *p=s;*p;p++){
        if(*p=='.'){ if(dot||!ni) return 1; dot=1; continue; }
        if(*p<'0'||*p>'9') return 1;
        if(++nd>15) return 1;
        n=n*10+(u64)(*p-'0'); if(dot){ den*=10; nf++; } else ni++; }
    if(!ni||(dot&&!nf)) return 1;
    double v=(double)n/(double)den;                                                  /* both exact below 2^53: the correctly rounded value */
    if(!(v>=1.0&&v<=ZOOM_MAX)) return 2;
    *z=v; return 0;
}
void kh_aspect_zoom_config(const char *v){
    if(v&&*v){ double z=0; int e=kh_fov_zoom_parse(v,&z);
        if(e==1) kh_fatal(NULL,"fov-zoom-value-malformed: KHNMC_FOV_ZOOM=%s (expected a plain decimal number from 1.0 to 1.89, such as 1.3)",v);
        if(e) kh_fatal(NULL,"fov-zoom-value-range: KHNMC_FOV_ZOOM=%s (allowed: 1.0 to 1.89; 1.0 = no zoom)",v);
        asp_zoom_tall=z; asp_zoom_set=1;
        if(!asp_mode){ fprintf(stderr,"kh: aspect: NOTE: fov-zoom-not-applied: KHNMC_FOV_ZOOM=%s is set, but KHNMC_ASPECT is pc: the zoom applies in fill mode only, nothing is zoomed\n",v); return; } }
    if(asp_mode) fprintf(stderr,"kh: aspect: zoom (task 49): %g on a screen taller than wide (%s), 1 = nothing written on every other screen; the camera stays the exe's build, "
                         "the drawing sites of f_19cfc0 get a copy with the matrices of the distance times the zoom\n",
                         asp_zoom_tall,asp_zoom_set?"KHNMC_FOV_ZOOM":"the default; KHNMC_FOV_ZOOM=1.0..1.89 changes it");
}
/* the zoom of a W x H screen: KHNMC_FOV_ZOOM (default 1.3) in fill mode when it is taller than wide, else exactly 1 */
double kh_aspect_zoom(double W,double H){ return (asp_mode==1&&W>0&&H>W)?asp_zoom_tall:1.0; }
static double gx_zoom(KCtx *g){ return kh_aspect_zoom(rdf(g,GX+0x18),rdf(g,GX+0x1c)); }
static int zoom_class(u64 cam,u64 ra){
    for(int k=0;k<ZOOM_NCAM;k++) if(cam==CAM_CUR+CAM_SIZE*(u64)k) return k;
    return ra==ZOOM_RA_MAPOBJ?ZC_MAPOBJ:ZC_OTHER;
}
/* the 62 call sites of f_19cfc0, by return address (0x140000000 + rva), and what their camera is used for (comment above) */
enum { ZS_DRAW=1, ZS_GAME=2, ZS_MODE=3 };
typedef struct { u32 rva; u8 cls; s8 slot; const char *what; } ZoomSite;
static const ZoomSite zsites[]={
    {0x16598e,ZS_GAME,-1,"f_165700 FOV set"},           {0x166719,ZS_DRAW,0,"f_1666f0 dk view-projection"},    {0x16740a,ZS_DRAW,1,"f_1673b0 dk::COMMAND_DRAW"},
    {0x17072b,ZS_DRAW,2,"f_170450 render frame"},       {0x176740,ZS_GAME,-1,"f_176710 camera framing"},       {0x177fbc,ZS_GAME,-1,"f_177f20 FOV save"},
    {0x178343,ZS_GAME,-1,"f_1780d0 FOV set"},           {0x178620,ZS_GAME,-1,"f_178580 lock-on limit x"},      {0x1786b7,ZS_GAME,-1,"f_178580 lock-on limit y"},
    {0x18695c,ZS_DRAW,3,"f_186920 map pass"},           {0x18ef29,ZS_DRAW,4,"f_18eee0 dk::ModelSprite"},       {0x18f2f5,ZS_DRAW,5,"f_18f240 dk::SpotBlend"},
    {0x1a17c1,ZS_GAME,-1,"f_1a1610 near plane"},        {0x1b5f25,ZS_GAME,-1,"f_1b5e30 kn on-screen"},         {0x1c8ef5,ZS_GAME,-1,"f_1c8d90 camera (distance)"},
    {0x1ca0c1,ZS_GAME,-1,"f_1c9f70 camera (distance)"}, {0x1cb3b5,ZS_GAME,-1,"f_1cb360 target search"},        {0x1e2598,ZS_MODE,-1,"gb f_1e2580 camera init"},
    {0x1e2c83,ZS_MODE,-1,"gb f_1e2c40"},                {0x1e3839,ZS_MODE,-1,"gb f_1e3820 camera init"},       {0x1f3e39,ZS_MODE,-1,"gb f_1f3e10 screen position"},
    {0x205d73,ZS_MODE,-1,"gb f_205d30"},                {0x20d7f0,ZS_MODE,-1,"gb f_20d760"},                   {0x21969c,ZS_MODE,-1,"gb f_219470 projector"},
    {0x2199cc,ZS_MODE,-1,"gb f_219900 projector"},      {0x219b3a,ZS_MODE,-1,"gb f_219900 projector"},         {0x24760d,ZS_MODE,-1,"gm f_247420"},
    {0x24818c,ZS_MODE,-1,"gm f_248160"},                {0x258a70,ZS_MODE,-1,"gm f_258930"},                   {0x25b3e5,ZS_MODE,-1,"gm f_25b010"},
    {0x25b461,ZS_MODE,-1,"gm f_25b010"},                {0x26e463,ZS_MODE,-1,"gm f_26e440 FOV"},               {0x271f40,ZS_MODE,-1,"gm f_271d00 copy"},
    {0x271f53,ZS_MODE,-1,"gm f_271d00 copy"},           {0x273950,ZS_MODE,-1,"gm::SPRITEFROM3D f_273820"},     {0x27cc3d,ZS_MODE,-1,"gm::GUMIOBJ f_27cbc0"},
    {0x282882,ZS_MODE,-1,"gm f_2827d0"},                {0x2bd4ec,ZS_DRAW,6,"f_2bd4c0 effect pass"},           {0x2d27d7,ZS_GAME,-1,"f_2d27a0 FOV"},
    {0x2d31e5,ZS_GAME,-1,"f_2d27a0 camera copy"},       {0x2e1513,ZS_MODE,-1,"Ti f_2e1110"},                   {0x38eb8d,ZS_GAME,-1,"f_38eb50 FOV get"},
    {0x38ec06,ZS_GAME,-1,"f_38ebc0 FOV get"},           {0x38ecf0,ZS_GAME,-1,"f_38ebc0 FOV set"},              {0x396c8b,ZS_GAME,-1,"f_396b80 camera pan"},
    {0x3b717e,ZS_DRAW,7,"f_3b7150 command menu fade"},  {0x3f3ef0,ZS_GAME,-1,"f_3f3ed0 YS on-screen"},          {0x411fb1,ZS_DRAW,8,"f_411f30 prize draw order"},
    {0x430ecb,ZS_GAME,-1,"f_430ea0 script syscall"},    {0x4381ab,ZS_GAME,-1,"f_438070 object picker"},        {0x543048,ZS_MODE,-1,"Ti f_543020"},
    {0x543ea7,ZS_MODE,-1,"Ti f_543e00"},                {0x5467f6,ZS_MODE,-1,"Ti f_546750"},                   {0x546b61,ZS_MODE,-1,"Ti f_546ab0"},
    {0x546d1f,ZS_MODE,-1,"Ti f_546c80"},                {0x547420,ZS_MODE,-1,"Ti f_547370"},                   {0x5481ce,ZS_MODE,-1,"Ti f_548130"},
    {0x548348,ZS_MODE,-1,"Ti f_548330"},                {0x549187,ZS_MODE,-1,"Ti f_549170"},                   {0x549254,ZS_MODE,-1,"Ti f_549240"},
    {0x5539a1,ZS_MODE,-1,"Ti f_553970"},                {0x554a60,ZS_MODE,-1,"Ti f_554a30"} };
#define ZOOM_NSITE ((int)(sizeof zsites/sizeof *zsites))
#define ZOOM_NDRAW 9
static int zoom_site(u64 ra){                                                         /* index into zsites, or -1 */
    if(ra<GS_BASE||ra-GS_BASE>0xffffffffull) return -1;
    u32 r=(u32)(ra-GS_BASE); int lo=0, hi=ZOOM_NSITE-1;
    while(lo<=hi){ int m=(lo+hi)/2; if(zsites[m].rva==r) return m; if(zsites[m].rva<r) lo=m+1; else hi=m-1; }
    return -1;
}
typedef struct { _Atomic u64 calls, identity, suppressed, other, edited[3], inside, differ, badcam,
                 s_calls, s_game, s_mode, s_draw, d_copy[2], d_z1, d_supp, d_nomatch, d_other, d_thread, s_unknown, site[64];
                 _Atomic u32 tid; _Atomic int warned, s_warned; } ZoomStat;
static ZoomStat zs;
/* the zoomed draw matrices of the last two builds of each scene camera ([k][0] the last), with the matrices of build 1 they belong to */
typedef struct { u8 pc_vp[0x40], pc_pr[0x40], z_vp[0x40], z_pr[0x40]; int valid; } ZoomEnt;
static ZoomEnt zent[ZOOM_NCAM][2];
/* per D site: the copy handed out (0x184 bytes, host memory given to the game by its guest address) */
#include "rewind.h"
static u8 local_shadow[ZOOM_NDRAW][0x190] __attribute__((aligned(64)));
static u8 (*zshadow)[0x190]=local_shadow;
static void zoom_storage_init(void){
    if(!kh_cut_enabled || !asp_mode || zshadow!=local_shadow) return;
    /* f_19cfc0 leaves interior pointers to the draw copy in guest memory. */
    u64 address=ga_alloc(sizeof local_shadow,64);
    if(!address) kh_fatal(NULL,"savestate-zoom-shadow-allocation");
    zshadow=(void *)(uintptr_t)(address+gs_bias);
    memset(zshadow,0,sizeof local_shadow);
}
#define ZOOM_GADDR(p) ((u64)((uintptr_t)(p)-gs_bias))
int kh_aspect_zoom_shadow(u64 a){                                                     /* a lies in a copy of zshadow (H2c, GA_OK's stand-in) */
    u64 lo=ZOOM_GADDR(zshadow[0]); return a>=lo&&a<lo+sizeof local_shadow;
}
/* the M sites: the game frame of the last call; the zoom is 1 in that frame and the next */
static _Atomic u64 zsup_frame; static _Atomic int zsup_have;
/* The guest may still hold a draw-camera pointer into zshadow at a cut. */
void kh_rewind_aspect(KhRecords *r){
    if(!asp_mode) return;
    KH_RECORD(r,zent);
    kh_record(r,zshadow,sizeof local_shadow);
    KH_RECORD(r,zsup_frame);
    KH_RECORD(r,zsup_have);
    KH_RECORD(r,last3d_plus1);
}
static int zoom_supp_at(u64 now,int have,u64 f){ return have&&now>=f&&now-f<=1; }
static int zoom_suppressed(void){ return zoom_supp_at(kh_game_frames(),atomic_load(&zsup_have),atomic_load(&zsup_frame)); }
typedef struct { u64 ra, n; } ZoomRa;
static ZoomRa zra[16]; static int n_zra; static u64 zra_cut;      /* builds of other cameras on a tall screen, by return address (under mm) */
static ZoomRa zua[8]; static int n_zua; static u64 zua_cut;       /* f_19cfc0 from an unknown return address (under mm) */
/* the bits written for a saved distance at zoom z */
static u32 zoom_value(u32 saved,double z){ return fbits((float)((double)bf(saved)*z)); }
static void zoom_push(int k,const u8 *pc,const u8 *zz){                               /* note build k's matrices (zz NULL: none zoomed) */
    zent[k][1]=zent[k][0];
    if(zz){ memcpy(zent[k][0].pc_vp,pc+CAM_VP,0x40); memcpy(zent[k][0].pc_pr,pc+CAM_PROJ,0x40); memcpy(zent[k][0].z_vp,zz+CAM_VP,0x40); memcpy(zent[k][0].z_pr,zz+CAM_PROJ,0x40); zent[k][0].valid=1; }
    else zent[k][0].valid=0;
}
/* KHNMC_ASPECT_LOG=1 or 2: the scene camera after f_19cba0's build of every 50th game frame: the distance cam+0x40, the FOV angles
 * cam+0x44 / +0x48, the view's eye / look-at point / roll (the view array of f_19ce40(1), 0x140ac1020 + 0x80 k: +0x48, +0x58, +0x78)
 * and FNV-1a hashes of all 0x184 bytes of the camera and the 0x80 bytes of its view: two runs that differ only in the zoom show with
 * these lines whether the game's camera stayed the same (task 49's route pairs) */
#define ZOOM_RA_FRAME  (GS_BASE+0x19cc81ull)       /* f_19cba0: return address of its f_19cad0 */
#define VIEW_CUR       (GS_BASE+0xac1020ull)

#define ZOOM_LOG_EVERY 50                           /* (run-G49/fix/diag builds a copy with 1: every game frame) */
#define ZOOM_LOG_MAX   600

static _Atomic u64 zoom_log_lines, zoom_log_cut;
static u64 zoom_fnv(const u8 *p,size_t n){ u64 h=0xcbf29ce484222325ull; for(size_t i=0;i<n;i++){ h^=p[i]; h*=0x100000001b3ull; } return h; }
static void zoom_log(KCtx *g,u64 cam,u64 ra){
    if(ra!=ZOOM_RA_FRAME) return;
    int k=cam==CAM_CUR?0:cam==CAM_CUR+CAM_SIZE?1:-1; if(k<0) return;
    u64 fr=kh_game_frames(); if(fr%ZOOM_LOG_EVERY) return;
    if(atomic_fetch_add(&zoom_log_lines,1)>=ZOOM_LOG_MAX){ zoom_log_cut++; return; }
    u64 v=VIEW_CUR+0x80ull*(u64)k;
    fprintf(stderr,"kh: aspect: zoom: camera %d at game frame %llu: distance %.9g, fov %.9g / %.9g; eye (%.9g, %.9g, %.9g), at (%.9g, %.9g, %.9g), roll %.9g; camera %016llx view %016llx\n",k,P(fr),
            rdf(g,cam+CAM_D),rdf(g,cam+0x44),rdf(g,cam+0x48),rdf(g,v+0x48),rdf(g,v+0x4c),rdf(g,v+0x50),rdf(g,v+0x58),rdf(g,v+0x5c),rdf(g,v+0x60),rdf(g,v+0x78),
            P(zoom_fnv(hp(g,cam),CAM_SIZE)),P(zoom_fnv(hp(g,v),0x80)));
}
/* f_19cad0(cam = guest rcx) with the lifted body as a parameter (the selftest passes a stand-in) */
static void zoom_build(KCtx *g,void (*body)(KCtx *)){
    u64 cam=KH_RCX(g), ra=rd64(g,KH_RSP(g));
    zs.calls++;
    double z=gx_zoom(g); int k=zoom_class(cam,ra);
    if(z==1.0){ zs.identity++; if(k<ZOOM_NCAM) zoom_push(k,NULL,NULL);              /* not taller than wide, or the value 1: nothing is written */
        body(g); if(__builtin_expect(kh_aspect_log,0)) zoom_log(g,cam,ra); return; }
    if(k==ZC_OTHER){ zs.other++;
        pthread_mutex_lock(&mm); int i; for(i=0;i<n_zra;i++) if(zra[i].ra==ra) break;
        if(i==n_zra){ if(n_zra<16){ zra[i].ra=ra; zra[i].n=0; n_zra++; } else zra_cut++; }
        if(i<16) zra[i].n++; pthread_mutex_unlock(&mm);
        body(g); return; }
    if(zoom_suppressed()){ zs.suppressed++; if(k<ZOOM_NCAM) zoom_push(k,NULL,NULL);   /* a play mode of the M sites runs: the exe's build only */
        body(g); if(__builtin_expect(kh_aspect_log,0)) zoom_log(g,cam,ra); return; }
    u64 sp=KH_RSP(g);
    if(!cam||!GA_OK(cam)||!GA_OK(cam+CAM_SIZE-1)||!GA_OK(sp-ZOOM_STACK)||!GA_OK(sp-1)){ zs.badcam++;
        if(!atomic_exchange(&zs.warned,1)) fprintf(stderr,"kh: aspect: WARNING: zoom: camera 0x%llx or the stack below 0x%llx (return address 0x%llx) is not guest memory: not zoomed (counted)\n",P(cam),P(sp),P(ra));
        if(k<ZOOM_NCAM) zoom_push(k,NULL,NULL);
        body(g); return; }
    { u32 t0=0; atomic_compare_exchange_strong(&zs.tid,&t0,cur_tid()); }
    static __thread u8 st0[KH_ST_SIZE] __attribute__((aligned(64))), st1[KH_ST_SIZE] __attribute__((aligned(64))), S1[CAM_SIZE], S2[CAM_SIZE], stk[ZOOM_STACK];
    memcpy(st0,g->st,KH_ST_SIZE);
    body(g);                                                                          /* 1: the exe's build */
    memcpy(st1,g->st,KH_ST_SIZE); memcpy(S1,hp(g,cam),CAM_SIZE); memcpy(stk,hp(g,sp-ZOOM_STACK),ZOOM_STACK);
    u32 d=rd32(g,cam+CAM_D), w=zoom_value(d,z);
    memcpy(g->st,st0,KH_ST_SIZE); wr32(g,cam+CAM_D,w);
    body(g);                                                                          /* 2: the same call with the distance d * z */
    memcpy(S2,hp(g,cam),CAM_SIZE);
    memcpy(hp(g,cam),S1,CAM_SIZE); memcpy(hp(g,sp-ZOOM_STACK),stk,ZOOM_STACK); memcpy(g->st,st1,KH_ST_SIZE);   /* all as build 1 left it */
    if(__builtin_expect(kh_aspect_log,0)) zoom_log(g,cam,ra);                        /* the camera as the game now has it */
    if(memcmp(S2+CAM_D,&w,4)){ zs.inside++; if(k<ZOOM_NCAM) zoom_push(k,NULL,NULL);
        if(!atomic_exchange(&zs.warned,1)) fprintf(stderr,"kh: aspect: WARNING: zoom: cam+0x40 of camera 0x%llx changed inside f_19cad0 (we wrote 0x%08x): no zoomed matrices noted, the exe's build stands (counted)\n",P(cam),w);
        return; }
    if(memcmp(S2+0x44,S1+0x44,0x84-0x44)||memcmp(S2+0x144,S1+0x144,CAM_SIZE-0x144)){ zs.differ++;   /* fields without d: the same in both builds */
        if(!atomic_exchange(&zs.warned,1)) fprintf(stderr,"kh: aspect: WARNING: zoom: the zoomed build of camera 0x%llx differs from the exe's in a field that has no distance in it (counted; its draw matrices are used all the same)\n",P(cam)); }
    if(k<ZOOM_NCAM){ zoom_push(k,S1,S2); zs.edited[k]++; return; }
    memcpy(hp(g,cam+CAM_VP),S2+CAM_VP,0x40); memcpy(hp(g,cam+CAM_PROJ),S2+CAM_PROJ,0x40); zs.edited[ZC_MAPOBJ]++;   /* the map pass's own stack camera: drawn only */
}
void orig_f_19cad0(KCtx *);
void f_19cad0(KCtx *g){
    if(__builtin_expect(!asp_mode,1)){ orig_f_19cad0(g); return; }                 /* nothing set or pc: exactly the exe */
    zoom_build(g,orig_f_19cad0);
}
/* f_19cfc0 returned the camera in rax (fill mode), called from return address ra */
static void zoom_cam_post(KCtx *g,u64 ra){
    zs.s_calls++;
    int i=zoom_site(ra);
    if(i<0){ zs.s_unknown++;
        pthread_mutex_lock(&mm); int j; for(j=0;j<n_zua;j++) if(zua[j].ra==ra) break;
        if(j==n_zua){ if(n_zua<8){ zua[j].ra=ra; zua[j].n=0; n_zua++; } else zua_cut++; }
        if(j<8) zua[j].n++; pthread_mutex_unlock(&mm);
        if(!atomic_exchange(&zs.s_warned,1)) fprintf(stderr,"kh: aspect: WARNING: zoom-camera-site-unknown: f_19cfc0 called from return address 0x%llx, none of its 62 call sites: the camera as the exe built it is returned (counted)\n",P(ra));
        return; }
    zs.site[i]++;
    if(zsites[i].cls==ZS_GAME){ zs.s_game++; return; }
    if(zsites[i].cls==ZS_MODE){ zs.s_mode++; atomic_store(&zsup_frame,kh_game_frames()); atomic_store(&zsup_have,1); return; }
    zs.s_draw++;
    if(gx_zoom(g)==1.0){ zs.d_z1++; return; }
    if(zoom_suppressed()){ zs.d_supp++; return; }
    u64 cam=KH_RAX(g); int k=-1, pv=0;
    for(int j=0;j<ZOOM_NCAM;j++){ if(cam==CAM_CUR+CAM_SIZE*(u64)j) k=j; else if(cam==CAM_PREV+CAM_SIZE*(u64)j){ k=j; pv=1; } }
    if(k<0){ zs.d_other++; return; }
    const u8 *c=hp(g,cam);
    for(int e=0;e<2;e++){ const ZoomEnt *t=&zent[k][e];
        if(!t->valid||memcmp(c+CAM_VP,t->pc_vp,0x40)||memcmp(c+CAM_PROJ,t->pc_pr,0x40)) continue;
        u8 *s=zshadow[(int)zsites[i].slot]; memcpy(s,c,CAM_SIZE); memcpy(s+CAM_VP,t->z_vp,0x40); memcpy(s+CAM_PROJ,t->z_pr,0x40);
        { u32 t0=atomic_load(&zs.tid); if(t0&&t0!=cur_tid()) zs.d_thread++; }
        KH_RAX(g)=ZOOM_GADDR(s); zs.d_copy[pv]++; return; }
    zs.d_nomatch++;                                                                   /* built at zoom 1 (or before any zoomed build): drawn as it is */
}
void orig_f_19cfc0(KCtx *);
void f_19cfc0(KCtx *g){
    if(__builtin_expect(!asp_mode,1)){ orig_f_19cfc0(g); return; }                 /* nothing set or pc: exactly the exe */
    u64 ra=rd64(g,KH_RSP(g));
    orig_f_19cfc0(g);
    zoom_cam_post(g,ra);
}
static void zoom_report(FILE *f){
    fprintf(f,"kh: aspect report: zoom (task 49; 0x19cad0, the camera matrix build): %g on a screen taller than wide (%s); builds %llu: zoom 1, nothing written %llu; "
              "a play mode ran (M sites), the exe's build only %llu; built twice, the camera kept as the exe built it and the zoomed draw matrices noted: scene camera 0x140ac1840 %llu, 0x140ac19c4 %llu; "
              "the map pass's per-object copy (return address 0x140188312), draw matrices zoomed %llu; other cameras (object-owned), not zoomed %llu; "
              "cam+0x40 changed inside the zoomed build %llu; a field without the distance differs %llu; camera or stack not guest memory %llu; first guest thread %u\n",
            asp_zoom_tall,asp_zoom_set?"KHNMC_FOV_ZOOM":"default",P(zs.calls),P(zs.identity),P(zs.suppressed),P(zs.edited[ZC_SCENE0]),P(zs.edited[ZC_SCENE1]),P(zs.edited[ZC_MAPOBJ]),
            P(zs.other),P(zs.inside),P(zs.differ),P(zs.badcam),atomic_load(&zs.tid));
    fprintf(f,"kh: aspect report: zoom: camera accessor 0x19cfc0: calls %llu: game logic and other readers (G), the camera %llu; play modes (M: gummi ship, garage, minigames) %llu; "
              "drawing sites (D) %llu: a zoomed copy %llu (of a previous-frame copy %llu), zoom 1 %llu, a play mode ran %llu, no noted build matches %llu, not a scene camera %llu; "
              "copies on another guest thread than the builds %llu; unknown return address %llu\n",
            P(zs.s_calls),P(zs.s_game),P(zs.s_mode),P(zs.s_draw),P(zs.d_copy[0]+zs.d_copy[1]),P(zs.d_copy[1]),P(zs.d_z1),P(zs.d_supp),P(zs.d_nomatch),P(zs.d_other),P(zs.d_thread),P(zs.s_unknown));
    fprintf(f,"kh: aspect report: zoom: camera accessor by call site:"); int any=0;
    for(int i=0;i<ZOOM_NSITE;i++) if(zs.site[i]){ fprintf(f,"%s 0x%llx %c %s %llu",any?";":"",P(GS_BASE+zsites[i].rva),zsites[i].cls==ZS_DRAW?'D':zsites[i].cls==ZS_MODE?'M':'G',zsites[i].what,P(zs.site[i])); any=1; }
    fprintf(f,"%s\n",any?"":" none");
    pthread_mutex_lock(&mm);
    if(n_zra){ fprintf(f,"kh: aspect report: zoom: camera builds NOT zoomed on a tall screen, by return address:"); for(int i=0;i<n_zra;i++) fprintf(f," 0x%llx x%llu",P(zra[i].ra),P(zra[i].n));
        if(zra_cut) fprintf(f," (+%llu calls from further return addresses: table full)",P(zra_cut)); fputc('\n',f); }
    if(n_zua){ fprintf(f,"kh: aspect: WARNING: zoom: f_19cfc0 from unknown return addresses (the camera was returned):"); for(int i=0;i<n_zua;i++) fprintf(f," 0x%llx x%llu",P(zua[i].ra),P(zua[i].n));
        if(zua_cut) fprintf(f," (+%llu calls from further return addresses)",P(zua_cut)); fputc('\n',f); }
    pthread_mutex_unlock(&mm);
    if(zs.inside||zs.differ||zs.badcam||zs.d_thread) fprintf(f,"kh: aspect: WARNING: zoom: %llu builds changed cam+0x40 inside, %llu zoomed builds differ in a field without the distance, %llu cameras or stacks not guest memory, %llu copies on another thread\n",
                                                  P(zs.inside),P(zs.differ),P(zs.badcam),P(zs.d_thread));
    if(kh_aspect_log) fprintf(f,"kh: aspect report: zoom: camera lines (every %d game frames, KHNMC_ASPECT_LOG): %llu printed, %llu not printed (%d at most)\n",ZOOM_LOG_EVERY,P(atomic_load(&zoom_log_lines)-zoom_log_cut),P(zoom_log_cut),ZOOM_LOG_MAX);
}

/* ==== task 53: the HUD scale of the tall (portrait) screen =========================================================================
 *
 * In fill mode the exe lays its 2D out in a 16:9 band as wide as the screen; on the portrait phone (2007x2853) that band is 1129 of
 * 2853 rows and the HUD, sized for a 2007-wide 16:9 screen, looks tiny.  On a screen TALLER THAN WIDE (fill mode only; exactly 1 on
 * every other screen, in pc and with nothing set) H3 (task 42, below) scales two HUD widgets about their own outer corner:
 *   the command menu (dk groups 1 and 2: dk::COMMAND_DRAW and its children, COMMAND_ONE, the reaction command dk::ReactionDecideSprite,
 *       and the sub-menus the command menu's code registers with group 1) by KHNMC_HUD_SCALE_CMD, default 1.25, about its outer
 *       BOTTOM-LEFT corner;
 *   the player gauges (dk group 3: GAUGE_PLAYER, the HP / MP / Drive bars dk::GAUGE_ONE, the face ring dk::CIRCLE_MASK, the face
 *       dk::FACE, dk::SECTOR_MASK, their dk::Sprite children; and the party gauges, groups 4 / 5, which the exe stacks over them in the
 *       same corner: scaled about the same point they keep their place against the player's gauge) by KHNMC_HUD_SCALE_HP, default 1.5,
 *       about their outer BOTTOM-RIGHT corner.
 * How: every HUD draw uses vertex shader 586160, pixel = (POSITION - xyOffset) * MS.xy + MS.zw (586160.asm: `mad r0.xy, r0.xyxx,
 * cb2[2].xyxx, cb2[2].zwzz`, then `round_pi`), with MagicScale MS = GX+0xa0..0xac staged by f_1121d0 at every draw, and the D3D12
 * scissor of a draw is mapped with the same MS (f_142c00: x * [r8+0x10] + [r8+0x18], y * [r8+0x14] + [r8+0x1c], r8 = GX+0x90,
 * 0x140142cde..0x140142d1b).  H3 already writes MS.w per dk object; for an object of a scaled group that H3 anchors to the BOTTOM it
 * writes all four: MS' = (s MS.x, s MS.y, MS.z + (1 - s) px MS.x, MS.w_bottom + (1 - s) py MS.y), i.e. the object's every vertex, its
 * text and its scissor are scaled by s about the GS point (px, py), which keeps its pixel.  Any other object of such a pass gets the
 * loop's MS.x / .y / .z back (and its MS.w as H3 decides), so nothing outside the two widgets changes.
 * The pivots are constants (GS frame = POSITION - xyOffset), the widgets' resting outer corners measured in the draw logs
 * (implementation notes: base binary, default route, 2007x2853 fill, presents 1000..1403, field and battle): the command menu's slots span
 * x -70..82 and its last slot ends at y 416 (the band's last row: on the PC it sits on the screen's bottom edge), so (-70, 416); the
 * face ring dk::CIRCLE_MASK spans x 490..590, y 305..405 (the outermost piece of the gauges: GAUGE_ONE 377..585 x 310..400.5, FACE
 * 504..574 x 305..400), so (590, 405).  A pivot taken from the widgets' extent per frame would move with their slide-in / slide-out and
 * shake animations (M: CIRCLE_MASK x 490..790 over the route) and the HUD would wobble; fixed, the animation is scaled with the widget.
 * Margins stay as the PC has them (the pivot is the widget's own corner): no piece can be pushed off the frame by s >= 1 (it grows up
 * and inward); the largest s before the resting box would pass the opposite frame edge on ANY screen taller than wide (frame x -85.33
 * .. 597.33, y -b .. 416 + b, b > 161.7) is 3.17 for the gauges' width, more for the rest, above the allowed 3.0 (selftest).
 * KHNMC_HUD_SCALE_CMD / KHNMC_HUD_SCALE_HP: a plain decimal 0.5 .. 3.0 (the grammar of KHNMC_FOV_ZOOM); 1.0 = exactly today (nothing
 * written); malformed or outside the range: the named fatals hud-scale-value-malformed / hud-scale-value-range; set with KHNMC_ASPECT
 * pc or unset: checked, the NOTE hud-scale-not-applied, nothing changes. */
#define HUD_CMD_DEFAULT 1.25                       /* owner 2026-11-18: "the command menu 25% bigger" */
#define HUD_HP_DEFAULT  1.5                        /* owner 2026-11-18: "hp portrait thing 50% bigger" */
enum { HUD_CMD=0, HUD_HP=1, HUD_N };
static double hud_s[HUD_N]={HUD_CMD_DEFAULT,HUD_HP_DEFAULT}; static int hud_set[HUD_N];
static const char *hud_env[HUD_N]={"KHNMC_HUD_SCALE_CMD","KHNMC_HUD_SCALE_HP"};
static const char *hud_what[HUD_N]={"command menu (dk groups 1, 2)","player gauges (dk groups 3, 4, 5)"};
static const float hud_px[HUD_N]={-70.0f,590.0f}, hud_py[HUD_N]={416.0f,405.0f};       /* the pivots, GS frame (measured: above) */
/* a KHNMC_HUD_SCALE_* value: DIGITS[.DIGITS], at most 15 digits, nothing else.  0 = *v set, 1 = malformed, 2 = outside 0.5 .. 3.0 */
static int decimal_range(const char *s,double *v,double lo,double hi){
    u64 n=0, den=1; int nd=0, ni=0, nf=0, dot=0;
    if(!s) return 1;
    for(const char *p=s;*p;p++){
        if(*p=='.'){ if(dot||!ni) return 1; dot=1; continue; }
        if(*p<'0'||*p>'9') return 1;
        if(++nd>15) return 1;
        n=n*10+(u64)(*p-'0'); if(dot){ den*=10; nf++; } else ni++; }
    if(!ni||(dot&&!nf)) return 1;
    double x=(double)n/(double)den;
    if(!(x>=lo&&x<=hi)) return 2;
    *v=x; return 0;
}
int kh_hud_scale_parse(const char *s,double *v){ return decimal_range(s,v,0.5,3.0); }
static double style_scale=2,style_raise=0.25,enemy_scale=1.5,sub_scale=1.5,sub_lower=0.06;
static int menu_band;
static void portrait_number(const char *key,double *value,double lo,double hi){
    const char *s=getenv(key);
    if(!s || !*s) return;
    int e=decimal_range(s,value,lo,hi);
    if(e) kh_fatal(NULL,"portrait-value-%s: %s=%s (plain decimal %g..%g)",e==1?"malformed":"range",key,s,lo,hi);
}
static void portrait_config(void){
    portrait_number("KHNMC_HUD_SCALE_STYLE",&style_scale,0.5,3);
    portrait_number("KHNMC_HUD_STYLE_RAISE",&style_raise,0,0.5);
    portrait_number("KHNMC_HUD_SCALE_ENEMY",&enemy_scale,0.5,3);
    portrait_number("KHNMC_SUB_SCALE",&sub_scale,0.5,3);
    portrait_number("KHNMC_SUB_LOWER",&sub_lower,0,0.5);
    const char *s=getenv("KHNMC_MENU_LAYOUT");
    menu_band=s && !strcmp(s,"band");
    if(s && *s && !menu_band && strcmp(s,"portrait")) kh_fatal(NULL,"menu-layout-value: KHNMC_MENU_LAYOUT=%s (known: portrait, band)",s);
}
void kh_aspect_hud_config(const char *cmd,const char *hpv){
    const char *v[HUD_N]={cmd,hpv};
    for(int k=0;k<HUD_N;k++) if(v[k]&&*v[k]){ double s=0; int e=kh_hud_scale_parse(v[k],&s);
        if(e==1) kh_fatal(NULL,"hud-scale-value-malformed: %s=%s (expected a plain decimal number from 0.5 to 3.0, such as 1.25)",hud_env[k],v[k]);
        if(e) kh_fatal(NULL,"hud-scale-value-range: %s=%s (allowed: 0.5 to 3.0; 1.0 = the size of today)",hud_env[k],v[k]);
        hud_s[k]=s; hud_set[k]=1; }
    if(!asp_mode){ if(hud_set[HUD_CMD]||hud_set[HUD_HP]) fprintf(stderr,"kh: aspect: NOTE: hud-scale-not-applied: %s%s%s is set, but KHNMC_ASPECT is pc: the HUD scale applies in fill mode only, nothing is scaled\n",
                                                             hud_set[HUD_CMD]?hud_env[HUD_CMD]:"",hud_set[HUD_CMD]&&hud_set[HUD_HP]?" / ":"",hud_set[HUD_HP]?hud_env[HUD_HP]:""); return; }
    fprintf(stderr,"kh: aspect: HUD scale (task 53): command menu %g (%s), player gauges %g (%s) on a screen taller than wide, about their outer corners; 1 = nothing written; every other screen: 1\n",
            hud_s[HUD_CMD],hud_set[HUD_CMD]?hud_env[HUD_CMD]:"default",hud_s[HUD_HP],hud_set[HUD_HP]?hud_env[HUD_HP]:"default");
}
/* the HUD scale of kind k (HUD_CMD / HUD_HP) on a W x H screen: the configured value in fill mode when it is taller than wide, else 1 */
double kh_aspect_hud_scale(int k,double W,double H){ return (asp_mode==1&&W>0&&H>W&&k>=0&&k<HUD_N)?hud_s[k]:1.0; }
static int hud_kind(u32 grp){ return grp==1||grp==2?HUD_CMD:grp>=3&&grp<=5?HUD_HP:-1; }
/* MagicScale for an object scaled by s about the GS point (px, py): ms = the loop's MS.x / .y / .z bits and the anchored MS.w bits in,
 * the scaled four out (single precision, as MS is stored) */
static void hud_ms(u32 *ms,double s,float px,float py){
    double x=bf(ms[0]), y=bf(ms[1]), z=bf(ms[2]), w=bf(ms[3]);
    ms[0]=fbits((float)(s*x)); ms[1]=fbits((float)(s*y)); ms[2]=fbits((float)(z+(1.0-s)*(double)px*x)); ms[3]=fbits((float)(w+(1.0-s)*(double)py*y));
}
/* ---- task 53 item 2: the title screen's layout on a screen taller than wide (KHNMC_TITLE_LAYOUT=portrait|band, default portrait)
 * The title is drawn in the dk loop by two Tz::Layout objects the title code keeps in static pointers: the title layout
 * [0x140b1d5d0] (created at 0x1402b467b..0x1402b46b5: 0xf8 bytes, vtable Tz::Layout 0x1405b1dc0) and the title menu's layout
 * [0x140b1d5d8] (0x1402b448a..0x1402b44c5, the same class; runtime/route_state.c reads both for the title states).  Their parts come
 * through f_11a330 from the layout draw f_1969a0 (return addresses 0x140197033 / 0x14019701a, and 0x1401977e3 for the menu's glow
 * strips).  In the 16:9 band (GS 0..416) they are composed for a wide screen: the alley picture at the right (x 195..595, y 0..400:
 * its top is a straight cut that meets the screen's top edge; task 46's R3 moves it to the frame's top), the logo block at the left
 * (KINGDOM HEARTS x -77..298 y 83..194, the heart 11..206 x 52..275, the Disney / Square Enix line 10..205 x 37..69, the crown, the
 * Nobody May Cry line -5..247 x 154..218, the credits and a sweep line 253..293; centre x 110.5), the menu rows NEW GAME / LOAD /
 * BACK (y 295..402, bars across the width), GAME SETTINGS at the top right (344..597 x 14..36).  On a tall frame (GS y -b .. 416 + b,
 * x -85.33 .. 597.33) that leaves the picture in a top corner, everything else in the middle band and a third of the screen empty.
 * Portrait layout: each part of those two objects is classified by its GS box (as submitted, ttl_elem) and moved as its group:
 *   picture  scaled by sP about its top centre (395, 0) to the frame's horizontal centre (256) and top row (-b): its cut top edge stays
 *            on the screen's edge (a gap above it would show that edge as a hard line: task 46 review finding 1), its faded sides
 *            and rounded bottom are all on screen;  sP = 1.35 (540 GS columns of 682.67: a margin of 71 each side), less when the
 *            screen is too short for the stack below (never less than 1: then the band layout is kept, counted);
 *   menu     NEW GAME / LOAD / BACK and GAME SETTINGS moved down (x unchanged: the bars run across the screen) so that GAME SETTINGS
 *            ends TTL_MB GS rows above the frame's bottom, the menu 18 rows above it;
 *   logo     scaled by sL = 1.1 about its top centre (110.5, 37) to the frame's centre column, centred between the picture and the
 *            menu.
 *   Everything else (the backdrops, which H7's R1 stretches; the fade-in flashes; the New Game dialogs, which are other dk objects
 *   (dk::SpriteMessage, Tz::MenuItem) and are already centred on the screen: the band is) keeps today's place.  The parts are drawn
 *   at these places in every frame they are drawn, so the composition stays put through the fades, the menu's opening and the
 *   exit animation.  KHNMC_TITLE_LAYOUT=band keeps today's placement (R3 for the picture); other values: the named fatal
 *   title-layout-value; with KHNMC_ASPECT pc / unset: checked, NOTE title-layout-not-applied. */
int kh_phase_title_alive(void); void kh_phase_test_set_alive(int initial,int title,int newgame);   /* runtime/phase_log.c: the title task 0x2b4590 runs */
#define T_LAYOUT_PTR (GS_BASE+0xb1d5d0ull)         /* the title layout object (runtime/route_state.c T_LAYOUT) */
#define T_MENU_PTR   (GS_BASE+0xb1d5d8ull)         /* the title menu's layout object (T_MENU) */
#define TTL_SP_MAX 1.35                            /* the picture's scale */
#define TTL_SL     1.1                             /* the logo's scale */
#define TTL_MB     60.0                            /* GS rows below GAME SETTINGS (163 px at 2007x2853) */
#define TTL_GAP_MS 18.0                            /* GS rows between the menu and GAME SETTINGS */
#define TTL_GAP_MIN 12.0                           /* the least gap above and below the logo */
enum { TT_PIC=0, TT_LOGO, TT_MENU, TT_SET, TT_N };
static const char *tt_name[TT_N]={"the alley picture","the logo","the title menu (NEW GAME / LOAD / BACK)","GAME SETTINGS"};
/* each element's anchor point P (GS, as the exe lays it out) and block height */
static const double tt_px[TT_N]={395.0,110.5,0.0,0.0}, tt_py[TT_N]={0.0,37.0,295.0,14.0}, tt_h[TT_N]={400.0,247.0,107.0,22.0};
static int asp_title_band, asp_title_set;          /* KHNMC_TITLE_LAYOUT=band / set */
void kh_aspect_title_config(const char *v){
    if(v&&*v){ if(!strcmp(v,"portrait")) asp_title_band=0; else if(!strcmp(v,"band")) asp_title_band=1;
        else kh_fatal(NULL,"title-layout-value: KHNMC_TITLE_LAYOUT=%s (known: portrait, band)",v);
        asp_title_set=1;
        if(!asp_mode){ fprintf(stderr,"kh: aspect: NOTE: title-layout-not-applied: KHNMC_TITLE_LAYOUT=%s is set, but KHNMC_ASPECT is pc: the title layout applies in fill mode only, nothing is moved\n",v); return; } }
    if(asp_mode) fprintf(stderr,"kh: aspect: title layout (task 53): %s on a screen taller than wide (%s); every other screen: the band\n",asp_title_band?"band (today's placement)":"portrait",
                         asp_title_set?"KHNMC_TITLE_LAYOUT":"the default; KHNMC_TITLE_LAYOUT=band keeps today's");
}
/* the portrait layout of a W x H screen: 0 = none (not fill, not taller than wide, band chosen, or too short: *tooshort = 1);
 * else per element the scale s[e] and the target T (tx, ty) of its anchor point */
typedef struct { double s[TT_N], tx[TT_N], ty[TT_N]; } TtLay;
int kh_aspect_title_layout(double W,double H,TtLay *L,int *tooshort){
    if(tooshort) *tooshort=0;
    if(asp_mode!=1||!(W>0&&H>W)||asp_title_band) return 0;
    double b=kh_aspect_b(W,H), yT=-b, yB=416.0+b;
    double setTop=yB-TTL_MB-tt_h[TT_SET], menuTop=setTop-TTL_GAP_MS-tt_h[TT_MENU];
    double sP=(menuTop-yT-tt_h[TT_LOGO]*TTL_SL-2.0*TTL_GAP_MIN)/tt_h[TT_PIC]; if(sP>TTL_SP_MAX) sP=TTL_SP_MAX;
    if(!(sP>=1.0)){ if(tooshort) *tooshort=1; return 0; }
    double picBot=yT+tt_h[TT_PIC]*sP, gap=(menuTop-picBot-tt_h[TT_LOGO]*TTL_SL)/2.0;
    L->s[TT_PIC]=sP;      L->tx[TT_PIC]=256.0;     L->ty[TT_PIC]=yT;
    L->s[TT_LOGO]=TTL_SL; L->tx[TT_LOGO]=256.0;    L->ty[TT_LOGO]=picBot+gap;
    L->s[TT_MENU]=1.0;    L->tx[TT_MENU]=0.0;      L->ty[TT_MENU]=menuTop;
    L->s[TT_SET]=1.0;     L->tx[TT_SET]=0.0;       L->ty[TT_SET]=setTop;
    return 1;
}
/* a part of the title's layouts by its GS box bb = x0, x1, y0, y1 (as submitted): TT_* or -1 (left where it is) */
int kh_aspect_title_elem(const float *bb){
    double x0=bb[0], x1=bb[1], y0=bb[2], y1=bb[3], cx=(x0+x1)/2.0, cy=(y0+y1)/2.0;
    if(!(x1>=x0&&y1>=y0)) return -1;
    if(x0>=194.0&&x0<=196.0&&x1>=594.0&&x1<=596.0&&y0>=-1.0&&y0<=1.0&&y1>=399.0&&y1<=401.0) return TT_PIC;   /* the 400 x 400 picture */
    if(x0>=330.0&&x1<=610.0&&y0>=5.0&&y1<=45.0) return TT_SET;                        /* GAME SETTINGS (344..597 x 14..36) */
    if(y0>=288.0&&y1-y0<=140.0) return TT_MENU;                                       /* the rows 295..402, their glow, the cursor; sliding down in the exit animation (to 447) */
    if(y1-y0<=420.0&&cy>=30.0&&cy<=290.0&&cx>=-600.0&&cx<=360.0) return TT_LOGO;       /* the logo block, its credits and sweep line (sliding out to the left in the exit animation, x -413) and its exit flash; not the backdrops (479 tall) nor the fade-in flashes (511 / 639) */
    return -1;
}
/* f_1969a0 submits the credit rows separately and its underline as two joined quads.
 * The exit animation slides them left independently of the rest of the logo. */
static double title_logo_pivot(const float b[4]){
    float w=b[1]-b[0], h=b[3]-b[2];
    if(b[2]>=253 && b[3]<=293){
        if((w==160 && h==12) || (w==170 && h==15)) return (b[0]+b[1])*0.5;
        if(h==2 && w==95) return b[0]+11.5; /* 72 pixels precede this piece */
        if(h==2 && w==72) return b[0]+83.5;
    }
    return tt_px[TT_LOGO];
}
typedef struct { _Atomic u64 batches, q[TT_N], left, other, tooshort; Ivs fr[TT_N]; } TtStat;
static TtStat ttl;
static _Atomic int ttl_logged;
static void ttl_report(FILE *f){
    fprintf(f,"kh: aspect report: title layout (task 53; KHNMC_TITLE_LAYOUT=%s): batches of the title's two layout objects on a screen taller than wide %llu; quads moved: picture %llu, logo %llu, menu %llu, GAME SETTINGS %llu; "
              "their quads left where they are (unclassified: backdrops, flashes) %llu; Tz::Layout quads of other dk objects while the title runs (left) %llu; batches on a screen too short for the layout (band kept) %llu\n",
            asp_title_band?"band":"portrait",P(ttl.batches),P(ttl.q[TT_PIC]),P(ttl.q[TT_LOGO]),P(ttl.q[TT_MENU]),P(ttl.q[TT_SET]),P(ttl.left),P(ttl.other),P(ttl.tooshort));
    pthread_mutex_lock(&mm);
    for(int e=0;e<TT_N;e++) if(ttl.q[e]){ fprintf(f,"kh: aspect report: title layout: %s:",tt_name[e]); iv_print(f,&ttl.fr[e]); fputc('\n',f); }
    pthread_mutex_unlock(&mm);
}
typedef struct { _Atomic u64 passes, objs[HUD_N], back; } HudStat;
static HudStat hud;
static void hud_report(FILE *f){
    fprintf(f,"kh: aspect report: HUD scale (task 53; H3's MagicScale per dk object): on a screen taller than wide the %s x%g about GS (%g, %g), the %s x%g about GS (%g, %g); "
              "dk passes with a scale %llu; objects scaled: command menu %llu, player gauges %llu; other objects of those passes given the loop's MagicScale.x / .y / .z back %llu\n",
            hud_what[HUD_CMD],hud_s[HUD_CMD],hud_px[HUD_CMD],hud_py[HUD_CMD],hud_what[HUD_HP],hud_s[HUD_HP],hud_px[HUD_HP],hud_py[HUD_HP],P(hud.passes),P(hud.objs[HUD_CMD]),P(hud.objs[HUD_HP]),P(hud.back));
}

/* ---- H4 ---------------------------------------------------------------------------------------------------------------------- */
typedef struct { u64 obj; const char *what; int kind; s16 y0,h0; } H4Obj;      /* kind 2 = whole screen, 1 = top bar, 0 = bottom bar */
static const H4Obj h4o[7]={
    {GS_BASE+0xabb3b0ull,"fade 0x156940 (obj 0x140abb3b0)",2,-1,0x1a2},{GS_BASE+0xabb720ull,"fade 0x1569b0 (obj 0x140abb720)",2,-1,0x1a2},
    {GS_BASE+0xabb290ull,"fade 0x156a70 (obj 0x140abb290)",2,-1,0x1a2},
    {GS_BASE+0xabb170ull,"event bar top (obj 0x140abb170)",1,-1,0x24},{GS_BASE+0xabb600ull,"event bar top 2 (obj 0x140abb600)",1,-1,0x24},
    {GS_BASE+0xabb050ull,"event bar bottom (obj 0x140abb050)",0,0x17d,0x24},{GS_BASE+0xabb4e0ull,"event bar bottom 2 (obj 0x140abb4e0)",0,0x17d,0x24}};
static int h4_index(u64 o){ for(int i=0;i<7;i++) if(h4o[i].obj==o) return i; return -1; }
/* the rect at B extra GS rows on each side for the exe's rect of object kind `kind` */
static void h4_target(int kind,int B,s16 y0,s16 h0,s16 *y,s16 *h){
    *y=(s16)(kind==0 ? y0+B : y0-B);
    *h=(s16)(kind==2 ? h0+2*B : h0);
}
typedef struct { _Atomic u64 draws, rewritten, already, other, setter_rewritten, setter_other; s16 oy[4],oh[4]; int no; } H4Stat;
static H4Stat h4s[7]; static _Atomic u64 h4_e20_calls, h4_off, h4_identity;
/* 0 = B is 0 (nothing written), 1 = rewritten, 2 = already the extended rect, 3 = another rect (left) */
static int h4_apply(KCtx *g,int i,int B){
    const H4Obj *o=&h4o[i]; s16 y=(s16)rd16(g,o->obj+0xe), h=(s16)rd16(g,o->obj+0xa), ty, th;
    if(!B) return 0;
    h4_target(o->kind,B,o->y0,o->h0,&ty,&th);
    if(y==o->y0&&h==o->h0){ wr16(g,o->obj+0xe,(u16)ty); wr16(g,o->obj+0xa,(u16)th); return 1; }
    if(y==ty&&h==th) return 2;
    pthread_mutex_lock(&mm); H4Stat *s=&h4s[i]; int k; for(k=0;k<s->no;k++) if(s->oy[k]==y&&s->oh[k]==h) break; if(k==s->no&&s->no<4){ s->oy[k]=y; s->oh[k]=h; s->no++; } pthread_mutex_unlock(&mm);
    return 3;
}
void orig_f_156f10(KCtx *); void orig_f_156e20(KCtx *);
void f_156f10(KCtx *g){
    if(__builtin_expect(!asp_mode&&!kh_aspect_m,1)){ orig_f_156f10(g); return; }
    int i=h4_index(KH_RCX(g));
    if(i>=0){ h4s[i].draws++;
        if(kh_aspect_m&&!asp_mode){ s16 y=(s16)rd16(g,h4o[i].obj+0xe), h=(s16)rd16(g,h4o[i].obj+0xa);   /* pc + M: which rects the exe draws */
            pthread_mutex_lock(&mm); H4Stat *s=&h4s[i]; int k; for(k=0;k<s->no;k++) if(s->oy[k]==y&&s->oh[k]==h) break; if(k==s->no&&s->no<4){ s->oy[k]=y; s->oh[k]=h; s->no++; } pthread_mutex_unlock(&mm); }
        if(asp_mode){ if(asp_off_h4) h4_off++;
            else { int k=h4_apply(g,i,gx_B(g)); if(k==0) h4_identity++; else if(k==1) h4s[i].rewritten++; else if(k==2) h4s[i].already++; else h4s[i].other++; } } }
    /* In the picture fixture, draw the dormant primary pair through the same virtual sprite draw as L15701f.
     * The event starter f_3ac350 publishes its task at 0x142a11478. Leave the fade timers and state words alone. */
    if(test_event_bars&&(i==3||i==5)&&gx_B(g)&&rd64(g,GS_BASE+0x2a11478ull)){
        u64 obj=h4o[i].obj;
        if(!rd32(g,obj+0x108)||rd32(g,obj+0x114)){
            u32 color=rd32(g,obj+0x14);
            u64 flags=rd64(g,obj+0x90);
            wr32(g,obj+0x14,0x80000000);
            wr64(g,obj+0x90,0x44);  /* f_156d00 selects this mode for a black rectangle. */
            kh_call(g,rd64(g,rd64(g,obj)+8),obj,0,0,0);
            wr64(g,obj+0x90,flags); wr32(g,obj+0x14,color);
        }
    }
    orig_f_156f10(g);
}
void f_156e20(KCtx *g){
    orig_f_156e20(g);
    if(__builtin_expect(!asp_mode,1)) return;
    h4_e20_calls++;
    if(asp_off_h4){ h4_off++; return; }
    int B=gx_B(g); if(!B){ h4_identity++; return; }
    for(int i=3;i<7;i++){ int k=h4_apply(g,i,B); if(k==1) h4s[i].setter_rewritten++; else if(k==3) h4s[i].setter_other++; }
}

/* ---- H5 ---------------------------------------------------------------------------------------------------------------------- */
static _Atomic u64 h5_calls, h5_ext, h5_other, h5_off, h5_identity;
/* (y0, h) of a scissor rect at B extra rows: full height (0, 416) -> (-B, 416 + 2B); returns 0 for any other rect (left) */
static int h5_target(int B,s32 y,s32 h,s32 *ny,s32 *nh){ if(!B||y!=0||h!=416) return 0; *ny=-B; *nh=416+2*B; return 1; }
typedef struct { u64 ra; s32 x,y,w,h; u64 n; Ivs fr; } Sc;
static Sc sc126[64]; static int n_sc126; static u64 sc126_cut;
typedef struct { s32 l,t,r,b; u64 n; Ivs fr; } Rs;
static Rs rs[48]; static int n_rs; static u64 rs_cut, rs_calls;
void orig_f_126fd0(KCtx *);
void f_126fd0(KCtx *g){
    if(__builtin_expect(!asp_mode&&!kh_aspect_m,1)){ orig_f_126fd0(g); return; }
    s32 x=(s32)(u32)KH_RCX(g), y=(s32)(u32)KH_RDX(g), w=(s32)(u32)KH_R8(g), h=(s32)(u32)KH_R9(g);
    if(kh_aspect_m){ u64 ra=rd64(g,KH_RSP(g)); u32 fr=(u32)kh_game_frames(); pthread_mutex_lock(&mm); int i;
        for(i=0;i<n_sc126;i++) if(sc126[i].ra==ra&&sc126[i].x==x&&sc126[i].y==y&&sc126[i].w==w&&sc126[i].h==h) break;
        if(i==n_sc126){ if(n_sc126<64){ Sc *s=&sc126[i]; s->ra=ra; s->x=x; s->y=y; s->w=w; s->h=h; n_sc126++; } else sc126_cut++; }
        if(i<64){ sc126[i].n++; iv_add(&sc126[i].fr,fr); } pthread_mutex_unlock(&mm); }
    if(asp_mode){ h5_calls++;
        if(asp_off_h5) h5_off++;
        else { int B=gx_B(g); s32 ny,nh;
            if(!B) h5_identity++;
            else if(h5_target(B,y,h,&ny,&nh)){ KH_RDX(g)=(u64)(u32)ny; KH_R9(g)=(u64)(u32)nh; h5_ext++; }
            else h5_other++; } }
    orig_f_126fd0(g);
}
void kh_aspect_m_rs_scissor(KCtx *g,u32 n,u64 rects){             /* runtime/null_d3d12.c: RSSetScissorRects (M) */
    if(!n||!GA_OK(rects)) return; u32 fr=(u32)kh_game_frames();
    s32 l=(s32)rd32(g,rects), t=(s32)rd32(g,rects+4), r=(s32)rd32(g,rects+8), b=(s32)rd32(g,rects+12);
    pthread_mutex_lock(&mm); rs_calls++; int i; for(i=0;i<n_rs;i++) if(rs[i].l==l&&rs[i].t==t&&rs[i].r==r&&rs[i].b==b) break;
    if(i==n_rs){ if(n_rs<48){ rs[i].l=l; rs[i].t=t; rs[i].r=r; rs[i].b=b; n_rs++; } else rs_cut++; }
    if(i<48){ rs[i].n++; iv_add(&rs[i].fr,fr); } pthread_mutex_unlock(&mm);
}

/* ==== task 42: H3, the HUD anchored to the edges of the tall frame (NATIVE-FILL-PLAN.md section 5 "H3 hook points (corrected)",
 * section 8 step 6) ===================================================================================================================
 *
 * The HUD is drawn by the dk draw loop f_166640 (shard_0028.c:28068): setScreenMode f_126500({1,0,0,0}) (return address 0x140166677),
 * which makes f_1433a0 compute MagicScale MS = GX+0xa0..0xac for the loop's mode (on a narrow screen: the 16:9 layout fitted to the
 * full width and CENTRED vertically, MS.w = the band offset o in pixels); then for each object: the first is [rdi+0x18] (L166685,
 * no call), the next ones come from f_4ad270(id [rbx+0xc]) (return address 0x140166693; 0 = end of the list); an object without
 * flag 0x42 at +0x10 draws through its vtable slot 2; at the end f_126500 (return address 0x1401666bf) restores the previous mode.
 * Every HUD draw M saw uses vertex shader 586160 (screen row = GS y * MS.y + MS.w), and f_1121d0 compares GX+0xa0's 16 bytes with its
 * copy and re-stages them at every draw (0x140112321..), so a write to GX+0xac between two objects moves the next object's draws.
 * The D3D12 scissor of a scissored object is mapped at the draw by f_142c00 with the same MS (f_1121d0 0x140112e80: r8 = GX+0x90,
 * [r8+0x14] = MS.y, [r8+0x1c] = MS.w), so it moves with the object (the INFORMATION bar's rect: TASK42 item 3; checked by M).
 *   H3a: post-call hook on f_126500 at return address 0x140166677 only: MS.w_centre = GX+0xac (just computed for the loop's mode);
 *        the first object = [guest rdi + 0x18] (f_126500 saves and restores rdi: 0x140126505 / 0x140126530); GX+0xac = MS.w_centre +
 *        s(first) * o.
 *   H3b: runtime/bgload.c's f_4ad270 hook at return address 0x140166693: rax != 0: GX+0xac = MS.w_centre + s(rax) * o; rax = 0 (the end
 *        of the list): nothing.
 *   s = -1 top (GS y 0 at frame row 0), +1 bottom (GS y 416 at the frame's last row), 0 centre (MS.w_centre written explicitly, so
 *        an object after an anchored one is centred again).  The loop's own restore at 0x1401666bf recomputes MS: nothing to undo.
 *   o = (H - 9W/16) / 2 from GX+0x18 / +0x1c (kh_aspect_o, the band offset of tasks 37 / 40), in pixels as MS.w is (GX+0x28 = 1 and
 *        MS.w = 135 = o at 1920x1350: M).  At 16:9 or wider o = 0 and H3 writes NOTHING (an exact identity), and with nothing set or
 *        KHNMC_ASPECT=pc the hooks are one flag test and the lifted body.
 *   A pass is anchored only when (a) the frame is tall: H1 removes the exe's 16:9 bars in this frame (the 3D view was drawn in this
 *        game frame or the previous one, H1's gate; or KHNMC_ASPECT_TEST_H1=unconditional), otherwise the bars cover the strips and
 *        the HUD stays in the band (counted: "bars on"); and (b) MS.w_centre equals o within 1/2 pixel, i.e. the exe centred the band
 *        as H3 assumes (otherwise the pass is left as the exe made it: the named, counted condition h3-band-mismatch, a WARNING line).
 *   KHNMC_ASPECT_TEST_OFF=h3: every pass is left as the exe made it (counted).
 *
 * The anchor table (TASK42 item 1).  An object's anchor comes from its class (vtable); the generic children (dk::Sprite,
 * dk::SpriteImage, dk::SpriteMessage) take the anchor of their OWNER, named by the dk group at +0x14:
 *   f_166950(obj, prio, group) registers every dk object with the dk manager [0x140abcd20] and stores the group:
 *   `mov [rcx+0x14],r8d` at 0x14016695a; its task callback 0x1401667d0 appends the object to the draw list that f_166640 walks.  The
 *   widget code passes ONE group for a widget and all its children: f_16a900 (the sprite init: group = its 6th argument,
 *   0x14016a920), f_16aa10 (r9d, 0x14016aa20), f_16aa50 (7th argument, 0x14016aa6f); the command menu f_15aff0 / f_15b270 / f_15d4a0
 *   / f_15dc80 / f_15e410 pass 1 (e.g. 0x14015b08f), COMMAND_ONE's f_15d670 passes 2 (0x14015d9ee), the enemy gauge's f_182dd0
 *   passes 6 (0x140182e57), the INFORMATION bar's f_18d830 / f_18e520 pass 7 (0x14018db06), the lock-on's f_17bc60 0x11
 *   (0x14017bd4a).  The engine itself treats +0x14 as the widget: f_166bd0(group) sets flag 0x2 (hidden: the 0x42 test of the draw
 *   loop at 0x14016669f) on EVERY object of the manager whose +0x14 equals group (0x140166bff), and f_166f30 / f_167020 / f_167200 ...
 *   compare +0x14 the same way.  M (task 40, M40/*-M-review.txt "first drawn object" bytes; task 42 per-class group counts) shows +0x14
 *   = 1 on dk::COMMAND_DRAW and the dk::Sprite after the SpriteImage (GS y 290..637), 2 on COMMAND_ONE (and dk::ReactionDecideSprite),
 *   8 on dk::COUNTER and its children (GS y -13.7..96.4, camp route), 3 on GAUGE_PLAYER / GAUGE_ONE /
 *   CIRCLE_MASK / FACE / SECTOR_MASK, 6 on GAUGE_ENEMY and its dk::Sprite (GS y 26..44), 7 on INFORMATION_WINDOW, ScisSpriteMessage
 *   and its dk::Sprite (GS y 15..58), 0x11 on LockOn, 0 on SpriteImage (first in the pass), dk::Layout, Radar, the menus' Tz:: objects
 *   and the dk::Sprite / SpriteMessage after them.  A pointer field naming the owner does not exist (task 40: none in +0x08..+0x78).
 * The group also guards a fixed class: a top (bottom) class whose group is in the group table as bottom (top) stays centred (counted,
 * the class and group listed in the exit report: "class and group disagree"); a centre group (0 = no widget, 0x11) does not veto.  A
 * class NOT in the table stays centred whatever its group (TASK42 item 1 "Anything unclassified stays centred", plan section 5 "Anything
 * not in the table is drawn at the centre"; task 42 review finding 4: a group rule for unlisted classes would move classes nobody
 * measured, e.g. a 3D-attached marker or a message window registered under a HUD group); a generic child of a group not in the table
 * stays centred too.  Every such object is counted and its (class, group) pair listed in the exit report ("H3 centred: ..."), so a
 * class M has not seen shows up there with its group (the camp route's NEXT_EXP / GET_MUNNY / ReactionDecideSprite were found that
 * way and are now in the table). */
enum { H3_CENTRE=0, H3_TOP=1, H3_BOTTOM=2, H3_GENERIC=3 };
static const char *h3_aname[4]={"centre","top","bottom","owner"};
typedef struct { u64 vt; const char *name; int anchor; const char *ev; } H3Cls;
/* GS y = POSITION - xyOffset of the class's draws (0..416 = the 16:9 band; M40, story / default route, null backend) */
static const H3Cls h3cls[]={
    {GS_BASE+0x5b2078ull,"dk::GAUGE_ONE",H3_BOTTOM,"HP / MP / Drive bars, GS y 265.9..445.1 (story) / 310..400.5 (default), group 3"},
    {GS_BASE+0x5b2018ull,"dk::CIRCLE_MASK",H3_BOTTOM,"the face ring, GS y 260..425.7 / 305..405, group 3"},
    {GS_BASE+0x5b2048ull,"dk::FACE",H3_BOTTOM,"GS y 296..407 / 305..400, group 3"},
    {GS_BASE+0x5b1fe8ull,"dk::SECTOR_MASK",H3_BOTTOM,"a mask quad of the face gauge (GS y -1837..6351), drawn with FACE, group 3"},
    {GS_BASE+0x5b2100ull,"dk::GAUGE_PLAYER",H3_BOTTOM,"container of the player gauges (no draws), group 3"},
    {GS_BASE+0x5b2158ull,"dk::GAUGE_FRIEND",H3_BOTTOM,"party gauges (plan section 5 table; never drawn in M: flag 0x42 on every visit), groups 4 / 5 (statically: f_181d20 -> f_17ed50, the group table)"},
    {GS_BASE+0x5b1c28ull,"dk::COMMAND_DRAW",H3_BOTTOM,"container of the command menu (no draws), group 1"},
    {GS_BASE+0x5b1bf8ull,"dk::COMMAND_ONE",H3_BOTTOM,"command slots, GS y 282..637 (the panel continues below the band), group 2"},
    {GS_BASE+0x5b21e0ull,"dk::GAUGE_ENEMY",H3_TOP,"container of the enemy gauge (its dk::Sprite: GS y 26..44), group 6"},
    {GS_BASE+0x5b2430ull,"dk::INFORMATION_WINDOW",H3_TOP,"container of the INFORMATION bar (no draws), group 7"},
    {GS_BASE+0x5b2400ull,"dk::ScisSpriteMessage",H3_TOP,"INFORMATION text, GS y 14..58, scissored, group 7"},
    {GS_BASE+0x5b1df0ull,"dk::Layout",H3_CENTRE,"the mod's style rank (battle), group 0: measured, it rests at the band's vertical centre (its draws' GS y mid-points 192..224 in both routes, 2,935 of 3,602 in 192..208; band centre 208; GS y 61.7..361.7 is its zoom-in): no edge is nearer, so centred"},
    {GS_BASE+0x5b23a0ull,"dk::MISSION_GAUGE",H3_TOP,"Shan-Yu gate gauge, group 10; children GS y 17..141.625"},
    {GS_BASE+0x5b2370ull,"dk::COUNTER",H3_TOP,"the mission counter's container (no draws), group 8; its dk::Sprite children: GS y -13.7..96.4 (camp route M)"},
    {GS_BASE+0x5b2460ull,"dk::NEXT_EXP",H3_TOP,"camp route M: GS y 37..54 at the left edge (x -61..-27), group 7 (the INFORMATION widget's EXP line)"},
    {GS_BASE+0x5b2490ull,"dk::GET_MUNNY",H3_TOP,"camp route M: GS y 37..92 at the left edge (x -57..-10), group 7 (the munny line)"},
    {GS_BASE+0x5b1bc8ull,"dk::ReactionDecideSprite",H3_BOTTOM,"camp route M: the reaction command over the command menu, GS y 204.5..336.25, x -122..26, group 2"},
    {GS_BASE+0x5b24f8ull,"dk::LockOn",H3_CENTRE,"attached to 3D (follows the target, GS y 17..814), group 0x11"},
    {GS_BASE+0x5b2540ull,"dk::Exp",H3_CENTRE,"attached to 3D (plan section 5); camp route M: GS y 108..233 mid-screen, group 15"},
    {GS_BASE+0x5b1b60ull,"dk::TREASURE_EVENT_INFO",H3_CENTRE,"camp route M: a full-band event window (GS y -63..416), group 0"},
    {GS_BASE+0x5b24c0ull,"dk::Sprite3D",H3_CENTRE,"attached to 3D (skipped by flag 0x42 in M)"},
    {GS_BASE+0x5b2340ull,"dk::Radar",H3_TOP,"the minimap, top right: no draws in the loop in the default and story routes; in the camp route (frames 20846+) its draws' GS y mid-points lie in 16..112 at x 273..789, inside the scissor (1604,177)-(1874,426) = GS x 485..581, y 16..112 at 1920x1350; group 0"},
    {GS_BASE+0x5b2638ull,"dk::LEVELUP_INFO",H3_CENTRE,"container, no draws in either route, group 0"},
    {GS_BASE+0x5b1b90ull,"dk::TREASURE_INFO",H3_CENTRE,"container, no draws in either route, group 0"},
    {GS_BASE+0x5b1ee0ull,"dk::BILLBOARD_PLACE",H3_CENTRE,"no draws in either route, group 0"},
    {GS_BASE+0x5b1e80ull,"dk::BILLBOARD_GUIDE",H3_CENTRE,"32 draws at GS y 360..388 in story frames 3475-3506 only, group 0: not classified by M, centred"},
    {GS_BASE+0x5b1dc0ull,"Tz::Layout",H3_CENTRE,"full-screen menus (title, New Game, camp menu): the 16:9 safe area"},
    {GS_BASE+0x5bab18ull,"Tz::MenuItem",H3_CENTRE,"menus"},
    {GS_BASE+0x5baba8ull,"Tz::LightCur",H3_CENTRE,"menus"},
    {GS_BASE+0x5b8478ull,"Tz::LayMsg",H3_CENTRE,"the camp menu (camp route, frames 20518-21813)"},{GS_BASE+0x5bbc20ull,"Tz::MultiSeq",H3_CENTRE,"the camp menu"},
    {GS_BASE+0x5bab58ull,"Tz::LaySeqMess",H3_CENTRE,"the camp menu"},{GS_BASE+0x5bac58ull,"Tz::MenuObj",H3_CENTRE,"the camp menu (a full-frame quad)"},
    {GS_BASE+0x5c5180ull,"YS::WINDOW_OBJ2D",H3_CENTRE,"message (talk) windows: the centred band"},
    {GS_BASE+0x5b1a70ull,"dk::Sprite",H3_GENERIC,"generic child: its owner's anchor by the group +0x14"},
    {GS_BASE+0x5b1a40ull,"dk::SpriteImage",H3_GENERIC,"generic (first in the HUD pass, group 0: centre)"},
    {GS_BASE+0x5b1aa0ull,"dk::SpriteMessage",H3_GENERIC,"generic text (menus: group 0)"}};
#define H3_NCLS ((int)(sizeof h3cls/sizeof *h3cls))
/* Slot 2 of a dk draw object, reached only at the draw call in f_166640. */
static int hud_draw_owner(u64 vt,u32 group){
    for(int i=0;i<H3_NCLS;i++) if(h3cls[i].vt==vt){
        if(h3cls[i].anchor==H3_GENERIC) return group>=1 && group<=17 && group!=7;
        if(vt==GS_BASE+0x5b2400 || vt==GS_BASE+0x5b2430) return 0; /* INFORMATION text box */
        if(h3cls[i].anchor!=H3_CENTRE) return 1;
        switch(vt-GS_BASE){
        case 0x5b1df0: /* style rank */
        case 0x5b24f8: case 0x5b2540: case 0x5b24c0: /* lock-on, EXP and 3D sprites */
        case 0x5b2638: case 0x5b1b90: case 0x5b1ee0: return 1; /* field notices */
        default: return 0; /* menus and windows */
        }
    }
    return 0;
}

int kh_hide_dk_draw(KCtx *g){
    if(kh_phase_title_alive() || kh_route_cond_eval(g,KR_MENU_OPEN,0,0,NULL)) return 0;
    u64 obj=KH_RCX(g);
    return hud_draw_owner(rd64(g,obj),rd32(g,obj+0x14));
}
typedef struct { u32 grp; int anchor; const char *owner; } H3Grp;
static const H3Grp h3grp[]={
    {1,H3_BOTTOM,"the command menu (dk::COMMAND_DRAW and its children)"},{2,H3_BOTTOM,"dk::COMMAND_ONE"},
    {3,H3_BOTTOM,"the player gauges (dk::GAUGE_PLAYER, GAUGE_ONE, CIRCLE_MASK, FACE, SECTOR_MASK)"},
    /* task 42 review finding 5: the party gauges' groups, read statically (never drawn in the routes: dk::GAUGE_FRIEND has flag 0x42 at
     * every visit).  f_17ed50 (the gauge init) stores r9d / r8d at +0x34 / +0x38 and registers with group r8d + r9d + 3 (0x14017ed82
     * `lea ecx,[r9+0x3]`, 0x14017ed86 `add r8d,ecx`, 0x14017ed8c `jmp 0x140166950`); f_17edf0 returns the same sum for the children.
     * The player gauge f_17f270 passes 0 / 0 (0x14017f284 / 0x14017f287): group 3, as M measured.  The GAUGE_FRIEND builder f_181d20
     * sets r9d = 1 (0x140181d41) and is called twice: r8d = 1 (0x14016c5ca, call 0x14016c5d3) -> group 5, r8d = 0 (0x14016c5e9, call
     * 0x14016c5ef) -> group 4.  Its children (constructor 0x14016b5a0: dk::FACE at +0x50, 4 dk::GAUGE_ONE at +0x370, 5 dk::CIRCLE_MASK at
     * +0xb70) register with that group (r15d = f_17edf0's result: f_17e4b0 r9d at 0x140181dcd, f_17ed10 -> f_16a900's 6th argument at
     * 0x140181da8), so a generic child of the party gauges takes the bottom with them */
    {4,H3_BOTTOM,"the party gauges (dk::GAUGE_FRIEND built by f_181d20 with r8d = 0: f_17ed50 group r8d + r9d + 3 = 4) and their children"},
    {5,H3_BOTTOM,"the party gauges (dk::GAUGE_FRIEND built by f_181d20 with r8d = 1: group 5) and their children"},
    {10,H3_TOP,"mission gauge children (f_18caa3..f_18cb72, group 10 passed to f_16a900)"},
    {6,H3_TOP,"the enemy gauge (dk::GAUGE_ENEMY)"},
    {8,H3_TOP,"the mission counter (dk::COUNTER and its children: f_18be20 registers them with group 8, 0x14018be6c / 0x14018bf2b; camp route M: GS y -13.7..96.4)"},{7,H3_TOP,"the INFORMATION bar (dk::INFORMATION_WINDOW, ScisSpriteMessage)"},
    {0x11,H3_CENTRE,"the lock-on (dk::LockOn)"},
    {0,H3_CENTRE,"no widget: the HUD pass's head dk::SpriteImage, dk::Layout, Radar, the menus' Tz:: objects and their children"}};
#define H3_NGRP ((int)(sizeof h3grp/sizeof *h3grp))
/* why an object got its anchor */
enum { H3W_CLASS=0, H3W_GROUP, H3W_UNLISTED, H3W_UNCLASSIFIED, H3W_GROUP_UNKNOWN, H3W_DISAGREE, H3W_N };
static const char *h3_wname[H3W_N]={"by its class","by its owner's group (+0x14)","class not in the table, centred (its group is in the table)","class not in the table, centred (its group is not in the table either)",
                                     "generic child of a group not in the table","class and group disagree"};
static int h3_cls_index(u64 vt){ for(int i=0;i<H3_NCLS;i++) if(h3cls[i].vt==vt) return i; return -1; }
static int h3_grp_index(u32 grp){ for(int i=0;i<H3_NGRP;i++) if(h3grp[i].grp==grp) return i; return -1; }
/* the anchor of an object of class vtable vt and dk group grp: H3_CENTRE / H3_TOP / H3_BOTTOM, *why = H3W_* */
int kh_aspect_h3_anchor(u64 vt,u32 grp,int *why){
    int c=h3_cls_index(vt), k=h3_grp_index(grp), w, a;
    if(c<0){ w=k<0?H3W_UNCLASSIFIED:H3W_UNLISTED; a=H3_CENTRE; }                         /* not in the table: centred (TASK42 item 1), listed */
    else if(h3cls[c].anchor==H3_GENERIC){ if(k<0){ w=H3W_GROUP_UNKNOWN; a=H3_CENTRE; } else { w=H3W_GROUP; a=h3grp[k].anchor; } }
    else if(h3cls[c].anchor!=H3_CENTRE&&k>=0&&h3grp[k].anchor!=H3_CENTRE&&h3grp[k].anchor!=h3cls[c].anchor){ w=H3W_DISAGREE; a=H3_CENTRE; }   /* top against bottom */
    else { w=H3W_CLASS; a=h3cls[c].anchor; }
    if(why) *why=w; return a;
}
/* the MS.w H3 writes for an anchor: centre wc, top wc - o, bottom wc + o (single precision, as MS is stored) */
static float h3_value(float wc,float o,int anchor){ return anchor==H3_TOP?wc-o:anchor==H3_BOTTOM?wc+o:wc; }
typedef struct { _Atomic u64 passes, off, identity, bars, mismatch, anchored, empty, first[3], next[3], ends, nopass, nested, why[H3W_N], cls[H3_NCLS+1][3];
                 _Atomic u64 inner, inner_back, inner_anch, inner_reapplied, inner_other; _Atomic int warned, warned_inner; float mm_wc, mm_o; } H3Stat;
static H3Stat h3; static _Atomic int h3_logged;
/* setScreenMode (f_126500) inside an object of an anchored pass, by (class of the current object, return address) (task 42 review finding 2) */
typedef struct { u64 vt, ra, n, back; int a; char name[72]; } H3Inner;
static H3Inner h3in[16]; static int n_h3in; static u64 h3in_cut;
/* (class, group) pairs that were centred for want of a classification or because the class and the group disagree (exit report) */
typedef struct { u64 vt; u32 grp; int why; u64 n; char name[72]; } H3Odd;
static H3Odd h3odd[32]; static int n_h3odd; static u64 h3odd_cut;
static void rtti_name(KCtx *g,u64 vt,char *out,size_t n);    /* M, below */
#define H3_DEPTH 4
static __thread int h3_depth;              /* dk passes begun (H3a) and not yet restored (0x1401666bf) on this thread */
static __thread int h3_on[H3_DEPTH]; static __thread float h3_wc[H3_DEPTH], h3_o[H3_DEPTH];
/* per anchored pass: the loop's MagicScale as H3a found it (GX+0xa0..0xac bits, before H3 wrote) and the current object (its class, its
 * anchor, the MS.w H3 wrote for it; anchor -1 = none yet) */
static __thread u32 h3_ms[H3_DEPTH][4]; static __thread u64 h3_cur_vt[H3_DEPTH]; static __thread int h3_cur_a[H3_DEPTH]; static __thread float h3_cur_v[H3_DEPTH];
/* task 53: per anchored pass on a screen taller than wide, the HUD scales (h3_hs = one of them is not 1: MS.x / .y / .z are written
 * for every object of the pass) and the four MagicScale words written for the current object */
static __thread int h3_hs[H3_DEPTH]; static __thread double h3_hsc[H3_DEPTH][HUD_N]; static __thread u32 h3_cur_ms[H3_DEPTH][4];
static __thread u64 h3_cur_obj[H3_DEPTH];  /* task 53 item 2: the current dk object (the title's layouts are told by their address) */
/* one object: its anchor, GX+0xac written (task 53: with a HUD scale, GX+0xa0..0xac); `first` = H3a (the list head), else H3b */
static void h3_apply(KCtx *g,u64 obj,int first){
    int d=h3_depth-1; u64 vt=(GA_OK(obj)&&GA_OK(obj+0x17))?rd64(g,obj):0; u32 grp=vt?rd32(g,obj+0x14):0; int why, a=kh_aspect_h3_anchor(vt,grp,&why);
    float v=h3_value(h3_wc[d],h3_o[d],a);
    if(h3_hs[d]){ u32 *ms=h3_cur_ms[d]; int k=a==H3_BOTTOM?hud_kind(grp):-1;            /* task 53: the scaled widgets, the loop's MS.x / .y / .z for the rest */
        ms[0]=h3_ms[d][0]; ms[1]=h3_ms[d][1]; ms[2]=h3_ms[d][2]; ms[3]=fbits(v);
        if(k>=0&&h3_hsc[d][k]!=1.0){ hud_ms(ms,h3_hsc[d][k],hud_px[k],hud_py[k]); hud.objs[k]++; } else hud.back++;
        if(a==H3_TOP && grp==6) hud_ms(ms,enemy_scale,590,26);
        if(vt==GS_BASE+0x5b1df0ull && (style_scale!=1 || style_raise!=0)){
            double W=rdf(g,GX+0x18),H=rdf(g,GX+0x1c);
            double xs=bf(ms[0]);
            // The complete rank animation spans GS x 384..683, y 61..362.
            double scale=fmin(style_scale,(W-2)/(299*xs));
            hud_ms(ms,scale,533.5f,208);
            ms[3]=fbits((float)(bf(ms[3])-H*style_raise));
        }
        for(int i=0;i<3;i++) wr32(g,GX+0xa0+4ull*(u64)i,ms[i]);
        v=bf(ms[3]); }
    wrf(g,GX+0xac,v); h3_cur_vt[d]=vt; h3_cur_a[d]=a; h3_cur_v[d]=v; h3_cur_obj[d]=obj;
    h3.anchored++; (first?h3.first:h3.next)[a]++; h3.why[why]++; { int c=h3_cls_index(vt); h3.cls[c<0?H3_NCLS:c][a]++; }
    if(why>=H3W_UNLISTED&&vt){ pthread_mutex_lock(&mm); int i; for(i=0;i<n_h3odd;i++) if(h3odd[i].vt==vt&&h3odd[i].grp==grp&&h3odd[i].why==why) break;
        if(i==n_h3odd){ if(n_h3odd<32){ h3odd[i].vt=vt; h3odd[i].grp=grp; h3odd[i].why=why; rtti_name(g,vt,h3odd[i].name,sizeof h3odd[i].name); n_h3odd++; } else h3odd_cut++; }
        if(i<32) h3odd[i].n++; pthread_mutex_unlock(&mm); }
}
/* H3a: after the lifted f_126500 returned to 0x140166677 (list = guest rdi) */
static void h3a_post(KCtx *g){
    h3.passes++;
    if(h3_depth>0) h3.nested++;
    int d=h3_depth<H3_DEPTH?h3_depth:H3_DEPTH-1; h3_depth=d+1; h3_on[d]=0; h3_hs[d]=0;
    if(asp_off_h3){ h3.off++; return; }
    float W=rdf(g,GX+0x18), H=rdf(g,GX+0x1c); double o=(W>0&&H>0)?kh_aspect_o(W,H):0.0;
    if(o==0.0){ h3.identity++; return; }                                             /* 16:9 or wider: nothing is written */
    if(!asp_h1_uncond&&!gate_open_at(kh_game_frames())){
        if(asp_off_h7){ h3.bars++; return; }                                         /* the exe's bars are on this frame: the HUD stays in the band */
        h7_h3_no3d++; }                                                              /* task 46 (H7): no bars on any frame, so the pass is anchored as in play */
    float wc=rdf(g,GX+0xac);
    double dw=(double)wc-o; if(!(dw<=0.5&&dw>=-0.5)){ h3.mismatch++; h3.mm_wc=wc; h3.mm_o=(float)o;
        if(!atomic_exchange(&h3.warned,1)) fprintf(stderr,"kh: aspect: WARNING: h3-band-mismatch: the dk loop's MagicScale.w is %g where the band offset o of the %gx%g screen is %g: the pass is left as the exe made it (counted)\n",wc,W,H,o);
        return; }
    h3_on[d]=1; h3_wc[d]=wc; h3_o[d]=(float)o; h3_cur_vt[d]=0; h3_cur_a[d]=-1; h3_cur_obj[d]=0;
    for(int i=0;i<4;i++) h3_ms[d][i]=rd32(g,GX+0xa0+4ull*(u64)i);
    for(int k=0;k<HUD_N;k++){ h3_hsc[d][k]=kh_aspect_hud_scale(k,W,H); if(h3_hsc[d][k]!=1.0) h3_hs[d]=1; }   /* task 53: a screen taller than wide */
    if(W>0 && H>W && (style_scale!=1 || style_raise || enemy_scale!=1)) h3_hs[d]=1;
    if(h3_hs[d]) hud.passes++;
    if(kh_aspect_log&&!atomic_exchange(&h3_logged,1))
        fprintf(stderr,"kh: aspect: H3: first anchored dk pass: screen %gx%g, o %g; MagicScale (GX+0xa0..0xac) %g %g %g %g; GS frame GX+0x38/0x3c %g/%g, GX+0x48/0x4c %g/%g; GX+0x28 %g; game frame %llu, present %llu, guest thread %u\n",
                W,H,o,rdf(g,GX+0xa0),rdf(g,GX+0xa4),rdf(g,GX+0xa8),wc,rdf(g,GX+0x38),rdf(g,GX+0x3c),rdf(g,GX+0x48),rdf(g,GX+0x4c),rdf(g,GX+0x28),P(kh_game_frames()),P(gfx_presents),cur_tid());
    u64 list=KH_RDI(g), first=GA_OK(list+0x18)&&GA_OK(list+0x1f)?rd64(g,list+0x18):0;
    if(first) h3_apply(g,first,1); else h3.empty++;
}
/* H3b: f_4ad270 returned obj to 0x140166693 */
static void h3b_post(KCtx *g,u64 obj){
    if(!h3_depth){ h3.nopass++; return; }
    if(!h3_on[h3_depth-1]) return;
    if(!obj){ h3.ends++; return; }
    h3_apply(g,obj,0);
}
static void h3_end(void){ if(h3_depth>0) h3_depth--; }                                 /* the loop's restore (0x1401666bf) */
/* task 42 review finding 2: f_126500 returned to an address other than 0x140166677 / 0x1401666bf while an anchored pass is open on this
 * thread, i.e. from inside the current object's draw (the dk loop calls nothing else between H3a and its restore but slot 2, f_4ad270
 * and f_126500).  f_126500 always calls f_1433a0 (shard_0023.c L126515), which recomputes MagicScale for the mode it sets, so the
 * object's anchored MS.w is gone after the call.  YS::WINDOW_OBJ2D does this (0x1403bc4c7: its zoom mode {0.8, 0, 0, w}, 0x1403bc4f0:
 * the saved mode back; camp route M: 173 + 173), a centre class.  When the call leaves the loop's own MagicScale (all of GX+0xa0..0xac
 * as H3a found it: the loop's mode is back) and the object is anchored, its anchored MS.w is written again, so the rest of its draws
 * stay at its edge; another MagicScale (a mode of the object's own) is left as the exe made it.  Every call is counted by (class,
 * return address); one in an ANCHORED object is the named condition h3-inner-mode-anchored (a WARNING line once and in the report;
 * runcheck h3 requires 0): its draws in its own mode are not anchored, and no anchored class has been measured doing it. */
static void h3_inner_post(KCtx *g,u64 ra){
    int d=h3_depth-1, a=h3_cur_a[d]; u64 vt=h3_cur_vt[d]; int anch=a==H3_TOP||a==H3_BOTTOM, back=1;
    for(int i=0;i<4;i++) if(rd32(g,GX+0xa0+4ull*(u64)i)!=h3_ms[d][i]) back=0;
    h3.inner++; if(back) h3.inner_back++;
    if(anch){ h3.inner_anch++; if(back){ if(h3_hs[d]) for(int i=0;i<3;i++) wr32(g,GX+0xa0+4ull*(u64)i,h3_cur_ms[d][i]);   /* task 53: a scaled object's MS.x / .y / .z too */
            wrf(g,GX+0xac,h3_cur_v[d]); h3.inner_reapplied++; } else h3.inner_other++; }
    pthread_mutex_lock(&mm); int i; for(i=0;i<n_h3in;i++) if(h3in[i].vt==vt&&h3in[i].ra==ra) break;
    if(i==n_h3in){ if(n_h3in<16){ memset(&h3in[i],0,sizeof h3in[i]); h3in[i].vt=vt; h3in[i].ra=ra; h3in[i].a=a; if(vt) rtti_name(g,vt,h3in[i].name,sizeof h3in[i].name); else snprintf(h3in[i].name,sizeof h3in[i].name,"(no object yet)"); n_h3in++; } else h3in_cut++; }
    if(i<16){ h3in[i].n++; if(back) h3in[i].back++; }
    if(anch&&!atomic_exchange(&h3.warned_inner,1))
        fprintf(stderr,"kh: aspect: WARNING: h3-inner-mode-anchored: setScreenMode (f_126500) from return address 0x%llx inside an anchored object (%s, vtable 0x%llx, %s): "
                       "%s; its draws in a mode of its own are not anchored (counted; see the exit report)\n",P(ra),i<16?h3in[i].name:"?",P(vt),h3_aname[a],
                       back?"the loop's MagicScale is back, its anchored MS.w written again":"another mode, left as the exe made it");
    pthread_mutex_unlock(&mm);
}
static void h3_report(FILE *f){
    fprintf(f,"kh: aspect report: H3 (HUD anchoring, dk loop 0x166640: f_126500 at 0x140166677 = H3a, f_4ad270 at 0x140166693 = H3b): passes %llu: "
              "o = 0 (16:9 or wider), nothing written %llu; the exe's bars on (no 3D view), left centred %llu; band mismatch (h3-band-mismatch), left %llu; test aid off %llu; anchored %llu (empty list %llu); "
              "begun inside another pass %llu; objects: first (H3a) top %llu bottom %llu centre %llu, next (H3b) top %llu bottom %llu centre %llu; ends of the list (rax = 0) %llu; next object outside a pass %llu\n",
            P(h3.passes),P(h3.identity),P(h3.bars),P(h3.mismatch),P(h3.off),P(h3.passes-h3.identity-h3.bars-h3.mismatch-h3.off),P(h3.empty),P(h3.nested),
            P(h3.first[H3_TOP]),P(h3.first[H3_BOTTOM]),P(h3.first[H3_CENTRE]),P(h3.next[H3_TOP]),P(h3.next[H3_BOTTOM]),P(h3.next[H3_CENTRE]),P(h3.ends),P(h3.nopass));
    fprintf(f,"kh: aspect report: H3 objects by the reason of their anchor:"); for(int w=0;w<H3W_N;w++) fprintf(f,"%s %s %llu",w?";":"",h3_wname[w],P(h3.why[w])); fputc('\n',f);
    fprintf(f,"kh: aspect report: H3 objects by class (top / bottom / centre):");
    for(int c=0;c<=H3_NCLS;c++){ u64 t=h3.cls[c][H3_TOP], b=h3.cls[c][H3_BOTTOM], m=h3.cls[c][H3_CENTRE]; if(t||b||m) fprintf(f," %s %llu/%llu/%llu;",c<H3_NCLS?h3cls[c].name:"(not in the table)",P(t),P(b),P(m)); }
    fputc('\n',f);
    pthread_mutex_lock(&mm);
    for(int i=0;i<n_h3odd;i++)
        fprintf(f,"kh: aspect report: H3 centred: %s vtable 0x%llx %s, group (+0x14) %u: %llu objects\n",h3_wname[h3odd[i].why],P(h3odd[i].vt),h3odd[i].name,h3odd[i].grp,P(h3odd[i].n));
    if(h3odd_cut) fprintf(f,"kh: aspect report: H3 centred: %llu objects of further (class, group) pairs (table full)\n",P(h3odd_cut));
    pthread_mutex_unlock(&mm);
    fprintf(f,"kh: aspect report: H3 setScreenMode (f_126500) inside an object of an anchored pass (any return address but 0x140166677 / 0x1401666bf; f_126500 recomputes MagicScale): %llu: "
              "the loop's MagicScale back after it %llu; in anchored objects %llu (the loop's MagicScale back: the anchored MS.w written again %llu; another mode, left as the exe made it %llu)\n",
            P(h3.inner),P(h3.inner_back),P(h3.inner_anch),P(h3.inner_reapplied),P(h3.inner_other));
    pthread_mutex_lock(&mm);
    for(int i=0;i<n_h3in;i++) fprintf(f,"kh: aspect report: H3 setScreenMode inside an object: %s vtable 0x%llx (%s) from return address 0x%llx: %llu (the loop's MagicScale back after it %llu)\n",
                                      h3in[i].name,P(h3in[i].vt),h3in[i].a>=0?h3_aname[h3in[i].a]:"no object",P(h3in[i].ra),P(h3in[i].n),P(h3in[i].back));
    if(h3in_cut) fprintf(f,"kh: aspect report: H3 setScreenMode inside an object: %llu calls of further (class, return address) pairs (table full)\n",P(h3in_cut));
    pthread_mutex_unlock(&mm);
    if(h3.inner_anch) fprintf(f,"kh: aspect: WARNING: h3-inner-mode-anchored: %llu setScreenMode calls inside anchored objects (anchored MS.w written again after %llu, another mode left %llu): not measured, see the lines above\n",
                              P(h3.inner_anch),P(h3.inner_reapplied),P(h3.inner_other));
    if(h3.mismatch) fprintf(f,"kh: aspect: WARNING: h3-band-mismatch: %llu dk passes left as the exe made them (MagicScale.w %g against o %g at the last)\n",P(h3.mismatch),h3.mm_wc,h3.mm_o);
    if(h3.nested||h3.nopass) fprintf(f,"kh: aspect: NOTE: H3: %llu dk passes begun inside another pass, %llu next objects outside a pass (both 0 in task 40's M)\n",P(h3.nested),P(h3.nopass));
}

/* ==== task 46: H7, no black bars in the menus of the tall frame =========================================================================
 *
 * With tasks 37 / 40 / 42 the 3D view fills the tall frame and the HUD sits at its edges, but every frame WITHOUT the 3D view (title,
 * New Game, the save menu, the camp / main menu, the pause screen, the map-change wipe) kept the exe's 16:9 bars (H1's gate).  H7 has
 * two halves, both only with KHNMC_ASPECT=fill on a screen narrower than 16:9 (r > 1, o > 0):
 *   (a) no bars on any frame: H1 rewrites the 16:9 fit on every frame (the gate of task 37 no longer decides), and H3 anchors every
 *       dk pass (the gate of task 42 existed only because the bars covered the strips).  The strips then show what is drawn there:
 *       the frame's clear colour, the picture the final blit copies (the 3D scene behind the pause screen; the camp menu's own
 *       full-frame quad, Tz::MenuObj, VS 586710 in pixels), layout pieces that lie beyond the band, and the rectangles of (b).
 *   (b) backdrops reach the frame's first and last row.  A backdrop is recognised by its shape, in the GS frame (0..416 = the band):
 *       R1  one quad that is an axis-aligned rectangle spanning the band's whole height (y_min <= 0 + 1, y_max >= 416 - 1): a
 *           background picture, a dimming or fading rectangle, a full-height side strip.  Only that quad is changed.
 *       R2  a batch (the parts of one layout sequence frame) in which no quad is such a rectangle but whose wide rectangles (at
 *           least as wide as the frame, less 2 GS columns; wherever they are in x: the save menu slides its backdrop in from the
 *           left) stack without a gap from y <= 0 + 1 to y >= 416 - 1: a background picture cut into horizontal stripes (the camp
 *           and save menus: 8 stripes of 64 rows, x -101..612, y -51..461, plus 8 right-hand pieces of the same picture).  The
 *           stack's stripes and every rectangle of the batch that lies on a stripe's rows (the right-hand pieces) are changed, with
 *           one factor, so the picture stays in one piece; any other quad of the batch (a header, a cursor) is left where it is and
 *           counted (review finding 4).
 *       R3  a rectangle that R1 and R2 left, narrower than the frame, at least 3/4 of the band tall, with ONE edge exactly on a band
 *           edge (y_min = 0 or y_max = 416 within 1/8 row): a picture the 16:9 screen cuts with a straight edge (the title's 400 x
 *           400 picture, x 195..595, y 0..400: its top is cut by the screen, its other three sides fade out).  It is MOVED by b to
 *           the frame's edge, not stretched: the cut edge is the frame's edge again and the picture keeps its shape (review finding
 *           1: left in place, its cut edge was a hard line 135 rows below the frame's top).
 *       An R1 rectangle is left ("covered by its batch") when the batch's own rectangles above and below it, each at least as wide
 *       as the part of it inside the frame and meeting it without a gap, already reach beyond the frame: the batch covers the strips
 *       itself (the map-change transition's batch of a centre square with a rectangle on each side: review finding 2).
 *       The change is a stretch in y about the band's centre by the smallest factor that makes the rectangle (R1) or the stack (R2)
 *       cover the frame: y' = 208 + (y - 208) s, s = max(1, (208 + b) / (208 - y_min), (208 + b) / (y_max - 208)), b = 208 (r - 1) =
 *       the strip's height in GS rows.  s = r for a rectangle that is exactly the band (0..416); less for one that already reaches
 *       beyond it (the title's pictures -31..448: 1.09; the pause screen's dim -16..436: 1.16; the camp menu's stripes: 1.03); s = 1,
 *       i.e. nothing written, for one that already covers the frame (H4's fades).  Everything else (widgets, text, cursors: quads
 *       that do not span the band's height) is left exactly where it is, and x is never changed.  A rectangle is a quad whose
 *       corners lie within 1/8 GS unit of its box (GS coordinates are multiples of 1/16; a piece drawn through a 180-degree turn
 *       comes out one step off: review finding 3).
 * Where: every 2D primitive is submitted through f_11a330(GX, count, prim, flags) (0x14011a330; reached by a jump from f_11a2f0 and
 * 0x14011a480, so the return address on the guest stack names the game's draw routine).  Its vertices were written by the caller
 * into the block f_121ff0 handed out: the current vertex buffer object B (0x14011a386..0x14011a3b2: [GX+0x59b8] != 0 ? [GX+0x59b8] +
 * 0x60 + 0x58 * (s32)[that + 0x58] : GX + 0x5790 + 0x58 * [GX+0x578c]) holds the stride at +0x8 and the block's address at +0x48
 * (f_121ff0 0x1401220c4 / 0x1401220f4; f_127ef0 passes exactly these to the index builder f_142f40, 0x140127f43 / 0x140127f46).  A
 * vertex begins with POSITION x, y (floats, GS units); the vertex shader subtracts cbConstantVS.xyOffset, which f_1121d0 stages from
 * GX+0x138 / +0x13c (0x14011301e / 0x140113026), and maps with MagicScale, staged from GX+0xa0 unless [GX+0xb4] or [GX+0xb8] is 0
 * (then GX+0x90: 0x140112386..0x14011239d).  Primitive 8 is a list of quads, 4 vertices each (f_142f40 0x140142f64..0x140142feb builds
 * one strip per 4 vertices); the layout draw f_1969a0 submits the parts of one sequence frame as one such list (0x140196f59:
 * [rdi+0x30] parts, 4 vertices of 0x1c bytes each from f_197b60).  H7 acts at the entry of f_11a330, before the lifted body builds
 * the indices and draws:
 *   - only for the draw routines of the table h7site (by return address).  The ppp effect routines submit world-space quads through
 *     f_11a330 too, with the same 28-byte vertex and a 3D vertex shader: a band-height rectangle from any other routine is counted
 *     and listed (h7-unlisted-site) and never written.
 *   - MagicScale must be the centred band of this screen: the row GX+0xa0 in force, MS.w = o and 416 MS.y = H - 2 o within 1/2 pixel
 *     (the dk loop's mode, the menus' mode, the fades' mode).  An anchored HUD object (MS.w = 0 or 2 o), a zoomed window, a draw
 *     into a small target: not examined (counted).
 *   - the vertex must be the 28-byte 2D vertex and the batch a quad list (primitive 8, a multiple of 4 vertices) or 4 vertices.
 *     A batch whose box spans the band's height without R1 or R2 applying is left and LISTED (by return address and dk class): the
 *     place to look when a screen keeps a strip (the map-change wipe's frame of 5 quads is one: it covers the frame by itself).
 *   - the lifted body's own lazy begin (0x14011a345: [GX+0x4c0] == 0 with [GX+0x4b0] != 0) is left to the exe (counted).
 *   - the vertex block must be the one f_121ff0 just handed out (h7_pre): otherwise the named, counted condition h7-vertex-block
 *     (a WARNING line in the exit report) and the batch is left.
 * Why a stretch in y and not a uniform scale: a uniform scale would crop the picture at both sides and move its features in x too;
 * the smallest covering factor keeps the distortion at 3 to 25 % in y, none at the band's centre, and widgets do not move at all.
 * At 16:9 or wider nothing is read or written beyond GX+0x18 / +0x1c; with nothing set or KHNMC_ASPECT=pc the hook is one flag test and
 * the lifted body.  KHNMC_ASPECT_TEST_OFF=h7 turns both halves off (the picture of task 42). */
#define H7_T 1.0                           /* GS rows / columns of tolerance at the band's edges */
#define H7_GAP 0.51                        /* R2: the largest gap between two stripes of a stack (GS rows) */
#define H7_STRIDE 0x1cu                    /* the 2D vertex: f_15fee0 and f_1969a0 ask f_111970 for blocks of stride 0x1c (0x140160076, 0x140196f62) */
#define H7_MAXV 4096                       /* a batch of more vertices is not examined (counted) */
#define H7_MAXQ (H7_MAXV/4)                /* so every quad of an examined batch has its box in h7_pre's tables */
#define H7_E 0.125                         /* GS units: a rectangle's corners against its box, and "exactly on a band edge" (R3); GS coordinates are multiples of 1/16 */
#define H7_R3_MIN 312.0                    /* R3: the rectangle is at least 3/4 of the band tall (GS rows) */
enum { H7K_FULL=0, H7K_PIECE, H7K_STACK, H7K_ALREADY, H7K_SITE, H7K_SHAPE, H7K_EDGE, H7K_BATCH, H7K_TITLE, H7K_WIPE, H7K_CAMP, H7K_RANK, H7K_N };
static const char *h7_kname[H7K_N]={"R1 stretched, full band width","R1 stretched, a band-height piece narrower than the band","R2 stretched, a quad of a batch whose wide stripes stack over the band height",
                                     "already covering the frame's height, left","a band-height rectangle or stripe stack from a draw routine that is not in H7's table, left",
                                     "the batch's box spans the band height but neither R1 nor R2 applies, left",
                                     "R3 moved to the frame's edge, a picture cut by a band edge",
                                     "a band-height rectangle whose batch covers the strips above and below it, left",
                                     "a part of the title's layout moved to its portrait place (task 53)","map-change wipe, outer tiles extended","portrait camp panel","rank animation at screen edge"};
/* the draw routines whose rectangles H7 may stretch, by the return address f_11a330 finds on the guest stack (it is entered by a jump
 * from f_11a2f0, so this is the address after `call 0x14011a2f0`).  Any other routine is left alone whatever it draws: the pppEi /
 * ppp effect routines (0x1404b3c85, 0x1404ade0d, 0x1404b8de6 ...) submit world-space quads through f_11a330 too, some with the same
 * 28-byte vertex (M, camp route: 196 batches from 0x1404b3c85 drawn with a 3D vertex shader, 1,568 of their quads band-height
 * rectangles by their numbers). */
typedef struct { u64 ra; const char *what; } H7Site;
static const H7Site h7site[]={
    {GS_BASE+0x16022cull,"f_15fee0, the GS sprite / rectangle draw (0x140160227): fades (H4 already extends its three), flat rectangles"},
    {GS_BASE+0x197033ull,"f_1969a0, the layout / sequence part draw, plain path (0x14019702e): the parts of one sequence frame as a quad list"},
    {GS_BASE+0x19701aull,"f_1969a0, the same with its blend path (0x140197015)"}};
#define H7_NSITE ((int)(sizeof h7site/sizeof *h7site))
static int h7_site_index(u64 ra){ for(int i=0;i<H7_NSITE;i++) if(h7site[i].ra==ra) return i; return -1; }
typedef struct { _Atomic u64 calls, off, identity, lazy, mode90, modeother, nobuf, fmt, count, batches, quads, small, kind[H7K_N], batch_ext, stack_batches, stack_other; } H7Stat;
static H7Stat h7;
typedef struct { u64 ra, vt; int kind; u32 prim, count; u64 n; float y0,y1,x0,x1; double smin,smax; Ivs fr; char name[72]; } H7Row;
#define H7_ROWS 96
static H7Row h7r[H7_ROWS]; static int n_h7r; static u64 h7r_cut;
static __thread int h7_in;                 /* 1 + kind while the lifted f_11a330 runs for a batch H7 classified (M: the draws made inside) */
static u64 h7_m_draws[H7K_N][16];        /* [kind][vertex-shader kind VK_*] (M, null backend) */
/* the smallest stretch about GS y 208 with which y0..y1 (y0 < 208 < y1) covers -b..416 + b; 1 when it already does */
static double h7_cover(double y0,double y1,double b){ double s=1.0, t=(208.0+b)/(208.0-y0), u=(208.0+b)/(y1-208.0); if(t>s) s=t; if(u>s) s=u; return s; }
static float h7_map(float y,double s){ return (float)(208.0+((double)y-208.0)*s); }
/* a quad's box bb = x0, x1, y0, y1 from its 4 corners (x, y) in the GS frame; 1 when it is an axis-aligned rectangle */
static int h7_box(const float (*v)[2],float *bb){
    float x0=v[0][0],x1=x0,y0=v[0][1],y1=y0; int nan=!(x0==x0)||!(y0==y0);
    for(int i=1;i<4;i++){ if(v[i][0]<x0) x0=v[i][0]; if(v[i][0]>x1) x1=v[i][0]; if(v[i][1]<y0) y0=v[i][1]; if(v[i][1]>y1) y1=v[i][1]; if(!(v[i][0]==v[i][0])||!(v[i][1]==v[i][1])) nan=1; }
    bb[0]=x0; bb[1]=x1; bb[2]=y0; bb[3]=y1;
    if(nan||!(x1>x0)||!(y1>y0)) return 0;
    for(int i=0;i<4;i++){ float dx0=v[i][0]-x0, dx1=x1-v[i][0], dy0=v[i][1]-y0, dy1=y1-v[i][1];
        if(!((dx0<=(float)H7_E||dx1<=(float)H7_E)&&(dy0<=(float)H7_E||dy1<=(float)H7_E))) return 0; }   /* rotated or skewed (more than 2 GS subpixels) */
    return 1;
}
/* R1 for one quad (rect = h7_box's result): -1 (not a rectangle spanning the band height: left), H7K_ALREADY, or H7K_FULL / H7K_PIECE
 * with *s = its factor.  r = the screen's ratio; xl / xr = the GS columns of the frame's left and right edge */
static int h7_r1(const float *bb,int rect,double r,double xl,double xr,double *s){
    if(!rect||!(bb[2]<=0.0+H7_T&&bb[3]>=416.0-H7_T)) return -1;
    *s=h7_cover(bb[2],bb[3],208.0*(r-1.0)); if(*s<=1.0) return H7K_ALREADY;
    return (bb[0]<=xl+H7_T&&bb[1]>=xr-H7_T)?H7K_FULL:H7K_PIECE;
}
/* review finding 2: 1 when the R1 rectangle q of a batch of n quads (boxes bb[i], rect[i]) need not be stretched because the batch
 * covers the strips itself: its other rectangles that are at least as wide as q's part inside the frame xl..xr and whose edge MEETS
 * q's top / bottom edge (or the edge reached so far) within H7_GAP carry q's column range beyond the frame's first and last row (b =
 * the strip's height in GS rows).  The map-change transition draws such a batch: a centre square, a rectangle above, one below, one
 * on each side.  A rectangle that overlaps or contains q is another layer, not a tile above or below it, and does not count (a
 * picture drawn over a larger black rectangle must still be stretched) */
static int h7_batch_covers(const float (*bb)[4],const u8 *rect,int n,int q,double b,double xl,double xr){
    double x0=bb[q][0]>xl?bb[q][0]:xl, x1=bb[q][1]<xr?bb[q][1]:xr, y0=bb[q][2], y1=bb[q][3];
    for(int it=0,ch=1;ch&&it<n;it++){ ch=0;
        for(int i=0;i<n;i++){ if(i==q||!rect[i]||(double)bb[i][0]>x0+H7_T||(double)bb[i][1]<x1-H7_T) continue;   /* not across q's columns */
            if((double)bb[i][2]<y0&&(double)bb[i][3]>=y0-H7_GAP&&(double)bb[i][3]<=y0+H7_GAP){ y0=bb[i][2]; ch=1; }
            if((double)bb[i][3]>y1&&(double)bb[i][2]<=y1+H7_GAP&&(double)bb[i][2]>=y1-H7_GAP){ y1=bb[i][3]; ch=1; } } }
    return h7_cover(y0,y1,b)<=1.0;
}
/* mapj's five tiles in submission order: picture, bottom, top, right, left. Only the caps' outer edges may move. */
static int h7_wipe_caps(const float bb[][4],const u8 *rect,double xl,double xr,double b){
    const float *c=bb[0], *lo=bb[1], *hi=bb[2], *r=bb[3], *l=bb[4];
    for(int i=0;i<5;i++) if(!rect[i]) return 0;
    return lo[0]==hi[0]&&lo[1]==hi[1]&&lo[0]==l[0]&&lo[1]==r[1]&&lo[0]<=xl&&lo[1]>=xr
        &&l[1]==c[0]&&r[0]==c[1]&&lo[2]==c[3]&&hi[3]==c[2]
        &&l[2]==c[2]&&r[2]==c[2]&&l[3]==c[3]&&r[3]==c[3]
        &&hi[2]<=0&&lo[3]>=416&&(hi[2]>-b||lo[3]<416+b);
}
/* R2 for a batch of n quads (boxes bb[i], rect[i]): 0 = its wide rectangles (as wide as the frame xl..xr less 2 T, anywhere in x) do
 * not stack over the band height; 1 = they do and cover the frame already; 2 = they do, *s = the stack's factor.  on[i] = 1 for the
 * quads the factor is for: the stack's stripes and every rectangle on a stripe's rows (y_min and y_max within T of one stripe's: a
 * further piece of the same picture row); any other quad of the batch is not part of the picture (review finding 4) */
static int h7_r2(const float (*bb)[4],const u8 *rect,int n,double r,double xl,double xr,double *s,u8 *on){
    float lo[H7_MAXQ], hi[H7_MAXQ]; int m=0, e;
    for(int i=0;i<n&&m<H7_MAXQ;i++) if(rect[i]&&(double)bb[i][1]-(double)bb[i][0]>=xr-xl-2.0*H7_T){ lo[m]=bb[i][2]; hi[m]=bb[i][3]; m++; }
    if(m<2) return 0;                                                                /* one wide band-height rectangle is R1's case */
    for(int a=1;a<m;a++){ float l=lo[a], h=hi[a]; int c=a; while(c>0&&lo[c-1]>l){ lo[c]=lo[c-1]; hi[c]=hi[c-1]; c--; } lo[c]=l; hi[c]=h; }
    double y0=lo[0], y1=hi[0];
    if(!(y0<=0.0+H7_T)) return 0;
    for(e=1;e<m&&y1<416.0-H7_T;e++){ if(lo[e]>y1+H7_GAP) return 0; if(hi[e]>y1) y1=hi[e]; }
    if(!(y1>=416.0-H7_T)) return 0;
    for(;e<m&&lo[e]<=y1+H7_GAP;e++) if(hi[e]>y1) y1=hi[e];                            /* stripes beyond the band belong to the stack too */
    for(int i=0;i<n;i++){ on[i]=0; if(!rect[i]) continue;                             /* the stack is the sorted stripes 0..e-1 */
        for(int a=0;a<e;a++){ double d0=(double)bb[i][2]-(double)lo[a], d1=(double)bb[i][3]-(double)hi[a]; if(d0<=H7_T&&d0>=-H7_T&&d1<=H7_T&&d1>=-H7_T){ on[i]=1; break; } } }
    *s=h7_cover(y0,y1,208.0*(r-1.0)); return *s<=1.0?1:2;
}
/* R3 for one quad that R1 left, in a batch R2 left (review finding 1): -1 / +1 = a rectangle narrower than the frame xl..xr, at least
 * H7_R3_MIN rows tall, whose top / bottom edge (and only that one) lies exactly on the band's: it is moved up / down by b; 0 = not */
static int h7_r3(const float *bb,int rect,double xl,double xr){
    if(!rect||!((double)bb[3]-(double)bb[2]>=H7_R3_MIN)) return 0;
    if((double)bb[1]-(double)bb[0]>=xr-xl-2.0*H7_T) return 0;                         /* as wide as the frame: a backdrop's piece, R1's and R2's business */
    int top=(double)bb[2]>=-H7_E&&(double)bb[2]<=H7_E, bot=(double)bb[3]>=416.0-H7_E&&(double)bb[3]<=416.0+H7_E;
    return top&&!bot?-1:bot&&!top?1:0;
}
static void h7_note(KCtx *g,u64 ra,int kind,u32 prim,u32 count,const float *bb,double s){
    u64 vt=(h3_depth>0&&h3_on[h3_depth-1])?h3_cur_vt[h3_depth-1]:0;
    pthread_mutex_lock(&mm); int i; for(i=0;i<n_h7r;i++) if(h7r[i].ra==ra&&h7r[i].vt==vt&&h7r[i].kind==kind&&h7r[i].prim==prim) break;
    if(i==n_h7r){ if(n_h7r<H7_ROWS){ H7Row *q=&h7r[i]; memset(q,0,sizeof *q); q->ra=ra; q->vt=vt; q->kind=kind; q->prim=prim; q->count=count; q->x0=bb[0]; q->x1=bb[1]; q->y0=bb[2]; q->y1=bb[3]; q->smin=q->smax=s;
            if(vt) rtti_name(g,vt,q->name,sizeof q->name); else snprintf(q->name,sizeof q->name,"(outside a dk object)"); n_h7r++; } else { h7r_cut++; pthread_mutex_unlock(&mm); return; } }
    H7Row *q=&h7r[i]; q->n++; if(bb[0]<q->x0) q->x0=bb[0]; if(bb[1]>q->x1) q->x1=bb[1]; if(bb[2]<q->y0) q->y0=bb[2]; if(bb[3]>q->y1) q->y1=bb[3]; if(s<q->smin) q->smin=s; if(s>q->smax) q->smax=s;
    iv_add(&q->fr,(u32)kh_game_frames());
    pthread_mutex_unlock(&mm);
}
/* the current vertex buffer object as f_11a330 finds it (0x14011a386..0x14011a3b2) */
static u64 h7_vbobj(KCtx *g){
    u64 d=rd64(g,GX+0x59b8);
    if(d){ if(!GA_OK(d+0x58)||!GA_OK(d+0x5b)) return 0; return d+0x60+0x58ull*(u64)(s64)(s32)rd32(g,d+0x58); }
    return GX+0x5790+0x58ull*(u64)rd32(g,GX+0x578c);
}
static void h7_quadlog(KCtx *g,u64 ra,u32 prim,int q,int nq,const float *bb,int kind,double s,double y0n,double y1n,u64 va,u32 stride);   /* M's draw log (below) */
/* Camp's SEQD panels meet at x=157, header ends at y=34, footer begins
 * at y=312. Use the whole submitted batch, including a text run's glyphs. */
static __thread u32 batch_ms[4];
static void camp_matrix(u32 ms[4],double b,const float box[4],u64 vt,int page){
    double height=416+2*b, cx=(box[0]+box[1])/2, scale=1.2;
    double px=256,py=208,tx=256,ty=208;
    if(box[3]<=35 && vt!=GS_BASE+0x5bab18){
        py=0; ty=20-b; scale=1.05;
    }else if((box[2]>=311 || (box[0]>=150 && box[2]>=303)) && vt!=GS_BASE+0x5bab18){
        py=405; ty=416+b-15; scale=1.05;
    }else if(page==36 || page==20){
        /* Config choices share rows with their labels. Party places a native 3D actor in its card. */
        scale=1.18;
        if(page==20) px=tx=75;
    }else if(height>=900){
        int left=vt==GS_BASE+0x5bab18 || vt==GS_BASE+0x5baba8 || cx<156;
        if(page==0 && !left){
            /* Keep the preview frame around the separate Tz::MenuObj camera. */
            px=tx=332; py=ty=175; scale=1.22;
        }else{
            scale=page==0?1.18:fmin(1.5,(height-190)/560);
            px=left?80:332; py=36;
            ty=60-b+(left?0:280*scale+14);
        }
    }
    double xs=bf(ms[0]),ys=bf(ms[1]);
    hud_ms(ms,scale,px,py);
    ms[2]=fbits((float)(bf(ms[2])+(tx-px)*xs));
    ms[3]=fbits((float)(bf(ms[3])+(ty-py)*ys));
}
/* H7 (b) at the entry of f_11a330: returns the kind (H7K_*) of a classified batch, -1 otherwise */
static int h7_pre(KCtx *g){
    h7.calls++;
    if(asp_off_h7){ h7.off++; return -1; }
    float W=rdf(g,GX+0x18), H=rdf(g,GX+0x1c); double o=(W>0&&H>0)?kh_aspect_o(W,H):0.0;
    if(o==0.0){ h7.identity++; return -1; }                                          /* 16:9 or wider: nothing is read or written */
    s32 count=(s32)(u32)KH_RDX(g); u32 prim=(u32)KH_R8(g);
    if(count<4||count>H7_MAXV){ h7.count++; return -1; }
    if(!rd32(g,GX+0xb4)||!rd32(g,GX+0xb8)){ h7.mode90++; return -1; }                 /* f_1121d0 stages MagicScale from GX+0x90 */
    float msx=rdf(g,GX+0xa0), msy=rdf(g,GX+0xa4), msz=rdf(g,GX+0xa8), msw=rdf(g,GX+0xac);
    double dw=(double)msw-o, dh=416.0*(double)msy-((double)H-2.0*o);
    int rank=H>W && h3_depth && h3_on[h3_depth-1] && h3_cur_vt[h3_depth-1]==GS_BASE+0x5b1df0 &&
             (style_scale!=1 || style_raise!=0);
    if((!rank && !(dw<=0.5&&dw>=-0.5&&dh<=0.5&&dh>=-0.5))||!(msx>0)){ h7.modeother++; return -1; }
    if(!rd8(g,GX+0x4c0)&&rd64(g,GX+0x4b0)){ h7.lazy++; return -1; }                   /* the body's lazy begin is pending: left to the exe */
    u64 B=h7_vbobj(g); if(!B||!GA_OK(B)||!GA_OK(B+0x57)){ h7.nobuf++; return -1; }
    u32 stride=rd32(g,B+0x8); u64 vp=rd64(g,B+0x48);
    if(stride!=H7_STRIDE){ h7.fmt++; return -1; }                                    /* not the 2D vertex */
    /* the block must be the one f_121ff0 handed out for this batch: base [B+0x40] + the write offset [B+0x38] (0x1401220ef /
     * 0x1401220f1), at least `count` vertices long ([B+0x50] = the count asked for, 0x1401220d0) and inside the buffer's capacity
     * [B+0x30] (0x1401220e5).  With a real graphics backend the buffer is the memory ID3D12Resource::Map returned (runtime/gfx_wrap.c:
     * host - bias), which is not guest-arena memory: the game has just written these very bytes, so they are read and written through
     * the same address and never tested with GA_OK. */
    { u64 base=rd64(g,B+0x40), cap=rd64(g,B+0x30); u32 off=rd32(g,B+0x38); s32 asked=(s32)rd32(g,B+0x50);
      if(!base||!vp||vp!=base+off||count>asked||(u64)off+(u64)count*stride>cap){ h7.nobuf++; return -1; } }
    h7.batches++;
    float ox=rdf(g,GX+0x138), oy=rdf(g,GX+0x13c);
    if(rank){
        /* The resting letter keeps its requested centre. Clamp only frames of the zoom animation that reach an edge. */
        float left=W,right=0,top=H,bottom=0;
        for(int i=0;i<count;i++){
            float x=(rdf(g,vp+(u64)i*stride)-ox)*msx+msz;
            float y=(rdf(g,vp+(u64)i*stride+4)-oy)*msy+msw;
            left=fminf(left,x); right=fmaxf(right,x);
            top=fminf(top,y); bottom=fmaxf(bottom,y);
        }
        if(left>=0 && right<=W && top>=0 && bottom<=H) return -1;
        memcpy(batch_ms,hp(g,GX+0xa0),sizeof batch_ms);
        batch_ms[2]=fbits(msz+fmaxf(0,-left)+fminf(0,W-right));
        batch_ms[3]=fbits(msw+fmaxf(0,-top)+fminf(0,H-bottom));
        return H7K_RANK;
    }
    double r=kh_aspect_r(W,H), b=208.0*(r-1.0), xl=(0.0-(double)msz)/(double)msx, xr=((double)W-(double)msz)/(double)msx, s=1.0;
    u64 ra=rd64(g,KH_RSP(g)); int site=h7_site_index(ra), best=-1, wrote=0, k2=0, nq=((prim==8&&!(count&3))||count==4)?count/4:0;
    float ab[4]={1e30f,-1e30f,1e30f,-1e30f}, qb[H7_MAXQ][4]; u8 qr[H7_MAXQ], qon[H7_MAXQ]; signed char qk[H7_MAXQ];   /* the batch's box; per quad its box, rectangle flag, R2 mask, kind */
    /* every quad's box first: R1's "covered by its batch" test, R2 and R3 need the whole batch */
    for(int q=0;q<nq;q++){ float v[4][2]; u64 a=vp+(u64)q*4u*stride; float *bb=qb[q];
        for(int i=0;i<4;i++){ v[i][0]=rdf(g,a+(u64)i*stride)-ox; v[i][1]=rdf(g,a+(u64)i*stride+4)-oy; }
        qr[q]=(u8)h7_box(v,bb); h7.quads++;
        if(bb[0]<ab[0]) ab[0]=bb[0]; if(bb[1]>ab[1]) ab[1]=bb[1]; if(bb[2]<ab[2]) ab[2]=bb[2]; if(bb[3]>ab[3]) ab[3]=bb[3]; }
    if(!menu_band && H>W && h3_depth && h3_on[h3_depth-1] &&
       rd8(g,GS_BASE+0x9006b0) && rd32(g,GS_BASE+0x900724)==8 &&
       (ra==GS_BASE+0x197033 || ra==GS_BASE+0x19701a || ra==GS_BASE+0x1977e3 || ra==GS_BASE+0x3aff0f)){
        if(!nq){
            for(int i=0;i<count;i++){
                u64 p=vp+(u64)i*stride;
                float x=rdf(g,p)-ox,y=rdf(g,p+4)-oy;
                ab[0]=fminf(ab[0],x); ab[1]=fmaxf(ab[1],x);
                ab[2]=fminf(ab[2],y); ab[3]=fmaxf(ab[3],y);
            }
        }
        if(ab[3]-ab[2]<416){ /* full-height stripes, side trims and dims retain H7's backdrop layout */
            memcpy(batch_ms,hp(g,GX+0xa0),sizeof batch_ms);
            camp_matrix(batch_ms,b,ab,h3_cur_vt[h3_depth-1],rd32(g,GS_BASE+0xbee64c));
            h7.kind[H7K_CAMP]++;
            return H7K_CAMP;
        }
    }
    if(ra==GS_BASE+0x197033ull&&nq==5&&!h3_depth&&h7_wipe_caps(qb,qr,xl,xr,b)){
        for(int q=0;q<5;q++){
            const float *bb=qb[q]; u64 a=vp+(u64)q*4u*stride;
            float top=q==2&&bb[2]>-b?(float)-b:bb[2];
            float bottom=q==1&&bb[3]<416+b?(float)(416+b):bb[3];
            if(top!=bb[2]||bottom!=bb[3]){
                for(int i=0;i<4;i++){ u64 y=a+(u64)i*stride+4;
                    wrf(g,y,oy+(rdf(g,y)-oy==bb[2]?top:bottom)); }
                h7.kind[H7K_WIPE]++; h7_note(g,ra,H7K_WIPE,prim,(u32)count,bb,1.0);
            }
            if(__builtin_expect(kh_aspect_m,0)) h7_quadlog(g,ra,prim,q,nq,bb,H7K_WIPE,1.0,top,bottom,a,stride);
        }
        h7.batch_ext++; return H7K_WIPE;
    }
    /* task 53 item 2: the parts of the title's two layout objects (the current dk object is [0x140b1d5d0] or [0x140b1d5d8] while the
     * title task runs) to their portrait places, on a screen taller than wide; R1 / R2 / R3 do not touch them */
    u8 qt[H7_MAXQ]; memset(qt,0,(size_t)(nq>0?nq:0));
    { u64 ob=(h3_depth>0&&h3_on[h3_depth-1])?h3_cur_obj[h3_depth-1]:0;
      if(ob&&nq&&W>0&&H>W&&kh_phase_title_alive()){
        if(ob==rd64(g,T_LAYOUT_PTR)||ob==rd64(g,T_MENU_PTR)){ TtLay L; int ts=0;
            if(kh_aspect_title_layout(W,H,&L,&ts)){ int tsite=ra==GS_BASE+0x197033ull||ra==GS_BASE+0x19701aull||ra==GS_BASE+0x1977e3ull; ttl.batches++;   /* f_1969a0, the layout draw */
                if(!atomic_exchange(&ttl_logged,1)) fprintf(stderr,"kh: aspect: title layout (task 53): first portrait batch: screen %gx%g: picture x%.4f to GS (256, %.2f), logo x%.2f to (256, %.2f), menu to y %.2f, GAME SETTINGS to y %.2f\n",
                                                            W,H,L.s[TT_PIC],L.ty[TT_PIC],L.s[TT_LOGO],L.ty[TT_LOGO],L.ty[TT_MENU],L.ty[TT_SET]);
                for(int q=0;q<nq;q++){ int e=tsite?kh_aspect_title_elem(qb[q]):-1; if(e<0){ ttl.left++; continue; }
                    u64 a=vp+(u64)q*4u*stride; double sc=L.s[e], y0n=L.ty[e]+sc*((double)qb[q][2]-tt_py[e]), y1n=L.ty[e]+sc*((double)qb[q][3]-tt_py[e]);
                    double pivot=e==TT_LOGO ? title_logo_pivot(qb[q]) : tt_px[e];
                    for(int i=0;i<4;i++){ u64 p=a+(u64)i*stride;
                        if(e==TT_PIC||e==TT_LOGO) wrf(g,p,(float)(L.tx[e]+sc*((double)(rdf(g,p)-ox)-pivot))+ox);      /* the menu rows keep x */
                        wrf(g,p+4,(float)(L.ty[e]+sc*((double)(rdf(g,p+4)-oy)-tt_py[e]))+oy); }
                    if(__builtin_expect(kh_aspect_m,0)) h7_quadlog(g,ra,prim,q,nq,qb[q],H7K_TITLE,sc,y0n,y1n,a,stride);
                    qt[q]=1; wrote=1; ttl.q[e]++;
                    pthread_mutex_lock(&mm); iv_add(&ttl.fr[e],(u32)kh_game_frames()); pthread_mutex_unlock(&mm); } }
            else if(ts) ttl.tooshort++; }
        else if(h3_cur_vt[h3_depth-1]==GS_BASE+0x5b1dc0ull) ttl.other+=(u64)nq; } }       /* another Tz::Layout while the title runs: left, counted */
    /* R1, quad by quad */
    for(int q=0;q<nq;q++){ const float *bb=qb[q]; u64 a=vp+(u64)q*4u*stride;
        if(qt[q]){ qk[q]=(signed char)H7K_TITLE; h7.kind[H7K_TITLE]++; if(best<0||H7K_TITLE<best) best=H7K_TITLE; continue; }   /* task 53: moved above */
        int kind=h7_r1(bb,qr[q],r,xl,xr,&s);
        if((kind==H7K_FULL||kind==H7K_PIECE)&&nq>1&&h7_batch_covers((const float (*)[4])qb,qr,nq,q,b,xl,xr)) kind=H7K_BATCH;   /* review finding 2 */
        if(kind>=0&&site<0) kind=H7K_SITE;                                           /* not one of the 2D draw routines of the table: never written */
        int st=kind==H7K_FULL||kind==H7K_PIECE; qk[q]=(signed char)kind;
        if(__builtin_expect(kh_aspect_m,0)) h7_quadlog(g,ra,prim,q,nq,bb,kind,st?s:1.0,st?h7_map(bb[2],s):bb[2],st?h7_map(bb[3],s):bb[3],a,stride);
        if(st){ for(int i=0;i<4;i++){ u64 p=a+(u64)i*stride+4; wrf(g,p,h7_map(rdf(g,p)-oy,s)+oy); } wrote=1; }
        if(kind<0){ h7.small++; continue; }
        h7.kind[kind]++; h7_note(g,ra,kind,prim,(u32)count,bb,st?s:1.0); if(best<0||kind<best) best=kind; }
    /* R2: no quad of the batch is a band-height rectangle.  Only the stack's stripes and the rectangles on their rows are changed */
    if(best<0&&nq>=2&&(k2=h7_r2((const float (*)[4])qb,qr,nq,r,xl,xr,&s,qon))!=0){
        int st, non=0; best=site<0?H7K_SITE:k2==1?H7K_ALREADY:H7K_STACK; st=best==H7K_STACK; if(st) h7.stack_batches++;
        for(int q=0;q<nq;q++){ u64 a=vp+(u64)q*4u*stride; if(!qon[q]){ if(st) h7.stack_other++; continue; }
            non++; qk[q]=(signed char)best;
            if(__builtin_expect(kh_aspect_m,0)) h7_quadlog(g,ra,prim,q,nq,qb[q],best,st?s:1.0,st?h7_map(qb[q][2],s):qb[q][2],st?h7_map(qb[q][3],s):qb[q][3],a,stride);
            if(st){ for(int i=0;i<4;i++){ u64 p=a+(u64)i*stride+4; wrf(g,p,h7_map(rdf(g,p)-oy,s)+oy); } wrote=1; }
            h7_note(g,ra,best,prim,(u32)count,qb[q],st?s:1.0); }
        h7.small-=(u64)non; h7.kind[best]+=(u64)non; }
    /* R3: a picture cut by a band edge, among the quads R1 left in a batch that is not a stripe stack: moved to the frame's edge */
    if(!k2) for(int q=0;q<nq;q++){ int d; if(qk[q]>=0||!(d=h7_r3(qb[q],qr[q],xl,xr))) continue;
        u64 a=vp+(u64)q*4u*stride; int kind=site<0?H7K_SITE:H7K_EDGE; double dy=(double)d*b; qk[q]=(signed char)kind;
        if(__builtin_expect(kh_aspect_m,0)) h7_quadlog(g,ra,prim,q,nq,qb[q],kind,1.0,kind==H7K_EDGE?(double)qb[q][2]+dy:qb[q][2],kind==H7K_EDGE?(double)qb[q][3]+dy:qb[q][3],a,stride);
        if(kind==H7K_EDGE){ for(int i=0;i<4;i++){ u64 p=a+(u64)i*stride+4; wrf(g,p,(float)((double)(rdf(g,p)-oy)+dy)+oy); } wrote=1; }
        h7.small--; h7.kind[kind]++; h7_note(g,ra,kind,prim,(u32)count,qb[q],kind==H7K_EDGE?dy:1.0); if(best<0||kind<best) best=kind; }
    if(!nq) for(s32 i=0;i<count;i++){ float x=rdf(g,vp+(u64)i*stride)-ox, y=rdf(g,vp+(u64)i*stride+4)-oy;   /* another primitive: its box only (never changed) */
        if(x<ab[0]) ab[0]=x; if(x>ab[1]) ab[1]=x; if(y<ab[2]) ab[2]=y; if(y>ab[3]) ab[3]=y; }
    if(best<0&&ab[2]<=0.0+H7_T&&ab[3]>=416.0-H7_T){ best=H7K_SHAPE; h7.kind[best]++; h7_note(g,ra,best,prim,(u32)count,ab,1.0); }
    if(wrote) h7.batch_ext++;
    return best;
}
/* f_3afd90 receives the complete laid-out page. Its alignment (text+0x80)
 * and measured dimensions (+0x20/+0x28) precede the per-glyph draw loop. */
void orig_f_3afd90(KCtx *);
void f_3afd90(KCtx *g){
    int d=h3_depth-1;
    if(!asp_mode || d<0 || !h3_on[d] || h3_cur_vt[d]!=GS_BASE+0x5c5180){
        orig_f_3afd90(g); return;
    }
    double W=rdf(g,GX+0x18),H=rdf(g,GX+0x1c);
    u64 text=A0(g);
    double width=rd64(g,text+0x20),height=rd64(g,text+0x28);
    if(H<=W || W<=0 || !width || !height){ orig_f_3afd90(g); return; }
    int align=rd32(g,text+0x80);
    double x=(s32)A1(g),y=(s32)A2(g);
    double left=x-(align==1?width:(align>=2 && align<=4)?floor(width/2):0);
    double bottom=y+(align==4 || align==5?0:align==3?height-floor(height/2):height);
    u32 save[4],ms[4]; memcpy(save,hp(g,GX+0xa0),sizeof save); memcpy(ms,save,sizeof ms);
    double sx=bf(ms[0]),sy=bf(ms[1]);
    if(sx<=0 || sy<=0){ orig_f_3afd90(g); return; }
    /* Native widths count advances. The last 32-GS glyph quad can extend past its advance. */
    double scale=fmin(sub_scale,fmin(W/(sx*(width+32)),H*(1-sub_lower)/(sy*(height+2))));
    hud_ms(ms,scale,(float)(left+width/2),(float)bottom);
    double dx=bf(ms[0]),dy=bf(ms[1]);
    ms[2]=fbits((float)fmax(-left*dx,fmin(bf(ms[2]),W-(left+width+32)*dx)));
    /* For event cues the measured baseline ends at GS 400, the last glyph quad at 402.
     * A taller text object already contains all its wrapped rows. */
    double rows=(align==4 && y>=385 && y<=400)?402-y:2;
    ms[3]=fbits((float)fmax(-(bottom-height)*dy,H*(1-sub_lower)-(bottom+rows)*dy));
    memcpy(hp(g,GX+0xa0),ms,sizeof ms);
    orig_f_3afd90(g);
    memcpy(hp(g,GX+0xa0),save,sizeof save);
}
/* f_1969a0 keeps the active sprite table at [rdi+8] and the first part at
 * [rdi+0x28]. In title LAYD sequence 7 the 39 sprites precede the parts.
 * Groups 8, 9 and 10 are the underline pair and the two credit rows. */
static int title_credit_batch(KCtx *g,u64 ra,u64 seq,u32 vertices){
    if(ra!=GS_BASE+0x197033 && ra!=GS_BASE+0x19701a) return 0;
    if(!seq || (vertices!=4 && vertices!=8)) return 0;
    u64 sprites=rd64(g,seq+8), part=rd64(g,seq+0x28);
    u64 parts=sprites+39*44;
    if(vertices==8)
        return part==parts+8*20 && rd32(g,part+16)==28 && rd32(g,part+36)==27;
    return (part==parts+10*20 && rd32(g,part+16)==20) ||
           (part==parts+11*20 && rd32(g,part+16)==21);
}
void orig_f_11a330(KCtx *);
void f_11a330(KCtx *g){
    if(__builtin_expect(hide_title_credit,0) && kh_phase_title_alive() &&
       title_credit_batch(g,rd64(g,KH_RSP(g)),KH_RDI(g),(u32)KH_RDX(g))){
        KH_RDX(g)=0; /* the exe's empty submission path still finalizes the vertex buffer */
        hidden_credit_batches++;
        orig_f_11a330(g);
        return;
    }
    if(__builtin_expect(!asp_mode,1)){ orig_f_11a330(g); return; }                    /* nothing set / pc: exactly the exe */
    int k=h7_pre(g);
    if(k<0){ orig_f_11a330(g); return; }
    int sv=h7_in; h7_in=1+k;
    u32 saved[4];
    if(k==H7K_CAMP || k==H7K_RANK){ memcpy(saved,hp(g,GX+0xa0),sizeof saved); memcpy(hp(g,GX+0xa0),batch_ms,sizeof saved); }
    orig_f_11a330(g);
    if(k==H7K_CAMP || k==H7K_RANK) memcpy(hp(g,GX+0xa0),saved,sizeof saved);
    h7_in=sv;
}
static void h7_report(FILE *f){
    fprintf(f,"kh: aspect report: H7 (task 46: no bars on any frame, backdrops stretched to the frame's height; 0x11a330): H1 rewrites on frames without the 3D view %llu; dk passes anchored on such frames %llu; "
              "2D batches %llu: test aid off %llu; o = 0 (16:9 or wider), nothing read %llu; fewer than 4 or more than 4096 vertices (left) %llu; MagicScale from GX+0x90 (left) %llu; "
              "MagicScale not the centred band (left) %llu; the body's lazy begin pending (left) %llu; not the 28-byte 2D vertex (left) %llu; vertex block not the one f_121ff0 handed out (h7-vertex-block, left) %llu; examined %llu batches, %llu quads: "
              "not a backdrop (left) %llu; stretched %llu (R1 full band width %llu, R1 band-height pieces %llu, R2 quads of stripe stacks %llu in %llu batches) and moved to the frame's edge (R3, pictures cut by a band edge) %llu, in %llu batches; already covering the frame's height (left) %llu; "
              "band-height rectangles covered by their own batch (left) %llu; quads of stripe-stack batches that are not on a stripe's rows (left) %llu; "
              "backdrop shapes of a draw routine not in the table (left, listed) %llu; batches spanning the band height without R1 or R2 (left, listed) %llu\n",
            P(h7_h1_no3d),P(h7_h3_no3d),P(h7.calls),P(h7.off),P(h7.identity),P(h7.count),P(h7.mode90),P(h7.modeother),P(h7.lazy),P(h7.fmt),P(h7.nobuf),P(h7.batches),P(h7.quads),P(h7.small),
            P(h7.kind[H7K_FULL]+h7.kind[H7K_PIECE]+h7.kind[H7K_STACK]),P(h7.kind[H7K_FULL]),P(h7.kind[H7K_PIECE]),P(h7.kind[H7K_STACK]),P(h7.stack_batches),P(h7.kind[H7K_EDGE]),P(h7.batch_ext),P(h7.kind[H7K_ALREADY]),
            P(h7.kind[H7K_BATCH]),P(h7.stack_other),P(h7.kind[H7K_SITE]),P(h7.kind[H7K_SHAPE]));
    pthread_mutex_lock(&mm);
    for(int i=0;i<n_h7r;i++){ H7Row *q=&h7r[i];
        fprintf(f,"kh: aspect report: H7 %s: %s: from return address 0x%llx, dk object %s: %llu (primitive %u, %u vertices in the first batch), GS frame as submitted x %g..%g, y %g..%g",
                q->kind==H7K_SHAPE?"batch":"quad",h7_kname[q->kind],P(q->ra),q->name,P(q->n),q->prim,q->count,q->x0,q->x1,q->y0,q->y1);
        if(q->kind<=H7K_STACK) fprintf(f,", factor %.4f..%.4f",q->smin,q->smax);
        if(q->kind==H7K_EDGE) fprintf(f,", moved by %.4f..%.4f GS rows",q->smin,q->smax);
        fputc(';',f); iv_print(f,&q->fr); fputc('\n',f); }
    if(h7r_cut) fprintf(f,"kh: aspect report: H7 quad: %llu of further (kind, return address, class) rows (table full)\n",P(h7r_cut));
    pthread_mutex_unlock(&mm);
    if(h7.nobuf) fprintf(f,"kh: aspect: WARNING: h7-vertex-block: %llu 2D batches in the band's mode whose vertex block is not base + offset of the current vertex buffer object, or longer than what f_121ff0 handed out: left as the exe made them\n",P(h7.nobuf));
    if(h7.kind[H7K_SITE]) fprintf(f,"kh: aspect: NOTE: h7-unlisted-site: %llu quads with a backdrop's shape came from draw routines that are not in H7's table and were left as the exe made them (see the `H7 quad: a band-height rectangle or stripe stack from a draw routine` lines)\n",P(h7.kind[H7K_SITE]));
}

/* ---- M: the dk draw loop and the draws ----------------------------------------------------------------------------------------- */
enum { VK_NONE, VK_586160, VK_586710, VK_586c80, VK_5871b0, VK_593b10, VK_59ac40, VK_3D, VK_UNKNOWN, VK_N };
static const char *vk_name[VK_N]={"no-pso","586160","586710","586c80","5871b0","593b10","59ac40","3d","unknown-vs"};
static int vk_ms(int k){ return k==VK_586160||k==VK_586710||k==VK_586c80; }      /* the VS that read MagicScale.zw (plan section 5) */
/* yreach / yout: MagicScale draws whose GS y range reaches beyond the 16:9 frame 0..416 / lies wholly beyond it, and when (task 40
 * review finding 4: dk::COMMAND_ONE's range 290..637 runs past the screen; these say how many draws and in which frames) */
/* task 42 (the anchor table's evidence): per class the dk groups (+0x14) of its drawn objects, where its MagicScale draws' staged
 * MagicScale came from (GX+0xa0 = what H3 writes, GX+0x90 = a row H3 does not move, neither), and a histogram of its draws' GS y
 * mid-points (8-row bins from GS y -16: the resting place of an animated element such as the style rank) */
#define YH_N 56
typedef struct { u64 vt; char name[72]; u64 seen, skipped, first, nested, draws[VK_N], ydraws, ycb, ystg, yreach, yout; int pmin,pmax; float ymin,ymax,xmin,xmax,pymin,pymax; Ivs fr, fryr; u8 snap[0x40]; int have_snap;
                 u32 grpv[6]; u64 grpn[6], grpcut, mss[4], yh[YH_N], grpd[6]; float grpy0[6], grpy1[6]; } DkCls;
static DkCls dkc[192]; static int n_dkc; static u64 dkc_cut, dk_passes, dk_passes_nested, dk_objects, dk_out_draws[VK_N], dk_gap_draws[VK_N];
static u32 dk_tid, draw_tid; static u64 draws_other_thread;
static float xyo[8][2]; static u64 xyo_n[8]; static int n_xyo; static u64 ms_novb, ms_nocb;
/* task 40 review (finding 4): where each MagicScale draw's xyOffset / MagicScale came from (runtime/null_d3d12.c m_draw): 1 = a bound
 * root CBV holding exactly the rows f_1121d0 staged in GX (+0x59f0 xyOffset, +0x5a00 MagicScale), 2 = a bound root CBV whose
 * MagicScale row is GX+0xa0 (the old rule) but whose rows are NOT the staged ones (its values used), 3 = the staged rows only (no bound
 * root CBV holds them: f_1121d0 binds cbConstantVS through a descriptor table, SetGraphicsRootDescriptorTable); the staged MagicScale
 * equals GX+0xa0 / GX+0x90 / neither (f_1121d0 copies this+0x90 instead of +0xa0 under the test at 0x140112396); draws on a list
 * with no root CBV bound at all */
static u64 ms_src[4], ms_mssrc[4], ms_nocbv;
typedef struct { u64 ra; u64 n; } NRa;
static NRa nra[32]; static int n_nra;
static __thread int dk_in, dk_pos, dk_cur=-1, dk_depth, dk_last_cls=-1, dk_prev_other=-1; static __thread u64 dk_obj;
/* per drawn instance (finding 4: the generic children's owners): object, class, the class of the nearest preceding object of ANOTHER
 * class in the pass, up to 3 fields in +0x08..+0x78 that point to an object with an RTTI class name (owner candidates) */
typedef struct { u64 obj; int cls, prevcls, np; u16 poff[3]; int pcls[3]; u64 draws, ydraws; int pmin,pmax; float ymin,ymax,pymin,pymax; Ivs fr; u8 snap[0x40]; } DkInst;
#define NINST 3072
static DkInst dki[NINST]; static int n_dki; static u64 dki_cut; static int dki_hash[8192];
/* the MagicScale draws outside dk objects, by the return addresses on the guest stack (finding 4: which HUD pieces are drawn outside
 * the dk loop) */
typedef struct { u64 ra[4]; int nra, where; u64 n, kinds[VK_N], ydraws; float ymin,ymax,pymin,pymax; Ivs fr; } ChainG;
#define NCG 192
static ChainG cg[NCG]; static int n_cg; static u64 cg_cut, cg_nochain;
static u64 text_lo, text_hi; static int text_state;                      /* the image's executable sections: 0 not read yet, 1 read, 2 not found */
/* KHNMC_ASPECT_M_DRAWLOG=FILE@FROM-TO[,FROM-TO...] (task 40 review finding 5, a diagnostic of M): every draw of those presents, one line each */
static FILE *dl_f; static char dl_path[600]; static u64 dl_lo[8], dl_hi[8]; static int dl_n; static u64 dl_lines;
/* M: occlusion volumes (task 40 review finding 5: H2b removed 8 draws at 5 story presents).  f_170450 calls the map pass f_186920
 * (0x1401704a1), then zeroes rctx+0xa08 and calls f_185930(occluders [0x140abd460], eye, rctx+0x508, rctx+0xa08, rctx+0x488)
 * (0x1401704b6..0x170512; 0x16fc30 is a second such caller), rctx = [0x140abd440].  Every occluder polygon that faces the eye and
 * whose box f_186130 does not cull with the clip rctx+0x488 adds one volume: 5 planes, 0x50 bytes (f_186280), at rctx+0x508 + 0x50 n.
 * f_187490 (0x187490) then culls an object whose box lies wholly inside one of them (f_185e50, the p-vertex test on 5 planes).
 * rctx+0x488 is the map pass's clip, which H2b's edit leaves wide (plan 4.4), so occluders in the strips count too.  rctx+0x508 has
 * 16 slots (the init at 0x140186428 fills 0x10) and f_185930 has no bound check: a 17th volume would overwrite the count at +0xa08.
 * Sampled at each map pass: the count the previous frame left, attributed to the present of that previous map pass, and only when
 * rctx is the one the previous map pass had (a first run on the stories' map changes showed 0xffffffff in a new rctx at 4 map
 * passes, in pc as in fill: not a count f_185930 left; those are counted as "rctx changed", not sampled). */
#define RCTX_GLOBAL (GS_BASE+0xabd440ull)
#define OCC_SLOTS 16u
typedef struct { u64 n, norctx, newrc, hist[OCC_SLOTS+2], over, maxp, prevp, prevrc; u32 max, prevfr; Ivs frover; int nat, nov; u64 atp[96], ovp[8]; u32 atn[96], ovn[8]; } OccSt;
static OccSt occ;
/* one sample: n volumes at present p (game frame fr); inlog = p is in a KHNMC_ASPECT_M_DRAWLOG range (its count is listed) */
static void occ_note(OccSt *s,u64 p,u32 fr,u32 n,int inlog){
    s->n++; s->hist[n>OCC_SLOTS?OCC_SLOTS+1:n]++;
    if(n>s->max){ s->max=n; s->maxp=p; }
    if(n>OCC_SLOTS){ s->over++; iv_add(&s->frover,fr); if(s->nov<8){ s->ovp[s->nov]=p; s->ovn[s->nov]=n; s->nov++; } }
    if(inlog&&s->nat<96){ s->atp[s->nat]=p; s->atn[s->nat]=n; s->nat++; }
}
static void m_occ(KCtx *g){
    u64 rc=rd64(g,RCTX_GLOBAL), p=atomic_load(&gfx_presents)+1; u32 fr=(u32)kh_game_frames();
    pthread_mutex_lock(&mm);
    if(occ.prevp){
        if(!rc||!GA_OK(rc+0xa08)||!GA_OK(rc+0xa0b)) occ.norctx++;
        else if(rc!=occ.prevrc) occ.newrc++;
        else { int inlog=0; for(int r=0;r<dl_n;r++) if(occ.prevp>=dl_lo[r]&&occ.prevp<=dl_hi[r]) inlog=1;
            occ_note(&occ,occ.prevp,occ.prevfr,rd32(g,rc+0xa08),inlog); } }
    occ.prevp=p; occ.prevfr=fr; occ.prevrc=rc;
    pthread_mutex_unlock(&mm);
}
static void rtti_name(KCtx *g,u64 vt,char *out,size_t n){
    out[0]=0;
    if(vt<GS_BASE+0x1000||vt>=GS_BASE+0x900000||!GA_OK(vt-8)) { snprintf(out,n,"(vtable outside the image)"); return; }
    u64 col=rd64(g,vt-8); if(col<GS_BASE||col>=GS_BASE+0x900000||!GA_OK(col+24)||rd32(g,col)!=1){ snprintf(out,n,"(no RTTI)"); return; }
    u64 td=GS_BASE+rd32(g,col+12); size_t k=0;
    if(td<GS_BASE||td>=GS_BASE+0x900000) { snprintf(out,n,"(no RTTI)"); return; }
    for(;k+1<n;k++){ u8 c=rd8(g,td+16+k); if(c<32||c>126) break; out[k]=(char)c; } out[k]=0;
}
static int dk_class(KCtx *g,u64 obj){              /* under mm */
    u64 vt=GA_OK(obj)&&GA_OK(obj+0x17)?rd64(g,obj):0; int i;
    for(i=0;i<n_dkc;i++) if(dkc[i].vt==vt) return i;
    if(n_dkc>=192){ dkc_cut++; return -1; }
    DkCls *c=&dkc[n_dkc]; memset(c,0,sizeof *c); c->vt=vt; c->pmin=1<<30; c->pmax=-1; c->ymin=c->xmin=c->pymin=1e30f; c->ymax=c->xmax=c->pymax=-1e30f;
    if(vt) rtti_name(g,vt,c->name,sizeof c->name); else snprintf(c->name,sizeof c->name,"(object not readable)");
    return n_dkc++;
}
static void dk_set(KCtx *g,u64 obj){
    if(!obj){ dk_cur=-1; dk_obj=0; return; }
    pthread_mutex_lock(&mm); int i=dk_class(g,obj); dk_objects++;
    if(i>=0){ DkCls *c=&dkc[i]; c->seen++; if(dk_pos==0) c->first++; if(dk_pos<c->pmin) c->pmin=dk_pos; if(dk_pos>c->pmax) c->pmax=dk_pos;
        if(rd8(g,obj+0x10)&0x42) c->skipped++; iv_add(&c->fr,(u32)kh_game_frames());
        if(!c->have_snap&&GA_OK(obj)&&GA_OK(obj+0x3f)&&!(rd8(g,obj+0x10)&0x42)){ memcpy(c->snap,hp(g,obj),0x40); c->have_snap=1; }   /* the first drawn object's bytes (owner field: plan section 5) */
        if(GA_OK(obj+0x17)&&!(rd8(g,obj+0x10)&0x42)){ u32 gr=rd32(g,obj+0x14); int k;                                                     /* task 42: its dk group */
            for(k=0;k<6;k++) if(c->grpn[k]&&c->grpv[k]==gr) break; if(k==6) for(k=0;k<6;k++) if(!c->grpn[k]) break;
            if(k<6){ c->grpv[k]=gr; c->grpn[k]++; } else c->grpcut++; } }
    pthread_mutex_unlock(&mm);
    if(i!=dk_last_cls) dk_prev_other=dk_last_cls; dk_last_cls=i;                       /* the class of the nearest preceding object of another class */
    dk_cur=i; dk_obj=obj; dk_pos++;
}
/* the per-instance record of the current object (under mm); -1 when the table is full */
static int dk_inst(KCtx *g){
    u64 o=dk_obj; u32 h=(u32)((o>>4)*0x9e3779b1u)&8191u;
    for(int k=0;k<8192;k++){ int j=dki_hash[(h+k)&8191u]; if(!j) break; DkInst *d=&dki[j-1]; if(d->obj==o&&d->cls==dk_cur) return j-1; if(k==8191) return -1; }
    if(n_dki>=NINST){ dki_cut++; return -1; }
    DkInst *d=&dki[n_dki]; memset(d,0,sizeof *d); d->obj=o; d->cls=dk_cur; d->prevcls=dk_prev_other; d->pmin=1<<30; d->pmax=-1; d->ymin=d->pymin=1e30f; d->ymax=d->pymax=-1e30f;
    if(GA_OK(o)&&GA_OK(o+0x7f)){ memcpy(d->snap,hp(g,o),0x40);
        for(u32 off=8;off<0x80&&d->np<3;off+=8){ u64 p=rd64(g,o+off); if(p<0x10000||!GA_OK(p)||!GA_OK(p+0x17)||p==o) continue;
            u64 vt=rd64(g,p); if(vt<GS_BASE+0x1000||vt>=GS_BASE+0x900000||!GA_OK(vt-8)) continue;
            char nm[72]; rtti_name(g,vt,nm,sizeof nm); if(strncmp(nm,".?AV",4)) continue;
            int pc=dk_class(g,p); if(pc<0) continue; d->poff[d->np]=(u16)off; d->pcls[d->np]=pc; d->np++; } }
    for(int k=0;k<8192;k++){ u32 s=(h+(u32)k)&8191u; if(!dki_hash[s]){ dki_hash[s]=n_dki+1; break; } }
    return n_dki++;
}
/* the image's executable sections, from its PE header at the image base (for the return-address test) */
static void text_range(KCtx *g){
    text_state=2; if(!GA_OK(GS_BASE+0x40)||rd16(g,GS_BASE)!=0x5a4d) return;
    u64 nt=GS_BASE+rd32(g,GS_BASE+0x3c); if(!GA_OK(nt+24)||rd32(g,nt)!=0x4550) return;
    u32 ns=rd16(g,nt+6), opt=rd16(g,nt+20); u64 sh=nt+24+opt; u64 lo=~0ull,hi=0;
    for(u32 i=0;i<ns&&i<64;i++){ u64 s=sh+40ull*i; if(!GA_OK(s+39)) return; if(!(rd32(g,s+36)&0x20000000u)) continue;
        u64 a=GS_BASE+rd32(g,s+12), e=a+rd32(g,s+8); if(a<lo) lo=a; if(e>hi) hi=e; }
    if(hi>lo){ text_lo=lo; text_hi=hi; text_state=1; }
}
/* v is preceded by a call instruction: E8 rel32; FF 15 / FF 9x disp32; FF 5x disp8; FF 54 24 disp8; FF Dx (with or without REX) */
static int call_before(KCtx *g,u64 v){
    if(!GA_OK(v-6)) return 0;
    u8 b6=rd8(g,v-6),b5=rd8(g,v-5),b4=rd8(g,v-4),b3=rd8(g,v-3),b2=rd8(g,v-2),b1=rd8(g,v-1);
    return b5==0xe8||(b6==0xff&&(b5==0x15||(b5&0xf8)==0x90))||(b3==0xff&&(b2&0xf8)==0x50)||(b4==0xff&&b3==0x54&&b2==0x24)||(b2==0xff&&(b1&0xf8)==0xd0);
}
/* up to `max` return addresses on the guest stack at a draw (the first is the game function that called the D3D12 method) */
static int m_chain(KCtx *g,u64 *ra,int max){
    if(!text_state) text_range(g); if(text_state!=1) return 0;
    int n=0; u64 sp=KH_RSP(g);
    for(u64 a=sp;a<sp+0x2000&&n<max;a+=8){ if(!GA_OK(a+7)) break; u64 v=rd64(g,a); if(v>=text_lo&&v<text_hi&&call_before(g,v)) ra[n++]=v; }
    return n;
}
/* task 46: with the draw log open, every quad H7 examined in the log's presents: one `#h7q` line (its GS box as submitted, H7's kind
 * or -1, the factor and the y range it was given; a quad of an R2 batch or an R3 quad has two lines, the first with kind -1).  Review
 * finding 5: the last two columns are the bytes +8..+27 of the quad's first and last vertex as it was submitted (5 words each: what
 * follows POSITION x, y in the 28-byte 2D vertex), so that a flat rectangle can be told from a picture by its numbers */
static void h7_quadlog(KCtx *g,u64 ra,u32 prim,int q,int nq,const float *bb,int kind,double s,double y0n,double y1n,u64 va,u32 stride){
    if(!dl_f) return; u64 fr=gfx_presents+1; int in=0; for(int r=0;r<dl_n;r++) if(fr>=dl_lo[r]&&fr<=dl_hi[r]) in=1; if(!in) return;
    u32 w[2][5]; for(int c=0;c<2;c++) for(int i=0;i<5;i++) w[c][i]=stride>=28?rd32(g,va+(u64)(c?3:0)*stride+8u+4u*(u32)i):0;
    pthread_mutex_lock(&mm);
    fprintf(dl_f,"#h7q\t%llu\t%llu\t%llx\t%s\t%u\t%d/%d\t%g\t%g\t%g\t%g\t%d\t%.4f\t%g\t%g\t%08x.%08x.%08x.%08x.%08x\t%08x.%08x.%08x.%08x.%08x\n",P(fr),P(kh_game_frames()),P(ra-GS_BASE),dk_in&&dk_cur>=0?dkc[dk_cur].name:"-",prim,q,nq,bb[0],bb[1],bb[2],bb[3],kind,s,
            y0n,y1n,w[0][0],w[0][1],w[0][2],w[0][3],w[0][4],w[1][0],w[1][1],w[1][2],w[1][3],w[1][4]);
    pthread_mutex_unlock(&mm);
}
/* runtime/bgload.c f_4ad270 when kh_aspect_dk (fill, or KHNMC_ASPECT_LOG=2): obj = its result.  H3b first (it sets the object's MS.w
 * before M notes the object current; M records only) */
void kh_aspect_dk_next(KCtx *g,u64 ra,u64 obj){
    if(ra!=DK_RA_NEXT) return;
    if(asp_mode) h3b_post(g,obj);
    if(kh_aspect_m&&dk_in) dk_set(g,obj); }
void orig_f_126500(KCtx *);
void f_126500(KCtx *g){
    if(__builtin_expect(!kh_aspect_m&&!asp_mode,1)){ orig_f_126500(g); return; }
    u64 ra=rd64(g,KH_RSP(g));
    if(ra==DK_RA_END){ if(asp_mode) h3_end(); if(kh_aspect_m&&dk_in){ dk_in=0; dk_cur=-1; dk_obj=0; } orig_f_126500(g); return; }
    if(kh_aspect_m&&ra!=DK_RA_MODE&&dk_in&&dk_cur>=0){ pthread_mutex_lock(&mm); dkc[dk_cur].nested++; int i; for(i=0;i<n_nra;i++) if(nra[i].ra==ra) break;
        if(i==n_nra&&n_nra<32){ nra[i].ra=ra; n_nra++; } if(i<32) nra[i].n++; pthread_mutex_unlock(&mm); }
    int inner=asp_mode&&ra!=DK_RA_MODE&&h3_depth>0&&h3_on[h3_depth-1];               /* task 42 review: inside an object of an anchored pass */
    orig_f_126500(g);
    if(inner) h3_inner_post(g,ra);
    if(ra==DK_RA_MODE){                                                              /* the plan's H3a point: list = guest rdi */
        if(asp_mode) h3a_post(g);                                                    /* task 42: H3a (before M notes the first object) */
        if(!kh_aspect_m) return;
        u64 list=KH_RDI(g); pthread_mutex_lock(&mm); dk_passes++; if(dk_in) dk_passes_nested++; if(!dk_tid) dk_tid=cur_tid(); pthread_mutex_unlock(&mm);
        dk_in=1; dk_pos=0; dk_cur=-1; dk_obj=0; dk_last_cls=dk_prev_other=-1;
        if(GA_OK(list+0x18)) dk_set(g,rd64(g,list+0x18)); }
}
/* runtime/null_d3d12.c, KHNMC_ASPECT_LOG=2: one draw; kind = the bound pipeline's VS; for the MagicScale VS the x / y range of its
 * POSITION in the vertex buffer (GS units with the 2048-based offset; the GS frame subtracts GX+0x90 / +0x94, recorded here) */
typedef struct { int kind, have; float ymin,ymax,xmin,xmax; int have_off, src, mssrc, nocbv; float ox,oy,msy,msw; u64 list, pso; int indexed; u32 count, start; s32 base; } KhMDraw;
void kh_aspect_m_draw(KCtx *g,const KhMDraw *d){
    int kind=d->kind, have=d->have; float ox=d->ox, oy=d->oy, msy=d->msy, msw=d->msw;
    if(kind<0||kind>=VK_N) kind=VK_UNKNOWN;
    u64 ra[6]; int nra_=-1;
    int out=!dk_in||dk_cur<0, inlog=0; u64 fr=gfx_presents+1;
    if(dl_f) for(int r=0;r<dl_n;r++) if(fr>=dl_lo[r]&&fr<=dl_hi[r]) inlog=1;
    if((out&&vk_ms(kind))||inlog) nra_=m_chain(g,ra,6);
    pthread_mutex_lock(&mm);
    u32 t=cur_tid(); if(!draw_tid) draw_tid=t; else if(t!=draw_tid) draws_other_thread++;
    if(vk_ms(kind)){ if(!have) ms_novb++; else if(!d->have_off){ ms_nocb++; have=0; }
        if(have){ ms_src[d->src&3]++; ms_mssrc[d->mssrc&3]++; if(d->nocbv) ms_nocbv++; } }
    if(have&&vk_ms(kind)){ int i; for(i=0;i<n_xyo;i++) if(xyo[i][0]==ox&&xyo[i][1]==oy) break; if(i==n_xyo&&n_xyo<8){ xyo[i][0]=ox; xyo[i][1]=oy; n_xyo++; } if(i<8) xyo_n[i]++; }
    float a=d->ymin-oy, b=d->ymax-oy, pa=a*msy+msw, pb=b*msy+msw; int yv=have&&vk_ms(kind);
    if(h7_in>0&&h7_in<=H7K_N) h7_m_draws[h7_in-1][kind]++;                               /* task 46: which vertex shader draws H7's batches */
    if(!dk_in) dk_out_draws[kind]++;
    else if(dk_cur<0) dk_gap_draws[kind]++;
    else { DkCls *c=&dkc[dk_cur]; c->draws[kind]++;
        if(yv){ c->ydraws++; if(d->src==3) c->ystg++; else c->ycb++; float xa=d->xmin-ox, xb=d->xmax-ox;
            if(dk_obj&&GA_OK(dk_obj+0x17)){ u32 gr=rd32(g,dk_obj+0x14);                                         /* task 42: GS y per (class, group) */
                for(int k=0;k<6;k++) if(c->grpn[k]&&c->grpv[k]==gr){ if(!c->grpd[k]++){ c->grpy0[k]=a; c->grpy1[k]=b; } else { if(a<c->grpy0[k]) c->grpy0[k]=a; if(b>c->grpy1[k]) c->grpy1[k]=b; } break; } }
            c->mss[d->mssrc&3]++; { float ym=(a+b)*0.5f; int bi=ym<-16.0f?0:ym>=-16.0f+8.0f*YH_N?YH_N-1:(int)((ym+16.0f)/8.0f); if(bi<0) bi=0; if(bi>=YH_N) bi=YH_N-1; c->yh[bi]++; }   /* task 42 */
            if(a<c->ymin) c->ymin=a; if(b>c->ymax) c->ymax=b; if(xa<c->xmin) c->xmin=xa; if(xb>c->xmax) c->xmax=xb;
            if(pa<c->pymin) c->pymin=pa; if(pb>c->pymax) c->pymax=pb;
            if(a<0||b>416){ c->yreach++; if(b<0||a>416) c->yout++; iv_add(&c->fryr,(u32)kh_game_frames()); } }
        if(dk_obj){ int j=dk_inst(g); if(j>=0){ DkInst *q=&dki[j]; q->draws++; if(dk_pos-1<q->pmin) q->pmin=dk_pos-1; if(dk_pos-1>q->pmax) q->pmax=dk_pos-1; iv_add(&q->fr,(u32)kh_game_frames());
            if(yv){ q->ydraws++; if(a<q->ymin) q->ymin=a; if(b>q->ymax) q->ymax=b; if(pa<q->pymin) q->pymin=pa; if(pb>q->pymax) q->pymax=pb; } } } }
    if(out&&vk_ms(kind)){
        if(nra_<=0) cg_nochain++;
        else { int w=!dk_in?0:1, k=nra_<4?nra_:4, i;
            for(i=0;i<n_cg;i++){ if(cg[i].where!=w||cg[i].nra!=k) continue; int s=1; for(int m=0;m<k;m++) if(cg[i].ra[m]!=ra[m]) s=0; if(s) break; }
            if(i==n_cg){ if(n_cg<NCG){ ChainG *c=&cg[i]; memset(c,0,sizeof *c); for(int m=0;m<k;m++) c->ra[m]=ra[m]; c->nra=k; c->where=w; c->ymin=c->pymin=1e30f; c->ymax=c->pymax=-1e30f; n_cg++; } else { cg_cut++; i=-1; } }
            if(i>=0){ ChainG *c=&cg[i]; c->n++; c->kinds[kind]++; iv_add(&c->fr,(u32)kh_game_frames());
                if(yv){ c->ydraws++; if(a<c->ymin) c->ymin=a; if(b>c->ymax) c->ymax=b; if(pa<c->pymin) c->pymin=pa; if(pb>c->pymax) c->pymax=pb; } } } }
    if(inlog){
        { char ch[128]; int p=0; ch[0]=0; for(int m=0;m<nra_&&p<(int)sizeof ch-12;m++) p+=snprintf(ch+p,sizeof ch-p,"%s%llx",m?",":"",P(ra[m]-GS_BASE));
            fprintf(dl_f,"%llu\t%llu\t%u\t%llx\t%llx\t%s\t%d\t%u\t%u\t%d\t%s\t%s\t%d\t%s",P(fr),P(kh_game_frames()),t,P(d->list),P(d->pso),vk_name[kind],d->indexed,d->count,d->start,d->base,
                    !dk_in?"out":dk_cur<0?"gap":"obj",dk_in&&dk_cur>=0?dkc[dk_cur].name:"-",dk_in?dk_pos-1:-1,nra_>0?ch:"-");
            /* task 46: the draw's GS-frame box (POSITION - xyOffset), its screen rows, which MagicScale row was staged, H7's kind (0 = none, 1 + H7K_*) */
            if(yv) fprintf(dl_f,"\t%g\t%g\t%g\t%g\t%.1f\t%.1f\t%d\t%d\n",d->xmin-ox,d->xmax-ox,a,b,pa,pb,d->mssrc,h7_in); else fprintf(dl_f,"\t-\t-\t-\t-\t-\t-\t-\t%d\n",h7_in);
            dl_lines++; } }
    pthread_mutex_unlock(&mm);
}
void kh_aspect_drawlog_config(const char *v){
    if(!v||!*v) return;
    if(!kh_aspect_m) kh_fatal(NULL,"aspect-m-drawlog-needs-m: KHNMC_ASPECT_M_DRAWLOG=%s needs KHNMC_ASPECT_LOG=2 (it is a diagnostic of the measurement M's draw recorder, null backend)",v);
    const char *at=strrchr(v,'@');
    if(!at||at==v||!at[1]) kh_fatal(NULL,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=%s (expected FILE@FROM-TO[,FROM-TO...] with present numbers, at most 8 ranges)",v);
    if((size_t)(at-v)>=sizeof dl_path) kh_fatal(NULL,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG: the file name is too long");
    const char *p=at+1; int bad=0; dl_n=0;
    for(;;){ char *e; if(*p<'0'||*p>'9'){ bad=1; break; } u64 lo=strtoull(p,&e,10); if(*e!='-'||e[1]<'0'||e[1]>'9'){ bad=1; break; } u64 hi=strtoull(e+1,&e,10);
        if(hi<lo||dl_n>=8){ bad=1; break; } dl_lo[dl_n]=lo; dl_hi[dl_n]=hi; dl_n++; p=e; if(!*p) break; if(*p!=','){ bad=1; break; } p++; }
    if(bad||!dl_n) kh_fatal(NULL,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=%s (expected FILE@FROM-TO[,FROM-TO...] with present numbers FROM <= TO, at most 8 ranges)",v);
    memcpy(dl_path,v,(size_t)(at-v)); dl_path[at-v]=0;
    dl_f=fopen(dl_path,"w"); if(!dl_f) kh_fatal(NULL,"aspect-m-drawlog-open: KHNMC_ASPECT_M_DRAWLOG: cannot create %s (%s)",dl_path,strerror(errno));
    setvbuf(dl_f,NULL,_IOLBF,0);
    fprintf(dl_f,"frame\tgame_frame\ttid\tlist\tpso\tvs\tindexed\tcount\tstart\tbase\tdk\tclass\tpos\tchain_rva\tgs_x0\tgs_x1\tgs_y0\tgs_y1\trow0\trow1\tms_src\th7\n");
    fprintf(stderr,"kh: aspect: KHNMC_ASPECT_M_DRAWLOG: every draw of %d present range(s) (the first %llu-%llu) into %s (a diagnostic of the measurement M; the frame column numbers presents as the `kh: gfx frame=` lines do)\n",dl_n,P(dl_lo[0]),P(dl_hi[0]),dl_path);
}
void kh_aspect_m_null_report(FILE *f);
static void m_report(FILE *f){
    extern int kh_gfx_vkd3d_on; if(!kh_gfx_vkd3d_on) kh_aspect_m_null_report(f); else fprintf(f,"kh: aspect M: draws and scissor rects are recorded by the null backend only (KHNMC_GFX=vkd3d / vkd3dw here): no draw columns\n");
    pthread_mutex_lock(&mm);
    fprintf(f,"kh: aspect M: dk passes (f_166640 setScreenMode at 0x140166677) %llu (begun inside another pass %llu), objects made current %llu, guest thread of the passes %u; "
              "draws (null backend) on guest thread %u, on another thread %llu; classes %d%s\n",P(dk_passes),P(dk_passes_nested),P(dk_objects),dk_tid,draw_tid,P(draws_other_thread),n_dkc,dkc_cut?" (table full: some classes not recorded)":"");
    fprintf(f,"kh: aspect M: H7: draws made inside a classified batch, by kind and vertex shader:");
    for(int k=0;k<H7K_N;k++){ fprintf(f," [%s:",h7_kname[k]); for(int v=0;v<VK_N;v++) if(h7_m_draws[k][v]) fprintf(f," %s=%llu",vk_name[v],P(h7_m_draws[k][v])); fprintf(f,"]"); } fputc('\n',f);
    fprintf(f,"kh: aspect M: draws by vertex shader outside dk passes:"); for(int k=0;k<VK_N;k++) if(dk_out_draws[k]) fprintf(f," %s=%llu",vk_name[k],P(dk_out_draws[k]));
    fprintf(f,"; inside a pass but outside an object:"); for(int k=0;k<VK_N;k++) if(dk_gap_draws[k]) fprintf(f," %s=%llu",vk_name[k],P(dk_gap_draws[k])); fputc('\n',f);
    fprintf(f,"kh: aspect M: GS offset (cbConstantVS xyOffset.xy, from the source counted in the next line) at the MagicScale draws:"); for(int i=0;i<n_xyo;i++) fprintf(f," (%g, %g) x%llu",xyo[i][0],xyo[i][1],P(xyo_n[i]));
    fprintf(f,"; MagicScale draws whose vertices could not be read %llu, whose constant buffer was not found %llu (no y range for those)\n",P(ms_novb),P(ms_nocb));
    fprintf(f,"kh: aspect M: MagicScale draws by the source of their xyOffset / MagicScale (task 40 review): a bound root CBV holding exactly the rows f_1121d0 staged in GX (+0x59f0, +0x5a00) %llu; "
              "a bound root CBV with MagicScale = GX+0xa0 but other rows (its values used) %llu; the staged rows only (no bound root CBV holds them) %llu; "
              "the staged MagicScale equals GX+0xa0 %llu, GX+0x90 %llu, neither %llu; draws on a list with no root CBV bound at all %llu\n",
            P(ms_src[1]),P(ms_src[2]),P(ms_src[3]),P(ms_mssrc[1]),P(ms_mssrc[2]),P(ms_mssrc[3]),P(ms_nocbv));
    for(int i=0;i<n_dkc;i++){ DkCls *c=&dkc[i]; u64 nd=0; for(int k=0;k<VK_N;k++) nd+=c->draws[k];
        fprintf(f,"kh: aspect M: anchor class %d vtable 0x%llx %s: current %llu times (flags&0x42 skip %llu), first in its pass %llu, position %d..%d, nested setScreenMode %llu; draws %llu:",
                i,P(c->vt),c->name[0]?c->name:"(no name)",P(c->seen),P(c->skipped),P(c->first),c->pmin>c->pmax?-1:c->pmin,c->pmax,P(c->nested),P(nd));
        for(int k=0;k<VK_N;k++) if(c->draws[k]) fprintf(f," %s=%llu",vk_name[k],P(c->draws[k]));
        if(c->ydraws){ fprintf(f,"; its %llu MagicScale draws with vertices (rows from a bound CB %llu, from the staged rows only %llu): GS frame (POSITION - xyOffset) y %g..%g, x %g..%g; screen rows (y MagicScale.y + MagicScale.w) %.1f..%.1f",P(c->ydraws),P(c->ycb),P(c->ystg),c->ymin,c->ymax,c->xmin,c->xmax,c->pymin,c->pymax);
            fprintf(f,"; of them reaching beyond the 16:9 frame (GS y < 0 or > 416) %llu, wholly beyond it %llu",P(c->yreach),P(c->yout)); if(c->yreach){ fprintf(f," (in"); iv_print(f,&c->fryr); fputc(')',f); } }
        if(c->draws[VK_5871b0]||c->draws[VK_593b10]||c->draws[VK_3D]||c->draws[VK_59ac40]) fprintf(f,"; FLAG: draws that ignore MagicScale.zw");
        { int any=0; for(int k=0;k<6;k++) if(c->grpn[k]){ fprintf(f,"%s%u x%llu",any?", ":"; dk groups (+0x14) of its drawn objects: ",c->grpv[k],P(c->grpn[k]));      /* task 42 */
              if(c->grpd[k]) fprintf(f," (%llu MagicScale draws, GS y %g..%g)",P(c->grpd[k]),c->grpy0[k],c->grpy1[k]); any=1; }
          if(c->grpcut) fprintf(f,", other groups x%llu",P(c->grpcut));
          if(c->ydraws){ fprintf(f,"; staged MagicScale of its draws: GX+0xa0 %llu, GX+0x90 %llu, neither %llu; GS y mid-points (8-row bins from -16):",P(c->mss[1]),P(c->mss[2]),P(c->mss[3]));
              for(int k=0;k<YH_N;k++) if(c->yh[k]) fprintf(f," [%d..%d)=%llu",-16+8*k,-8+8*k,P(c->yh[k])); } }
        fputc(';',f); iv_print(f,&c->fr);
        if(c->have_snap){ fprintf(f,"; first drawn object +0x00..0x3f:"); for(int k=0;k<0x40;k+=4){ u32 v; memcpy(&v,c->snap+k,4); fprintf(f,"%s%08x",k%16?" ":" | ",v); } }
        fputc('\n',f); }
    /* per drawn instance, grouped by (class, the nearest preceding class of another kind, the owner-candidate pointer fields) */
    { int ng=0; static int gi[NINST], gn[NINST]; static u64 gd[NINST]; static int ord[NINST];
      for(int i=0;i<n_dki;i++){ DkInst *d=&dki[i]; int k;
          for(k=0;k<ng;k++){ DkInst *e=&dki[gi[k]]; if(e->cls!=d->cls||e->prevcls!=d->prevcls||e->np!=d->np) continue; int s=1; for(int m=0;m<d->np;m++) if(e->poff[m]!=d->poff[m]||e->pcls[m]!=d->pcls[m]) s=0; if(s) break; }
          if(k==ng){ gi[ng]=i; gn[ng]=0; gd[ng]=0; ng++; } gn[k]++; gd[k]+=d->draws; }
      for(int k=0;k<ng;k++) ord[k]=k;
      for(int a=1;a<ng;a++){ int x=ord[a],b=a; while(b>0&&gd[ord[b-1]]<gd[x]){ ord[b]=ord[b-1]; b--; } ord[b]=x; }
      fprintf(f,"kh: aspect M: drawn dk instances %d%s, in %d groups by (class, nearest preceding object of another class, pointer fields +0x08..+0x78 to objects with an RTTI name); the groups by draws:\n",n_dki,dki_cut?" (table full: some not recorded)":"",ng);
      for(int r=0;r<ng&&r<120;r++){ int k=ord[r]; DkInst *e=&dki[gi[k]]; u64 yd=0; int pmin=1<<30,pmax=-1; float ymin=1e30f,ymax=-1e30f,pymin=1e30f,pymax=-1e30f; Ivs fr; memset(&fr,0,sizeof fr);
          for(int i=0;i<n_dki;i++){ DkInst *d=&dki[i]; if(d->cls!=e->cls||d->prevcls!=e->prevcls||d->np!=e->np) continue; int s=1; for(int m=0;m<d->np;m++) if(e->poff[m]!=d->poff[m]||e->pcls[m]!=d->pcls[m]) s=0; if(!s) continue;
              yd+=d->ydraws; if(d->pmin<pmin) pmin=d->pmin; if(d->pmax>pmax) pmax=d->pmax; if(d->ydraws){ if(d->ymin<ymin) ymin=d->ymin; if(d->ymax>ymax) ymax=d->ymax; if(d->pymin<pymin) pymin=d->pymin; if(d->pymax>pymax) pymax=d->pymax; }
              iv_merge(&fr,&d->fr); }
          fprintf(f,"kh: aspect M: instances: %s after %s",dkc[e->cls].name,e->prevcls>=0?dkc[e->prevcls].name:"(first in the pass)");
          if(e->np){ fprintf(f,", pointers"); for(int m=0;m<e->np;m++) fprintf(f," +0x%x->%s",e->poff[m],dkc[e->pcls[m]].name); } else fprintf(f,", no pointer to an RTTI object");
          fprintf(f,": %d instances, %llu draws, position %d..%d",gn[k],P(gd[k]),pmin,pmax);
          if(yd) fprintf(f,"; %llu MagicScale draws: GS y %g..%g, screen rows %.1f..%.1f",P(yd),ymin,ymax,pymin,pymax);
          fputc(';',f); iv_print(f,&fr); fprintf(f,"; first instance 0x%llx +0x00..0x3f:",P(e->obj)); for(int m=0;m<0x40;m+=4){ u32 v; memcpy(&v,e->snap+m,4); fprintf(f,"%s%08x",m%16?" ":" | ",v); } fputc('\n',f); } }
    /* the MagicScale draws outside dk objects, by return addresses (rva) */
    { int ord[NCG]; for(int i=0;i<n_cg;i++) ord[i]=i;
      for(int a=1;a<n_cg;a++){ int x=ord[a],b=a; while(b>0&&cg[ord[b-1]].n<cg[x].n){ ord[b]=ord[b-1]; b--; } ord[b]=x; }
      fprintf(f,"kh: aspect M: MagicScale draws outside dk objects by the return addresses on the guest stack (the first = the game function that called the D3D12 draw): %d groups%s, draws without a return address found %llu%s\n",
              n_cg,cg_cut?" (table full: some not recorded)":"",P(cg_nochain),text_state==2?" (NO executable section found in the image's PE header: no chains)":"");
      for(int r=0;r<n_cg&&r<80;r++){ ChainG *c=&cg[ord[r]]; fprintf(f,"kh: aspect M: outside: %s, return addresses (rva)",c->where?"inside a pass, between objects":"outside dk passes");
          for(int m=0;m<c->nra;m++) fprintf(f,"%s0x%llx",m?" < ":" ",P(c->ra[m]-GS_BASE));
          fprintf(f,": %llu draws:",P(c->n)); for(int k=0;k<VK_N;k++) if(c->kinds[k]) fprintf(f," %s=%llu",vk_name[k],P(c->kinds[k]));
          if(c->ydraws) fprintf(f,"; GS y %g..%g, screen rows %.1f..%.1f",c->ymin,c->ymax,c->pymin,c->pymax);
          fputc(';',f); iv_print(f,&c->fr); fputc('\n',f); } }
    if(dl_f) fprintf(f,"kh: aspect M: KHNMC_ASPECT_M_DRAWLOG: %llu draws written to %s\n",P(dl_lines),dl_path);
    fprintf(f,"kh: aspect M: occlusion volumes (f_185930 after the map pass, into rctx+0x508 = [0x140abd440]+0x508, %u slots, count rctx+0xa08; "
              "built with the map pass's clip rctx+0x488, which H2b leaves wide): sampled at %llu map passes (rctx not readable %llu, rctx changed since the previous map pass %llu: not sampled); count:",
            OCC_SLOTS,P(occ.n),P(occ.norctx),P(occ.newrc));
    for(u32 i=0;i<=OCC_SLOTS;i++) if(occ.hist[i]) fprintf(f," %u x%llu",i,P(occ.hist[i]));
    if(occ.hist[OCC_SLOTS+1]) fprintf(f," more than %u x%llu",OCC_SLOTS,P(occ.hist[OCC_SLOTS+1]));
    fprintf(f,"; largest %u (present %llu); more than %u (beyond the array: f_185930 has no bound check) %llu",occ.max,P(occ.maxp),OCC_SLOTS,P(occ.over));
    if(occ.over){ fprintf(f," in"); iv_print(f,&occ.frover); fprintf(f," (present=value:"); for(int i=0;i<occ.nov;i++) fprintf(f," %llu=0x%x",P(occ.ovp[i]),occ.ovn[i]); fputc(')',f); }
    if(occ.nat){ fprintf(f,"; at the draw-log presents (present=count):"); for(int i=0;i<occ.nat;i++) fprintf(f," %llu=%u",P(occ.atp[i]),occ.atn[i]); }
    fputc('\n',f);
    if(occ.over) fprintf(f,"kh: aspect: WARNING: occlusion volumes: more than %u at %llu map passes (largest %u): f_185930 wrote beyond rctx+0x508's %u slots\n",OCC_SLOTS,P(occ.over),occ.max,OCC_SLOTS);
    for(int i=0;i<n_nra;i++) fprintf(f,"kh: aspect M: nested setScreenMode (f_126500 inside a dk object) from return address 0x%llx: %llu\n",P(nra[i].ra),P(nra[i].n));
    for(int i=0;i<n_sc126;i++){ Sc *s=&sc126[i]; fprintf(f,"kh: aspect M: scissor f_126fd0 from 0x%llx: x %d y %d w %d h %d%s: %llu;",P(s->ra),s->x,s->y,s->w,s->h,s->y==0&&s->h==416?" (FULL HEIGHT)":"",P(s->n)); iv_print(f,&s->fr); fputc('\n',f); }
    if(sc126_cut) fprintf(f,"kh: aspect M: scissor f_126fd0: %llu calls of rects beyond the 64-entry table\n",P(sc126_cut));
    fprintf(f,"kh: aspect M: D3D12 RSSetScissorRects (null backend) calls %llu, distinct first rects %d%s\n",P(rs_calls),n_rs,rs_cut?" (table full)":"");
    for(int i=0;i<n_rs;i++){ Rs *s=&rs[i]; fprintf(f,"kh: aspect M: D3D12 scissor (%d,%d)-(%d,%d): %llu;",s->l,s->t,s->r,s->b,P(s->n)); iv_print(f,&s->fr); fputc('\n',f); }
    for(int i=0;i<n_c84;i++){ C84 *c=&c84[i];
        const char *who=c->ra==1?"H2b's camera (f_19cfc0(1) in 0x186920)":c->ra==2?"H2c's camera (0x2bd190 rcx)":"f_19d040 (cam+0x84) from";
        fprintf(f,"kh: aspect M: camera: %s",who); if(c->ra>2) fprintf(f," 0x%llx",P(c->ra));
        if(c->cls==0) fprintf(f,": 0x140ac1840[%d] (current, rebuilt by f_19cad0)",c->idx); else if(c->cls==1) fprintf(f,": 0x140ac1220[%d] (previous-frame copy by f_186810)",c->idx); else fprintf(f,": other 0x%llx",P(c->ptr));
        fprintf(f,": %llu (while H2b's edit is in force %llu, H2c's %llu);",P(c->n),P(c->inb),P(c->inc)); iv_print(f,&c->fr); fputc('\n',f); }
    if(c84_cut) fprintf(f,"kh: aspect M: camera table full: %llu calls not recorded\n",P(c84_cut));
    fprintf(f,"kh: aspect M: byte 0x140ac183c at the effect-pass calls:"); for(int v=0;v<256;v++) if(sel_hist[v]) fprintf(f," %d x%llu",v,P(sel_hist[v])); fputc('\n',f);
    for(int i=0;i<7;i++){ H4Stat *s=&h4s[i]; fprintf(f,"kh: aspect M: %s: f_156f10 calls %llu",h4o[i].what,P(s->draws));
        if(s->no){ fprintf(f,"; rects (y, h) recorded (pc: the rects drawn; fill: those neither the exe's nor the extended one):"); for(int k=0;k<s->no;k++) fprintf(f," (%d, %d)",s->oy[k],s->oh[k]); } fputc('\n',f); }
    pthread_mutex_unlock(&mm);
}

/* exit report (runtime/seams.c), only when a switch is set */
void kh_aspect_report(FILE *f){
    if(hide_title_credit) fprintf(f,"kh: title credit: %llu batches suppressed\n",P(hidden_credit_batches));
    if(!asp_mode&&!kh_aspect_log) return;
    fprintf(f,"kh: aspect report: KHNMC_ASPECT=%s%s; H1 (0x11c1e0): calls %llu, not narrow %llu, narrow %llu (rewritten to the full height %llu, kept: no 3D view in this frame or the previous one %llu), "
              "result different from the model %llu; narrow calls by game frames since the map pass: same frame %llu, previous frame %llu, 2 or more frames earlier %llu, never %llu; map pass (0x186920) calls %llu; first guest threads: H1 %u, map pass %u\n",
            asp_mode?"fill":"pc",asp_h1_uncond?" + KHNMC_ASPECT_TEST_H1=unconditional":"",P(h1_calls),P(h1_wide),P(h1_narrow),P(h1_rewritten),P(h1_kept),P(h1_mismatch),
            P(h1_since[0]),P(h1_since[1]),P(h1_since[2]),P(h1_since[3]),P(map_calls),h1_tid,map_tid);
    if(h1_mismatch) fprintf(f,"kh: aspect: WARNING: %llu H1 calls were left as the exe made them because its result differed from the model\n",P(h1_mismatch));
    if(asp_mode){                                                                   /* task 40: H2, H4, H5 */
        const H2Stat *s[2]={&h2b,&h2c};
        for(int i=0;i<2;i++) fprintf(f,"kh: aspect report: %s: calls %llu: r = 1, nothing written %llu; m11 divided by %s and restored %llu; test aid off %llu; changed inside the call (game's value kept) %llu; camera not guest memory %llu\n",
                                     s[i]->name,P(s[i]->calls),P(s[i]->identity),asp_nocull?"2047/208 (TEST AID no-cull)":"r",P(s[i]->edited),P(s[i]->off),P(s[i]->inside),P(s[i]->badcam));
        for(int i=0;i<2;i++) if(s[i]->inside||s[i]->badcam) fprintf(f,"kh: aspect: WARNING: %s: %llu calls changed cam+0x98 inside, %llu cameras not guest memory\n",s[i]->name,P(s[i]->inside),P(s[i]->badcam));
        u64 tr=0,ta=0,to=0,sr=0,so=0;
        fprintf(f,"kh: aspect report: H4 (fades and event bars): f_156e20 calls %llu; r = 1, nothing written %llu; test aid off %llu;",P(h4_e20_calls),P(h4_identity),P(h4_off));
        for(int i=0;i<7;i++){ H4Stat *h=&h4s[i]; tr+=h->rewritten; ta+=h->already; to+=h->other; sr+=h->setter_rewritten; so+=h->setter_other;
            fprintf(f," [%s: drawn %llu, rewritten at the draw %llu, already extended %llu, other rect %llu, rewritten after f_156e20 %llu, other rect there %llu]",
                    h4o[i].what,P(h->draws),P(h->rewritten),P(h->already),P(h->other),P(h->setter_rewritten),P(h->setter_other)); }
        fprintf(f,"; totals: rewritten %llu + %llu, already %llu, other rect %llu + %llu\n",P(tr),P(sr),P(ta),P(to),P(so));
        if(to||so) fprintf(f,"kh: aspect: NOTE: H4 left %llu fade / bar draws and %llu bar settings with a rect that is neither the exe's nor the extended one (see the M lines for the rects)\n",P(to),P(so));
        fprintf(f,"kh: aspect report: H5 (full-height scissors, 0x126fd0): calls %llu: extended %llu, other rects (left) %llu, r = 1 %llu, test aid off %llu\n",P(h5_calls),P(h5_ext),P(h5_other),P(h5_identity),P(h5_off));
        h3_report(f);                                                               /* task 42 */
        h7_report(f);                                                               /* task 46 */
        zoom_report(f);                                                             /* task 49 */
        hud_report(f);                                                              /* task 53 */
        ttl_report(f);                                                              /* task 53 item 2 */
    }
    if(kh_aspect_m) m_report(f);
    if(!kh_aspect_log) return;
    pthread_mutex_lock(&am);
    fprintf(f,"kh: aspect report: swap chain: %llu records, last size %ux%u%s\n",P(sc_notes),sc_w,sc_h,sc_lines_cut?" (some record lines not printed, see the count)":"");
    if(sc_lines_cut) fprintf(f,"kh: aspect report: swap chain lines not printed: %llu\n",P(sc_lines_cut));
    extern int kh_gfx_vkd3d_on;
    if(kh_gfx_vkd3d_on) fprintf(f,"kh: aspect report: rt census: not recorded here with KHNMC_GFX=vkd3d / vkd3dw (the resources are vkd3d's): see the `kh: gfx vkd3d census: render-target` line\n");
    else { fprintf(f,"kh: aspect report: rt census: %llu render-target / depth-stencil / multisampled textures created, %d kinds%s:",P(rt_total),n_rtk,rtk_more?" (more kinds than the table holds: see the count)":"");
    for(int i=0;i<n_rtk;i++){ RtKind *k=&rtk[i]; fprintf(f," [%uD fmt%u %ux%u a%u m%u s%u fl0x%x = %llu]",k->dim-1,k->fmt,k->w,k->h,k->arr,k->mips,k->samples,k->flags,P(k->n)); }
    fputc('\n',f); if(rtk_more) fprintf(f,"kh: aspect report: rt census: %llu textures of kinds beyond the 64-entry table\n",P(rtk_more)); }
    if(gx_have) gx_print(f,"at the last change",&gx_last);
    fprintf(f,"kh: aspect report: GX size lines %llu printed, %llu not printed; presents where the built size was below the requested one (VRAM guard): %llu\n",P(gx_lines),P(gx_lines_cut),P(gx_guard_presents));
    pthread_mutex_unlock(&am);
}

/* ==== selftest ===================================================================================================================== */
static int sfails, schecks;
#define ACHECK(c) do{ schecks++; if(!(c)){ sfails++; fprintf(stderr,"kh: aspect selftest FAIL line %d: %s\n",__LINE__,#c); } }while(0)
/* a guest-ABI call of an import or hook: return address pushed, rcx..r9, stack arguments 4.. (as runtime/selftest.c do_call) */
static u64 st_call(KCtx *g,void (*fn)(KCtx*),const u64 *a,int n,int *rsp_ok){
    u64 sv[5]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g)}, sp=(sv[0]-0x400)&~15ull; memset(hp(g,sp),0,0x80);
    for(int i=4;i<n;i++) wr64(g,sp+0x20+8ull*(u64)(i-4),a[i]);
    sp-=8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0;
    fn(g); u64 r=KH_RAX(g); if(rsp_ok) *rsp_ok=KH_RSP(g)==sp+8;
    KH_RSP(g)=sv[0]; KH_RCX(g)=sv[1]; KH_RDX(g)=sv[2]; KH_R8(g)=sv[3]; KH_R9(g)=sv[4]; return r;
}
/* task 42: the same with a chosen return address (the hooks H3a / H3b act only at the dk loop's call sites) and guest rdi (f_166640's
 * list; f_126500 saves and restores it) */
static u64 st_call_ra(KCtx *g,void (*fn)(KCtx*),u64 ra,u64 rdi,const u64 *a,int n,int *rsp_ok){
    u64 sv[6]={KH_RSP(g),KH_RCX(g),KH_RDX(g),KH_R8(g),KH_R9(g),KH_RDI(g)}, sp=(sv[0]-0x400)&~15ull; memset(hp(g,sp),0,0x80);
    for(int i=4;i<n;i++) wr64(g,sp+0x20+8ull*(u64)(i-4),a[i]);
    sp-=8; wr64(g,sp,ra); KH_RSP(g)=sp; KH_RCX(g)=n>0?a[0]:0; KH_RDX(g)=n>1?a[1]:0; KH_R8(g)=n>2?a[2]:0; KH_R9(g)=n>3?a[3]:0; KH_RDI(g)=rdi;
    fn(g); u64 r=KH_RAX(g); if(rsp_ok) *rsp_ok=KH_RSP(g)==sp+8&&KH_RDI(g)==rdi;
    KH_RSP(g)=sv[0]; KH_RCX(g)=sv[1]; KH_RDX(g)=sv[2]; KH_R8(g)=sv[3]; KH_R9(g)=sv[4]; KH_RDI(g)=sv[5]; return r;
}
static u64 st_vcall(KCtx *g,u64 obj,int slot,u64 a1){      /* a COM method through the object's guest vtable */
    u64 sv[3]={KH_RSP(g),KH_RCX(g),KH_RDX(g)}, sp=((sv[0]-0x400)&~15ull)-8; wr64(g,sp,0x7ff7ffff0000ull); KH_RSP(g)=sp; KH_RCX(g)=obj; KH_RDX(g)=a1;
    kh_dispatch(g,rd64(g,rd64(g,obj)+8ull*(u64)slot)); u64 r=KH_RAX(g); KH_RSP(g)=sv[0]; KH_RCX(g)=sv[1]; KH_RDX(g)=sv[2]; return r;
}
static int near(double a,double b,double tol){ double d=a-b; return d<=tol&&d>=-tol; }

/* the named fatals of the two configuration functions: each case in a child that must end with exit code 3 and the name on stderr */
void kh_screen_config(const char *screen,const char *khwin);
static int fatal_case(int which,const char *expect){
    FILE *f=kh_tmpfile(); if(!f) return 0;
    fflush(stderr); pid_t child=kh_fork();
    if(child==0){ dup2(fileno(f),2);
        switch(which){
        case 0: kh_screen_config("1920",NULL); break;
        case 1: kh_screen_config("1920x1350x2",NULL); break;
        case 2: kh_screen_config("8x8",NULL); break;
        case 3: kh_screen_config("1920x1350","800x600"); break;
        case 4: kh_aspect_config("wide",NULL,NULL); break;
        case 5: kh_aspect_config(NULL,"yes",NULL); break;
        case 6: kh_aspect_config("fill",NULL,"always"); break;
        case 7: kh_aspect_config("pc",NULL,"unconditional"); break;
        case 8: asp_mode=1; kh_aspect_test_config("h2,h6",NULL); break;              /* task 40 test aids */
        case 9: asp_mode=0; kh_aspect_test_config("h2",NULL); break;
        case 10: asp_mode=1; kh_aspect_test_config(NULL,"yes"); break;
        case 11: asp_mode=0; kh_aspect_test_config(NULL,"1"); break;
        case 12: asp_mode=1; kh_aspect_test_config("h4,",NULL); break;
        case 13: kh_aspect_config(NULL,"3",NULL); break;
        case 14: kh_aspect_m=0; kh_aspect_drawlog_config("x.tsv@1-2"); break;              /* task 40 review: the draw-log diagnostic */
        case 15: kh_aspect_m=1; kh_aspect_drawlog_config("x.tsv@5-3"); break;
        case 16: kh_aspect_m=1; kh_aspect_drawlog_config("x.tsv@1-2,"); break;
        case 17: kh_aspect_m=1; kh_aspect_drawlog_config("/nonexistent-khnmc-dir/x.tsv@1-2"); break;
        case 18: kh_aspect_zoom_config("2.5"); break;                                      /* task 49: KHNMC_FOV_ZOOM */
        case 19: kh_aspect_zoom_config("0.99"); break;
        case 20: kh_aspect_zoom_config("1,15"); break;
        case 21: kh_aspect_zoom_config("1.15x"); break;
        case 37: kh_aspect_zoom_config("2.0"); break;                                      /* review finding 3: 1.89 at most (task 49 fix; 25 is task 53's) */
        /* not fatals: the value is taken and announced (22); pc: the named NOTE and no zoom (23); unset or empty: the default 1.3 (24).
         * Exit code 3 = the state is as expected (the caller also looks for the line) */
        case 22: asp_mode=1; asp_zoom_set=0; asp_zoom_tall=ZOOM_DEFAULT; kh_aspect_zoom_config("1.15");
                 if(asp_zoom_tall==1.15&&asp_zoom_set&&kh_aspect_zoom(2007,2853)==1.15&&kh_aspect_zoom(2853,2007)==1.0) _exit(3); break;
        case 23: asp_mode=0; asp_zoom_set=0; asp_zoom_tall=ZOOM_DEFAULT; kh_aspect_zoom_config("1.15"); if(kh_aspect_zoom(2007,2853)==1.0) _exit(3); break;
        case 24: asp_mode=1; asp_zoom_set=0; asp_zoom_tall=ZOOM_DEFAULT; kh_aspect_zoom_config(NULL); kh_aspect_zoom_config("");
                 if(asp_zoom_tall==1.3&&!asp_zoom_set&&kh_aspect_zoom(2007,2853)==1.3) _exit(3); break;
        case 25: kh_aspect_hud_config("3.5",NULL); break;                                  /* task 53: KHNMC_HUD_SCALE_CMD / _HP */
        case 26: kh_aspect_hud_config(NULL,"0.4"); break;
        case 27: kh_aspect_hud_config("1,25",NULL); break;
        case 28: kh_aspect_hud_config(NULL,"1.5x"); break;
        /* not fatals (exit code 3 = the state is as expected): values taken and announced (29); pc: the named NOTE and no scale (30);
         * unset or empty: the defaults 1.25 / 1.5 on a screen taller than wide, 1 on every other (31) */
        case 29: asp_mode=1; hud_s[0]=HUD_CMD_DEFAULT; hud_s[1]=HUD_HP_DEFAULT; hud_set[0]=hud_set[1]=0; kh_aspect_hud_config("2","1.0");
                 if(hud_s[0]==2.0&&hud_s[1]==1.0&&hud_set[0]&&hud_set[1]&&kh_aspect_hud_scale(HUD_CMD,2007,2853)==2.0&&kh_aspect_hud_scale(HUD_HP,2007,2853)==1.0&&kh_aspect_hud_scale(HUD_CMD,2853,2007)==1.0) _exit(3); break;
        case 30: asp_mode=0; hud_s[0]=HUD_CMD_DEFAULT; hud_s[1]=HUD_HP_DEFAULT; hud_set[0]=hud_set[1]=0; kh_aspect_hud_config("2",NULL);
                 if(kh_aspect_hud_scale(HUD_CMD,2007,2853)==1.0&&kh_aspect_hud_scale(HUD_HP,2007,2853)==1.0) _exit(3); break;
        case 31: asp_mode=1; hud_s[0]=HUD_CMD_DEFAULT; hud_s[1]=HUD_HP_DEFAULT; hud_set[0]=hud_set[1]=0; kh_aspect_hud_config(NULL,"");
                 if(kh_aspect_hud_scale(HUD_CMD,2007,2853)==1.25&&kh_aspect_hud_scale(HUD_HP,2007,2853)==1.5&&kh_aspect_hud_scale(HUD_HP,2007,2007)==1.0&&kh_aspect_hud_scale(HUD_HP,1920,1350)==1.0) _exit(3); break;
        case 32: asp_mode=1; kh_aspect_title_config("wide"); break;                        /* task 53 item 2: KHNMC_TITLE_LAYOUT */
        case 33: asp_mode=0; asp_title_band=0; kh_aspect_title_config("band"); { TtLay L; if(asp_title_band&&!kh_aspect_title_layout(2007,2853,&L,NULL)) _exit(3); } break;
        case 34: asp_mode=1; asp_title_band=0; kh_aspect_title_config("band"); { TtLay L; if(asp_title_band&&!kh_aspect_title_layout(2007,2853,&L,NULL)) _exit(3); } break;
        case 36: asp_mode=1; asp_title_band=0; asp_title_set=0; kh_aspect_title_config(NULL); kh_aspect_title_config("");   /* unset or empty: portrait */
                 { TtLay L; if(!asp_title_band&&kh_aspect_title_layout(2007,2853,&L,NULL)) _exit(3); } break;
        }
        _exit(99); }
    int status=0; if(child<0){ fclose(f); return 0; } waitpid(child,&status,0);
    rewind(f); char out[8192]; size_t n=fread(out,1,sizeof out-1,f); out[n]=0; fclose(f);
    int ok=WIFEXITED(status)&&WEXITSTATUS(status)==3&&strstr(out,expect)!=NULL;
    if(!ok) fprintf(stderr,"kh: aspect selftest: fatal case %d: exit status 0x%x, expected exit code 3 with '%s'; child said: %.300s\n",which,status,expect,out);
    return ok;
}

int kh_screen_parse(const char *s,int *w,int *h); int kh_screen_size(int *w,int *h);
void kh_screen_test_state(const int *set,int *old); void kh_screen_test_counts(u64 *create,u64 *setpos); u64 kh_null_output_obj(void);
void kh_screen_report(FILE *f);

/* the built-size check (screen-not-applied): GX's built size (+0x8, ints) and screen size (+0x18, floats) set for a case */
static void gx_sizes(KCtx *g,s32 bw,s32 bh,float W,float H){ wr32(g,GX+0x8,(u32)bw); wr32(g,GX+0xc,(u32)bh); wrf(g,GX+0x18,W); wrf(g,GX+0x1c,H); }
/* its mismatch cases in a child, so that their WARNING and report lines go to the child's stderr and not into the selftest's: the
 * seam at 1920x1350; present 11 with a 16:9 swap chain, 12 with GX+0x18 at 16:9, 13 with the built size GX+0x8 halved (the VRAM
 * guard), 14 with everything right.  The child's exit code is 0 when the four results and the two counts are as expected. */
static int check_child(KCtx *g,char *out,size_t n){
    FILE *f=kh_tmpfile(); if(!f) return -1;
    fflush(stderr); pid_t child=kh_fork();
    if(child==0){ dup2(fileno(f),2);
        const int on[8]={1,1920,1350,1920,1350,0,0,1}; kh_screen_test_state(on,NULL);
        atomic_store(&scr_chk_presents,0); atomic_store(&scr_chk_bad,0); atomic_store(&scr_chk_logged,0);
        gx_sizes(g,1920,1350,1920,1350); int r1=kh_screen_check_present(g,1920,1080,11);
        gx_sizes(g,1920,1350,1920,1080); int r2=kh_screen_check_present(g,1920,1350,12);
        gx_sizes(g,960,675,1920,1350);   int r3=kh_screen_check_present(g,1920,1350,13);
        gx_sizes(g,1920,1350,1920,1350); int r4=kh_screen_check_present(g,1920,1350,14);
        kh_screen_report(stderr); fflush(stderr);
        _exit(r1==1&&r2==1&&r3==1&&r4==0&&atomic_load(&scr_chk_presents)==4&&atomic_load(&scr_chk_bad)==3?0:5); }
    int status=0; if(child<0){ fclose(f); return -1; } waitpid(child,&status,0);
    rewind(f); size_t k=fread(out,1,n-1,f); out[k]=0; fclose(f);
    return WIFEXITED(status)?WEXITSTATUS(status):128;
}
void rt_imp_user32__IsZoomed(KCtx*); void rt_imp_user32__GetClientRect(KCtx*); void rt_imp_user32__GetSystemMetrics(KCtx*); void rt_imp_user32__GetMonitorInfoA(KCtx*);
void rt_imp_user32__EnumDisplaySettingsA(KCtx*); void rt_imp_user32__SetWindowPos(KCtx*); void rt_imp_user32__CreateWindowExA(KCtx*); void rt_imp_user32__GetCursorPos(KCtx*);

/* S1 + S2 through the imports the game calls: a screen set to sw x sh (set=1), or the seam off (set=0, the state before task 37) */
static void seam_cases(KCtx *g,int set,int sw,int sh){
    u64 m=ga_calloc(1,0x200); int ok;
    u64 z=st_call(g,rt_imp_user32__IsZoomed,(u64[]){0xAB0001},1,&ok); ACHECK(ok&&z==(u64)(set?1:0));
    st_call(g,rt_imp_user32__GetClientRect,(u64[]){0xAB0001,m},2,&ok);
    ACHECK(ok&&rd32(g,m)==0&&rd32(g,m+4)==0&&rd32(g,m+8)==(u32)(set?sw:1280)&&rd32(g,m+12)==(u32)(set?sh:720));
    ACHECK(st_call(g,rt_imp_user32__GetSystemMetrics,(u64[]){0},1,NULL)==(u64)sw&&st_call(g,rt_imp_user32__GetSystemMetrics,(u64[]){1},1,NULL)==(u64)sh);
    st_call(g,rt_imp_user32__GetCursorPos,(u64[]){m},1,NULL); ACHECK(rd32(g,m)==(u32)(sw/2)&&rd32(g,m+4)==(u32)(sh/2));
    memset(hp(g,m),0,0x200); wr32(g,m,0x68);
    ACHECK(st_call(g,rt_imp_user32__GetMonitorInfoA,(u64[]){0xAB0100,m},2,NULL)==1&&rd32(g,m+12)==(u32)sw&&rd32(g,m+16)==(u32)sh&&rd32(g,m+28)==(u32)sw&&rd32(g,m+32)==(u32)sh);
    static const u32 modes[4]={0,1,2,0xFFFFFFFFu};
    for(int i=0;i<4;i++){ memset(hp(g,m),0,0x200);
        u64 r=st_call(g,rt_imp_user32__EnumDisplaySettingsA,(u64[]){0,modes[i],m},3,NULL);
        int expect=set?(modes[i]==0||modes[i]==0xFFFFFFFFu):(modes[i]<=1||modes[i]==0xFFFFFFFFu);   /* one mode with the seam; modes 0 and 1 without */
        ACHECK(r==(u64)expect&&(!expect||(rd32(g,m+0x6c)==(u32)sw&&rd32(g,m+0x70)==(u32)sh&&rd32(g,m+0x78)==60&&(rd32(g,m+0x48)&0x400000)))); }
    u64 c0,p0,c1,p1; kh_screen_test_counts(&c0,&p0);
    st_call(g,rt_imp_user32__SetWindowPos,(u64[]){0xAB0001,0,10,20,1280,720,0},7,NULL);         /* a size request */
    st_call(g,rt_imp_user32__GetClientRect,(u64[]){0xAB0001,m},2,NULL);
    ACHECK(rd32(g,m+8)==(u32)(set?sw:1280)&&rd32(g,m+12)==(u32)(set?sh:720));
    st_call(g,rt_imp_user32__CreateWindowExA,(u64[]){0,0,0,0,0,0,1600,900,0,0,0,0},12,NULL);
    st_call(g,rt_imp_user32__GetClientRect,(u64[]){0xAB0001,m},2,NULL);
    ACHECK(rd32(g,m+8)==(u32)(set?sw:1600)&&rd32(g,m+12)==(u32)(set?sh:900));
    kh_screen_test_counts(&c1,&p1); ACHECK(c1==c0+(set?1:0)&&p1==p0+(set?1:0));                 /* counted only by the seam */
    u64 out=kh_null_output_obj();                                                                    /* S2: IDXGIOutput::GetDesc DesktopCoordinates */
    if(out){ memset(hp(g,m),0xa5,0x60); ACHECK(st_vcall(g,out,7,m)==0&&rd32(g,m+0x40)==0&&rd32(g,m+0x44)==0&&rd32(g,m+0x48)==(u32)sw&&rd32(g,m+0x4c)==(u32)sh&&rd32(g,m+0x50)==1); }
    else ACHECK(out!=0);
    ga_free(m);
}

/* the lifted f_11c1e0 through the hook, with GX's size fields set to a test screen.  f_11c150 (the GX accessor it calls first) takes
 * its fast path when its thread-safe-static guard reads as initialised: the guard is set for the call and put back, as are GX's
 * 32 bytes at +0x10 (nothing is constructed). */
static void h1_call(KCtx *g,float W,float H,float sx,float sy,float s,float *x,float *y){
    u64 out=ga_calloc(1,16); int ok=0;
    wrf(g,GX+0x18,W); wrf(g,GX+0x1c,H); wrf(g,GX+0x20,sx); wrf(g,GX+0x24,sy); wrf(g,GX+0x28,s); wrf(g,GX+0x10,W); wrf(g,GX+0x14,H);
    u64 r=st_call(g,f_11c1e0,(u64[]){out},1,&ok); ACHECK(ok&&r==out);
    *x=rdf(g,out); *y=rdf(g,out+4); ga_free(out);
}
/* task 40: a stand-in for a culling pass in the H2 test: it records cam+0x98 as it sees it and, when asked, writes it */
static u64 h2t_cam; static u32 h2t_seen, h2t_put;
static void h2t_fn(KCtx *g){ h2t_seen=rd32(g,h2t_cam+CAM_M11); if(h2t_put) wr32(g,h2t_cam+CAM_M11,h2t_put); }
/* task 40 review: a stand-in pass for the wrappers h2b_wrap / h2c_wrap, recording cam+0x98 of three cameras as it sees them */
static u64 h2w_cam[3]; static u32 h2w_seen[3]; static int h2w_calls;
static void h2w_fn(KCtx *g){ h2w_calls++; for(int i=0;i<3;i++) h2w_seen[i]=rd32(g,h2w_cam[i]+CAM_M11); }
void f_19cfc0(KCtx *);
/* ---- task 42: H3 through the real hooks and the lifted bodies -----------------------------------------------------------------------
 * One dk pass as f_166640 makes it: setScreenMode = the hook f_126500 (lifted f_126500 -> f_11c150, f_1433a0) with return address
 * 0x140166677 and guest rdi = the list ([list+0x18] = the first object); the next objects = the hook f_4ad270 (runtime/bgload.c; lifted
 * f_4ad270 decodes the id the lifted f_4ad240 made) with return address 0x140166693; the end of the list = id 0 (rax = 0); the
 * restore = f_126500 with return address 0x1401666bf.  ws[i] = GX+0xac after object i's hook; before every H3b call GX+0xac holds a
 * signalling NaN, so ws[i] is that NaN exactly when H3b wrote nothing; ws[n] = after the end of the list. */
void f_4ad270(KCtx *);
#define H3T_SNAN 0x7f80dead
#define ID_TAB (GS_BASE+0x2b0d720ull)       /* the id table (runtime/idtable.c PTAB) and its guard: saved and restored around the test */
static int h3t_pass(KCtx *g,const u64 *objs,int n,float *ws,float *wsend){
    u64 list=ga_calloc(1,0x80), out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10), back=ga_calloc(1,0x20); int ok=h3_depth==0, k;
    wr64(g,list+0x18,objs[0]); wrf(g,mode,1.0f);                                                 /* f_166640's mode {1,0,0,0} */
    st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); ok&=k; ws[0]=rdf(g,GX+0xac);
    for(int i=1;i<n;i++){ u32 id=(u32)kh_call(g,GS_BASE+0x4ad240ull,objs[i],0,0,0); wr32(g,GX+0xac,H3T_SNAN);
        u64 r=st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){id},1,&k); ok&=k&&r==objs[i]; ws[i]=rdf(g,GX+0xac); }
    wr32(g,GX+0xac,H3T_SNAN); u64 r=st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){0},1,&k); ok&=k&&r==0; *wsend=rdf(g,GX+0xac);
    st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){back,out},2,&k); ok&=k&&h3_depth==0;         /* the loop's restore: the pass is over */
    ga_free(list); ga_free(out); ga_free(mode); ga_free(back); return ok;
}
static u64 h3t_obj(KCtx *g,u64 vt,u32 grp){ u64 o=ga_calloc(1,0x40); wr64(g,o,vt); wr32(g,o+0x14,grp); return o; }
/* task 53: the same pass, recording all four MagicScale words after each object's hook (ms[i][0..3] = GX+0xa0..0xac); before every H3b
 * call all four hold the signalling NaN, so a word that is still that NaN was not written; ms0 = the loop's MagicScale (f_1433a0's) */
static int h3t_pass4(KCtx *g,const u64 *objs,int n,u32 (*ms)[4],u32 *ms0){
    u64 list=ga_calloc(1,0x80), out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10), back=ga_calloc(1,0x20); int ok=h3_depth==0, k;
    wr64(g,list+0x18,objs[0]); wrf(g,mode,1.0f);
    st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); ok&=k&&h3_depth==(asp_mode?1:0);
    for(int j=0;j<4;j++){ ms[0][j]=rd32(g,GX+0xa0+4ull*(u64)j); ms0[j]=h3_ms[0][j]; }
    for(int i=1;i<n;i++){ u32 id=(u32)kh_call(g,GS_BASE+0x4ad240ull,objs[i],0,0,0); for(int j=0;j<4;j++) wr32(g,GX+0xa0+4ull*(u64)j,H3T_SNAN);
        u64 r=st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){id},1,&k); ok&=k&&r==objs[i]; for(int j=0;j<4;j++) ms[i][j]=rd32(g,GX+0xa0+4ull*(u64)j); }
    u64 r=st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){0},1,&k); ok&=k&&r==0;
    st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){back,out},2,&k); ok&=k&&h3_depth==0;
    ga_free(list); ga_free(out); ga_free(mode); ga_free(back); return ok;
}
static void h3_selftest(KCtx *g){
    /* the anchor lookup: every class of the table, the generic children by their owner group, the guards */
    { int w;
      for(int c=0;c<H3_NCLS;c++) if(h3cls[c].anchor!=H3_GENERIC){ ACHECK(kh_aspect_h3_anchor(h3cls[c].vt,0,&w)==h3cls[c].anchor&&w==H3W_CLASS); }   /* group 0: no veto */
      static const u64 gen[3]={GS_BASE+0x5b1a70ull,GS_BASE+0x5b1a40ull,GS_BASE+0x5b1aa0ull};                 /* dk::Sprite, SpriteImage, SpriteMessage */
      for(int i=0;i<3;i++){
          ACHECK(kh_aspect_h3_anchor(gen[i],1,&w)==H3_BOTTOM&&w==H3W_GROUP&&kh_aspect_h3_anchor(gen[i],2,NULL)==H3_BOTTOM&&kh_aspect_h3_anchor(gen[i],3,NULL)==H3_BOTTOM);
          ACHECK(kh_aspect_h3_anchor(gen[i],6,NULL)==H3_TOP&&kh_aspect_h3_anchor(gen[i],7,&w)==H3_TOP&&w==H3W_GROUP);
          ACHECK(kh_aspect_h3_anchor(gen[i],0x11,&w)==H3_CENTRE&&w==H3W_GROUP);
          ACHECK(kh_aspect_h3_anchor(gen[i],0,&w)==H3_CENTRE&&w==H3W_GROUP&&kh_aspect_h3_anchor(gen[i],99,&w)==H3_CENTRE&&w==H3W_GROUP_UNKNOWN); }
      /* "a child takes its owner's anchor": the (owner class, group) pairs M measured (+0x14 of the drawn objects) give the child the
       * owner's anchor */
      static const struct { u64 vt; u32 grp; } own[]={{GS_BASE+0x5b1c28ull,1},{GS_BASE+0x5b1bf8ull,2},{GS_BASE+0x5b2100ull,3},{GS_BASE+0x5b2078ull,3},{GS_BASE+0x5b2018ull,3},
          {GS_BASE+0x5b2048ull,3},{GS_BASE+0x5b1fe8ull,3},{GS_BASE+0x5b21e0ull,6},{GS_BASE+0x5b2430ull,7},{GS_BASE+0x5b2400ull,7},{GS_BASE+0x5b24f8ull,0x11},
          {GS_BASE+0x5b2158ull,4},{GS_BASE+0x5b2158ull,5}};                                     /* the party gauges (statically: review finding 5) */
      for(size_t i=0;i<sizeof own/sizeof *own;i++){ int wo, wc; ACHECK(kh_aspect_h3_anchor(own[i].vt,own[i].grp,&wo)==kh_aspect_h3_anchor(gen[0],own[i].grp,&wc)&&wo==H3W_CLASS&&wc==H3W_GROUP); }
      /* the decisions of section 1 of implementation notes (measured: M40, run-B42/M), pinned: (vtable, its measured group, anchor) */
      static const struct { u32 vt, grp; int a; } dec[]={{0x5b2078,3,H3_BOTTOM},{0x5b2018,3,H3_BOTTOM},{0x5b2048,3,H3_BOTTOM},{0x5b1fe8,3,H3_BOTTOM},{0x5b2100,3,H3_BOTTOM},
          {0x5b1c28,1,H3_BOTTOM},{0x5b1bf8,2,H3_BOTTOM},{0x5b1bc8,2,H3_BOTTOM},{0x5b21e0,6,H3_TOP},{0x5b2430,7,H3_TOP},{0x5b2400,7,H3_TOP},{0x5b2460,7,H3_TOP},
          {0x5b2490,7,H3_TOP},{0x5b2370,8,H3_TOP},{0x5b2340,0,H3_TOP},{0x5b1df0,0,H3_CENTRE},{0x5b24f8,0x11,H3_CENTRE},{0x5b2540,15,H3_CENTRE},{0x5b1dc0,0,H3_CENTRE},
          {0x5c5180,0,H3_CENTRE},{0x5b1a70,7,H3_TOP},{0x5b1a70,6,H3_TOP},{0x5b1a70,8,H3_TOP},{0x5b1a70,1,H3_BOTTOM},{0x5b1a40,1,H3_BOTTOM},{0x5b1a40,0,H3_CENTRE},
          /* task 42 review finding 5: the party gauges' groups 4 / 5 (f_181d20 -> f_17ed50, statically) and their children */
          {0x5b2158,4,H3_BOTTOM},{0x5b2158,5,H3_BOTTOM},{0x5b2048,4,H3_BOTTOM},{0x5b2078,5,H3_BOTTOM},{0x5b2018,4,H3_BOTTOM},
          {0x5b1a70,4,H3_BOTTOM},{0x5b1a70,5,H3_BOTTOM},{0x5b1a40,4,H3_BOTTOM},{0x5b1a40,5,H3_BOTTOM}};
      for(size_t i=0;i<sizeof dec/sizeof *dec;i++) ACHECK(kh_aspect_h3_anchor(GS_BASE+dec[i].vt,dec[i].grp,NULL)==dec[i].a);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b1a70ull,4,&w)==H3_BOTTOM&&w==H3W_GROUP&&kh_aspect_h3_anchor(GS_BASE+0x5b1a70ull,5,&w)==H3_BOTTOM&&w==H3W_GROUP);   /* not "group unknown" */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b2400ull,4,&w)==H3_CENTRE&&w==H3W_DISAGREE&&kh_aspect_h3_anchor(GS_BASE+0x5b2158ull,5,&w)==H3_BOTTOM&&w==H3W_CLASS);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b2078ull,7,&w)==H3_CENTRE&&w==H3W_DISAGREE);                 /* a bottom class in the top group: centred */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b2430ull,3,&w)==H3_CENTRE&&w==H3W_DISAGREE);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b24f8ull,7,&w)==H3_CENTRE&&w==H3W_CLASS);                     /* attached to 3D: centre whatever its group */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b2078ull,99,&w)==H3_BOTTOM&&w==H3W_CLASS);                    /* a group not in the table: the class holds */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b2078ull,0x11,&w)==H3_BOTTOM&&w==H3W_CLASS&&kh_aspect_h3_anchor(GS_BASE+0x5b2430ull,0,&w)==H3_TOP&&w==H3W_CLASS);   /* a centre group does not veto */
      /* a class not in the table stays centred whatever its group (TASK42 item 1; review finding 4), the reason says whether the group is known */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,7,&w)==H3_CENTRE&&w==H3W_UNLISTED&&kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,2,&w)==H3_CENTRE&&w==H3W_UNLISTED);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,3,&w)==H3_CENTRE&&w==H3W_UNLISTED&&kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,6,&w)==H3_CENTRE&&w==H3W_UNLISTED);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,0x11,&w)==H3_CENTRE&&w==H3W_UNLISTED&&kh_aspect_h3_anchor(GS_BASE+0x5b1234ull,9,&w)==H3_CENTRE&&w==H3W_UNCLASSIFIED);
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b1a70ull,8,&w)==H3_TOP&&w==H3W_GROUP&&kh_aspect_h3_anchor(GS_BASE+0x5b2370ull,8,&w)==H3_TOP&&w==H3W_CLASS);   /* the counter */
      ACHECK(kh_aspect_h3_anchor(GS_BASE+0x5b23a0ull,10,&w)==H3_TOP&&w==H3W_CLASS);
      for(int i=0;i<3;i++) ACHECK(kh_aspect_h3_anchor(gen[i],10,&w)==H3_TOP&&w==H3W_GROUP);
      ACHECK(kh_aspect_h3_anchor(0,99,&w)==H3_CENTRE&&w==H3W_UNCLASSIFIED);
      ACHECK(h3_value(135.0f,135.0f,H3_TOP)==0.0f&&h3_value(135.0f,135.0f,H3_BOTTOM)==270.0f&&h3_value(135.0f,135.0f,H3_CENTRE)==135.0f);
      ACHECK(h3_value(188.0625f,188.0625f,H3_TOP)==0.0f&&h3_value(188.0625f,188.0625f,H3_BOTTOM)==376.125f); }
    /* the passes through the real hooks */
    u8 gxsave[0x100]; memcpy(gxsave,hp(g,GX),0x100); u32 guard=rd32(g,GX_GUARD);                  /* GX's first 0x100 bytes: sizes, mode, MS, scissor */ wr32(g,GX_GUARD,0x80000000u);
    u8 tsave[0x204]; memcpy(tsave,hp(g,ID_TAB),0x204); int tlive=rd32(g,ID_TAB+0x200)!=0;
    if(!tlive){ for(int i=0;i<64;i++) wr64(g,ID_TAB+8ull*i,~0ull); wr32(g,ID_TAB+0x200,0x80000000u); }   /* as 0x4ad2c0 leaves it */
    ACHECK(hud_draw_owner(GS_BASE+0x5b1df0,0) && hud_draw_owner(GS_BASE+0x5b2340,0));
    ACHECK(hud_draw_owner(GS_BASE+0x5b1a70,3) && !hud_draw_owner(GS_BASE+0x5b1aa0,0));
    ACHECK(!hud_draw_owner(GS_BASE+0x5b1dc0,0) && !hud_draw_owner(GS_BASE+0x5c5180,0));
    int sm=asp_mode, su=asp_h1_uncond, so=asp_off_h3, sdk=kh_aspect_dk, sml=kh_aspect_m; u64 s3=atomic_load(&last3d_plus1); H3Stat sh; memcpy(&sh,&h3,sizeof sh);
    kh_aspect_m=0; kh_aspect_dk=1; asp_h1_uncond=0; asp_off_h3=0; h3.warned=1; h3.warned_inner=1;   /* no WARNING lines from the test cases */
    /* f_1433a0 (0x1433a0..0x14355f, this = GX+8) reads the screen GX+0x18 / +0x1c, GX+0x10 / +0x14, the scales GX+0x20 / +0x24 / +0x28 and
     * the GS frame 512 x 416 at GX+0x38 / +0x3c and GX+0x48 / +0x4c (the values a run has: task 42's first-pass line); it writes MS at
     * GX+0xa0..0xac (MS.w = mode.w * h + (GX+0x14 - h) / 2, h = 9W/16 on a narrow screen), SafeScale GX+0xb0, the flag GX+0xb4 = 1 */
    #define H3SCREEN(W_,H_) do{ wrf(g,GX+0x18,(float)(W_)); wrf(g,GX+0x1c,(float)(H_)); wrf(g,GX+0x10,(float)(W_)); wrf(g,GX+0x14,(float)(H_)); wrf(g,GX+0x20,1); wrf(g,GX+0x24,1); wrf(g,GX+0x28,1); \
                                wrf(g,GX+0x38,512); wrf(g,GX+0x3c,416); wrf(g,GX+0x48,512); wrf(g,GX+0x4c,416); }while(0)
    /* the objects: a top class FIRST in its pass (the H3a case), then bottom, a generic child of the INFORMATION group (top), one of
     * the command group (bottom), the lock-on (centre), a generic child of group 0 (centre), an unclassified class of a group not in the
     * table (centre), a bottom
     * class again (after the centre ones: the centre ones must have restored MS.w_centre in between) */
    const u64 o[8]={h3t_obj(g,GS_BASE+0x5b2400ull,7),h3t_obj(g,GS_BASE+0x5b2078ull,3),h3t_obj(g,GS_BASE+0x5b1a70ull,7),h3t_obj(g,GS_BASE+0x5b1a70ull,1),
                    h3t_obj(g,GS_BASE+0x5b24f8ull,0x11),h3t_obj(g,GS_BASE+0x5b1a70ull,0),h3t_obj(g,GS_BASE+0x5b9990ull,9),h3t_obj(g,GS_BASE+0x5b2048ull,3)};
    static const int exp[8]={-1,+1,-1,+1,0,0,0,+1};
    static const int scr[6][2]={{1920,1350},{2670,1878},{1920,1080},{2560,1440},{3840,2160},{2560,1080}};
    for(int s=0;s<6;s++){ H3SCREEN(scr[s][0],scr[s][1]); double o_=kh_aspect_o(scr[s][0],scr[s][1]); float ws[8], we, pc[8], pce;
        note_3d(kh_game_frames());
        asp_mode=0; u64 np0=h3.nopass, ps0=h3.passes; ACHECK(h3t_pass(g,o,8,pc,&pce));                  /* pc: the exe's MS.w_centre */
        ACHECK(fbits(pc[0])!=H3T_SNAN&&fbits(pce)==H3T_SNAN); for(int i=1;i<8;i++) ACHECK(fbits(pc[i])==H3T_SNAN);   /* pc: H3b writes nothing */
        { float pm[8], pme; kh_aspect_m=1; ACHECK(h3t_pass(g,o,8,pm,&pme)); kh_aspect_m=0;              /* pc with the measurement M: the hooks run, H3 does not */
          ACHECK(fbits(pm[0])==fbits(pc[0])&&fbits(pme)==H3T_SNAN); for(int i=1;i<8;i++) ACHECK(fbits(pm[i])==H3T_SNAN);
          ACHECK(h3.nopass==np0&&h3.passes==ps0&&h3_depth==0); }
        if(s<2) ACHECK(pc[0]==(float)o_);                                                               /* the exe's MS.w is the band offset o */
        else ACHECK(pc[0]==0.0f);
        asp_mode=1; u64 a0=h3.anchored, id0=h3.identity; ACHECK(h3t_pass(g,o,8,ws,&we));                /* fill */
        ACHECK(fbits(we)==H3T_SNAN);                                                                     /* the end of the list: nothing */
        if(s<2){ for(int i=0;i<8;i++) ACHECK(ws[i]==pc[0]+(float)exp[i]*(float)o_);
                 ACHECK(ws[0]==0.0f&&ws[1]==2.0f*pc[0]&&h3.anchored==a0+8); }                          /* top: GS y 0 at row 0; bottom: 416 at the last row */
        else { ACHECK(fbits(ws[0])==fbits(pc[0])&&h3.identity==id0+1&&h3.anchored==a0); for(int i=1;i<8;i++) ACHECK(fbits(ws[i])==H3T_SNAN); } }   /* 16:9 or wider: nothing written */
    /* the bars on (no 3D view in this frame or the previous one): the pass stays centred, nothing written; with
     * KHNMC_ASPECT_TEST_H1=unconditional it is anchored */
    { H3SCREEN(1920,1350); float ws[8], we; atomic_store(&last3d_plus1,0); asp_mode=1; u64 b0=h3.bars;
      ACHECK(h3t_pass(g,o,8,ws,&we)&&ws[0]==135.0f&&h3.bars==b0+1); for(int i=1;i<8;i++) ACHECK(fbits(ws[i])==H3T_SNAN);
      asp_h1_uncond=1; ACHECK(h3t_pass(g,o,8,ws,&we)&&ws[0]==0.0f&&ws[1]==270.0f&&ws[4]==135.0f); asp_h1_uncond=0; note_3d(kh_game_frames()); }
    /* the test aid off: nothing written, counted */
    { float ws[8], we; asp_off_h3=1; u64 f0=h3.off; ACHECK(h3t_pass(g,o,8,ws,&we)&&ws[0]==135.0f&&h3.off==f0+1); for(int i=1;i<8;i++) ACHECK(fbits(ws[i])==H3T_SNAN); asp_off_h3=0; }
    /* a first object of each anchor (H3a anchors the head of the list, not only the objects from f_4ad270) and an empty list */
    { const u64 one[3]={o[1],o[4],o[2]}; float ws[1], we;
      for(int i=0;i<3;i++){ ACHECK(h3t_pass(g,&one[i],1,ws,&we)&&ws[0]==(i==0?270.0f:i==1?135.0f:0.0f)&&fbits(we)==H3T_SNAN); }
      u64 z=0; u64 e0=h3.empty; ACHECK(h3t_pass(g,&z,1,ws,&we)&&ws[0]==135.0f&&h3.empty==e0+1); }
    /* the owner field is +0x14 (a u32): the same generic child moves with the group written there, and nothing else in the object
     * counts (+0x10 flags and +0x18 hold group-like values here) */
    { u64 c=h3t_obj(g,GS_BASE+0x5b1a70ull,7); wr32(g,c+0x10,3); wr32(g,c+0x18,3); const u64 two[2]={o[4],c}; float ws[2], we;
      ACHECK(h3t_pass(g,two,2,ws,&we)&&ws[1]==0.0f);
      wr32(g,c+0x14,3); wr32(g,c+0x10,7); wr32(g,c+0x18,7); ACHECK(h3t_pass(g,two,2,ws,&we)&&ws[1]==270.0f);
      wr32(g,c+0x14,0x00000103u); ACHECK(h3t_pass(g,two,2,ws,&we)&&ws[1]==135.0f);   /* group 0x103: not in the table */
      ga_free(c);
      u64 u=h3t_obj(g,GS_BASE+0x5b9990ull,7); const u64 two2[2]={o[4],u};                             /* a class not in the table stays centred */
      u64 ul0=h3.why[H3W_UNLISTED], uc0=h3.why[H3W_UNCLASSIFIED];                                      /* whatever its group (review finding 4), counted */
      ACHECK(h3t_pass(g,two2,2,ws,&we)&&ws[1]==135.0f); wr32(g,u+0x14,2); ACHECK(h3t_pass(g,two2,2,ws,&we)&&ws[1]==135.0f);
      wr32(g,u+0x14,9); ACHECK(h3t_pass(g,two2,2,ws,&we)&&ws[1]==135.0f); ACHECK(h3.why[H3W_UNLISTED]==ul0+2&&h3.why[H3W_UNCLASSIFIED]==uc0+1); ga_free(u); }
    /* only the dk loop's call sites: f_126500 from elsewhere (here 0x1401704a1) and f_4ad270 from elsewhere (0x1401668ff, f_1668d0's
     * walk of the manager list) inside a pass change nothing */
    { u64 list=ga_calloc(1,0x40), out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10); wr64(g,list+0x18,o[1]); wrf(g,mode,1.0f); int k; u64 ps=h3.passes;
      st_call_ra(g,f_126500,GS_BASE+0x1704a1ull,list,(u64[]){out,mode},2,&k); ACHECK(k&&rdf(g,GX+0xac)==135.0f&&h3_depth==0&&h3.passes==ps);
      st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); ACHECK(k&&rdf(g,GX+0xac)==270.0f&&h3_depth==1);
      u32 id=(u32)kh_call(g,GS_BASE+0x4ad240ull,o[0],0,0,0); wr32(g,GX+0xac,H3T_SNAN);
      ACHECK(st_call_ra(g,f_4ad270,GS_BASE+0x1668ffull,list,(u64[]){id},1,&k)==o[0]&&k&&rd32(g,GX+0xac)==H3T_SNAN);
      ACHECK(st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){id},1,&k)==o[0]&&k&&rdf(g,GX+0xac)==0.0f);
      st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){mode,out},2,&k); ACHECK(k&&h3_depth==0); ga_free(list); ga_free(out); ga_free(mode); }
    /* the band mismatch: MagicScale.w not the band offset (here written by hand after the exe's f_126500): the pass is left, counted */
    { u64 mm0=h3.mismatch; int d0=h3_depth; wrf(g,GX+0xac,100.0f); u64 sv=KH_RDI(g); u64 list=ga_calloc(1,0x40); wr64(g,list+0x18,o[0]); KH_RDI(g)=list;
      h3a_post(g); ACHECK(h3.mismatch==mm0+1&&rdf(g,GX+0xac)==100.0f&&h3_depth==d0+1&&!h3_on[h3_depth-1]);
      wr32(g,GX+0xac,H3T_SNAN); h3b_post(g,o[1]); ACHECK(rd32(g,GX+0xac)==H3T_SNAN); h3_end(); ACHECK(h3_depth==d0); KH_RDI(g)=sv; ga_free(list); }
    /* review finding 2: setScreenMode INSIDE an object (YS::WINDOW_OBJ2D's pair: f_126500 from 0x1403bc4c7 with its zoom mode {0.8, 0,
     * 0, 0.1} and the saved mode out in rcx, then from 0x1403bc4f0 with the saved mode back), through the hook and the lifted body.
     * f_126500 recomputes MagicScale: after the pair MS.w is the band centre's again (the exe: 135).  An anchored object gets its
     * anchored MS.w again after the call that brings the loop's MagicScale back; the call that sets another mode is left as the exe
     * made it; a centre object is left (the value is the centre's); all counted by (class, return address); nothing is counted or
     * written outside a pass, with the test aid, in pc or with the bars on */
    { H3SCREEN(1920,1350); note_3d(kh_game_frames()); asp_mode=1;
      const u64 RA_WSET=GS_BASE+0x3bc4ccull, RA_WBACK=GS_BASE+0x3bc4f5ull;
      H3Inner hin[16]; memcpy(hin,h3in,sizeof hin); int nhin=n_h3in; u64 hcut=h3in_cut; n_h3in=0; h3in_cut=0;
      u64 list=ga_calloc(1,0x40), out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10), wm=ga_calloc(1,0x10), sv=ga_calloc(1,0x20), tmp=ga_calloc(1,0x20);
      u64 win=h3t_obj(g,GS_BASE+0x5c5180ull,0); int k, kk=1; u32 id;
      wrf(g,mode,1.0f); wrf(g,wm,0.8f); wrf(g,wm+12,0.1f);
      u64 i0=h3.inner, b0=h3.inner_back, a0=h3.inner_anch, r0=h3.inner_reapplied, x0=h3.inner_other;
      st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm},2,&k); kk&=k; float wexe=rdf(g,GX+0xac);           /* outside a pass: the exe's value */
      st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); kk&=k;
      ACHECK(kk&&wexe!=135.0f&&wexe!=0.0f&&wexe!=270.0f&&h3.inner==i0&&h3_depth==0);
      wr64(g,list+0x18,o[0]);                                                                           /* the list head: a top class */
      st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); ACHECK(k&&h3_depth==1&&rdf(g,GX+0xac)==0.0f);
      st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm},2,&k); ACHECK(k&&rdf(g,GX+0xac)==wexe);            /* its own mode: as the exe made it */
      st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); ACHECK(k&&rdf(g,GX+0xac)==0.0f);           /* the loop's mode back: top again */
      ACHECK(h3.inner==i0+2&&h3.inner_back==b0+1&&h3.inner_anch==a0+2&&h3.inner_reapplied==r0+1&&h3.inner_other==x0+1);
      /* a mode of its own whose MS.w equals the band centre's: mode {scale, kind, x, y} = {0.5, 0, 0, -0.5} (f_1433a0: MS.w = y * s9W/16
       * + (H - s9W/16) / 2 = -270 + 405 = 135, MS.x / MS.y halved): not the loop's MagicScale, so left as the exe made it (only all
       * of GX+0xa0..0xac equal to H3a's counts as the loop's mode back) */
      { u64 wm0=ga_calloc(1,0x10); wrf(g,wm0,0.5f); wrf(g,wm0+12,-0.5f); st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm0},2,&k); float w0=rdf(g,GX+0xac);
        ACHECK(k&&w0==135.0f&&rd32(g,GX+0xa0)!=h3_ms[0][0]); st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); ACHECK(k&&rdf(g,GX+0xac)==0.0f);
        ACHECK(h3.inner==i0+4&&h3.inner_back==b0+2&&h3.inner_anch==a0+4&&h3.inner_reapplied==r0+2&&h3.inner_other==x0+2); ga_free(wm0); i0+=2; b0+=1; a0+=2; r0+=1; x0+=1; }
      id=(u32)kh_call(g,GS_BASE+0x4ad240ull,o[1],0,0,0); ACHECK(st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){id},1,&k)==o[1]&&k&&rdf(g,GX+0xac)==270.0f);
      st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm},2,&k); ACHECK(k&&rdf(g,GX+0xac)==wexe);
      st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); ACHECK(k&&rdf(g,GX+0xac)==270.0f);         /* bottom again */
      id=(u32)kh_call(g,GS_BASE+0x4ad240ull,win,0,0,0); ACHECK(st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){id},1,&k)==win&&k&&rdf(g,GX+0xac)==135.0f);
      u64 a1=h3.inner_anch, r1=h3.inner_reapplied;                                                       /* the window itself: centre */
      st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm},2,&k); ACHECK(k&&rdf(g,GX+0xac)==wexe);
      st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); ACHECK(k&&rdf(g,GX+0xac)==135.0f);
      ACHECK(h3.inner==i0+6&&h3.inner_back==b0+3&&h3.inner_anch==a1&&h3.inner_reapplied==r1&&a1==a0+4&&r1==r0+2&&h3.inner_other==x0+2);
      ACHECK(st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){0},1,&k)==0&&k);
      st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){tmp,out},2,&k); ACHECK(k&&h3_depth==0);
      { int nw=0; for(int i=0;i<n_h3in;i++) if(h3in[i].vt==GS_BASE+0x5c5180ull&&h3in[i].a==H3_CENTRE&&h3in[i].n==1&&h3in[i].back==(h3in[i].ra==RA_WBACK)) nw++;
        ACHECK(n_h3in==6&&nw==2); }                                                                      /* 3 classes x 2 return addresses */
      /* outside an anchored pass nothing: the test aid, pc, the bars on */
      for(int c=0;c<3;c++){ u64 ic=h3.inner; if(c==0) asp_off_h3=1; else if(c==1) asp_mode=0; else atomic_store(&last3d_plus1,0);
          st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); ACHECK(k&&rdf(g,GX+0xac)==135.0f);
          st_call_ra(g,f_126500,RA_WSET,list,(u64[]){sv,wm},2,&k); ACHECK(k&&rdf(g,GX+0xac)==wexe);
          st_call_ra(g,f_126500,RA_WBACK,list,(u64[]){tmp,sv},2,&k); ACHECK(k&&rdf(g,GX+0xac)==135.0f&&h3.inner==ic);
          st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){0},1,&k); st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){tmp,out},2,&k); ACHECK(k&&h3_depth==0);
          asp_off_h3=0; asp_mode=1; note_3d(kh_game_frames()); }
      ga_free(list); ga_free(out); ga_free(mode); ga_free(wm); ga_free(sv); ga_free(tmp); ga_free(win);
      memcpy(h3in,hin,sizeof hin); n_h3in=nhin; h3in_cut=hcut; }
    /* ---- task 53: the HUD scale through the same hooks (f_126500 at 0x140166677 with the lifted f_1433a0, f_4ad270 at 0x140166693) ----
     * The objects of one pass: a top class first (H3a), the HP class, the command menu's generic child (group 1), COMMAND_ONE (2), the
     * reaction command (2), the lock-on (centre), a generic child of the gauges (3), the party gauges (4) and a child of group 5, the
     * enemy gauge (top), a generic child of group 0 (centre), a gauge class in the top group (class and group disagree: centre), a
     * class not in the table in group 3 (centre), the face ring (3).  hk = the scale each must get (-1 none), an = its anchor. */
    { const u64 ob[14]={h3t_obj(g,GS_BASE+0x5b2400ull,7),h3t_obj(g,GS_BASE+0x5b2078ull,3),h3t_obj(g,GS_BASE+0x5b1a70ull,1),h3t_obj(g,GS_BASE+0x5b1bf8ull,2),
                        h3t_obj(g,GS_BASE+0x5b1bc8ull,2),h3t_obj(g,GS_BASE+0x5b24f8ull,0x11),h3t_obj(g,GS_BASE+0x5b1a70ull,3),h3t_obj(g,GS_BASE+0x5b2158ull,4),
                        h3t_obj(g,GS_BASE+0x5b1a70ull,5),h3t_obj(g,GS_BASE+0x5b21e0ull,6),h3t_obj(g,GS_BASE+0x5b1a70ull,0),h3t_obj(g,GS_BASE+0x5b2078ull,7),
                        h3t_obj(g,GS_BASE+0x5b9990ull,3),h3t_obj(g,GS_BASE+0x5b2018ull,3)};
      static const int hk[14]={-1,HUD_HP,HUD_CMD,HUD_CMD,HUD_CMD,-1,HUD_HP,HUD_HP,HUD_HP,-1,-1,-1,-1,HUD_HP}, an[14]={-1,1,1,1,1,0,1,1,1,-1,0,0,0,1};
      static const double PX[2]={-70.0,590.0}, PY[2]={416.0,405.0};                                   /* the pivots (implementation notes: measured), pinned */
      double hs0[2]={hud_s[0],hud_s[1]}; int hset0[2]={hud_set[0],hud_set[1]}; HudStat hsv; memcpy(&hsv,&hud,sizeof hsv);
      u32 ms[14][4], ref[14][4], m0[4];
      /* the arithmetic: parse, the screens, the map keeps the pivot's pixel and scales distances by s */
      { double v=0; ACHECK(kh_hud_scale_parse("1.25",&v)==0&&v==1.25&&kh_hud_scale_parse("0.5",&v)==0&&v==0.5&&kh_hud_scale_parse("3.0",&v)==0&&v==3.0&&kh_hud_scale_parse("3",&v)==0&&v==3.0);
        ACHECK(decimal_range("0.06",&v,0,0.5)==0 && v==0.06 && decimal_range("0.51",&v,0,0.5)==2);
        ACHECK(decimal_range("0",&v,0,0.5)==0 && !v && decimal_range(".25",&v,0,0.5)==1);
        /* The two halves of a Config row must receive exactly the same transform. */
        u32 left[4]={fbits(1),fbits(1),0,0},right[4]; memcpy(right,left,sizeof left);
        camp_matrix(left,300,(float[]){40,138,80,106},GS_BASE+0x5bab18,36);
        camp_matrix(right,300,(float[]){178,493,80,106},0,36);
        ACHECK(!memcmp(left,right,sizeof left) && bf(left[0])>1);
        ACHECK(kh_hud_scale_parse("0.49",&v)==2&&kh_hud_scale_parse("3.01",&v)==2&&kh_hud_scale_parse("0",&v)==2);
        ACHECK(kh_hud_scale_parse("1,25",&v)==1&&kh_hud_scale_parse("1.25x",&v)==1&&kh_hud_scale_parse("",&v)==1&&kh_hud_scale_parse(".5",&v)==1&&kh_hud_scale_parse("1.",&v)==1&&kh_hud_scale_parse("-1",&v)==1&&kh_hud_scale_parse("1e0",&v)==1&&kh_hud_scale_parse(NULL,&v)==1);
        hud_s[0]=1.25; hud_s[1]=1.5; asp_mode=1;
        ACHECK(kh_aspect_hud_scale(HUD_CMD,2007,2853)==1.25&&kh_aspect_hud_scale(HUD_HP,2007,2853)==1.5&&kh_aspect_hud_scale(HUD_HP,1000,1001)==1.5);
        ACHECK(kh_aspect_hud_scale(HUD_HP,2853,2007)==1.0&&kh_aspect_hud_scale(HUD_HP,2007,2007)==1.0&&kh_aspect_hud_scale(HUD_CMD,1920,1350)==1.0&&kh_aspect_hud_scale(HUD_CMD,2670,1878)==1.0&&kh_aspect_hud_scale(HUD_CMD,1920,1080)==1.0);
        asp_mode=0; ACHECK(kh_aspect_hud_scale(HUD_CMD,2007,2853)==1.0&&kh_aspect_hud_scale(HUD_HP,2007,2853)==1.0); asp_mode=1;
        ACHECK(hud_kind(1)==HUD_CMD&&hud_kind(2)==HUD_CMD&&hud_kind(3)==HUD_HP&&hud_kind(4)==HUD_HP&&hud_kind(5)==HUD_HP&&hud_kind(0)<0&&hud_kind(6)<0&&hud_kind(7)<0&&hud_kind(8)<0&&hud_kind(0x11)<0);
        u32 t[4]={fbits(2.93994f),fbits(2.71379f),fbits(250.875f),fbits(1724.06f)}; hud_ms(t,1.5,590.0f,405.0f);
        ACHECK(near(590.0*bf(t[0])+bf(t[2]),590.0*2.93994f+250.875f,1e-3)&&near(405.0*bf(t[1])+bf(t[3]),405.0*2.71379f+1724.06f,1e-3));   /* the pivot keeps its pixel */
        ACHECK(near((377.0*bf(t[0])+bf(t[2]))-(590.0*bf(t[0])+bf(t[2])),1.5*(377.0-590.0)*2.93994f,1e-3)&&near(bf(t[1]),1.5*2.71379f,1e-6));   /* 213 GS columns: 1.5 times as many pixels */
        /* the margins: a scale >= 1 moves every point of the resting boxes away from the pivot's edges only (up / inward); the largest
         * allowed (3.0) keeps the boxes inside the frame of ANY screen taller than wide (x -85.33 .. 597.33, y -b .. 416 + b, b > 161.7):
         * gauges x 377..590 y 305..405 about (590, 405), command menu x -70..82 y 290..416 about (-70, 416) */
        double bmin=208.0*(16.0/9.0-1.0), xl=-256.0*(16.0/9.0)/(4.0/3.0)+256.0, xr=512.0-xl;
        ACHECK(near(xl,-85.333,1e-3)&&kh_aspect_b(1000,1001)>bmin&&kh_aspect_b(2007,2853)>bmin);
        ACHECK(590.0-(590.0-377.0)*3.0>=xl&&405.0-(405.0-305.0)*3.0>=-bmin&&-70.0+(82.0+70.0)*3.0<=xr&&416.0-(416.0-290.0)*3.0>=-bmin); }
      /* the passes */
      H3SCREEN(2007,2853); note_3d(kh_game_frames());
      double o_=kh_aspect_o(2007,2853);
      asp_mode=0; ACHECK(h3t_pass4(g,ob,14,ref,m0)); for(int i=1;i<14;i++) for(int j=0;j<4;j++) ACHECK(ref[i][j]==H3T_SNAN);   /* pc: nothing written */
      ACHECK(near(bf(ref[0][3]),o_,0.5)&&bf(ref[0][0])>0&&bf(ref[0][1])>0);                            /* the loop's MagicScale (f_1433a0) */
      const u32 *L=ref[0];
      asp_mode=1;
      double es=enemy_scale,ss=style_scale,sr=style_raise;
      enemy_scale=1.5; style_scale=2; style_raise=0.25;
      ACHECK(h3t_pass4(g,ob,14,ms,m0) && near(bf(ms[9][0]),1.5*bf(L[0]),1e-5));
      u64 rank=h3t_obj(g,GS_BASE+0x5b1df0ull,0);
      ACHECK(h3t_pass4(g,&rank,1,ms,m0) && near(bf(ms[0][1]),2*bf(L[1]),1e-5));
      ACHECK(near(bf(ms[0][3])+208*bf(ms[0][1]),2853*0.25,1.0));
      ga_free(rank);
      enemy_scale=style_scale=1; style_raise=0;
      for(int c=0;c<4;c++){                                                                               /* scales: defaults, 1 / 1, command 1 only, 2.0 / 0.5 */
          hud_s[0]=c==0?1.25:c==1?1.0:c==2?1.0:2.0; hud_s[1]=c==0?1.5:c==1?1.0:c==2?1.5:0.5;
          u64 p0=hud.passes, so0[2]={hud.objs[0],hud.objs[1]}, bk0=hud.back; int nk[2]={0,0}, nb=0;
          ACHECK(h3t_pass4(g,ob,14,ms,m0));
          int any=hud_s[0]!=1.0||hud_s[1]!=1.0;
          for(int i=0;i<14;i++){ float w=h3_value(bf(L[3]),(float)o_,an[i]<0?H3_TOP:an[i]>0?H3_BOTTOM:H3_CENTRE); int k=hk[i]; double s=k>=0?hud_s[k]:1.0;
              if(k>=0&&s!=1.0){ nk[k]++;
                  ACHECK(ms[i][0]==fbits((float)(s*bf(L[0])))&&ms[i][1]==fbits((float)(s*bf(L[1]))));
                  ACHECK(ms[i][2]==fbits((float)(bf(L[2])+(1.0-s)*PX[k]*bf(L[0])))&&ms[i][3]==fbits((float)(w+(1.0-s)*PY[k]*bf(L[1]))));
                  ACHECK(near(PX[k]*bf(ms[i][0])+bf(ms[i][2]),PX[k]*bf(L[0])+bf(L[2]),2e-3)&&near(PY[k]*bf(ms[i][1])+bf(ms[i][3]),PY[k]*bf(L[1])+w,2e-3)); }   /* the corner stays */
              else if(any||i==0){ if(i) nb++; for(int j=0;j<3;j++) ACHECK(ms[i][j]==L[j]); ACHECK(ms[i][3]==fbits(w)); }   /* the loop's MS.x / .y / .z, H3's MS.w */
              else { for(int j=0;j<3;j++) ACHECK(ms[i][j]==H3T_SNAN); ACHECK(ms[i][3]==fbits(w)); } }    /* both 1: only MS.w, as task 42 */
          ACHECK(hud.passes==p0+(any?1u:0u)&&hud.objs[0]==so0[0]+(u64)nk[0]&&hud.objs[1]==so0[1]+(u64)nk[1]&&hud.back==bk0+(any?(u64)(14-nk[0]-nk[1]):0u));
          (void)nb; }
      /* not taller than wide (the landscape phone, 1920x1350, 16:9): H3 as task 42, MS.x / .y / .z never written, whatever the scales */
      hud_s[0]=2.0; hud_s[1]=3.0;
      { static const int sc[3][2]={{2670,1878},{1920,1350},{1920,1080}};
        for(int s=0;s<3;s++){ H3SCREEN(sc[s][0],sc[s][1]); u64 p0=hud.passes; ACHECK(h3t_pass4(g,ob,14,ms,m0)&&hud.passes==p0);
            for(int i=1;i<14;i++) for(int j=0;j<3;j++) ACHECK(ms[i][j]==H3T_SNAN); } }
      /* the test aid h3 off and the bars on (H7 off, no 3D view): no scale either */
      H3SCREEN(2007,2853);
      { u64 p0=hud.passes; asp_off_h3=1; ACHECK(h3t_pass4(g,ob,14,ms,m0)); asp_off_h3=0; for(int i=1;i<14;i++) for(int j=0;j<4;j++) ACHECK(ms[i][j]==H3T_SNAN);
        int sh7=asp_off_h7; asp_off_h7=1; atomic_store(&last3d_plus1,0); ACHECK(h3t_pass4(g,ob,14,ms,m0)); for(int i=1;i<14;i++) for(int j=0;j<4;j++) ACHECK(ms[i][j]==H3T_SNAN);
        asp_off_h7=sh7; note_3d(kh_game_frames()); ACHECK(hud.passes==p0); }
      /* setScreenMode inside a scaled object (the case of task 42 review finding 2): its own mode is left as the exe made it; the loop's
       * mode back gets all four scaled words again */
      { hud_s[0]=1.25; hud_s[1]=1.5;
        u64 list=ga_calloc(1,0x40), out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10), wm=ga_calloc(1,0x10), sv=ga_calloc(1,0x20), tmp=ga_calloc(1,0x20); int k;
        H3Inner hin[16]; memcpy(hin,h3in,sizeof hin); int nhin=n_h3in; u64 hcut=h3in_cut;
        wrf(g,mode,1.0f); wrf(g,wm,0.8f); wrf(g,wm+12,0.1f); wr64(g,list+0x18,ob[1]);
        st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){out,mode},2,&k); u32 a4[4]; for(int j=0;j<4;j++) a4[j]=rd32(g,GX+0xa0+4ull*(u64)j);
        ACHECK(k&&h3_depth==1&&a4[1]==fbits((float)(1.5*bf(L[1])))&&a4[0]!=L[0]);
        st_call_ra(g,f_126500,GS_BASE+0x3bc4ccull,list,(u64[]){sv,wm},2,&k); ACHECK(k&&rd32(g,GX+0xa4)!=a4[1]&&rd32(g,GX+0xa4)!=L[1]);   /* its own mode */
        st_call_ra(g,f_126500,GS_BASE+0x3bc4f5ull,list,(u64[]){tmp,sv},2,&k); ACHECK(k); for(int j=0;j<4;j++) ACHECK(rd32(g,GX+0xa0+4ull*(u64)j)==a4[j]);   /* scaled again */
        ACHECK(st_call_ra(g,f_4ad270,DK_RA_NEXT,list,(u64[]){0},1,&k)==0&&k); st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){tmp,out},2,&k); ACHECK(k&&h3_depth==0);
        memcpy(h3in,hin,sizeof hin); n_h3in=nhin; h3in_cut=hcut;
        ga_free(list); ga_free(out); ga_free(mode); ga_free(wm); ga_free(sv); ga_free(tmp); }
      for(int i=0;i<14;i++) ga_free(ob[i]);
      enemy_scale=es; style_scale=ss; style_raise=sr;
      hud_s[0]=hs0[0]; hud_s[1]=hs0[1]; hud_set[0]=hset0[0]; hud_set[1]=hset0[1]; memcpy(&hud,&hsv,sizeof hsv); asp_mode=1; }
    #undef H3SCREEN
    for(int i=0;i<8;i++) ga_free(o[i]);
    memcpy(&h3,&sh,sizeof sh); asp_mode=sm; asp_h1_uncond=su; asp_off_h3=so; kh_aspect_dk=sdk; kh_aspect_m=sml; atomic_store(&last3d_plus1,s3);
    memcpy(hp(g,ID_TAB),tsave,0x204); wr32(g,GX_GUARD,guard); memcpy(hp(g,GX),gxsave,0x100);
}
/* ---- task 46: H7 through the real hook f_11a330 and the lifted body -----------------------------------------------------------------
 * A 2D batch as the game submits it: the vertices in a guest block, the current vertex buffer object (GX+0x5790, index [GX+0x578c] =
 * 0, [GX+0x59b8] = 0) holding the stride (+0x8) and the block (+0x48), then the hook f_11a330(GX, count, prim, 0) with the draw
 * routine's return address.  The lifted body runs: with [GX+0x4c0] = 1 its lazy begin is done and with [GX + 0x7c00 + 0x228 [GX+0x60]]
 * = 0 it takes its own no-device path (0x14011a3c8 -> 0x14011a447: three stores into the buffer object, no draw), so the test needs no
 * D3D12 device.  MagicScale is the exe's: the lifted f_126500 / f_1433a0 computes it for the screen and the mode {1,0,0,0}.  Every
 * vertex carries a pattern in its other 20 bytes and a guard word follows the block: `same` says that nothing but y changed. */
#define H7T_OX 1792.0f
#define H7T_OY 1840.0f
#define H7T_LAY  (GS_BASE+0x197033ull)
#define H7T_LAYB (GS_BASE+0x19701aull)
#define H7T_RECT (GS_BASE+0x16022cull)
#define H7T_PPP  (GS_BASE+0x4b3c85ull)
static u64 h7t_vb; static u32 h7t_stride;
static void h7t_quad(float (*v)[2],float x0,float x1,float y0,float y1){ v[0][0]=x0; v[0][1]=y0; v[1][0]=x1; v[1][1]=y0; v[2][0]=x0; v[2][1]=y1; v[3][0]=x1; v[3][1]=y1; }
/* the y the hook must store for a submitted GS y and a factor (the same single-precision steps as h7_pre) */
static float h7t_exp(float y,double s){ float st=y+H7T_OY, in=st-H7T_OY; return h7_map(in,s)+H7T_OY; }
static float h7t_raw(float y){ return y+H7T_OY; }
static float h7t_mov(float y,double dy){ float st=y+H7T_OY, in=st-H7T_OY; return (float)((double)in+dy)+H7T_OY; }   /* R3: moved by dy (the same steps as h7_pre) */
/* submit nv vertices; out[i] = the stored y of vertex i afterwards; *same = x, the other bytes and the guard word are unchanged */
static int h7t_submit(KCtx *g,u64 ra,u32 prim,const float (*v)[2],int nv,float *out,int *same){
    u64 vb=h7t_vb, B=GX+0x5790; u32 st=h7t_stride; int k, sm=1;
    for(int i=0;i<nv;i++){ u64 a=vb+(u64)i*st; for(u32 q=0;q<st;q+=4) wr32(g,a+q,0xa5000000u+(u32)i*256u+q); wrf(g,a,v[i][0]+H7T_OX); wrf(g,a+4,v[i][1]+H7T_OY); }
    wr32(g,vb+(u64)nv*st,0xdeadbeefu);
    wr32(g,GX+0x578c,0); wr64(g,GX+0x59b8,0); wr32(g,B+0x8,st); wr64(g,B+0x30,0x2000); wr32(g,B+0x38,0); wr64(g,B+0x40,vb); wr64(g,B+0x48,vb); wr32(g,B+0x50,(u32)nv);   /* as f_121ff0 leaves it */
    st_call_ra(g,f_11a330,ra,0,(u64[]){GX,(u64)(u32)nv,prim,0},4,&k);
    if(rd32(g,vb+(u64)nv*st)!=0xdeadbeefu) sm=0;
    for(int i=0;i<nv;i++){ u64 a=vb+(u64)i*st; out[i]=rdf(g,a+4); if(fbits(rdf(g,a))!=fbits(v[i][0]+H7T_OX)) sm=0; for(u32 q=8;q<st;q+=4) if(rd32(g,a+q)!=0xa5000000u+(u32)i*256u+q) sm=0; }
    *same=sm; return k;
}
/* the screen and the exe's own MagicScale for the 2D mode {1,0,0,0}, the GS offset of a menu frame */
static void h7t_screen(KCtx *g,float W,float H){
    wrf(g,GX+0x18,W); wrf(g,GX+0x1c,H); wrf(g,GX+0x10,W); wrf(g,GX+0x14,H); wrf(g,GX+0x20,1); wrf(g,GX+0x24,1); wrf(g,GX+0x28,1); wrf(g,GX+0x38,512); wrf(g,GX+0x3c,416); wrf(g,GX+0x48,512); wrf(g,GX+0x4c,416);
    u64 out=ga_calloc(1,0x20), mode=ga_calloc(1,0x10); int k; wrf(g,mode,1.0f); st_call(g,f_126500,(u64[]){out,mode},2,&k); ACHECK(k&&rd32(g,GX+0xb4)!=0);
    wr32(g,GX+0xb8,1);                      /* with [GX+0xb4] the condition under which f_1121d0 stages MagicScale from GX+0xa0 (0x140112386) */
    /* MS.x / MS.z as a run has them (the 512-wide GS frame over the band's 4:3 middle: task 42's first-pass line, MagicScale 2.8125
     * 2.59615 240 135 at 1920x1350 and 3.91113 3.61028 333.75 188.062 at 2670x1878; the selftest's GX has no widescreen setting and
     * f_1433a0 leaves MS.x = MS.y here).  H7 uses them only for the frame's left and right GS columns (-85.33 and 597.33) */
    { double bh=(double)H-2.0*kh_aspect_o(W,H); wrf(g,GX+0xa0,(float)(bh*4.0/3.0/512.0)); wrf(g,GX+0xa8,(float)(((double)W-bh*4.0/3.0)/2.0)); }
    wrf(g,GX+0x138,H7T_OX); wrf(g,GX+0x13c,H7T_OY); ga_free(out); ga_free(mode);
}
static int h7t_all(const float *out,const float (*v)[2],int nv,int raw){ for(int i=0;i<nv;i++) if(fbits(out[i])!=fbits(raw?h7t_raw(v[i][1]):0)) return 0; return 1; }
static void h7_selftest(KCtx *g){
    {
        u64 seq=ga_calloc(1,0x38), sprites=ga_calloc(1,39*44+13*20), parts=sprites+39*44;
        wr64(g,seq+8,sprites);
        const u32 ids[]={5,28,27,20,21,10};
        for(int i=0;i<6;i++) wr32(g,parts+(7+i)*20+16,ids[i]);
        for(int i=7;i<=12;i++){
            u32 count=i==8?8:4;
            wr64(g,seq+0x28,parts+i*20);
            ACHECK(title_credit_batch(g,GS_BASE+0x197033,seq,count)==(i==8 || i==10 || i==11));
            ACHECK(title_credit_batch(g,GS_BASE+0x19701a,seq,count)==(i==8 || i==10 || i==11));
            ACHECK(!title_credit_batch(g,GS_BASE+0x1977e3,seq,count));
            ACHECK(!title_credit_batch(g,GS_BASE+0x197033,seq,12));
        }
        ga_free(sprites); ga_free(seq);
    }
    { float bb[5][4]={{43,492,11,461},{-480,1015,461,682},{-480,1015,-213,11},{492,1015,11,461},{-480,43,11,461}};
      const u8 rect[5]={1,1,1,1,1};
      ACHECK(h7_wipe_caps(bb,rect,-85.333,597.333,317.65));
      ACHECK(!h7_wipe_caps(bb,rect,-85.333,597.333,52));
      bb[3][0]++; ACHECK(!h7_wipe_caps(bb,rect,-85.333,597.333,317.65)); }
    /* the arithmetic */
    ACHECK(h7_cover(0,416,52)==1.25&&h7_cover(-53,469,52)==1.0&&h7_cover(-52,468,52)==1.0&&h7_cover(-1000,2000,52)==1.0);
    ACHECK(h7_cover(-31,448,52)==260.0/239.0&&h7_cover(-30,449,52)==260.0/238.0&&h7_cover(-16,436,52)==260.0/224.0&&h7_cover(-51,461,52)==260.0/253.0&&h7_cover(-8,423,52)==260.0/215.0);
    ACHECK(h7_cover(-63,416,52)==1.25&&h7_cover(0,500,52)==1.25&&h7_cover(-51.5,468,52)==260.0/259.5);
    ACHECK(h7_map(0,1.25)==-52.0f&&h7_map(416,1.25)==468.0f&&h7_map(208,1.25)==208.0f&&h7_map(208,260.0/224.0)==208.0f&&h7_map(-16,260.0/224.0)==-52.0f);
    for(int i=0;i<5;i++){ static const double q[5][2]={{0,416},{-31,448},{-16,436},{-51,461},{-8,423}};        /* the stretched range covers the frame, and exactly at one edge */
        double sp=h7_cover(q[i][0],q[i][1],52), t=208+(q[i][0]-208)*sp, u=208+(q[i][1]-208)*sp; ACHECK(t<=-52+1e-9&&u>=468-1e-9&&(near(t,-52,1e-9)||near(u,468,1e-9))); }
    { float v[4][2], bb[4]; double sp=0;
      h7t_quad(v,-101,602,-16,436); ACHECK(h7_box(v,bb)==1&&bb[0]==-101&&bb[1]==602&&bb[2]==-16&&bb[3]==436);
      ACHECK(h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_FULL&&sp==260.0/224.0);
      ACHECK(h7_r1(bb,0,1.25,-85.333,597.333,&sp)==-1);                                               /* not a rectangle */
      h7t_quad(v,-88,8,-16,432); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_PIECE&&sp==260.0/224.0);
      h7t_quad(v,-84.5f,596.5f,1,415); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_FULL);   /* the tolerances: 1 GS row / column */
      h7t_quad(v,-84,596,1,415); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_PIECE);
      h7t_quad(v,-101,602,1.5f,436); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==-1);
      h7t_quad(v,-101,602,-16,414.5f); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==-1);
      h7t_quad(v,195,595,0,400); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==-1&&h7_r3(bb,1,-85.333,597.333)==-1);   /* the title's 400 x 400 picture: not R1's (not band-height); R3 moves it up (review finding 1) */
      h7t_quad(v,-86,598,-53,469); ACHECK(h7_box(v,bb)==1&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_ALREADY&&sp==1.0);
      h7t_quad(v,-86,598,-52,468); ACHECK(h7_r1(bb,h7_box(v,bb),1.25,-85.333,597.333,&sp)==H7K_ALREADY);
      h7t_quad(v,-86,598,-51.5f,468); ACHECK(h7_r1(bb,h7_box(v,bb),1.25,-85.333,597.333,&sp)==H7K_FULL&&sp==260.0/259.5);   /* half a row short: stretched by 0.2 % */
      h7t_quad(v,0,512,0,416); v[3][1]=420; ACHECK(h7_box(v,bb)==0);                                    /* skewed */
      v[0][0]=23.3f; v[0][1]=187.8f; v[1][0]=258.3f; v[1][1]=-47.25f; v[2][0]=258.4f; v[2][1]=422.8f; v[3][0]=493.4f; v[3][1]=187.7f; ACHECK(h7_box(v,bb)==0&&bb[2]==-47.25f&&bb[3]==422.8f);   /* rotated */
      h7t_quad(v,5,5,0,416); ACHECK(h7_box(v,bb)==0); h7t_quad(v,0,512,7,7); ACHECK(h7_box(v,bb)==0);     /* no area */
      h7t_quad(v,0,512,0,416); v[2][1]=__builtin_nanf(""); ACHECK(h7_box(v,bb)==0); }
    /* R2: the camp menu's backdrop as M logged it (camp route, present 21652): 8 full-width stripes of 64 rows from -51 and the 8
     * right-hand pieces of the same picture */
    { float bb[20][4]; u8 rc[20], on[20]; double sp=0; int n=0;
      for(int i=0;i<8;i++){ bb[n][0]=612; bb[n][1]=875+40*i; bb[n][2]=-51+64*i; bb[n][3]=13+64*i; rc[n++]=1; }
      for(int i=0;i<8;i++){ bb[n][0]=-101; bb[n][1]=612; bb[n][2]=-51+64*i; bb[n][3]=13+64*i; rc[n++]=1; }
      ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0);
      ACHECK(h7_r2(bb,rc,15,1.25,-85.333,597.333,&sp,on)==0);                                           /* the last stripe missing: ends at 397 */
      ACHECK(h7_r2(bb+9,rc+9,7,1.25,-85.333,597.333,&sp,on)==0);                                        /* the first missing: begins at 13 */
      { float sv=bb[11][3]; bb[11][3]=sv-1; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==0); bb[11][3]=sv-0.5f; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2); bb[11][3]=sv; }   /* a gap of a row / of half a row */
      rc[12]=0; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==0); rc[12]=1;                         /* one stripe is not a rectangle */
      { float sv=bb[10][1]; bb[10][1]=579; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==0); bb[10][1]=580; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2); bb[10][1]=sv; }   /* one stripe narrower than the frame (682.67 - 2) */
      for(int i=8;i<16;i++){ bb[i][0]-=700; bb[i][1]-=700; } ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0); for(int i=8;i<16;i++){ bb[i][0]+=700; bb[i][1]+=700; }   /* sliding in from the left */
      ACHECK(h7_r2(bb,rc,8,1.25,-85.333,597.333,&sp,on)==0);                                            /* only the right-hand pieces */
      ACHECK(h7_r2(bb+8,rc+8,1,1.25,-85.333,597.333,&sp,on)==0);                                        /* one rectangle is R1's case */
      bb[8][2]=-60; bb[15][3]=470; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==1&&sp==1.0);       /* a stack that covers the frame already */
      bb[15][3]=461; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0);
      /* review finding 4: the mask.  The 8 stripes and the 8 right-hand pieces (the same rows) are the picture; a header (the camp
       * menu's: y -25..34), a panel below the band, a rotated piece on a stripe's rows and a rectangle one row and a half off a
       * stripe's rows are not */
      bb[8][2]=-51; ACHECK(h7_r2(bb,rc,16,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0);              /* the batch as M logged it again */
      { int all=1; for(int i=0;i<16;i++) if(on[i]!=1) all=0; ACHECK(all); }
      bb[16][0]=100; bb[16][1]=400; bb[16][2]=-25; bb[16][3]=34; rc[16]=1;  bb[17][0]=157; bb[17][1]=687; bb[17][2]=333; bb[17][3]=547; rc[17]=1;   /* the panel: its top on a stripe's first row, its bottom not */
      bb[18][0]=620; bb[18][1]=700; bb[18][2]=13; bb[18][3]=77; rc[18]=0;   bb[19][0]=620; bb[19][1]=700; bb[19][2]=14.5f; bb[19][3]=77; rc[19]=1;
      ACHECK(h7_r2(bb,rc,20,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0&&on[16]==0&&on[17]==0&&on[18]==0&&on[19]==0&&on[0]==1&&on[7]==1&&on[8]==1&&on[15]==1);
      bb[19][2]=14; ACHECK(h7_r2(bb,rc,20,1.25,-85.333,597.333,&sp,on)==2&&on[19]==1); }                 /* within a row of a stripe's rows: a piece of the picture */
    /* review finding 6: stripes beyond the band belong to the stack.  ..., 333..397, 397..416, 416..461: the stack is -51..461 (factor
     * 260/253), not -51..416 (1.25); a stripe beyond a gap of a row is not part of it */
    { float bb[9][4]; u8 rc[9], on[9]; double sp=0; int n=0;
      for(int i=0;i<7;i++){ bb[n][0]=-101; bb[n][1]=612; bb[n][2]=-51+64*i; bb[n][3]=13+64*i; rc[n++]=1; }
      bb[n][0]=-101; bb[n][1]=612; bb[n][2]=397; bb[n][3]=416; rc[n++]=1;  bb[n][0]=-101; bb[n][1]=612; bb[n][2]=416; bb[n][3]=461; rc[n++]=1;
      ACHECK(h7_r2(bb,rc,9,1.25,-85.333,597.333,&sp,on)==2&&sp==260.0/253.0&&on[8]==1&&on[7]==1);
      ACHECK(h7_r2(bb,rc,8,1.25,-85.333,597.333,&sp,on)==2&&sp==1.25);                                   /* without the last stripe: the stack ends at the band's edge */
      bb[8][2]=417; ACHECK(h7_r2(bb,rc,9,1.25,-85.333,597.333,&sp,on)==2&&sp==1.25&&on[8]==0&&on[7]==1); }
    /* review finding 3: a rectangle one or two GS subpixels (1/16) off at one corner is a rectangle (the map-change shutter's left
     * piece as M logged it: box -328.5..55.5625, -11.5..428.5; its mirror piece 457..841, -12..428); three subpixels off is skewed */
    { float v[4][2], bb[4]; double sp=0;
      h7t_quad(v,-328.5f,55.5625f,-11.5f,428.5f); v[3][0]=55.5f; ACHECK(h7_box(v,bb)==1&&bb[1]==55.5625f&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_PIECE&&sp==260.0/219.5);
      v[3][0]=55.4375f; ACHECK(h7_box(v,bb)==1); v[3][0]=55.375f; ACHECK(h7_box(v,bb)==0);
      h7t_quad(v,457,841,-12,428); v[0][1]=-11.9375f; ACHECK(h7_box(v,bb)==1&&bb[2]==-12.0f&&h7_r1(bb,1,1.25,-85.333,597.333,&sp)==H7K_PIECE&&sp==260.0/220.0);
      v[0][1]=-11.8125f; ACHECK(h7_box(v,bb)==0); }
    /* review finding 1: R3, a picture cut by a band edge.  Exactly on ONE band edge (1/8 row), at least 312 rows tall, narrower than
     * the frame, a rectangle */
    { float v[4][2], bb[4];
      h7t_quad(v,195,595,0,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==-1);                     /* the title's picture: its top edge */
      h7t_quad(v,195,595,16,416); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==1);                     /* the mirror case: the bottom edge */
      h7t_quad(v,195,595,0.125f,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==-1); h7t_quad(v,195,595,0.1875f,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);
      h7t_quad(v,195,595,-0.125f,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==-1); h7t_quad(v,195,595,-0.1875f,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);
      h7t_quad(v,195,595,104,416.125f); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==1); h7t_quad(v,195,595,104,416.1875f); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);
      h7t_quad(v,195,595,104,415.8125f); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);
      h7t_quad(v,195,595,0,312); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==-1); h7t_quad(v,195,595,0,311.5f); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);   /* 3/4 of the band; shorter: a widget */
      h7t_quad(v,-84,596,0,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==-1); h7t_quad(v,-84.5f,596.5f,0,400); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);  /* as wide as the frame less 2 columns: a backdrop's piece */
      h7t_quad(v,195,595,0,416); ACHECK(h7_r3(bb,h7_box(v,bb),-85.333,597.333)==0);                      /* both edges: band-height, R1's */
      h7t_quad(v,195,595,0,400); ACHECK(h7_r3(bb,0,-85.333,597.333)==0); }                                /* not a rectangle */
    /* review finding 2: h7_batch_covers.  The map-change transition's batch as M logged it (camp route, present 3534): a centre
     * square and a rectangle below, above, right and left of it */
    { static const float w5[5][4]={{13,524,-16,495},{-581,1118,495,746},{-581,1118,-271,-16},{524,1118,-16,495},{-581,13,-16,495}};
      float bb[6][4]; u8 rc[6]={1,1,1,1,1,1}; memcpy(bb,w5,sizeof w5);
      ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==1&&h7_batch_covers(bb,rc,5,3,52,-85.333,597.333)==1&&h7_batch_covers(bb,rc,5,4,52,-85.333,597.333)==1);
      rc[2]=0; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==0); rc[2]=1;                         /* the piece above is not a rectangle */
      bb[2][3]=-17; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==0); bb[2][3]=-16.5f; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==1); bb[2][3]=-16;   /* a gap of a row / of half a row above */
      bb[2][2]=-51; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==0); bb[2][2]=-52; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==1); bb[2][2]=-271;      /* the piece above ends a row short of the frame's first row */
      bb[2][0]=15; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==0&&h7_batch_covers(bb,rc,5,3,52,-85.333,597.333)==1); bb[2][0]=-581;   /* the piece above does not span the centre's columns (it still spans the right piece's) */
      bb[5][0]=-581; bb[5][1]=1118; bb[5][2]=-400; bb[5][3]=-271;                                            /* two pieces above, one over the other */
      bb[2][2]=-40; bb[5][3]=-40; ACHECK(h7_batch_covers(bb,rc,5,0,52,-85.333,597.333)==0&&h7_batch_covers(bb,rc,6,0,52,-85.333,597.333)==1); bb[2][2]=-271;
      /* a rectangle that contains the R1 rectangle (another layer: a picture over a larger black rectangle) is not a tile above or below it */
      bb[0][0]=-100; bb[0][1]=700; bb[0][2]=-16; bb[0][3]=432; bb[1][0]=-100; bb[1][1]=700; bb[1][2]=-60; bb[1][3]=480;
      ACHECK(h7_batch_covers(bb,rc,2,0,52,-85.333,597.333)==0);
      bb[0][2]=-16; bb[0][3]=480; bb[1][2]=-60; bb[1][3]=200; ACHECK(h7_batch_covers(bb,rc,2,0,52,-85.333,597.333)==0);     /* overlaps its top part only */
      bb[0][2]=-60; bb[0][3]=432; bb[1][2]=200; bb[1][3]=480; ACHECK(h7_batch_covers(bb,rc,2,0,52,-85.333,597.333)==0);     /* overlaps its bottom part only */
      bb[0][2]=-16; bb[0][3]=480; bb[1][2]=-60; bb[1][3]=-16; ACHECK(h7_batch_covers(bb,rc,2,0,52,-85.333,597.333)==1); }   /* a tile above, the rectangle itself passes the frame's last row */
    /* review finding 7: the run's start line says on which frames H1 acts */
    { char bn[512]; int m0=asp_mode, u0=asp_h1_uncond, o0=asp_off_h7, a0=kh_aspect_m, l0=kh_aspect_log; kh_aspect_m=kh_aspect_log=0;
      asp_mode=1; asp_h1_uncond=0; asp_off_h7=0; asp_banner(bn,sizeof bn);
      ACHECK(!strcmp(bn,"kh: aspect: KHNMC_ASPECT=fill (H1: no 16:9 bars on a screen narrower than 16:9, on every frame: task 46, H7)"));
      asp_off_h7=1; asp_banner(bn,sizeof bn); ACHECK(strstr(bn,", only on frames with the 3D view: TEST AID KHNMC_ASPECT_TEST_OFF=h7")!=NULL&&strstr(bn,"on every frame")==NULL);
      asp_h1_uncond=1; asp_banner(bn,sizeof bn); ACHECK(strstr(bn,"UNCONDITIONAL: every frame, KHNMC_ASPECT_TEST_H1 test aid)")!=NULL&&strstr(bn,"3D view")==NULL);
      asp_mode=0; asp_h1_uncond=0; asp_off_h7=0; kh_aspect_log=1; asp_banner(bn,sizeof bn); ACHECK(!strcmp(bn,"kh: aspect: KHNMC_ASPECT=pc; KHNMC_ASPECT_LOG=1: size records on"));
      asp_mode=m0; asp_h1_uncond=u0; asp_off_h7=o0; kh_aspect_m=a0; kh_aspect_log=l0; }
    ACHECK(h7_site_index(H7T_LAY)>=0&&h7_site_index(H7T_LAYB)>=0&&h7_site_index(H7T_RECT)>=0&&h7_site_index(H7T_PPP)<0&&h7_site_index(GS_BASE+0x16a853ull)<0&&h7_site_index(0)<0);
    /* through the hook and the lifted body */
    u8 gxsave[0x148]; memcpy(gxsave,hp(g,GX),0x148); u8 s4b0[0x18]; memcpy(s4b0,hp(g,GX+0x4b0),0x18); u8 svb[0x60]; memcpy(svb,hp(g,GX+0x578c),0x60);
    u64 s59b8=rd64(g,GX+0x59b8), s7c00=rd64(g,GX+0x7c00); u32 guard=rd32(g,GX_GUARD); wr32(g,GX_GUARD,0x80000000u);
    int sm=asp_mode, su=asp_h1_uncond, so3=asp_off_h3, sdk=kh_aspect_dk, sml=kh_aspect_m, sl=kh_aspect_log; u64 s3=atomic_load(&last3d_plus1), sn1=h7_h1_no3d, sn3=h7_h3_no3d;
    H7Stat sh7; memcpy(&sh7,&h7,sizeof sh7); H3Stat sh3; memcpy(&sh3,&h3,sizeof sh3); static H7Row srow[H7_ROWS]; memcpy(srow,h7r,sizeof srow); int snr=n_h7r; u64 scut=h7r_cut;
    kh_aspect_m=0; kh_aspect_log=0; asp_h1_uncond=0; asp_off_h3=0; asp_off_h7=0; asp_mode=1; h3.warned=1; h3.warned_inner=1;
    wr8(g,GX+0x4c0,1); wr64(g,GX+0x7c00,0); wr32(g,GX+0x60,0);
    h7t_vb=ga_calloc(1,0x2000); h7t_stride=H7_STRIDE;
    float v[80][2], out[80]; int k, same;
    #define H7SUB(ra_,prim_,nv_) (k=h7t_submit(g,(ra_),(prim_),(const float (*)[2])v,(nv_),out,&same))
    static const float scr[2][2]={{1920,1350},{2670,1878}};
    for(int sc=0;sc<2;sc++){ float W=scr[sc][0], H=scr[sc][1]; h7t_screen(g,W,H);
        double r=kh_aspect_r(W,H), b=208.0*(r-1.0), o=kh_aspect_o(W,H), msy=rdf(g,GX+0xa4), msw=rdf(g,GX+0xac);
        ACHECK(near(msw,o,1e-3)&&near(416.0*msy,H-2.0*o,1e-2));                                          /* the exe's mode is the centred band */
        ACHECK(near(-(double)rdf(g,GX+0xa8)/rdf(g,GX+0xa0),-85.3333,1e-3)&&near(((double)W-rdf(g,GX+0xa8))/rdf(g,GX+0xa0),597.3333,1e-3));
        /* R1: exactly the band -> the frame's first and last row */
        { double xl=-(double)rdf(g,GX+0xa8)/rdf(g,GX+0xa0), xr=((double)W-rdf(g,GX+0xa8))/rdf(g,GX+0xa0); u64 f0=h7.kind[H7K_FULL], e0=h7.batch_ext;
          h7t_quad(v,(float)xl,(float)xr,0,416); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7.kind[H7K_FULL]==f0+1&&h7.batch_ext==e0+1);
          for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],r)));
          double row0=((double)out[0]-H7T_OY)*msy+msw, row1=((double)out[2]-H7T_OY)*msy+msw; ACHECK(near(row0,0,0.01)&&near(row1,H,0.01));   /* screen rows 0 and H */
          if(sc==0) ACHECK(out[0]==H7T_OY-52.0f&&out[1]==H7T_OY-52.0f&&out[2]==H7T_OY+468.0f&&out[3]==H7T_OY+468.0f); }
        /* R1: the pause screen's dim rectangle (M: -101..602, -16..436), from both paths of f_1969a0 and from f_15fee0 (primitive 6) */
        { static const u64 ras[3]={H7T_LAY,H7T_LAYB,H7T_RECT}; static const u32 pr[3]={8,8,6}; double sp=h7_cover(-16,436,b);
          for(int c=0;c<3;c++){ u64 f0=h7.kind[H7K_FULL]; h7t_quad(v,-101,602,-16,436); H7SUB(ras[c],pr[c],4); ACHECK(k&&same&&h7.kind[H7K_FULL]==f0+1);
              for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp)));
              ACHECK(((double)out[0]-H7T_OY)*msy+msw<=0.01&&((double)out[2]-H7T_OY)*msy+msw>=H-0.01); } }
        /* R1: a full-height side strip (the camp menu: -88..8) is a piece */
        { u64 p0=h7.kind[H7K_PIECE]; double sp=h7_cover(-16,432,b); h7t_quad(v,-88,8,-16,432); H7SUB(H7T_LAYB,8,4); ACHECK(k&&same&&h7.kind[H7K_PIECE]==p0+1);
          for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); }
        /* widgets: not one bit changes */
        { u64 s0=h7.small; h7t_quad(v,116,401,193,223); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.small==s0+1);
          h7t_quad(v,195,595,0,300); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1));       /* touches the band's top edge only, 300 rows tall */
          h7t_quad(v,195,595,0.5f,400); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1));    /* half a row off the band's edge */
          h7t_quad(v,157,687,314,547); H7SUB(H7T_LAYB,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1));    /* reaches below the band only */
          h7t_quad(v,-101,612,-51,13); H7SUB(H7T_LAYB,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.small==s0+5); }   /* one stripe alone */
        /* a batch of three quads: only the band-height rectangle in the middle changes */
        { double sp=h7_cover(-16,432,b); h7t_quad(v,116,401,193,223); h7t_quad(v+4,-100,1084,-16,432); h7t_quad(v+8,178,337,225,255); u64 f0=h7.kind[H7K_FULL], s0=h7.small, e0=h7.batch_ext;
          H7SUB(H7T_LAYB,8,12); ACHECK(k&&same&&h7.kind[H7K_FULL]==f0+1&&h7.small==s0+2&&h7.batch_ext==e0+1&&h7t_all(out,(const float (*)[2])v,4,1)&&h7t_all(out+8,(const float (*)[2])(v+8),4,1));
          for(int i=4;i<8;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); }
        /* R3 (review finding 1): the title's picture (195..595, 0..400) is moved up by b: its cut edge is the frame's first row, its
         * height, its x and the rest of its vertices unchanged; the mirror case moves down to the frame's last row; from a routine
         * that is not in the table and with the test aid: not one bit */
        { u64 e0=h7.kind[H7K_EDGE], x0=h7.batch_ext, s0=h7.small, t0=h7.kind[H7K_SITE];
          h7t_quad(v,195,595,0,400); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7.kind[H7K_EDGE]==e0+1&&h7.batch_ext==x0+1&&h7.small==s0);
          for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_mov(v[i][1],-b)));
          ACHECK(near(((double)out[0]-H7T_OY)*msy+msw,0,0.01)&&near((double)out[2]-(double)out[0],400,1e-3)&&fbits(out[0])==fbits(out[1])&&fbits(out[2])==fbits(out[3]));
          if(sc==0) ACHECK(out[0]==H7T_OY-52.0f&&out[2]==H7T_OY+348.0f);
          h7t_quad(v,195,595,16,416); H7SUB(H7T_LAYB,8,4); ACHECK(k&&same&&h7.kind[H7K_EDGE]==e0+2);
          for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_mov(v[i][1],b)));
          ACHECK(near(((double)out[2]-H7T_OY)*msy+msw,H,0.01)&&near((double)out[2]-(double)out[0],400,1e-3));
          h7t_quad(v,195,595,0,400); H7SUB(H7T_PPP,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_EDGE]==e0+2&&h7.kind[H7K_SITE]==t0+1);
          asp_off_h7=1; H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_EDGE]==e0+2); asp_off_h7=0;
          /* the title's batch shapes together: the flat backdrop (R1), the picture (R3), the logo (left) */
          { double sp=h7_cover(-31,448,b); u64 f0=h7.kind[H7K_FULL]; x0=h7.batch_ext; h7t_quad(v,-149,660,-31,448); h7t_quad(v+4,195,595,0,400); h7t_quad(v+8,11,206,52,275);
            H7SUB(H7T_LAY,8,12); ACHECK(k&&same&&h7.kind[H7K_FULL]==f0+1&&h7.kind[H7K_EDGE]==e0+3&&h7.batch_ext==x0+1&&h7t_all(out+8,(const float (*)[2])(v+8),4,1));
            for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))&&fbits(out[4+i])==fbits(h7t_mov(v[4+i][1],-b))); }
          /* in a stripe-stack batch (R2) nothing is moved: the same rectangle among the camp menu's 16 quads is left where it is */
          { int n=0; double sp=h7_cover(-51,461,b); u64 o0=h7.stack_other, k0=h7.kind[H7K_STACK];
            for(int i=0;i<8;i++){ h7t_quad(v+4*n,612,875+40*i,-51+64*i,13+64*i); n++; } for(int i=0;i<8;i++){ h7t_quad(v+4*n,-101,612,-51+64*i,13+64*i); n++; }
            h7t_quad(v+64,195,595,0,400); H7SUB(H7T_LAYB,8,68); ACHECK(k&&same&&h7.kind[H7K_STACK]==k0+16&&h7.stack_other==o0+1&&h7.kind[H7K_EDGE]==e0+3&&h7t_all(out+64,(const float (*)[2])(v+64),4,1));
            for(int i=0;i<64;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); } }
        /* review finding 2: the map-change transition's batch (camp route, present 3534: centre, below, above, right, left).  Its
         * band-height centre, right and left pieces are covered by the batch's own pieces above and below: not one bit changes */
        { static const float w5[5][4]={{13,524,-16,495},{-581,1118,495,746},{-581,1118,-271,-16},{524,1118,-16,495},{-581,13,-16,495}};
          u64 c0=h7.kind[H7K_BATCH], p0=h7.kind[H7K_PIECE], f0=h7.kind[H7K_FULL], x0=h7.batch_ext, s0=h7.small, h0=h7.kind[H7K_SHAPE];
          for(int q=0;q<5;q++) h7t_quad(v+4*q,w5[q][0],w5[q][1],w5[q][2],w5[q][3]);
          H7SUB(H7T_LAY,8,20); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,20,1)&&h7.kind[H7K_BATCH]==c0+3&&h7.kind[H7K_PIECE]==p0&&h7.kind[H7K_FULL]==f0&&h7.batch_ext==x0&&h7.small==s0+2);
          /* five frames later (present 3539) no piece spans the band: left, listed as a batch that spans it */
          { static const float w6[5][4]={{82,449,47,414},{-346,877,414,595},{-346,877,-137,47},{449,877,47,414},{-346,82,47,414}};
            for(int q=0;q<5;q++) h7t_quad(v+4*q,w6[q][0],w6[q][1],w6[q][2],w6[q][3]);
            H7SUB(H7T_LAY,8,20); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,20,1)&&h7.kind[H7K_BATCH]==c0+3&&h7.kind[H7K_SHAPE]==h0+1); }
          h7t_screen(g,2007,2853);
          H7SUB(H7T_LAY,8,20); ACHECK(k&&same);
          for(int i=0;i<20;i++){
              double margin=kh_aspect_b(2007,2853);
              float y=(i==6||i==7)?(float)(416+margin):(i==8||i==9)?(float)-margin:v[i][1];
              ACHECK(near(out[i],H7T_OY+y,0.001));
          }
          h7t_screen(g,W,H);
          /* the camp menu's two side strips in one batch (present 21652) and the save menu's rectangle with its side piece (present
           * 20562) are still stretched: no piece of their batch lies above or below them */
          { double sp=h7_cover(-16,432,b); p0=h7.kind[H7K_PIECE]; f0=h7.kind[H7K_FULL]; h7t_quad(v,-88,8,-16,432); h7t_quad(v+4,504,600,-16,432);
            H7SUB(H7T_LAYB,8,8); ACHECK(k&&same&&h7.kind[H7K_PIECE]==p0+2&&h7.kind[H7K_BATCH]==c0+3); for(int i=0;i<8;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp)));
            h7t_quad(v,-100,1084,-16,432); h7t_quad(v+4,-101,168,-16,432);
            H7SUB(H7T_LAYB,8,8); ACHECK(k&&same&&h7.kind[H7K_PIECE]==p0+3&&h7.kind[H7K_FULL]==f0+1&&h7.kind[H7K_BATCH]==c0+3); for(int i=0;i<8;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); }
          /* a band-height rectangle drawn over a larger rectangle of the same batch (another layer, not a tile): still stretched */
          { double sp=h7_cover(-16,432,b); u64 a0=h7.kind[H7K_ALREADY]; f0=h7.kind[H7K_FULL]; h7t_quad(v,-100,700,-60,480); h7t_quad(v+4,-100,700,-16,432);
            H7SUB(H7T_LAY,8,8); ACHECK(k&&same&&h7.kind[H7K_FULL]==f0+1&&h7.kind[H7K_ALREADY]==a0+1&&h7.kind[H7K_BATCH]==c0+3&&h7t_all(out,(const float (*)[2])v,4,1));
            for(int i=4;i<8;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); } }
        /* review finding 3: the shutter's left piece with one corner a GS subpixel off is stretched like its mirror piece */
        { u64 p0=h7.kind[H7K_PIECE]; double sp=h7_cover(-11.5,428.5,b); h7t_quad(v,-328.5f,55.5625f,-11.5f,428.5f); v[3][0]=55.5f;
          H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7.kind[H7K_PIECE]==p0+1); for(int i=0;i<4;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp)));
          ACHECK(((double)out[0]-H7T_OY)*msy+msw<=0.01&&((double)out[2]-H7T_OY)*msy+msw>=H-0.01); }
        /* review finding 4: a header, a panel and a rotated piece in the camp menu's stripe batch stay exactly where they are; the 16
         * quads of the picture are stretched as before */
        { int n=0; double sp=h7_cover(-51,461,b); u64 k0=h7.kind[H7K_STACK], sb0=h7.stack_batches, s0=h7.small, o0=h7.stack_other;
          for(int i=0;i<8;i++){ h7t_quad(v+4*n,612,875+40*i,-51+64*i,13+64*i); n++; } for(int i=0;i<8;i++){ h7t_quad(v+4*n,-101,612,-51+64*i,13+64*i); n++; }
          h7t_quad(v+64,100,400,-25,34); h7t_quad(v+68,157,687,314,547);
          v[72][0]=660; v[72][1]=13; v[73][0]=700; v[73][1]=45; v[74][0]=620; v[74][1]=45; v[75][0]=660; v[75][1]=77;
          H7SUB(H7T_LAYB,8,76); ACHECK(k&&same&&h7.kind[H7K_STACK]==k0+16&&h7.stack_batches==sb0+1&&h7.small==s0+3&&h7.stack_other==o0+3);
          for(int i=0;i<64;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp)));
          ACHECK(h7t_all(out+64,(const float (*)[2])(v+64),12,1)); }
        /* already covering the frame (H4's fade rectangle), a rotated square, 5 vertices, a draw routine that is not in the table: left */
        { u64 a0=h7.kind[H7K_ALREADY], h0=h7.kind[H7K_SHAPE], t0=h7.kind[H7K_SITE], c0=h7.count;
          h7t_quad(v,-86,598,-54,470); H7SUB(H7T_RECT,6,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_ALREADY]==a0+1);
          v[0][0]=23.3f; v[0][1]=187.8f; v[1][0]=258.3f; v[1][1]=-47.25f; v[2][0]=258.4f; v[2][1]=422.8f; v[3][0]=493.4f; v[3][1]=187.7f;
          H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_SHAPE]==h0+1);
          h7t_quad(v,-101,602,-16,436); v[4][0]=0; v[4][1]=0; H7SUB(H7T_LAY,5,5); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,5,1)&&h7.kind[H7K_SHAPE]==h0+2);   /* not a quad list */
          h7t_quad(v,-101,602,-16,436); H7SUB(H7T_PPP,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_SITE]==t0+1);
          H7SUB(GS_BASE+0x16a853ull,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.kind[H7K_SITE]==t0+2);
          h7t_quad(v,-101,602,-16,436); H7SUB(H7T_LAY,8,3); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,3,1)&&h7.count==c0+1); }
        /* R2: the camp menu's 16 quads -> all 64 vertices with the stack's factor; without its last stripe: nothing */
        { int n=0; double sp=h7_cover(-51,461,b); u64 k0=h7.kind[H7K_STACK], sb0=h7.stack_batches, s0=h7.small, h0=h7.kind[H7K_SHAPE];
          for(int i=0;i<8;i++){ h7t_quad(v+4*n,612,875+40*i,-51+64*i,13+64*i); n++; } for(int i=0;i<8;i++){ h7t_quad(v+4*n,-101,612,-51+64*i,13+64*i); n++; }
          H7SUB(H7T_LAYB,8,64); ACHECK(k&&same&&h7.kind[H7K_STACK]==k0+16&&h7.stack_batches==sb0+1&&h7.small==s0);
          for(int i=0;i<64;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp)));
          ACHECK(((double)out[32]-H7T_OY)*msy+msw<=0.01&&((double)out[62]-H7T_OY)*msy+msw>=H-0.01&&fbits(out[34])==fbits(out[36]));   /* covers the frame; the stripes still meet */
          H7SUB(H7T_LAYB,8,60); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,60,1)&&h7.kind[H7K_STACK]==k0+16&&h7.kind[H7K_SHAPE]==h0+1);
          H7SUB(H7T_PPP,8,64); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,64,1)&&h7.kind[H7K_STACK]==k0+16);             /* the same stack from another routine */
          for(int i=0;i<64;i++) v[i][0]-=700; H7SUB(H7T_LAY,8,64); ACHECK(k&&same&&h7.kind[H7K_STACK]==k0+32&&h7.stack_batches==sb0+2);   /* the stack while it slides in */
          for(int i=0;i<64;i++) ACHECK(fbits(out[i])==fbits(h7t_exp(v[i][1],sp))); }
        /* the mode: an anchored HUD object's MagicScale (MS.w = 0 / 2 o), a zoomed mode, the GX+0x90 row, another vertex: left */
        { u64 m0=h7.modeother, n0=h7.mode90, f0=h7.fmt; float w0=rdf(g,GX+0xac), y0=rdf(g,GX+0xa4); h7t_quad(v,-101,602,-16,436);
          wrf(g,GX+0xac,0); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.modeother==m0+1);
          wrf(g,GX+0xac,(float)(2.0*o)); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.modeother==m0+2);
          wrf(g,GX+0xac,w0); wrf(g,GX+0xa4,y0*0.8f); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.modeother==m0+3); wrf(g,GX+0xa4,y0);
          wr32(g,GX+0xb4,0); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.mode90==n0+1); wr32(g,GX+0xb4,1);
          wr32(g,GX+0xb8,0); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.mode90==n0+2); wr32(g,GX+0xb8,1);
          h7t_stride=60; H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.fmt==f0+1); h7t_stride=H7_STRIDE;
          H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&fbits(out[0])==fbits(h7t_exp(-16,h7_cover(-16,436,b)))); }                           /* and with the mode back: stretched */
        /* the lazy begin pending (h7_pre alone: the body would call the device), an unreadable block */
        { u64 l0=h7.lazy, nb0=h7.nobuf, B=GX+0x5790, svrsp=KH_RSP(g), svrdx=KH_RDX(g), svr8=KH_R8(g); h7t_quad(v,-101,602,-16,436); H7SUB(H7T_LAY,8,4);
          for(int i=0;i<4;i++) wrf(g,h7t_vb+(u64)i*H7_STRIDE+4,h7t_raw(v[i][1]));
          u64 sp=(svrsp-0x400)&~15ull; wr64(g,sp,H7T_LAY); KH_RSP(g)=sp; KH_RDX(g)=4; KH_R8(g)=8;
          wr8(g,GX+0x4c0,0); wr64(g,GX+0x4b0,h7t_vb); ACHECK(h7_pre(g)==-1&&h7.lazy==l0+1&&rdf(g,h7t_vb+4)==h7t_raw(-16)); wr8(g,GX+0x4c0,1); wr64(g,GX+0x4b0,0);
          wr32(g,B+0x38,0);                                                                 /* the lifted body's no-device path left [B+0x38] as it was: 0 */
          wr64(g,B+0x48,0); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb0+1); wr64(g,B+0x48,h7t_vb+0x1c); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb0+2);   /* not base + offset */
          wr64(g,B+0x48,h7t_vb); wr32(g,B+0x50,3); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb0+3); wr32(g,B+0x50,4);                         /* more vertices than asked for */
          wr64(g,B+0x30,0x6f); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb0+4); wr64(g,B+0x30,0x70);                                          /* beyond the capacity; exactly the capacity is fine */
          wr64(g,B+0x40,0); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb0+5&&rdf(g,h7t_vb+4)==h7t_raw(-16)); wr64(g,B+0x40,h7t_vb);
          ACHECK(h7_pre(g)==H7K_FULL&&rdf(g,h7t_vb+4)==h7t_exp(-16,h7_cover(-16,436,b)));
          /* a block that is not guest-arena memory (a real backend's mapped buffer) is used all the same: here a host array */
          { static float hb[4*7+1]; u64 ga=(u64)(uintptr_t)hb-(u64)((uintptr_t)hp(g,GX)-(uintptr_t)GX); float q[4][2]; h7t_quad(q,-101,602,-16,436);
            for(int i=0;i<4;i++){ hb[i*7]=q[i][0]+H7T_OX; hb[i*7+1]=q[i][1]+H7T_OY; } hb[28]=1234.5f;
            wr64(g,B+0x40,ga); wr64(g,B+0x48,ga); wr64(g,B+0x30,0x70); wr32(g,B+0x38,0); wr32(g,B+0x50,4);
            if(!GA_OK(ga)){ ACHECK(h7_pre(g)==H7K_FULL&&hb[1]==h7t_exp(-16,h7_cover(-16,436,b))&&hb[15]==h7t_exp(436,h7_cover(-16,436,b))&&hb[0]==-101+H7T_OX&&hb[28]==1234.5f); }
            else kh_selftest_not_run("aspect","H7 on a vertex block outside the guest arena (the selftest's static array is inside it)",1);
            wr64(g,B+0x40,h7t_vb); wr64(g,B+0x48,h7t_vb); wr64(g,B+0x30,0x2000); }
          /* review finding 6: a block at a non-zero write offset of the buffer ([B+0x38] = 0x70, the block = base + 0x70: a second
           * batch in the same buffer) is used; the quad at the buffer's base is not touched; the capacity is tested with the offset */
          { for(int i=0;i<4;i++){ u64 a0=h7t_vb+(u64)i*H7_STRIDE, a1=h7t_vb+0x70+(u64)i*H7_STRIDE; wrf(g,a0,v[i][0]+H7T_OX); wrf(g,a0+4,h7t_raw(v[i][1])); wrf(g,a1,v[i][0]+H7T_OX); wrf(g,a1+4,h7t_raw(v[i][1])); }
            u64 nb1=h7.nobuf; wr32(g,B+0x38,0x70); wr64(g,B+0x48,h7t_vb+0x70); wr64(g,B+0x30,0x2000); wr32(g,B+0x50,4);
            ACHECK(h7_pre(g)==H7K_FULL&&h7.nobuf==nb1&&rdf(g,h7t_vb+0x70+4)==h7t_exp(-16,h7_cover(-16,436,b))&&rdf(g,h7t_vb+0x70+3*H7_STRIDE+4)==h7t_exp(436,h7_cover(-16,436,b))
                   &&rdf(g,h7t_vb+4)==h7t_raw(-16)&&rdf(g,h7t_vb+3*H7_STRIDE+4)==h7t_raw(436));
            for(int i=0;i<4;i++) wrf(g,h7t_vb+0x70+(u64)i*H7_STRIDE+4,h7t_raw(v[i][1]));
            wr64(g,B+0x30,0xdf); ACHECK(h7_pre(g)==-1&&h7.nobuf==nb1+1&&rdf(g,h7t_vb+0x70+4)==h7t_raw(-16)); wr64(g,B+0x30,0xe0); ACHECK(h7_pre(g)==H7K_FULL&&h7.nobuf==nb1+1);   /* offset + 4 vertices = 0xe0 */
            wr32(g,B+0x38,0); wr64(g,B+0x48,h7t_vb); wr64(g,B+0x30,0x2000); }
          KH_RSP(g)=svrsp; KH_RDX(g)=svrdx; KH_R8(g)=svr8; }
        /* test aid off, pc: not one bit, pc counts nothing */
        { u64 o0=h7.off, c0=h7.calls; h7t_quad(v,-101,602,-16,436);
          asp_off_h7=1; H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.off==o0+1); asp_off_h7=0;
          asp_mode=0; c0=h7.calls; H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.calls==c0); asp_mode=1; } }
    /* 16:9 or wider: nothing is read or written (an unreadable block is not even looked at) */
    { static const float same169[4][2]={{1920,1080},{2560,1440},{3840,2160},{2560,1080}};
      for(int i=0;i<4;i++){ h7t_screen(g,same169[i][0],same169[i][1]); u64 i0=h7.identity, nb0=h7.nobuf, e0=h7.batch_ext, B=GX+0x5790;
          h7t_quad(v,-200,700,0,416); H7SUB(H7T_LAY,8,4); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,4,1)&&h7.identity==i0+1&&h7.batch_ext==e0);
          int n=0; for(int q=0;q<8;q++){ h7t_quad(v+4*n,-200,700,-51+64*q,13+64*q); n++; } H7SUB(H7T_LAYB,8,32); ACHECK(k&&same&&h7t_all(out,(const float (*)[2])v,32,1)&&h7.identity==i0+2);
          u64 svrsp=KH_RSP(g), svrdx=KH_RDX(g), sp=(svrsp-0x400)&~15ull; wr64(g,sp,H7T_LAY); KH_RSP(g)=sp; KH_RDX(g)=4; wr64(g,B+0x48,0);
          ACHECK(h7_pre(g)==-1&&h7.identity==i0+3&&h7.nobuf==nb0); KH_RSP(g)=svrsp; KH_RDX(g)=svrdx; } }
    /* (a) no bars on any frame: H1 without the 3D view rewrites the fit, with the test aid it keeps the exe's bars */
    { float x,y; u64 n0=h7_h1_no3d; atomic_store(&last3d_plus1,0);
      h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1350.0f&&h7_h1_no3d==n0+1);
      h1_call(g,2670,1878,1,1,1,&x,&y); ACHECK(x==2670.0f&&y==1878.0f&&h7_h1_no3d==n0+2);
      h1_call(g,1920,1080,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f&&h7_h1_no3d==n0+2);            /* 16:9: not narrow, nothing rewritten */
      h1_call(g,2560,1080,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f&&h7_h1_no3d==n0+2);
      asp_off_h7=1; h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f&&h7_h1_no3d==n0+2); asp_off_h7=0;
      note_3d(kh_game_frames()); h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1350.0f&&h7_h1_no3d==n0+2);   /* with the 3D view: H1 as before, not counted here */
      asp_mode=0; atomic_store(&last3d_plus1,0); h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f&&h7_h1_no3d==n0+2); asp_mode=1; }
    /* (a) a dk pass on a frame without the 3D view is anchored (a top class first, then a bottom class); with the test aid: the bars */
    { u8 tsave[0x204]; memcpy(tsave,hp(g,ID_TAB),0x204); if(rd32(g,ID_TAB+0x200)==0){ for(int i=0;i<64;i++) wr64(g,ID_TAB+8ull*i,~0ull); wr32(g,ID_TAB+0x200,0x80000000u); }
      int sdk2=kh_aspect_dk; kh_aspect_dk=1; u64 o[2]={h3t_obj(g,GS_BASE+0x5b2400ull,7),h3t_obj(g,GS_BASE+0x5b2078ull,3)}; float ws[2], we; u64 n0=h7_h3_no3d, b0=h3.bars;
      h7t_screen(g,1920,1350); atomic_store(&last3d_plus1,0);
      ACHECK(h3t_pass(g,o,2,ws,&we)&&ws[0]==0.0f&&ws[1]==270.0f&&h7_h3_no3d==n0+1&&h3.bars==b0);
      asp_off_h7=1; ACHECK(h3t_pass(g,o,2,ws,&we)&&ws[0]==135.0f&&fbits(ws[1])==H3T_SNAN&&h7_h3_no3d==n0+1&&h3.bars==b0+1); asp_off_h7=0;
      h7t_screen(g,1920,1080); ACHECK(h3t_pass(g,o,2,ws,&we)&&fbits(ws[1])==H3T_SNAN&&h7_h3_no3d==n0+1);                     /* 16:9: nothing written */
      kh_aspect_dk=sdk2; ga_free(o[0]); ga_free(o[1]); memcpy(hp(g,ID_TAB),tsave,0x204); }
    /* ---- task 53 item 2: the title's portrait layout through the hooks (H3a opens a dk pass whose head is the title's layout object,
     * then f_11a330 with the layout draw's return address) ---- */
    { /* the parts by their GS boxes as M logged them (dl-title-p.tsv: presents 405..812 of the field route at 2007x2853) */
      static const struct { float b[4]; int e; } tb[]={{{195,595,0,400},TT_PIC},{{195,595,0.5f,399.5f},TT_PIC},{{344,597,14,36},TT_SET},
          {{-47,522,305,330},TT_MENU},{{-71,-47,305,330},TT_MENU},{{198,326,306,330},TT_MENU},{{-83,-42,295,336},TT_MENU},{{-47,389,388,413},TT_MENU},{{-39,-39,427,447},TT_MENU},{{535,598,378,402},TT_MENU},
          {{10,205,37,69},TT_LOGO},{{11,206,52,275},TT_LOGO},{{-77,298,83,194},TT_LOGO},{{87,133,78,113},TT_LOGO},{{-5,247,154,218},TT_LOGO},{{-68,92,253,265},TT_LOGO},
          {{-65,105,269,284},TT_LOGO},{{-124,-52,266,268},TT_LOGO},{{-413,-243,278,293},TT_LOGO},{{-373,596,-4,283},TT_LOGO},{{-397,621,-11,290},TT_LOGO},{{-114,336,73,206},TT_LOGO},
          {{-149,660,-31,448},-1},{{-129,680,-30,449},-1},{{0,511,-100,411},-1},{{-107,702,-18,621},-1},{{200,595,0,400},-1},{{-93,604,-8,423},-1}};
      for(size_t i=0;i<sizeof tb/sizeof *tb;i++) ACHECK(kh_aspect_title_elem(tb[i].b)==tb[i].e);
      /* the layout of the phone's portrait screen, from the numbers (not from the code's tables): GAME SETTINGS 60 GS rows above the
       * bottom, the menu 18 above it, the picture 1.35 at the top, the logo 1.1 centred between */
      double b=kh_aspect_b(2007,2853), yT=-b, yB=416.0+b, st=yB-60.0-22.0, mt=st-18.0-107.0, sP=1.35, pb=yT+400.0*sP, lt=pb+(mt-pb-247.0*1.1)/2.0; TtLay L; int ts=0;
      ACHECK(kh_aspect_title_layout(2007,2853,&L,&ts)==1&&ts==0&&near(L.s[TT_PIC],1.35,1e-12)&&near(L.s[TT_LOGO],1.1,1e-12)&&L.s[TT_MENU]==1.0&&L.s[TT_SET]==1.0);
      ACHECK(near(L.ty[TT_PIC],yT,1e-9)&&near(L.ty[TT_SET],st,1e-9)&&near(L.ty[TT_MENU],mt,1e-9)&&near(L.ty[TT_LOGO],lt,1e-9)&&L.tx[TT_PIC]==256.0&&L.tx[TT_LOGO]==256.0);
      ACHECK(256.0-200.0*sP>=-85.334&&256.0+200.0*sP<=597.334&&pb<lt-12.0&&lt+247.0*1.1<mt-12.0&&st+22.0<=yB-60.0+1e-9);   /* inside the frame, in this order */
      ACHECK(near(lt-pb,mt-(lt+247.0*1.1),1e-9));                                                     /* the logo centred between */
      ACHECK(kh_aspect_title_layout(2853,2007,&L,&ts)==0&&kh_aspect_title_layout(1920,1350,&L,&ts)==0&&kh_aspect_title_layout(2007,2007,&L,&ts)==0&&ts==0);
      ACHECK(kh_aspect_title_layout(1000,1001,&L,&ts)==0&&ts==1);                                       /* too short for the stack: the band, counted */
      ACHECK(kh_aspect_title_layout(1080,1920,&L,&ts)==1&&L.s[TT_PIC]==1.35&&near(L.ty[TT_SET],416.0+kh_aspect_b(1080,1920)-82.0,1e-9));   /* a taller screen: the same sizes, more room */
      asp_title_band=1; ACHECK(kh_aspect_title_layout(2007,2853,&L,&ts)==0); asp_title_band=0;
      asp_mode=0; ACHECK(kh_aspect_title_layout(2007,2853,&L,&ts)==0); asp_mode=1;
      /* through the hooks */
      u8 tsv[16]; memcpy(tsv,hp(g,T_LAYOUT_PTR),16); u8 tsave[0x204]; memcpy(tsave,hp(g,ID_TAB),0x204); int sdk2=kh_aspect_dk; kh_aspect_dk=1; TtStat stt; memcpy(&stt,&ttl,sizeof stt);
      u64 tl=h3t_obj(g,GS_BASE+0x5b1dc0ull,0), tm=h3t_obj(g,GS_BASE+0x5b1dc0ull,0), other=h3t_obj(g,GS_BASE+0x5b1dc0ull,0); wr64(g,T_LAYOUT_PTR,tl); wr64(g,T_MENU_PTR,tm);
      u64 list=ga_calloc(1,0x40), lo=ga_calloc(1,0x20), md=ga_calloc(1,0x10), bk=ga_calloc(1,0x20); wrf(g,md,1.0f);
      #define TTPASS(obj_) do{ int k_; wr64(g,list+0x18,(obj_)); st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){lo,md},2,&k_); ACHECK(k_&&h3_depth==1&&h3_cur_obj[0]==(obj_)); }while(0)
      #define TTEND() do{ int k_; st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){bk,lo},2,&k_); ACHECK(k_&&h3_depth==0); }while(0)
      /* xs / ys: the quad's 4 corners after the hook (GS, the offsets removed) */
      #define TTSUB(ra_,x0_,x1_,y0_,y1_) do{ h7t_quad(v,(x0_),(x1_),(y0_),(y1_)); H7SUB((ra_),8,4); for(int i_=0;i_<4;i_++){ xs[i_]=rdf(g,h7t_vb+(u64)i_*H7_STRIDE)-H7T_OX; ys[i_]=out[i_]-H7T_OY; } }while(0)
      float xs[4], ys[4];
      double r=kh_aspect_r(2007,2853);
      kh_phase_test_set_alive(0,1,0);
      h7t_screen(g,2007,2853); TTPASS(tl);
      { u64 b0=ttl.batches, p0=ttl.q[TT_PIC];
        TTSUB(H7T_LAY,195,595,0,400); ACHECK(k&&near(xs[0],256.0-200.0*sP,2e-3)&&near(xs[1],256.0+200.0*sP,2e-3)&&near(ys[0],yT,2e-3)&&near(ys[2],yT+400.0*sP,2e-3)&&ttl.batches==b0+1&&ttl.q[TT_PIC]==p0+1);
        ACHECK(near(xs[0],xs[2],0)&&near(ys[0],ys[1],0));                                             /* still an axis-aligned rectangle */
        TTSUB(H7T_LAYB,-77,298,83,194); ACHECK(near(xs[0],256.0+1.1*(-77.0-110.5),2e-3)&&near(xs[1],256.0+1.1*(298.0-110.5),2e-3)&&near(ys[0],lt+1.1*(83.0-37.0),2e-3)&&near(ys[2],lt+1.1*(194.0-37.0),2e-3));
        TTSUB(H7T_LAY,-188,-28,262,274); ACHECK(near((xs[0]+xs[1])/2,256,2e-3));
        TTSUB(H7T_LAY,-181,-11,278,293); ACHECK(near((xs[0]+xs[1])/2,256,2e-3));
        TTSUB(H7T_LAY,-212,-140,275,277); float seam=xs[1];
        TTSUB(H7T_LAY,-140,-45,275,277); ACHECK(near(xs[0],seam,2e-3)&&near(xs[1],256+83.5*1.1,2e-3));
        TTSUB(H7T_LAY,344,597,14,36); ACHECK(xs[0]==344.0f&&xs[1]==597.0f&&near(ys[0],st,2e-3)&&near(ys[2],st+22.0,2e-3));   /* GAME SETTINGS: x as it was */
        TTSUB(H7T_LAY,-47,522,305,330); ACHECK(xs[0]==-47.0f&&xs[1]==522.0f&&near(ys[0],mt+10.0,2e-3)&&near(ys[2],mt+35.0,2e-3));   /* the menu row drawn by the title layout (the exit animation) */
        u64 r10=h7.kind[H7K_FULL];
        TTSUB(H7T_LAY,-149,660,-31,448); ACHECK(xs[0]==-149.0f&&near(ys[0],h7_map(-31,h7_cover(-31,448,208.0*(r-1.0))),2e-3)&&h7.kind[H7K_FULL]==r10+1);   /* the backdrop: R1 as before */
        u64 l0=ttl.left; TTSUB(H7T_LAY,0,511,-100,411); ACHECK(xs[0]==0.0f&&ys[0]==-100.0f&&ys[2]==411.0f&&ttl.left==l0+1);   /* the fade-in flash: left */
        TTSUB(H7T_RECT,195,595,0,400); ACHECK(xs[0]==195.0f&&near(ys[0],-b,2e-3)&&ttl.left==l0+2); }          /* another draw routine: not the layout's part (R3 as before) */
      TTEND();
      TTPASS(tm);                                                                                        /* the menu's layout object */
      { TTSUB(GS_BASE+0x1977e3ull,198,326,306,330); ACHECK(xs[0]==198.0f&&near(ys[0],mt+11.0,2e-3)&&near(ys[2],mt+35.0,2e-3));     /* its glow strip (f_1969a0's third path) */
        TTSUB(H7T_LAYB,-83,-42,295,336); ACHECK(xs[0]==-83.0f&&near(ys[0],mt,2e-3)); }
      TTEND();
      /* not the title's: another Tz::Layout (counted), the title task not running, KHNMC_TITLE_LAYOUT=band, a landscape screen, a screen
       * too short: the picture keeps today's placement (R3: moved up by b, x as it was), the logo stays where it is */
      for(int c=0;c<5;c++){ float W=c==3?2670.0f:c==4?1000.0f:2007.0f, H=c==3?1878.0f:c==4?1001.0f:2853.0f; double bb_=kh_aspect_b(W,H); u64 ot=ttl.other, ts0=ttl.tooshort;
          h7t_screen(g,W,H); if(c==1) kh_phase_test_set_alive(0,0,0); if(c==2) asp_title_band=1;
          TTPASS(c==0?other:tl);
          TTSUB(H7T_LAY,195,595,0,400); ACHECK(k&&xs[0]==195.0f&&xs[1]==595.0f&&near(ys[0],-bb_,2e-3)&&near(ys[2],400.0-bb_,2e-3));
          TTSUB(H7T_LAY,-77,298,83,194); ACHECK(xs[0]==-77.0f&&ys[0]==83.0f&&ys[2]==194.0f);
          ACHECK(ttl.other==ot+(c==0?2u:0u)&&ttl.tooshort==ts0+(c==4?2u:0u));
          TTEND(); kh_phase_test_set_alive(0,1,0); asp_title_band=0; }
      /* pc and the test aid h7: not one bit */
      h7t_screen(g,2007,2853); TTPASS(tl); asp_off_h7=1; TTSUB(H7T_LAY,195,595,0,400); ACHECK(k&&same&&xs[0]==195.0f&&ys[0]==0.0f&&ys[2]==400.0f); asp_off_h7=0; TTEND();
      asp_mode=0; { int k_; wr64(g,list+0x18,tl); st_call_ra(g,f_126500,DK_RA_MODE,list,(u64[]){lo,md},2,&k_); TTSUB(H7T_LAY,195,595,0,400); ACHECK(k&&same&&ys[0]==0.0f&&ys[2]==400.0f); st_call_ra(g,f_126500,DK_RA_END,list,(u64[]){bk,lo},2,&k_); } asp_mode=1;
      #undef TTPASS
      #undef TTEND
      #undef TTSUB
      kh_phase_test_set_alive(0,0,0); memcpy(hp(g,T_LAYOUT_PTR),tsv,16); memcpy(&ttl,&stt,sizeof stt); kh_aspect_dk=sdk2; memcpy(hp(g,ID_TAB),tsave,0x204);
      ga_free(tl); ga_free(tm); ga_free(other); ga_free(list); ga_free(lo); ga_free(md); ga_free(bk); }
    #undef H7SUB
    ga_free(h7t_vb); h7t_vb=0;
    memcpy(&h7,&sh7,sizeof sh7); memcpy(&h3,&sh3,sizeof sh3); memcpy(h7r,srow,sizeof srow); n_h7r=snr; h7r_cut=scut; h7_h1_no3d=sn1; h7_h3_no3d=sn3;
    asp_mode=sm; asp_h1_uncond=su; asp_off_h3=so3; kh_aspect_dk=sdk; kh_aspect_m=sml; kh_aspect_log=sl; atomic_store(&last3d_plus1,s3);
    wr64(g,GX+0x59b8,s59b8); wr64(g,GX+0x7c00,s7c00); memcpy(hp(g,GX+0x578c),svb,0x60); memcpy(hp(g,GX+0x4b0),s4b0,0x18); wr32(g,GX_GUARD,guard); memcpy(hp(g,GX),gxsave,0x148);
}
/* ---- task 49: the zoom ----------------------------------------------------------------------------------------------------------------
 * A: zoom_build (what the hook f_19cad0 runs in fill mode) around a stand-in for the lifted body that records cam+0x40 as it sees it,
 *    writes marks into the four matrices, the stack below the call and rax, and "returns" (rsp + 8) as the lifted body does.
 * B: the real hooks and the lifted bodies.  The game's own camera init f_19dc50(cam, d, sx, sy, 2048, 2048, 1, 16770000, near, far)
 *    with the arguments f_19d050 passes for the two scene cameras (read from the image: 0x14019d05e..0x14019d140), which ends in the
 *    tail jump to f_19cad0, i.e. the hook (0x14019dd92): the camera after the hook is, all 0x184 bytes, the exe's build; the noted
 *    draw matrices are, bit for bit, the lifted body's build of a camera whose distance is d x z.  Then the lifted f_19cfc0 through its
 *    hook from D, G, M and unknown return addresses on both scene cameras and their previous-frame copies (0x140ac1840 / 0x140ac1220
 *    and 0x140ac19c4 / 0x140ac13a4), the lifted f_19d040 and f_19d010 (not changed by the zoom), H2 on the camera and on a copy. */
void f_19dc50(KCtx *); void f_19cfc0(KCtx *); void f_19d010(KCtx *);
static u64 zt_cam; static u32 zt_seen[4], zt_put; static int zt_calls, zt_put_on;
static void zt_fn(KCtx *g){
    int n=zt_calls++; u32 d=rd32(g,zt_cam+CAM_D); if(n<4) zt_seen[n]=d;
    wr32(g,zt_cam+CAM_VP,d); wr32(g,zt_cam+0x84,d^1u); wr32(g,zt_cam+0xc4,d^2u); wr32(g,zt_cam+CAM_PROJ,d^3u); wr32(g,zt_cam+0x7c,0x43800000u);
    wr32(g,KH_RSP(g)-0x100,d^4u); KH_RAX(g)=0x1000u+(u64)n;                         /* the stack below the call and a register */
    if(zt_put&&n+1==zt_put_on) wr32(g,zt_cam+CAM_D,zt_put);
    KH_RSP(g)+=8;                                                                    /* `ret` */
}
/* zoom_build as the hook calls it (camera in guest rcx, the return address on the guest stack), around the stand-in; *sp_out = rsp at the call */
static void zt_build(KCtx *g,u64 cam,u64 ra,u64 *sp_out){
    u64 sv_sp=KH_RSP(g), sv_cx=KH_RCX(g), sp=((sv_sp-0x400)&~15ull)-8; wr64(g,sp,ra); KH_RSP(g)=sp; KH_RCX(g)=cam; zt_calls=0;
    zoom_build(g,zt_fn); if(sp_out) *sp_out=sp; KH_RSP(g)=sv_sp; KH_RCX(g)=sv_cx;
}
/* the lifted f_19dc50 on cam with distance d and return address ra (its tail jump keeps ra for f_19cad0) */
static int zt_init(KCtx *g,u64 cam,float d,u64 ra){
    v128 sx[4]; for(int i=0;i<4;i++) sx[i]=KH_XMM(g,i);
    float a2=rdf(g,GS_BASE+0x585c7cull), a3=rdf(g,GS_BASE+0x5b1380ull);                      /* the scales 0.8 and 0.8667 (cam+0x4c / +0x50) */
    { v128 o=VZ; memcpy(&o,&d,4); KH_XMM(g,1)=o; } { v128 o=VZ; memcpy(&o,&a2,4); KH_XMM(g,2)=o; } { v128 o=VZ; memcpy(&o,&a3,4); KH_XMM(g,3)=o; }
    u64 c2048=rd32(g,GS_BASE+0x5b2c08ull), one=rd32(g,GS_BASE+0x623a80ull);
    u64 a[10]={cam,0,0,0,c2048,c2048,one,rd32(g,GS_BASE+0x5b26f0ull),one,rd32(g,GS_BASE+0x5b26ecull)};   /* near 1, far 4000000 */
    int ok=0; st_call_ra(g,f_19dc50,ra,KH_RDI(g),a,10,&ok);
    for(int i=0;i<4;i++) KH_XMM(g,i)=sx[i];
    return ok;
}
/* the lifted f_19cfc0(cl) through its hook from return address ra: the camera pointer */
static u64 zt_get(KCtx *g,int cl,u64 ra){ int ok=0; u64 r=st_call_ra(g,f_19cfc0,GS_BASE+ra,KH_RDI(g),(u64[]){(u64)cl},1,&ok); return ok?r:0; }
/* the lifted f_19d010(cam): the low 4 bytes of xmm0 */
static u32 zt_d(KCtx *g,u64 cam){ v128 s0=KH_XMM(g,0); int k=0; st_call(g,f_19d010,(u64[]){cam},1,&k); u32 r; memcpy(&r,&KH_XMM(g,0),4); KH_XMM(g,0)=s0; return k?r:0xdeadbeefu; }
static float zt_f(const u8 *s,u32 off){ float f; memcpy(&f,s+off,4); return f; }
static void zoom_selftest(KCtx *g){
    /* the value: DIGITS[.DIGITS] in 1.0 .. 1.89 (review finding 3) */
    { double z=0;
      ACHECK(kh_fov_zoom_parse("1.3",&z)==0&&z==1.3); ACHECK(kh_fov_zoom_parse("1",&z)==0&&z==1.0); ACHECK(kh_fov_zoom_parse("1.0",&z)==0&&z==1.0);
      ACHECK(kh_fov_zoom_parse("1.89",&z)==0&&z==1.89); ACHECK(kh_fov_zoom_parse("1.8900",&z)==0&&z==1.89); ACHECK(kh_fov_zoom_parse("1.42",&z)==0&&z==1.42);
      ACHECK(kh_fov_zoom_parse("01.5",&z)==0&&z==1.5); ACHECK(kh_fov_zoom_parse("1.15",&z)==0&&z==1.15); ACHECK(kh_fov_zoom_parse("1.88999999999999",&z)==0&&z==1.88999999999999);
      static const char *bad[]={"","1.",".",".5","1..2","1.2.3","+1.2","-1.2"," 1.2","1.2 ","1,15","1e0","0x1p0","nan","inf","1.15x","one","1.0000000000000001"};
      z=7.0; for(size_t i=0;i<sizeof bad/sizeof *bad;i++) ACHECK(kh_fov_zoom_parse(bad[i],&z)==1&&z==7.0);
      ACHECK(kh_fov_zoom_parse(NULL,&z)==1);
      static const char *range[]={"0","0.99","0.999999","1.891","1.8900000000001","1.9","2","2.0","3","1000000","0.0"};
      for(size_t i=0;i<sizeof range/sizeof *range;i++) ACHECK(kh_fov_zoom_parse(range[i],&z)==2&&z==7.0); }
    /* finding 3, the bound itself: (4 / 3) tan(f) / tan(f - 10 deg), f = half the horizontal FOV, has its least value 1.894 > 1.89 at 50 deg;
     * the routes' field camera (fov_x 1.55 rad): 1.907 */
    { const double PI_=3.14159265358979323846; double lo=9; for(int i=1001;i<8999;i++){ double f=i/100.0*PI_/180, q=(341.333333/256.0)*__builtin_tan(f)/__builtin_tan(f-10*PI_/180); if(q<lo) lo=q; }
      ACHECK(lo>ZOOM_MAX&&lo<1.8940&&near((341.333333/256.0)*__builtin_tan(0.775)/__builtin_tan(0.775-0.17453),1.907,1e-3)); }
    int sm=asp_mode, szs=asp_zoom_set, snc=asp_nocull; double szt=asp_zoom_tall; ZoomStat sst; memcpy(&sst,&zs,sizeof sst);
    ZoomRa sra[16]; memcpy(sra,zra,sizeof sra); int snra=n_zra; u64 scut=zra_cut; ZoomRa sua[8]; memcpy(sua,zua,sizeof sua); int snua=n_zua; u64 sucut=zua_cut;
    static ZoomEnt sent[ZOOM_NCAM][2]; memcpy(sent,zent,sizeof sent); u64 ssf=zsup_frame; int ssh=zsup_have;
    u8 gxsave[0x100]; memcpy(gxsave,hp(g,GX),0x100); u32 guard=rd32(g,GX_GUARD); wr32(g,GX_GUARD,0x80000000u);
    static u8 ccsave[ZOOM_NCAM*CAM_SIZE], cpsave[ZOOM_NCAM*CAM_SIZE]; memcpy(ccsave,hp(g,CAM_CUR),sizeof ccsave); memcpy(cpsave,hp(g,CAM_PREV),sizeof cpsave);
    u32 sidx=rd32(g,CAM_IDX); u8 ssel=rd8(g,CAM_SEL);
    int slog=kh_aspect_log; kh_aspect_log=0;                                                 /* no camera lines from the test cases */
    memset(&zs,0,sizeof zs); zs.warned=1; zs.s_warned=1; n_zra=0; zra_cut=0; n_zua=0; zua_cut=0; asp_nocull=0;   /* warned: no WARNING lines from the test cases */
    memset(zent,0,sizeof zent); zsup_have=0; zsup_frame=0;
    /* the zoom of a screen: KHNMC_FOV_ZOOM (default 1.3) in fill mode when it is taller than wide, else exactly 1 */
    asp_mode=1; asp_zoom_tall=ZOOM_DEFAULT;
    ACHECK(ZOOM_DEFAULT==1.3&&kh_aspect_zoom(2007,2853)==1.3&&kh_aspect_zoom(1878,2670)==1.3&&kh_aspect_zoom(1080,1081)==1.3);
    ACHECK(kh_aspect_zoom(2853,2007)==1.0&&kh_aspect_zoom(1920,1080)==1.0&&kh_aspect_zoom(1920,1350)==1.0&&kh_aspect_zoom(2670,1878)==1.0);
    ACHECK(kh_aspect_zoom(2000,2000)==1.0&&kh_aspect_zoom(0,0)==1.0&&kh_aspect_zoom(0,100)==1.0&&kh_aspect_zoom(2560,1080)==1.0);
    asp_mode=0; ACHECK(kh_aspect_zoom(2007,2853)==1.0); asp_mode=1;                             /* pc: never */
    asp_zoom_tall=1.15; ACHECK(kh_aspect_zoom(2007,2853)==1.15&&kh_aspect_zoom(2853,2007)==1.0); asp_zoom_tall=ZOOM_DEFAULT;
    ACHECK(zoom_value(fbits(512.0f),1.3)==fbits(665.6f)&&zoom_value(fbits(512.0f),1.89)==fbits((float)(512.0*1.89))&&zoom_value(fbits(512.0f),1.5)==fbits(768.0f));
    /* the play modes: the zoom is 1 in the game frame of an M call and the next one */
    ACHECK(zoom_supp_at(100,1,100)&&zoom_supp_at(100,1,99)&&!zoom_supp_at(100,1,98)&&!zoom_supp_at(100,0,100)&&!zoom_supp_at(100,1,101)&&!zoom_supp_at(0,0,0));
    /* the cameras and the call sites, against the image: f_19d050 builds two scene cameras (`lea rcx,[rip+..]` at 0x14019d05e and
     * 0x14019d14c), the second ends 8 bytes before the view array of f_19ce40(0) and the previous-frame copies end at the int 0x140ac1528;
     * f_188120's build is the call at 0x14018830d; every entry of zsites is the return address of a `call f_19cfc0`, in rising order,
     * and .text has exactly 62 such calls and no jump to it (D sites: the slots 0..8, once each) */
    #define LEA_T(a_) ((a_)+7+(u64)(s64)(s32)rd32(g,(a_)+3))
    ACHECK(rd8(g,GS_BASE+0x19d05eull)==0x48&&rd8(g,GS_BASE+0x19d05full)==0x8d&&rd8(g,GS_BASE+0x19d060ull)==0x0d&&LEA_T(GS_BASE+0x19d05eull)==CAM_CUR);
    ACHECK(rd8(g,GS_BASE+0x19d14cull)==0x48&&rd8(g,GS_BASE+0x19d14dull)==0x8d&&rd8(g,GS_BASE+0x19d14eull)==0x0d&&LEA_T(GS_BASE+0x19d14cull)==CAM_CUR+CAM_SIZE);
    #undef LEA_T
    ACHECK(CAM_CUR+ZOOM_NCAM*CAM_SIZE+8==GS_BASE+0xac1b50ull&&CAM_PREV+ZOOM_NCAM*CAM_SIZE==GS_BASE+0xac1528ull);
    ACHECK(rd8(g,ZOOM_RA_MAPOBJ-5)==0xe8&&ZOOM_RA_MAPOBJ+(u64)(s64)(s32)rd32(g,ZOOM_RA_MAPOBJ-4)==GS_BASE+0x19cad0ull);
    ACHECK(rd32(g,GS_BASE+0x19d010ull)==0x41100ff3u&&rd16(g,GS_BASE+0x19d014ull)==0xc340u);   /* f_19d010: movss xmm0,[rcx+0x40]; ret */
    { int ok=ZOOM_NSITE==62, nd=0, slots=0;
      for(int i=0;i<ZOOM_NSITE;i++){ u64 r=GS_BASE+zsites[i].rva;
          if(rd8(g,r-5)!=0xe8||r+(u64)(s64)(s32)rd32(g,r-4)!=GS_BASE+0x19cfc0ull||(i&&zsites[i].rva<=zsites[i-1].rva)) ok=0;
          if(zoom_site(r)!=i||zoom_site(r+1)>=0||zoom_site(r-1)>=0) ok=0;
          if(zsites[i].cls==ZS_DRAW){ nd++; if(zsites[i].slot<0||zsites[i].slot>=ZOOM_NDRAW||(slots&(1<<zsites[i].slot))) ok=0; else slots|=1<<zsites[i].slot; }
          else if(zsites[i].slot!=-1) ok=0; }
      ACHECK(ok&&nd==ZOOM_NDRAW&&slots==(1<<ZOOM_NDRAW)-1);
      const u8 *t=hp(g,GS_BASE+0x1000ull); int ne8=0, ne9=0, inside=0;                    /* .text: VA 0x1000, virtual size 0x5773f3 */
      for(u64 i=0;i+5<=0x5773f3ull;i++){ if(t[i]!=0xe8&&t[i]!=0xe9) continue;
          s32 rel; memcpy(&rel,t+i+1,4); u64 va=GS_BASE+0x1000ull+i;
          if(va+5+(u64)(s64)rel==GS_BASE+0x19cfc0ull){ if(t[i]==0xe8){ ne8++; if(zoom_site(va+5)>=0) inside++; } else ne9++; } }
      ACHECK(ne8==62&&ne9==0&&inside==62); }
    #define ZSCREEN(W_,H_) do{ wrf(g,GX+0x18,(float)(W_)); wrf(g,GX+0x1c,(float)(H_)); wrf(g,GX+0x10,(float)(W_)); wrf(g,GX+0x14,(float)(H_)); wrf(g,GX+0x20,1); wrf(g,GX+0x24,1); wrf(g,GX+0x28,1); \
                               wrf(g,GX+0x38,512); wrf(g,GX+0x3c,416); wrf(g,GX+0x48,640); wrf(g,GX+0x4c,480); }while(0)   /* the GS frame values a run has (task 42's first-pass line) */
    const u32 snan=0x7f812345u, v=fbits(512.0f), vz=fbits(665.6f);                              /* a signalling NaN (any write changes it), d, d x 1.3 */
    const u64 RA=GS_BASE+0x19cc81ull;                                                          /* f_19cba0's call of f_19cad0 returns here */
    u64 heap=ga_calloc(1,0x200), sp=0;
    /* A: the portrait screen, fill, the default zoom: two builds, the first (the exe's) stands */
    ZSCREEN(2007,2853);
    for(int k=0;k<ZOOM_NCAM;k++){ u64 cam=CAM_CUR+CAM_SIZE*(u64)k; zt_cam=cam; zt_put=0;
        memset(hp(g,cam),0,CAM_SIZE); wr32(g,cam+CAM_D,v); u64 e0=zs.edited[k]; zt_build(g,cam,RA,&sp);
        ACHECK(zt_calls==2&&zt_seen[0]==v&&zt_seen[1]==vz&&zs.edited[k]==e0+1);                  /* 1: d, 2: d x 1.3 */
        ACHECK(rd32(g,cam+CAM_D)==v&&rd32(g,cam+CAM_VP)==v&&rd32(g,cam+0x84)==(v^1u)&&rd32(g,cam+0xc4)==(v^2u)&&rd32(g,cam+CAM_PROJ)==(v^3u));   /* the camera: build 1 */
        ACHECK(KH_RAX(g)==0x1000u&&rd32(g,sp-0x100)==(v^4u));                                 /* registers and the stack below the call: as build 1 left them */
        ACHECK(zent[k][0].valid&&!memcmp(zent[k][0].pc_vp,&v,4)&&rd32(g,cam+CAM_VP)==v);
        { u32 a,b; memcpy(&a,zent[k][0].z_vp,4); memcpy(&b,zent[k][0].z_pr,4); ACHECK(a==vz&&b==(vz^3u)); }   /* the noted draw matrices: build 2 */
        wr32(g,cam+CAM_D,snan); zt_build(g,cam,RA,&sp); ACHECK(zt_calls==2&&zt_seen[0]==snan&&zt_seen[1]!=snan&&rd32(g,cam+CAM_D)==snan);   /* restored bit for bit */
        ACHECK(zent[k][1].valid&&!memcmp(zent[k][1].pc_vp,&v,4)&&zent[k][0].valid);           /* the build before stays noted (the previous-frame copy) */
        zt_put=0x40490fdbu; zt_put_on=2; u64 in0=zs.inside; wr32(g,cam+CAM_D,v); zt_build(g,cam,RA,&sp);   /* build 2 wrote cam+0x40 itself: build 1 stands, nothing noted */
        ACHECK(rd32(g,cam+CAM_D)==v&&rd32(g,cam+CAM_VP)==v&&zs.inside==in0+1&&!zent[k][0].valid&&zs.edited[k]==e0+2);
        zt_put_on=1; wr32(g,cam+CAM_D,v); zt_build(g,cam,RA,&sp);                              /* build 1 wrote it (the exe would): its value stays, build 2 from it */
        ACHECK(rd32(g,cam+CAM_D)==0x40490fdbu&&zt_seen[1]==zoom_value(0x40490fdbu,1.3)&&zs.inside==in0+1&&zent[k][0].valid); zt_put=0; }
    { zt_cam=heap; wr32(g,heap+CAM_D,snan); u64 o0=zs.other; zt_build(g,heap,RA,0);              /* an object-owned camera: one build, listed */
      ACHECK(zt_calls==1&&zt_seen[0]==snan&&rd32(g,heap+CAM_D)==snan&&zs.other==o0+1&&n_zra==1&&zra[0].ra==RA&&zra[0].n==1);
      zt_cam=CAM_PREV; wr32(g,CAM_PREV+CAM_D,snan); zt_build(g,CAM_PREV,RA+0x10,0);             /* a previous-frame copy is no build target */
      ACHECK(zt_calls==1&&rd32(g,CAM_PREV+CAM_D)==snan&&zs.other==o0+2&&n_zra==2&&zra[1].ra==RA+0x10);
      zt_cam=heap; wr32(g,heap+CAM_D,v); u64 e0=zs.edited[ZC_MAPOBJ]; zt_build(g,heap,ZOOM_RA_MAPOBJ,0);   /* the map pass's per-object copy: drawn only, zoomed */
      ACHECK(zt_calls==2&&zt_seen[1]==vz&&rd32(g,heap+CAM_D)==v&&rd32(g,heap+CAM_VP)==vz&&rd32(g,heap+CAM_PROJ)==(vz^3u)&&rd32(g,heap+0x84)==(v^1u)&&rd32(g,heap+0xc4)==(v^2u));
      ACHECK(zs.edited[ZC_MAPOBJ]==e0+1&&zs.other==o0+2);
      u64 b0=zs.badcam; zt_build(g,0,ZOOM_RA_MAPOBJ,0); ACHECK(zs.badcam==b0+1&&zt_calls==1); }   /* no camera: one build, counted */
    /* identity: a screen that is not taller than wide, and the value 1 on the tall one: one build, nothing written, nothing noted */
    { static const int wide[6][2]={{2853,2007},{1920,1080},{1920,1350},{2670,1878},{2000,2000},{2560,1080}}; u64 cam=CAM_CUR; zt_cam=cam;
      for(int i=0;i<7;i++){ if(i<6) ZSCREEN(wide[i][0],wide[i][1]); else { ZSCREEN(2007,2853); asp_zoom_tall=1.0; }
          zent[0][0].valid=1; wr32(g,cam+CAM_D,snan); u64 id0=zs.identity, c0=zs.calls, o0=zs.other; zt_build(g,cam,RA,0);
          ACHECK(zt_calls==1&&zt_seen[0]==snan&&rd32(g,cam+CAM_D)==snan&&zs.identity==id0+1&&zs.calls==c0+1&&zs.other==o0&&!zent[0][0].valid); }
      asp_zoom_tall=1.89; wr32(g,cam+CAM_D,v); zt_build(g,cam,RA,0); ACHECK(zt_calls==2&&zt_seen[1]==fbits((float)(512.0*1.89))&&rd32(g,cam+CAM_D)==v);   /* the range's ends */
      asp_zoom_tall=1.5; zt_build(g,cam,RA,0); ACHECK(zt_seen[1]==fbits(768.0f)&&rd32(g,cam+CAM_D)==v); asp_zoom_tall=ZOOM_DEFAULT;
      /* a play mode ran in this game frame: one build, nothing noted */
      zsup_have=1; zsup_frame=kh_game_frames(); zent[0][0].valid=1; u64 su0=zs.suppressed; zt_build(g,cam,RA,0);
      ACHECK(zt_calls==1&&zt_seen[0]==v&&zs.suppressed==su0+1&&!zent[0][0].valid); zsup_have=0;
      zt_build(g,cam,ZOOM_RA_MAPOBJ,0); ACHECK(zt_calls==2); zsup_have=1; zt_build(g,cam,ZOOM_RA_MAPOBJ,0); ACHECK(zt_calls==1); zsup_have=0; }
    /* B: the real hooks and the lifted bodies, on both scene cameras */
    { static u8 Spc[CAM_SIZE], Spc2[CAM_SIZE], Sref[CAM_SIZE], Sref2[CAM_SIZE], Sa[CAM_SIZE], Sb[CAM_SIZE];
      float d0=rdf(g,GS_BASE+0x5b1390ull), d1=600.0f; ACHECK(d0==512.0f);
      ZSCREEN(2007,2853);
      asp_mode=0; memset(hp(g,CAM_CUR),0,CAM_SIZE); u64 c0=zs.calls; ACHECK(zt_init(g,CAM_CUR,d0,RA)); memcpy(Spc,hp(g,CAM_CUR),CAM_SIZE);   /* pc: the exe's build */
      /* the numbers by hand: halfW = 256 + 85.33, halfH = 208; clip m00 = d sx / halfW = 512 * 0.8 / 341.33 = 1.2, m11 = d sy / 208 =
       * 512 * 0.86667 / 208 = 2.13333; the guard band divides by 2047 */
      ACHECK(zs.calls==c0&&rd32(g,CAM_CUR+CAM_D)==v&&near(zt_f(Spc,0x7c),341.33333,1e-3)&&zt_f(Spc,0x80)==208.0f);
      ACHECK(near(zt_f(Spc,0x84),1.2,1e-5)&&near(zt_f(Spc,0x98),2.1333333,1e-5)&&near(zt_f(Spc,0xc4),512.0*0.8/2047.0,1e-6)&&near(zt_f(Spc,0xd8),512.0*(13.0/15.0)/2047.0,1e-6));
      memset(hp(g,CAM_CUR),0,CAM_SIZE); ACHECK(zt_init(g,CAM_CUR,bf(vz),RA)); memcpy(Sref,hp(g,CAM_CUR),CAM_SIZE);   /* the exe's own build for d = 665.6 */
      memset(hp(g,CAM_CUR),0,CAM_SIZE); ACHECK(zt_init(g,CAM_CUR,d1,RA)); memcpy(Spc2,hp(g,CAM_CUR),CAM_SIZE);       /* and for d = 600, 780 */
      memset(hp(g,CAM_CUR),0,CAM_SIZE); ACHECK(zt_init(g,CAM_CUR,bf(zoom_value(fbits(d1),1.3)),RA)); memcpy(Sref2,hp(g,CAM_CUR),CAM_SIZE);
      asp_mode=1;
      for(int k=0;k<ZOOM_NCAM;k++){ u64 cam=CAM_CUR+CAM_SIZE*(u64)k, prev=CAM_PREV+CAM_SIZE*(u64)k; wr32(g,CAM_IDX,(u32)k);
        /* the build through the hook (f_19dc50's tail jump): the camera is the exe's build, all 0x184 bytes; the noted matrices are the
         * exe's build for d x 1.3, both axes by 1.3 */
        memset(hp(g,cam),0,CAM_SIZE); u64 e0=zs.edited[k]; ACHECK(zt_init(g,cam,d0,RA)); ACHECK(zs.edited[k]==e0+1&&!memcmp(hp(g,cam),Spc,CAM_SIZE));
        ACHECK(zent[k][0].valid&&!memcmp(zent[k][0].pc_vp,Spc+CAM_VP,0x40)&&!memcmp(zent[k][0].pc_pr,Spc+CAM_PROJ,0x40));
        ACHECK(!memcmp(zent[k][0].z_vp,Sref+CAM_VP,0x40)&&!memcmp(zent[k][0].z_pr,Sref+CAM_PROJ,0x40));
        ACHECK(memcmp(Sref,Spc,0x40)!=0&&near(zt_f(Sref,0x104)/zt_f(Spc,0x104),1.3,1e-6)&&near(zt_f(Sref,0x118)/zt_f(Spc,0x118),1.3,1e-6));
        /* f_19cfc0 through its hook: a D site gets a copy (the camera with the zoomed cam+0x00 / +0x104), a G site the camera */
        u64 m0=zs.d_copy[0]; u64 r=zt_get(g,1,0x18695c);                                     /* the map pass */
        ACHECK(kh_aspect_zoom_shadow(r)&&r!=cam&&zs.d_copy[0]==m0+1);
        if(kh_aspect_zoom_shadow(r)){ const u8 *s=hp(g,r);
            ACHECK(!memcmp(s,Sref,0x40)&&!memcmp(s+0x40,Spc+0x40,0xc4)&&!memcmp(s+CAM_PROJ,Sref+CAM_PROJ,0x40)&&!memcmp(s+0x144,Spc+0x144,0x40)); }
        ACHECK(zt_get(g,1,0x1b5f25)==cam&&zt_get(g,1,0x3f3ef0)==cam&&zt_get(g,1,0x176740)==cam&&zt_get(g,1,0x430ecb)==cam);   /* kn, YS, the framing, the script syscall */
        /* the clip matrix (f_19d040, kn's return address) and the distance (f_19d010) of the camera and of its copy are the exe's */
        { int ok=0; u64 c=st_call_ra(g,f_19d040,GS_BASE+0x1b5f2dull,KH_RDI(g),(u64[]){cam},1,&ok); ACHECK(ok&&c==cam+0x84&&!memcmp(hp(g,c),Spc+0x84,0x80)); }
        ACHECK(zt_d(g,cam)==v&&fbits(zt_f(hp(g,r),CAM_D))==v);
        /* the next frame as f_19cba0 makes it: the previous-frame copy, then a build with another distance */
        memcpy(hp(g,prev),hp(g,cam),CAM_SIZE); ACHECK(zt_init(g,cam,d1,RA)&&!memcmp(hp(g,cam),Spc2,CAM_SIZE)&&zt_d(g,prev)==v&&zt_d(g,cam)==fbits(d1));
        wr8(g,CAM_SEL,0); u64 p0=zs.d_copy[1];
        r=zt_get(g,0,0x166719);                                                                /* dk f_1666f0, cl = 0: the previous-frame copy */
        ACHECK(kh_aspect_zoom_shadow(r)&&zs.d_copy[1]==p0+1&&!memcmp(hp(g,r),Sref,0x40)&&!memcmp((u8*)hp(g,r)+CAM_PROJ,Sref+CAM_PROJ,0x40)&&!memcmp((u8*)hp(g,r)+0x40,Spc+0x40,0xc4));
        ACHECK(zt_get(g,0,0x1cb3b5)==prev);                                                    /* the target search (G) gets the copy itself */
        r=zt_get(g,1,0x2bd4ec);                                                                /* the effect pass on the current camera: the new build */
        ACHECK(kh_aspect_zoom_shadow(r)&&!memcmp(hp(g,r),Sref2,0x40)&&!memcmp((u8*)hp(g,r)+CAM_PROJ,Sref2+CAM_PROJ,0x40)&&!memcmp((u8*)hp(g,r)+0x40,Spc2+0x40,0xc4));
        { KhRecords cut={0};
          kh_records_begin(&cut,0); kh_rewind_aspect(&cut); kh_records_end(&cut);
          memset(zent,0,sizeof zent); memset(zshadow,0,sizeof local_shadow); zsup_have=1; zsup_frame=kh_game_frames();
          kh_records_begin(&cut,1); kh_rewind_aspect(&cut); kh_records_end(&cut); kh_records_free(&cut);
          ACHECK(!memcmp(hp(g,r),Sref2,0x40) && zt_get(g,1,0x2bd4ec)==r); }
        wr8(g,CAM_SEL,1); ACHECK(zt_get(g,0,0x18ef29)!=cam&&kh_aspect_zoom_shadow(zt_get(g,0,0x18ef29)));   /* byte 0x140ac183c set: cl = 0 gives the current camera */
        /* H2 on the camera (the exe's m11) and on a copy (the copy's m11: the exe's too), divided by r and restored */
        { double rr=kh_aspect_r(2007,2853); u64 s=zt_get(g,1,0x18695c); H2Stat st={"selftest"}; st.warned=1; u32 m11=rd32(g,cam+CAM_M11);
          h2t_cam=cam; h2t_put=0; h2t_seen=0; h2_around(g,cam,h2t_fn,&st); ACHECK(st.edited==1&&h2t_seen==h2_value(m11,rr)&&rd32(g,cam+CAM_M11)==m11);
          h2t_cam=s; h2t_seen=0; h2_around(g,s,h2t_fn,&st); ACHECK(st.edited==2&&st.badcam==0&&h2t_seen==h2_value(m11,rr)&&rd32(g,s+CAM_M11)==m11&&near(rr,2.527155,1e-6)); }
        /* a play mode (M site) in this game frame: D sites get the camera; landscape: the camera; a camera built at zoom 1: no noted
         * build matches, the camera */
        u64 n0=zs.d_supp; ACHECK(zt_get(g,1,0x1f3e39)==cam&&zsup_have&&zt_get(g,1,0x18695c)==cam&&zs.d_supp==n0+1); zsup_have=0;
        ZSCREEN(2853,2007); n0=zs.d_z1; ACHECK(zt_get(g,1,0x18695c)==cam&&zs.d_z1==n0+1);
        n0=zs.identity; ACHECK(zt_init(g,cam,d0,RA)&&zs.identity==n0+1&&!zent[k][0].valid); ZSCREEN(2007,2853);   /* landscape build: one, nothing noted */
        n0=zs.d_nomatch; ACHECK(zt_get(g,1,0x18695c)==cam&&zs.d_nomatch==n0+1);
        n0=zs.s_unknown; ACHECK(zt_get(g,1,0x18695d)==cam&&zs.s_unknown==n0+1);               /* an unknown return address: the camera, counted */
        /* pc: the hook is the lifted body, nothing counted */
        asp_mode=0; n0=zs.s_calls; ACHECK(zt_init(g,cam,d0,RA)&&zt_get(g,1,0x18695c)==cam&&zs.s_calls==n0); asp_mode=1; }
      ACHECK(zs.site[zoom_site(GS_BASE+0x18695cull)]>=2*ZOOM_NCAM&&zs.d_other==0&&zs.d_thread==0);
      /* identity through the real hook: fill on screens that are not taller than wide, and the value 1 on the tall one, build what pc
       * builds, bit for bit (all 0x184 bytes), and nothing is noted */
      { static const int scr[5][2]={{2853,2007},{1920,1080},{2670,1878},{1920,1350},{2007,2853}}; u64 cam=CAM_CUR+CAM_SIZE;
        for(int i=0;i<5;i++){ ZSCREEN(scr[i][0],scr[i][1]); asp_zoom_tall=i==4?1.0:ZOOM_DEFAULT;
            asp_mode=0; memset(hp(g,cam),0,CAM_SIZE); ACHECK(zt_init(g,cam,d0,RA)); memcpy(Sa,hp(g,cam),CAM_SIZE);
            asp_mode=1; memset(hp(g,cam),0,CAM_SIZE); u64 id0=zs.identity; ACHECK(zt_init(g,cam,d0,RA)); memcpy(Sb,hp(g,cam),CAM_SIZE);
            ACHECK(!memcmp(Sa,Sb,CAM_SIZE)&&zs.identity==id0+1&&near(zt_f(Sb,0x84),1.2,1e-5)&&!zent[1][0].valid); }
        asp_zoom_tall=ZOOM_DEFAULT; }
      /* the map pass's per-object build through the real hook (return address 0x140188312): the zoomed draw matrices, the rest the exe's;
       * an object-owned camera: the exe's */
      ZSCREEN(2007,2853); asp_mode=1;
      memset(hp(g,heap),0,CAM_SIZE); ACHECK(zt_init(g,heap,d0,ZOOM_RA_MAPOBJ));
      ACHECK(!memcmp(hp(g,heap),Sref,0x40)&&!memcmp((u8*)hp(g,heap)+CAM_PROJ,Sref+CAM_PROJ,0x40)&&!memcmp((u8*)hp(g,heap)+0x40,Spc+0x40,0xc4)&&!memcmp((u8*)hp(g,heap)+0x144,Spc+0x144,0x40));
      memset(hp(g,heap),0,CAM_SIZE); ACHECK(zt_init(g,heap,d0,RA)); ACHECK(!memcmp(hp(g,heap),Spc,CAM_SIZE)); }
    #undef ZSCREEN
    ga_free(heap);
    memcpy(hp(g,CAM_CUR),ccsave,sizeof ccsave); memcpy(hp(g,CAM_PREV),cpsave,sizeof cpsave); wr32(g,CAM_IDX,sidx); wr8(g,CAM_SEL,ssel);
    wr32(g,GX_GUARD,guard); memcpy(hp(g,GX),gxsave,0x100);
    kh_aspect_log=slog; asp_mode=sm; asp_zoom_set=szs; asp_zoom_tall=szt; asp_nocull=snc; memcpy(&zs,&sst,sizeof zs); memcpy(zra,sra,sizeof zra); n_zra=snra; zra_cut=scut;
    memcpy(zua,sua,sizeof zua); n_zua=snua; zua_cut=sucut; memcpy(zent,sent,sizeof zent); zsup_frame=ssf; zsup_have=ssh;
}

int kh_aspect_selftest(KCtx *g){
    sfails=schecks=0;
    /* task 46: the cases of tasks 37 / 40 / 42 below test the hooks with H7 off (the gate of H1 / H3 as those tasks made it); H7's own
     * cases (h7_selftest) follow them with it on */
    int s_h7=asp_off_h7; asp_off_h7=1;
    /* -- r, o, b (plan section 3) -- */
    static const int same[4][2]={{1920,1080},{2560,1440},{3840,2160},{2560,1080}};
    for(int i=0;i<4;i++){ double W=same[i][0],H=same[i][1]; ACHECK(kh_aspect_r(W,H)==1.0&&kh_aspect_o(W,H)==0.0&&kh_aspect_b(W,H)==0.0); }
    ACHECK(kh_aspect_r(1920,1350)==1.25&&kh_aspect_o(1920,1350)==135.0&&kh_aspect_b(1920,1350)==52.0);
    static const int phone[4][2]={{2670,1878},{2225,1565},{1780,1252},{1335,939}};          /* native and the 5/6, 2/3, 1/2 sizes */
    static const double phone_o[4]={188.0625,156.71875,125.375,94.03125};
    for(int i=0;i<4;i++){ double W=phone[i][0],H=phone[i][1];
        ACHECK(near(kh_aspect_r(W,H),1.250437,5e-7)&&kh_aspect_o(W,H)==phone_o[i]&&near(kh_aspect_b(W,H),52.0909,5e-5)); }
    { extern int kh_window_resize_selftest(void); ACHECK(kh_window_resize_selftest()==0); }
    /* -- KHNMC_SCREEN values -- */
    { int w=0,h=0;
      ACHECK(kh_screen_parse("1920x1350",&w,&h)==0&&w==1920&&h==1350); ACHECK(kh_screen_parse("2670x1878",&w,&h)==0&&w==2670&&h==1878);
      ACHECK(kh_screen_parse("16x16384",&w,&h)==0&&w==16&&h==16384);
      static const char *bad[]={"","1920","1920x","x1080","1920X1080","1920x1080x","1920x1080 "," 1920x1080","+1920x1080","-1920x1080","1920*1080","19 20x1080","1920x-1080","0x1080x"};
      for(size_t i=0;i<sizeof bad/sizeof *bad;i++) ACHECK(kh_screen_parse(bad[i],&w,&h)==1);
      static const char *range[]={"15x1080","1920x15","16385x1080","1920x16385","0x0","9999999x1080"};
      for(size_t i=0;i<sizeof range/sizeof *range;i++) ACHECK(kh_screen_parse(range[i],&w,&h)==2); }
    /* -- S1 / S2 through the imports: the seam set, then off (the state before task 37), then the run's state back -- */
    { int old[8]; const int off[8]={0,1920,1080,1280,720,100,100,0};
      kh_screen_test_state(off,old); kh_screen_config("1920x1350",NULL); seam_cases(g,1,1920,1350);   /* the configuration path a run takes */
      kh_screen_test_state(off,NULL); seam_cases(g,0,1920,1080);
      kh_screen_test_state(old,NULL); }
    /* -- the 3D-view condition -- */
    { u64 sv=atomic_load(&last3d_plus1);
      atomic_store(&last3d_plus1,0); ACHECK(!gate_open_at(0)&&!gate_open_at(500)&&since_3d(7)==-1);
      note_3d(100); ACHECK(gate_open_at(100)&&gate_open_at(101)&&!gate_open_at(102)&&!gate_open_at(5000)&&gate_open_at(99)&&since_3d(103)==3);
      atomic_store(&last3d_plus1,sv); }
    /* -- H1 on the lifted f_11c1e0 -- */
    { u8 gxsave[32]; memcpy(gxsave,hp(g,GX+0x10),32); u32 guard=rd32(g,GX_GUARD); wr32(g,GX_GUARD,0x80000000u);
      int sm=asp_mode, su=asp_h1_uncond, sl=kh_aspect_log; u64 s3=atomic_load(&last3d_plus1); kh_aspect_log=0;
      typedef struct { float W,H,sx,sy,s; } Case;
      static const Case cs[]={{1920,1080,1,1,1},{2560,1440,1,1,1},{3840,2160,1,1,1},{2560,1080,1,1,1},       /* 16:9 or wider: identity */
                              {1920,1350,1,1,1},{2670,1878,1,1,1},{1920,1350,1,1,0.5f},{2670,1878,1.0f,0.5f,0.75f},{2670,1878,1,1,1.5f}};
      for(size_t i=0;i<sizeof cs/sizeof *cs;i++){ const Case *c=&cs[i]; float px,py,fx,fy,kx,ky,ux,uy;
          asp_mode=0; asp_h1_uncond=0; h1_call(g,c->W,c->H,c->sx,c->sy,c->s,&px,&py);                     /* pc */
          asp_mode=1; note_3d(kh_game_frames()); h1_call(g,c->W,c->H,c->sx,c->sy,c->s,&fx,&fy);         /* fill, 3D view in this frame */
          atomic_store(&last3d_plus1,0); h1_call(g,c->W,c->H,c->sx,c->sy,c->s,&kx,&ky);                  /* fill, no 3D view */
          asp_h1_uncond=1; h1_call(g,c->W,c->H,c->sx,c->sy,c->s,&ux,&uy);                                /* fill, unconditional */
          H1Model m=h1_model(c->W,c->H,c->sx,c->sy,c->s);
          ACHECK(fbits(px)==fbits(m.x)&&fbits(py)==fbits(m.y));                                            /* the model is the exe */
          ACHECK(fbits(fx)==fbits(px)&&fbits(kx)==fbits(px)&&fbits(ux)==fbits(px));                        /* x never changes */
          ACHECK(fbits(ky)==fbits(py));                                                                    /* no 3D view: the exe's bars */
          if(i<4) ACHECK(!m.narrow&&fbits(fy)==fbits(py)&&fbits(uy)==fbits(py));                           /* 16:9 or wider: identity in every mode */
          else ACHECK(m.narrow&&fbits(fy)==fbits(m.yfull)&&fbits(uy)==fbits(m.yfull)&&fy!=py); }
      { float x,y; asp_mode=0; asp_h1_uncond=0; h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f);   /* the numbers themselves */
        asp_mode=1; asp_h1_uncond=1; h1_call(g,1920,1350,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1350.0f);
        asp_mode=0; asp_h1_uncond=0; h1_call(g,2670,1878,1,1,1,&x,&y); ACHECK(x==2670.0f&&y==1501.875f);
        asp_mode=1; asp_h1_uncond=1; h1_call(g,2670,1878,1,1,1,&x,&y); ACHECK(x==2670.0f&&y==1878.0f);
        asp_mode=1; asp_h1_uncond=1; h1_call(g,2560,1080,1,1,1,&x,&y); ACHECK(x==1920.0f&&y==1080.0f);     /* wider: the exe's pillarbox stays */
        /* the scale factors, with numbers worked out by hand (not by the model): s = GX+0x28 (capped at 1), y *= GX+0x24 */
        asp_mode=0; asp_h1_uncond=0; h1_call(g,1920,1350,1,1,0.5f,&x,&y); ACHECK(x==960.0f&&y==540.0f);
        asp_mode=1; asp_h1_uncond=1; h1_call(g,1920,1350,1,1,0.5f,&x,&y); ACHECK(x==960.0f&&y==675.0f);
        asp_mode=0; asp_h1_uncond=0; h1_call(g,2670,1878,1,0.5f,0.75f,&x,&y); ACHECK(x==2002.5f&&y==563.203125f);
        asp_mode=1; asp_h1_uncond=1; h1_call(g,2670,1878,1,0.5f,0.75f,&x,&y); ACHECK(x==2002.5f&&y==704.25f); }
      asp_mode=sm; asp_h1_uncond=su; kh_aspect_log=sl; atomic_store(&last3d_plus1,s3);
      wr32(g,GX_GUARD,guard); memcpy(hp(g,GX+0x10),gxsave,32); }
    /* -- task 40: B, H2, H4, H5, M (plan section 7 unit tests: exact identity at 1920x1080, 2560x1440, 3840x2160, 2560x1080) -- */
    { static const int same4[4][2]={{1920,1080},{2560,1440},{3840,2160},{2560,1080}};
      for(int i=0;i<4;i++) ACHECK(kh_aspect_B(same4[i][0],same4[i][1])==0);
      ACHECK(kh_aspect_B(1920,1350)==52);
      for(int i=0;i<4;i++) ACHECK(kh_aspect_B(phone[i][0],phone[i][1])==53);                       /* b = 52.09: ceil */
      u8 gxsave[32]; memcpy(gxsave,hp(g,GX+0x10),32); u32 guard=rd32(g,GX_GUARD); wr32(g,GX_GUARD,0x80000000u);
      int sm=asp_mode, snc=asp_nocull, so4=asp_off_h4, so5=asp_off_h5; asp_off_h4=asp_off_h5=0;
      #define SCREEN(W_,H_) do{ wrf(g,GX+0x18,(float)(W_)); wrf(g,GX+0x1c,(float)(H_)); wrf(g,GX+0x10,(float)(W_)); wrf(g,GX+0x14,(float)(H_)); wrf(g,GX+0x20,1); wrf(g,GX+0x24,1); wrf(g,GX+0x28,1); }while(0)
      /* H2: the m11 edit around a stand-in for the culling pass that records what it sees at cam+0x98 */
      { u64 cam=ga_calloc(1,0x200); h2t_cam=cam; H2Stat st={"selftest"}; st.warned=1;           /* warned: no WARNING line in the selftest output */
        const u32 snan=0x7f812345u, v=0x3f9e0419u;                                                   /* a signalling NaN (any write changes it) and 1.2345 */
        asp_mode=1; asp_nocull=0;
        for(int i=0;i<4;i++){ SCREEN(same4[i][0],same4[i][1]);                                       /* 16:9 or wider: nothing written */
            wr32(g,cam+CAM_M11,snan); h2t_put=0; h2t_seen=0; u64 id0=st.identity; h2_around(g,cam,h2t_fn,&st);
            ACHECK(h2t_seen==snan&&rd32(g,cam+CAM_M11)==snan&&st.identity==id0+1);
            wr32(g,cam+CAM_M11,v); h2t_put=0x40490fdbu; h2_around(g,cam,h2t_fn,&st); ACHECK(rd32(g,cam+CAM_M11)==0x40490fdbu&&st.inside==0); }   /* the pass's own write stays */
        SCREEN(1920,1350); wr32(g,cam+CAM_M11,fbits(2.5f)); h2t_put=0; u64 e0=st.edited; h2_around(g,cam,h2t_fn,&st);
        ACHECK(h2t_seen==fbits(2.0f)&&rd32(g,cam+CAM_M11)==fbits(2.5f)&&st.edited==e0+1);          /* 2.5 / 1.25, then the saved bits back */
        wr32(g,cam+CAM_M11,v); h2_around(g,cam,h2t_fn,&st); ACHECK(h2t_seen==fbits((float)((double)bf(v)/1.25))&&rd32(g,cam+CAM_M11)==v);
        wr32(g,cam+CAM_M11,snan); h2_around(g,cam,h2t_fn,&st); ACHECK(rd32(g,cam+CAM_M11)==snan);   /* restored bit for bit */
        SCREEN(2670,1878); wr32(g,cam+CAM_M11,v); h2_around(g,cam,h2t_fn,&st);
        ACHECK(h2t_seen==fbits((float)((double)bf(v)/kh_aspect_r(2670,1878)))&&rd32(g,cam+CAM_M11)==v);
        asp_nocull=1; SCREEN(1920,1350); h2_around(g,cam,h2t_fn,&st); ACHECK(h2t_seen==fbits((float)((double)bf(v)/(2047.0/208.0)))&&rd32(g,cam+CAM_M11)==v);   /* the no-cull reference */
        asp_nocull=0; h2t_put=0x40490fdbu; u64 in0=st.inside; h2_around(g,cam,h2t_fn,&st);
        ACHECK(rd32(g,cam+CAM_M11)==0x40490fdbu&&st.inside==in0+1);                                  /* the pass wrote cam+0x98 itself: its value is kept, counted */
        h2t_put=0; u64 bc0=st.badcam; h2_around(g,0,h2t_fn,&st); ACHECK(st.badcam==bc0+1);
        ga_free(cam);
        /* H2b's camera = what the lifted f_19cfc0(1) returns; f_19cfc0(0) with byte 0x140ac183c = 0 is the previous-frame array (H2c's camera in play, M) */
        u32 si=rd32(g,CAM_IDX); u8 ss=rd8(g,CAM_SEL);
        for(int k=0;k<3;k++){ wr32(g,CAM_IDX,(u32)k); u64 r1=st_call(g,f_19cfc0,(u64[]){1},1,NULL); ACHECK(r1==h2b_cam(g)&&r1==CAM_CUR+CAM_SIZE*(u64)k);
            wr8(g,CAM_SEL,0); ACHECK(st_call(g,f_19cfc0,(u64[]){0},1,NULL)==CAM_PREV+CAM_SIZE*(u64)k); }
        wr32(g,CAM_IDX,si); wr8(g,CAM_SEL,ss);
        int ix; ACHECK(cam_class(CAM_CUR+CAM_SIZE,&ix)==0&&ix==1&&cam_class(CAM_PREV,&ix)==1&&ix==0&&cam_class(CAM_PREV+8,&ix)==2); }
      /* the wrappers the hooks f_186920 / f_2bd190 run (task 40 review finding 1), around a stand-in pass that records cam+0x98 of the
       * three cameras H2 could edit: f_19cfc0(1)'s 0x140ac1840[k], the previous-frame copy 0x140ac1220[k] (the effect pass's camera in
       * play, M) and a camera on the heap.  H2b must edit f_19cfc0(1)'s only, H2c rcx's only, each restored, nothing with its test
       * aid off, in pc or at 16:9; the no-cull aid divides by 2047/208. */
      { const int k=1; u64 cams[3]={CAM_CUR+CAM_SIZE*k,CAM_PREV+CAM_SIZE*k,ga_calloc(1,0x200)}; u32 si=rd32(g,CAM_IDX), sv0=rd32(g,cams[0]+CAM_M11), sv1=rd32(g,cams[1]+CAM_M11);
        u8 s2b[sizeof h2b], s2c[sizeof h2c]; memcpy(s2b,&h2b,sizeof h2b); memcpy(s2c,&h2c,sizeof h2c);
        u64 smc=atomic_load(&map_calls), s3d=atomic_load(&last3d_plus1); u32 smt=atomic_load(&map_tid); int sm2=kh_aspect_m, sob=asp_off_h2b, soc=asp_off_h2c; u64 rcx0=KH_RCX(g);
        kh_aspect_m=0; asp_off_h2b=asp_off_h2c=0; asp_nocull=0; h2b.warned=h2c.warned=1;
        const u32 v=0x3f9e0419u, vr=fbits((float)((double)bf(v)/1.25));
        #define SET3() do{ for(int q=0;q<3;q++) wr32(g,cams[q]+CAM_M11,v); h2w_calls=0; }while(0)
        wr32(g,CAM_IDX,(u32)k); for(int q=0;q<3;q++) h2w_cam[q]=cams[q];
        asp_mode=1; SCREEN(1920,1350);
        SET3(); KH_RCX(g)=cams[2]; u64 e0=h2b.edited; h2b_wrap(g,h2w_fn);
        ACHECK(h2w_calls==1&&h2w_seen[0]==vr&&h2w_seen[1]==v&&h2w_seen[2]==v&&rd32(g,cams[0]+CAM_M11)==v&&h2b.edited==e0+1);           /* H2b: f_19cfc0(1)'s camera only, restored */
        SET3(); KH_RCX(g)=cams[2]; e0=h2c.edited; h2c_wrap(g,h2w_fn);
        ACHECK(h2w_calls==1&&h2w_seen[2]==vr&&h2w_seen[0]==v&&h2w_seen[1]==v&&rd32(g,cams[2]+CAM_M11)==v&&h2c.edited==e0+1);           /* H2c: rcx's camera only */
        SET3(); KH_RCX(g)=cams[1]; h2c_wrap(g,h2w_fn); ACHECK(h2w_seen[1]==vr&&h2w_seen[0]==v&&rd32(g,cams[1]+CAM_M11)==v);           /* the previous-frame copy, as in play */
        asp_off_h2b=1; SET3(); u64 o0=h2b.off; h2b_wrap(g,h2w_fn); ACHECK(h2w_calls==1&&h2w_seen[0]==v&&h2b.off==o0+1); asp_off_h2b=0;  /* test aid off: nothing written */
        asp_off_h2c=1; SET3(); KH_RCX(g)=cams[2]; o0=h2c.off; h2c_wrap(g,h2w_fn); ACHECK(h2w_calls==1&&h2w_seen[2]==v&&h2c.off==o0+1); asp_off_h2c=0;
        asp_nocull=1; SET3(); h2b_wrap(g,h2w_fn); ACHECK(h2w_seen[0]==fbits((float)((double)bf(v)/(2047.0/208.0)))&&rd32(g,cams[0]+CAM_M11)==v); asp_nocull=0;
        asp_mode=0; SET3(); KH_RCX(g)=cams[2]; h2b_wrap(g,h2w_fn); ACHECK(h2w_calls==1&&h2w_seen[0]==v&&h2w_seen[1]==v&&h2w_seen[2]==v);   /* pc: each wrapper */
        h2c_wrap(g,h2w_fn); ACHECK(h2w_calls==2&&h2w_seen[0]==v&&h2w_seen[1]==v&&h2w_seen[2]==v); asp_mode=1;                           /* checked on its own */
        SCREEN(1920,1080); SET3(); KH_RCX(g)=cams[2]; h2b_wrap(g,h2w_fn); ACHECK(h2w_seen[0]==v); h2c_wrap(g,h2w_fn); ACHECK(h2w_seen[2]==v);       /* 16:9 */
        #undef SET3
        wr32(g,cams[0]+CAM_M11,sv0); wr32(g,cams[1]+CAM_M11,sv1); wr32(g,CAM_IDX,si); ga_free(cams[2]); KH_RCX(g)=rcx0;
        memcpy(&h2b,s2b,sizeof h2b); memcpy(&h2c,s2c,sizeof h2c); atomic_store(&map_calls,smc); atomic_store(&last3d_plus1,s3d); atomic_store(&map_tid,smt);
        kh_aspect_m=sm2; asp_off_h2b=sob; asp_off_h2c=soc; }
      /* M's return-address scan (task 40 review finding 4): the image's executable range from its PE header; a return address is a
       * value in it preceded by a call (0x1401704a1 = after f_170450's call of f_186920, 0x140166677 = after f_166640's f_126500) */
      { int ts=text_state; text_range(g); ACHECK(text_state==1&&text_lo<=GS_BASE+0x1121d0&&GS_BASE+0x1121d0<text_hi&&!(text_lo<=GS_BASE+0x5b24f8&&GS_BASE+0x5b24f8<text_hi));
        ACHECK(call_before(g,GS_BASE+0x1704a1)&&call_before(g,DK_RA_MODE));
        u64 b=ga_calloc(1,0x40); static const u8 forms[5][6]={{0,0,0,0,0,0},{0,0xe8,1,2,3,4},{0xff,0x15,1,2,3,4},{0,0,0,0,0xff,0xd0},{0,0,0,0xff,0x50,8}};
        for(int i=0;i<5;i++){ memcpy(hp(g,b+8),forms[i],6); ACHECK(call_before(g,b+14)==(i>0)); }
        memset(hp(g,b),0,0x40); u64 sp0=KH_RSP(g), st=ga_calloc(1,0x100); wr64(g,st,0x1234); wr64(g,st+8,GS_BASE+0x1704a1); wr64(g,st+16,GS_BASE+0x5b24f8); wr64(g,st+24,DK_RA_MODE);
        KH_RSP(g)=st; u64 ra[2]={0,0}; int nr=m_chain(g,ra,2); KH_RSP(g)=sp0; ACHECK(nr==2&&ra[0]==GS_BASE+0x1704a1&&ra[1]==DK_RA_MODE);
        ga_free(st); ga_free(b); if(!ts){ text_state=0; text_lo=text_hi=0; } }
      /* H4: the rect targets, then the seven objects, then the hooks on the lifted f_156e20 and f_156f10 */
      { s16 y,h; h4_target(2,52,-1,0x1a2,&y,&h); ACHECK(y==-53&&h==522); h4_target(1,52,-1,0x24,&y,&h); ACHECK(y==-53&&h==36);
        h4_target(0,52,0x17d,0x24,&y,&h); ACHECK(y==0x17d+52&&h==36); h4_target(2,53,-1,0x1a2,&y,&h); ACHECK(y==-54&&h==524);
        for(int k=0;k<3;k++){ h4_target(k,0,k?-1:0x17d,0x24,&y,&h); ACHECK(y==(k?-1:0x17d)&&h==0x24); }
        u8 osave[7][0x120]; for(int i=0;i<7;i++) memcpy(osave[i],hp(g,h4o[i].obj),0x120);
        for(int i=0;i<7;i++){ const H4Obj *o=&h4o[i]; wr16(g,o->obj+0xe,(u16)o->y0); wr16(g,o->obj+0xa,(u16)o->h0);
            ACHECK(h4_apply(g,i,0)==0&&(s16)rd16(g,o->obj+0xe)==o->y0&&(s16)rd16(g,o->obj+0xa)==o->h0);          /* B = 0: nothing written */
            s16 ty,th; h4_target(o->kind,52,o->y0,o->h0,&ty,&th);
            ACHECK(h4_apply(g,i,52)==1&&(s16)rd16(g,o->obj+0xe)==ty&&(s16)rd16(g,o->obj+0xa)==th);
            ACHECK(h4_apply(g,i,52)==2&&(s16)rd16(g,o->obj+0xe)==ty);                                            /* already extended */
            wr16(g,o->obj+0xe,7); ACHECK(h4_apply(g,i,52)==3&&(s16)rd16(g,o->obj+0xe)==7&&(s16)rd16(g,o->obj+0xa)==th); }   /* another rect: left */
        for(int i=0;i<7;i++){ memcpy(hp(g,h4o[i].obj),osave[i],0x120); h4s[i].no=0; }
        /* the lifted f_156e20 (it only sets the four bar objects): x and w are the exe's in every mode, y and h per screen and mode */
        s16 px[4],pw[4];
        static const int scr[][2]={{1920,1080},{1920,1350},{2670,1878},{2007,2853},{2560,1080}};
        for(int sc=0;sc<5;sc++) for(int mode=0;mode<2;mode++){ SCREEN(scr[sc][0],scr[sc][1]); asp_mode=mode; int B=kh_aspect_B(scr[sc][0],scr[sc][1]);
            for(int i=3;i<7;i++){ wr16(g,h4o[i].obj+0xe,0x5555); wr16(g,h4o[i].obj+0xa,0x5555); }
            int ok=0; st_call(g,f_156e20,NULL,0,&ok); ACHECK(ok);
            for(int i=3;i<7;i++){ const H4Obj *o=&h4o[i]; s16 ty=o->y0,th=o->h0; if(mode) ty+=(o->kind==1 ? -B : B);
                ACHECK((s16)rd16(g,o->obj+0xe)==ty&&(s16)rd16(g,o->obj+0xa)==th);
                if(sc==0&&mode==0){ px[i-3]=(s16)rd16(g,o->obj+0xc); pw[i-3]=(s16)rd16(g,o->obj+0x8); }
                else ACHECK((s16)rd16(g,o->obj+0xc)==px[i-3]&&(s16)rd16(g,o->obj+0x8)==pw[i-3]); }
            if(sc==1&&mode==1) ACHECK((s16)rd16(g,h4o[3].obj+0xe)==-53&&(s16)rd16(g,h4o[3].obj+0xa)==36&&(s16)rd16(g,h4o[5].obj+0xe)==433&&(s16)rd16(g,h4o[5].obj+0xa)==36); }
        ACHECK(pw[0]==2*((s16)~px[0]+0x101)&&pw[1]==pw[0]&&px[1]==px[0]);                                          /* x = ~m, w = 2 (m + 0x101) for the exe's margin m (here from the selftest's GX) */
        for(int i=3;i<7;i++) memcpy(hp(g,h4o[i].obj),osave[i],0x120);
        /* the lifted f_156f10 with the object's state word +0x108 = 0 (it returns before drawing): the fade rect at its entry */
        for(int i=0;i<7;i++) for(int sc=0;sc<5;sc++) for(int mode=0;mode<2;mode++){ const H4Obj *o=&h4o[i]; SCREEN(scr[sc][0],scr[sc][1]); asp_mode=mode;
            wr32(g,o->obj+0x108,0); wr16(g,o->obj+0xe,(u16)o->y0); wr16(g,o->obj+0xa,(u16)o->h0); int ok=0; st_call(g,f_156f10,(u64[]){o->obj},1,&ok);
            s16 ty=o->y0,th=o->h0; if(mode){ int B=kh_aspect_B(scr[sc][0],scr[sc][1]); ty+=o->kind==0 ? B : -B; if(o->kind==2) th+=2*B; }
            ACHECK(ok&&(s16)rd16(g,o->obj+0xe)==ty&&(s16)rd16(g,o->obj+0xa)==th);
            if(i<3&&sc==1&&mode==1) ACHECK(ty==-53&&th==522); }
        /* Substitute the game's position setter for the virtual draw: x becomes zero exactly on a forced draw. */
        u64 event_slot=GS_BASE+0x2a11478ull, saved_event=rd64(g,event_slot), vt=ga_calloc(1,16);
        wr64(g,vt+8,GS_BASE+0x160cd0ull);
        for(int pass=0;pass<5;pass++) for(int i=0;i<7;i++){
            const u64 obj=h4o[i].obj;
            SCREEN(1920,pass==4?1080:1350); asp_mode=1; test_event_bars=pass!=0;
            wr64(g,event_slot,pass==2?0:obj); wr64(g,obj,vt); wr32(g,obj+0x108,0);
            wr32(g,obj+0x114,pass==3); wr16(g,obj+0xc,123); wr32(g,obj+0x14,0x12345678);
            int ok=0; st_call(g,f_156f10,(u64[]){obj},1,&ok);
            ACHECK(ok&&rd16(g,obj+0xc)==(((pass==1||pass==3)&&(i==3||i==5))?0:123));
            ACHECK(rd32(g,obj+0x14)==0x12345678&&rd32(g,obj+0x108)==0);
        }
        test_event_bars=0; wr64(g,event_slot,saved_event); ga_free(vt);
        for(int i=0;i<7;i++){ memcpy(hp(g,h4o[i].obj),osave[i],0x120); h4s[i].draws=h4s[i].rewritten=h4s[i].already=h4s[i].other=h4s[i].setter_rewritten=h4s[i].setter_other=0; h4s[i].no=0; }
        h4_e20_calls=h4_identity=0; }
      /* H5: the targets, then the lifted f_126fd0 (it stores x, y, w, h at GX+0xf4 / +0xf8 / +0xfc / +0x100) */
      { s32 ny,nh; ACHECK(h5_target(52,0,416,&ny,&nh)==1&&ny==-52&&nh==520); ACHECK(h5_target(53,0,416,&ny,&nh)==1&&ny==-53&&nh==522);
        ACHECK(!h5_target(0,0,416,&ny,&nh)&&!h5_target(52,0,415,&ny,&nh)&&!h5_target(52,2,416,&ny,&nh)&&!h5_target(52,0,64,&ny,&nh));
        u8 ssave[0x14]; memcpy(ssave,hp(g,GX+0xf4),0x14);
        static const s32 rect[3][4]={{-86,0,684,416},{-86,0,362,64},{0,0,256,416}};
        static const int scr[3][2]={{1920,1080},{1920,1350},{2670,1878}};
        for(int sc=0;sc<3;sc++) for(int mode=0;mode<2;mode++) for(int k=0;k<3;k++){ SCREEN(scr[sc][0],scr[sc][1]); asp_mode=mode; int B=kh_aspect_B(scr[sc][0],scr[sc][1]);
            int ok=0; st_call(g,f_126fd0,(u64[]){(u64)(u32)rect[k][0],(u64)(u32)rect[k][1],(u64)(u32)rect[k][2],(u64)(u32)rect[k][3]},4,&ok);
            int full=mode&&B&&rect[k][1]==0&&rect[k][3]==416;
            ACHECK(ok&&(s32)rd32(g,GX+0xf4)==rect[k][0]&&(s32)rd32(g,GX+0xfc)==rect[k][2]&&rd32(g,GX+0x104)==1
                   &&(s32)rd32(g,GX+0xf8)==(full?-B:rect[k][1])&&(s32)rd32(g,GX+0x100)==(full?416+2*B:rect[k][3])); }
        memcpy(hp(g,GX+0xf4),ssave,0x14); h5_calls=h5_ext=h5_other=h5_identity=0; }
      #undef SCREEN
      asp_mode=sm; asp_nocull=snc; asp_off_h4=so4; asp_off_h5=so5; h2b.calls=h2b.identity=h2b.edited=0; h2c.calls=h2c.identity=h2c.edited=0;
      wr32(g,GX_GUARD,guard); memcpy(hp(g,GX+0x10),gxsave,32); }
    /* -- task 42: H3 -- */
    h3_selftest(g);
    /* -- task 49: the zoom -- */
    zoom_selftest(g);
    /* -- M: the RTTI names, the frame intervals, the null backend's vertex-shader kinds -- */
    { char nm[72]; rtti_name(g,GS_BASE+0x5b24f8ull,nm,sizeof nm); ACHECK(!strcmp(nm,".?AVLockOn@dk@@"));
      rtti_name(g,GS_BASE+0x5b1c28ull,nm,sizeof nm); ACHECK(!strcmp(nm,".?AVCOMMAND_DRAW@dk@@"));
      rtti_name(g,0x10,nm,sizeof nm); ACHECK(!strncmp(nm,"(vtable outside",15));
      Ivs v; memset(&v,0,sizeof v); iv_add(&v,10); iv_add(&v,11); iv_add(&v,40); iv_add(&v,100); ACHECK(v.n==2&&v.v[0].a==10&&v.v[0].b==40&&v.v[1].a==100&&v.v[1].b==100);
      for(int i=0;i<NIV+3;i++) iv_add(&v,1000+100*(u32)i); ACHECK(v.n==NIV&&v.cut==5&&v.v[NIV-1].b==1000+100*(NIV+2));
      /* iv_merge (the M group lines): the union in order, joined; the default route's COMMAND_ONE instances 930-6999 and 414 / 557 */
      { Ivs a,b; memset(&a,0,sizeof a); memset(&b,0,sizeof b); a.v[0]=(Iv){930,6999}; a.n=1; b.v[0]=(Iv){414,414}; b.v[1]=(Iv){557,557}; b.n=2;
        iv_merge(&a,&b); ACHECK(a.n==3&&a.cut==0&&a.v[0].a==414&&a.v[0].b==414&&a.v[1].a==557&&a.v[1].b==557&&a.v[2].a==930&&a.v[2].b==6999);
        memset(&a,0,sizeof a); memset(&b,0,sizeof b); a.v[0]=(Iv){100,200}; a.n=1; b.v[0]=(Iv){150,400}; b.v[1]=(Iv){420,500}; b.n=2;
        iv_merge(&a,&b); ACHECK(a.n==1&&a.v[0].a==100&&a.v[0].b==500);                                   /* overlapping and a 20-frame gap: one */
        memset(&a,0,sizeof a); memset(&b,0,sizeof b); for(int i=0;i<NIV;i++){ a.v[i]=(Iv){(u32)(1000*i),(u32)(1000*i+10)}; b.v[i]=(Iv){(u32)(1000*i+500),(u32)(1000*i+510)}; } a.n=b.n=NIV;
        iv_merge(&a,&b); ACHECK(a.n==NIV&&a.cut==NIV&&a.v[0].a==0&&a.v[1].a==500&&a.v[NIV-1].b==1000*(NIV-1)+510); }   /* overflow: counted, joined into the last */
      /* the occlusion-volume record (task 40 review finding 5): 16 volumes fill rctx+0x508, a 17th is beyond it; through m_occ with a
       * stand-in rctx at [0x140abd440]: the first map pass only notes its present, the next one samples the count for that present */
      { OccSt sv=occ; memset(&occ,0,sizeof occ);
        occ_note(&occ,7,7,16,0); ACHECK(occ.n==1&&occ.hist[16]==1&&occ.over==0&&occ.max==16&&occ.maxp==7&&occ.nat==0);
        occ_note(&occ,8,8,17,1); ACHECK(occ.n==2&&occ.hist[OCC_SLOTS+1]==1&&occ.over==1&&occ.max==17&&occ.maxp==8&&occ.frover.n==1&&occ.nat==1&&occ.atp[0]==8&&occ.atn[0]==17);
        memset(&occ,0,sizeof occ); u64 srp=rd64(g,RCTX_GLOBAL), rc=ga_calloc(1,0xa40), spr=atomic_load(&gfx_presents); int sdn=dl_n; dl_n=0; wr64(g,RCTX_GLOBAL,rc);
        atomic_store(&gfx_presents,99); wr32(g,rc+0xa08,5); m_occ(g); ACHECK(occ.n==0&&occ.prevp==100);   /* the map pass of present 100 */
        atomic_store(&gfx_presents,100); wr32(g,rc+0xa08,3); m_occ(g); ACHECK(occ.n==1&&occ.hist[3]==1&&occ.max==3&&occ.maxp==100&&occ.prevp==101);   /* present 100 left 3 */
        u64 rc2=ga_calloc(1,0xa40); wr32(g,rc2+0xa08,0xffffffffu); wr64(g,RCTX_GLOBAL,rc2); m_occ(g);
        ACHECK(occ.n==1&&occ.newrc==1&&occ.over==0);                                                       /* a new rctx: not a count f_185930 left */
        m_occ(g); ACHECK(occ.n==2&&occ.over==1&&occ.nov==1&&occ.ovn[0]==0xffffffffu);                       /* the same rctx again: sampled */
        wr64(g,RCTX_GLOBAL,0); m_occ(g); ACHECK(occ.n==2&&occ.norctx==1);
        wr64(g,RCTX_GLOBAL,srp); atomic_store(&gfx_presents,spr); dl_n=sdn; ga_free(rc); ga_free(rc2); occ=sv; }
      extern int kh_null_m_vs_kind_test(KCtx *,u64,u64);
      static const u32 rv[8]={0x586160,0x586710,0x586c80,0x5871b0,0x593b10,0x59ac40,0x589080,0x5987d0}; static const int kv[8]={VK_586160,VK_586710,VK_586c80,VK_5871b0,VK_593b10,VK_59ac40,VK_3D,VK_3D};
      for(int i=0;i<8;i++) ACHECK(kh_null_m_vs_kind_test(g,GS_BASE+rv[i],rd32(g,GS_BASE+rv[i]+24))==kv[i]);
      u64 cp=ga_calloc(1,0x600); memcpy(hp(g,cp),hp(g,GS_BASE+0x586160),0x5a8); ACHECK(kh_null_m_vs_kind_test(g,cp,0x5a8)==VK_586160);
      wr8(g,cp+9,rd8(g,cp+9)^1); ACHECK(kh_null_m_vs_kind_test(g,cp,0x5a8)==VK_UNKNOWN); ga_free(cp); }
    /* -- the built size against KHNMC_SCREEN (screen-not-applied, task 37 review) -- */
    { int old[8]; const int on[8]={1,1920,1350,1920,1350,0,0,1}, off[8]={0,1920,1080,1280,720,100,100,0};
      u8 gxsave[0x18]; memcpy(gxsave,hp(g,GX+0x8),0x18); u64 p0=atomic_load(&scr_chk_presents), b0=atomic_load(&scr_chk_bad);
      kh_screen_test_state(on,old); gx_sizes(g,1920,1350,1920,1350);
      ACHECK(kh_screen_check_present(g,1920,1350,1)==0&&atomic_load(&scr_chk_presents)==p0+1&&atomic_load(&scr_chk_bad)==b0);   /* built as the screen */
      kh_screen_test_state(off,NULL); gx_sizes(g,1920,1350,1920,1350);
      ACHECK(kh_screen_check_present(g,1280,720,2)==0&&atomic_load(&scr_chk_presents)==p0+1&&atomic_load(&scr_chk_bad)==b0);    /* no KHNMC_SCREEN: nothing checked */
      kh_screen_test_state(old,NULL); memcpy(hp(g,GX+0x8),gxsave,0x18); atomic_store(&scr_chk_presents,p0); atomic_store(&scr_chk_bad,b0);
      if(kh_fork_ok()){ char out[8192]; int rc=check_child(g,out,sizeof out);
          ACHECK(rc==0);                                                                                                 /* 3 mismatches, 1 match */
          ACHECK(strstr(out,"kh: screen: WARNING: screen-not-applied: KHNMC_SCREEN=1920x1350, but at present 11 ")!=NULL
                 &&strstr(out,"swap chain 1920x1080 (NOT the screen)")!=NULL);                                           /* the first, named */
          ACHECK(strstr(out,"but at present 12")==NULL&&strstr(out,"but at present 13")==NULL);                         /* printed once */
          ACHECK(strstr(out,"the built size checked at 4 presents (swap chain, GX+0x8, GX+0x18): not 1920x1350 at 3\n")!=NULL);   /* counted */
          ACHECK(strstr(out,"kh: screen: WARNING: screen-not-applied: at 3 of 4 presents")!=NULL);                       /* the exit report says so */
          if(rc) fprintf(stderr,"kh: aspect selftest: built-size check child: exit %d; it said: %.600s\n",rc,out); }
      else kh_selftest_not_run("aspect","the screen-not-applied cases (no child process)",5); }
    /* -- the named fatals -- */
    if(kh_fork_ok()){
        ACHECK(fatal_case(0,"screen-value-malformed: KHNMC_SCREEN=1920:")); ACHECK(fatal_case(1,"screen-value-malformed: KHNMC_SCREEN=1920x1350x2"));
        ACHECK(fatal_case(2,"screen-value-range: KHNMC_SCREEN=8x8")); ACHECK(fatal_case(3,"screen-and-kh-win:"));
        ACHECK(fatal_case(4,"aspect-mode-unknown: KHNMC_ASPECT=wide")); ACHECK(fatal_case(5,"aspect-log-value: KHNMC_ASPECT_LOG=yes"));
        ACHECK(fatal_case(6,"aspect-test-h1-value: KHNMC_ASPECT_TEST_H1=always")); ACHECK(fatal_case(7,"aspect-test-h1-needs-fill:"));
        ACHECK(fatal_case(8,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=h2,h6: 'h6' is not one of h2, h2b, h2c, h3, h4, h5")); ACHECK(fatal_case(9,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_OFF=h2"));
        ACHECK(fatal_case(10,"aspect-test-nocull-value: KHNMC_ASPECT_TEST_NOCULL=yes")); ACHECK(fatal_case(11,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_NOCULL=1"));
        ACHECK(fatal_case(12,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=h4, ends with a comma")); ACHECK(fatal_case(13,"aspect-log-value: KHNMC_ASPECT_LOG=3"));
        ACHECK(fatal_case(14,"aspect-m-drawlog-needs-m: KHNMC_ASPECT_M_DRAWLOG=x.tsv@1-2")); ACHECK(fatal_case(15,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=x.tsv@5-3"));
        ACHECK(fatal_case(16,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=x.tsv@1-2,")); ACHECK(fatal_case(17,"aspect-m-drawlog-open: KHNMC_ASPECT_M_DRAWLOG: cannot create /nonexistent-khnmc-dir/x.tsv"));
        ACHECK(fatal_case(18,"fov-zoom-value-range: KHNMC_FOV_ZOOM=2.5 ")); ACHECK(fatal_case(19,"fov-zoom-value-range: KHNMC_FOV_ZOOM=0.99 "));   /* task 49 */
        ACHECK(fatal_case(20,"fov-zoom-value-malformed: KHNMC_FOV_ZOOM=1,15 ")); ACHECK(fatal_case(21,"fov-zoom-value-malformed: KHNMC_FOV_ZOOM=1.15x "));
        ACHECK(fatal_case(22,"kh: aspect: zoom (task 49): 1.15 on a screen taller than wide (KHNMC_FOV_ZOOM)"));
        ACHECK(fatal_case(23,"kh: aspect: NOTE: fov-zoom-not-applied: KHNMC_FOV_ZOOM=1.15 is set, but KHNMC_ASPECT is pc"));
        ACHECK(fatal_case(24,"kh: aspect: zoom (task 49): 1.3 on a screen taller than wide (the default"));
        ACHECK(fatal_case(25,"hud-scale-value-range: KHNMC_HUD_SCALE_CMD=3.5 ")); ACHECK(fatal_case(26,"hud-scale-value-range: KHNMC_HUD_SCALE_HP=0.4 "));   /* task 53 */
        ACHECK(fatal_case(27,"hud-scale-value-malformed: KHNMC_HUD_SCALE_CMD=1,25 ")); ACHECK(fatal_case(28,"hud-scale-value-malformed: KHNMC_HUD_SCALE_HP=1.5x "));
        ACHECK(fatal_case(29,"kh: aspect: HUD scale (task 53): command menu 2 (KHNMC_HUD_SCALE_CMD), player gauges 1 (KHNMC_HUD_SCALE_HP) on a screen taller than wide"));
        ACHECK(fatal_case(30,"kh: aspect: NOTE: hud-scale-not-applied: KHNMC_HUD_SCALE_CMD is set, but KHNMC_ASPECT is pc"));
        ACHECK(fatal_case(31,"kh: aspect: HUD scale (task 53): command menu 1.25 (default), player gauges 1.5 (default) on a screen taller than wide"));
        ACHECK(fatal_case(32,"title-layout-value: KHNMC_TITLE_LAYOUT=wide (known: portrait, band)"));          /* task 53 item 2 */
        ACHECK(fatal_case(33,"kh: aspect: NOTE: title-layout-not-applied: KHNMC_TITLE_LAYOUT=band is set, but KHNMC_ASPECT is pc"));
        ACHECK(fatal_case(34,"kh: aspect: title layout (task 53): band (today's placement) on a screen taller than wide (KHNMC_TITLE_LAYOUT)"));
        ACHECK(fatal_case(36,"kh: aspect: title layout (task 53): portrait on a screen taller than wide (the default"));
        ACHECK(fatal_case(37,"fov-zoom-value-range: KHNMC_FOV_ZOOM=2.0 (allowed: 1.0 to 1.89"));   /* task 49 fix: finding 3 range */ }
    else kh_selftest_not_run("aspect","the named fatals and value cases of KHNMC_SCREEN / KHNMC_ASPECT* / KHNMC_FOV_ZOOM / KHNMC_HUD_SCALE_* / KHNMC_TITLE_LAYOUT (no child process)",37);
    asp_off_h7=0; h7_selftest(g); asp_off_h7=s_h7;
    fprintf(stderr,"kh: aspect selftest: %d checks, %d failures\n",schecks,sfails);
    return sfails;
}
