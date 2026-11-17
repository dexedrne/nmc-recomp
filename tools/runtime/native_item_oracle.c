/* Task 16 oracle, the same kind of isolated-block execution as tools/native_difficulty_oracle.c (Task 7).
 * Executes ONLY two straight-line integer blocks of the shipped KH2Tweaks_Refactor.dll item handler 0x31260, never its
 * initializer, DllMain, console code, or any call:
 *   block A  RVA 31441..3147b  byte pct, setne flag, movsx command id, the two 32-bit products, +9999
 *   block B  RVA 31482..314df  cmp byte [cmd+0x2f],2 and the two magic-number divisions (97247453 = /-10000, 68db8bad = /10000)
 * Each block's exit is replaced by a one-byte RET (at 3147c, the indirect call of the command lookup, and at 314e0, the start
 * of the id dispatch).  No call slot is redirected and nothing outside .text is mapped.  The command lookup between the two
 * blocks is replaced by loading rax with a caller-owned 0x30-byte record.  The results are compared with the reference
 * expressions in runtime/mod_item_cases.h, which are also the expected values of runtime/mod_difficulty_items_test.c.
 *
 * Build: gcc -O2 -Wall -Wextra -I runtime tools/native_item_oracle.c -o <dir>/native_item_oracle
 * Run:   <dir>/native_item_oracle /path/to/KH2Tweaks_Refactor.dll */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include "mod_item_cases.h"
#if !defined(__x86_64__)
#error This oracle executes original x86-64 instructions and requires x86-64.
#endif
static uint16_t r16(const void *p){uint16_t x;memcpy(&x,p,2);return x;}
static uint32_t r32(const void *p){uint32_t x;memcpy(&x,p,4);return x;}

struct io {
    uint64_t rec, sheet, user, mult, cmd;          /* in: rsi, rbp, r14, r15, rax (block B) */
    uint32_t mid_hp, mid_mp, flag, cmdid;          /* after block A: r14d, ebx, r12b, ecx */
    uint64_t slot, r12_after;                      /* the block's [rsp+0x90] store and r12 after it */
    uint32_t out_hp, out_mp;                       /* after block B: r14d, ebx */
};
_Static_assert(offsetof(struct io,cmd)==0x20 && offsetof(struct io,mid_hp)==0x28 && offsetof(struct io,flag)==0x30 &&
               offsetof(struct io,slot)==0x38 && offsetof(struct io,r12_after)==0x40 && offsetof(struct io,out_hp)==0x48,"io layout");
#define R12_SENTINEL 0x1212121212121212ull
extern void run_item_blocks(void *block_a,void *block_b,struct io *io);
extern void run_item_div(void *block_b,void *cmd,uint32_t hp_numer,uint32_t mp_numer,uint32_t *out);
__asm__(".text\n.globl run_item_blocks\nrun_item_blocks:\n"
        "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $0xb8,%rsp\n"
        "mov %rdi,%r13\nmov %rsi,0xa0(%rsp)\nmov %rdx,0xa8(%rsp)\n"
        "mov 0x00(%rdx),%rsi\nmov 0x08(%rdx),%rbp\nmov 0x10(%rdx),%r14\nmov 0x18(%rdx),%r15\n"
        "movabs $0x1212121212121212,%r12\n"
        "call *%r13\n"                                     /* block A; its [rsp+0x90] is 0x88(%rsp) here */
        "mov 0xa8(%rsp),%rdx\n"
        "mov %r14d,0x28(%rdx)\nmov %ebx,0x2c(%rdx)\nmovzbl %r12b,%eax\nmov %eax,0x30(%rdx)\nmov %ecx,0x34(%rdx)\n"
        "mov 0x88(%rsp),%rax\nmov %rax,0x38(%rdx)\nmov %r12,0x40(%rdx)\n"
        "mov 0x20(%rdx),%rax\n"
        "call *0xa0(%rsp)\n"                               /* block B, fed block A's r14d/ebx untouched */
        "mov 0xa8(%rsp),%rdx\nmov %r14d,0x48(%rdx)\nmov %ebx,0x4c(%rdx)\n"
        "add $0xb8,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
        ".globl run_item_div\nrun_item_div:\n"
        "push %rbx\npush %r14\npush %r12\n"
        "mov %r8,%r12\nmov %edx,%r14d\nmov %ecx,%ebx\nmov %rsi,%rax\n"
        "call *%rdi\n"
        "mov %r14d,0(%r12)\nmov %ebx,4(%r12)\n"
        "pop %r12\npop %r14\npop %rbx\nret\n");

static unsigned long long checks,failures;
static void fail(const char *what,unsigned long long a,unsigned long long b,const struct io *io,unsigned pct,unsigned byte){
    if(++failures<=20) fprintf(stderr,"FAIL %s: got %llx want %llx (pct=%u mult=%llx user=%llx byte=%u)\n",what,a,b,pct,
                               (unsigned long long)(io?io->mult:0),(unsigned long long)(io?io->user:0),byte);
}
#define EQ(what,a,b) do{ checks++; if((unsigned long long)(a)!=(unsigned long long)(b)) fail(what,(unsigned long long)(a),(unsigned long long)(b),&io,pct,byte); }while(0)
static uint64_t rng_state=0x9e3779b97f4a7c15ull;
static uint64_t rng(void){ rng_state^=rng_state<<13; rng_state^=rng_state>>7; rng_state^=rng_state<<17; return rng_state; }

