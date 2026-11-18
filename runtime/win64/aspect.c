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
int kh_aspect_m;                           /* task 40: KHNMC_ASPECT_LOG=2, the measurement M (also read by runtime/null_d3d12.c, bgload.c) */
void kh_aspect_config(const char *mode,const char *log,const char *test_h1){
    if(mode&&*mode){ if(!strcmp(mode,"pc")) asp_mode=0; else if(!strcmp(mode,"fill")) asp_mode=1;
        else kh_fatal(NULL,"aspect-mode-unknown: KHNMC_ASPECT=%s (known: pc, fill)",mode); }
    if(log&&*log){ if(!strcmp(log,"1")) kh_aspect_log=1; else if(!strcmp(log,"2")) kh_aspect_log=kh_aspect_m=1; else if(!strcmp(log,"0")) kh_aspect_log=0;
        else kh_fatal(NULL,"aspect-log-value: KHNMC_ASPECT_LOG=%s (expected 2, 1 or 0)",log); }
    if(test_h1&&*test_h1){
        if(strcmp(test_h1,"unconditional")) kh_fatal(NULL,"aspect-test-h1-value: KHNMC_ASPECT_TEST_H1=%s (the only value is unconditional, a test aid)",test_h1);
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-h1-needs-fill: KHNMC_ASPECT_TEST_H1=unconditional changes H1, which runs only with KHNMC_ASPECT=fill");
        asp_h1_uncond=1; }
    if(asp_mode||kh_aspect_log)
        fprintf(stderr,"kh: aspect: KHNMC_ASPECT=%s%s%s\n",asp_mode?"fill (H1: no 16:9 bars on a screen narrower than 16:9":"pc",
                asp_mode?(asp_h1_uncond?", UNCONDITIONAL: every frame, KHNMC_ASPECT_TEST_H1 test aid)":", on frames with the 3D view)"):"",
                kh_aspect_m?"; KHNMC_ASPECT_LOG=2: size records and the measurement M on":kh_aspect_log?"; KHNMC_ASPECT_LOG=1: size records on":"");
}
/* task 40 test aids (plan step 5 / 7 tests; both need KHNMC_ASPECT=fill, any other value is a named fatal):
 *   KHNMC_ASPECT_TEST_OFF=list   comma-separated h2 (= h2b,h2c), h2b, h2c, h4, h5: those fill hooks pass straight through (the picture
 *                                "without its change"); the others stay on.
 *   KHNMC_ASPECT_TEST_NOCULL=1   H2b / H2c divide m11 by 2047/208 (the exe's guard band) instead of r: the "no-cull reference" of
 *                                plan step 5 (nothing is culled vertically inside the guard band; the tall picture must equal it). */
