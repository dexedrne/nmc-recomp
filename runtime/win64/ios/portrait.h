#ifndef KH_IOS_PORTRAIT_H
#define KH_IOS_PORTRAIT_H
#include <math.h>

/* The picture's own half-turn for upside-down portrait (owner 2026-11-18 and 2026-11-23: "kh2 portrait doesnt work upside down,
 * only right side up portrait").  An iPhone does not turn its interface upside down, so the app turns the view controller's root
 * view itself while the phone is held that way (runtime/ios/ios_main.m: applyFlip); the Metal picture and the touch overlay are
 * subviews, so both turn and UIKit maps touches through the same turn.
 *
 *   upright_interface  the interface is portrait (the app is not in landscape)
 *   inverted           the latched answer: 1 the phone is held upside down in portrait, 0 upright
 *   device             the phone's own report: +1 inverted portrait, -1 upright portrait, 0 otherwise
 *   motion             a CoreMotion sample exists; x,y = its gravity vector in the DEVICE frame, so y runs toward the screen's
 *                      top edge and +y (with |y| > |x|) means the phone is held upside down
 *
 * Gravity decides whenever it is decisive: it is never stale.  The threshold is 0.35, not 0.68, so a reclined hold (the screen
 * about 70 degrees from vertical, 20 degrees off lying flat) still counts - the owner plays in bed, and 0.68 needed the phone
 * almost upright.  |y| > |x| keeps landscape out (gravity is along x there).  Where gravity cannot decide (face up, face down, a diagonal turn) the phone's
 * own report is used: an upside-down report always turns the picture, an upright report turns it back only when there is no
 * motion at all.  An upright report never cancels an inversion gravity has seen, because that stale report is common on this
 * device (2026-11-20 note); without motion the report is the only signal there is.  Otherwise the latch stays, so nothing
 * oscillates. */
static inline int kh_portrait_flip(int upright_interface,int *inverted,int device,int motion,double x,double y){
    if(motion && fabs(y)>0.35 && fabs(y)>fabs(x)) *inverted=y>0;
    else if(device>0) *inverted=1;
    else if(!motion && device<0) *inverted=0;
    return upright_interface && *inverted;
}
#endif
