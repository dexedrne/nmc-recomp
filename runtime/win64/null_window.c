/* null_window.c : headless window / display seam (user32).  One hidden top-level window that reports focus and a fixed client
 * rectangle; one 1920x1080 monitor.  gdi32 is deliberately not provided (all 296 sites are in the sd Layout debug-draw classes:
 * calling any of them is a named fatal through the weak default).
 *
 * Task 37 (seam S1 of NATIVE-FILL-PLAN.md, section 6): KHNMC_SCREEN=WxH (W and H 16..16384, digits only, a lower-case x) reports a
 * W x H screen instead: the monitor (GetMonitorInfoA, GetSystemMetrics, the cursor centre), ONE display mode of that size
 * (EnumDisplaySettingsA mode 0 and ENUM_CURRENT_SETTINGS; every other index ends the list), a window that is maximized from the
 * first IsZoomed call on, and a client rectangle of W x H that window-size requests (CreateWindowExA, SetWindowPos) do not change
 * (they are counted and reported at exit).  The game then sizes its back buffer through its own maximized-window branch (exe
 * 0x117fd0: IsZoomed -> GetClientRect -> 0x1119b0).  The DXGI output's desktop rectangle (null_d3d12.c, seam S2) reports the same
 * size.  Unset or empty: exactly the behaviour before task 37.  A malformed value, or KHNMC_SCREEN together with KH_WIN, is a named
 * fatal.  Fullscreen is not part of this seam (the game's fullscreen setting byte, GX+0x416, takes another branch); so every present
 * checks that the game really built W x H (runtime/aspect.c kh_screen_check_present: the named, counted WARNING screen-not-applied
 * otherwise, in the exit report below). */
#include "khnmc_rt.h"

#define HWND_MAIN 0xAB0001ull
#define HMON_MAIN 0xAB0100ull
static int win_w=1280, win_h=720, win_x=100, win_y=100, win_zoomed=0, win_visible=0;
static u64 wndproc_ga, winlong[32];
static int scr_w=1920, scr_h=1080;
static int scr_set;                                   /* task 37: 1 = KHNMC_SCREEN gave the screen */
static _Atomic u64 scr_req_create, scr_req_setpos;    /* window-size requests the screen seam did not apply (counted, reported) */
void kh_aspect_init(void);

