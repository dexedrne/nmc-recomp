#pragma once
#include <string.h>

/* Auto preserves the old Nintendo heuristic and optional X/Y override. */
static inline int kh_pad_swap_xy(const char *layout,int nintendo,const char *override){
    if(!strcmp(layout,"positions")) return 0;
    if(!strcmp(layout,"letters")) return 1;
    return override && *override ? *override=='1' : nintendo;
}

/* GC's A/B/X/Y channels, translated to XInput Cross/Circle/Square/Triangle. */
static inline unsigned short kh_pad_faces(int a,int b,int x,int y,int swap_xy,int swap_ab){
    return (a?(swap_ab?0x2000:0x1000):0) | (b?(swap_ab?0x1000:0x2000):0) |
           (x?(swap_xy?0x8000:0x4000):0) | (y?(swap_xy?0x4000:0x8000):0);
}
