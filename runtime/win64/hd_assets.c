/* hd_assets.c : remastered (HD) asset protocol over the loose tree.
 *
 * Spec: docs/platform/01-FILE-LAYER.md 1.3.3 (algorithm of OpenKH Panacea ScanRemasteredFolder, Apache-2.0).  When a loader serves a loose file with
 * useHdAsset set, file_layer.c calls kh_hd_begin(); this module
 *   1. runs the container walker of runtime/remaster.c on the loaded bytes (ordered origOffset list),
 *   2. lists <tree>/remastered/<rel>/ recursively (breadth first, each directory in NTFS name order), sorts with the Panacea comparator
 *      (stable), truncates to the offset count and pairs entry i with offset i (file bytes are served as-is: .dds / .png / SCD),
 *   3. caches the resulting table per file, with a guest-memory array of 0x30-byte RemasteredEntry records.
 * The lifted resource manager (ResourceEntry 0x12cd80, runs inside SetResourceItem on the loading thread) then pulls the table through
 * GetRemasteredCount 0x106440, GetRemasteredEntry 0x106480 and the GetRemasteredAsset thunk 0x106460; those three are replaced here.
 * With no current table (real-package mode) they run the untouched lifted code.
 *
 * Deviations from Panacea, all deliberate and counted in the report: the package's own entry list does not exist, so when the folder holds
 * fewer files than the walker has offsets only the entries named "-<index>.dds" that exist on disk are served (the rest were package data);
 * malformed containers, names longer than 31 characters and files changed after the scan are named fatals, never fabricated bytes.
 *
 * Provenance (TASK18).  Panacea scans the MOD's remastered folder; the baked tree is the vanilla extraction with the mod laid over it,
 * so one folder can hold the vanilla HD set and the mod's.  Without knowing which file is whose the vanilla set wins wherever its
 * names sort first: the mod's P_EX100.mdlx (Roxas) got the vanilla "-N.dds" of Sora instead of the mod's "-N.png", and the mod's
 * re-laid-out field2d/us/zz0command.2dd got the vanilla command-menu sheet (both seen in rendered frames).  KHNMC_HD_MODLIST=<file>
 * names the mod's files (`mod <tree path>`) and the game files whose vanilla HD set must not be attached (`nohd <tree path>`); the
 * list is made by tools/hd_modlist.py.  With a list:
 *   mod-set       the folder holds mod HD files: only those are candidates (sorted and paired with the offsets as before).  If they
 *                 are fewer than the offsets, the rest comes from the package's own entries: for a VANILLA game file those are the
 *                 folder's vanilla files at the same positions; for a MOD game file they are not served (the package entries keep
 *                 the vanilla file's offsets, which this tree cannot know; the texture inside the mod's file is used);
 *   nohd          no HD entry at all (the mod moved or replaced the textures and ships no HD set: its embedded textures are used);
 *   restructured  a mod game file without mod HD files whose vanilla set has another count than the file has textures: none;
 *   assumed       a mod game file without mod HD files and a vanilla set of the right count: served as before, on the assumption
 *                 that the mod left the textures where they were (counted and listed in KHNMC_HDLOG: not proven);
 *   vanilla game files without mod HD files: unchanged.
 * Without a list nothing changes, and the report warns when files were dropped by the truncation rule. */
#define _GNU_SOURCE
#include "khnmc_hd.h"
#include "khnmc_remaster.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

void orig_f_106440(KCtx *g);
void orig_f_106460(KCtx *g);
void orig_f_106480(KCtx *g);

typedef struct { char name[32]; int32_t orig; int32_t size; char *host; } HdEnt;
struct HdTable {
    char *key; int64_t fsize; HdTable *next;
    int n; HdEnt *e; u64 gent;
    int dir_found, files_listed, offsets, missing, truncated, ext_known;
    const char *rule;                /* provenance rule that shaped the table (NULL = none / no list) */
    int ignored;                     /* vanilla files left out by it */
};

/* statistics (guarded by hd_lock where they are not atomic) */
static pthread_mutex_t hd_lock=PTHREAD_MUTEX_INITIALIZER;
static HdTable *buckets[1024];
static _Atomic u64 st_begin, st_cache_hit, st_built, st_nodir, st_withdir, st_count_calls, st_entry_calls, st_assets, st_asset_bytes, st_zero_tables,
                   st_entries_listed, st_missing, st_truncated, st_unknown_ext, st_orig_fallback, st_audio_slots;
static _Atomic u64 st_kind[8];
static const char *kind_names[8]={"DDS/DXT1","DDS/DXT3","DDS/DXT5","DDS/DX10","DDS/A8R8G8B8","DDS/other","PNG","other"};
static struct { u32 w,h; u64 n; } st_dims[48];
static int st_ndims;

static __thread HdTable *cur_table;

