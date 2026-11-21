#pragma once
#include <math.h>

enum { KH_TOUCH_AUTO, KH_TOUCH_ALWAYS, KH_TOUCH_OFF };
typedef struct { int mode,size,opacity,fixed,sensitivity,shortcuts,dpad; } KHTouchSettings;
extern void (*kh_ios_touch_changed)(KHTouchSettings);
static inline int kh_touch_visible(int mode,int physical){
    return mode==KH_TOUCH_ALWAYS || (mode==KH_TOUCH_AUTO && !physical);
}

enum { KH_UP, KH_DOWN, KH_LEFT, KH_RIGHT, KH_A, KH_B, KH_X, KH_Y,
       KH_LT, KH_LB, KH_L3, KH_MENU, KH_RMC, KH_R3, KH_RB, KH_RT,
       KH_SLOT_A, KH_SLOT_B, KH_SLOT_X, KH_SLOT_Y, KH_TOUCH_COUNT };
static inline unsigned kh_touch_mask(int i){
    static const unsigned masks[]={1,2,4,8,0x1000,0x2000,0x4000,0x8000,
        0x10000,0x100,0x40,0x10,0x50,0x80,0x200,0x20000,0x1100,0x2100,0x4100,0x8100};
    return masks[i];
}
static inline double kh_touch_diameter(int size,double w,double h){
    return fmin(44+size*6,fmin((w-8)/(h>w?10:8),h/(h>w?16:9)));
}
/* All persistent controls fit above the lower HUD corners, in safe-area points. */
static inline void kh_touch_button_frame(int i,int size,double w,double h,double *x,double *y,double *diameter){
    double d=kh_touch_diameter(size,w,h);
    if(i<KH_LT){
        static const int dx[]={0,0,-1,1,0,1,-1,0},dy[]={-1,1,0,0,1,0,0,-1};
        *x=(i<KH_A?1.5*d+2:w-1.5*d-2)+dx[i]*d;
        *y=(h>w?h*.655:h*.44)+dy[i]*d;
    }else if(h>w){
        /* The animated rank fills the upper right. Stack these rows on the left. */
        int n=i-KH_LT;
        *x=2+(n%4+.5)*d;
        *y=h*.25+(n/4+.55)*d;
    }else if(i<KH_SLOT_A){
        *x=w/2+(i-KH_LT-3.5)*d;
        *y=d*.55;
    }else{
        *x=w/2+(i-KH_SLOT_A-1.5)*d;
        *y=d*1.6;
    }
    *diameter=d;
}
static inline void kh_touch_stick_home(int side,double d,double w,double h,double *x,double *y){
    double inset=h>w?2+3.95*d:w*.23;
    *x=side?w-inset:inset; *y=h*(h>w?.655:.75);
}
typedef struct { double x,y,dx,dy; } KHStick;
static inline void kh_touch_drag(KHStick *s,double x,double y,double radius,int fixed,int sensitivity){
    radius*=100.0/sensitivity;
    double dx=x-s->x,dy=y-s->y,len=hypot(dx,dy);
    if(len>radius){
        dx*=radius/len; dy*=radius/len;
        if(!fixed){s->x=x-dx; s->y=y-dy;}
    }
    s->dx=dx/radius; s->dy=-dy/radius;
}
