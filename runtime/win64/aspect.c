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
 *
 * H1 (post-call hook on f_11c1e0, plan section 4.3).  f_11c1e0(out) is the exe's "largest 16:9 rectangle in the screen"
 * (shard_0022.c:26728): s = minss(1.0, GX+0x28); W' = GX+0x18 * s, H' = GX+0x1c * s (floats); "narrow" when 9W' < 16H' (comiss; an
 * unordered compare also counts as narrow); narrow: (W', 9W' * 0.0625), otherwise (16H' / 9, H'); then x *= GX+0x20, y *= GX+0x24;
 * it returns out in rax.  Its callers draw black bars of (GX+0x10 - result) / 2 around it: f_14e050 (shard_0026.c:36792, after the
 * fades / overlay of 0x156940 -> 0x14e020) and the settings-menu copy f_4ed940 (shard_0105.c:38419).  On a narrow screen H1 replaces y
 * by H' * GX+0x24, the value of the exe's own not-narrow branch: the bars get zero height.  x is never changed; at 16:9 or wider the
 * exe's y already is that value, so H1 is an identity there.  H1 acts when fill is on, the screen is narrow, and the 3D view was drawn
 * in this game frame or the previous one (frames without a 3D view, such as the title and full-screen menus, keep the exe's bars);
 * KHNMC_ASPECT_TEST_H1=unconditional drops that last condition.  Each call is checked against a replica of the exe's arithmetic
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
void kh_aspect_config(const char *mode,const char *log,const char *test_h1){
    if(mode&&*mode){ if(!strcmp(mode,"pc")) asp_mode=0; else if(!strcmp(mode,"fill")) asp_mode=1;
        else kh_fatal(NULL,"aspect-mode-unknown: KHNMC_ASPECT=%s (known: pc, fill)",mode); }
    if(log&&*log){ if(!strcmp(log,"1")) kh_aspect_log=1; else if(!strcmp(log,"0")) kh_aspect_log=0;
        else kh_fatal(NULL,"aspect-log-value: KHNMC_ASPECT_LOG=%s (expected 1 or 0)",log); }
    if(test_h1&&*test_h1){
        if(strcmp(test_h1,"unconditional")) kh_fatal(NULL,"aspect-test-h1-value: KHNMC_ASPECT_TEST_H1=%s (the only value is unconditional, a test aid)",test_h1);
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-h1-needs-fill: KHNMC_ASPECT_TEST_H1=unconditional changes H1, which runs only with KHNMC_ASPECT=fill");
        asp_h1_uncond=1; }
    if(asp_mode||kh_aspect_log)
        fprintf(stderr,"kh: aspect: KHNMC_ASPECT=%s%s%s\n",asp_mode?"fill (H1: no 16:9 bars on a screen narrower than 16:9":"pc",
                asp_mode?(asp_h1_uncond?", UNCONDITIONAL: every frame, KHNMC_ASPECT_TEST_H1 test aid)":", on frames with the 3D view)"):"",
                kh_aspect_log?"; KHNMC_ASPECT_LOG=1: size records on":"");
}
void kh_aspect_init(void){ kh_aspect_config(getenv("KHNMC_ASPECT"),getenv("KHNMC_ASPECT_LOG"),getenv("KHNMC_ASPECT_TEST_H1")); }

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
void orig_f_186920(KCtx *);
void f_186920(KCtx *g){
    if(__builtin_expect(asp_mode||kh_aspect_log,0)){ map_calls++; u32 z=0; atomic_compare_exchange_strong(&map_tid,&z,cur_tid()); note_3d(kh_game_frames()); }
    orig_f_186920(g);
}

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
    if(asp_h1_uncond||gate_open_d(d)){
        wrf(g,out+4,m.yfull); h1_rewritten++;
        if(kh_aspect_log&&!atomic_exchange(&h1_logged_rewrite,1))
            fprintf(stderr,"kh: aspect: H1: first rewrite: %g x %g -> %g x %g (%s) at game frame %llu, present %llu\n",ox,oy,ox,m.yfull,asp_h1_uncond?"unconditional":d<0?"?":d==0?"3D view drawn in this frame":"3D view drawn in the previous frame",P(now),P(gfx_presents));
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

/* exit report (runtime/seams.c), only when a switch is set */
void kh_aspect_report(FILE *f){
    if(!asp_mode&&!kh_aspect_log) return;
    fprintf(f,"kh: aspect report: KHNMC_ASPECT=%s%s; H1 (0x11c1e0): calls %llu, not narrow %llu, narrow %llu (rewritten to the full height %llu, kept: no 3D view in this frame or the previous one %llu), "
              "result different from the model %llu; narrow calls by game frames since the map pass: same frame %llu, previous frame %llu, 2 or more frames earlier %llu, never %llu; map pass (0x186920) calls %llu; first guest threads: H1 %u, map pass %u\n",
            asp_mode?"fill":"pc",asp_h1_uncond?" + KHNMC_ASPECT_TEST_H1=unconditional":"",P(h1_calls),P(h1_wide),P(h1_narrow),P(h1_rewritten),P(h1_kept),P(h1_mismatch),
            P(h1_since[0]),P(h1_since[1]),P(h1_since[2]),P(h1_since[3]),P(map_calls),h1_tid,map_tid);
    if(h1_mismatch) fprintf(f,"kh: aspect: WARNING: %llu H1 calls were left as the exe made them because its result differed from the model\n",P(h1_mismatch));
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
        ACHECK(r==(u64)expect&&(!expect||(rd32(g,m+0x6c)==(u32)sw&&rd32(g,m+0x70)==(u32)sh))); }
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
int kh_aspect_selftest(KCtx *g){
    sfails=schecks=0;
    /* -- r, o, b (plan section 3) -- */
    static const int same[4][2]={{1920,1080},{2560,1440},{3840,2160},{2560,1080}};
    for(int i=0;i<4;i++){ double W=same[i][0],H=same[i][1]; ACHECK(kh_aspect_r(W,H)==1.0&&kh_aspect_o(W,H)==0.0&&kh_aspect_b(W,H)==0.0); }
    ACHECK(kh_aspect_r(1920,1350)==1.25&&kh_aspect_o(1920,1350)==135.0&&kh_aspect_b(1920,1350)==52.0);
    static const int phone[4][2]={{2670,1878},{2225,1565},{1780,1252},{1335,939}};          /* native and the 5/6, 2/3, 1/2 sizes */
    static const double phone_o[4]={188.0625,156.71875,125.375,94.03125};
    for(int i=0;i<4;i++){ double W=phone[i][0],H=phone[i][1];
        ACHECK(near(kh_aspect_r(W,H),1.250437,5e-7)&&kh_aspect_o(W,H)==phone_o[i]&&near(kh_aspect_b(W,H),52.0909,5e-5)); }
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
        ACHECK(fatal_case(6,"aspect-test-h1-value: KHNMC_ASPECT_TEST_H1=always")); ACHECK(fatal_case(7,"aspect-test-h1-needs-fill:")); }
    else kh_selftest_not_run("aspect","the 8 named fatals of KHNMC_SCREEN / KHNMC_ASPECT* (no child process)",8);
    fprintf(stderr,"kh: aspect selftest: %d checks, %d failures\n",schecks,sfails);
    return sfails;
}
