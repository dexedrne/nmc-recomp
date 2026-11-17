/* remaster.c : C port of the KH2 container walkers of OpenKH Panacea (Apache-2.0; read from OpenKh.Research.Panacea/Panacea.cpp, cross-checked against
 * tools/platform/remaster_walk.py).  Pure functions over a byte buffer: given the final bytes of a game file they produce the ordered list of
 * "origOffset" values (file offset of each texture + 0x20000000 HD-link flag, -1 = audio slot) that the remastered-asset protocol keys on.
 * All reads are bounds checked; a malformed file sets Buf.err instead of reading out of range (Panacea would read garbage). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "khnmc_remaster.h"

typedef int32_t s32; typedef int16_t s16; typedef int64_t s64; typedef uint8_t u8;
typedef struct { const u8 *b; int64_t n; int err; } Buf;
typedef struct { s32 *v; int n, cap; } Vec;

static s32 I32(Buf *B, int64_t o){ if(o<0||o+4>B->n){ B->err=1; return 0; } s32 v; memcpy(&v,B->b+o,4); return v; }
static s16 I16(Buf *B, int64_t o){ if(o<0||o+2>B->n){ B->err=1; return 0; } s16 v; memcpy(&v,B->b+o,2); return v; }
static s64 I64(Buf *B, int64_t o){ if(o<0||o+8>B->n){ B->err=1; return 0; } s64 v; memcpy(&v,B->b+o,8); return v; }
static int MAGIC(Buf *B, int64_t o, const char *m, int len){ if(o<0||o+len>B->n) return 0; return memcmp(B->b+o,m,(size_t)len)==0; }
static s32 W32(int64_t v){ return (s32)(uint32_t)(uint64_t)v; }
static void push(Vec *V, s32 x){ if(V->n==V->cap){ V->cap=V->cap?V->cap*2:16; V->v=realloc(V->v,sizeof(s32)*(size_t)V->cap); } V->v[V->n++]=x; }

static void get_imd(Buf *B, int64_t a, int64_t base, Vec *E){
    if(a>=0 && a+16<=B->n && MAGIC(B,a,"IMGD",4)) push(E,W32(I32(B,a+8)+base+0x20000000LL));
}
static void get_imz(Buf *B, int64_t a, int64_t base, Vec *E){
    if(MAGIC(B,a,"IMGZ",4)){ s32 cnt=I32(B,a+12); int64_t off=a+16;
        while(cnt>0 && !B->err){ cnt--; s32 imdoff=I32(B,off); get_imd(B,a+imdoff,base+imdoff,E); off+=8; } }
}
static void get_tm2(Buf *B, int64_t a, int64_t base, Vec *E){
    if(MAGIC(B,a,"TIM2",4)){ s32 cnt=I16(B,a+6); int64_t off=a+0x10; base+=0x10;
        while(cnt>0 && !B->err){ cnt--; s32 nxt=I32(B,off); push(E,W32(I16(B,off+12)+base+0x20000000LL)); off+=nxt; base+=nxt; } }   /* as Panacea: no check on nxt (a 0 size with cnt==1 is valid shipped data, e.g. NMC map/eh02.map 'rada'); the loop is bounded by cnt and every read is bounds-checked */
}
typedef struct { s32 texoff; s16 tbp0, shx, shy; int unique; } Tex;
static int tex_cmp(const void *x, const void *y){ const Tex *a=x,*b=y; return (int)a->tbp0-(int)b->tbp0; }
/* qsort is not stable; do a stable insertion sort by TBP0 (upstream uses std::sort: the order of equal TBP0 values is unspecified there) */
static void tex_sort(Tex *t, int n){ for(int i=1;i<n;i++){ Tex k=t[i]; int j=i-1; while(j>=0 && tex_cmp(&t[j],&k)>0){ t[j+1]=t[j]; j--; } t[j+1]=k; } }
static void get_dpd(Buf *B, int64_t a, int64_t base, Vec *E){
    int64_t p=a; if(I32(B,p)!=0x96) return; p+=4; p+=((int64_t)I32(B,p)+1)*4; s32 texcnt=I32(B,p); p+=4;
    if(B->err||texcnt<0||texcnt>4096){ B->err=1; return; }
    Tex *tex=calloc((size_t)texcnt+1,sizeof *tex);
    for(int t=0;t<texcnt;t++){ s32 texoff=I32(B,p); p+=4; int64_t tp=a+texoff; tex[t].texoff=texoff; tex[t].tbp0=I16(B,tp); tex[t].shx=I16(B,tp+8); tex[t].shy=I16(B,tp+10); }
    if(B->err){ free(tex); return; }
    for(int i=0;i<texcnt;i++) for(int j=i+1;j<texcnt;j++) if(tex[i].tbp0==tex[j].tbp0&&tex[i].shx==tex[j].shx&&tex[i].shy==tex[j].shy){ tex[i].unique=1; tex[j].unique=1; }
    tex_sort(tex,texcnt);
    for(int i=0;i<texcnt;i++){ if(!tex[i].unique) continue; for(int j=0;j<texcnt;j++) if(tex[j].tbp0==tex[i].tbp0) tex[j].unique=2; }   /* uniq set: TBP0 of any unique texture */
    for(int i=0;i<texcnt;i++) if(tex[i].unique==2) tex[i].unique=1;
    int64_t sharedk[4096]; int64_t sharedv[4096]; int ns=0;
    for(int i=0;i<texcnt;i++){ int64_t offset;
        if(!tex[i].unique){ int f=-1; for(int k=0;k<ns;k++) if(sharedk[k]==tex[i].tbp0){ f=k; break; }
            if(f>=0){ offset=sharedv[f]; if(E->n>0) E->v[E->n-1]=W32(offset+base+0x20000000LL); continue; }
            offset=tex[i].texoff+0x20; sharedk[ns]=tex[i].tbp0; sharedv[ns]=offset; ns++; }
        else offset=tex[i].texoff+0x20;
        push(E,W32(offset+base+0x20000000LL)); }
    free(tex);
}
static void get_dpx(Buf *B, int64_t a, int64_t base, Vec *E){
    int64_t p=a+(int64_t)I32(B,a)*0x20+4; s32 cnt=I32(B,p); p+=4;
    while(cnt>0 && !B->err){ cnt--; s32 dpdoff=I32(B,p)-0xC; p+=4; get_dpd(B,a+dpdoff,base+dpdoff,E); }
}
static void get_pax(Buf *B, int64_t a, int64_t base, Vec *E){
    if(MAGIC(B,a,"PAX_",4)){ s32 dpxoff=I32(B,a+12); get_dpx(B,a+dpxoff+12,base+dpxoff+12,E); }
}
static void get_raw(Buf *B, int64_t a, int64_t end, int64_t base, Vec *E){
    if(I32(B,a)!=0) return;
    s32 tic=I32(B,a+8), gsc=I32(B,a+12), offdata=I32(B,a+16), clut=I32(B,a+20), gsinfo=I32(B,a+24);
    if(B->err||tic<0||tic>4096||gsc<0||gsc>4096){ B->err=1; return; }
    s32 *pic=calloc((size_t)tic+1,4); for(int t=0;t<tic;t++) pic[t]=I32(B,a+clut+0x90+(int64_t)t*0x90+116);
    int mod=0; for(int m=0;m<gsc;m++){ s64 tex0=I64(B,a+gsinfo+0x70+(int64_t)m*0xA0); int psm=(int)((tex0>>20)&0x3f); if(psm!=20) mod++; }
    int p4=0,p8=0; s32 *basetex=calloc((size_t)gsc+1,4);
    for(int p=0;p<gsc;p++){ int64_t ko=a+offdata+p; if(ko<0||ko>=B->n){ B->err=1; break; } int key=(int8_t)B->b[ko];
        s64 tex0=I64(B,a+gsinfo+0x70+(int64_t)p*0xA0); int psm=(int)((tex0>>20)&0x3f);
        if(key<0||key>=tic){ B->err=1; break; }
        s32 fo=W32(base+pic[key]+0x20000000LL);
        if(psm==20){ fo+=p4+mod*0x10; p4++; } else { fo+=p8*0x10; p8++; }
        basetex[p]=fo; }
    if(!B->err){
        /* TEXA blocks: img -> list of entries, in file order */
        int ntx=0, ctx=0; struct { s32 img, val; } *tx=NULL;
        int64_t pos=a; while(pos<end && pos+4<=B->n){ const u8 *f=memmem(B->b+pos,(size_t)(end-pos<B->n-pos?end-pos:B->n-pos),"TEXA",4); if(!f) break; pos=f-B->b;
            s32 img=I16(B,pos+0x0A), to=I32(B,pos+0x28); if(ntx==ctx){ ctx=ctx?ctx*2:16; tx=realloc(tx,sizeof *tx*(size_t)ctx); } tx[ntx].img=img; tx[ntx].val=W32(base+(pos-a)+to+8+(int64_t)img*0x10+0x20000000LL); ntx++; pos+=4; }
        for(int i=0;i<gsc;i++){ push(E,basetex[i]); for(int k=0;k<ntx;k++) if(tx[k].img==i) push(E,tx[k].val); }
        free(tx); }
    free(pic); free(basetex);
}
static void get_bar(Buf *B, int64_t a, int64_t base, Vec *E, int depth){
    if(depth>8 || !MAGIC(B,a,"BAR\x01",4)) return;
    Vec tim={0},pax={0},tm2={0},gen={0},aud={0};
    s32 cnt=I32(B,a+4); int64_t off=a+16;
    while(cnt>0 && !B->err){ cnt--; s32 typ=I32(B,off), datoff=I32(B,off+8), sz=I32(B,off+12);
        switch(typ){
        case 7: get_raw(B,a+datoff,a+datoff+sz,base+datoff,&tim); break;
        case 10: get_tm2(B,a+datoff,base+datoff,&tm2); break;
        case 18: get_pax(B,a+datoff,base+datoff,&pax); break;
        case 24: get_imd(B,a+datoff,base+datoff,&gen); break;
        case 29: get_imz(B,a+datoff,base+datoff,&gen); break;
        case 31: case 34: if(MAGIC(B,a+datoff,"ORIGIN",6)) push(&aud,-1); break;
        case 36: push(&gen,W32(base+datoff+0x20000000LL)); break;
        case 46: get_bar(B,a+datoff,base+datoff,&gen,depth+1); break;
        default: break; }
        off+=16; }
    Vec *grp[5]={&tim,&pax,&tm2,&gen,&aud};
    for(int g=0;g<5;g++){ for(int i=0;i<grp[g]->n;i++) push(E,grp[g]->v[i]); free(grp[g]->v); }
}