int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: %s DLL\n",argv[0]);return 2;}
    FILE *f=fopen(argv[1],"rb");if(!f){perror(argv[1]);return 2;}
    if(fseek(f,0,SEEK_END)){perror("seek");return 2;}
    long length=ftell(f);rewind(f);
    if(length<64){fprintf(stderr,"short DOS header\n");return 2;}
    unsigned char *file=malloc((size_t)length);
    if(!file || fread(file,1,(size_t)length,f)!=(size_t)length){fprintf(stderr,"read failed\n");return 2;}
    fclose(f);
    uint32_t pe=r32(file+0x3c);
    if(pe>(size_t)length-24 || memcmp(file+pe,"PE\0\0",4)){fprintf(stderr,"invalid PE\n");return 2;}
    unsigned n=r16(file+pe+6),optsize=r16(file+pe+20);
    size_t sections=(size_t)pe+24+optsize;
    if(optsize<60 || sections>(size_t)length || n>((size_t)length-sections)/40){fprintf(stderr,"invalid sections\n");return 2;}
    size_t size=r32(file+pe+24+56);
    if(size<0x314e4 || size>0x10000000){fprintf(stderr,"invalid image size\n");return 2;}
    unsigned char *image=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(image==MAP_FAILED){perror("mmap");return 2;}
    int text_found=0;
    for(unsigned i=0;i<n;i++){
        unsigned char *s=file+sections+i*40;
        uint32_t va=r32(s+12),bytes=r32(s+16),off=r32(s+20);
        if(va>size || bytes>size-va || off>(size_t)length || bytes>(size_t)length-off){fprintf(stderr,"invalid section bounds\n");return 2;}
        if(!memcmp(s,".text\0\0\0",8)){
            memcpy(image+va,file+off,bytes);
            text_found=va<=0x31441 && (size_t)va+bytes>0x314e3;
            printf("PE .text RVA=%x raw=%x bytes=%x; native blocks RVA 31441..3147b and 31482..314df\n",va,off,bytes);
        }
    }
    if(!text_found){fprintf(stderr,"missing item code\n");return 2;}
    /* Refuse any other build of the DLL: FNV-1a 64 of the 162 bytes 31441..314e2, and the two instructions that become RET. */
    uint64_t hash=0xcbf29ce484222325ull;
    for(unsigned a=0x31441;a<0x314e3;a++) hash=(hash^image[a])*0x100000001b3ull;
    if(hash!=0xc96e3a1131dfa539ull || image[0x3147c]!=0xff || image[0x3147d]!=0x15 || image[0x314e0]!=0x0f || image[0x314e1]!=0xb7){
        fprintf(stderr,"unexpected block bytes (hash %llx): not the analysed DLL\n",(unsigned long long)hash);return 2;
    }
    image[0x3147c]=0xc3; image[0x314e0]=0xc3;
    if(mprotect(image,size,PROT_READ|PROT_EXEC)){perror("mprotect");return 2;}
    void *block_a=image+0x31441,*block_b=image+0x31482;

    unsigned char rec[0x18],sheet[0x278],cmd[0x30],rec0[0x18],sheet0[0x278],cmd0[0x30];
    static const uint64_t users[]={0,2,1,0xffffffffull,0x100000000ull,0xffffffff00000000ull,0x80000000ull};
    static const uint16_t cmdwords[]={23,0,0x7fff,0x8000,0xffff};
    struct io io; unsigned pct=0,byte=0; unsigned long long run=0;
