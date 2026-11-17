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
 * No entitlements, no network, no data in the bundle.  Bundle id beer.vyvanse.khnmc (tools/ios/make_project.py). */
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define KH_APP_THREAD_STACK (1u<<20)          /* keep equal to tools/ios/host_shell.c; the guest threads get their own (loader.c) */
int kh_main(int argc,char **argv);

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

int main(int argc,char *argv[]){
    @autoreleasepool {
        /* the two directories only the system can name; runtime/host_env.c takes everything else from them */
        setenv("KH_APP_HOME",[NSHomeDirectory() fileSystemRepresentation],1);
        NSString *tmp=NSTemporaryDirectory();
        if(tmp.length) setenv("KH_TMPDIR",[tmp fileSystemRepresentation],1);
        return UIApplicationMain(argc,argv,nil,NSStringFromClass([KHAppDelegate class]));
    }
}
