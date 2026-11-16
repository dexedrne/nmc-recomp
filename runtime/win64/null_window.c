/* null_window.c : headless window / display seam (user32).  One hidden top-level window that reports focus and a fixed client
 * rectangle; one 1920x1080 monitor.  gdi32 is deliberately not provided (all 296 sites are in the sd Layout debug-draw classes:
 * calling any of them is a named fatal through the weak default). */
#include "khnmc_rt.h"

#define HWND_MAIN 0xAB0001ull
#define HMON_MAIN 0xAB0100ull
static int win_w=1280, win_h=720, win_x=100, win_y=100, win_zoomed=0, win_visible=0;
static u64 wndproc_ga, winlong[32];
static int scr_w=1920, scr_h=1080;
void kh_seam_window_init(void){ const char *e=getenv("KH_WIN"); if(e) sscanf(e,"%dx%d",&win_w,&win_h); }

IMP(U32,RegisterClassExA){ kh_cnt_window_calls++; u64 wc=A0(g); wndproc_ga=rd64(g,wc+8); SET_RAX(g,0xC001); }
IMP(U32,UnregisterClassA){ SET_RAX(g,1); }
IMP(U32,CreateWindowExA){ kh_cnt_window_calls++; /* (exstyle, class, title, style, x, y, w, h, parent, menu, inst, param) */
    int w=(int)(s32)ARG(g,6),h=(int)(s32)ARG(g,7); if(w>0&&w<16384) win_w=w; if(h>0&&h<16384) win_h=h; SET_RAX(g,HWND_MAIN); }
IMP(U32,DestroyWindow){ kh_cnt_window_calls++; SET_RAX(g,1); }
IMP(U32,ShowWindow){ kh_cnt_window_calls++; win_visible=(int)A1(g)!=0; SET_RAX(g,0); }
IMP(U32,SetWindowPos){ kh_cnt_window_calls++; /* (hwnd, after, x, y, cx, cy, flags) */ u32 fl=(u32)ARG(g,6); if(!(fl&2)){ win_x=(int)(s32)A2(g); win_y=(int)(s32)A3(g); } if(!(fl&1)){ win_w=(int)(s32)ARG(g,4); win_h=(int)(s32)ARG(g,5); } SET_RAX(g,1); }
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
IMP(U32,EnumDisplaySettingsA){ u32 mode=(u32)A1(g); u64 d=A2(g); if(mode>1&&mode!=0xFFFFFFFFu){ SET_RAX(g,0); return; }
    wr16(g,d+0x44,0x9c); wr32(g,d+0x48,0x5c0000|0x400000|0x80000|0x100000|0x20000); wr32(g,d+0x68,32); wr32(g,d+0x6c,(u32)scr_w); wr32(g,d+0x70,(u32)scr_h); wr32(g,d+0x78,60); SET_RAX(g,1); }
IMP(U32,GetWindowLongPtrA){ int i=(int)(s32)A1(g); u64 v=0; if(i==-4) v=wndproc_ga; else if(i==-6) v=GS_BASE; else if(i==-16) v=0x10CF0000; else if(i==-20) v=0; else if(i>=0&&i<32*8) v=winlong[i/8]; SET_RAX(g,v); }
IMP(U32,SetWindowLongPtrA){ int i=(int)(s32)A1(g); u64 old=0; if(i==-4){ old=wndproc_ga; wndproc_ga=A2(g); } else if(i>=0&&i<32*8){ old=winlong[i/8]; winlong[i/8]=A2(g); } SET_RAX(g,old); }
IMP(U32,SetWindowLongPtrW){ int i=(int)(s32)A1(g); u64 old=0; if(i>=0&&i<32*8){ old=winlong[i/8]; winlong[i/8]=A2(g); } SET_RAX(g,old); }
IMP(U32,DefWindowProcA){ SET_RAX(g,0); }
IMP(U32,SendMessageA){ kh_cnt_window_calls++; SET_RAX(g,0); }
IMP(U32,PostQuitMessage){ kh_cnt_window_calls++; kh_warn("PostQuitMessage(%d) called by guest",(int)A0(g)); }
IMP(U32,LoadCursorA){ SET_RAX(g,0xC0001); }