static int asp_off_h2b, asp_off_h2c, asp_off_h4, asp_off_h5, asp_nocull;
void kh_aspect_test_config(const char *off,const char *nocull){
    if(off&&*off){
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_OFF=%s turns fill hooks off, which run only with KHNMC_ASPECT=fill",off);
        const char *p=off;
        while(*p){ size_t n=strcspn(p,",");
            if(n==2&&!strncmp(p,"h2",2)) asp_off_h2b=asp_off_h2c=1; else if(n==3&&!strncmp(p,"h2b",3)) asp_off_h2b=1; else if(n==3&&!strncmp(p,"h2c",3)) asp_off_h2c=1;
            else if(n==2&&!strncmp(p,"h4",2)) asp_off_h4=1; else if(n==2&&!strncmp(p,"h5",2)) asp_off_h5=1;
            else kh_fatal(NULL,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=%s: '%.*s' is not one of h2, h2b, h2c, h4, h5 (comma-separated)",off,(int)n,p);
            p+=n; if(*p==','){ p++; if(!*p) kh_fatal(NULL,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=%s ends with a comma",off); } } }
    if(nocull&&*nocull){
        if(strcmp(nocull,"1")) kh_fatal(NULL,"aspect-test-nocull-value: KHNMC_ASPECT_TEST_NOCULL=%s (the only value is 1, a test aid)",nocull);
        if(asp_mode!=1) kh_fatal(NULL,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_NOCULL=1 changes H2, which runs only with KHNMC_ASPECT=fill");
        asp_nocull=1; }
    if(asp_off_h2b||asp_off_h2c||asp_off_h4||asp_off_h5||asp_nocull)
        fprintf(stderr,"kh: aspect: TEST AID: %s%s%s%s%s(not for play)\n",asp_off_h2b?"H2b off; ":"",asp_off_h2c?"H2c off; ":"",asp_off_h4?"H4 off; ":"",asp_off_h5?"H5 off; ":"",
                asp_nocull?"H2 divides m11 by 2047/208 instead of r (the no-cull reference); ":"");
}
void kh_aspect_drawlog_config(const char *v);
void kh_aspect_init(void){ kh_aspect_config(getenv("KHNMC_ASPECT"),getenv("KHNMC_ASPECT_LOG"),getenv("KHNMC_ASPECT_TEST_H1"));
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
 * H4 (plan section 5 "full-screen fades", "event letterbox bars"; the setter offsets [R] verified here): f_160cc0(obj, w, h) stores
 *   s16 w, h at obj+0x8 / +0xa and f_160cd0(obj, x, y) s16 x, y at obj+0xc / +0xe (shard_0028.c:1516 / :1538).  The three fades
 *   (f_156940 -> object 0x140abb3b0, f_1569b0 -> 0x140abb720, f_156a70 -> 0x140abb290) set y = -1, h = 0x1a2 and then DRAW at once:
 *   each ends in f_156f10(obj), whose last step is the object's vfn1 (shard_0027.c L15701f), so a hook after them would come one
 *   frame late and be overwritten.  H4a therefore sits at the entry of f_156f10 and acts on those three objects only when they hold
 *   the exe's rect (y = -1, h = 0x1a2): y = -1 - B, h = 0x1a2 + 2B.  The event letterbox bars: f_156e20 only SETS four objects (top
 *   0x140abb170 and 0x140abb600: y = -1, h = 0x24; bottom 0x140abb050 and 0x140abb4e0: y = 0x17d, h = 0x24; its last setter is a
 *   tail call, so no return address names it) and draws nothing; H4b runs after it: top bars y = -1 - B, h = 0x24 + B (inner edge
 *   unchanged), bottom bars h = 0x24 + B (y unchanged).  H4a also applies the bar rewrite at their draw (f_156f10), in case another
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
static void h2_around(KCtx *g,u64 cam,void (*fn)(KCtx *),H2Stat *st){
    st->calls++;
    double r=gx_r(g);
    if(r==1.0){ st->identity++; fn(g); return; }                                   /* 16:9 or wider: nothing is written */
    u64 a=cam+CAM_M11;
    if(!cam||!GA_OK(a)||!GA_OK(a+3)){ st->badcam++;
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
/* M only: f_19d040 = `lea rax,[rcx+0x84]`, the accessor every cam+0x84 reader calls */
void f_19d040(KCtx *g){
    if(__builtin_expect(kh_aspect_m,0)) m_cam(g,rd64(g,KH_RSP(g)),KH_RCX(g));
    orig_f_19d040(g);
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
    if(kind==2){ *y=(s16)(y0-B); *h=(s16)(h0+2*B); } else if(kind==1){ *y=(s16)(y0-B); *h=(s16)(h0+B); } else { *y=y0; *h=(s16)(h0+B); } }
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

/* ---- M: the dk draw loop and the draws ----------------------------------------------------------------------------------------- */
enum { VK_NONE, VK_586160, VK_586710, VK_586c80, VK_5871b0, VK_593b10, VK_59ac40, VK_3D, VK_UNKNOWN, VK_N };
static const char *vk_name[VK_N]={"no-pso","586160","586710","586c80","5871b0","593b10","59ac40","3d","unknown-vs"};
static int vk_ms(int k){ return k==VK_586160||k==VK_586710||k==VK_586c80; }      /* the VS that read MagicScale.zw (plan section 5) */
/* yreach / yout: MagicScale draws whose GS y range reaches beyond the 16:9 frame 0..416 / lies wholly beyond it, and when (task 40
 * review finding 4: dk::COMMAND_ONE's range 290..637 runs past the screen; these say how many draws and in which frames) */
typedef struct { u64 vt; char name[72]; u64 seen, skipped, first, nested, draws[VK_N], ydraws, ycb, ystg, yreach, yout; int pmin,pmax; float ymin,ymax,xmin,xmax,pymin,pymax; Ivs fr, fryr; u8 snap[0x40]; int have_snap; } DkCls;
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
        if(!c->have_snap&&GA_OK(obj)&&GA_OK(obj+0x3f)&&!(rd8(g,obj+0x10)&0x42)){ memcpy(c->snap,hp(g,obj),0x40); c->have_snap=1; } }   /* the first drawn object's bytes (owner field: plan section 5) */
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
void kh_aspect_dk_next(KCtx *g,u64 ra,u64 obj){    /* runtime/bgload.c f_4ad270, KHNMC_ASPECT_LOG=2: obj = its result */
    if(ra!=DK_RA_NEXT||!dk_in) return; dk_set(g,obj); }
void orig_f_126500(KCtx *);
void f_126500(KCtx *g){
    if(__builtin_expect(!kh_aspect_m,1)){ orig_f_126500(g); return; }
    u64 ra=rd64(g,KH_RSP(g));
    if(ra==DK_RA_END){ if(dk_in){ dk_in=0; dk_cur=-1; dk_obj=0; } orig_f_126500(g); return; }
    if(ra!=DK_RA_MODE&&dk_in&&dk_cur>=0){ pthread_mutex_lock(&mm); dkc[dk_cur].nested++; int i; for(i=0;i<n_nra;i++) if(nra[i].ra==ra) break;
        if(i==n_nra&&n_nra<32){ nra[i].ra=ra; n_nra++; } if(i<32) nra[i].n++; pthread_mutex_unlock(&mm); }
    orig_f_126500(g);
    if(ra==DK_RA_MODE){                                                              /* the plan's H3a point: list = guest rdi */
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
    if(!dk_in) dk_out_draws[kind]++;
    else if(dk_cur<0) dk_gap_draws[kind]++;
    else { DkCls *c=&dkc[dk_cur]; c->draws[kind]++;
        if(yv){ c->ydraws++; if(d->src==3) c->ystg++; else c->ycb++; float xa=d->xmin-ox, xb=d->xmax-ox;
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
            fprintf(dl_f,"%llu\t%llu\t%u\t%llx\t%llx\t%s\t%d\t%u\t%u\t%d\t%s\t%s\t%d\t%s\n",P(fr),P(kh_game_frames()),t,P(d->list),P(d->pso),vk_name[kind],d->indexed,d->count,d->start,d->base,
                    !dk_in?"out":dk_cur<0?"gap":"obj",dk_in&&dk_cur>=0?dkc[dk_cur].name:"-",dk_in?dk_pos-1:-1,nra_>0?ch:"-");
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
    fprintf(dl_f,"frame\tgame_frame\ttid\tlist\tpso\tvs\tindexed\tcount\tstart\tbase\tdk\tclass\tpos\tchain_rva\n");
    fprintf(stderr,"kh: aspect: KHNMC_ASPECT_M_DRAWLOG: every draw of %d present range(s) (the first %llu-%llu) into %s (a diagnostic of the measurement M; the frame column numbers presents as the `kh: gfx frame=` lines do)\n",dl_n,P(dl_lo[0]),P(dl_hi[0]),dl_path);
}
void kh_aspect_m_null_report(FILE *f);
static void m_report(FILE *f){
    extern int kh_gfx_vkd3d_on; if(!kh_gfx_vkd3d_on) kh_aspect_m_null_report(f); else fprintf(f,"kh: aspect M: draws and scissor rects are recorded by the null backend only (KHNMC_GFX=vkd3d / vkd3dw here): no draw columns\n");
    pthread_mutex_lock(&mm);
    fprintf(f,"kh: aspect M: dk passes (f_166640 setScreenMode at 0x140166677) %llu (begun inside another pass %llu), objects made current %llu, guest thread of the passes %u; "
              "draws (null backend) on guest thread %u, on another thread %llu; classes %d%s\n",P(dk_passes),P(dk_passes_nested),P(dk_objects),dk_tid,draw_tid,P(draws_other_thread),n_dkc,dkc_cut?" (table full: some classes not recorded)":"");
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
        case 8: asp_mode=1; kh_aspect_test_config("h2,h3",NULL); break;              /* task 40 test aids */
        case 9: asp_mode=0; kh_aspect_test_config("h2",NULL); break;
        case 10: asp_mode=1; kh_aspect_test_config(NULL,"yes"); break;
        case 11: asp_mode=0; kh_aspect_test_config(NULL,"1"); break;
        case 12: asp_mode=1; kh_aspect_test_config("h4,",NULL); break;
        case 13: kh_aspect_config(NULL,"3",NULL); break;
        case 14: kh_aspect_m=0; kh_aspect_drawlog_config("x.tsv@1-2"); break;              /* task 40 review: the draw-log diagnostic */
        case 15: kh_aspect_m=1; kh_aspect_drawlog_config("x.tsv@5-3"); break;
        case 16: kh_aspect_m=1; kh_aspect_drawlog_config("x.tsv@1-2,"); break;
        case 17: kh_aspect_m=1; kh_aspect_drawlog_config("/nonexistent-khnmc-dir/x.tsv@1-2"); break;
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
/* task 40: a stand-in for a culling pass in the H2 test: it records cam+0x98 as it sees it and, when asked, writes it */
static u64 h2t_cam; static u32 h2t_seen, h2t_put;
static void h2t_fn(KCtx *g){ h2t_seen=rd32(g,h2t_cam+CAM_M11); if(h2t_put) wr32(g,h2t_cam+CAM_M11,h2t_put); }
/* task 40 review: a stand-in pass for the wrappers h2b_wrap / h2c_wrap, recording cam+0x98 of three cameras as it sees them */
static u64 h2w_cam[3]; static u32 h2w_seen[3]; static int h2w_calls;
static void h2w_fn(KCtx *g){ h2w_calls++; for(int i=0;i<3;i++) h2w_seen[i]=rd32(g,h2w_cam[i]+CAM_M11); }
void f_19cfc0(KCtx *);
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
      { s16 y,h; h4_target(2,52,-1,0x1a2,&y,&h); ACHECK(y==-53&&h==522); h4_target(1,52,-1,0x24,&y,&h); ACHECK(y==-53&&h==88);
        h4_target(0,52,0x17d,0x24,&y,&h); ACHECK(y==0x17d&&h==88); h4_target(2,53,-1,0x1a2,&y,&h); ACHECK(y==-54&&h==524);
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
        static const int scr[3][2]={{1920,1080},{1920,1350},{2670,1878}};
        for(int sc=0;sc<3;sc++) for(int mode=0;mode<2;mode++){ SCREEN(scr[sc][0],scr[sc][1]); asp_mode=mode; int B=kh_aspect_B(scr[sc][0],scr[sc][1]);
            for(int i=3;i<7;i++){ wr16(g,h4o[i].obj+0xe,0x5555); wr16(g,h4o[i].obj+0xa,0x5555); }
            int ok=0; st_call(g,f_156e20,NULL,0,&ok); ACHECK(ok);
            for(int i=3;i<7;i++){ const H4Obj *o=&h4o[i]; s16 ty=o->y0,th=o->h0; if(mode) h4_target(o->kind,B,o->y0,o->h0,&ty,&th);
                ACHECK((s16)rd16(g,o->obj+0xe)==ty&&(s16)rd16(g,o->obj+0xa)==th);
                if(sc==0&&mode==0){ px[i-3]=(s16)rd16(g,o->obj+0xc); pw[i-3]=(s16)rd16(g,o->obj+0x8); }
                else ACHECK((s16)rd16(g,o->obj+0xc)==px[i-3]&&(s16)rd16(g,o->obj+0x8)==pw[i-3]); }
            if(sc==1&&mode==1) ACHECK((s16)rd16(g,h4o[3].obj+0xe)==-53&&(s16)rd16(g,h4o[3].obj+0xa)==88&&(s16)rd16(g,h4o[5].obj+0xe)==381&&(s16)rd16(g,h4o[5].obj+0xa)==88); }
        ACHECK(pw[0]==2*((s16)~px[0]+0x101)&&pw[1]==pw[0]&&px[1]==px[0]);                                          /* x = ~m, w = 2 (m + 0x101) for the exe's margin m (here from the selftest's GX) */
        for(int i=3;i<7;i++) memcpy(hp(g,h4o[i].obj),osave[i],0x120);
        /* the lifted f_156f10 with the object's state word +0x108 = 0 (it returns before drawing): the fade rect at its entry */
        for(int i=0;i<3;i++) for(int sc=0;sc<3;sc++) for(int mode=0;mode<2;mode++){ const H4Obj *o=&h4o[i]; SCREEN(scr[sc][0],scr[sc][1]); asp_mode=mode;
            wr32(g,o->obj+0x108,0); wr16(g,o->obj+0xe,(u16)o->y0); wr16(g,o->obj+0xa,(u16)o->h0); int ok=0; st_call(g,f_156f10,(u64[]){o->obj},1,&ok);
            s16 ty=o->y0,th=o->h0; if(mode) h4_target(2,kh_aspect_B(scr[sc][0],scr[sc][1]),o->y0,o->h0,&ty,&th);
            ACHECK(ok&&(s16)rd16(g,o->obj+0xe)==ty&&(s16)rd16(g,o->obj+0xa)==th);
            if(sc==1&&mode==1) ACHECK(ty==-53&&th==522); }
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
        ACHECK(fatal_case(8,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=h2,h3: 'h3' is not one of")); ACHECK(fatal_case(9,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_OFF=h2"));
        ACHECK(fatal_case(10,"aspect-test-nocull-value: KHNMC_ASPECT_TEST_NOCULL=yes")); ACHECK(fatal_case(11,"aspect-test-needs-fill: KHNMC_ASPECT_TEST_NOCULL=1"));
        ACHECK(fatal_case(12,"aspect-test-off-value: KHNMC_ASPECT_TEST_OFF=h4, ends with a comma")); ACHECK(fatal_case(13,"aspect-log-value: KHNMC_ASPECT_LOG=3"));
        ACHECK(fatal_case(14,"aspect-m-drawlog-needs-m: KHNMC_ASPECT_M_DRAWLOG=x.tsv@1-2")); ACHECK(fatal_case(15,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=x.tsv@5-3"));
        ACHECK(fatal_case(16,"aspect-m-drawlog-value: KHNMC_ASPECT_M_DRAWLOG=x.tsv@1-2,")); ACHECK(fatal_case(17,"aspect-m-drawlog-open: KHNMC_ASPECT_M_DRAWLOG: cannot create /nonexistent-khnmc-dir/x.tsv")); }
    else kh_selftest_not_run("aspect","the 18 named fatals of KHNMC_SCREEN / KHNMC_ASPECT* (no child process)",18);
    fprintf(stderr,"kh: aspect selftest: %d checks, %d failures\n",schecks,sfails);
    return sfails;
}
