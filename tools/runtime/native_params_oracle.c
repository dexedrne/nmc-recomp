/* Task 11 oracle: execute ONLY the shipped KH2Tweaks_Refactor.dll's two handler functions (range dispatcher DLL RVA 0x2df50, frame
 * dispatcher 0x2ec30) natively on x86-64, never its initializer or DllMain.  The image is copied into private memory, the DLL's
 * function-pointer slots (.data/.bss 0x81d48..0x82978) are pointed at recording stubs instead of exe code, the std::cout logging
 * helpers and the security-cookie check are patched to no-ops, and the DLL's snprintf helper is replaced by a checked host snprintf.
 * The same generated cases as runtime/mod_params_test.c are run and the canonical transcript hash printed; kinds 64/65 (std::unordered_map
 * + TLS static init, which cannot be executed natively without a Windows TEB) are excluded on both sides.
 *
 * Build: gcc -O2 -I runtime tools/native_params_oracle.c -o /tmp/native_params_oracle
 * Run:   /tmp/native_params_oracle /path/to/KH2Tweaks_Refactor.dll [transcript.txt]     (static reading of the file only; nothing else is run) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "mod_params_cases.h"
#if !defined(__x86_64__)
#error This oracle executes original x86-64 instructions and requires x86-64.
#endif

struct frame { uint64_t id, rcx, rdx, r8, r9, arg5, x0, x1, x2, x3, ret; };
static const struct { uint32_t slot, exe; } slots[]={
    {0x82598,0x39b760},{0x824d0,0x3c6dc0},{0x824e0,0x3c8310},{0x82978,0x3bde30},{0x825d8,0x3d74e0},{0x825c8,0x3d78d0},{0x82808,0x3c0ad0},
    {0x827f8,0x3c0c30},{0x82850,0x3c0cd0},{0x826a8,0x4ad240},{0x829e0,0x3e1840},{0x82530,0x3b3e80},{0x82038,0x3cb9f0},{0x82058,0x40d810},
    {0x82510,0x3b4da0},{0x82538,0x3d9fe0},{0x828a8,0x3eaae0},{0x82918,0x3db2f0},{0x81d48,0x1a85e0},{0x82000,0x409b00},{0x82150,0x3b68b0},{0x82050,0x40dd70},
};
#define NSLOT ((int)(sizeof slots/sizeof *slots))
#define ID_SNPRINTF 100

static uint8_t *image; static size_t image_size;
static const struct mpc_case *cur;
static struct kh_mp_call calls[64]; static int ncalls; static int errors;

uint64_t stub_c(struct frame *f){
    if(f->id==ID_SNPRINTF){
        const char *fmt=(const char *)f->r8;
        if(f->rdx!=5 || strcmp(fmt,"%04d")){ fprintf(stderr,"oracle: DLL snprintf helper called with n=%llu fmt='%s' (expected 5, %%04d)\n",(unsigned long long)f->rdx,fmt); errors++; }
        return (uint64_t)snprintf((char*)f->rcx,(size_t)f->rdx,fmt,(int)(uint32_t)f->r9);
    }
    if(f->id>=(uint64_t)NSLOT){ fprintf(stderr,"oracle: bad stub id %llu\n",(unsigned long long)f->id); errors++; return 0; }
    struct kh_mp_call *c=&calls[ncalls<63?ncalls:63]; ncalls++;
    memset(c,0,sizeof *c);
    c->rva=slots[f->id].exe; c->a[0]=f->rcx; c->a[1]=f->rdx; c->a[2]=f->r8; c->a[3]=f->r9; c->a[4]=f->arg5;
    c->f2=(uint32_t)f->x2; c->f3=(uint32_t)f->x3; c->x0=f->x0;
    if(c->rva==0x3e1840 && c->a[2]) memcpy(c->mem,(void*)c->a[2],24);
    if(c->rva==0x3db2f0 && c->a[1]){ uint64_t v=0; memcpy(&v,(void*)c->a[1],5); c->mem[0]=v; }
    return mpc_fake(cur,c->rva);
}
/* Win64-ABI stub entry: r11d = stub id.  Saves the Win64 callee-saved registers the SysV C code may clobber (rsi, rdi, xmm6-15). */
__asm__(".text\n.globl stub_common\nstub_common:\n"
        "push %rbp\nmov %rsp,%rbp\npush %rsi\npush %rdi\nsub $0x100,%rsp\n"
        "mov %r11,0x00(%rsp)\nmov %rcx,0x08(%rsp)\nmov %rdx,0x10(%rsp)\nmov %r8,0x18(%rsp)\nmov %r9,0x20(%rsp)\n"
        "mov 0x30(%rbp),%rax\nmov %rax,0x28(%rsp)\n"
        "movq %xmm0,0x30(%rsp)\nmovq %xmm1,0x38(%rsp)\nmovq %xmm2,0x40(%rsp)\nmovq %xmm3,0x48(%rsp)\n"
        "movups %xmm6,0x60(%rsp)\nmovups %xmm7,0x70(%rsp)\nmovups %xmm8,0x80(%rsp)\nmovups %xmm9,0x90(%rsp)\nmovups %xmm10,0xa0(%rsp)\n"
        "movups %xmm11,0xb0(%rsp)\nmovups %xmm12,0xc0(%rsp)\nmovups %xmm13,0xd0(%rsp)\nmovups %xmm14,0xe0(%rsp)\nmovups %xmm15,0xf0(%rsp)\n"
        "mov %rsp,%rdi\ncall stub_c\n"
        "movups 0x60(%rsp),%xmm6\nmovups 0x70(%rsp),%xmm7\nmovups 0x80(%rsp),%xmm8\nmovups 0x90(%rsp),%xmm9\nmovups 0xa0(%rsp),%xmm10\n"
        "movups 0xb0(%rsp),%xmm11\nmovups 0xc0(%rsp),%xmm12\nmovups 0xd0(%rsp),%xmm13\nmovups 0xe0(%rsp),%xmm14\nmovups 0xf0(%rsp),%xmm15\n"
        "lea -16(%rbp),%rsp\npop %rdi\npop %rsi\npop %rbp\nret\n");
