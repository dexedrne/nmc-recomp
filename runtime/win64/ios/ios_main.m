/* runtime/ios/ios_main.m : the iOS app's main() and its minimal UIKit shell (task 26).
 *
 * UNRUN.  Written on 2026-11-17 without an iOS SDK: this file has never been compiled.  Its structure has run, as plain C, on
 * Linux x86-64 and on arm64 macOS (tools/ios/host_shell.c: the same two libraries, kh_main on a second thread with this stack
 * size, the main thread parked, the process ended from the game thread); what is specific to this file is UIKit and Foundation.
 *
 * What it does, and what it leaves to C code that has run (runtime/host_env.c, KH_IOS_PATHS):
 *   - main(): passes the app container (NSHomeDirectory) and its scratch directory (NSTemporaryDirectory) to the runtime as
 *     KH_APP_HOME and KH_TMPDIR, then UIApplicationMain.  Nothing else is decided here.
 *   - when the app has finished launching: one thread runs kh_main() (runtime/loader.c built with -DKH_MAIN_NAME=kh_main).
 *     kh_main reads Documents/khnmc.conf: arguments (`arg --selftest`), variables (`env KH_RUN_GAME_FRAMES=7000`), the log file
 *     and the status file ("running", then "exit N").  So the same binary runs the selftest or a route, and whoever started it
 *     (simctl on a Mac, a person with the Files app) reads the result from Documents.
 *   - the process ends from the game thread (_exit after the status file is written).  For the user that is "the app closed":
 *     right for a test shell, to be replaced when the game has a picture (docs/IOS-SHELL.md: Metal layer, controller, audio).
 *   - a window with one label, because an app without a scene is killed by the system's launch watchdog, and so that a person
 *     who starts it sees where to look.
 * No network, no data in the bundle; the game data in Documents is excluded from backup (above main); sound through RemoteIO
 * (setup_sound_session).  Bundle id
 * beer.vyvanse.khnmc (tools/ios/make_project.py). */
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define KH_APP_THREAD_STACK (1u<<20)          /* keep equal to tools/ios/host_shell.c; the guest threads get their own (loader.c) */
int kh_main(int argc,char **argv);
int kh_coreaudio_start(void);                 /* runtime/ios/audio_coreaudio.c (COREAUDIO=1) */
extern void (*kh_audio_pull_ready)(void);     /* runtime/khnmc_audio_out.h: called once the mixer's pull ring exists */
void kh_input_host_pad_set(unsigned short buttons,unsigned char lt,unsigned char rt,short lx,short ly,short rx,short ry);   /* null_input.c */

/* The controller (GameController framework) is XInput pad 0 (KHNMC_INPUT=host, set below unless khnmc.conf says otherwise): every
 * change of any element sends the whole state.  Buttons by position, as the PC game sees an Xbox pad: A (bottom; cross on a
 * DualSense) = 0x1000, B (right) = 0x2000, X (left) = 0x4000, Y (top) = 0x8000, LB/RB 0x100/0x200, the menu button START 0x10, the
 * options button BACK 0x20, the stick clicks 0x40/0x80, the d-pad 0x1/0x2/0x4/0x8; triggers 0..255; sticks -32767..32767 (up = +). */