/* file extension as Panacea sees it: last ".xyz" lower-cased; ".a.xx" language archives keep the ".a" prefix */
void kh_rm_ext(const char *path, char *out, size_t cap){
    const char *base=strrchr(path,'/'); base=base?base+1:path; const char *d=strrchr(base,'.'); out[0]=0; if(!d) return;
    size_t i=(size_t)(d-base); char e[32]; size_t n=0; for(const char *p=d;*p&&n<sizeof e-3;p++) e[n++]=(char)tolower((unsigned char)*p); e[n]=0;
    if(i>=2 && base[i-2]=='.' && tolower((unsigned char)base[i-1])=='a'){ snprintf(out,cap,".a%s",e); } else snprintf(out,cap,"%s",e);
}
static const char *BAR_EXTS[]={".2dd",".2ld",".a.fm",".a.fr",".a.gr",".a.it",".a.sp",".a.us",".a.uk",".a.jp",".bar",".bin",".mag",".map",".mdlx",0};
/* returns 1 if the extension is a known container type (offsets valid, possibly 0 of them), 0 if unknown (caller falls back to "no entries"); *err set on malformed data */
int kh_rm_offsets(const char *path, const unsigned char *bytes, int64_t n, int32_t **out, int *count, int *err){
    char ext[40]; kh_rm_ext(path,ext,sizeof ext); Buf B={bytes,n,0}; Vec E={0}; int known=1;
    if(!strcmp(ext,".imd")) get_imd(&B,0,0,&E);
    else if(!strcmp(ext,".imz")) get_imz(&B,0,0,&E);
    else if(!strcmp(ext,".tm2")) get_tm2(&B,0,0,&E);
    else if(!strcmp(ext,".pax")) get_pax(&B,0,0,&E);
    else if(!strcmp(ext,".dpd")) get_dpd(&B,0,0,&E);
    else { known=0; for(int i=0;BAR_EXTS[i];i++) if(!strcmp(ext,BAR_EXTS[i])){ known=1; break; } if(known) get_bar(&B,0,0,&E,0); }
    *out=E.v; *count=E.n; if(err) *err=B.err; return known;
}

/* Panacea sortRemasteredFiles comparator (strict weak order only when names follow the -N / N convention; reproduced as written) */
int kh_rm_less(const char *an, const char *bn){
    size_t la=strlen(an), lb=strlen(bn); const char *ea=la>=4?an+la-4:an, *eb=lb>=4?bn+lb-4:bn;
    const char *fna=an, *fnb=bn; if(*fna=='-') fna++; if(*fnb=='-') fnb++;
    if(!strcasecmp(ea,".dds")){ if(strcasecmp(eb,".dds")) return 1; if(isdigit((unsigned char)*fna)&&isdigit((unsigned char)*fnb)) return atoi(fna)<atoi(fnb); }
    else if(!strcasecmp(ea,".png")){ if(!strcasecmp(eb,".dds")) return 0; if(strcasecmp(eb,".png")) return 1; if(isdigit((unsigned char)*fna)&&isdigit((unsigned char)*fnb)) return atoi(fna)<atoi(fnb); }
    return 0;
}
