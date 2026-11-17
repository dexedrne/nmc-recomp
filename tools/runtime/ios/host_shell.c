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
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define KH_APP_THREAD_STACK (1u<<20)          /* keep equal to runtime/ios/ios_main.m */
int kh_main(int argc,char **argv);

static void *game_thread(void *arg){
    char **argv=arg; int argc=0; while(argv[argc]) argc++;
    int rc=kh_main(argc,argv);               /* comes back only for an early error (usage, no image); the exit hook has run */
    fflush(stdout); fflush(stderr); _exit(rc);
    return NULL;
}
int main(int argc,char **argv){
    (void)argc;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,KH_APP_THREAD_STACK);
    pthread_t th; if(pthread_create(&th,&at,game_thread,argv)){ perror("host_shell: pthread_create"); return 1; }
    for(;;) pause();                          /* the main thread belongs to the "UI" */
}
