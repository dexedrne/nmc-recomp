#include "khnmc_rt.h"
#include <unistd.h>
#include <sys/wait.h>
static int checks, failures;
#define T(x) do { checks++; if(!(x)){ failures++; fprintf(stderr,"difficulty rewards FAIL line %d: %s\n",__LINE__,#x); } } while(0)
/* One hook call in a child with stderr captured: 1 when it ended in kh_fatal (exit status 3) and printed every needle. */
static int fatal_child(KCtx *g,u32 rva,u64 a0,u64 a1,u64 a2,const char *n1,const char *n2,const char *n3){
    FILE *log=tmpfile(); if(!log) kh_fatal(g,"rewards selftest cannot open a fatal log");
    pid_t child=fork(); if(child<0) kh_fatal(g,"rewards selftest fork failed");
    if(!child){ if(dup2(fileno(log),2)<0) _exit(98); kh_call(g,GS_BASE+rva,a0,a1,a2,0); _exit(99); }
    int status=0; if(waitpid(child,&status,0)!=child) kh_fatal(g,"rewards selftest waitpid failed");
    rewind(log); char out[4096]; size_t len=fread(out,1,sizeof out-1,log); out[len]=0; fclose(log);
    int ok=WIFEXITED(status)&&WEXITSTATUS(status)==3&&strstr(out,n1)&&strstr(out,n2)&&strstr(out,n3);
    if(!ok) fprintf(stderr,"  rewards fatal child status=0x%x output: %.300s\n",status,out);
    return ok;
}
/* Called inside the caps fixture, which provides the real character-2 sheet registry. */
int kh_mod_rewards_selftest(KCtx *g,u64 sheet){
    u32 old=rd32(g,GS_BASE+0x9abd48);
    u64 msg=ga_calloc(1,24),oldmsg=rd64(g,GS_BASE+0x2a11678);
    wr32(g,msg+4,1); wr32(g,msg+8,0xadc); wr32(g,msg+12,16); wr64(g,GS_BASE+0x2a11678,msg);
    u64 hud=ga_calloc(1,0x100),oldhud=rd64(g,GS_BASE+0xabe930); wr32(g,hud+0x30,15); wr64(g,GS_BASE+0xabe930,hud);
    u64 active=GS_BASE+0x9acde4+4*rd8(g,GS_BASE+0x717008); u32 oldactive=rd32(g,active); wr32(g,active,0x12121212);
    u64 rec=ga_calloc(1,48)+16;
    const int values[]={0,1,3,127,128,255};
    for(int d=0;d<4;d++) for(int i=0;i<6;i++){
        int v=values[i],sv=(s8)v,grant=d>=2?sv/2:sv;
        wr32(g,GS_BASE+0x9abd48,d);
        memset(hp(g,rec),0,16); wr8(g,rec+1,2); wr8(g,rec+2,v); wr8(g,rec+3,v);
        wr32(g,sheet+4,1000); wr32(g,sheet+0x184,1000);
        kh_call(g,GS_BASE+0x3edf90,rec,sheet,0,0);
        T(rd32(g,sheet+4)==(u32)(1000+grant)); T(rd32(g,sheet+0x184)==(u32)(1000+grant));
    }
    /* Dormant shipped lvup flags: both HP and MP use HP multiplier, then x0.5. */
    const int packed_values[]={0,1,0x13,0xff};
    for(int d=0;d<4;d++) for(int i=0;i<4;i++){
        int packed=packed_values[i]; wr32(g,GS_BASE+0x9abd48,d);
        memset(hp(g,rec-16),0,48); wr8(g,rec+14,3); wr8(g,rec+15,packed);
        wr32(g,sheet+4,1000); wr32(g,sheet+0x184,1000);
        kh_call(g,GS_BASE+0x3ed7d0,rec,2,1,0);
        T(rd32(g,sheet+4)==1000+(packed>>4)/(d>=2?4:2));
        T(rd32(g,sheet+0x184)==1000+(packed&15)/(d>=2?4:2));
    }
    /* Inspect the actual popup message list left on the guest stack by the hook. */
    for(int d=0;d<4;d++) for(int i=0;i<6;i++) for(int j=0;j<6;j++){
        wr32(g,GS_BASE+0x9abd48,d); memset(hp(g,rec-16),0,48);
        wr8(g,rec-9,values[i]); wr8(g,rec+7,values[j]);
        u64 entry=((KH_RSP(g)-0x100)&~15ull)-8;
        u64 list=((entry-0x180)&~15ull)+0x40;
        kh_call(g,GS_BASE+0x3ed7d0,rec,2,1,0);
        int before=d>=2?(s8)values[i]*3/2:(s8)values[i];
        int after=d>=2?(s8)values[j]*3/2:(s8)values[j];
        T(rd64(g,list)==(after>before?msg+16:0));
    }
    /* Bonus scaling is restricted to 1,2,3,14, with mask bits 1<<difficulty. */
    const int characters[]={1,2,3,4,12,14};
    for(int d=0;d<4;d++) for(int c=0;c<6;c++) for(int mask=0;mask<2;mask++){
        wr32(g,GS_BASE+0x9abd48,d); memset(hp(g,rec),0,16);
        wr8(g,rec+1,characters[c]); wr8(g,rec+2,3); wr8(g,rec+3,7);
        wr8(g,rec+12,mask? (1u<<((d+1)%4)):0);
        wr32(g,sheet+4,1000); wr32(g,sheet+0x184,1000);
        kh_call(g,GS_BASE+0x3edf90,rec,sheet,0,0);
        int scaled=d>=2&&(characters[c]<=3||characters[c]==14);
        T(rd32(g,sheet+4)==1000+(mask?0:scaled?1:3));
        T(rd32(g,sheet+0x184)==1000+(mask?0:scaled?3:7));
    }
    /* A dword outside 0..3: neither DLL handler has a path to the original (no reference to its trampoline), and both then take
     * their multipliers from a stack slot nothing wrote (31f66 / 31f6c, 2f9fe / 2fa04).  A named fatal that says so; the vanilla
     * body is not an answer. */
    static const s32 invalid[]={-1,4,INT32_MAX,INT32_MIN};
    static const char *const shown[]={"= 0xffffffff ","= 0x4 ","= 0x7fffffff ","= 0x80000000 "};
    for(int d=0;d<4;d++){
        wr32(g,GS_BASE+0x9abd48,(u32)invalid[d]); memset(hp(g,rec-16),0,48); wr8(g,rec+1,2); wr8(g,rec+2,255);
        wr32(g,sheet+4,1000); wr32(g,sheet+0x184,1000);
        T(fatal_child(g,0x3edf90,rec,sheet,0,"mod-difficulty-out-of-range:",shown[d],"exe 0x3edf90 = DLL 0x2f9b0"));
        wr8(g,rec+14,3); wr8(g,rec+15,255);
        T(fatal_child(g,0x3ed7d0,rec,2,1,"mod-difficulty-out-of-range:",shown[d],"exe 0x3ed7d0 = DLL 0x31ee0"));
        T(rd32(g,sheet+4)==1000 && rd32(g,sheet+0x184)==1000);
    }
    /* Stat-sheet AP stays the vanilla unsigned-byte, character-1/14-only rule. */
    for(int d=0;d<4;d++) for(int c=1;c<=3;c++) for(int i=0;i<6;i++){
        wr32(g,GS_BASE+0x9abd48,d); wr8(g,rec+7,values[i]);
        T(kh_call(g,GS_BASE+0x3eda00,rec,c,0,0)==(d==3 && c==1?values[i]*3/2:values[i]));
    }
    wr32(g,active,oldactive); wr64(g,GS_BASE+0xabe930,oldhud); ga_free(hud); wr64(g,GS_BASE+0x2a11678,oldmsg); ga_free(msg); ga_free(rec-16); wr32(g,GS_BASE+0x9abd48,old);
    fprintf(stderr,"kh: difficulty rewards selftest: %d checks, %d failures\n",checks,failures);
    return failures;
}