/* ---- provenance list --------------------------------------------------------------------------------------------- */
typedef struct Prov { char *path; int nohd; struct Prov *next; } Prov;
static Prov *prov_b[4096]; static int prov_state, prov_mods, prov_nohds; static char prov_file[1024];      /* prov_state: 0 = not looked at, 1 = no list, 2 = list loaded */
static _Atomic u64 st_p_modset, st_p_short, st_p_nohd, st_p_restruct, st_p_assumed, st_p_ignored, st_p_vanilla_fill;
static unsigned prov_hash(const char *k){ unsigned h=2166136261u; for(;*k;k++) h=(h^(unsigned char)tolower((unsigned char)*k))*16777619u; return h&4095; }
static void prov_clear(void){ for(int i=0;i<4096;i++){ Prov *e=prov_b[i]; while(e){ Prov *n=e->next; free(e->path); free(e); e=n; } prov_b[i]=NULL; } prov_mods=prov_nohds=0; prov_state=1; prov_file[0]=0; }
static int prov_add(const char *path,int nohd){
    while(*path=='/'||(path[0]=='.'&&path[1]=='/')) path+=path[0]=='/'?1:2;
    Prov *e=calloc(1,sizeof *e); if(!e||!(e->path=strdup(path))){ free(e); return 0; }
    e->nohd=nohd; unsigned b=prov_hash(e->path); e->next=prov_b[b]; prov_b[b]=e; if(nohd) prov_nohds++; else prov_mods++; prov_state=2; return 1;
}
static int prov_find(const char *path,int nohd){ for(Prov *e=prov_b[prov_hash(path)];e;e=e->next) if(e->nohd==nohd&&!strcasecmp(e->path,path)) return 1; return 0; }
/* caller holds hd_lock.  A list that cannot be read or parsed is a named fatal: a silently ignored list would give the wrong textures back. */
static void prov_load(KCtx *g){
    if(prov_state) return; prov_state=1;
    /* KHNMC_HD_MODLIST=<file> names the list; "none" switches it off.  Without the variable the baked tree's own list
     * ($KHNMC_DATA/khnmc-hd-modlist.txt, written next to the data by the bake step) is used when it exists: a baked tree always merges the
     * vanilla HD files with the mod's, and without the list the vanilla textures are served for the mod's models (seen: a red Sora outfit on
     * the mod's Roxas, the vanilla command-menu sheet on the mod's menu). */
    const char *p=getenv("KHNMC_HD_MODLIST"); static char dflt[1100];
    if(p&&!strcmp(p,"none")) return;
    if(!p||!*p){ const char *root=getenv("KHNMC_DATA"); if(!root||!*root) return;
        if(snprintf(dflt,sizeof dflt,"%s/khnmc-hd-modlist.txt",root)>=(int)sizeof dflt) return;
        if(access(dflt,R_OK)) return;
        p=dflt; }
    FILE *f=fopen(p,"r"); if(!f) kh_fatal(g,"hd-modlist: cannot open KHNMC_HD_MODLIST=%s (%s)",p,strerror(errno));
    snprintf(prov_file,sizeof prov_file,"%s",p);
    char line[4096]; unsigned ln=0;
    while(fgets(line,sizeof line,f)){ ln++;
        size_t n=strlen(line); if(n==sizeof line-1&&line[n-1]!='\n') kh_fatal(g,"hd-modlist: %s line %u is too long",p,ln);
        while(n&&(line[n-1]=='\n'||line[n-1]=='\r'||line[n-1]==' '||line[n-1]=='\t')) line[--n]=0;
        char *s=line; while(*s==' '||*s=='\t') s++;
        if(!*s||*s=='#') continue;
        int nohd; if(!strncmp(s,"mod ",4)){ nohd=0; s+=4; } else if(!strncmp(s,"nohd ",5)){ nohd=1; s+=5; } else kh_fatal(g,"hd-modlist: %s line %u: expected `mod <path>` or `nohd <path>`, got: %s",p,ln,s);
        while(*s==' ') s++; char *c=strstr(s,"  #"); if(c){ *c=0; n=strlen(s); while(n&&s[n-1]==' ') s[--n]=0; }
        if(!*s) kh_fatal(g,"hd-modlist: %s line %u has no path",p,ln);
        if(!prov_add(s,nohd)) kh_fatal(g,"hd-modlist: out of memory");
    }
    if(ferror(f)) kh_fatal(g,"hd-modlist: read error on %s",p);
    fclose(f);
    if(prov_state!=2) kh_fatal(g,"hd-modlist: %s holds no `mod` or `nohd` line",p);
    fprintf(stderr,"kh: hd: provenance list %s: %d mod files, %d nohd rules\n",p,prov_mods,prov_nohds);
}

/* ---- host side: scan ----------------------------------------------------------------------------------------------- */
typedef struct { char name[256]; char *host; int64_t size; } HdFile;
typedef struct { char *host; char *rel; } HdDir;

/* NTFS lists a directory in upper-case code-unit order; for the ASCII names of the tree that is a byte compare of the upper-cased name. */
static int ntfs_cmp(const void *x,const void *y){
    const unsigned char *a=*(char *const *)x,*b=*(char *const *)y;
    for(;;a++,b++){ int ca=toupper(*a),cb=toupper(*b); if(ca!=cb) return ca<cb?-1:1; if(!ca) return 0; }
}
static void free_files(HdFile *f,int n){ for(int i=0;i<n;i++) free(f[i].host); free(f); }

/* breadth-first listing of `top` (host path); returns -1 and sets err on failure */
static int scan_files(const char *top,HdFile **out,int *count,char *err,size_t cap){
    HdDir *q=NULL; int qn=0,qcap=0,qi=0; HdFile *fl=NULL; int fn=0,fcap=0; int rc=-1;
    q=malloc(sizeof *q*16); qcap=16; if(!q){ snprintf(err,cap,"out of memory"); return -1; }
    q[qn].host=strdup(top); q[qn].rel=strdup(""); qn++;
    while(qi<qn){
        HdDir cur=q[qi++]; DIR *d=opendir(cur.host);
        if(!d){ snprintf(err,cap,"cannot open directory %s: %s",cur.host,strerror(errno)); goto done; }
        char **names=NULL; int nn=0,ncap=0; struct dirent *de;
        while((de=readdir(d))){
            if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
            if(nn==ncap){ ncap=ncap?ncap*2:32; names=realloc(names,sizeof *names*(size_t)ncap); if(!names){ closedir(d); snprintf(err,cap,"out of memory"); goto done; } }
            names[nn++]=strdup(de->d_name);
        }
        closedir(d);
        qsort(names,(size_t)nn,sizeof *names,ntfs_cmp);
        for(int i=0;i<nn;i++){
            char path[4096],rel[1024]; struct stat st;
            if(snprintf(path,sizeof path,"%s/%s",cur.host,names[i])>=(int)sizeof path||
               snprintf(rel,sizeof rel,"%s%s%s",cur.rel,*cur.rel?"/":"",names[i])>=(int)sizeof rel){ snprintf(err,cap,"path too long below %s",cur.host); goto done; }
            if(stat(path,&st)){ snprintf(err,cap,"cannot stat %s: %s",path,strerror(errno)); goto done; }
            if(S_ISDIR(st.st_mode)){
                if(qn==qcap){ qcap*=2; q=realloc(q,sizeof *q*(size_t)qcap); if(!q){ snprintf(err,cap,"out of memory"); goto done; } }
                q[qn].host=strdup(path); q[qn].rel=strdup(rel); qn++;
            } else if(S_ISREG(st.st_mode)){
                if(fn==fcap){ fcap=fcap?fcap*2:32; fl=realloc(fl,sizeof *fl*(size_t)fcap); if(!fl){ snprintf(err,cap,"out of memory"); goto done; } }
                if(strlen(rel)>=sizeof fl[fn].name){ snprintf(err,cap,"remastered file name too long: %s",rel); goto done; }
                if(st.st_size>INT_MAX){ snprintf(err,cap,"remastered file too large (%lld bytes): %s",(long long)st.st_size,path); goto done; }
                strcpy(fl[fn].name,rel); fl[fn].host=strdup(path); fl[fn].size=st.st_size; fn++;
            }
        }
        for(int i=0;i<nn;i++) free(names[i]); free(names);
    }
    rc=0;
done:
    for(int i=0;i<qn;i++){ free(q[i].host); free(q[i].rel); } free(q);
    if(rc){ free_files(fl,fn); *out=NULL; *count=0; } else { *out=fl; *count=fn; }
    return rc;
}