/* KHNMC_SCREEN value: 0 = ok (*w, *h set), 1 = not "<digits>x<digits>", 2 = a side outside 16..16384 */
int kh_screen_parse(const char *s,int *w,int *h){
    if(!s) return 1;
    long v[2]; const char *p=s;
    for(int k=0;k<2;k++){
        if(*p<'0'||*p>'9') return 1;
        long n=0; int digits=0; while(*p>='0'&&*p<='9'){ if(++digits>6) return 2; n=n*10+(*p-'0'); p++; }
        v[k]=n;
        if(k==0){ if(*p!='x') return 1; p++; }
    }
    if(*p) return 1;
    if(v[0]<16||v[0]>16384||v[1]<16||v[1]>16384) return 2;
    *w=(int)v[0]; *h=(int)v[1]; return 0;
}
/* the seam's configuration from the two variables (unset = NULL); every refusal is a named fatal */
void kh_screen_config(const char *screen,const char *khwin){
    if(!screen||!*screen) return;
    int w=0,h=0, r=kh_screen_parse(screen,&w,&h);
    if(r==1) kh_fatal(NULL,"screen-value-malformed: KHNMC_SCREEN=%s: expected WxH, two decimal numbers joined by a lower-case x (e.g. 1920x1350)",screen);
    if(r) kh_fatal(NULL,"screen-value-range: KHNMC_SCREEN=%s: each side must be 16..16384 pixels",screen);
    if(khwin&&*khwin) kh_fatal(NULL,"screen-and-kh-win: KHNMC_SCREEN=%s and KH_WIN=%s are both set: the screen seam reports a maximized window whose client rectangle is the screen, KH_WIN would be ignored",screen,khwin);
    scr_w=w; scr_h=h; scr_set=1; win_w=w; win_h=h; win_x=0; win_y=0; win_zoomed=1;
    fprintf(stderr,"kh: screen: KHNMC_SCREEN=%dx%d: the monitor, the one display mode, the client rectangle and the DXGI output are %dx%d; the window is maximized (IsZoomed=1) from the first call; what the game builds is checked at every present (WARNING screen-not-applied when it is another size)\n",w,h,w,h);
}
void kh_seam_window_init(void){
    const char *e=getenv("KH_WIN"); if(e) sscanf(e,"%dx%d",&win_w,&win_h);
    kh_screen_config(getenv("KHNMC_SCREEN"),e);
    kh_aspect_init();                                 /* task 37: KHNMC_ASPECT / KHNMC_ASPECT_LOG (runtime/aspect.c), read with the screen */
}
/* the screen the seam reports (1920x1080 unless KHNMC_SCREEN); returns 1 when KHNMC_SCREEN set it */
int kh_screen_size(int *w,int *h){ if(w) *w=scr_w; if(h) *h=scr_h; return scr_set; }
void kh_screen_check_counts(u64 *presents,u64 *bad);   /* runtime/aspect.c: the built size against the screen at each present */
void kh_screen_report(FILE *f){
    if(!scr_set) return;
    u64 np=0,nb=0; kh_screen_check_counts(&np,&nb);
    fprintf(f,"kh: screen: KHNMC_SCREEN=%dx%d; window-size requests not applied (the window stays maximized at the screen size): CreateWindowExA %llu, SetWindowPos %llu; "
              "the built size checked at %llu presents (swap chain, GX+0x8, GX+0x18): not %dx%d at %llu\n",
            scr_w,scr_h,(unsigned long long)scr_req_create,(unsigned long long)scr_req_setpos,(unsigned long long)np,scr_w,scr_h,(unsigned long long)nb);
    if(nb) fprintf(f,"kh: screen: WARNING: screen-not-applied: at %llu of %llu presents the game's swap chain or built size was not KHNMC_SCREEN=%dx%d (the first one is described where it happened)\n",
                   (unsigned long long)nb,(unsigned long long)np,scr_w,scr_h);
}
/* selftest only (runtime/aspect.c): put the seam into a known state and back.  Returns the state it replaced through *old (8 ints). */
void kh_screen_test_state(const int *set,int *old){
    if(old){ old[0]=scr_set; old[1]=scr_w; old[2]=scr_h; old[3]=win_w; old[4]=win_h; old[5]=win_x; old[6]=win_y; old[7]=win_zoomed; }
    if(set){ scr_set=set[0]; scr_w=set[1]; scr_h=set[2]; win_w=set[3]; win_h=set[4]; win_x=set[5]; win_y=set[6]; win_zoomed=set[7]; }
}
void kh_screen_test_counts(u64 *create,u64 *setpos){ if(create) *create=scr_req_create; if(setpos) *setpos=scr_req_setpos; }

IMP(U32,RegisterClassExA){ kh_cnt_window_calls++; u64 wc=A0(g); wndproc_ga=rd64(g,wc+8); SET_RAX(g,0xC001); }
IMP(U32,UnregisterClassA){ SET_RAX(g,1); }
IMP(U32,CreateWindowExA){ kh_cnt_window_calls++; /* (exstyle, class, title, style, x, y, w, h, parent, menu, inst, param) */
    int w=(int)(s32)ARG(g,6),h=(int)(s32)ARG(g,7);
    if(scr_set){ if(w!=scr_w||h!=scr_h) scr_req_create++; SET_RAX(g,HWND_MAIN); return; }      /* maximized at the screen size */
    if(w>0&&w<16384) win_w=w; if(h>0&&h<16384) win_h=h; SET_RAX(g,HWND_MAIN); }
IMP(U32,DestroyWindow){ kh_cnt_window_calls++; SET_RAX(g,1); }
IMP(U32,ShowWindow){ kh_cnt_window_calls++; win_visible=(int)A1(g)!=0; SET_RAX(g,0); }
IMP(U32,SetWindowPos){ kh_cnt_window_calls++; /* (hwnd, after, x, y, cx, cy, flags) */ u32 fl=(u32)ARG(g,6);
    if(scr_set){ if(!(fl&1)&&((int)(s32)ARG(g,4)!=scr_w||(int)(s32)ARG(g,5)!=scr_h)) scr_req_setpos++; SET_RAX(g,1); return; }   /* SWP_NOSIZE = 1 */
    if(!(fl&2)){ win_x=(int)(s32)A2(g); win_y=(int)(s32)A3(g); } if(!(fl&1)){ win_w=(int)(s32)ARG(g,4); win_h=(int)(s32)ARG(g,5); } SET_RAX(g,1); }
