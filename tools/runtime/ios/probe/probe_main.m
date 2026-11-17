/* tools/ios/probe/probe_main.m : the device probe as an app (task 26).  SOURCE ONLY, UNRUN: never compiled (no iOS SDK where it
 * was written), never installed.  The measuring part, tools/ios/probe/probe_core.c, has run on Linux and on macOS.
 *
 * For the owner, later, on the real iPhone (tools/ios/make_project.py --probe writes the Xcode project; bundle id
 * beer.vyvanse.khnmc.probe; no entitlements):
 *   1. build and install it the way the owner installs his own apps (signing is his step; nothing here signs);
 *   2. start it once.  It writes Documents/probe.txt and shows "done".  The last part commits and writes memory in 64 MiB steps
 *      up to PROBE_TOUCH_MIB (default 1,536 MiB; the field route touches about 912 MiB).  If the system ends the app first, that
 *      is an answer too: the last line of probe.txt says how far it got.  To go further, put a file `probe-touch-mib.txt` with one
 *      number (for example 4096) into the app's Documents before starting it;
 *   3. fetch probe.txt (Files app: On My iPhone > KHNMC Probe; or Finder file sharing) and send it back;
 *   4. optionally a second build with probe.entitlements.example (extended virtual addressing, increased memory limit: paid team
 *      only), to see what the entitlements change.
 * It uses no network, reads nothing but its own Documents, and holds no game data. */
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef PROBE_TOUCH_MIB
#define PROBE_TOUCH_MIB 1536
#endif
int khnmc_probe(const char *out_path,long touch_mib);
static UILabel *probe_label;

static void *probe_thread(void *arg){
    (void)arg;
    NSString *docs=[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
    long touch=PROBE_TOUCH_MIB;
    NSString *t=[NSString stringWithContentsOfFile:[docs stringByAppendingPathComponent:@"probe-touch-mib.txt"] encoding:NSUTF8StringEncoding error:nil];
    if(t.length){ long v=strtol(t.UTF8String,NULL,10); if(v>=0&&v<=12288) touch=v; }
    int rc=khnmc_probe([[docs stringByAppendingPathComponent:@"probe.txt"] fileSystemRepresentation],touch);
    dispatch_async(dispatch_get_main_queue(),^{ probe_label.text=rc?@"KHNMC probe: could not write Documents/probe.txt":@"KHNMC probe: done.\n\nFetch Documents/probe.txt (Files app, or Finder file sharing)."; });
    return NULL;
}

@interface KHProbeSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation KHProbeSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if(![scene isKindOfClass:[UIWindowScene class]]) return;
    UIWindow *w=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc=[[UIViewController alloc] init];
    vc.view.backgroundColor=[UIColor blackColor];
    probe_label=[[UILabel alloc] initWithFrame:CGRectInset(vc.view.bounds,24,24)];
    probe_label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    probe_label.numberOfLines=0; probe_label.textColor=[UIColor lightGrayColor]; probe_label.textAlignment=NSTextAlignmentCenter;
    probe_label.text=@"KHNMC probe: measuring.\n\nIf the app closes by itself, that is a result too:\nDocuments/probe.txt says how far it got.";
    [vc.view addSubview:probe_label];
    w.rootViewController=vc; self.window=w; [w makeKeyAndVisible];
    static int started; if(!started){ started=1; pthread_t th; pthread_create(&th,NULL,probe_thread,NULL); }
}
@end

@interface KHProbeAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation KHProbeAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions { (void)application; (void)launchOptions; return YES; }
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c=[[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass=[KHProbeSceneDelegate class];
    return c;
}
@end

int main(int argc,char *argv[]){
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass([KHProbeAppDelegate class])); }
}