static void sort_files(HdFile *f,int n){          /* stable, same comparator as Panacea sortRemasteredFiles */
    for(int i=1;i<n;i++){ HdFile k=f[i]; int j=i; while(j>0&&kh_rm_less(k.name,f[j-1].name)){ f[j]=f[j-1]; j--; } f[j]=k; }
}

static void table_free(HdTable *t){ if(!t) return; for(int i=0;i<t->n;i++) free(t->e[i].host); free(t->e); free(t->key); free(t); }

/* Builds the table for `rel`. Returns 0, or -1 with a message in err. `root` is the tree; the folder is <root>/remastered/<rel> (case-insensitive). */
static int hd_build(const char *root,const char *rel,const unsigned char *bytes,int64_t n,HdTable **out,char *err,size_t cap){
    HdTable *t=calloc(1,sizeof *t); if(!t){ snprintf(err,cap,"out of memory"); return -1; }
    char sub[2200],host[4300]; struct stat st;
    if(snprintf(sub,sizeof sub,"remastered/%s",rel)>=(int)sizeof sub||strlen(root)+strlen(sub)+2>=sizeof host){ snprintf(err,cap,"path too long: %s",rel); table_free(t); return -1; }
    if(!kh_resolve_ci(root,sub,host,sizeof host)||stat(host,&st)||!S_ISDIR(st.st_mode)){ *out=t; return 0; }   /* no remastered folder: zero entries */
    t->dir_found=1;
    int32_t *offs=NULL; int noffs=0,werr=0;
    t->ext_known=kh_rm_offsets(rel,bytes,n,&offs,&noffs,&werr);
    if(werr){ snprintf(err,cap,"malformed container: the walker for %s read outside the %lld loaded bytes",rel,(long long)n); free(offs); table_free(t); return -1; }
    t->offsets=noffs;
    HdFile *files=NULL; int nf=0;
    if(scan_files(host,&files,&nf,err,cap)){ free(offs); table_free(t); return -1; }
    t->files_listed=nf;
    sort_files(files,nf);
    int rc=-1;
    if(prov_state==2){
        int fmod=prov_find(rel,0), nm=0; char *ism=calloc((size_t)(nf?nf:1),1); if(!ism){ snprintf(err,cap,"out of memory"); goto fail; }
        for(int i=0;i<nf;i++){ char full[2600]; snprintf(full,sizeof full,"remastered/%s/%s",rel,files[i].name); ism[i]=(char)prov_find(full,0); nm+=ism[i]; }
        if(prov_find(rel,1)){ t->rule="nohd"; t->ignored=nf; t->e=calloc(1,sizeof *t->e); free(ism); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; } rc=0; goto fail; }
        if(nm>0||fmod){
            if(nm==0&&nf!=noffs){ t->rule="restructured"; t->ignored=nf; t->e=calloc(1,sizeof *t->e); free(ism); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; } rc=0; goto fail; }
            if(nm==0){ t->rule="assumed"; free(ism); }                          /* falls through to the unchanged pairing below */
            else {
                t->rule=nm>=noffs?"mod-set":(fmod?"mod-set-short":"mod-set+vanilla"); t->e=calloc((size_t)(noffs?noffs:1),sizeof *t->e); if(!t->e){ free(ism); snprintf(err,cap,"out of memory"); goto fail; }
                int k=0, v=0;                                                  /* k = position among the mod files, v = among the vanilla files */
                for(int pass=0;pass<2;pass++) for(int i=0;i<nf;i++){
                    int slot=-1;
                    if(pass==0&&ism[i]){ if(k<noffs) slot=k; else t->truncated++; k++; }
                    if(pass==1&&!ism[i]){ if(!fmod&&v>=nm&&v<noffs) slot=v; else t->ignored++; v++; }
                    if(slot<0) continue;
                    if(strlen(files[i].name)>=sizeof t->e[0].name){ free(ism); snprintf(err,cap,"remastered entry name longer than 31 characters: %s (folder of %s)",files[i].name,rel); goto fail; }
                    HdEnt *e=&t->e[t->n++]; strcpy(e->name,files[i].name); e->orig=offs[slot]; e->size=(int32_t)files[i].size; e->host=files[i].host; files[i].host=NULL;
                }
                if(t->n<noffs) t->missing=noffs-t->n;
                free(ism); rc=0; goto fail;
            }
        } else free(ism);
    }
    if(nf>=noffs){
        t->truncated=nf-noffs; t->e=calloc((size_t)(noffs?noffs:1),sizeof *t->e); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        for(int i=0;i<noffs;i++){
            if(strlen(files[i].name)>=sizeof t->e[i].name){ snprintf(err,cap,"remastered entry name longer than 31 characters: %s (folder of %s)",files[i].name,rel); goto fail; }
            strcpy(t->e[i].name,files[i].name); t->e[i].orig=offs[i]; t->e[i].size=(int32_t)files[i].size; t->e[i].host=files[i].host; files[i].host=NULL; t->n++;
        }
    } else {
        /* fewer files than offsets: the package supplied the rest; only "-<index>.dds" names found on disk can be served from the tree */
        t->e=calloc((size_t)(noffs?noffs:1),sizeof *t->e); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        for(int i=0;i<noffs;i++){
            char nm[32]; snprintf(nm,sizeof nm,"-%d.dds",i); int hit=-1;
            for(int k=0;k<nf;k++) if(!strcmp(files[k].name,nm)){ hit=k; break; }
            if(hit<0){ t->missing++; continue; }
            strcpy(t->e[t->n].name,nm); t->e[t->n].orig=offs[i]; t->e[t->n].size=(int32_t)files[hit].size; t->e[t->n].host=files[hit].host; files[hit].host=NULL; t->n++;
        }
    }
    rc=0;