extern void stub_common(void);
/* SysV -> Win64 call: rdi=fn, rsi=rcx arg, rdx=rdx arg, ecx=xmm2 bits */
extern void call_win64(void *fn,void *rcx,void *rdx,uint32_t xmm2);
__asm__(".text\n.globl call_win64\ncall_win64:\n"
        "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $0x48,%rsp\n"
        "movd %ecx,%xmm2\nmov %rsi,%rcx\ncall *%rdi\n"
        "add $0x48,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n");

static uint32_t r32(const uint8_t *p){ uint32_t x; memcpy(&x,p,4); return x; }
static uint16_t r16(const uint8_t *p){ uint16_t x; memcpy(&x,p,2); return x; }
static uint8_t *stubs; static int nstubs;
static uint64_t make_stub(uint32_t id){
    uint8_t *p=stubs+32*(size_t)nstubs++; uint64_t target=(uint64_t)(void*)stub_common;
    p[0]=0x41; p[1]=0xBB; memcpy(p+2,&id,4);             /* mov r11d,id */
    p[6]=0x49; p[7]=0xBA; memcpy(p+8,&target,8);          /* mov r10,stub_common */
    p[16]=0x41; p[17]=0xFF; p[18]=0xE2;                   /* jmp r10 */
    return (uint64_t)(void*)p;
}
static void patch_jmp(uint32_t rva,uint32_t id){ uint64_t t=make_stub(id); memcpy(image+rva,(void*)(uintptr_t)(t),0); /* placeholder to keep types honest */
    uint8_t *p=image+rva; uint64_t target=(uint64_t)(void*)stub_common;
    p[0]=0x41; p[1]=0xBB; memcpy(p+2,&id,4); p[6]=0x49; p[7]=0xBA; memcpy(p+8,&target,8); p[16]=0x41; p[17]=0xFF; p[18]=0xE2; }