static short pad_axis(float v){ if(!(v>=-1.0f)) v=-1.0f; if(v>1.0f) v=1.0f; return (short)lrintf(v*32767.0f); }
static unsigned char pad_trigger(float v){ if(!(v>=0.0f)) v=0.0f; if(v>1.0f) v=1.0f; return (unsigned char)lrintf(v*255.0f); }
static void pad_send(GCExtendedGamepad *p){
    unsigned short b=0;
    if(p.dpad.up.pressed) b|=0x0001; if(p.dpad.down.pressed) b|=0x0002; if(p.dpad.left.pressed) b|=0x0004; if(p.dpad.right.pressed) b|=0x0008;
    if(p.buttonMenu.pressed) b|=0x0010; if(p.buttonOptions.pressed) b|=0x0020;
    if(p.leftThumbstickButton.pressed) b|=0x0040; if(p.rightThumbstickButton.pressed) b|=0x0080;
    if(p.leftShoulder.pressed) b|=0x0100; if(p.rightShoulder.pressed) b|=0x0200;
    if(p.buttonA.pressed) b|=0x1000; if(p.buttonB.pressed) b|=0x2000; if(p.buttonX.pressed) b|=0x4000; if(p.buttonY.pressed) b|=0x8000;
    kh_input_host_pad_set(b,pad_trigger(p.leftTrigger.value),pad_trigger(p.rightTrigger.value),
                          pad_axis(p.leftThumbstick.xAxis.value),pad_axis(p.leftThumbstick.yAxis.value),
                          pad_axis(p.rightThumbstick.xAxis.value),pad_axis(p.rightThumbstick.yAxis.value));
}
static void pad_attach(GCController *c){
    GCExtendedGamepad *p=c.extendedGamepad;
    if(!p){ fprintf(stderr,"kh: ios: controller '%s' has no extended gamepad profile: not used\n",c.vendorName.UTF8String?:"?"); return; }
    p.valueChangedHandler=^(GCExtendedGamepad *gp,GCControllerElement *el){ (void)el; pad_send(gp); };
    pad_send(p);
    fprintf(stderr,"kh: ios: controller connected: %s\n",c.vendorName.UTF8String?:"?");
}
static void setup_controllers(void){
    for(GCController *c in GCController.controllers) pad_attach(c);
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ pad_attach((GCController *)n.object); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
        usingBlock:^(NSNotification *n){ (void)n; kh_input_host_pad_set(0,0,0,0,0,0,0); fprintf(stderr,"kh: ios: controller disconnected: pad released\n"); }];
}

/* Sound: the mixer runs in pull mode (KHNMC_AUDIO=pull unless khnmc.conf says otherwise) and the RemoteIO unit pulls from it.  The
 * session is SoloAmbient, the iOS default for games: it follows the ring/silent switch and stops other apps' audio while it plays.
 * A failed start is reported in the log and the game goes on without sound (the mixer keeps its own clock). */
static void start_sound(void){
    if(kh_coreaudio_start()) fprintf(stderr,"kh: ios: the audio unit did not start: no sound in this run\n");
}
static void setup_sound_session(void){
    NSError *err=nil; AVAudioSession *as=[AVAudioSession sharedInstance];
    if(![as setCategory:AVAudioSessionCategorySoloAmbient error:&err]) fprintf(stderr,"kh: ios: audio session category: %s\n",err.localizedDescription.UTF8String);
    err=nil; if(![as setActive:YES error:&err]) fprintf(stderr,"kh: ios: audio session activation: %s\n",err.localizedDescription.UTF8String);
    kh_audio_pull_ready=start_sound;
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

@interface KHSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation KHSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if(![scene isKindOfClass:[UIWindowScene class]]) return;
    UIWindow *w=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc=[[UIViewController alloc] init];
    vc.view.backgroundColor=[UIColor blackColor];
    UILabel *label=[[UILabel alloc] initWithFrame:CGRectInset(vc.view.bounds,24,24)];
    label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    label.numberOfLines=0; label.textColor=[UIColor lightGrayColor]; label.textAlignment=NSTextAlignmentCenter;
    label.text=@"KHNMC test shell\n\nWhat runs is set by Documents/khnmc.conf.\nThe log and the status file named there are written into Documents.\nThe app closes when the run ends.";
    [vc.view addSubview:label];
    w.rootViewController=vc; self.window=w; [w makeKeyAndVisible];
    [UIApplication sharedApplication].idleTimerDisabled=YES;
    setup_controllers();   /* a game: the screen must not dim and lock while it runs (iOS suspends a locked app) */
    start_game_once();
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