fail:
    free_files(files,nf); free(offs);
    if(rc){ table_free(t); return -1; }
    *out=t; return 0;
}
static void prov_count(const HdTable *t){
    if(!t->rule) return;
    if(!strcmp(t->rule,"nohd")) st_p_nohd++; else if(!strcmp(t->rule,"restructured")) st_p_restruct++; else if(!strcmp(t->rule,"assumed")) st_p_assumed++;
    else if(!strcmp(t->rule,"mod-set")) st_p_modset++; else if(!strcmp(t->rule,"mod-set-short")) st_p_short++; else st_p_vanilla_fill++;
    st_p_ignored+=(u64)t->ignored;
}

/* ---- guest side -------------------------------------------------------------------------------------------------- */
static unsigned bucket_of(const char *k){ unsigned h=2166136261u; for(;*k;k++) h=(h^(unsigned char)tolower((unsigned char)*k))*16777619u; return h&1023; }
static int key_eq(const char *a,const char *b){ return !strcasecmp(a,b); }

HdTable *kh_hd_begin(KCtx *g,const char *root,const char *rel,const unsigned char *bytes,int64_t n){
    char err[512]; HdTable *prev=cur_table; st_begin++;
    char key[2300]; snprintf(key,sizeof key,"%s",rel);
    pthread_mutex_lock(&hd_lock);
    unsigned b=bucket_of(key); HdTable *t;
    for(t=buckets[b];t;t=t->next) if(t->fsize==n&&key_eq(t->key,key)) break;
    if(t){ st_cache_hit++; pthread_mutex_unlock(&hd_lock); cur_table=t; return prev; }
    prov_load(g);
    if(hd_build(root,rel,bytes,n,&t,err,sizeof err)){ pthread_mutex_unlock(&hd_lock); kh_fatal(g,"hd assets: %s",err); }
    t->key=strdup(key); t->fsize=n; if(!t->key) kh_fatal(g,"hd assets: out of memory");
    t->gent=ga_calloc((u64)(t->n?t->n:1),0x30); if(!t->gent) kh_fatal(g,"hd assets: guest arena exhausted for %d entries of %s",t->n,rel);
    for(int i=0;i<t->n;i++){ u64 e=t->gent+0x30ull*(u64)i;
        memcpy(hp(g,e),t->e[i].name,32); wr32(g,e+0x24,(u32)t->e[i].orig); wr32(g,e+0x28,(u32)t->e[i].size); wr32(g,e+0x2c,(u32)-2); }
    t->next=buckets[b]; buckets[b]=t;
    st_built++; if(t->dir_found){ st_withdir++; st_entries_listed+=(u64)t->n; st_missing+=(u64)t->missing; st_truncated+=(u64)t->truncated;
        for(int i=0;i<t->n;i++) if(t->e[i].orig==-1) st_audio_slots++; } else st_nodir++;
    if(!t->n) st_zero_tables++;
    if(t->dir_found&&!t->ext_known) st_unknown_ext++;
    prov_count(t);
    pthread_mutex_unlock(&hd_lock);
    if(t->rule&&strcmp(t->rule,"mod-set")) fprintf(stderr,"kh: hd: %s: provenance rule %s: %d of %d folder files served for %d textures (%d vanilla files left out)\n",rel,t->rule,t->n,t->files_listed,t->offsets,t->ignored);
    if(t->dir_found&&!t->ext_known) fprintf(stderr,"kh: hd: %s has a remastered folder (%d files) but its extension has no container walker: 0 entries served\n",rel,t->files_listed);
    if(t->dir_found&&t->missing) fprintf(stderr,"kh: hd: %s: folder has %d files for %d walker offsets; %d entries missing on disk (originally package data), %d served\n",rel,t->files_listed,t->offsets,t->missing,t->n);
    const char *lg=getenv("KHNMC_HDLOG");
    if(lg){ FILE *f=fopen(lg,"a"); if(!f) kh_fatal(g,"hd assets: cannot append %s",lg);
        int bad=fprintf(f,"%s dir=%d offsets=%d files=%d served=%d missing=%d truncated=%d rule=%s ignored-vanilla=%d\n",rel,t->dir_found,t->offsets,t->files_listed,t->n,t->missing,t->truncated,t->rule?t->rule:"-",t->ignored)<0;
        if(fclose(f)||bad) kh_fatal(g,"hd assets: cannot write %s",lg); }
    cur_table=t; return prev;
}
void kh_hd_end(HdTable *previous){ cur_table=previous; }

/* GetRemasteredCount(): i64 */
void f_106440(KCtx *g){
    if(!cur_table){ st_orig_fallback++; orig_f_106440(g); return; }
    st_count_calls++; SET_RAX(g,(u32)cur_table->n); KH_RET(g);
}
/* GetRemasteredEntry(CFileMan*, int *origOffset, int index): RemasteredEntry* or NULL */
void f_106480(KCtx *g){
    if(!cur_table){ st_orig_fallback++; orig_f_106480(g); return; }
    HdTable *t=cur_table; s32 i=(s32)A2(g); st_entry_calls++;
    if(i<0) kh_fatal(g,"hd assets: GetRemasteredEntry called with negative index %d",i);
    if(!A1(g)) kh_fatal(g,"hd assets: GetRemasteredEntry called without an output pointer (index %d of %s)",i,t->key);
    if(i>=t->n){ SET_RAX(g,0); KH_RET(g); return; }
    wr32(g,A1(g),(u32)t->e[i].orig); SET_RAX(g,t->gent+0x30ull*(u64)i); KH_RET(g);
}

static void note_asset(const unsigned char *b,u32 size){
    int k=7; u32 w=0,h=0;
    if(size>=4&&!memcmp(b,"DDS ",4)&&size>=128){
        u32 pff,bits; memcpy(&h,b+12,4); memcpy(&w,b+16,4); memcpy(&pff,b+80,4); memcpy(&bits,b+88,4);
        if(pff&4){ if(!memcmp(b+84,"DXT1",4)) k=0; else if(!memcmp(b+84,"DXT3",4)) k=1; else if(!memcmp(b+84,"DXT5",4)) k=2; else if(!memcmp(b+84,"DX10",4)) k=3; else k=5; }
        else if((pff&0x41)&&bits==32) k=4; else k=5;
    } else if(size>=8&&!memcmp(b,"\x89PNG",4)) k=6;
    st_kind[k]++;
    if(k<=5){ pthread_mutex_lock(&hd_lock); int i; for(i=0;i<st_ndims;i++) if(st_dims[i].w==w&&st_dims[i].h==h) break;
        if(i==st_ndims&&st_ndims<48){ st_dims[i].w=w; st_dims[i].h=h; st_ndims++; } if(i<48) st_dims[i].n++; pthread_mutex_unlock(&hd_lock); }
}

