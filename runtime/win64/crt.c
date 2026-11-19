/* crt.c : UCRT / vcruntime140 / msvcp140 imports as host implementations (heap, string, stdio, printf engine, convert, time,
 * utility, startup, C++ throw thunks = terminate-on-throw).  Order of treatment follows IMPORT-CENSUS call-site counts. */
#include "khnmc_rt.h"
#include "savestate.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#undef isdigit
#undef isprint
#undef tolower
#undef toupper

int kh_cnt_init_xi, kh_cnt_init_xc;
static _Atomic int nopenfail;
static u64 kh_cmdline_ga;

/* ======================================================================================================================
 * heap (malloc family -> guest arena)
 * ==================================================================================================================== */
/* Reserve the first f_501480 object before the timing thread can claim its
 * size class. f_439064 calls malloc via a tail jump, with a 0x28-byte frame. */
static _Atomic u64 cut_boot_object, cut_boot_table;
/* f_1287d0 evicts the oldest 72-byte node when the history reaches
 * 0x1e entries (0x128fb6..0x129020). Reserve the whole initial fill. */
enum { CUT_TIMING_HISTORY=30 };
static u64 cut_boot_requests[CUT_TIMING_HISTORY];
static unsigned cut_requests_used;
void kh_cut_reserve_boot_object(void){
    u64 p=ga_alloc_src(80,16,GA_SRC_CRT);
    if(!p) kh_fatal(NULL,"savestate-boot-reservation: 80-byte object");
    atomic_store(&cut_boot_object,p);
    p=ga_alloc_src(280,16,GA_SRC_CRT);
    if(!p) kh_fatal(NULL,"savestate-boot-reservation: 280-byte object");
    atomic_store(&cut_boot_table,p);
    for(unsigned i=0;i<CUT_TIMING_HISTORY;i++){
        cut_boot_requests[i]=ga_alloc_src(72,16,GA_SRC_CRT);
        if(!cut_boot_requests[i]) kh_fatal(NULL,"savestate-boot-reservation: timing request");
    }
}
IMP(CRTH,malloc){
    u64 p=0;
    if(kh_cut_enabled&&(A0(g)==72||A0(g)==80||A0(g)==280)&&
       rd64(g,KH_RSP(g))==GS_BASE+0x439083){
        u64 caller=rd64(g,KH_RSP(g)+0x30)-GS_BASE;
        if(A0(g)==72&&caller==0x10ef68&&cur_thread()->start==GS_BASE+0x129110&&cut_requests_used<CUT_TIMING_HISTORY)
            p=cut_boot_requests[cut_requests_used++];
        if(A0(g)==80&&caller==0x5014bf) p=atomic_exchange(&cut_boot_object,0);
        if(A0(g)==280&&caller==0x4f102e) p=atomic_exchange(&cut_boot_table,0);
        if(p) fprintf(stderr,"kh: savestate-boot-object caller=%llx guest=%llx\n",
                      (unsigned long long)caller,(unsigned long long)p);
    }
    SET_RAX(g,p?p:ga_alloc_src(A0(g),16,GA_SRC_CRT));
}
IMP(CRTH,calloc){ SET_RAX(g,ga_calloc_src(A0(g),A1(g),GA_SRC_CRT)); }
IMP(CRTH,realloc){ SET_RAX(g,ga_realloc(A0(g),A1(g))); }
IMP(CRTH,free){ ga_free(A0(g)); }
IMP(CRTH,_recalloc){ u64 p=A0(g),n=A1(g)*A2(g); u64 old=p?ga_usable(p):0; u64 q=ga_realloc(p,n); if(q&&n>old) memset(hp(g,q+old),0,n-old); SET_RAX(g,q); }
IMP(CRTH,_aligned_malloc){ u64 al=A1(g); if(al<16) al=16; SET_RAX(g,ga_alloc_src(A0(g),al,GA_SRC_CRT)); }
IMP(CRTH,_aligned_realloc){ u64 p=A0(g),n=A1(g),al=A2(g); if(al<16) al=16; if(!p){ SET_RAX(g,ga_alloc_src(n,al,GA_SRC_CRT)); return; }
    u64 old=ga_usable(p); u64 q=ga_alloc_src(n,al,GA_SRC_CRT); if(q){ memcpy(hp(g,q),hp(g,p),old<n?old:n); ga_free(p);} SET_RAX(g,q); }
IMP(CRTH,_aligned_free){ ga_free(A0(g)); }
IMP(CRTH,_callnewh){ SET_RAX(g,0); }
IMP(CRTH,_set_new_mode){ SET_RAX(g,0); }

/* ======================================================================================================================
 * string / ctype / memory (vcruntime140 mem* and str*)
 * ==================================================================================================================== */
IMP(VCR,memcpy){ memcpy(hp(g,A0(g)),hp(g,A1(g)),A2(g)); SET_RAX(g,A0(g)); }
IMP(VCR,memmove){ memmove(hp(g,A0(g)),hp(g,A1(g)),A2(g)); SET_RAX(g,A0(g)); }
IMP(VCR,memset){ memset(hp(g,A0(g)),(int)(A1(g)&0xff),A2(g)); SET_RAX(g,A0(g)); }
IMP(VCR,memcmp){ int r=memcmp(hp(g,A0(g)),hp(g,A1(g)),A2(g)); SET_RAX(g,(u32)(r<0?-1:r>0?1:0)); }   /* sign only: host libcs return different magnitudes */
IMP(VCR,memchr){ void *r=memchr(hp(g,A0(g)),(int)(A1(g)&0xff),A2(g)); SET_RAX(g,r?(u64)((uintptr_t)r-gs_bias):0); }
#define HP2G(g,p) ((p)?(u64)((uintptr_t)(p)-gs_bias):0ull)
IMP(VCR,strchr){ char *r=strchr(gstr(g,A0(g)),(int)(s8)A1(g)); SET_RAX(g,HP2G(g,r)); }
IMP(VCR,strrchr){ char *r=strrchr(gstr(g,A0(g)),(int)(s8)A1(g)); SET_RAX(g,HP2G(g,r)); }
IMP(VCR,strstr){ char *r=strstr(gstr(g,A0(g)),gstr(g,A1(g))); SET_RAX(g,HP2G(g,r)); }
IMP(VCR,wcschr){ u64 p=A0(g); u16 c=(u16)A1(g); for(;;p+=2){ u16 v=rd16(g,p); if(v==c){ SET_RAX(g,p); return; } if(!v){ SET_RAX(g,0); return; } } }
IMP(VCR,wcsstr){ u64 h=A0(g),n=A1(g); size_t nl=gw_len(g,n); if(!nl){ SET_RAX(g,h); return; } for(u64 p=h;rd16(g,p);p+=2){ size_t i=0; while(i<nl&&rd16(g,p+2*i)==rd16(g,n+2*i)) i++; if(i==nl){ SET_RAX(g,p); return; } if(!rd16(g,p+2*i)) break; } SET_RAX(g,0); }
IMP(CRTT,strcmp){ int r=strcmp(gstr(g,A0(g)),gstr(g,A1(g))); SET_RAX(g,(u32)(r<0?-1:r>0?1:0)); }
IMP(CRTT,strncmp){ int r=strncmp(gstr(g,A0(g)),gstr(g,A1(g)),A2(g)); SET_RAX(g,(u32)(r<0?-1:r>0?1:0)); }
IMP(CRTT,_strnicmp){ int r=strncasecmp(gstr(g,A0(g)),gstr(g,A1(g)),A2(g)); SET_RAX(g,(u32)(r<0?-1:r>0?1:0)); }
IMP(CRTT,strncpy){ strncpy((char*)hp(g,A0(g)),gstr(g,A1(g)),A2(g)); SET_RAX(g,A0(g)); }
IMP(CRTT,strncat){ strncat((char*)hp(g,A0(g)),gstr(g,A1(g)),A2(g)); SET_RAX(g,A0(g)); }
IMP(CRTT,strcpy_s){ u64 d=A0(g); u64 n=A1(g); const char *s=gstr(g,A2(g)); if(!d||!n){ SET_RAX(g,22); return; } size_t l=strlen(s); if(l>=n){ wr8(g,d,0); SET_RAX(g,34); return; } memcpy(hp(g,d),s,l+1); SET_RAX(g,0); }
IMP(CRTT,strcat_s){ u64 d=A0(g); u64 n=A1(g); const char *s=gstr(g,A2(g)); size_t dl=strnlen(gstr(g,d),n); size_t l=strlen(s); if(dl>=n||dl+l>=n){ if(n) wr8(g,d,0); SET_RAX(g,34); return; } memcpy(hp(g,d+dl),s,l+1); SET_RAX(g,0); }
IMP(CRTT,strncpy_s){ u64 d=A0(g),n=A1(g),cnt=A3(g); const char *s=gstr(g,A2(g)); if(!d||!n){ SET_RAX(g,22); return; }
    size_t l=strnlen(s,cnt==(u64)-1?strlen(s):cnt); if(l>=n){ if(cnt==(u64)-1){ wr8(g,d,0); SET_RAX(g,34); return; } l=n-1; } memcpy(hp(g,d),s,l); wr8(g,d+l,0); SET_RAX(g,0); }
