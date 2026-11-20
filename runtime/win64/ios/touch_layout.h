#pragma once
#include <math.h>

enum { KH_TOUCH_AUTO, KH_TOUCH_ALWAYS, KH_TOUCH_OFF };
static inline int kh_touch_visible(int mode,int physical){
    return mode==KH_TOUCH_ALWAYS || (mode==KH_TOUCH_AUTO && !physical);
}

/* Centers in safe-area points. Keep the command and HP corners below the two diamonds. */
static inline void kh_touch_button_frame(int i,int size,double w,double h,double *x,double *y,double *diameter){
    static const int dx[]={0,0,-1,1,0,1,-1,0,-1,0,1};
    static const int dy[]={-1,1,0,0,1,0,0,-1,0,0,0};
    double d=fmin(44+size*6,fmin((w-12)/6,h/5));
    double side=1.5*d+2;
    *x=(i<4?side:i<8?w-side:w/2)+dx[i]*d;
    *y=(i<8?h*(h>w?0.58:0.44):d*0.55)+dy[i]*d;
    *diameter=d;
}
