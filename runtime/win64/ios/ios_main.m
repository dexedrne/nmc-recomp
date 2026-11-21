/* runtime/ios/ios_main.m : the iOS app's main() and its UIKit shell (task 26; TASK41: the game view, the display link, suspend / resume).
 *
 * TASK41 PARTS UNCOMPILED.  Written on Linux without an iOS SDK.  Task 26's structure has run on the phone (2026-11-18: the null-backend
 * game with sound).  What TASK41 added (KHGameView, the display-link thread, the scene-delegate life cycle, the audio interruption
 * handler) has never been compiled; MAC-STEPS-41.md lists every API used here with the iOS version it needs (deployment target 17.0)
 * and the order in which to check it on the phone.  Its C side (runtime/app_life.c, gfx_present.c, null_window.c) runs on Linux.
 *
 * What it does, and what it leaves to C code that has run (runtime/host_env.c, KH_IOS_PATHS):
 *   - main(): passes the app container (NSHomeDirectory) and its scratch directory (NSTemporaryDirectory) to the runtime as
 *     KH_APP_HOME and KH_TMPDIR, registers the audio start / stop the life cycle uses, then UIApplicationMain.
 *   - THE GAME VIEW (IOS-GRAPHICS-PLAN step 10): KHGameView, whose +layerClass is CAMetalLayer, so the view is the layer's delegate
 *     (MoltenVK's guide).  contentScaleFactor = the screen's nativeScale (the drawable is the panel's pixels), opaque, black.  Landscape
 *     only (Info.plist and the view controller), status bar hidden, home indicator auto-hidden.
 *   - THE ORDER OF START: after the first layout with a size, kh_present_note_drawable(w_px, h_px) (every app) and
 *     kh_present_set_layer(layer, w_px, h_px) (only in an app built with the graphics backend: kh_present_built()), then the game
 *     thread.  So the drawable size is in the runtime BEFORE the game reads its screen size: khnmc.conf `env KHNMC_SCREEN=layer`
 *     (device_run.sh PRESENT=1) sizes the game to it (runtime/null_window.c), also in the null app.
 *     Later layouts: kh_present_set_size (stored; our swapchain is rebuilt at the next Present only when the size changed).
 *     The CAMetalLayer's drawableSize is not set here: MoltenVK's surface capabilities give currentExtent = the layer's bounds x
 *     contentsScale (naturalDrawableSizeMVK, MVKSurface::getNaturalExtent; contentsScale is the view's contentScaleFactor), and its
 *     swapchain sets drawableSize = imageExtent itself (MVKSwapchain::initCAMetalLayer), on the present thread.
 *   - THE DISPLAY LINK: a CADisplayLink on its OWN thread's run loop (UI work on the main thread never delays a tick), 60/60/60.  Each
 *     tick calls kh_vsync_tick, which paces the game's WaitForVBlank, but only in a run that presents to the layer
 *     (kh_present_wants_ticks() = 1, i.e. KHNMC_GFX_PRESENT=layer); in any other run the link stops at its first look after start-up,
 *     so the null app keeps its pacing, and its thread ends (its run loop is run in an explicit loop that the stop ends).  The test
 *     label is hidden after the first present (an overlay costs compositing every frame).
 *   - THREADS: every Vulkan WSI call is made by the runtime on the game's present thread, never here; this file never calls a vkd3d
 *     queue function, and the main thread never waits for the game without a short bound (kh_app_suspend / kh_app_resume).  It never
 *     activates the audio session or starts / stops the audio unit either: those run on the runtime's life worker (khnmc_app.h).
 *   - SUSPEND AND RESUME (step 11, runtime/khnmc_app.h): sceneWillResignActive -> kh_app_will_resign_active (the gate set, the focus
 *     lost, returns at once: GPU work is still allowed then); sceneDidEnterBackground -> kh_app_suspend(2000 ms bound);
 *     sceneDidBecomeActive -> the layer's size, kh_app_resume(500 ms bound), then every controller's CURRENT state.  Audio-session
 *     interruptions are posted to the life worker (kh_app_audio_interruption): it stops the unit when one begins and starts it again
 *     when it ends (or at the resume, when the app was suspended meanwhile).  A controller that disconnects
 *     releases pad 0 (a button held then would otherwise stay held).  `env KHNMC_APP_LIFE=off` in khnmc.conf turns the life cycle
 *     off (the calls are counted and do nothing).
 * No network, no data in the bundle; the game data in Documents is excluded from backup (above main); sound through RemoteIO.
 * Bundle id beer.vyvanse.khnmc (tools/ios/make_project.py). */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreMotion/CoreMotion.h>
#include "portrait.h"
#include "touch_layout.h"
#include "../pad_layout.h"
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#import <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../khnmc_app.h"
#include "../settings.h"
#include <string.h>
#include "../gfx_present.h"
#include "../khnmc_audio_out.h"

#define KH_APP_THREAD_STACK (1u<<20)          /* keep equal to tools/ios/host_shell.c; the guest threads get their own (loader.c) */
int kh_main(int argc,char **argv);
extern void (*kh_audio_pull_ready)(void);     /* runtime/khnmc_audio_out.h: called once the mixer's pull ring exists */
void kh_input_host_pad_set(unsigned short buttons,unsigned char lt,unsigned char rt,short lx,short ly,short rx,short ry);   /* null_input.c */