IMP(CRTT,isdigit){ int c=(int)(s32)A0(g); SET_RAX(g,(c>=0&&c<256&&isdigit(c))?4:0); }
IMP(CRTT,isprint){ int c=(int)(s32)A0(g); SET_RAX(g,(c>=0&&c<128&&isprint(c))?0x157:0); }
IMP(CRTT,tolower){ int c=(int)(s32)A0(g); SET_RAX(g,(c>=0&&c<128)?(u32)tolower(c):(u32)c); }
IMP(CRTT,toupper){ int c=(int)(s32)A0(g); SET_RAX(g,(c>=0&&c<128)?(u32)toupper(c):(u32)c); }
IMP(CRTT,towlower){ u32 c=(u32)A0(g)&0xffff; SET_RAX(g,(c<128)?(u32)tolower((int)c):c); }
IMP(CRTT,wcsncmp){ u64 a=A0(g),b=A1(g),n=A2(g); for(u64 i=0;i<n;i++){ u16 x=rd16(g,a+2*i),y=rd16(g,b+2*i); if(x!=y){ SET_RAX(g,(u32)(x<y?-1:1)); return; } if(!x) break; } SET_RAX(g,0); }
IMP(CRTT,wcsncpy){ u64 d=A0(g),s=A1(g),n=A2(g); u64 i=0; for(;i<n;i++){ u16 c=rd16(g,s+2*i); wr16(g,d+2*i,c); if(!c){ i++; break; } } for(;i<n;i++) wr16(g,d+2*i,0); SET_RAX(g,d); }
IMP(CRTT,wcsncpy_s){ u64 d=A0(g),n=A1(g),s=A2(g),cnt=A3(g); if(!d||!n){ SET_RAX(g,22); return; } u64 l=0; while((cnt==(u64)-1||l<cnt)&&rd16(g,s+2*l)) l++; if(l>=n){ if(cnt==(u64)-1){ wr16(g,d,0); SET_RAX(g,34); return; } l=n-1; }
    for(u64 i=0;i<l;i++) wr16(g,d+2*i,rd16(g,s+2*i)); wr16(g,d+2*l,0); SET_RAX(g,0); }
static __thread u64 wcstok_state;
IMP(CRTT,wcstok){ u64 str=A0(g),delim=A1(g),ctxp=A2(g); u64 *ctx=ctxp?NULL:&wcstok_state; u64 cur=str?str:(ctxp?rd64(g,ctxp):*ctx);
    #define ISD(c) ({ int _r=0; for(u64 q=delim;rd16(g,q);q+=2) if(rd16(g,q)==(c)){_r=1;break;} _r; })
    if(!cur){ SET_RAX(g,0); return; } while(rd16(g,cur)&&ISD(rd16(g,cur))) cur+=2; if(!rd16(g,cur)){ if(ctxp) wr64(g,ctxp,cur); else *ctx=cur; SET_RAX(g,0); return; }
    u64 tok=cur; while(rd16(g,cur)&&!ISD(rd16(g,cur))) cur+=2; if(rd16(g,cur)){ wr16(g,cur,0); cur+=2; } if(ctxp) wr64(g,ctxp,cur); else *ctx=cur; SET_RAX(g,tok); }

/* convert */
IMP(CRTC,atoi){ SET_RAX(g,(u32)(s32)atoi(gstr(g,A0(g)))); }
IMP(CRTC,atof){ ret_d(g,atof(gstr(g,A0(g)))); }
IMP(CRTC,mbstowcs){ u64 d=A0(g); const char *s=gstr(g,A1(g)); u64 n=A2(g);
    if(!d){ size_t l=0; char *tmp=malloc(strlen(s)*2+8); /* count UTF-16 units */ for(const unsigned char *p=(const unsigned char*)s;*p;){ unsigned c=*p; int ex=c>=0xF0?3:c>=0xE0?2:c>=0xC0?1:0; p+=1+ex; l+=(c>=0xF0)?2:1; } free(tmp); SET_RAX(g,l); return; }
    size_t lim=n?n:1; utf8_to_gw(g,d,lim+1,s); size_t l=gw_len(g,d); if(l>n) l=n; SET_RAX(g,l); }
IMP(CRTC,wcstombs_s){ u64 pret=A0(g),d=A1(g),dsz=A2(g),src=A3(g); u64 mx=ARG(g,4); char tmp[8192]; gw_to_utf8(g,src,tmp,sizeof tmp); size_t l=strlen(tmp);
    if(!d){ if(pret) wr64(g,pret,l+1); SET_RAX(g,0); return; } if(l+1>dsz){ if(dsz) wr8(g,d,0); SET_RAX(g,34); return; } (void)mx; memcpy(hp(g,d),tmp,l+1); if(pret) wr64(g,pret,l+1); SET_RAX(g,0); }
