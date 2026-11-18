/* tools/ios/host_shell.c : the iOS app shell with everything that needs UIKit taken out (task 26), for Linux and macOS.
 *
 * An iOS app owns main(): UIApplicationMain never returns and keeps the main thread, so the game runs on a second thread and
 * the runtime's own main() is linked as a function (runtime/loader.c with -DKH_MAIN_NAME=kh_main, in libkhnmc-rt.a).  This file
 * is that structure and nothing else, so that it can be linked and run where there is no iOS SDK (`make IOS_PATHS=1 hostshell`):
 *   - the same two archives the app links: libkhnmc-rt.a (every hand-written object, force-loaded) and libkh2loose.a (the shards);
 *   - kh_main() on a thread that is not the process's main thread, with the stack the app gives it (runtime/ios/ios_main.m:
 *     KH_APP_THREAD_STACK), while the main thread only waits, as it does inside UIApplicationMain;
 *   - the process ends from the game thread (kh_host_exit -> _exit), not by main() returning.
 * What the game is told to do comes from <KH_APP_HOME>/Documents/khnmc.conf (runtime/host_env.c), exactly as in the app.
 * tools/ios_paths_selftest.sh and tools/route_one.py --ios-paths start it like any KH_IOS_PATHS binary. */
/* TASK41: the app now starts the game after its view's first layout, once it has handed the layer's drawable size to the runtime
 * (kh_present_set_layer; `env KHNMC_SCREEN=layer` sizes the game's screen from it, runtime/null_window.c).  The same order here:
 * KH_HOSTSHELL_DRAWABLE=WxH hands over that size (no layer: Linux has no CAMetalLayer) before the game thread starts.  The app's life
 * cycle (runtime/khnmc_app.h) is driven on Linux by the runtime's KHNMC_TEST_SUSPEND trigger, a host thread that calls the same
 * kh_app_will_resign_active / kh_app_suspend / kh_app_resume the scene delegate calls. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define KH_APP_THREAD_STACK (1u<<20)          /* keep equal to runtime/ios/ios_main.m */
int kh_main(int argc,char **argv);
void kh_present_set_layer(void *ca_metal_layer,uint32_t w_px,uint32_t h_px);   /* runtime/gfx_present.h */
void kh_present_note_drawable(uint32_t w_px,uint32_t h_px);
int  kh_present_built(void);

static void *game_thread(void *arg){
    char **argv=arg; int argc=0; while(argv[argc]) argc++;
    int rc=kh_main(argc,argv);               /* comes back only for an early error (usage, no image); the exit hook has run */
    fflush(stdout); fflush(stderr); _exit(rc);
    return NULL;
}
int main(int argc,char **argv){
    (void)argc;
    const char *d=getenv("KH_HOSTSHELL_DRAWABLE"); unsigned w=0,h=0; char x=0;
    if(d&&*d){ if(sscanf(d,"%u%c%u",&w,&x,&h)!=3||x!='x'||!w||!h){ fprintf(stderr,"host_shell: KH_HOSTSHELL_DRAWABLE=%s: expected WxH\n",d); return 2; }
        kh_present_note_drawable(w,h); if(kh_present_built()) kh_present_set_layer(NULL,w,h); }   /* as the app after its first layout: the size before the game starts */
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,KH_APP_THREAD_STACK);
    pthread_t th; if(pthread_create(&th,&at,game_thread,argv)){ perror("host_shell: pthread_create"); return 1; }
    for(;;) pause();                          /* the main thread belongs to the "UI" */
}