int main(int argc,char **argv){
    if(argc<2){ fprintf(stderr,"usage: %s DLL [transcript.txt]\n",argv[0]); return 2; }
    FILE *f=fopen(argv[1],"rb"); if(!f){ perror(argv[1]); return 2; }
    fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    uint8_t *file=malloc((size_t)len); if(!file||fread(file,1,(size_t)len,f)!=(size_t)len){ fprintf(stderr,"read failed\n"); return 2; }
    fclose(f);
    uint32_t pe=r32(file+0x3c);
    if(pe>(size_t)len-0x100||memcmp(file+pe,"PE\0\0",4)){ fprintf(stderr,"invalid PE\n"); return 2; }
    unsigned n=r16(file+pe+6),osz=r16(file+pe+20); size_t sec=(size_t)pe+24+osz; image_size=r32(file+pe+24+56);
    if(image_size<0x83000||image_size>0x1000000||sec+(size_t)n*40>(size_t)len){ fprintf(stderr,"invalid image\n"); return 2; }
    image=mmap(0,image_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(image==MAP_FAILED){ perror("mmap"); return 2; }
    for(unsigned i=0;i<n;i++){
        const uint8_t *s=file+sec+i*40; uint32_t vs=r32(s+8),va=r32(s+12),rs=r32(s+16),ro=r32(s+20);
        if(va>image_size||rs>image_size-va||ro>(size_t)len||rs>(size_t)len-ro){ fprintf(stderr,"invalid section\n"); return 2; }
        memcpy(image+va,file+ro,rs<vs||!vs?rs:vs);
    }
    stubs=mmap(0,8192,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(stubs==MAP_FAILED){ perror("mmap"); return 2; }
    for(int i=0;i<NSLOT;i++){ uint64_t t=make_stub((uint32_t)i); memcpy(image+slots[i].slot,&t,8); }
    /* sanity: the code at the two handler entries must be the push/mov prologue read from the disassembly */
    static const uint8_t pro_r[4]={0x48,0x89,0x5c,0x24}, pro_f[3]={0x56,0x41,0x56};
    if(memcmp(image+0x2df50,pro_r,4)||memcmp(image+0x2ec30+1,pro_f,3)){ fprintf(stderr,"oracle: handler prologue mismatch (wrong DLL?)\n"); return 2; }
    /* logging helpers (std::cout chains): return the stream argument; cookie check: return */
    static const uint32_t log_fn[]={0x15e10,0x160f0,0x168a0,0x16ad0,0x234d0,0x2f270};
    for(unsigned i=0;i<sizeof log_fn/sizeof *log_fn;i++){ image[log_fn[i]]=0x48; image[log_fn[i]+1]=0x89; image[log_fn[i]+2]=0xC8; image[log_fn[i]+3]=0xC3; }
    image[0x3a8b0]=0xC3;
    patch_jmp(0x2d130,ID_SNPRINTF);
    uint8_t *arena=mmap(0,MPC_ARENA,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(arena==MAP_FAILED){ perror("mmap"); return 2; }
    FILE *dump=argc>2?fopen(argv[2],"w"):NULL;
    struct mpc_case *c=malloc(sizeof *c); static char line[8192];
    int cases[2]={3600,3720}; int rc=0;
    for(int range=1;range>=0;range--){
        uint64_t h=0xcbf29ce484222325ull; int ran=0;
        for(int i=0;i<cases[range?0:1];i++){
            mpc_make(c,range,(uint32_t)i,(uint64_t)(uintptr_t)arena);
            if(range&&(c->kind==64||c->kind==65)) continue;
            memcpy(arena,c->buf,MPC_ARENA); ncalls=0; cur=c;
            call_win64(image+(range?0x2df50:0x2ec30),arena+MPC_R,arena+MPC_C,c->xmm2);
            if(ncalls>63){ fprintf(stderr,"oracle: too many calls in case %d\n",i); errors++; }
            size_t l=mpc_line(line,sizeof line,c,calls,ncalls<63?ncalls:63,arena);
            h=mpc_fnv(h,line,l); h=mpc_fnv(h,"\n",1); if(dump) fprintf(dump,"%s\n",line); ran++;
        }
        printf("ORACLE %s cases=%d run=%d hash=%016llx\n",range?"range":"frame",cases[range?0:1],ran,(unsigned long long)h);
    }
    if(errors){ fprintf(stderr,"oracle: %d errors\n",errors); rc=1; }
    return rc;
}