IMP(CRTC,wcstoul){ u64 s=A0(g),endp=A1(g); int base=(int)A2(g); char tmp[128]; size_t i=0; u64 p=s; while(i<sizeof tmp-1&&rd16(g,p)&&rd16(g,p)<128){ tmp[i++]=(char)rd16(g,p); p+=2; } tmp[i]=0;
    char *e; unsigned long v=strtoul(tmp,&e,base); if(endp) wr64(g,endp,s+2*(u64)(e-tmp)); SET_RAX(g,(u32)v); }

/* utility: qsort / bsearch reproduce the Microsoft algorithms exactly (the permutation of equal keys depends on them) */
static int qcmp(KCtx *g,u64 cmp,u64 a,u64 b){ return (s32)kh_call(g,cmp,a,b,0,0); }
static void qswap(KCtx *g,u64 a,u64 b,u64 w){ if(a==b) return; u8 *pa=hp(g,a),*pb=hp(g,b); while(w--){ u8 t=*pa; *pa++=*pb; *pb++=t; } }
static void shortsort(KCtx *g,u64 lo,u64 hi,u64 w,u64 cmp){ while(hi>lo){ u64 mx=lo; for(u64 p=lo+w;p<=hi;p+=w) if(qcmp(g,cmp,p,mx)>0) mx=p; qswap(g,mx,hi,w); hi-=w; } }
IMP(CRTU,qsort){
    u64 base=A0(g),num=A1(g),w=A2(g),cmp=A3(g); if(num<2||w==0) return;
    u64 lostk[64],histk[64]; int stk=0; u64 lo=base,hi=base+w*(num-1);
    for(;;){
        u64 size=(hi-lo)/w+1;
        if(size<=8){ shortsort(g,lo,hi,w,cmp); }
        else {
            u64 mid=lo+(size/2)*w;
            if(qcmp(g,cmp,lo,mid)>0) qswap(g,lo,mid,w);
            if(qcmp(g,cmp,lo,hi)>0) qswap(g,lo,hi,w);
            if(qcmp(g,cmp,mid,hi)>0) qswap(g,mid,hi,w);
            u64 loguy=lo,higuy=hi;
            for(;;){
                if(mid>loguy){ do{ loguy+=w; }while(loguy<mid&&qcmp(g,cmp,loguy,mid)<=0); }
                if(mid<=loguy){ do{ loguy+=w; }while(loguy<=hi&&qcmp(g,cmp,loguy,mid)<=0); }
                do{ higuy-=w; }while(higuy>mid&&qcmp(g,cmp,higuy,mid)>0);
                if(higuy<loguy) break;
                qswap(g,loguy,higuy,w); if(mid==higuy) mid=loguy;
            }
            higuy+=w;
            if(mid<higuy){ do{ higuy-=w; }while(higuy>mid&&qcmp(g,cmp,higuy,mid)==0); }
            if(mid>=higuy){ do{ higuy-=w; }while(higuy>lo&&qcmp(g,cmp,higuy,mid)==0); }
            if(higuy-lo>=hi-loguy){ if(lo<higuy){ lostk[stk]=lo; histk[stk]=higuy; stk++; } if(loguy<hi){ lo=loguy; continue; } }
            else { if(loguy<hi){ lostk[stk]=loguy; histk[stk]=hi; stk++; } if(lo<higuy){ hi=higuy; continue; } }
        }
        if(stk==0) break; stk--; lo=lostk[stk]; hi=histk[stk];
    }
}
IMP(CRTU,bsearch){ u64 key=A0(g),base=A1(g),num=A2(g),w=A3(g),cmp=ARG(g,4); u64 lo=base,hi=base+(num?num-1:0)*w; u64 res=0;
    if(num==0){ SET_RAX(g,0); return; }
    while(lo<=hi){ u64 half=num/2; if(half){ u64 mid=lo+(num&1?half:half-1)*w; int r=(s32)kh_call(g,cmp,key,mid,0,0);
            if(!r){ res=mid; break; } else if(r<0){ hi=mid-w; num=num&1?half:half-1; } else { lo=mid+w; num=half; } }
        else if(num){ res=(s32)kh_call(g,cmp,key,lo,0,0)?0:lo; break; } else break; }
    SET_RAX(g,res); }
static __thread u32 holdrand=1;
IMP(CRTU,rand){ holdrand=holdrand*214013u+2531011u; SET_RAX(g,(holdrand>>16)&0x7fff); }
IMP(CRTU,srand){ holdrand=(u32)A0(g); }

/* time */
static u64 tm_block(KCtx *g){ static __thread u64 b; if(!b) b=ga_alloc(64,16); return b; }
static void put_tm(KCtx *g,u64 b,const struct tm *t){ wr32(g,b,t->tm_sec); wr32(g,b+4,t->tm_min); wr32(g,b+8,t->tm_hour); wr32(g,b+12,t->tm_mday); wr32(g,b+16,t->tm_mon); wr32(g,b+20,t->tm_year); wr32(g,b+24,t->tm_wday); wr32(g,b+28,t->tm_yday); wr32(g,b+32,t->tm_isdst>0?1:0); }
IMP(CRTI,_time64){ time_t t=kh_cut_enabled?(time_t)(kh_cut_unix_ns()/1000000000ull):time(0); if(A0(g)) wr64(g,A0(g),(u64)t); SET_RAX(g,(u64)t); }
IMP(CRTI,_localtime64){ time_t t=(time_t)rd64(g,A0(g)); struct tm r; localtime_r(&t,&r); u64 b=tm_block(g); put_tm(g,b,&r); SET_RAX(g,b); }
IMP(CRTI,_gmtime64){ time_t t=(time_t)rd64(g,A0(g)); struct tm r; gmtime_r(&t,&r); u64 b=tm_block(g); put_tm(g,b,&r); SET_RAX(g,b); }
IMP(CRTI,_mktime64){ u64 b=A0(g); struct tm r; memset(&r,0,sizeof r); r.tm_sec=(int)rd32(g,b); r.tm_min=(int)rd32(g,b+4); r.tm_hour=(int)rd32(g,b+8); r.tm_mday=(int)rd32(g,b+12); r.tm_mon=(int)rd32(g,b+16); r.tm_year=(int)rd32(g,b+20); r.tm_isdst=(int)rd32(g,b+32); SET_RAX(g,(u64)mktime(&r)); }
IMP(CRTI,_difftime64){ ret_d(g,(double)((s64)A0(g)-(s64)A1(g))); }
IMP(CRTI,clock){ if(kh_cut_enabled){ SET_RAX(g,kh_cut_elapsed_ns()/1000000); return; } static u64 t0; u64 n=kh_now_ns()/1000000ull; if(!t0) t0=n; SET_RAX(g,n-t0); }
IMP(CRTM,__setusermatherr){ }
IMP(CRTL,_configthreadlocale){ SET_RAX(g,0); }
IMP(CRTF,_mkdir){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); if(ga_deterministic){ SET_RAX(g,0); return; } int r=mkdir(p,0755); if(r){ kh_set_errno(g,errno); kh_set_last_error(errno==EEXIST?183:3); } SET_RAX(g,(u32)r); }
IMP(CRTF,_chdir){ SET_RAX(g,0); kh_warn("_chdir(%s) ignored (game dir is the working directory)",gstr(g,A0(g))); }