/* GetRemasteredAsset thunk 0x106460 (CFileMan*, uint *outSize, int index): 16-aligned zero-padded block (rounded to 256) holding the file, or NULL for a bad index */
void f_106460(KCtx *g){
    if(!cur_table){ st_orig_fallback++; orig_f_106460(g); return; }
    HdTable *t=cur_table; s32 i=(s32)A2(g); u64 sizeptr=A1(g);
    if(i<0) kh_fatal(g,"hd assets: GetRemasteredAsset called with negative index %d",i);
    if(!sizeptr) kh_fatal(g,"hd assets: GetRemasteredAsset called without a size pointer (index %d of %s)",i,t->key);
    if(i>=t->n){ SET_RAX(g,0); KH_RET(g); return; }
    HdEnt *e=&t->e[i];
    if(e->size<=0) kh_fatal(g,"hd assets: %s entry %d (%s) is an empty file",t->key,i,e->name);
    FILE *f=fopen(e->host,"rb");
    if(!f) kh_fatal(g,"hd assets: cannot open %s (entry %d \"%s\" of %s): %s",e->host,i,e->name,t->key,strerror(errno));
    struct stat st;
    if(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||st.st_size!=e->size){ fclose(f); kh_fatal(g,"hd assets: %s changed after the scan (scanned %d bytes, now %lld)",e->host,e->size,(long long)st.st_size); }
    u64 alloc=((u64)e->size+0xff)&~0xffull, buf=ga_alloc_src(alloc,16,GA_SRC_FILE);
    if(!buf){ fclose(f); kh_fatal(g,"hd assets: guest arena exhausted for a %llu-byte asset (%s)",(unsigned long long)alloc,e->host); }
    memset(hp(g,buf),0,alloc);
    if(fread(hp(g,buf),1,(size_t)e->size,f)!=(size_t)e->size){ fclose(f); ga_free(buf); kh_fatal(g,"hd assets: short read of %s",e->host); }
    fclose(f);
    wr32(g,sizeptr,(u32)e->size);
    st_assets++; st_asset_bytes+=(u64)e->size; note_asset(hp(g,buf),(u32)e->size);
    SET_RAX(g,buf); KH_RET(g);
}

void kh_hd_report(FILE *f){
    pthread_mutex_lock(&hd_lock);
    fprintf(f,"kh: hd: loads-with-HD=%llu built=%llu cache-hits=%llu folders-found=%llu folders-absent=%llu zero-entry-tables=%llu entries-listed=%llu (audio-slots=%llu) assets-served=%llu (%llu bytes) "
              "truncated-extra-files=%llu missing-package-entries=%llu unknown-ext=%llu count-calls=%llu entry-calls=%llu lifted-fallback=%llu\n",
        (unsigned long long)st_begin,(unsigned long long)st_built,(unsigned long long)st_cache_hit,(unsigned long long)st_withdir,(unsigned long long)st_nodir,(unsigned long long)st_zero_tables,
        (unsigned long long)st_entries_listed,(unsigned long long)st_audio_slots,(unsigned long long)st_assets,(unsigned long long)st_asset_bytes,(unsigned long long)st_truncated,
        (unsigned long long)st_missing,(unsigned long long)st_unknown_ext,(unsigned long long)st_count_calls,(unsigned long long)st_entry_calls,(unsigned long long)st_orig_fallback);
    if(prov_state==2) fprintf(f,"kh: hd: provenance list %s (%d mod files, %d nohd rules): tables shaped by it: mod-set=%llu mod-set+vanilla=%llu mod-set-short=%llu nohd=%llu restructured=%llu assumed-unmoved=%llu; vanilla files left out=%llu\n",prov_file,prov_mods,prov_nohds,
        (unsigned long long)st_p_modset,(unsigned long long)st_p_vanilla_fill,(unsigned long long)st_p_short,(unsigned long long)st_p_nohd,(unsigned long long)st_p_restruct,(unsigned long long)st_p_assumed,(unsigned long long)st_p_ignored);
    else if(st_truncated) fprintf(f,"kh: hd: WARNING: %llu folder files were dropped by the truncation rule and KHNMC_HD_MODLIST is not set: in a tree that merges the vanilla HD files with a mod's, the dropped files can be the mod's own textures (tools/hd_modlist.py)\n",(unsigned long long)st_truncated);
    fprintf(f,"kh: hd: served asset kinds:");
    for(int k=0;k<8;k++) fprintf(f," %s=%llu",kind_names[k],(unsigned long long)st_kind[k]);
    fprintf(f,"\nkh: hd: served DDS dimensions:");
    for(int i=0;i<st_ndims;i++) fprintf(f," %ux%u=%llu",st_dims[i].w,st_dims[i].h,(unsigned long long)st_dims[i].n);
    fprintf(f,"\n");
    pthread_mutex_unlock(&hd_lock);
}

