/* A refused gummi start must fail by name; GAME_MODE has no bool refusal ABI. */
#include "mod_config.h"
#include <unistd.h>
#include <sys/wait.h>
static int checks, failures;
#define T(c) do { checks++; if(!(c)){ failures++; fprintf(stderr,"mod hooks selftest FAIL line %d: %s\n",__LINE__,#c); } } while(0)
int kh_mod_hooks_selftest(KCtx *g){
    u64 sp=KH_RSP(g), desc=ga_calloc(1,0x80);
    /* The original's state==3 early return preserves RAX and avoids allocation. */
    const u64 offsets[]={0,0x40};
    for(unsigned i=0;i<sizeof offsets/sizeof *offsets;i++){
        wr32(g,desc+offsets[i]+0x24,3);
        KH_RAX(g)=0x123456789abcdef0ull;
        u64 result=kh_call(g,GS_BASE+0x1505a0,desc+offsets[i],0,0,0);
        T(result==0x123456789abcdef0ull); T(KH_RSP(g)==sp);
        T(rd32(g,desc+offsets[i]+0x24)==3);
    }
    if(!kh_fork_ok()) kh_selftest_not_run("mod hooks","the refused gummi start (named fatal gummi-gate-contract)",7);   /* iOS: no fork (host_env.c) */
    else {
    FILE *capture=kh_tmpfile();
    if(!capture) kh_fatal(g,"mod hooks selftest: tmpfile failed");
    pid_t child=kh_fork();
    if(child<0) kh_fatal(g,"mod hooks selftest: fork failed");
    if(!child){
        if(dup2(fileno(capture),STDERR_FILENO)<0) _exit(98);
        wr32(g,GS_BASE+KH_MOD_GUMMI_TASK+0x24,3);
        KH_RSP(g)=sp-8; wr64(g,sp-8,GS_BASE+0x250230);
        KH_RCX(g)=GS_BASE+KH_MOD_GUMMI_TASK;
        kh_dispatch(g,GS_BASE+0x1505a0);
        _exit(99);
    }
    int status=0; T(waitpid(child,&status,0)==child);
    rewind(capture); char output[8192]; size_t n=fread(output,1,sizeof output-1,capture); output[n]=0; fclose(capture);
    T(WIFEXITED(status) && WEXITSTATUS(status)==3);
    T(strstr(output,"gummi-gate-contract:")!=NULL);
    T(strstr(output,"gummi gate blocked=1 task=0x716c10")!=NULL);
    T(strstr(output,"which original transition restores the world map")!=NULL);
    T(strstr(output,"rva 0x250230")!=NULL);
    T(KH_RSP(g)==sp);
    }
    ga_free(desc);
    fprintf(stderr,"kh: mod hooks selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