/* ======================================================================================================================
 * printf engine over a Win64 va_list in guest memory
 * ==================================================================================================================== */
typedef struct { char *b; size_t n, cap; } Buf;
static void bput(Buf *b,const char *s,size_t n){ if(b->n+n+1>b->cap){ b->cap=(b->n+n+1)*2; b->b=realloc(b->b,b->cap); } memcpy(b->b+b->n,s,n); b->n+=n; b->b[b->n]=0; }
static void bputc(Buf *b,char c){ bput(b,&c,1); }
static void put_utf8(Buf *b,u32 c){ char t[4]; int n; if(c<0x80){ t[0]=(char)c; n=1; } else if(c<0x800){ t[0]=(char)(0xC0|(c>>6)); t[1]=(char)(0x80|(c&63)); n=2; } else if(c<0x10000){ t[0]=(char)(0xE0|(c>>12)); t[1]=(char)(0x80|((c>>6)&63)); t[2]=(char)(0x80|(c&63)); n=3; } else { t[0]=(char)(0xF0|(c>>18)); t[1]=(char)(0x80|((c>>12)&63)); t[2]=(char)(0x80|((c>>6)&63)); t[3]=(char)(0x80|(c&63)); n=4; } bput(b,t,n); }
static void fmt_core(KCtx *g,Buf *out,const char *f,u64 va,int swap_s){
    u64 ap=va;
    #define NEXT() ({ u64 _v=rd64(g,ap); ap+=8; _v; })
    while(*f){
        if(*f!='%'){ const char *e=strchr(f,'%'); size_t n=e?(size_t)(e-f):strlen(f); bput(out,f,n); f+=n; continue; }
        const char *p=f+1; char flags[8]; int nf=0; int width=-1,prec=-1;
        while(*p&&strchr("-+ #0",*p)){ if(nf<7) flags[nf++]=*p; p++; } flags[nf]=0;
        if(*p=='*'){ width=(int)(s32)NEXT(); p++; if(width<0){ if(nf<7) flags[nf++]='-'; flags[nf]=0; width=-width; } } else if(isdigit((unsigned char)*p)){ width=0; while(isdigit((unsigned char)*p)) width=width*10+(*p++-'0'); }
        if(*p=='.'){ p++; prec=0; if(*p=='*'){ prec=(int)(s32)NEXT(); p++; } else while(isdigit((unsigned char)*p)) prec=prec*10+(*p++-'0'); }
        int lenmod=0; /* 0 int, 1 hh, 2 h, 3 l(32), 4 ll/I64/z/j/t/I (64), 5 w (wide), 6 L */
        for(;;){ if(*p=='h'){ if(p[1]=='h'){ lenmod=1; p+=2; } else { lenmod=2; p++; } } else if(*p=='l'){ if(p[1]=='l'){ lenmod=4; p+=2; } else { lenmod=3; p++; } }
            else if(*p=='w'){ lenmod=5; p++; } else if(*p=='z'||*p=='j'||*p=='t'){ lenmod=4; p++; } else if(*p=='L'){ lenmod=6; p++; }
            else if(*p=='I'){ if(p[1]=='6'&&p[2]=='4'){ lenmod=4; p+=3; } else if(p[1]=='3'&&p[2]=='2'){ lenmod=0; p+=3; } else { lenmod=4; p++; } } else break; }
        char c=*p; if(!c) break; p++;
        char spec[64]; char tmp[512]; size_t sl;
        switch(c){
        case '%': bputc(out,'%'); break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            u64 v=NEXT(); long long sv; unsigned long long uv;
            switch(lenmod){ case 1: sv=(s8)v; uv=(u8)v; break; case 2: sv=(s16)v; uv=(u16)v; break; case 4: sv=(s64)v; uv=v; break; default: sv=(s32)v; uv=(u32)v; }
            snprintf(spec,sizeof spec,"%%%s%s%s%s%s",flags,width>=0?"*":"",prec>=0?".*":"","ll",(c=='i'?"d":(char[2]){c,0}));
            int n; if(c=='d'||c=='i'){ if(width>=0&&prec>=0) n=snprintf(tmp,sizeof tmp,spec,width,prec,sv); else if(width>=0) n=snprintf(tmp,sizeof tmp,spec,width,sv); else if(prec>=0) n=snprintf(tmp,sizeof tmp,spec,prec,sv); else n=snprintf(tmp,sizeof tmp,spec,sv); }
            else { if(width>=0&&prec>=0) n=snprintf(tmp,sizeof tmp,spec,width,prec,uv); else if(width>=0) n=snprintf(tmp,sizeof tmp,spec,width,uv); else if(prec>=0) n=snprintf(tmp,sizeof tmp,spec,prec,uv); else n=snprintf(tmp,sizeof tmp,spec,uv); }
            bput(out,tmp,(size_t)(n<(int)sizeof tmp?n:(int)sizeof tmp-1)); break; }
        case 'c': { u64 v=NEXT(); int wide=(lenmod==3||lenmod==5)||(swap_s&&0); char t[8]; size_t n; Buf one={0};
            if(wide){ put_utf8(&one,(u32)(v&0xffff)); } else { bputc(&one,(char)v); } n=one.n; memcpy(t,one.b,n>7?7:n); free(one.b);
            int pad=width>1?width-1:0; if(!strchr(flags,'-')) while(pad--) bputc(out,' '),pad=pad; bput(out,t,n>7?7:n); if(strchr(flags,'-')){ pad=width>1?width-1:0; while(pad--) bputc(out,' '); } break; }
        case 'C': { u64 v=NEXT(); Buf one={0}; put_utf8(&one,(u32)(v&0xffff)); bput(out,one.b,one.n); free(one.b); break; }
        case 's': case 'S': {
            u64 v=NEXT(); int wide=(c=='s')?(lenmod==3||lenmod==5||swap_s):!(lenmod==3||lenmod==5||swap_s&&0);
            if(c=='S'){ wide=!swap_s; if(lenmod==2) wide=0; if(lenmod==3||lenmod==5) wide=1; } else { if(lenmod==2) wide=0; }
            Buf s={0};
            if(!v){ bput(&s,"(null)",6); }
            else if(wide){ size_t n=0; while(rd16(g,v+2*n)&&(prec<0||(int)n<prec)) n++; for(size_t i=0;i<n;i++){ u32 cc=rd16(g,v+2*i); if(cc>=0xD800&&cc<0xDC00&&i+1<n){ u32 d=rd16(g,v+2*i+2); cc=0x10000+((cc-0xD800)<<10)+(d-0xDC00); i++; } put_utf8(&s,cc); } }
            else { size_t n=strnlen(gstr(g,v),prec<0?(size_t)-1:(size_t)prec); bput(&s,gstr(g,v),n); }
            int pad=width>(int)s.n?width-(int)s.n:0; if(!strchr(flags,'-')) while(pad-->0) bputc(out,' '); bput(out,s.b,s.n); if(strchr(flags,'-')) while(pad-->0) bputc(out,' '); free(s.b); break; }
        case 'p': { u64 v=NEXT(); snprintf(tmp,sizeof tmp,"%016llX",(unsigned long long)v); int pad=width>16?width-16:0; if(!strchr(flags,'-')) while(pad-->0) bputc(out,' '); bput(out,tmp,16); if(strchr(flags,'-')) while(pad-->0) bputc(out,' '); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            u64 v=NEXT(); double d; memcpy(&d,&v,8);
            snprintf(spec,sizeof spec,"%%%s%s%s%c",flags,width>=0?"*":"",prec>=0?".*":"",c); int n;
            if(width>=0&&prec>=0) n=snprintf(tmp,sizeof tmp,spec,width,prec,d); else if(width>=0) n=snprintf(tmp,sizeof tmp,spec,width,d); else if(prec>=0) n=snprintf(tmp,sizeof tmp,spec,prec,d); else n=snprintf(tmp,sizeof tmp,spec,d);
            bput(out,tmp,(size_t)(n<(int)sizeof tmp?n:(int)sizeof tmp-1)); break; }
        case 'n': (void)NEXT(); break;
        default: bputc(out,'%'); (void)sl; bputc(out,c); break;
        }
        f=p;
    }
    #undef NEXT
}
int kh_vformat(KCtx *g,char *dst,size_t cap,const char *fmt,u64 va,int wide_fmt,int *needed){
    (void)wide_fmt; Buf b={0}; fmt_core(g,&b,fmt,va,0); if(needed) *needed=(int)b.n;
    size_t n=b.n<cap?b.n:(cap?cap-1:0); if(cap){ memcpy(dst,b.b?b.b:"",n); dst[n]=0; } free(b.b); return (int)n;
}
int kh_vformat_w(KCtx *g,u16 *dst,size_t cap,const u16 *wfmt,u64 va,int *needed){
    size_t fl=0; while(wfmt[fl]) fl++; char *u=malloc(fl*4+4); size_t o=0; for(size_t i=0;i<fl;i++){ u32 c=wfmt[i]; if(c>=0xD800&&c<0xDC00&&i+1<fl){ c=0x10000+((c-0xD800)<<10)+(wfmt[i+1]-0xDC00); i++; }
        if(c<0x80) u[o++]=(char)c; else if(c<0x800){ u[o++]=(char)(0xC0|(c>>6)); u[o++]=(char)(0x80|(c&63)); } else if(c<0x10000){ u[o++]=(char)(0xE0|(c>>12)); u[o++]=(char)(0x80|((c>>6)&63)); u[o++]=(char)(0x80|(c&63)); } else { u[o++]=(char)(0xF0|(c>>18)); u[o++]=(char)(0x80|((c>>12)&63)); u[o++]=(char)(0x80|((c>>6)&63)); u[o++]=(char)(0x80|(c&63)); } }
    u[o]=0; Buf b={0}; fmt_core(g,&b,u,va,1); free(u);
    /* utf8 -> utf16 into a temp, count units */
    size_t cnt=0; u16 *w=malloc((b.n+1)*2*sizeof(u16)); for(const unsigned char *p=(const unsigned char*)(b.b?b.b:"");*p;){ u32 c=*p++; if(c>=0xF0&&p[0]&&p[1]&&p[2]){ c=((c&7)<<18)|((p[0]&63)<<12)|((p[1]&63)<<6)|(p[2]&63); p+=3; } else if(c>=0xE0&&p[0]&&p[1]){ c=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63); p+=2; } else if(c>=0xC0&&p[0]){ c=((c&31)<<6)|(p[0]&63); p++; }
        if(c>=0x10000){ c-=0x10000; w[cnt++]=(u16)(0xD800+(c>>10)); w[cnt++]=(u16)(0xDC00+(c&1023)); } else w[cnt++]=(u16)c; }
    if(needed) *needed=(int)cnt; size_t n=cnt<cap?cnt:(cap?cap-1:0); if(cap){ memcpy(dst,w,n*2); dst[n]=0; } free(w); free(b.b); return (int)n;
}
/* __stdio_common_* : (options u64, ..., locale, va_list) */
IMP(CRTS,__stdio_common_vsprintf){ /* (opts, buf, count, fmt, locale, arglist) */
    u64 opts=A0(g),buf=A1(g),cnt=A2(g); const char *fmt=gstr(g,A3(g)); u64 va=ARG(g,5); int need; kh_vformat(g,(char*)hp(g,buf),cnt,fmt,va,0,&need);
    int ret; if(opts&2) ret=need; else ret=((u64)need>=cnt&&cnt)?-1:need; if(cnt&&(u64)need>=cnt&&!(opts&2)){ /* _vsnprintf: no terminator on truncation */ } SET_RAX(g,(u32)ret); }