/* ---- selftest ---------------------------------------------------------------------------------------------------- */
static int rm_cb(const char *p,const struct stat *s,int fl,struct FTW *w){ (void)s;(void)fl;(void)w; return remove(p); }
static int put_file(const char *root,const char *rel,const void *data,size_t n){
    char path[1024],dir[1024]; snprintf(path,sizeof path,"%s/%s",root,rel); snprintf(dir,sizeof dir,"%s",path);
    for(char *p=dir+strlen(root)+1;*p;p++) if(*p=='/'){ *p=0; mkdir(dir,0700); *p='/'; }
    FILE *f=fopen(path,"wb"); if(!f) return 0; int ok=fwrite(data,1,n,f)==n; return fclose(f)==0&&ok;
}
/* BAR of `nimd` type-24 IMD entries followed by `naudio` type-31 "ORIGIN" entries. IMD i sits at 0x100+0x40*i with pixel offset 0x20+i, so origOffset = 0x20000000+0x100+0x40*i+0x20+i. */
static int make_bar(unsigned char *buf,int nimd,int naudio){
    int cnt=nimd+naudio; memset(buf,0,0x1000); memcpy(buf,"BAR\x01",4); int32_t c=cnt; memcpy(buf+4,&c,4);
    for(int i=0;i<cnt;i++){ unsigned char *e=buf+16+16*i; int32_t typ=i<nimd?24:31,off=0x100+0x40*i,sz=0x40; memcpy(e,&typ,4); memcpy(e+8,&off,4); memcpy(e+12,&sz,4);
        unsigned char *d=buf+off; if(i<nimd){ memcpy(d,"IMGD",4); int32_t po=0x20+i; memcpy(d+8,&po,4); memset(d+12,0,4); } else memcpy(d,"ORIGIN",6); }
    return 0x100+0x40*cnt;
}
int kh_hd_selftest(KCtx *g){
    char dir[]="/tmp/khnmc-hd-XXXXXX"; int bad=0,checks=0;
#define TEST(c) do { checks++; if(!(c)){ fprintf(stderr,"hd selftest FAIL (line %d): %s\n",__LINE__,#c); bad++; } } while(0)
    if(!mkdtemp(dir)){ fprintf(stderr,"hd selftest: mkdtemp failed\n"); return 1; }
    unsigned char bar[0x1000]; char err[512]; HdTable *t; int sz;
    /* --- ordering: dds by atoi (leading '-' ignored), png after dds, truncation to the offset count --- */
    sz=make_bar(bar,3,0); (void)sz;
    const char *a[]={"-10.dds","-2.dds","b.png","-1.dds","-0.dds","-3.png","zz.bin"};
    for(unsigned i=0;i<sizeof a/sizeof *a;i++){ char rel[128]; snprintf(rel,sizeof rel,"remastered/obj/T1.bar/%s",a[i]); char body[16]; snprintf(body,sizeof body,"D%u",i); TEST(put_file(dir,rel,body,strlen(body))); }
    TEST(!hd_build(dir,"obj/T1.bar",bar,0x1000,&t,err,sizeof err)&&t);
    TEST(t->dir_found&&t->ext_known&&t->offsets==3&&t->files_listed==7&&t->n==3&&t->truncated==4&&t->missing==0);
    TEST(!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[1].name,"-1.dds")&&!strcmp(t->e[2].name,"-2.dds"));
    for(int i=0;i<3;i++) TEST((uint32_t)t->e[i].orig==0x20000000u+0x100u+0x40u*(unsigned)i+0x20u+(unsigned)i);   /* HD link flag + container offset */
    TEST(t->e[0].size==2&&t->e[2].size==2);
    table_free(t);
    /* --- case-insensitive rel and folder, nested audio slots, orig -1 --- */
    make_bar(bar,1,1);
    TEST(put_file(dir,"remastered/obj/F_X.a.us/se/obj/se200","SCD0",4)&&put_file(dir,"remastered/obj/F_X.a.us/-0.dds","DDS0",4));
    TEST(!hd_build(dir,"obj/f_x.A.US",bar,0x1000,&t,err,sizeof err)&&t);
    TEST(t->dir_found&&t->n==2&&!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[1].name,"se/obj/se200")&&t->e[1].orig==-1&&t->e[0].orig!=-1&&((uint32_t)t->e[0].orig>>24)==0x20);
    table_free(t);
    /* --- missing folder: zero entries, no error --- */
    TEST(!hd_build(dir,"obj/NOPE.bar",bar,0x1000,&t,err,sizeof err)&&t&&!t->dir_found&&t->n==0); table_free(t);
    TEST(!hd_build(dir,"nodir/at/all.bar",bar,0x1000,&t,err,sizeof err)&&t&&!t->dir_found&&t->n==0); table_free(t);
    /* --- fewer files than offsets: only the "-<i>.dds" entries on disk (index = walker offset index), the rest counted missing --- */
    make_bar(bar,3,0);
    TEST(put_file(dir,"remastered/obj/few.bar/-1.dds","XX",2));
    TEST(!hd_build(dir,"obj/few.bar",bar,0x1000,&t,err,sizeof err)&&t&&t->dir_found&&t->n==1&&t->missing==2&&!strcmp(t->e[0].name,"-1.dds")&&(uint32_t)t->e[0].orig==0x20000000u+0x140u+0x20u+1u);
    table_free(t);
    /* --- empty folder: all missing --- */
    { char p[1024]; snprintf(p,sizeof p,"%s/remastered/obj/empty.bar",dir); mkdir(p,0700); }
    TEST(!hd_build(dir,"obj/empty.bar",bar,0x1000,&t,err,sizeof err)&&t&&t->dir_found&&t->n==0&&t->missing==3); table_free(t);
    /* --- unknown extension: folder present, no walker: zero entries --- */
    TEST(put_file(dir,"remastered/obj/x.dat/-0.dds","XX",2));
    TEST(!hd_build(dir,"obj/x.dat",bar,0x1000,&t,err,sizeof err)&&t&&t->dir_found&&!t->ext_known&&t->n==0); table_free(t);
    /* --- breadth first: top-level files before nested ones, per-directory NTFS (upper-case) order --- */
    make_bar(bar,0,0); { unsigned char b2[0x1000]; int n3=make_bar(b2,4,0); (void)n3;
    TEST(put_file(dir,"remastered/obj/bfs.bar/sub/a","1",1)&&put_file(dir,"remastered/obj/bfs.bar/z","2",1)&&put_file(dir,"remastered/obj/bfs.bar/B","3",1)&&put_file(dir,"remastered/obj/bfs.bar/_u","4",1));
    TEST(!hd_build(dir,"obj/bfs.bar",b2,0x1000,&t,err,sizeof err)&&t&&t->n==4&&!strcmp(t->e[0].name,"B")&&!strcmp(t->e[1].name,"z")&&!strcmp(t->e[2].name,"_u")&&!strcmp(t->e[3].name,"sub/a")); table_free(t); }
    /* --- entry name over 31 characters among the served entries is an error --- */
    make_bar(bar,1,0);
    TEST(put_file(dir,"remastered/obj/long.bar/0123456789012345678901234567890123456789.dds","L",1));
    TEST(hd_build(dir,"obj/long.bar",bar,0x1000,&t,err,sizeof err)==-1&&strstr(err,"longer than 31"));
    /* --- malformed container (IMZ count points outside the buffer) is an error, not garbage --- */
    { unsigned char z[32]; memset(z,0,sizeof z); memcpy(z,"IMGZ",4); int32_t cnt=1000; memcpy(z+12,&cnt,4);
      TEST(put_file(dir,"remastered/obj/bad.imz/-0.dds","B",1)); TEST(hd_build(dir,"obj/bad.imz",z,sizeof z,&t,err,sizeof err)==-1&&strstr(err,"malformed")); }
    /* --- guest-visible protocol through the hooks (dispatch table), including cache, thread-local state and the lifted fallback --- */
    make_bar(bar,3,0);
    u64 gb=ga_alloc(0x1000,16); memcpy(hp(g,gb),bar,0x1000);
    u64 oldcount=rd32(g,GS_BASE+0x79cfd4),oldpkg=rd64(g,GS_BASE+0x79cfe0);
    u64 fake=ga_calloc(1,0x260); wr32(g,GS_BASE+0x79cfd4,0); wr64(g,GS_BASE+0x79cfe0,fake);
    TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==0);                                    /* no current table: the lifted code reads the (empty) package */
    u64 scratch=ga_alloc(16,16); TEST(kh_call(g,GS_BASE+0x106480,0,scratch,0,0)==0); ga_free(scratch);
    HdTable *prev=kh_hd_begin(g,dir,"obj/T1.bar",hp(g,gb),0x1000); TEST(prev==NULL);
    HdTable *t1=cur_table; u64 outp=ga_alloc(16,16),sizep=outp+8; wr32(g,outp,0xdeadbeef);
    TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==3);
    u64 e1=kh_call(g,GS_BASE+0x106480,0,outp,1,0);
    TEST(e1&&!strcmp((char*)hp(g,e1),"-1.dds")&&rd32(g,outp)==0x20000000u+0x100u+0x40u+0x20u+1u&&(s32)rd32(g,e1+0x2c)==-2&&rd32(g,e1+0x28)==2);
    TEST(kh_call(g,GS_BASE+0x106480,0,outp,3,0)==0&&kh_call(g,GS_BASE+0x106460,0,sizep,3,0)==0);   /* index past the end: NULL */
    wr32(g,sizep,0); u64 as=kh_call(g,GS_BASE+0x106460,0,sizep,1,0);
    TEST(as&&!(as&15)&&rd32(g,sizep)==2&&!memcmp(hp(g,as),"D3",2)&&rd8(g,as+2)==0&&rd8(g,as+255)==0&&ga_usable(as)>=256);   /* "D3": file -1.dds is entry index 3 of the written list */
    ga_free(as);
    u64 hits=st_cache_hit; HdTable *p2=kh_hd_begin(g,dir,"OBJ/t1.bar",hp(g,gb),0x1000);          /* same file, other case: served from the cache */
    TEST(p2==t1&&cur_table==t1&&st_cache_hit==hits+1);
    kh_hd_end(p2); TEST(cur_table==t1); kh_hd_end(prev); TEST(cur_table==NULL);
    TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==0);                                    /* state is thread-local and restored */
    wr32(g,GS_BASE+0x79cfd4,(u32)oldcount); wr64(g,GS_BASE+0x79cfe0,oldpkg); ga_free(fake); ga_free(gb); ga_free(outp);
    /* --- provenance (KHNMC_HD_MODLIST): a folder that holds the vanilla HD set and the mod's --- */
    if(prov_state!=2){
        make_bar(bar,3,0);
#define ORIG(i) (0x20000000u+0x100u+0x40u*(unsigned)(i)+0x20u+(unsigned)(i))
        for(int i=0;i<3;i++){ char rel[128],body[8]; snprintf(rel,sizeof rel,"remastered/obj/PV.mdlx/-%d.dds",i); snprintf(body,sizeof body,"V%d",i); TEST(put_file(dir,rel,body,2));
                              snprintf(rel,sizeof rel,"remastered/obj/PV.mdlx/-%d.png",i); snprintf(body,sizeof body,"M%d",i); TEST(put_file(dir,rel,body,2)); }
        /* no list: the vanilla "-N.dds" sort first and win; the mod's three PNG are the "truncated extra files" (the defect) */
        TEST(!hd_build(dir,"obj/PV.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->n==3&&t->truncated==3&&!t->rule&&!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[2].name,"-2.dds")); table_free(t);
        /* list: the game file and its three PNG are the mod's: exactly those are served, in order, on the three texture offsets */
        prov_clear(); TEST(prov_add("obj/PV.mdlx",0)&&prov_add("remastered/obj/PV.mdlx/-0.png",0)&&prov_add("./remastered/OBJ/pv.MDLX/-1.png",0)&&prov_add("/remastered/obj/PV.mdlx/-2.png",0));
        TEST(prov_state==2&&prov_mods==4&&prov_find("OBJ/pv.mdlx",0)&&!prov_find("obj/PV.mdlx",1)&&!prov_find("remastered/obj/PV.mdlx/-0.dds",0));
        TEST(!hd_build(dir,"obj/PV.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->n==3&&t->truncated==0&&t->missing==0&&t->ignored==3&&t->rule&&!strcmp(t->rule,"mod-set"));
        if(t&&t->n==3){ for(int i=0;i<3;i++){ char nm[16]; snprintf(nm,sizeof nm,"-%d.png",i); TEST(!strcmp(t->e[i].name,nm)&&(uint32_t)t->e[i].orig==ORIG(i)&&strstr(t->e[i].host,nm)); } } table_free(t);
        /* the same through the guest protocol: the asset bytes are the mod's file */
        { u64 gb2=ga_alloc(0x1000,16); memcpy(hp(g,gb2),bar,0x1000); u64 o2=ga_alloc(16,16); HdTable *pv=kh_hd_begin(g,dir,"obj/PV.mdlx",hp(g,gb2),0x1000);
          TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==3); u64 e0=kh_call(g,GS_BASE+0x106480,0,o2,2,0); TEST(e0&&!strcmp((char*)hp(g,e0),"-2.png")&&rd32(g,o2)==ORIG(2));
          u64 as2=kh_call(g,GS_BASE+0x106460,0,o2+8,2,0); TEST(as2&&rd32(g,o2+8)==2&&!memcmp(hp(g,as2),"M2",2)); ga_free(as2); kh_hd_end(pv); ga_free(gb2); ga_free(o2); }
        /* more mod files than textures: the first ones, the extra mod file is truncated (Panacea's rule on the mod's own folder) */
        { unsigned char b2[0x1000]; make_bar(b2,2,0); TEST(!hd_build(dir,"obj/PV.mdlx",b2,0x1000,&t,err,sizeof err)&&t&&t->n==2&&t->truncated==1&&!strcmp(t->e[1].name,"-1.png")&&!strcmp(t->rule,"mod-set")); table_free(t); }
        /* a mod game file with fewer mod HD files than textures: those two, the third texture keeps the image inside the file (no vanilla "-2.dds") */
        prov_clear(); TEST(prov_add("obj/PV.mdlx",0)&&prov_add("remastered/obj/PV.mdlx/-0.png",0)&&prov_add("remastered/obj/PV.mdlx/-1.png",0));
        TEST(!hd_build(dir,"obj/PV.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->n==2&&t->missing==1&&t->ignored==4&&!strcmp(t->rule,"mod-set-short")&&!strcmp(t->e[0].name,"-0.png")&&!strcmp(t->e[1].name,"-1.png")&&(uint32_t)t->e[1].orig==ORIG(1)); table_free(t);
        /* a VANILLA game file with one mod HD file (a texture-only mod): the mod file on texture 0, the package's own entries 1 and 2 behind it */
        prov_clear(); TEST(prov_add("remastered/obj/PV.mdlx/-0.png",0));
        TEST(!hd_build(dir,"obj/PV.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->n==3&&t->missing==0&&!strcmp(t->rule,"mod-set+vanilla"));
        if(t&&t->n==3){ TEST(!strcmp(t->e[0].name,"-0.png")&&(uint32_t)t->e[0].orig==ORIG(0)&&!strcmp(t->e[1].name,"-1.dds")&&(uint32_t)t->e[1].orig==ORIG(1)&&!strcmp(t->e[2].name,"-2.dds")&&(uint32_t)t->e[2].orig==ORIG(2)&&t->ignored==3); } table_free(t);
        /* nohd: nothing is served, the folder is still seen */
        prov_clear(); TEST(prov_add("obj/PV.mdlx",0)&&prov_add("obj/pv.mdlx",1)); TEST(prov_nohds==1&&prov_find("obj/PV.mdlx",1));
        TEST(!hd_build(dir,"obj/PV.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->dir_found&&t->n==0&&t->ignored==6&&!strcmp(t->rule,"nohd")); table_free(t);
        /* a mod game file without mod HD files: a vanilla set of another count is not attached (restructured), one of the right count is (assumed) */
        for(int i=0;i<5;i++){ char rel[128]; snprintf(rel,sizeof rel,"remastered/obj/RS.mdlx/-%d.dds",i); TEST(put_file(dir,rel,"VV",2)); }
        prov_clear(); TEST(prov_add("obj/RS.mdlx",0));
        TEST(!hd_build(dir,"obj/RS.mdlx",bar,0x1000,&t,err,sizeof err)&&t&&t->n==0&&t->ignored==5&&!strcmp(t->rule,"restructured")); table_free(t);
        { unsigned char b5[0x1000]; make_bar(b5,5,0); TEST(!hd_build(dir,"obj/RS.mdlx",b5,0x1000,&t,err,sizeof err)&&t&&t->n==5&&!strcmp(t->rule,"assumed")&&!strcmp(t->e[4].name,"-4.dds")&&(uint32_t)t->e[4].orig==ORIG(4)); table_free(t); }
        /* a vanilla game file without mod HD files: no rule, the old pairing (here with the truncation it always had) */
        TEST(!hd_build(dir,"obj/T1.bar",bar,0x1000,&t,err,sizeof err)&&t&&t->n==3&&!t->rule&&!strcmp(t->e[0].name,"-0.dds")); table_free(t);
        /* the list file: comments, both directives, a trailing comment; a wrong line or an unreadable file is a named fatal */
        { char lp[1100]; snprintf(lp,sizeof lp,"%s/modlist.txt",dir); const char *good="# made by a test\n\nmod obj/PV.mdlx\nmod remastered/obj/PV.mdlx/-0.png  # the mod's\n  nohd field2d/us/zz0command.2dd  # moved\n";
          TEST(put_file(dir,"modlist.txt",good,strlen(good)));
          pid_t pid=fork(); if(pid==0){ setenv("KHNMC_HD_MODLIST",lp,1); prov_clear(); prov_state=0; if(!freopen("/dev/null","w",stderr)){} prov_load(NULL);
              _exit(prov_state==2&&prov_mods==2&&prov_nohds==1&&prov_find("remastered/obj/PV.mdlx/-0.png",0)&&prov_find("FIELD2D/us/zz0command.2dd",1)&&!prov_find("field2d/us/zz0command.2dd",0)?0:1); }
          int stt=1; TEST(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)==0);
          TEST(put_file(dir,"bad.txt","mod obj/A.mdlx\nvanilla obj/B.mdlx\n",34)); snprintf(lp,sizeof lp,"%s/bad.txt",dir);
          pid=fork(); if(pid==0){ setenv("KHNMC_HD_MODLIST",lp,1); prov_clear(); prov_state=0; if(!freopen("/dev/null","w",stderr)){} prov_load(NULL); _exit(0); }
          stt=0; TEST(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)==3);
          snprintf(lp,sizeof lp,"%s/does-not-exist.txt",dir);
          pid=fork(); if(pid==0){ setenv("KHNMC_HD_MODLIST",lp,1); prov_clear(); prov_state=0; if(!freopen("/dev/null","w",stderr)){} prov_load(NULL); _exit(0); }
          stt=0; TEST(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)==3);
          TEST(put_file(dir,"empty.txt","# nothing\n",10)); snprintf(lp,sizeof lp,"%s/empty.txt",dir);
          pid=fork(); if(pid==0){ setenv("KHNMC_HD_MODLIST",lp,1); prov_clear(); prov_state=0; if(!freopen("/dev/null","w",stderr)){} prov_load(NULL); _exit(0); }
          stt=0; TEST(pid>0&&waitpid(pid,&stt,0)==pid&&WIFEXITED(stt)&&WEXITSTATUS(stt)==3); }
#undef ORIG
        prov_clear(); prov_state=0;                                                       /* the run's own list (if any) is read at the first real load */
        memset(buckets,0,sizeof buckets);                                                 /* drop the selftest's cached tables (they point into the temporary tree) */
        st_p_modset=st_p_short=st_p_nohd=st_p_restruct=st_p_assumed=st_p_ignored=st_p_vanilla_fill=0;
    } else fprintf(stderr,"kh: hd selftest: provenance checks SKIPPED (a list is already loaded)\n");
    nftw(dir,rm_cb,16,FTW_DEPTH|FTW_PHYS);
    fprintf(stderr,"kh: hd selftest: %d checks, %d failures\n",checks,bad); return bad;
}
