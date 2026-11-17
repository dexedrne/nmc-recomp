/* tools/ios/probe/probe_host.c : run the device probe's measuring part on a desk machine (a test of the PROGRAM, not an answer
 * about an iPhone).   cc -O1 -o probe_host probe_host.c probe_core.c -lpthread && ./probe_host OUT.txt TOUCH_MIB
 * TOUCH_MIB = how many MiB part 2 commits and writes (0 = skip; keep it small on a shared machine). */
#include <stdio.h>
#include <stdlib.h>
int khnmc_probe(const char *out_path,long touch_mib);
int main(int argc,char **argv){
    if(argc!=3){ fprintf(stderr,"usage: %s OUT.txt TOUCH_MIB\n",argv[0]); return 2; }
    return khnmc_probe(argv[1],atol(argv[2]));
}