IMP(CRTS,__stdio_common_vsprintf_s){ u64 buf=A1(g),cnt=A2(g); const char *fmt=gstr(g,A3(g)); u64 va=ARG(g,5); int need; kh_vformat(g,(char*)hp(g,buf),cnt,fmt,va,0,&need); SET_RAX(g,(u32)((u64)need>=cnt?-1:need)); }
IMP(CRTS,__stdio_common_vsnprintf_s){ /* (opts, buf, bufsz, count, fmt, locale, arglist) */
    u64 buf=A1(g),bsz=A2(g),cnt=A3(g); const char *fmt=gstr(g,ARG(g,4)); u64 va=ARG(g,6); int need; size_t lim=bsz; if(cnt!=(u64)-1&&cnt+1<lim) lim=cnt+1;
    kh_vformat(g,(char*)hp(g,buf),lim,fmt,va,0,&need); SET_RAX(g,(u32)((u64)need>=lim&&cnt!=(u64)-1&&(u64)need>cnt?-1:need)); }
IMP(CRTS,__stdio_common_vswprintf){ u64 buf=A1(g),cnt=A2(g),fmt=A3(g),va=ARG(g,5); int need; kh_vformat_w(g,(u16*)hp(g,buf),cnt,(const u16*)hp(g,fmt),va,&need); SET_RAX(g,(u32)(((u64)need>=cnt)?-1:need)); }
IMP(CRTS,__stdio_common_vswprintf_s){ u64 buf=A1(g),cnt=A2(g),fmt=A3(g),va=ARG(g,5); int need; kh_vformat_w(g,(u16*)hp(g,buf),cnt,(const u16*)hp(g,fmt),va,&need); SET_RAX(g,(u32)(((u64)need>=cnt)?-1:need)); }
IMP(CRTS,__stdio_common_vswscanf){ KH_UNIMPL(g,"__stdio_common_vswscanf"); }

/* ======================================================================================================================
 * stdio FILE objects: a FILE* handed to the guest is a 16-byte guest block holding an index into the host table
 * ==================================================================================================================== */
