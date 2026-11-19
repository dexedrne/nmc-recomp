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
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
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
static UILabel *test_label,*options_label;
static NSMutableDictionary<NSString *,NSString *> *chosen_options;
static int selected_option,selected_component;
static unsigned short previous_buttons;
static BOOL suppress_buttons;
static NSString *option_value(int index){
    const struct kh_option *o=&kh_options[index];
    if(!strcmp(o->name,"KHNMC_PLAYER_LOOK")) return kh_settings_look()?@"xion":@"roxas";
    NSString *chosen=chosen_options[@(o->name)];
    const char *configured=getenv(o->name);
    return chosen ?: @(configured ?: o->initial);
}
static void show_options(NSString *error){
    NSMutableString *text=[NSMutableString stringWithString:@"Options\n\n"];
    int first=MAX(0,MIN(selected_option-2,kh_setting_menu_count()-5));
    for(int i=first;i<MIN(first+5,kh_setting_menu_count());i++){
        const char *reason=kh_setting_unavailable(i);
        [text appendFormat:@"%@%@   %@\n",i==selected_option?@"▶  ":@"     ",@(kh_options[i].label),reason?[NSString stringWithFormat:@"Unavailable (%s)",reason]:option_value(i)];
    }
    [text appendFormat:@"\n%@\nD-pad to select and change   •   Circle to close",
     error ?: (kh_setting_action(selected_option)?@"Cross to execute when play resumes":kh_setting_live(selected_option)?@"Applies immediately":@"Applies on the next launch")];
    if(strchr(kh_options[selected_option].initial,',')){
        [text appendFormat:@"\nL1/R1 to select value %d",selected_component+1];
    }
    options_label.text=text;
}
static void adjust_option(int direction){
    if(kh_setting_unavailable(selected_option)) return;
    const struct kh_option *o=&kh_options[selected_option];
    NSString *current=option_value(selected_option),*value;
    if(o->choices){
        NSArray<NSString *> *choices=[@(o->choices) componentsSeparatedByString:@"|"];
        NSInteger index=[choices indexOfObject:current];
        if(index==NSNotFound) index=0;
        index=(index+direction+(NSInteger)choices.count)%(NSInteger)choices.count;
        value=choices[index];
    }else{
        NSMutableArray<NSString *> *parts=[[current componentsSeparatedByString:@","] mutableCopy];
        double next=fmax(o->minimum,fmin(o->maximum,parts[selected_component].doubleValue+direction*o->step));
        parts[selected_component]=[NSString stringWithFormat:@"%.8f",next];
        NSString *part=parts[selected_component];
        while([part hasSuffix:@"0"]) part=[part substringToIndex:part.length-1];
        if([part hasSuffix:@"."]) part=[part substringToIndex:part.length-1];
        parts[selected_component]=part;
        value=[parts componentsJoinedByString:@","];
    }
    if(!kh_setting_action(selected_option) && kh_setting_set(o->name,value.UTF8String)){show_options(@"Could not save this value");return;}
    chosen_options[@(o->name)]=value;
    show_options(nil);
}
static BOOL options_input(unsigned short buttons){
    unsigned short down=buttons & ~previous_buttons;
    previous_buttons=buttons;
    if(suppress_buttons){if(!buttons) suppress_buttons=NO;return YES;}
    if(UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return YES;
    if(options_label){
        if(down&0x2000){
            [options_label removeFromSuperview]; options_label=nil; suppress_buttons=YES;
            kh_app_resume(KH_APP_RESUME_BOUND_MS);
        }else if((down&0x1000) && kh_setting_action(selected_option)){
            int result=kh_setting_set(kh_options[selected_option].name,option_value(selected_option).UTF8String);
            if(result==-2) show_options(@(kh_setting_unavailable(selected_option) ?: "Unavailable"));
            else if(result) show_options(@"A command is already queued");
            else show_options(@"Queued. Circle to resume.");
        }else if(down&3){
            selected_option=(selected_option+((down&1)?-1:1)+kh_setting_menu_count())%kh_setting_menu_count();
            selected_component=0; show_options(nil);
        }else if(down&0x300){
            int count=(int)[option_value(selected_option) componentsSeparatedByString:@","].count;
            selected_component=(selected_component+((down&0x200)?1:-1)+count)%count;show_options(nil);
        }else if(down&12) adjust_option((down&8)?1:-1);
        return YES;
    }
    /* Hold L3 first, then press the DualSense Options button (GC buttonMenu). */
    if((buttons&0x40) && (down&0x10) && test_label.superview && kh_app_game_presented()){
        const char *life=getenv("KHNMC_APP_LIFE");
        if(life && !strcmp(life,"off")) return NO;
        kh_input_host_pad_set(0,0,0,0,0,0,0);
        kh_app_will_resign_active();
        kh_app_suspend(KH_APP_SUSPEND_BOUND_MS);
        UIView *parent=test_label.superview;
        options_label=[[UILabel alloc] initWithFrame:parent.bounds];
        options_label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        options_label.backgroundColor=[UIColor colorWithWhite:0.05 alpha:0.96];
        options_label.textColor=UIColor.whiteColor; options_label.textAlignment=NSTextAlignmentCenter;
        options_label.numberOfLines=0; options_label.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        options_label.adjustsFontSizeToFitWidth=YES; options_label.minimumScaleFactor=0.7;
        [parent addSubview:options_label];
        if(!chosen_options) chosen_options=[NSMutableDictionary new];
        const char *status=kh_cut_menu_status();
        show_options(status?@(status):nil);
        return YES;
    }
    return NO;
}

static unsigned short pad_buttons(GCExtendedGamepad *p){
    unsigned short b=0;
    if(p.dpad.up.pressed) b|=0x0001; if(p.dpad.down.pressed) b|=0x0002; if(p.dpad.left.pressed) b|=0x0004; if(p.dpad.right.pressed) b|=0x0008;
    if(p.buttonMenu.pressed) b|=0x0010; if(p.buttonOptions.pressed) b|=0x0020;
    if(p.leftThumbstickButton.pressed) b|=0x0040; if(p.rightThumbstickButton.pressed) b|=0x0080;
    if(p.leftShoulder.pressed) b|=0x0100; if(p.rightShoulder.pressed) b|=0x0200;
    if(p.buttonA.pressed) b|=0x1000; if(p.buttonB.pressed) b|=0x2000; if(p.buttonX.pressed) b|=0x4000; if(p.buttonY.pressed) b|=0x8000;
    return b;
}
static void pad_send(GCExtendedGamepad *p){
    unsigned short b=pad_buttons(p);
    if(options_input(b)) return;
    kh_input_host_pad_set(b,pad_trigger(p.leftTrigger.value),pad_trigger(p.rightTrigger.value),
                          pad_axis(p.leftThumbstick.xAxis.value),pad_axis(p.leftThumbstick.yAxis.value),
                          pad_axis(p.rightThumbstick.xAxis.value),pad_axis(p.rightThumbstick.yAxis.value));
}
/* ONE controller drives pad 0 (review r9: a second controller, or the disconnect of another one, must not change or release it): the
 * first connected controller with an extended gamepad profile; when it disconnects, the pad is released and the next connected one
 * (if any) takes over.  Everything here runs on the main queue. */
static GCController *pad_active;
static int pad_attach(GCController *c){
    GCExtendedGamepad *p=c.extendedGamepad;
    if(!p){ fprintf(stderr,"kh: ios: controller '%s' has no extended gamepad profile: not used\n",c.vendorName.UTF8String?:"?"); return 0; }
    if(pad_active && pad_active!=c){ fprintf(stderr,"kh: ios: controller '%s' connected: not used (another controller is pad 0)\n",c.vendorName.UTF8String?:"?"); return 0; }
    pad_active=c;
    c.handlerQueue=dispatch_get_main_queue();
    p.valueChangedHandler=^(GCExtendedGamepad *gp,GCControllerElement *el){ (void)el; if(gp==pad_active.extendedGamepad) pad_send(gp); };
    previous_buttons=pad_buttons(p);
    pad_send(p);
    fprintf(stderr,"kh: ios: controller connected: %s (pad 0)\n",c.vendorName.UTF8String?:"?");
    return 1;
}
static void setup_controllers(void){
    for(GCController *c in GCController.controllers) if(pad_attach(c)) break;
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ pad_attach((GCController *)n.object); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){
            GCController *c=(GCController *)n.object; if(c!=pad_active) return;
            if(c.extendedGamepad) c.extendedGamepad.valueChangedHandler=nil;
            pad_active=nil; previous_buttons=0; suppress_buttons=NO; kh_input_host_pad_set(0,0,0,0,0,0,0); fprintf(stderr,"kh: ios: controller disconnected: pad 0 released\n");
            for(GCController *o in GCController.controllers) if(o!=c && pad_attach(o)) break; }];
}
/* TASK41: after a suspension the CURRENT state of pad 0 (a button that came up meanwhile sent no event), or neutral without one */
static void pad_resync(void){
    if(pad_active && pad_active.extendedGamepad) pad_send(pad_active.extendedGamepad); else kh_input_host_pad_set(0,0,0,0,0,0,0);
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
    setup_controllers();
    setup_audio_interruptions();
    /* the game starts after the view's first layout (KHGameViewController viewDidLayoutSubviews) */
}
- (void)windowScene:(UIWindowScene *)scene didUpdateCoordinateSpace:(id<UICoordinateSpace>)oldSpace interfaceOrientation:(UIInterfaceOrientation)oldOrientation traitCollection:(UITraitCollection *)oldTraits {
    (void)scene; (void)oldSpace; (void)oldOrientation; (void)oldTraits;
    [(KHGameViewController *)self.window.rootViewController flipForDevice];
}
- (void)sceneWillResignActive:(UIScene *)scene {
    (void)scene; _left=YES;
    if(options_label) return;
    kh_app_will_resign_active();                               /* the gate is set and the focus lost; returns at once */
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene; _left=YES;
    if(options_label) return;
    kh_app_suspend(KH_APP_SUSPEND_BOUND_MS);                   /* bounded; every expired bound is logged and counted by the runtime */
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    (void)scene;
    if(!_left) return;                                         /* the first activation at launch: nothing was suspended */
    _left=NO;
    previous_buttons=pad_buttons(pad_active.extendedGamepad);
    if(options_label) return;
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