/* The controller (GameController framework) is XInput pad 0 (KHNMC_INPUT=host, set below unless khnmc.conf says otherwise): every
 * change of any element sends the whole state.  Buttons by position, as the PC game sees an Xbox pad: A (bottom; cross on a
 * DualSense) = 0x1000, B (right) = 0x2000, X (left) = 0x4000, Y (top) = 0x8000, LB/RB 0x100/0x200, the menu button START 0x10, the
 * options button BACK 0x20, the stick clicks 0x40/0x80, the d-pad 0x1/0x2/0x4/0x8; triggers 0..255; sticks -32767..32767 (up = +). */
static short pad_axis(float v){ if(!(v>=-1.0f)) v=-1.0f; if(v>1.0f) v=1.0f; return (short)lrintf(v*32767.0f); }
static unsigned char pad_trigger(float v){ if(!(v>=0.0f)) v=0.0f; if(v>1.0f) v=1.0f; return (unsigned char)lrintf(v*255.0f); }
static UILabel *test_label;
static GCController *pad_active;
static BOOL pad_nintendo;
void kh_input_host_nintendo(int on);
static GCControllerButtonInput *capture_button;
static void pad_layout(GCController *c){
    NSString *cat=c.productCategory?:@"", *name=c.vendorName?:@"";
    pad_nintendo=[cat rangeOfString:@"Switch" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
                 [cat rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
                 [name rangeOfString:@"Pro Controller" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
                 [name rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location!=NSNotFound;
}
static GCControllerButtonInput *touchpad_click(GCExtendedGamepad *p){
    if([p isKindOfClass:GCDualSenseGamepad.class]) return ((GCDualSenseGamepad *)p).touchpadButton;
    if([p isKindOfClass:GCDualShockGamepad.class]) return ((GCDualShockGamepad *)p).touchpadButton;
    return nil;
}
/* Optional profile elements have stable aliases even when their display names are localized.
 * Keep system gestures enabled. Home, PS and Mute have no game binding. */
static void pad_extras(GCExtendedGamepad *p){
    capture_button=nil;
    for(NSString *key in [p.buttons.allKeys sortedArrayUsingSelector:@selector(compare:)]){
        GCControllerButtonInput *v=p.buttons[key];
        if(v==p.buttonA || v==p.buttonB || v==p.buttonX || v==p.buttonY ||
           v==p.leftShoulder || v==p.rightShoulder || v==p.leftTrigger || v==p.rightTrigger ||
           v==p.leftThumbstickButton || v==p.rightThumbstickButton ||
           v==p.buttonMenu || v==p.buttonOptions ||
           v==p.dpad.left || v==p.dpad.right || v==p.dpad.up || v==p.dpad.down) continue;
        if(v!=p.buttonHome && ([key.lowercaseString containsString:@"share"] ||
                               [key.lowercaseString containsString:@"capture"])) capture_button=v;
    }
}
static unsigned short pad_buttons(GCExtendedGamepad *p){
    unsigned short b=0;
    if(p.dpad.up.pressed) b|=0x0001; if(p.dpad.down.pressed) b|=0x0002; if(p.dpad.left.pressed) b|=0x0004; if(p.dpad.right.pressed) b|=0x0008;
    if(p.buttonMenu.pressed) b|=0x0010; if(p.buttonOptions.pressed || touchpad_click(p).pressed || capture_button.pressed) b|=0x0020;
    if(p.leftThumbstickButton.pressed) b|=0x0040; if(p.rightThumbstickButton.pressed) b|=0x0080;
    if(p.leftShoulder.pressed) b|=0x0100; if(p.rightShoulder.pressed) b|=0x0200;
    return b | kh_pad_faces(p.buttonA.pressed,p.buttonB.pressed,p.buttonX.pressed,p.buttonY.pressed,0,0);
}
static void pad_send(GCExtendedGamepad *p){
    unsigned short b=pad_buttons(p);
    if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return;
    kh_input_host_pad_set(b,pad_trigger(p.leftTrigger.value),pad_trigger(p.rightTrigger.value),
                          pad_axis(p.leftThumbstick.xAxis.value),pad_axis(p.leftThumbstick.yAxis.value),
                          pad_axis(p.rightThumbstick.xAxis.value),pad_axis(p.rightThumbstick.yAxis.value));
}
/* ONE controller drives pad 0 (review r9: a second controller, or the disconnect of another one, must not change or release it): the
 * first connected controller with an extended gamepad profile; when it disconnects, the pad is released and the next connected one
 * (if any) takes over.  Everything here runs on the main queue. */
static int pad_attach(GCController *c){
    GCExtendedGamepad *p=c.extendedGamepad;
    if(!p){ fprintf(stderr,"kh: ios: controller '%s' has no extended gamepad profile: not used\n",c.vendorName.UTF8String?:"?"); return 0; }
    if(pad_active && pad_active!=c){ fprintf(stderr,"kh: ios: controller '%s' connected: not used (another controller is pad 0)\n",c.vendorName.UTF8String?:"?"); return 0; }
    pad_active=c;
    pad_layout(c);
    kh_title_pad_family(pad_nintendo,[p isKindOfClass:GCDualShockGamepad.class] || [p isKindOfClass:GCDualSenseGamepad.class]);
    /* A negative layout family keeps GC positions and the separate confirm/back preference. */
    kh_input_host_nintendo(pad_nintendo?1:-1);
    NSString *device=c.vendorName ?: c.productCategory;
    kh_menu_input_device(device.UTF8String,NULL);
    pad_extras(p);
    c.handlerQueue=dispatch_get_main_queue();
    p.valueChangedHandler=^(GCExtendedGamepad *gp,GCControllerElement *el){
        if(gp!=pad_active.extendedGamepad) return;
        NSString *element=el.unmappedLocalizedName ?: el.localizedName ?: el.aliases.anyObject;
        if(el) kh_menu_input_device(device.UTF8String,element.UTF8String);
        const char *log=getenv("KHNMC_PAD_LOG");
        if(log && *log=='1' && el){
            float v=[el isKindOfClass:[GCControllerButtonInput class]]?((GCControllerButtonInput *)el).value:-1.0f;
            fprintf(stderr,"kh: ios: pad element '%s' value %.2f\n",(el.localizedName?:@"?").UTF8String,v);
        }
        pad_send(gp); };
    pad_send(p);
    fprintf(stderr,"kh: ios: controller connected: %s (pad 0)\n",c.vendorName.UTF8String?:"?");
    return 1;
}
#if TARGET_OS_SIMULATOR
/* Runs once, before connecting a real pad, in sim_run.sh's disposable selftest container. */
static void sim_controller_pages(void){
    if(![NSProcessInfo.processInfo.arguments containsObject:@"--test-controller-pages"]) return;
    pad_active=[GCController controllerWithExtendedGamepad];
    GCExtendedGamepad *p=pad_active.extendedGamepad;
    [p.buttonY setValue:1]; [p.rightTrigger setValue:0.5f];
    BOOL ok=(kh_menu_map_buttons(pad_buttons(p),0)&0xc000)!=0 && pad_trigger(p.rightTrigger.value)==128;
    NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/controller-pages.txt"];
    NSString *result=ok?@"PASS":@"FAIL: simulator controller menu or mapped input";
    if(![result writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL])
        fprintf(stderr,"kh: simulator-controller-pages: could not write result\n");
    pad_active=nil;
}
#endif
/* Buttons win UIKit hit testing. Each remaining finger owns one stick until lift
 * or cancellation, even if it crosses the centre of the screen. */
@interface KHTouchSurface : UIView
- (void)reset;
- (void)publish;
@end
static KHTouchSurface *touch_surface;
static __weak UIWindow *game_window;
static KHTouchSettings touch_options;
static BOOL touch_ready,touch_flipped;
static id pad_connect_observer,pad_disconnect_observer;

@implementation KHTouchSurface {
    NSArray<UIButton *> *_buttons;
    UIView *_rings[2], *_knobs[2];
    id _finger[2];
    KHStick _sticks[2];
    CGSize _layoutSize;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if(!(self=[super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled=YES;
    for(int i=0;i<2;i++){
        _rings[i]=[UIView new]; _knobs[i]=[UIView new];
        for(UIView *part in @[_rings[i],_knobs[i]]){
            part.userInteractionEnabled=NO;
            part.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:part==_rings[i]?.12:.6];
            part.layer.borderColor=UIColor.whiteColor.CGColor;
            part.layer.borderWidth=1.5;
            [self addSubview:part];
        }
    }
    NSArray *labels=@[@"↑",@"↓",@"←",@"→",@"A",@"B",@"X",@"Y",@"L2",@"L1",@"L3",@"Menu",@"RMC",@"R3",@"R1",@"R2",@"L1+A",@"L1+B",@"L1+X",@"L1+Y"];
    NSArray *names=@[@"Up",@"Down",@"Left",@"Right",@"Cross",@"Circle",@"Square",@"Triangle",
        @"Left trigger",@"Left shoulder",@"Left stick click",@"Options",@"Roxas May Cry menu",@"Right stick click",@"Right shoulder",@"Right trigger",
        @"Shortcut L1 + Cross",@"Shortcut L1 + Circle",@"Shortcut L1 + Square",@"Shortcut L1 + Triangle"];
    NSMutableArray *buttons=[NSMutableArray array];
    for(int i=0;i<KH_TOUCH_COUNT;i++){
        UIButton *b=[UIButton buttonWithType:UIButtonTypeCustom];
        b.tag=i; b.accessibilityLabel=names[i];
        [b setTitle:labels[i] forState:UIControlStateNormal];
        b.titleLabel.shadowOffset=CGSizeMake(0,1);
        b.layer.borderWidth=1;
        b.titleLabel.font=[UIFont boldSystemFontOfSize:14];
        b.titleLabel.adjustsFontSizeToFitWidth=YES;
        [b addTarget:self action:@selector(down:) forControlEvents:UIControlEventTouchDown|UIControlEventTouchDragEnter];
        [b addTarget:self action:@selector(up:) forControlEvents:UIControlEventTouchUpInside|UIControlEventTouchUpOutside|UIControlEventTouchCancel|UIControlEventTouchDragExit];
        [self addSubview:b]; [buttons addObject:b];
    }
    _buttons=buttons;
    return self;
}
- (CGFloat)radius {
    return kh_touch_diameter(touch_options.size,self.bounds.size.width,self.bounds.size.height)*.85;
}
- (CGFloat)alphaForPress:(BOOL)pressed {
    double idle=touch_options.opacity/100.;
    return pressed?idle+.7*(1-idle):idle;
}
- (void)drawSticks {
    CGFloat r=self.radius;
    for(int i=0;i<2;i++){
        KHStick s=_sticks[i];
        BOOL active=_finger[i]!=nil;
        double x,y;
        kh_touch_stick_home(i,kh_touch_diameter(touch_options.size,self.bounds.size.width,self.bounds.size.height),
                           self.bounds.size.width,self.bounds.size.height,&x,&y);
        CGPoint center=active?CGPointMake(s.x,s.y):CGPointMake(x,y);
        for(UIView *v in @[_rings[i],_knobs[i]]){
            CGFloat d=v==_rings[i]?2*r:r*.7;
            v.bounds=CGRectMake(0,0,d,d);
            v.center=v==_rings[i]?center:CGPointMake(center.x+s.dx*r,center.y-s.dy*r);
            v.layer.cornerRadius=d/2;
            v.hidden=!active && !touch_options.fixed;
            v.alpha=[self alphaForPress:active];
        }
    }
}
- (void)publish {
    if(self!=touch_surface || UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return;
    unsigned mask=0;
    for(UIButton *b in _buttons) if(b.selected) mask|=kh_touch_mask((int)b.tag);
    kh_input_host_pad_set((unsigned short)mask,(mask&0x10000)?255:0,(mask&0x20000)?255:0,
        pad_axis(_sticks[0].dx),pad_axis(_sticks[0].dy),pad_axis(_sticks[1].dx),pad_axis(_sticks[1].dy));
}
- (void)styleButton:(UIButton *)b {
    CGFloat alpha=[self alphaForPress:b.selected];
    b.backgroundColor=[UIColor.blackColor colorWithAlphaComponent:.7*alpha];
    b.layer.borderColor=[UIColor.whiteColor colorWithAlphaComponent:alpha].CGColor;
    UIColor *ink=b.selected?UIColor.systemYellowColor:UIColor.whiteColor;
    [b setTitleColor:[ink colorWithAlphaComponent:.3+.7*alpha] forState:UIControlStateNormal];
    [b setTitleShadowColor:[UIColor.blackColor colorWithAlphaComponent:.7] forState:UIControlStateNormal];
}
- (void)setButton:(UIButton *)b pressed:(BOOL)pressed {
    if(self!=touch_surface || b.selected==pressed) return;
    b.selected=pressed;
    [self styleButton:b];
    if(pressed){
        [[[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight] impactOccurred];
        kh_menu_input_device("Touch controls",b.accessibilityLabel.UTF8String);
    }
    [self publish];
}
- (void)down:(UIButton *)b { [self setButton:b pressed:YES]; }
- (void)up:(UIButton *)b { [self setButton:b pressed:NO]; }
- (void)reset {
    for(int i=0;i<2;i++){ _finger[i]=nil; _sticks[i]=(KHStick){0}; }
    for(UIButton *b in _buttons){ b.selected=NO; [self styleButton:b]; }
    [self drawSticks];
    if(self==touch_surface) kh_input_host_pad_neutral();
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize size=self.bounds.size;
    if(!CGSizeEqualToSize(size,_layoutSize)){ [self reset]; _layoutSize=size; }
    for(UIButton *b in _buttons){
        int i=(int)b.tag;
        double x,y,d;
        kh_touch_button_frame(i,touch_options.size,size.width,size.height,&x,&y,&d);
        b.bounds=CGRectMake(0,0,d,d); b.center=CGPointMake(x,y); b.layer.cornerRadius=d/2;
        b.hidden=(i<KH_A && !touch_options.dpad) || (i>=KH_SLOT_A && !touch_options.shortcuts);
        [self styleButton:b];
    }
    [self drawSticks];
}
- (void)track:(id)finger point:(CGPoint)p begin:(BOOL)begin {
    int i;
    if(begin){
        i=p.x<self.bounds.size.width/2?0:1;
        if(_finger[i] || self.radius<=0) return;
        CGPoint origin=touch_options.fixed?_rings[i].center:p;
        if(touch_options.fixed && hypot(p.x-origin.x,p.y-origin.y)>self.radius) return;
        _finger[i]=finger; _sticks[i]=(KHStick){origin.x,origin.y,0,0};
        kh_menu_input_device("Touch controls",i?"Look stick":"Move stick");
    }else{
        i=finger==_finger[0]?0:finger==_finger[1]?1:-1;
        if(i<0) return;
    }
    kh_touch_drag(&_sticks[i],p.x,p.y,self.radius,touch_options.fixed,i?touch_options.sensitivity:100);
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for(UITouch *t in touches) [self track:t point:[t locationInView:self] begin:YES];
    [self drawSticks]; [self publish];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for(UITouch *t in touches) [self track:t point:[t locationInView:self] begin:NO];
    [self drawSticks]; [self publish];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for(int i=0;i<2;i++) if(_finger[i] && [touches containsObject:_finger[i]]){
        _finger[i]=nil; _sticks[i]=(KHStick){0};
    }
    [self drawSticks]; [self publish];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
@end

static void pad_release(void){
    pad_active.extendedGamepad.valueChangedHandler=nil;
    pad_active=nil; capture_button=nil;
    kh_input_host_pad_neutral();
    kh_menu_input_device(NULL,NULL);
}
static void pad_select(GCController *controller){
    if(controller==pad_active) return;
    pad_release();
    if(controller) pad_attach(controller);
}
static void reconcile_pads(GCController *excluded){
    if(!game_window) return;
    if(excluded && excluded==pad_active) pad_release();
    GCController *physical=nil;
    for(GCController *c in GCController.controllers){
        if(c==excluded || !c.extendedGamepad) continue;
        if(!physical || c==pad_active) physical=c;
    }
    if(!touch_ready || !kh_touch_visible(touch_options.mode,physical!=nil)){
        if(touch_surface){
            [touch_surface reset]; [touch_surface removeFromSuperview]; touch_surface=nil;
            kh_menu_input_device(NULL,NULL);
        }
        pad_select(physical);
        return;
    }
    pad_select(nil);
    if(!touch_surface){
        kh_input_host_nintendo(-1);
        kh_title_pad_family(0,0);
        kh_menu_input_device("Touch controls",NULL);
        UIView *view=game_window.rootViewController.view;
        touch_surface=[KHTouchSurface new];
        touch_surface.translatesAutoresizingMaskIntoConstraints=NO;
        [view addSubview:touch_surface];
        UILayoutGuide *guide=view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [touch_surface.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor],
            [touch_surface.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor],
            [touch_surface.topAnchor constraintEqualToAnchor:guide.topAnchor],
            [touch_surface.bottomAnchor constraintEqualToAnchor:guide.bottomAnchor]]];
        touch_surface.transform=touch_flipped?CGAffineTransformMakeRotation(M_PI):CGAffineTransformIdentity;
    }
    [touch_surface setNeedsLayout];
#if TARGET_OS_SIMULATOR
    static BOOL tested;
    if(!tested && [NSProcessInfo.processInfo.arguments containsObject:@"--test-controller-pages"] &&
       UIApplication.sharedApplication.applicationState==UIApplicationStateActive){
        tested=YES;
        [game_window.rootViewController.view layoutIfNeeded];
        UIButton *slot=(UIButton *)[touch_surface viewWithTag:KH_SLOT_X];
        UIButton *lb=(UIButton *)[touch_surface viewWithTag:KH_LB];
        BOOL ok=[touch_surface hitTest:slot.center withEvent:nil]==slot;
        extern void kh_input_host_pad_get(unsigned short *,unsigned short *,short *);
        unsigned short held=0;
        [lb sendActionsForControlEvents:UIControlEventTouchDown]; [slot sendActionsForControlEvents:UIControlEventTouchDown]; kh_input_host_pad_get(&held,NULL,NULL);
        ok &= held==0x4100;
        [slot sendActionsForControlEvents:UIControlEventTouchUpInside]; kh_input_host_pad_get(&held,NULL,NULL);
        ok &= held==0x100;
        [lb sendActionsForControlEvents:UIControlEventTouchCancel]; kh_input_host_pad_get(&held,NULL,NULL);
        ok &= held==0;
        // track: uses finger identity only. UIKit supplies the locations in the real event handlers.
        NSObject *left=[NSObject new], *right=[NSObject new];
        CGFloat w=touch_surface.bounds.size.width, h=touch_surface.bounds.size.height;
        [touch_surface track:left point:CGPointMake(w*.2,h*.65) begin:YES];
        [touch_surface track:right point:CGPointMake(w*.8,h*.65) begin:YES];
        [touch_surface track:left point:CGPointMake(w*.8,h*.65) begin:NO];
        [touch_surface track:right point:CGPointMake(w*.8,h*.3) begin:NO];
        [touch_surface publish];
        short lx=0;
        kh_input_host_pad_get(NULL,NULL,&lx);
        ok &= lx==32767;
        [touch_surface touchesEnded:(NSSet *)[NSSet setWithObject:left] withEvent:nil];
        kh_input_host_pad_get(NULL,NULL,&lx);
        ok &= lx==0 && (kh_input_host_pad_neutral()&0x10000)!=0;
        [touch_surface touchesCancelled:(NSSet *)[NSSet setWithObject:right] withEvent:nil];
        ok &= kh_input_host_pad_neutral()==0;
        [touch_surface reset];
        NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/touch-controller.txt"];
        if(![(ok?@"PASS":@"FAIL: shortcut hit target, overlapping chord or independent stick release") writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL])
            fprintf(stderr,"kh: simulator-touch-controller: could not write result\n");
    }
#endif
}
static void touch_settings_changed(KHTouchSettings options){
    dispatch_async(dispatch_get_main_queue(),^{
        [touch_surface reset];
        touch_options=options; touch_ready=YES;
        reconcile_pads(nil);
    });
}
static void setup_controllers(UIWindow *window){
    game_window=window;
    reconcile_pads(nil);
    pad_connect_observer=[NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ reconcile_pads(nil); }];
    pad_disconnect_observer=[NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ reconcile_pads(n.object); }];
}
static void pad_resync(void){
    if(touch_surface) [touch_surface publish];
    else if(pad_active) pad_send(pad_active.extendedGamepad);
    else kh_input_host_pad_neutral();
}

/* Sound: the mixer runs in pull mode (KHNMC_AUDIO=pull unless khnmc.conf says otherwise) and the RemoteIO unit pulls from it.  The
 * session is SoloAmbient, the iOS default for games: it follows the ring/silent switch and stops other apps' audio while it plays.
 * A failed start is reported in the log and the game goes on without sound (the mixer keeps its own clock). */
static _Atomic int sound_started;
/* TASK41: the audio device's start / stop for the life cycle (runtime/app_life.c calls them on its worker thread, never the main one):
 * the session is activated again before the unit starts (iOS deactivates it while the app is in the background) */
static int app_audio_start(void){
    @autoreleasepool {
        NSError *err=nil;
        if(![[AVAudioSession sharedInstance] setActive:YES error:&err]) fprintf(stderr,"kh: ios: audio session activation: %s\n",err.localizedDescription.UTF8String);
        return kh_coreaudio_start();
    }
}
static void app_audio_stop(void){ kh_coreaudio_stop(); }
/* the session is taken only when sound really starts (review r9: a silent or selftest run must not stop another app's music) */
static void start_sound(void){
    NSError *err=nil; AVAudioSession *as=[AVAudioSession sharedInstance];
    if(![as setCategory:AVAudioSessionCategorySoloAmbient error:&err]) fprintf(stderr,"kh: ios: audio session category: %s\n",err.localizedDescription.UTF8String);
    err=nil; if(![as setActive:YES error:&err]) fprintf(stderr,"kh: ios: audio session activation: %s\n",err.localizedDescription.UTF8String);
    if(kh_coreaudio_start()) fprintf(stderr,"kh: ios: the audio unit did not start: no sound in this run\n");
    else atomic_store(&sound_started,1);
}
static void setup_sound_session(void){ kh_audio_pull_ready=start_sound; kh_app_set_audio(app_audio_start,app_audio_stop); }
/* an interruption (a call, an alarm, another app's audio): posted to the runtime's life worker, which stops the unit when it begins and
 * starts it again when it ends, or at the resume when the app was suspended meanwhile (review A41: the main thread used to call
 * setActive: and kh_coreaudio_start / _stop itself, and could block on the CoreAudio lock the worker held inside a slow start) */
static void setup_audio_interruptions(void){
    [[NSNotificationCenter defaultCenter] addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){
            NSUInteger type=[n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
            if(!atomic_load(&sound_started)) return;
            if(type==AVAudioSessionInterruptionTypeBegan){ fprintf(stderr,"kh: ios: audio session interruption began (posted to the life worker)\n"); kh_app_audio_interruption(1); }
            else if(type==AVAudioSessionInterruptionTypeEnded){ fprintf(stderr,"kh: ios: audio session interruption ended (posted to the life worker)\n"); kh_app_audio_interruption(0); }
        }];
}

static void *game_thread(void *arg){
    (void)arg;
    static char name[]="khnmc"; char *argv[]={name,NULL};
    int rc=kh_main(1,argv);                   /* comes back only for an early error (usage, no image); the exit hook has run */
    fflush(stdout); fflush(stderr); _exit(rc);
    return NULL;
}
static void start_game_once(void){
    static int started; if(started) return; started=1;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,KH_APP_THREAD_STACK);
    pthread_t th;
    if(pthread_create(&th,&at,game_thread,NULL)){ perror("khnmc: pthread_create"); _exit(1); }
}

/* ---- TASK41: the display link, on its own thread ---- */
@interface KHDisplayTicker : NSObject
- (void)tick:(CADisplayLink *)link;
@end
static _Atomic int display_link_stopped;
@implementation KHDisplayTicker { int _registered, _label_done; }
- (void)tick:(CADisplayLink *)link {
    int want=kh_present_wants_ticks();
    if(want<0) return;                                         /* start-up has not read KHNMC_GFX_PRESENT yet */
    if(want==0){ [link invalidate]; atomic_store(&display_link_stopped,1); CFRunLoopStop(CFRunLoopGetCurrent());
        fprintf(stderr,"kh: ios: display link stopped: this run does not present to the layer (WaitForVBlank keeps its pacing)\n"); return; }
    if(!_registered){ kh_vsync_register(); _registered=1; fprintf(stderr,"kh: ios: display link: ticks pace WaitForVBlank (60/60/60, its own thread)\n"); }
    kh_vsync_tick(kh_app_host_ns());
    if(!_label_done && kh_present_presented()>0){ _label_done=1; dispatch_async(dispatch_get_main_queue(),^{ test_label.hidden=YES;
        /* owner 2026-11-18 ("need a frame counter"): KHNMC_HUD=1 (khnmc.conf: env KHNMC_HUD=1; read here, after the runtime applied the
         * conf) turns on Apple's Metal performance HUD on the game's layer: frame rate, frame time, GPU time, memory.  iOS 16+. */
        const char *hud=getenv("KHNMC_HUD");
        if(hud && hud[0]=='1' && hud[1]==0){
            if(@available(iOS 16.0,*)){ CAMetalLayer *ml=(CAMetalLayer *)test_label.superview.layer;
                if([ml isKindOfClass:[CAMetalLayer class]]){ ml.developerHUDProperties=@{@"mode":@"default"}; fprintf(stderr,"kh: ios: Metal performance HUD on (KHNMC_HUD=1)\n"); }
                else fprintf(stderr,"kh: ios: KHNMC_HUD=1 but the game view's layer is not a CAMetalLayer: no HUD\n"); }
        } else if(hud && hud[0]) fprintf(stderr,"kh: ios: KHNMC_HUD must be 1 (got '%s'): no HUD\n",hud);
    }); }
}
@end
static void start_display_link(void){
    NSThread *t=[[NSThread alloc] initWithBlock:^{
        CADisplayLink *dl=[CADisplayLink displayLinkWithTarget:[KHDisplayTicker new] selector:@selector(tick:)];
        dl.preferredFrameRateRange=CAFrameRateRangeMake(60,60,60);
        [dl addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
        /* review A41 (verification): -[NSRunLoop run] is not guaranteed to return once the link is gone (Apple: the system may attach other
         * sources; it then BLOCKS in the run loop, it does not spin).  An explicit loop instead: the stop flag and CFRunLoopStop end it */
        while(!atomic_load(&display_link_stopped)&&[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantFuture]]){}
    }];
    t.name=@"khnmc-display-link"; t.qualityOfService=NSQualityOfServiceUserInteractive;
    [t start];
}

/* ---- TASK41: the game view ---- */
@interface KHGameView : UIView
@end
@implementation KHGameView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface KHGameViewController : UIViewController
- (void)reportSize;
- (void)flipForDevice;
- (void)stopMotion;
@end
@implementation KHGameViewController {
    BOOL _started;
    int _upsideDown;
    CMDeviceMotion *_sample;
    CMMotionManager *_motion;
    KHGameView *_picture;
    id _orientationObserver;
    UIInterfaceOrientation _lastInterface;
    UIDeviceOrientation _lastDevice;
    NSString *_orientationLog;
}
- (void)loadView {
    KHGameView *v=[[KHGameView alloc] initWithFrame:CGRectZero];
    v.opaque=YES; v.backgroundColor=[UIColor blackColor];
    v.layer.opaque=YES;
    UILabel *label=[[UILabel alloc] initWithFrame:CGRectZero];
    label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    label.numberOfLines=0; label.textColor=[UIColor lightGrayColor]; label.textAlignment=NSTextAlignmentCenter;
    label.text=@"KHNMC test shell\n\nWhat runs is set by Documents/khnmc.conf.\nThe log and the status file named there are written into Documents.\nThe app closes when the run ends.";
    [v addSubview:label]; test_label=label;
    self.view=[[UIView alloc] initWithFrame:CGRectZero];
    [self.view addSubview:v];
    _picture=v;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
#define KH_IOS_PORTRAIT 1
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return KH_IOS_PORTRAIT ? UIInterfaceOrientationMaskAll : UIInterfaceOrientationMaskLandscape;   /* owner 11/18: upside-down portrait too */ }
/* the drawable's size in pixels (bounds x nativeScale), 0 x 0 before the view has a screen */
- (BOOL)pixelsW:(uint32_t *)w h:(uint32_t *)h {
    UIScreen *scr=self.view.window.windowScene.screen; if(!scr) return NO;
    CGFloat s=scr.nativeScale; if(_picture.contentScaleFactor!=s) _picture.contentScaleFactor=s;
    CGSize b=self.view.bounds.size; *w=(uint32_t)lround(b.width*s); *h=(uint32_t)lround(b.height*s);
    return *w>0 && *h>0;
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _picture.bounds=self.view.bounds;
    _picture.center=CGPointMake(CGRectGetMidX(self.view.bounds),CGRectGetMidY(self.view.bounds));
    [self flipForDevice];
    test_label.frame=CGRectInset(self.view.bounds,24,24);
    uint32_t w=0,h=0; if(![self pixelsW:&w h:&h]) return;          /* wait for a layout with a screen and a size */
    if(!_started){
        _started=YES;
        if(w<h) fprintf(stderr,"kh: ios: the first layout is portrait (%ux%u px): the game's screen follows it\n",w,h);
        kh_present_note_drawable(w,h);                                 /* every app (review A41, verification): KHNMC_SCREEN=layer needs only the size */
        if(kh_present_built()) kh_present_set_layer((__bridge void *)_picture.layer,w,h);   /* before the game reads its screen size */
        fprintf(stderr,"kh: ios: game view %ux%u px (scale %.3f), layer %s; starting the game\n",w,h,_picture.contentScaleFactor,kh_present_built()?"handed to the runtime":"not used (no graphics backend in this app)");
        start_display_link();
        start_game_once();
    } else kh_present_set_size(w,h);
}
- (void)reportSize { uint32_t w=0,h=0; if(_started && [self pixelsW:&w h:&h]) kh_present_set_size(w,h); }
- (void)viewDidLoad {
    [super viewDidLoad];
    [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
    __weak KHGameViewController *ws=self;
    _orientationObserver=[[NSNotificationCenter defaultCenter] addObserverForName:UIDeviceOrientationDidChangeNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ (void)n; [ws flipForDevice]; }];
    _motion=[CMMotionManager new];
    if(_motion.deviceMotionAvailable){
        _motion.deviceMotionUpdateInterval=0.15;
        [_motion startDeviceMotionUpdatesToQueue:NSOperationQueue.mainQueue withHandler:^(CMDeviceMotion *sample,NSError *error){
            if(error){ fprintf(stderr,"kh: ios: orientation motion stopped: %s\n",error.localizedDescription.UTF8String); [ws stopMotion]; }
            else if(sample){ KHGameViewController *vc=ws; if(vc){ vc->_sample=sample; [vc flipForDevice]; } }
        }];
    } else fprintf(stderr,"kh: ios: orientation motion unavailable\n");
}
- (void)stopMotion { [_motion stopDeviceMotionUpdates]; _sample=nil; [self flipForDevice]; }
- (void)dealloc {
    [_motion stopDeviceMotionUpdates];
    if(_orientationObserver) [[NSNotificationCenter defaultCenter] removeObserver:_orientationObserver];
    [[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];
}
- (void)flipForDevice {
    UIDeviceOrientation d=UIDevice.currentDevice.orientation;
    UIInterfaceOrientation io=self.view.window.windowScene.interfaceOrientation;
    CMDeviceMotion *sample=_sample;
    CMAcceleration gravity=sample ? sample.gravity : (CMAcceleration){0};
    // Latch between thresholds so a flat device or a diagonal turn cannot oscillate.
    int device=d==UIDeviceOrientationPortraitUpsideDown ? 1 : d==UIDeviceOrientationPortrait ? -1 : 0;
    // An unchanged upright report is common on this device, even while gravity is inverted.
    if(sample && d==_lastDevice) device=0;
    _lastDevice=d;
    BOOL flip=kh_portrait_flip(io==UIInterfaceOrientationPortrait,&_upsideDown,device,sample!=nil,gravity.x,gravity.y);
    CGAffineTransform t=flip ? CGAffineTransformMakeRotation((CGFloat)M_PI) : CGAffineTransformIdentity;
    if(touch_flipped!=flip){
        [touch_surface reset]; touch_flipped=flip; touch_surface.transform=t;
    }
    if(!CGAffineTransformEqualToTransform(t,_picture.transform)) _picture.transform=t;
    if(_started && io!=UIInterfaceOrientationUnknown && _lastInterface!=io && kh_present_built()){
        uint32_t w,h;
        if([self pixelsW:&w h:&h]) kh_present_set_layer((__bridge void *)_picture.layer,w,h);
    }
    _lastInterface=io;
    NSString *state=[NSString stringWithFormat:@"device=%ld interface=%ld bounds=%@ transform=%@ motion=%d flip=%d",
        (long)d,(long)io,NSStringFromCGRect(self.view.bounds),NSStringFromCGAffineTransform(_picture.transform),sample!=nil,flip];
    if(![state isEqualToString:_orientationLog]){
        _orientationLog=state;
        fprintf(stderr,"kh: ios: orientation %s gravityY=%.3f\n",state.UTF8String,gravity.y);
    }
}
@end

@interface KHSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation KHSceneDelegate { BOOL _left; }
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if(![scene isKindOfClass:[UIWindowScene class]]) return;
    UIWindow *w=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    w.rootViewController=[[KHGameViewController alloc] init]; self.window=w; [w makeKeyAndVisible];
    [UIApplication sharedApplication].idleTimerDisabled=YES;   /* a game: the screen must not dim and lock while it runs (iOS suspends a locked app) */
#if TARGET_OS_SIMULATOR
    sim_controller_pages();
#endif
    setup_controllers(w);
    setup_audio_interruptions();
    /* the game starts after the view's first layout (KHGameViewController viewDidLayoutSubviews) */
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    if(scene!=game_window.windowScene) return;
    [NSNotificationCenter.defaultCenter removeObserver:pad_connect_observer];
    [NSNotificationCenter.defaultCenter removeObserver:pad_disconnect_observer];
    pad_connect_observer=nil; pad_disconnect_observer=nil;
    [touch_surface reset]; [touch_surface removeFromSuperview]; touch_surface=nil;
    game_window=nil; pad_release();
}
- (void)windowScene:(UIWindowScene *)scene didUpdateCoordinateSpace:(id<UICoordinateSpace>)oldSpace interfaceOrientation:(UIInterfaceOrientation)oldOrientation traitCollection:(UITraitCollection *)oldTraits {
    (void)scene; (void)oldSpace; (void)oldOrientation; (void)oldTraits;
    [(KHGameViewController *)self.window.rootViewController flipForDevice];
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene; _left=YES;
    [touch_surface reset];
    kh_app_will_resign_active();                               /* the gate is set and the focus lost; returns at once */
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene; _left=YES;
    kh_app_suspend(KH_APP_SUSPEND_BOUND_MS);                   /* bounded; every expired bound is logged and counted by the runtime */
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    reconcile_pads(nil);
    if(!_left){ pad_resync(); return; }                         /* deliver any buttons held at launch */
    _left=NO;
    [(KHGameViewController *)self.window.rootViewController reportSize];   /* the layer's size before our swapchain is rebuilt */
    kh_app_resume(KH_APP_RESUME_BOUND_MS);                     /* bounded: stale swapchain, the PC's restore, audio (also after an interruption), focus, gate cleared */
    pad_resync();                                              /* every controller's CURRENT state */
}
@end

@interface KHAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation KHAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application; (void)launchOptions;
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c=[[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass=[KHSceneDelegate class];
    return c;
}
@end

/* The game data lives in Documents (data/, pkgdir/, kh2fm.exe: runtime/host_env.c).  iOS backs Documents up to iCloud by default, and
 * 42 GB of game data must never go there: the two folders are created if missing and marked excluded from backup, at every start
 * (a folder's mark covers what is copied into it later), the exe too when it is there.  What happened is written to
 * Documents/khnmc-backup.txt so it can be checked from the Mac (devicectl copy from). */
static void exclude_game_data_from_backup(void){
    NSFileManager *fm=[NSFileManager defaultManager];
    NSString *docs=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
    NSMutableString *out=[NSMutableString stringWithString:@"# written at every start by runtime/ios/ios_main.m\n"];
    for(NSString *name in @[@"data",@"pkgdir",@"kh2fm.exe"]){
        NSString *path=[docs stringByAppendingPathComponent:name]; BOOL dir=NO, ok=NO; NSError *err=nil;
        if(![name hasSuffix:@".exe"] && ![fm fileExistsAtPath:path]) [fm createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:&err];
        if([fm fileExistsAtPath:path isDirectory:&dir]){
            NSURL *u=[NSURL fileURLWithPath:path isDirectory:dir];
            ok=[u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&err];
            NSNumber *v=nil; [u getResourceValue:&v forKey:NSURLIsExcludedFromBackupKey error:nil];
            [out appendFormat:@"%@ excluded-from-backup=%@%@\n",name,(ok&&v.boolValue)?@"yes":@"NO",err?[@" error: " stringByAppendingString:err.localizedDescription]:@""];
        } else [out appendFormat:@"%@ absent\n",name];
    }
    [out writeToFile:[docs stringByAppendingPathComponent:@"khnmc-backup.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

int main(int argc,char *argv[]){
    @autoreleasepool {
        kh_ios_touch_changed=touch_settings_changed;
        exclude_game_data_from_backup();
        setenv("KHNMC_INPUT","host",0);       /* the controller is pad 0 (a route script, when set, wins; never in a selftest) */
        setenv("KHNMC_AUDIO","pull",0);       /* khnmc.conf may still set another mode (env KHNMC_AUDIO=null for silent test runs) */
        setup_sound_session();
        /* the two directories only the system can name; runtime/host_env.c takes everything else from them */
        setenv("KH_APP_HOME",[NSHomeDirectory() fileSystemRepresentation],1);
        NSString *tmp=NSTemporaryDirectory();
        if(tmp.length) setenv("KH_TMPDIR",[tmp fileSystemRepresentation],1);
        return UIApplicationMain(argc,argv,nil,NSStringFromClass([KHAppDelegate class]));
    }
}