IMP(U32,GetClientRect){ kh_cnt_window_calls++; u64 r=A1(g); wr32(g,r,0); wr32(g,r+4,0); wr32(g,r+8,(u32)win_w); wr32(g,r+12,(u32)win_h); SET_RAX(g,1); }
IMP(U32,AdjustWindowRect){ kh_cnt_window_calls++; u64 r=A0(g); (void)r; SET_RAX(g,1); }      /* no frame in the headless window: client == window */
IMP(U32,ScreenToClient){ SET_RAX(g,1); }
IMP(U32,GetCursorPos){ u64 p=A0(g); wr32(g,p,(u32)(scr_w/2)); wr32(g,p+4,(u32)(scr_h/2)); SET_RAX(g,1); }
IMP(U32,GetForegroundWindow){ kh_cnt_window_calls++; SET_RAX(g,HWND_MAIN); }            /* the window "has focus" (the pad poll is gated on it) */
IMP(U32,IsZoomed){ SET_RAX(g,win_zoomed); }
IMP(U32,GetSystemMetrics){ int i=(int)A0(g); SET_RAX(g,(u32)(i==0?scr_w:i==1?scr_h:0)); }
IMP(U32,MonitorFromWindow){ SET_RAX(g,HMON_MAIN); }
IMP(U32,GetMonitorInfoA){ u64 p=A1(g); u32 cb=rd32(g,p); wr32(g,p+4,0); wr32(g,p+8,0); wr32(g,p+12,(u32)scr_w); wr32(g,p+16,(u32)scr_h); wr32(g,p+20,0); wr32(g,p+24,0); wr32(g,p+28,(u32)scr_w); wr32(g,p+32,(u32)scr_h); wr32(g,p+36,1);
    if(cb>=0x48) strcpy((char*)hp(g,p+40),"\\\\.\\DISPLAY1"); SET_RAX(g,1); }
/* without KHNMC_SCREEN: modes 0 and 1 and ENUM_CURRENT_SETTINGS (-1) all report 1920x1080 (as before task 37); with it: mode 0 and
 * ENUM_CURRENT_SETTINGS report W x H and every other index ends the list (one mode) */
IMP(U32,EnumDisplaySettingsA){ u32 mode=(u32)A1(g); u64 d=A2(g);
    if(scr_set?(mode!=0&&mode!=0xFFFFFFFFu):(mode>1&&mode!=0xFFFFFFFFu)){ SET_RAX(g,0); return; }
    wr16(g,d+0x44,0x9c); wr32(g,d+0x48,0x5c0000|0x400000|0x80000|0x100000|0x20000); wr32(g,d+0x68,32); wr32(g,d+0x6c,(u32)scr_w); wr32(g,d+0x70,(u32)scr_h); wr32(g,d+0x78,60); SET_RAX(g,1); }
IMP(U32,GetWindowLongPtrA){ int i=(int)(s32)A1(g); u64 v=0; if(i==-4) v=wndproc_ga; else if(i==-6) v=GS_BASE; else if(i==-16) v=0x10CF0000; else if(i==-20) v=0; else if(i>=0&&i<32*8) v=winlong[i/8]; SET_RAX(g,v); }
IMP(U32,SetWindowLongPtrA){ int i=(int)(s32)A1(g); u64 old=0; if(i==-4){ old=wndproc_ga; wndproc_ga=A2(g); } else if(i>=0&&i<32*8){ old=winlong[i/8]; winlong[i/8]=A2(g); } SET_RAX(g,old); }
IMP(U32,SetWindowLongPtrW){ int i=(int)(s32)A1(g); u64 old=0; if(i>=0&&i<32*8){ old=winlong[i/8]; winlong[i/8]=A2(g); } SET_RAX(g,old); }
IMP(U32,DefWindowProcA){ SET_RAX(g,0); }
IMP(U32,SendMessageA){ kh_cnt_window_calls++; SET_RAX(g,0); }
IMP(U32,PostQuitMessage){ kh_cnt_window_calls++; kh_warn("PostQuitMessage(%d) called by guest",(int)A0(g)); }
IMP(U32,LoadCursorA){ SET_RAX(g,0xC0001); }
