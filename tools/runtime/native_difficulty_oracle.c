/* Execute only the shipped DLL's two cap blocks, never its initializer.
 * Build: gcc -O2 tools/native_difficulty_oracle.c -o /tmp/native_difficulty_oracle
 * Run: /tmp/native_difficulty_oracle /path/to/KH2Tweaks_Refactor.dll
 * The common exit RVA 31ba4 is stubbed with RET; guest cleanup is not tested here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#if !defined(__x86_64__)
#error This oracle executes original x86-64 instructions and requires x86-64.
#endif
static uint16_t r16(const void *p){uint16_t x;memcpy(&x,p,2);return x;}
static uint32_t r32(const void *p){uint32_t x;memcpy(&x,p,4);return x;}
extern void run_cap(void *,void *,void *,void *);
__asm__(".text\n.global run_cap\nrun_cap:\n"
        "push %rbp\npush %rsi\nsub $8,%rsp\n"
        "mov %rsi,%rbp\nmov %rcx,%rsi\ncall *%rdi\n"
        "add $8,%rsp\npop %rsi\npop %rbp\nret\n");
int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: %s DLL\n",argv[0]);return 2;}
    FILE *f=fopen(argv[1],"rb");if(!f){perror(argv[1]);return 2;}
    if(fseek(f,0,SEEK_END)){perror("seek");return 2;}
    long length=ftell(f);rewind(f);
    if(length<64){fprintf(stderr,"short DOS header\n");return 2;}
    unsigned char *file=malloc(length);
    if(!file || fread(file,1,length,f)!=(size_t)length){fprintf(stderr,"read failed\n");return 2;}
    fclose(f);
    uint32_t pe=r32(file+0x3c);
    if(pe>(size_t)length-24 || memcmp(file+pe,"PE\0\0",4)){fprintf(stderr,"invalid PE\n");return 2;}
    unsigned n=r16(file+pe+6),optsize=r16(file+pe+20);
    size_t sections=(size_t)pe+24+optsize;
    if(optsize<60 || sections>(size_t)length || n>((size_t)length-sections)/40){fprintf(stderr,"invalid sections\n");return 2;}
    size_t size=r32(file+pe+24+56);
    if(size<0x31ba5 || size>0x10000000){fprintf(stderr,"invalid image size\n");return 2;}
    unsigned char *image=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(image==MAP_FAILED){perror("mmap");return 2;}
    int text_found=0;
    for(unsigned i=0;i<n;i++){
        unsigned char *s=file+sections+i*40;
        uint32_t va=r32(s+12),bytes=r32(s+16),off=r32(s+20);
        if(va>size || bytes>size-va || off>(size_t)length || bytes>(size_t)length-off){fprintf(stderr,"invalid section bounds\n");return 2;}
        if(!memcmp(s,".text\0\0\0",8)){
            memcpy(image+va,file+off,bytes);
            text_found=va<=0x3178e && (size_t)va+bytes>0x31ba4;
            printf("PE .text RVA=%x raw=%x bytes=%x; native blocks RVA3178e/317bd\n",va,off,bytes);
        }
    }
    if(!text_found){fprintf(stderr,"missing cap code\n");return 2;}
    image[0x31ba4]=0xc3;
    if(mprotect(image,size,PROT_READ|PROT_EXEC)){perror("mprotect");return 2;}
    unsigned char sheet[0x278],save[0x249c]={0},effect[0x18]={0};
    unsigned checks=0,failures=0;
    for(int d=0;d<4;d++) for(int mp=0;mp<2;mp++){
        int cap=d==3?(mp?220:120):240;
        int values[]={0,1,cap-1,cap,cap+1,240,INT_MAX,INT_MIN};
        unsigned grants[]={0,1,2,255};
        memcpy(save+0x2498,&d,4);
        for(unsigned v=0;v<sizeof values/sizeof *values;v++) for(unsigned a=0;a<sizeof grants/sizeof *grants;a++){
            memset(sheet,0xa5,sizeof sheet);
            unsigned offset=mp?0x184:4;uint32_t before=(uint32_t)values[v],actual;
            memcpy(sheet+offset,&before,4);effect[4]=grants[a];
            int allowed=values[v]<(mp?220:120) || (d!=3 && values[v]<240);
            uint32_t expected=before+(allowed?grants[a]:0);
            run_cap(image+(mp?0x317bd:0x3178e),sheet,save,effect);
            memcpy(&actual,sheet+offset,4);checks++;
            if(actual!=expected){failures++;fprintf(stderr,"FAIL d=%d mp=%d before=%d grant=%u expected=%u got=%u\n",d,mp,values[v],grants[a],expected,actual);}
            for(unsigned b=0;b<sizeof sheet;b++) if((b<offset || b>=offset+4) && sheet[b]!=0xa5){fprintf(stderr,"unexpected sheet write at %x\n",b);return 1;}
        }
    }
    printf("native DLL cap blocks: %u checks, %u failures; common exit RET stub, cleanup excluded\n",checks,failures);
    munmap(image,size);free(file);return failures?1:0;
}