#define MAXF 4096
static FILE *ftab[MAXF]; static u64 fblk[MAXF]; static pthread_mutex_t fm=PTHREAD_MUTEX_INITIALIZER;
static u64 file_new(KCtx *g,FILE *f){ pthread_mutex_lock(&fm); for(int i=3;i<MAXF;i++) if(!ftab[i]){ ftab[i]=f; if(!fblk[i]) fblk[i]=ga_alloc(16,16); wr64(g,fblk[i],(u64)i); pthread_mutex_unlock(&fm); return fblk[i]; } pthread_mutex_unlock(&fm); return 0; }
static FILE *file_of(KCtx *g,u64 p){ if(!p) return NULL; u64 i=rd64(g,p); if(i>=MAXF) return NULL; return ftab[i]; }
IMP(CRTS,__acrt_iob_func){ int i=(int)A0(g); static FILE *std[3]; std[0]=stdin; std[1]=stdout; std[2]=stderr; pthread_mutex_lock(&fm); if(!ftab[i]){ ftab[i]=std[i]; fblk[i]=ga_alloc(16,16); wr64(g,fblk[i],(u64)i); } pthread_mutex_unlock(&fm); SET_RAX(g,fblk[i]); }
static __thread u64 commode_ga;
IMP(CRTS,__p__commode){ static u64 p; if(!p){ p=ga_alloc(16,16); wr32(g,p,0); } SET_RAX(g,p); }
IMP(CRTS,_set_fmode){ SET_RAX(g,0); }
static const char *mode_fix(const char *m,char *out){ size_t n=0; for(;*m&&n<15;m++){ if(*m=='t'||*m=='b'||*m=='N'||*m==',') { if(*m==',') break; continue; } out[n++]=*m; } out[n]=0; return out; }
IMP(CRTS,fopen){ char p[2048],m[16]; kh_winpath(gstr(g,A0(g)),p,sizeof p); FILE *f=fopen(p,mode_fix(gstr(g,A1(g)),m)); if(kh_verbose||(!f&&nopenfail++<60)) fprintf(stderr,"kh: file: fopen(\"%s\" -> \"%s\") = %p\n",gstr(g,A0(g)),p,(void*)f); if(!f){ kh_set_last_error(2); SET_RAX(g,0); return; } SET_RAX(g,file_new(g,f)); }
IMP(CRTS,fopen_s){ char p[2048],m[16]; kh_winpath(gstr(g,A1(g)),p,sizeof p); FILE *f=fopen(p,mode_fix(gstr(g,A2(g)),m)); if(!f){ wr64(g,A0(g),0); SET_RAX(g,(u32)errno); return; } wr64(g,A0(g),file_new(g,f)); SET_RAX(g,0); }
IMP(CRTS,_wfopen_s){ char p[2048],m[16],mm[64]; kh_winpath_w(g,A1(g),p,sizeof p); gw_to_utf8(g,A2(g),mm,sizeof mm); FILE *f=fopen(p,mode_fix(mm,m)); if(!f){ wr64(g,A0(g),0); SET_RAX(g,(u32)errno); return; } wr64(g,A0(g),file_new(g,f)); SET_RAX(g,0); }
IMP(CRTS,fclose){ FILE *f=file_of(g,A0(g)); u64 i=rd64(g,A0(g)); if(!f){ SET_RAX(g,(u32)-1); return; } int r=(i>2)?fclose(f):0; if(i>2){ pthread_mutex_lock(&fm); ftab[i]=NULL; pthread_mutex_unlock(&fm); } SET_RAX(g,(u32)r); }
IMP(CRTS,fflush){ if(!A0(g)){ fflush(NULL); SET_RAX(g,0); return; } FILE *f=file_of(g,A0(g)); SET_RAX(g,(u32)(f?fflush(f):-1)); }
IMP(CRTS,fgets){ FILE *f=file_of(g,A2(g)); char *r=f?fgets((char*)hp(g,A0(g)),(int)A1(g),f):NULL; SET_RAX(g,r?A0(g):0); }
IMP(CRTS,fputc){ FILE *f=file_of(g,A1(g)); SET_RAX(g,(u32)(f?fputc((int)(A0(g)&0xff),f):-1)); }
IMP(CRTS,fread){ FILE *f=file_of(g,A3(g)); SET_RAX(g,f?fread(hp(g,A0(g)),A1(g),A2(g),f):0); }
IMP(CRTS,fwrite){ FILE *f=file_of(g,A3(g)); SET_RAX(g,f?fwrite(hp(g,A0(g)),A1(g),A2(g),f):0); }
IMP(CRTS,fseek){ FILE *f=file_of(g,A0(g)); SET_RAX(g,(u32)(f?fseek(f,(long)(s32)A1(g),(int)A2(g)):-1)); }
IMP(CRTS,ftell){ FILE *f=file_of(g,A0(g)); SET_RAX(g,(u32)(f?(s32)ftell(f):-1)); }
IMP(CRTS,__stdio_common_vfprintf){ /* (opts, FILE*, fmt, locale, arglist) */
    FILE *f=file_of(g,A1(g)); int need; char *b=malloc(65536); kh_vformat(g,b,65536,gstr(g,A2(g)),ARG(g,4),0,&need); if(f) fwrite(b,1,need<65536?(size_t)need:65535,f); free(b); SET_RAX(g,(u32)need); }
/* Low-level CRT descriptors are guest-visible host numbers. Loose audio
 * streams have a separate offset-based table in stream_layer.c. */
typedef struct { int fd,used; char *path; } CutFile;
static CutFile cut_fds[MAXF]; static pthread_mutex_t cut_fm=PTHREAD_MUTEX_INITIALIZER;
static int cut_fd_new(int fd,const char *path){
    if(!kh_cut_enabled) return fd;
    pthread_mutex_lock(&cut_fm);
    int i=3; while(i<MAXF&&cut_fds[i].used) i++;
    if(i==MAXF) kh_fatal(NULL,"savestate-crt-descriptor-table-full");
    cut_fds[i]=(CutFile){fd,1,path?strdup(path):NULL};
    if(path&&!cut_fds[i].path) kh_fatal(NULL,"savestate-crt-path-memory");
    pthread_mutex_unlock(&cut_fm); return i;
}
static int cut_fd(int i){
    if(!kh_cut_enabled||i<3) return i;
    pthread_mutex_lock(&cut_fm);
    int fd=i<MAXF&&cut_fds[i].used?cut_fds[i].fd:-1;
    pthread_mutex_unlock(&cut_fm); return fd;
}
static int conv_oflag(int o){ int r=0; switch(o&3){ case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; } if(o&8) r|=O_APPEND; if(o&0x100) r|=O_CREAT; if(o&0x200) r|=O_TRUNC; if(o&0x400) r|=O_EXCL; return r; }
int kh_movie_open_fd(KCtx *g);                      /* task 44 (runtime/null_mf.c): the header read of Movie::open gets a stand-in, the file is not opened */
IMP(CRTS,_sopen_dispatch){ char p[2048]; kh_winpath(gstr(g,A0(g)),p,sizeof p); u64 pfh=ARG(g,4);
    { int mfd=kh_movie_open_fd(g); if(mfd>=0){ if(!pfh){ close(mfd); kh_set_errno(g,EINVAL); SET_RAX(g,EINVAL); return; } wr32(g,pfh,(u32)cut_fd_new(mfd,NULL)); SET_RAX(g,0); return; } }
    int fd=open(p,conv_oflag((int)A1(g)),0644);
    if(kh_verbose||strstr(p,".hed")||strstr(p,".pkg")||(fd<0&&nopenfail++<60)) fprintf(stderr,"kh: file: _sopen(\"%s\" -> \"%s\", oflag=%x) = %d%s\n",gstr(g,A0(g)),p,(unsigned)A1(g),fd,fd<0?" FAILED":"");
    if(fd<0){ int en=errno; kh_set_errno(g,en); if(pfh) wr32(g,pfh,(u32)-1); SET_RAX(g,(u32)en); kh_set_last_error(2); return; }
    /* _sopen_dispatch always returns errno_t; arg 6 selects validation, not the return ABI. */
    if(!pfh){ close(fd); kh_set_errno(g,EINVAL); SET_RAX(g,EINVAL); return; }
    wr32(g,pfh,(u32)cut_fd_new(fd,p)); SET_RAX(g,0); }
