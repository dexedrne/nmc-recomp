/* asprobe.c : address-space probe for task 20 (arm64 macOS). Prints this process's regions (mach_vm_region_recurse), then tries to
 * reserve the guest ranges of the Linux layout at their identity addresses with mach_vm_allocate(VM_FLAGS_FIXED) (fails instead of
 * replacing, the Darwin counterpart of MAP_FIXED_NOREPLACE).  Optional argv[1]=pause: sleeps so that vmmap can look at the pid. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <errno.h>
static void regions(const char *title){
    printf("---- regions below 64 GiB (%s); tag 30 = stack, 32 / 35 = dyld shared cache, 1..11 = malloc ----\n",title);
    mach_vm_address_t a=0; mach_vm_size_t sz=0; natural_t depth=0; mach_vm_address_t prev_end=0; unsigned long long above=0;
    for(;;){
        vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt=VM_REGION_SUBMAP_INFO_COUNT_64; depth=0;
        kern_return_t kr=mach_vm_region_recurse(mach_task_self(),&a,&sz,&depth,(vm_region_recurse_info_t)&info,&cnt);
        if(kr!=KERN_SUCCESS) break;
        if(a>=0x1000000000ull){ above++; a+=sz; continue; }
        if(a>prev_end) printf("   free  %012llx-%012llx  %9.1f MiB\n",(unsigned long long)prev_end,(unsigned long long)a,(a-prev_end)/1048576.0);
        printf("   used  %012llx-%012llx  %9.1f MiB prot=%d/%d tag=%u %s\n",(unsigned long long)a,(unsigned long long)(a+sz),sz/1048576.0,info.protection,info.max_protection,info.user_tag,info.is_submap?"submap":"");
        prev_end=a+sz; a+=sz;
    }
    printf("   (%llu more regions at or above 64 GiB)\n",above);
}
static void try_fixed(unsigned long long at,unsigned long long size,const char *what){
    mach_vm_address_t a=at; kern_return_t kr=mach_vm_allocate(mach_task_self(),&a,size,VM_FLAGS_FIXED);
    printf("reserve %-34s 0x%09llx..0x%09llx (%6llu MiB): %s",what,at,at+size,size>>20,kr==KERN_SUCCESS?"OK":mach_error_string(kr));
    if(kr==KERN_SUCCESS){ int r=mprotect((void*)a,size,PROT_NONE); printf("  (mprotect NONE %s)",r?strerror(errno):"ok"); mach_vm_deallocate(mach_task_self(),a,size); }
    printf("\n");
}
static void *thr(void *p){ int local; printf("second thread stack near %p\n",(void*)&local); return p; }
int main(int argc,char **argv){
    int local; void *m=malloc(64),*big=malloc(64<<20);
    printf("pid %d  page size %ld\n",getpid(),sysconf(_SC_PAGESIZE));
    printf("main executable header %p slide 0x%lx\n",(void*)_dyld_get_image_header(0),(long)_dyld_get_image_vmaddr_slide(0));
    printf("main stack near %p, small malloc %p, 64 MiB malloc %p, mmap(anon 1 MiB) ",(void*)&local,m,big);
    void *mm=mmap(0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0); printf("%p\n",mm);
    { task_dyld_info_data_t di; mach_msg_type_number_t c=TASK_DYLD_INFO_COUNT; if(task_info(mach_task_self(),TASK_DYLD_INFO,(task_info_t)&di,&c)==KERN_SUCCESS){
        const struct dyld_all_image_infos *ai=(const struct dyld_all_image_infos*)di.all_image_info_addr;
        printf("dyld shared cache base 0x%llx (slide 0x%llx)\n",(unsigned long long)ai->sharedCacheBaseAddress,(unsigned long long)ai->sharedCacheSlide); } }
    pthread_t t; pthread_create(&t,0,thr,0); pthread_join(t,0);
    regions("as started");
    try_fixed(0x140000000ull,0x2d30000ull,"image (+1 MiB slack)");
    try_fixed(0x140000000ull,58ull<<25,"image + id zone (58 windows)");
    try_fixed(0x140000000ull,0x40000000ull,"image .. 0x180000000 (32 windows)");
    try_fixed(0x142d30000ull,0x180000000ull-0x142d30000ull,"0x142d30000 .. 0x180000000");
    try_fixed(0x180000000ull,0x34000000ull,"id zone part inside 0x180000000..0x1b4000000");
    try_fixed(0x1b4000000ull,0x440000000ull-0x1b4000000ull,"far zone (Linux place)");
    try_fixed(0x140000000ull,0x300000000ull,"whole Linux layout (12 GiB)");
    try_fixed(0x1000000000ull,0x300000000ull,"12 GiB at 0x1000000000");
    { void *h=mmap((void*)0x140000000ull,58ull<<25,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0); printf("mmap hint 0x140000000, 58 windows, PROT_NONE -> %p\n",h); if(h!=MAP_FAILED) munmap(h,58ull<<25); }
    { void *h=mmap(0,0x300000000ull,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0); printf("mmap anywhere, 12 GiB, PROT_NONE -> %p\n",h); if(h!=MAP_FAILED) munmap(h,0x300000000ull); }
    if(argc>1&&!strcmp(argv[1],"pause")){ fflush(stdout); sleep(atoi(argc>2?argv[2]:"20")); }
    return 0;
}