#define ONE(PCT,MULT,MAXHP,MAXMP,BYTE,USER,CMDWORD) do{ \
        pct=(PCT); byte=(BYTE); uint32_t mult_=(MULT),maxhp_=(MAXHP),maxmp_=(MAXMP); uint16_t cw_=(CMDWORD); \
        memset(rec,0xa5,sizeof rec); memset(sheet,0x5a,sizeof sheet); memset(cmd,0x3c,sizeof cmd); \
        rec[4]=(unsigned char)pct; memcpy(rec+0x10,&cw_,2); memcpy(sheet+4,&maxhp_,4); memcpy(sheet+0x184,&maxmp_,4); cmd[0x2f]=(unsigned char)byte; \
        memcpy(rec0,rec,sizeof rec); memcpy(sheet0,sheet,sizeof sheet); memcpy(cmd0,cmd,sizeof cmd); \
        memset(&io,0,sizeof io); io.rec=(uint64_t)rec; io.sheet=(uint64_t)sheet; io.user=(USER); io.mult=0xabcdef0000000000ull|mult_; io.cmd=(uint64_t)cmd; \
        run_item_blocks(block_a,block_b,&io); run++; \
        int32_t hp_n=kh_item_ref_numer(pct,mult_,maxhp_),mp_n=kh_item_ref_numer(pct,mult_,maxmp_); \
        EQ("hp numerator",io.mid_hp,(uint32_t)hp_n); EQ("mp numerator",io.mid_mp,(uint32_t)mp_n); \
        EQ("flag",io.flag,(uint32_t)io.user!=0); EQ("command id",io.cmdid,(uint32_t)(int32_t)(int16_t)cw_); \
        EQ("saved r12",io.slot,R12_SENTINEL); EQ("r12",io.r12_after,(R12_SENTINEL&~0xffull)|((uint32_t)io.user!=0)); \
        EQ("hp amount",io.out_hp,(uint32_t)kh_item_ref_amount(hp_n,(int)byte)); EQ("mp amount",io.out_mp,(uint32_t)kh_item_ref_amount(mp_n,(int)byte)); \
        EQ("read-only",memcmp(rec0,rec,sizeof rec)||memcmp(sheet0,sheet,sizeof sheet)||memcmp(cmd0,cmd,sizeof cmd),0); \
    }while(0)
    /* (1) the selftest's tables, full cross product of percent x multiplier x max HP x max MP x command byte */
    for(int p=0;p<KH_ITEM_N(kh_item_pcts);p++) for(int m=0;m<KH_ITEM_N(kh_item_mults);m++) for(int x=0;x<KH_ITEM_N(kh_item_maxes);x++)
    for(int y=0;y<KH_ITEM_N(kh_item_maxes);y++) for(int b=0;b<KH_ITEM_N(kh_item_bytes);b++)
        ONE(kh_item_pcts[p],kh_item_mults[m],kh_item_maxes[x],kh_item_maxes[y],kh_item_bytes[b],users[run%7],cmdwords[run%5]);
    /* (2) every percent byte x every command byte value, realistic party values (byte-sized maxima, multiplier 100..250) */
    for(unsigned p=0;p<256;p++) for(unsigned b=0;b<256;b++)
        ONE(p,100+(p*7+b)%151,1+(p*31+b*17)%255,1+(p*13+b*29)%255,b,users[run%7],cmdwords[run%5]);
    /* (3) random 32-bit operands */
    for(unsigned i=0;i<2000000;i++){
        uint64_t a=rng(),c=rng(),d=rng();
        ONE((unsigned)(a&255),(uint32_t)(a>>8),(uint32_t)c,(uint32_t)(c>>32),(d&3)==0?2u:(unsigned)((d>>8)&255),(d>>16)&1?d>>20:0,(uint16_t)(d>>40));
    }
    unsigned long long chain_runs=run;
    /* (4) block B alone: numerators around the first multiples of 10000, the int32 extremes, and a stride over all 2^32 */
    {
        uint32_t out[2]; memset(cmd,0,sizeof cmd); unsigned long long before=checks;
        for(int reversed=0;reversed<2;reversed++){
            cmd[0x2f]=reversed?2:0;
            for(int64_t k=-200000;k<=200000;k++){
                int32_t a=(int32_t)k,b2=(int32_t)(k<0?INT32_MIN-k:INT32_MAX-k);
                run_item_div(block_b,cmd,(uint32_t)a,(uint32_t)b2,out);
                checks+=2;
                if(out[0]!=(uint32_t)kh_item_ref_amount(a,cmd[0x2f])){ if(++failures<=20) fprintf(stderr,"FAIL div n=%d byte=%u got %d\n",a,cmd[0x2f],(int32_t)out[0]); }
                if(out[1]!=(uint32_t)kh_item_ref_amount(b2,cmd[0x2f])){ if(++failures<=20) fprintf(stderr,"FAIL div n=%d byte=%u got %d\n",b2,cmd[0x2f],(int32_t)out[1]); }
            }
            for(uint64_t v=0;v<0x100000000ull;v+=251){
                int32_t a=(int32_t)(uint32_t)v,b2=(int32_t)(uint32_t)(v*0x9e3779b1u+12345u);
                run_item_div(block_b,cmd,(uint32_t)a,(uint32_t)b2,out);
                checks+=2;
                if(out[0]!=(uint32_t)kh_item_ref_amount(a,cmd[0x2f])){ if(++failures<=20) fprintf(stderr,"FAIL div n=%d byte=%u got %d\n",a,cmd[0x2f],(int32_t)out[0]); }
                if(out[1]!=(uint32_t)kh_item_ref_amount(b2,cmd[0x2f])){ if(++failures<=20) fprintf(stderr,"FAIL div n=%d byte=%u got %d\n",b2,cmd[0x2f],(int32_t)out[1]); }
            }
        }
        printf("block B alone: %llu numerator checks\n",checks-before);
    }
    printf("native DLL item blocks: %llu chained runs, %llu checks, %llu failures; exits 3147c/314e0 are RET stubs, no call executed\n",
           chain_runs,checks,failures);
    munmap(image,size);free(file);return failures?1:0;
}