IMP(CRTS,_close){ int i=(int)A0(g);
    if(!kh_cut_enabled||i<3){ SET_RAX(g,(u32)close(i)); return; }
    pthread_mutex_lock(&cut_fm);
    int rc=-1;
    if(i<MAXF&&cut_fds[i].used){ rc=close(cut_fds[i].fd); free(cut_fds[i].path); cut_fds[i]=(CutFile){0}; }
    pthread_mutex_unlock(&cut_fm); SET_RAX(g,(u32)rc); }
IMP(CRTS,_read){ SET_RAX(g,(u32)(s32)read(cut_fd((int)A0(g)),hp(g,A1(g)),(unsigned)A2(g))); }
IMP(CRTS,_write){ SET_RAX(g,(u32)(s32)write(cut_fd((int)A0(g)),hp(g,A1(g)),(unsigned)A2(g))); }
IMP(CRTS,_lseeki64){ SET_RAX(g,(u64)lseek(cut_fd((int)A0(g)),(off_t)(s64)A1(g),(int)A2(g))); }

/* ======================================================================================================================
 * startup / runtime services (UCRT runtime dll)
 * ==================================================================================================================== */
IMP(CRTR,_set_app_type){ SET_RAX(g,0); }
IMP(CRTR,_configure_narrow_argv){ SET_RAX(g,0); }
IMP(CRTR,_initialize_narrow_environment){ SET_RAX(g,0); }
IMP(CRTR,_get_narrow_winmain_command_line){ if(!kh_cmdline_ga) kh_cmdline_ga=ga_strdup("\"C:\\KH2\\KINGDOM HEARTS II FINAL MIX.exe\" "); SET_RAX(g,kh_cmdline_ga); }
IMP(CRTR,_initterm){ u64 a=A0(g),b=A1(g); for(u64 p=a;p<b;p+=8){ u64 fn=rd64(g,p); if(fn){ kh_cnt_init_xc++; kh_call(g,fn,0,0,0,0); } } }
IMP(CRTR,_initterm_e){ u64 a=A0(g),b=A1(g); u32 r=0; for(u64 p=a;p<b&&!r;p+=8){ u64 fn=rd64(g,p); if(fn){ kh_cnt_init_xi++; r=(u32)kh_call(g,fn,0,0,0,0); } } SET_RAX(g,r); }
typedef struct OnexitTab { u64 table; u64 *fns; int n,cap; struct OnexitTab *next; } OnexitTab; static OnexitTab *otabs; static pthread_mutex_t om=PTHREAD_MUTEX_INITIALIZER;
static OnexitTab *otab(u64 t){ for(OnexitTab *o=otabs;o;o=o->next) if(o->table==t) return o; OnexitTab *o=calloc(1,sizeof *o); o->table=t; o->next=otabs; otabs=o; return o; }
IMP(CRTR,_initialize_onexit_table){ u64 t=A0(g); if(!t){ SET_RAX(g,(u32)-1); return; } pthread_mutex_lock(&om); otab(t); pthread_mutex_unlock(&om); wr64(g,t,0); wr64(g,t+8,0); wr64(g,t+16,0); SET_RAX(g,0); }
static int onexit_add(u64 t,u64 fn){ pthread_mutex_lock(&om); OnexitTab *o=otab(t); if(o->n==o->cap){ o->cap=o->cap?o->cap*2:16; o->fns=realloc(o->fns,o->cap*8); } o->fns[o->n++]=fn; pthread_mutex_unlock(&om); return 0; }
IMP(CRTR,_register_onexit_function){ SET_RAX(g,(u32)onexit_add(A0(g),A1(g))); }
IMP(CRTR,_execute_onexit_table){ pthread_mutex_lock(&om); OnexitTab *o=otab(A0(g)); int n=o->n; u64 *fs=malloc(8*(size_t)(n?n:1)); memcpy(fs,o->fns,8*(size_t)n); o->n=0; pthread_mutex_unlock(&om); for(int i=n-1;i>=0;i--) kh_call(g,fs[i],0,0,0,0); free(fs); SET_RAX(g,0); }
IMP(CRTR,_crt_atexit){ onexit_add(1,A0(g)); SET_RAX(g,0); }
IMP(CRTR,_crt_at_quick_exit){ SET_RAX(g,0); }
IMP(CRTR,_register_thread_local_exe_atexit_callback){ onexit_add(2,A0(g)); }
IMP(CRTR,_seh_filter_exe){ kh_fatal(g,"_seh_filter_exe: unhandled exception in guest (terminate-on-throw policy), code=%llx",(unsigned long long)A0(g)); }
IMP(CRTR,_seh_filter_dll){ kh_fatal(g,"_seh_filter_dll reached"); }
extern void kh_exit_process(int code);
IMPN(CRTR,exit){ kh_exit_process((int)A0(g)); }
IMPN(CRTR,_exit){ kh_exit_process((int)A0(g)); }
IMP(CRTR,_cexit){ }
IMP(CRTR,_c_exit){ }
IMPN(CRTR,abort){ kh_fatal(g,"abort() called by guest"); }
IMPN(CRTR,terminate){ kh_fatal(g,"terminate() called by guest"); }
IMP(CRTR,perror){ fprintf(stderr,"kh: guest perror: %s\n",gstr(g,A0(g))); }
IMPN(CRTR,_invalid_parameter_noinfo_noreturn){ kh_fatal(g,"_invalid_parameter_noinfo_noreturn (CRT parameter validation failed in guest)"); }
IMP(CRTR,_invalid_parameter_noinfo){ kh_warn("_invalid_parameter_noinfo called (site rva 0x%llx)",(unsigned long long)(guest_return_addr(g)-GS_BASE)); }
u64 kh_errno_ptr(KCtx *g){ KThread *t=cur_thread(); u64 e=rd64(g,t->teb+0x700); if(!e){ e=ga_alloc(16,16); wr64(g,e,0); wr64(g,t->teb+0x700,e); } return e; }
void kh_set_errno(KCtx *g,int v){ wr32(g,kh_errno_ptr(g),(u32)v); }
IMP(CRTR,_errno){ SET_RAX(g,kh_errno_ptr(g)); }
IMP(CRTR,_get_errno){ u64 e=kh_errno_ptr(g); if(A0(g)) wr32(g,A0(g),rd32(g,e)); SET_RAX(g,0); }

