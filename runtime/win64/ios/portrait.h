#ifndef KH_IOS_PORTRAIT_H
#define KH_IOS_PORTRAIT_H
#include <math.h>

/* device is +1 for newly reported inverted portrait, -1 for upright, zero otherwise. */
static inline int kh_portrait_flip(int upright_interface,int *inverted,int device,int motion,double x,double y){
    if(motion && fabs(y)>0.68 && fabs(y)>fabs(x)) *inverted=y>0;
    else if(device) *inverted=device>0;
    return upright_interface && *inverted;
}
#endif