/* ======================================================================================================================
 * vcruntime140 / msvcp140: C++ throw thunks (terminate-on-throw), setjmp/longjmp, exception helpers
 * ==================================================================================================================== */
IMPN(VCR,_CxxThrowException){
    u64 obj=A0(g),ti=A1(g); char name[160]="?";
    if(ti&&GA_OK(ti)){ u32 cta=rd32(g,ti+12); if(cta){ u32 n=rd32(g,GS_BASE+cta); if(n){ u32 ct=rd32(g,GS_BASE+cta+4); u32 pt=rd32(g,GS_BASE+ct+4); u64 tia=GS_BASE+pt; if(GA_OK(tia)) snprintf(name,sizeof name,"%.150s",(const char*)hp(g,tia+16)); } } }
    kh_fatal(g,"C++ throw reached (terminate-on-throw policy): object=%llx type='%s' throwinfo=%llx",(unsigned long long)obj,name,(unsigned long long)ti);
}
IMPN(VCR,__CxxFrameHandler3){ kh_fatal(g,"__CxxFrameHandler3 called: a C++ exception is being unwound (not supported)"); }
IMPN(VCR,__C_specific_handler){ kh_fatal(g,"__C_specific_handler called (SEH exception, not supported)"); }
IMPN(VCR,__FrameUnwindFilter){ kh_fatal(g,"__FrameUnwindFilter called"); }
IMPN(VCR,_purecall){ kh_fatal(g,"pure virtual function call"); }
IMPN(VCR,__std_terminate){ kh_fatal(g,"__std_terminate (std::terminate)"); }
IMP(VCR,__std_type_info_destroy_list){ }
IMP(VCR,__std_exception_copy){ u64 src=A0(g),dst=A1(g); wr64(g,dst,0); wr64(g,dst+8,0); (void)src; }
IMP(VCR,__std_exception_destroy){ }
/* libpng: _setjmp records nothing and returns 0; longjmp (the PNG error path) is a named fatal. Rationale in README: a second return
 * into a lifted function body cannot be expressed in the C the lifter emits. */
IMP(VCR,__intrinsic_setjmp){ u64 jb=A0(g); memset(hp(g,jb),0,0x100); SET_RAX(g,0); }
IMPN(VCR,longjmp){ kh_fatal(g,"longjmp(%llx,%d): libpng error path reached; PNG decode failed (setjmp second return not supported in lifted code)",(unsigned long long)A0(g),(int)A1(g)); }
IMPN(MSVCP,__Throw_C_error_std__YAXH_Z){ kh_fatal(g,"std::_Throw_C_error(%d)",(int)A0(g)); }
IMPN(MSVCP,__Xbad_function_call_std__YAXXZ){ kh_fatal(g,"std::_Xbad_function_call (empty std::function invoked)"); }
IMPN(MSVCP,__Xlength_error_std__YAXPEBD_Z){ kh_fatal(g,"std::_Xlength_error(\"%s\")",gstr(g,A0(g))); }
IMP(MSVCP,___ExceptionPtrCopy__YAXPEAXPEBX_Z){ wr64(g,A0(g),rd64(g,A1(g))); }
IMP(MSVCP,___ExceptionPtrDestroy__YAXPEAX_Z){ }

#include "rewind.h"
u32 *kh_rewind_rng(void){ return &holdrand; }

const char *kh_rewind_crt_check(void){
    for(int i=3;i<MAXF;i++) if(cut_fds[i].used){
        int fd=cut_fds[i].fd,mode=fcntl(fd,F_GETFL); struct stat st;
        if(!cut_fds[i].path||mode<0||(mode&O_ACCMODE)!=O_RDONLY||fstat(fd,&st)||!S_ISREG(st.st_mode)||
           lseek(fd,0,SEEK_CUR)<0) return "savestate-crt-nonseekable-or-writable";
    }
    for(unsigned i=3;i<MAXF;i++) if(ftab[i]) return "savestate-crt-open-file";
    return NULL;
}
void kh_rewind_crt(KhRecords *r){
    KH_RECORD(r,fblk); KH_RECORD(r,cut_requests_used); KH_RECORD(r,cut_boot_object); KH_RECORD(r,cut_boot_table);
}

struct KhCrtFiles { unsigned n; struct SavedFd { int id,fd; off_t offset; char *path; } files[]; };
struct KhCrtFiles *kh_crt_files_capture(void){
    unsigned n=0;
    for(int i=3;i<MAXF;i++) n+=cut_fds[i].used;
    struct KhCrtFiles *s=calloc(1,sizeof *s+n*sizeof s->files[0]);
    if(!s) kh_fatal(NULL,"savestate-crt-file-memory");
    for(int i=3;i<MAXF;i++) if(cut_fds[i].used){
        struct SavedFd *f=&s->files[s->n++];
        *f=(struct SavedFd){i,fcntl(cut_fds[i].fd,F_DUPFD_CLOEXEC,3),lseek(cut_fds[i].fd,0,SEEK_CUR),strdup(cut_fds[i].path)};
        if(f->fd<0||f->offset<0||!f->path) kh_fatal(NULL,"savestate-crt-pin");
    }
    return s;
}
void kh_crt_files_restore(struct KhCrtFiles *s){
    for(int i=3;i<MAXF;i++) if(cut_fds[i].used){
        close(cut_fds[i].fd); free(cut_fds[i].path); cut_fds[i]=(CutFile){0};
    }
    for(unsigned i=0;i<s->n;i++){
        struct SavedFd *f=&s->files[i];
        int fd=fcntl(f->fd,F_DUPFD_CLOEXEC,3);
        char *path=strdup(f->path);
        if(fd<0||!path||lseek(fd,f->offset,SEEK_SET)!=f->offset) kh_fatal(NULL,"savestate-crt-rebind");
        cut_fds[f->id]=(CutFile){fd,1,path};
    }
}
void kh_crt_files_discard(struct KhCrtFiles *s){
    if(s){ for(unsigned i=0;i<s->n;i++){close(s->files[i].fd); free(s->files[i].path);} free(s); }
}

#include "slot_disk.h"
void kh_disk_files(KhRecords *r,struct KhCrtFiles *s){
    KH_RECORD(r,s->n);
    for(unsigned i=0;i<s->n;i++){
        struct SavedFd *f=s->files+i;
        struct stat st;
        if(fstat(f->fd,&st)) kh_fatal(NULL,"savestate-crt-file-stat");
        u64 identity[]={f->id,st.st_dev,st.st_ino,st.st_size};
        KH_RECORD(r,identity); KH_VALUE(r,f->offset);
    }
}
