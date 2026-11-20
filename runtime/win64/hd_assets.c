/* hd_assets.c : remastered (HD) asset protocol over the loose tree, paired exactly as the owner's PC pairs it (TASK22).
 *
 * When a loader serves a loose file with useHdAsset set, file_layer.c calls kh_hd_begin(); the lifted resource manager (ResourceEntry
 * constructor 0x12cd80, inside SetResourceItem on the loading thread) then pulls the table through GetRemasteredCount 0x106440,
 * GetRemasteredEntry 0x106480 and the GetRemasteredAsset thunk 0x106460, which are replaced here.  With no current table (real-package
 * mode) the three run the untouched lifted code.
 *
 * WHAT THE PC DOES (OpenKH Panacea, Apache-2.0, OpenKh.Research.Panacea/Panacea.cpp; the owner's panacea_settings.txt has mod_path only,
 * no extract_path, no dev_path).  Panacea's fake "ModFiles" package is registered by its SetReplacePath hook, which axaAppMain calls
 * AFTER the real packages were added (0x14f3b5 / 0x14f455 before 0x14f479), and GetFileInfo 0x106940 takes the first package that has
 * the file: for a file that is in a real package `fileinfo` is that package, with its own remastered table P.
 *
 *   G = the game file is the mod's                        Panacea: TransformFilePath finds mod_path\<rel>     here: `mod <rel>`
 *   D = the mod has a remastered folder for it            Panacea: DirectoryExists(mod_path\remastered\<rel>)  here: >=1 `mod remastered/<rel>/...`
 *   P = the package's own table of <rel> (name, origOffset WITH its flag bits, decompressedSize; empty when no package has the file)
 *
 *   G and D  -> ScanRemasteredFolder (Panacea.cpp 809) over the mod's files F of that folder, listed breadth first in NTFS name order:
 *                 offsets = container walk of the LOADED bytes (runtime/remaster.c; unknown extension: the origOffsets of P)
 *       scan        |F| >= |offsets|: F stable-sorted with sortRemasteredFiles, cut to |offsets|, origOffset[i] = offsets[i]
 *       scan-short  |F| <  |offsets|: P cut or padded (empty entries named "-<index>.dds") to |offsets|, origOffset[i] = offsets[i];
 *                   the asset of entry i is the mod's file of that name when the folder has one (as NTFS finds it: case-insensitive),
 *                   else the package's i-th asset, else (padding, no file) NULL: the PC's package returns NULL for an index past its
 *                   table, and ResourceEntry then gives the whole load up (0x12cf9c -> 0x12d502, SetResourceItem returns -1).
 *   otherwise -> P verbatim: order, names, origOffsets with their flag bits.  The walker is not used at all.
 *       package             the game file is the package's and the mod has no folder for it
 *       package+mod-names   the game file is the package's but the mod HAS a folder (not G, D): Panacea never scans it (GetRemasteredFiles
 *                           is only reached when the game file itself came from the mod, Panacea.cpp 1077..1100), but its
 *                           GetRemasteredAsset hook (1412) looks for mod_path\remastered\<rel>\<entry name> for EVERY entry, so a mod
 *                           file is served exactly where its name equals a package entry's name; other mod files are never used.
 *       package(mod file)   the game file is the mod's without a mod folder (G, not D): the package's entries keep the vanilla file's
 *                           origOffsets; where the mod moved an image the game simply finds no entry for it and draws the image
 *                           inside the mod's file.
 *       none                no table, nothing scanned: zero entries.
 *
 * The baked tree is the vanilla extraction with the mod laid over it, so "the mod's file" needs the provenance list
 * (KHNMC_HD_MODLIST, default <tree>/khnmc-hd-modlist.txt, `mod <tree path>` per mod file; tools/hd_modlist.py) and "P" needs the package
 * tables (KHNMC_HD_PACKAGES, default <tree>/khnmc-hd-packages.tsv; tools/hd_packages.py).  `nohd` lines of older lists are obsolete and
 * ignored (said at start-up): the package tables decide.
 *
 * Sizes.  A mod file is served whole (Panacea: ftell).  A package asset is the tree's vanilla file <tree>/remastered/<rel>/<name>; the
 * extraction wrote those padded to 16 bytes (0xCD filler), so exactly `decompressedSize` bytes of it are served and reported, as the
 * package does.  A vanilla file that is neither that size nor that size rounded up to 16 is not the package's asset: named fatal.
 *
 * What the game does with an entry (read from the exe, RVAs): ResourceEntry 0x12cd80 stores origOffset, flag bits included, in a table
 * of 256 dwords at +0x21c indexed by the NUMBER in the entry's name (the digits before the last '.'), and makes a node carrying that
 * number; a texture at file offset X is matched by scanning the table for (value & 0x1fffffff) == X up to the first entry whose low 29
 * bits are zero (0x12c530, 0x12ac8c).  Bits 29..30 are a per-texture kind: 0x12c4e0 returns (value >> 29) & 3 for the texture at an
 * offset and 0x12a4c0 stores it in the texture object (+0xa8; objects with the magic 0x2f02eb5).  Its readers: 0x1272bd (kind 3 sets
 * the device state +0x298, which gives a shader constant the factor 255/128 when the state +0x29c is set: 0x112835..) and the GS draw
 * emulation, which copies it into the global 0xad7750 (0x1ba7b3, 0x1be8ec, 0x1bedec) and takes its own draw sequences for kind 2 and
 * for kind 3 (0x1ba36e / 0x1ba395, 0x1bb427 / 0x1bb449).  No reader tells kind 0 from kind 1.  The packages use all four (top nibble
 * 0x0, 0x2, 0x4, 0x6); Panacea's walkers always produce 0x20000000 = kind 1, so in a SCANNED folder a texture the package marks as
 * kind 2 or 3 is drawn as kind 1 on the PC too.  That is reproduced, and `khnmc --hd-check` counts the entries concerned.
 *
 * Never fabricated: a package entry without its file, a provenance list that disagrees with the tree, a malformed container in a scanned
 * folder, a mod file name longer than 31 characters in a scanned folder (Panacea keeps names in 32 bytes), a file that changed after the
 * table was built and an empty file are named fatals.  A tree with a remastered folder but without package tables is a named fatal
 * unless KHNMC_HD_PACKAGES=none says so (then P is empty for every file, said at start-up); loaded tables without a provenance list are
 * a named fatal unless KHNMC_HD_MODLIST=none.
 *
 * `khnmc --hd-check [report.tsv]` builds the table of every game file of the tree with this code (no game run) and prints the totals. */
#define _GNU_SOURCE
#include "khnmc_hd.h"
#include "rmc.h"
#include "khnmc_remaster.h"
#include "kh_look.h"
#include "xion_swap.h"
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

enum { R_NONE, R_PKG, R_PKG_MODNAMES, R_PKG_MODGAME, R_SCAN, R_SCAN_SHORT, R_COUNT };
static const char *rule_names[R_COUNT]={"none","package","package+mod-names","package(mod-file)","scan","scan-short"};
enum { SRC_NULL, SRC_MOD, SRC_VANILLA };

typedef struct { char name[32]; uint32_t orig;
                 int32_t size;                    /* bytes served and reported through the size pointer */
                 int32_t gsize;                   /* decompressedSize field of the guest-visible entry (the game does not read it) */
                 int64_t fsize;                   /* size of the host file when the table was built */
                 char *host;                      /* NULL: no asset (scan-short padding without a file) */
                 KhRmcFile *rmc;
                 unsigned char src; } HdEnt;
struct HdTable {
    char *key; int64_t fsize; HdTable *next;
    int n; HdEnt *e; u64 gent;
    int hooded, look, rule, game_mod, mod_files, pkg_n;        /* mod_files: the mod's files in the folder (provenance); pkg_n: entries of the package table */
    int offsets;                                 /* walker offsets of a scanned folder, -1 when nothing was walked */
    int unknown_ext;                             /* scanned folder of a file type without a walker: the package's origOffsets were used */
    int truncated;                               /* scan: mod files beyond the offset count */
    int unserved;                                /* mod files of the folder that no entry serves (truncated ones included) */
    int n_mod, n_van, n_null, n_trim, n_modsize; /* entries by source; vanilla entries cut to the table size; mod-by-name entries of another size */
};

/* statistics (guarded by hd_lock where they are not atomic) */
static pthread_mutex_t hd_lock=PTHREAD_MUTEX_INITIALIZER;
static HdTable *buckets[1024];
static _Atomic u64 st_begin, st_cache_hit, st_built, st_count_calls, st_entry_calls, st_assets, st_asset_bytes, st_null_assets, st_orig_fallback,
                   st_entries_listed, st_orig_minus1, st_truncated, st_unserved, st_unknown_ext, st_trim, st_modsize;
static _Atomic u64 st_rule[R_COUNT], st_src[3];
static _Atomic u64 st_kind[8];
static const char *kind_names[8]={"DDS/DXT1","DDS/DXT3","DDS/DXT5","DDS/DX10","DDS/A8R8G8B8","DDS/other","PNG","other"};
static struct { u32 w,h; u64 n; } st_dims[48];
static int st_ndims;

static __thread HdTable *cur_table;

static unsigned ci_hash(const char *k){ unsigned h=2166136261u; for(;*k;k++) h=(h^(unsigned char)tolower((unsigned char)*k))*16777619u; return h; }
/* "a//b/./c" -> "a/b/c": the keys of both lists are plain tree paths */
static int canon_rel(const char *in,char *out,size_t cap){
    size_t o=0;
    for(const char *p=in;*p;){
        while(*p=='/') p++; if(!*p) break; const char *e=p; while(*e&&*e!='/') e++; size_t l=(size_t)(e-p);
        if(!(l==1&&p[0]=='.')){ if(o+l+2>cap) return 0; if(o) out[o++]='/'; memcpy(out+o,p,l); o+=l; }
        p=e;
    }
    if(!cap) return 0; out[o]=0; return 1;
}

int kh_look_key(const char *rel){
    int scene=kh_scene_pack_active(), xion=kh_look_enabled();
    if(!scene && !xion) return 0;
    char key[2300];
    if(!canon_rel(rel,key,sizeof key)) return 0;
    if(scene && kh_scene_key(key)){
        if(xion) return !strcasecmp(key,KH_SCENE_MDLX)?-1:0; /* X5 uses embedded textures. */
        return KH_SCENE_MODEL;
    }
    if(!xion) return 0;
    if(kh_player_voice(key)) return -1;
    if(kh_xion_enabled){
        static const char *const story[]={"obj/P_EX110.mdlx","obj/P_EX110_NPC_PAJAMAS.mdlx",
                                           "obj/H_EX510.mdlx","obj/H_EX510_PAJAMAS.mdlx"};
        for(size_t i=0;i<sizeof story/sizeof *story;i++)
            if(!strcasecmp(key,story[i])) return -1;
    }
    const char *base=!strncasecmp(key,"remastered/",11)?key+11:key;
    if(!strncasecmp(base,"menu/shopface/",14)){
        static const char *const shops[]={"menu/shopface/p_ex100.bin","menu/shopface/p_ex101.bin","menu/shopface/p_ex100_wi.bin"};
        for(size_t i=0;i<sizeof shops/sizeof *shops;i++){
            char dds[80],host[4096];
            snprintf(dds,sizeof dds,"remastered/%s/-0.dds",shops[i]);
            if(!strcasecmp(key,shops[i]) || !strcasecmp(key,dds))
                return kh_look_available(shops[i],host,sizeof host) && kh_look_available(dds,host,sizeof host);
        }
        return 0;
    }
    return kh_player_asset_key(key);
}

static const struct { const char *stem; int face; } keys[]={
        {"P_EX100",36},{"P_EX100_BTLF",47},{"P_EX100_MAGF",38},{"P_EX100_KH1F",47},
        {"P_EX100_TRIF",5},{"P_EX100_ULTF",41},{"P_EX100_HTLF",3},{"P_EX101",36},
        {"P_EX101_BTLF",47},{"P_EX110_BTLF",0},{"P_EX121",36},{"P_EX100_WI",36},
        {"P_EX100_WI_MAGF",38},{"P_EX100_WI_KH1F",39},{"P_EX100_WI_ULTF",41}
    };
int kh_look_pack_ready(int look){
    char rel[128],host[4096];
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++){
        snprintf(rel,sizeof rel,"obj/%s.mdlx",keys[i].stem);
        if(!kh_look_asset(look,rel,host,sizeof host)) return 0;
        snprintf(rel,sizeof rel,"obj/%s.a.us",keys[i].stem);
        if(!kh_look_asset(look,rel,host,sizeof host)) return 0;
        snprintf(rel,sizeof rel,"remastered/obj/%s.a.us/-%d.dds",keys[i].stem,keys[i].face);
        if(!kh_look_asset(look,rel,host,sizeof host)) return 0;
    }
    return 1;
}
int kh_player_asset_key(const char *key){
    int hd=!strncasecmp(key,"remastered/",11);
    const char *obj=key+(hd?11:0);
    if(strncasecmp(obj,"obj/",4)) return 0;
    obj+=4;
    for(size_t i=0;i<sizeof keys/sizeof *keys;i++){
        size_t n=strlen(keys[i].stem);
        if(strncasecmp(obj,keys[i].stem,n)||obj[n]!='.') continue;
        if(!hd && !strcasecmp(obj+n,".mdlx")) return -1;
        char suffix[32]=".a.us";
        if(hd) snprintf(suffix,sizeof suffix,".a.us/-%d.dds",keys[i].face);
        if(!strcasecmp(obj+n,suffix)) return keys[i].face+1;
    }
    return 0;
}

/* ---- provenance list: the mod's files -------------------------------------------------------------------------------- */
typedef struct Prov { char *path; int n; struct Prov *next; } Prov;
static Prov *prov_b[4096];                      /* every mod file */
static Prov *pdir_b[4096];                      /* every directory below remastered/ that holds mod files (at any depth): n = how many */
static int prov_state, prov_mods, prov_hd, prov_nohd_ignored, prov_none_explicit;    /* prov_state: 0 = not looked at, 1 = no list, 2 = list loaded */
static char prov_file[1024];
static Prov *prov_get(Prov **tab,const char *path){ for(Prov *e=tab[ci_hash(path)&4095];e;e=e->next) if(!strcasecmp(e->path,path)) return e; return NULL; }
static void prov_free(Prov **tab){ for(int i=0;i<4096;i++){ Prov *e=tab[i]; while(e){ Prov *n=e->next; free(e->path); free(e); e=n; } tab[i]=NULL; } }
static void prov_clear(void){ prov_free(prov_b); prov_free(pdir_b); prov_mods=prov_hd=prov_nohd_ignored=prov_none_explicit=0; prov_state=1; prov_file[0]=0; }
static Prov *prov_put(Prov **tab,const char *path){
    Prov *e=calloc(1,sizeof *e); if(!e||!(e->path=strdup(path))){ free(e); return NULL; }
    unsigned b=ci_hash(path)&4095; e->next=tab[b]; tab[b]=e; return e;
}
static int prov_add(const char *path){
    char c[2600]; if(!canon_rel(path,c,sizeof c)||!*c) return 0;
    if(prov_get(prov_b,c)){ prov_state=2; return 1; }                                   /* a repeated line must not count twice */
    if(!prov_put(prov_b,c)) return 0;
    prov_mods++; prov_state=2;
    if(!strncasecmp(c,"remastered/",11)){
        prov_hd++;
        for(size_t i=11;c[i];i++) if(c[i]=='/'){ c[i]=0; Prov *d=prov_get(pdir_b,c); if(!d) d=prov_put(pdir_b,c); c[i]='/'; if(!d) return 0; d->n++; }
    }
    return 1;
}
static int prov_find(const char *path){ return prov_get(prov_b,path)!=NULL; }
/* how many mod files the provenance list names below remastered/<rel>/ (0 = the mod has no folder for that game file) */
static int prov_dir_files(const char *rel){ char d[2600]; if(snprintf(d,sizeof d,"remastered/%s",rel)>=(int)sizeof d) return 0; Prov *e=prov_get(pdir_b,d); return e?e->n:0; }
/* Reads the list.  Returns 0, or -1 with a message. */
static int prov_load_file(const char *p,char *err,size_t cap){
    FILE *f=fopen(p,"r"); if(!f){ snprintf(err,cap,"cannot open %s (%s)",p,strerror(errno)); return -1; }
    char line[4096]; unsigned ln=0; int rc=-1;
    while(fgets(line,sizeof line,f)){ ln++;
        size_t n=strlen(line); if(n==sizeof line-1&&line[n-1]!='\n'){ snprintf(err,cap,"%s line %u is too long",p,ln); goto done; }
        while(n&&(line[n-1]=='\n'||line[n-1]=='\r'||line[n-1]==' '||line[n-1]=='\t')) line[--n]=0;
        char *s=line; while(*s==' '||*s=='\t') s++;
        if(!*s||*s=='#') continue;
        int nohd; if(!strncmp(s,"mod ",4)){ nohd=0; s+=4; } else if(!strncmp(s,"nohd ",5)){ nohd=1; s+=5; } else { snprintf(err,cap,"%s line %u: expected `mod <path>`, got: %s",p,ln,s); goto done; }
        while(*s==' ') s++; char *c=strstr(s,"  #"); if(c){ *c=0; n=strlen(s); while(n&&s[n-1]==' ') s[--n]=0; }
        if(!*s){ snprintf(err,cap,"%s line %u has no path",p,ln); goto done; }
        if(nohd){ prov_nohd_ignored++; continue; }                                      /* obsolete (TASK22): the package tables decide */
        if(!prov_add(s)){ snprintf(err,cap,"%s line %u: cannot store the path (too long, empty or out of memory)",p,ln); goto done; }
    }
    if(ferror(f)){ snprintf(err,cap,"read error on %s",p); goto done; }
    if(prov_state!=2){ snprintf(err,cap,"%s holds no `mod` line",p); goto done; }
    snprintf(prov_file,sizeof prov_file,"%s",p); rc=0;
done:
    fclose(f); return rc;
}

/* ---- package tables: the entries the packages hold for each game file ------------------------------------------------ */
typedef struct { char name[32]; uint32_t orig; int32_t size; } PkgEnt;
typedef struct PkgTab { char *path; int n; PkgEnt *e; struct PkgTab *next; } PkgTab;
static PkgTab *pkg_b[8192];
static int pkg_state, pkg_files; static long pkg_entries; static char pkg_file[1024];  /* pkg_state: 0 = not looked at, 1 = off (KHNMC_HD_PACKAGES=none), 2 = loaded, 3 = none and the tree has no remastered folder */
static void pkg_clear(void){
    for(int i=0;i<8192;i++){ PkgTab *t=pkg_b[i]; while(t){ PkgTab *n=t->next; free(t->path); free(t->e); free(t); t=n; } pkg_b[i]=NULL; }
    pkg_files=0; pkg_entries=0; pkg_state=1; pkg_file[0]=0;
}
static const PkgTab *pkg_find(const char *path){ for(PkgTab *t=pkg_b[ci_hash(path)&8191];t;t=t->next) if(!strcasecmp(t->path,path)) return t; return NULL; }
/* One line: path <TAB> count <TAB> name:origOffset(hex):decompressedSize ...   Returns 0, or -1 with a message.  The line is modified. */
static int pkg_parse_line(char *line,char *err,size_t cap){
    char *path=line,*tab=strchr(line,'\t'); if(!tab){ snprintf(err,cap,"no tab after the path"); return -1; }
    *tab=0; char *cnt=tab+1,*rest=strchr(cnt,'\t'); if(rest) *rest++=0;
    char c[2600]; if(!canon_rel(path,c,sizeof c)||!*c){ snprintf(err,cap,"empty or over-long path"); return -1; }
    char *end; errno=0; long want=strtol(cnt,&end,10);
    if(end==cnt||*end||errno||want<1||want>65536){ snprintf(err,cap,"bad entry count \"%s\" (a line needs at least one entry)",cnt); return -1; }
    if(pkg_find(c)){ snprintf(err,cap,"second table for %s",c); return -1; }
    PkgEnt *e=calloc((size_t)want,sizeof *e); if(!e){ snprintf(err,cap,"out of memory"); return -1; }
    long n=0;
    for(char *p=rest;p&&*p;){
        char *nx=strchr(p,'\t'); if(nx) *nx++=0;
        char *c2=strrchr(p,':'); if(!c2){ snprintf(err,cap,"entry \"%s\" is not name:origOffset:size",p); goto bad; }
        *c2=0; char *c1=strrchr(p,':'); if(!c1){ snprintf(err,cap,"entry \"%s\" is not name:origOffset:size",p); goto bad; }
        *c1=0; const char *hx=c1+1,*sz=c2+1; size_t nl=strlen(p),hl=strlen(hx);
        if(n>=want){ snprintf(err,cap,"more entries than the count %ld says",want); goto bad; }
        if(!nl||nl>31){ snprintf(err,cap,"entry name \"%s\" is empty or longer than 31 characters",p); goto bad; }
        if(!hl||hl>8||strspn(hx,"0123456789abcdefABCDEF")!=hl){ snprintf(err,cap,"origOffset \"%s\" of entry \"%s\" is not 1..8 hex digits",hx,p); goto bad; }
        errno=0; long v=strtol(sz,&end,10);
        if(end==sz||*end||errno||v<0||v>INT_MAX||strspn(sz,"0123456789")!=strlen(sz)){ snprintf(err,cap,"size \"%s\" of entry \"%s\" is not a byte count",sz,p); goto bad; }
        strcpy(e[n].name,p); e[n].orig=(uint32_t)strtoul(hx,NULL,16); e[n].size=(int32_t)v; n++;
        p=nx;
    }
    if(n!=want){ snprintf(err,cap,"%ld entries, the count says %ld",n,want); goto bad; }
    PkgTab *t=calloc(1,sizeof *t); if(!t||!(t->path=strdup(c))){ free(t); snprintf(err,cap,"out of memory"); goto bad; }
    t->n=(int)n; t->e=e; unsigned b=ci_hash(c)&8191; t->next=pkg_b[b]; pkg_b[b]=t; pkg_files++; pkg_entries+=n; return 0;
bad:
    free(e); return -1;
}
static int pkg_load_file(const char *p,char *err,size_t cap){
    FILE *f=fopen(p,"r"); if(!f){ snprintf(err,cap,"cannot open %s (%s)",p,strerror(errno)); return -1; }
    char *line=NULL; size_t lcap=0; ssize_t len; unsigned ln=0; int rc=-1; char e2[400];
    while((len=getline(&line,&lcap,f))>=0){ ln++;
        while(len&&(line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if((size_t)len!=strlen(line)){ snprintf(err,cap,"%s line %u holds a NUL byte",p,ln); goto done; }
        if(!len||line[0]=='#') continue;
        if(pkg_parse_line(line,e2,sizeof e2)){ snprintf(err,cap,"%s line %u: %s",p,ln,e2); goto done; }
    }
    if(ferror(f)){ snprintf(err,cap,"read error on %s",p); goto done; }
    if(!pkg_files){ snprintf(err,cap,"%s holds no table",p); goto done; }
    snprintf(pkg_file,sizeof pkg_file,"%s",p); pkg_state=2; rc=0;
done:
    free(line); fclose(f); return rc;
}

/* Both lists, once, at the first load with HD assets (caller holds hd_lock).  Every way of getting this wrong is a named fatal or a
 * loud line: a silently ignored list gives the wrong textures back (seen: a red Sora outfit on the mod's Roxas). */
static int cfg_loaded;
static void cfg_load(KCtx *g,const char *root){
    if(cfg_loaded) return; cfg_loaded=1;
    char err[1500],dflt[2100]; struct stat st;
    /* provenance */
    prov_clear();
    const char *p=getenv("KHNMC_HD_MODLIST");
    if(p&&!strcmp(p,"none")) prov_none_explicit=1;
    else {
        if(!p||!*p){ if(!kh_rmc_path("khnmc-hd-modlist.txt",dflt,sizeof dflt) && snprintf(dflt,sizeof dflt,"%s/khnmc-hd-modlist.txt",root)>=(int)sizeof dflt) kh_fatal(g,"hd-modlist: tree path too long: %s",root); p=access(dflt,R_OK)?NULL:dflt; }
        if(p){ if(prov_load_file(p,err,sizeof err)) kh_fatal(g,"hd-modlist: %s",err);
            fprintf(stderr,"kh: hd: provenance list %s: %d mod files (%d of them remastered)\n",p,prov_mods,prov_hd);
            if(prov_nohd_ignored) fprintf(stderr,"kh: hd: provenance list %s: %d `nohd` lines IGNORED (obsolete: the package tables decide which entries a file has)\n",p,prov_nohd_ignored); }
    }
    /* package tables */
    pkg_clear();
    p=getenv("KHNMC_HD_PACKAGES");
    if(p&&!strcmp(p,"none")){
        pkg_state=1;
        fprintf(stderr,"kh: hd: package tables OFF (KHNMC_HD_PACKAGES=none): only folders scanned from the mod get HD entries, every other file gets NONE\n");
    } else {
        if(!p||!*p){ if(!kh_rmc_path("khnmc-hd-packages.tsv",dflt,sizeof dflt) && snprintf(dflt,sizeof dflt,"%s/khnmc-hd-packages.tsv",root)>=(int)sizeof dflt) kh_fatal(g,"hd-packages: tree path too long: %s",root); p=access(dflt,R_OK)?NULL:dflt; }
        if(p){ if(pkg_load_file(p,err,sizeof err)) kh_fatal(g,"hd-packages: %s",err);
            fprintf(stderr,"kh: hd: package tables %s: %d game files, %ld entries\n",p,pkg_files,pkg_entries); }
        else {
            char rm[2100]; snprintf(rm,sizeof rm,"%s/remastered",root);
            if(!stat(rm,&st)&&S_ISDIR(st.st_mode))
                kh_fatal(g,"hd-packages: the tree %s has a remastered folder but no package tables (%s/khnmc-hd-packages.tsv is missing): make them with tools/hd_packages.py and "
                           "install them there or name them with KHNMC_HD_PACKAGES=<file>; KHNMC_HD_PACKAGES=none runs without them (then only folders scanned from the mod get HD entries)",root,root);
            pkg_state=3;
            fprintf(stderr,"kh: hd: no package tables and no remastered folder in %s: no HD entries\n",root);
        }
    }
    if(prov_state!=2){
        if(pkg_state==2&&!prov_none_explicit)
            kh_fatal(g,"hd-modlist: package tables are loaded but the tree %s has no provenance list (%s/khnmc-hd-modlist.txt is missing): without it the mod's files cannot be told from the "
                       "package's and no mod folder would be scanned.  Make it with tools/hd_modlist.py, name it with KHNMC_HD_MODLIST=<file>, or say KHNMC_HD_MODLIST=none for a tree without a mod",root,root);
        fprintf(stderr,"kh: hd: NO provenance list%s: every file counts as the package's own, no mod folder is scanned\n",prov_none_explicit?" (KHNMC_HD_MODLIST=none)":"");
    }
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
/* readdir that tells the end of a directory (NULL, errno 0) from a failed read (NULL, errno set: the listing is incomplete).
 * hd_readdir is readdir; only the selftest replaces it, to make a read fail part-way. */
static struct dirent *(*hd_readdir)(DIR *)=readdir;
static struct dirent *next_ent(DIR *d){ errno=0; return hd_readdir(d); }

/* breadth-first listing of `top` (host path), as Panacea's ScanFolder walks it; returns -1 and sets err on failure */
static int scan_files(const char *top,HdFile **out,int *count,char *err,size_t cap){
    HdDir *q=NULL; int qn=0,qcap=0,qi=0; HdFile *fl=NULL; int fn=0,fcap=0; int rc=-1;
    q=malloc(sizeof *q*16); qcap=16; if(!q){ snprintf(err,cap,"out of memory"); return -1; }
    q[qn].host=strdup(top); q[qn].rel=strdup(""); qn++;
    while(qi<qn){
        HdDir cur=q[qi++]; DIR *d=opendir(cur.host);
        if(!d){ snprintf(err,cap,"cannot open directory %s: %s",cur.host,strerror(errno)); goto done; }
        char **names=NULL; int nn=0,ncap=0; struct dirent *de;
        while((de=next_ent(d))){
            if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
            if(nn==ncap){ ncap=ncap?ncap*2:32; names=realloc(names,sizeof *names*(size_t)ncap); if(!names){ closedir(d); snprintf(err,cap,"out of memory"); goto done; } }
            names[nn++]=strdup(de->d_name);
        }
        { int rerr=errno,cerr=closedir(d)?errno:0;                                    /* rerr: readdir's NULL was a failed read, not the end */
          if(rerr||cerr){ snprintf(err,cap,"cannot %s directory %s: %s",rerr?"read":"close",cur.host,strerror(rerr?rerr:cerr));
              for(int i=0;i<nn;i++) free(names[i]); free(names); goto done; } }
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

/* The package's own asset of entry `pe`: the tree's vanilla file <dir>/<name>.  The extraction pads those files to 16 bytes, so the
 * file must be `decompressedSize` bytes or that rounded up to 16; exactly `decompressedSize` bytes are served, as the package does. */
static int vanilla_asset(const char *dir,const char *rel,const PkgEnt *pe,HdEnt *e,HdTable *t,char *err,size_t cap){
    char path[4500]; struct stat st;
    if(snprintf(path,sizeof path,"%s/%s",dir,pe->name)>=(int)sizeof path){ snprintf(err,cap,"path too long: entry \"%s\" of %s",pe->name,rel); return -1; }
    if(stat(path,&st)||!S_ISREG(st.st_mode)){ snprintf(err,cap,"package entry \"%s\" of %s has no file in the tree (%s): the tree does not hold this package asset",pe->name,rel,path); return -1; }
    int64_t pad=((int64_t)pe->size+15)&~15ll;
    if(st.st_size!=pe->size&&st.st_size!=pad){
        snprintf(err,cap,"package entry \"%s\" of %s: %s is %lld bytes, the package table says %d (%lld padded): it is not the package's asset, and the provenance list does not name it as the mod's",
                 pe->name,rel,path,(long long)st.st_size,pe->size,(long long)pad); return -1; }
    if(!(e->host=strdup(path))){ snprintf(err,cap,"out of memory"); return -1; }
    e->size=pe->size; e->fsize=st.st_size; e->src=SRC_VANILLA; t->n_van++; if(st.st_size!=pe->size) t->n_trim++;
    return 0;
}
static int mod_asset(const HdFile *f,HdEnt *e,HdTable *t,char *err,size_t cap){
    if(!(e->host=strdup(f->host))){ snprintf(err,cap,"out of memory"); return -1; }
    e->size=(int32_t)f->size; e->fsize=f->size; e->src=SRC_MOD; t->n_mod++; return 0;
}

#include "title_logo.h"
#define TITLE_LAYOUT KH_TITLE_LAYOUT
#define TITLE_IMAGE "remastered/" TITLE_LAYOUT "/-4.png"

/* Only the HD table varies. The loose title layout and its other textures stay shared. */
static int hd_look(const char *rel){
    if(!strcasecmp(rel,TITLE_LAYOUT) && kh_look_enabled()){
        char path[4096];
        if(kh_look_available(TITLE_IMAGE,path,sizeof path)) return 5;
        kh_warn("player-look-title: missing %s in the Xion pack; using original title",TITLE_IMAGE);
        return 0;
    }
    return kh_look_key(rel);
}

/* Builds the table for `rel` (canonical tree path of the game file). Returns 0, or -1 with a message in err.  `root` is the tree; the
 * folder is <root>/remastered/<rel> (case-insensitive).  `bytes` are only read when the folder is scanned. */
static int hd_build(const char *root,const char *rel,const unsigned char *bytes,int64_t n,HdTable **out,char *err,size_t cap){
    HdTable *t=calloc(1,sizeof *t); if(!t){ snprintf(err,cap,"out of memory"); return -1; }
    int gmod=prov_state==2&&prov_find(rel), nmod=prov_state==2?prov_dir_files(rel):0;
    const PkgTab *pt=pkg_state==2?pkg_find(rel):NULL;
    t->game_mod=gmod; t->mod_files=nmod; t->pkg_n=pt?pt->n:0; t->offsets=-1;
    HdFile *files=NULL; int nf=0; int32_t *offs=NULL; char *served=NULL; int rc=-1;
    char sub[2700],dir[4300],full[3000]; struct stat st;
    int styles=!strcasecmp(rel,TITLE_LAYOUT) && bytes && n>=4 && !memcmp(bytes+n-4,"T89L",4);
    t->look=styles?0:hd_look(rel);
    t->hooded=t->look>0 && kh_look_enabled()==2;
    if(t->look==KH_SCENE_MODEL && !strcasecmp(rel,KH_SCENE_MDLX)){
        int count=0,bad=0;
        if(!kh_rm_offsets(rel,bytes,n,&offs,&count,&bad) || bad || count!=KH_SCENE_TEXTURES){
            snprintf(err,cap,"scene-roxas-hd-offsets: expected %d texture offsets in %s, got %d",KH_SCENE_TEXTURES,rel,count); goto fail;
        }
        t->rule=R_SCAN; t->game_mod=1; t->mod_files=t->offsets=count; t->pkg_n=0;
        t->e=calloc(count,sizeof *t->e);
        if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        for(int i=0;i<count;i++){
            HdEnt *e=&t->e[t->n++];
            snprintf(e->name,sizeof e->name,"-%d.dds",i);
            snprintf(full,sizeof full,"remastered/%s/%s",KH_SCENE_MDLX,e->name);
            if(!kh_scene_path(full,dir,sizeof dir) || stat(dir,&st)){ snprintf(err,cap,"scene-roxas-hd-asset: %s",full); goto fail; }
            HdFile file={.host=dir,.size=st.st_size};
            e->orig=(uint32_t)offs[i]; e->gsize=(int32_t)file.size;
            if(mod_asset(&file,e,t,err,cap)) goto fail;
        }
        rc=0; goto fail;
    }
    if(snprintf(sub,sizeof sub,"remastered/%s",rel)>=(int)sizeof sub||strlen(root)+strlen(sub)+2>=sizeof dir){ snprintf(err,cap,"path too long: %s",rel); goto fail; }
    KhRmcFile *leaves=NULL;
    size_t leaves_n=kh_rmc_children(sub,&leaves);
    /* Keep the installed entry order and offsets. A new container without a
     * package table can discover its own leaves through the existing walker. */
    if(!pt && kh_rmc_find(rel)) gmod=t->game_mod=1;
    if(t->look<0 || (!(gmod&&(nmod||leaves_n))&&!pt)){
        t->rule=R_NONE; t->unserved=nmod; t->e=calloc(1,sizeof *t->e); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        *out=t; return 0;
    }
    int have_dir=kh_resolve_ci(root,sub,dir,sizeof dir) && !stat(dir,&st) && S_ISDIR(st.st_mode);
    if(!have_dir && !leaves_n){
        snprintf(err,cap,"%s: %s%d file(s) below remastered/%s, but the tree %s has no such folder",rel,nmod?"the provenance list names ":"the package table has ",nmod?nmod:pt->n,rel,root); goto fail; }
    if(nmod || leaves_n){
        if(nmod && have_dir && scan_files(dir,&files,&nf,err,cap)) goto fail;
        /* the mod's tree is NTFS: a name exists once per directory whatever its case.  Two tree files that differ only in case (they are
         * neighbours in the listing) cannot be told apart by the list: the bake must let the mod's file replace the other. */
        for(int i=0;i+1<nf;i++) if(!strcasecmp(files[i].name,files[i+1].name)){
            snprintf(err,cap,"remastered/%s holds two files that differ only in case (%s, %s): the mod's file system has one name for both, the bake must let the mod's file replace the other",rel,files[i].name,files[i+1].name); goto fail; }
        int k=0;
        for(int i=0;i<nf;i++){                                                          /* slots behind k never keep a pointer: every exit may free the whole array */
            if(snprintf(full,sizeof full,"remastered/%s/%s",rel,files[i].name)>=(int)sizeof full){ snprintf(err,cap,"path too long below remastered/%s",rel); goto fail; }
            if(!prov_find(full)){ free(files[i].host); files[i].host=NULL; continue; }
            if(k!=i){ files[k]=files[i]; files[i].host=NULL; }
            k++;
        }
        nf=k;
        int installed=nf;
        if(leaves_n){
            HdFile *merged=realloc(files,(nf+leaves_n)*sizeof *files);
            if(!merged){ snprintf(err,cap,"out of memory"); goto fail; }
            files=merged;
            for(size_t i=0;i<leaves_n;i++){
                const KhRmcFile *leaf=&leaves[i];
                if(leaf->size>INT_MAX){ snprintf(err,cap,"rmc-hd-size: %s",leaf->rel); goto fail; }
                const char *name=leaf->rel+strlen(sub)+1;
                if(strlen(name)>=sizeof files[nf].name){ snprintf(err,cap,"rmc-hd-name: %s",name); goto fail; }
                int slot=0;
                while(slot<nf && strcasecmp(files[slot].name,name)) slot++;
                if(slot==nf){
                    strcpy(files[nf++].name,name);
                    if(prov_find(leaf->rel)) installed++;
                }else free(files[slot].host);
                files[slot].host=strdup(leaf->host); files[slot].size=leaf->size;
                if(!files[slot].host){ snprintf(err,cap,"out of memory"); goto fail; }
            }
        }
        if(installed!=nmod){ snprintf(err,cap,"the provenance list names %d mod files below remastered/%s, the tree has %d of them: list and tree do not belong together",nmod,rel,nf); goto fail; }
        served=calloc((size_t)nf+1,1); if(!served){ snprintf(err,cap,"out of memory"); goto fail; }
    }
    if(gmod&&(nmod||(!pt && leaves_n))){
        /* ---- ScanRemasteredFolder ---- */
        int noffs=0,werr=0, known=kh_rm_offsets(rel,bytes,n,&offs,&noffs,&werr);
        if(werr){ snprintf(err,cap,"malformed container: the walker for %s read outside the %lld loaded bytes",rel,(long long)n); goto fail; }
        if(!known){                                                                     /* "unknown file type": the entries' own origOffsets */
            free(offs); noffs=pt?pt->n:0; offs=malloc(sizeof *offs*(size_t)(noffs?noffs:1)); if(!offs){ snprintf(err,cap,"out of memory"); goto fail; }
            for(int i=0;i<noffs;i++) offs[i]=(int32_t)pt->e[i].orig;
            t->unknown_ext=1;
        }
        if(styles && noffs!=19){snprintf(err,cap,"title-style-texture-count: %d",noffs);goto fail;}
        if(styles) noffs=9;
        t->offsets=noffs;
        for(int i=0;i<nf;i++) if(strlen(files[i].name)>31){ snprintf(err,cap,"mod file name longer than 31 characters in a scanned folder: %s (folder of %s); Panacea keeps names in 32 bytes",files[i].name,rel); goto fail; }
        t->e=calloc((size_t)(noffs?noffs:1),sizeof *t->e); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        if(nf>=noffs){
            t->rule=R_SCAN; sort_files(files,nf); t->truncated=nf-noffs; t->unserved=nf-noffs;
            for(int i=0;i<noffs;i++){ HdEnt *e=&t->e[i]; strcpy(e->name,files[i].name); e->orig=(uint32_t)offs[i]; e->gsize=(int32_t)files[i].size; t->n++;
                if(mod_asset(&files[i],e,t,err,cap)) goto fail; }
        } else {
            t->rule=R_SCAN_SHORT; int base=pt?pt->n:0;
            for(int i=0;i<noffs;i++){ HdEnt *e=&t->e[i]; t->n++;
                if(i<base){ strcpy(e->name,pt->e[i].name); e->gsize=pt->e[i].size; } else snprintf(e->name,sizeof e->name,"-%d.dds",i);
                e->orig=(uint32_t)offs[i];
                for(int k=0;k<nf;k++) if(!strcmp(e->name,files[k].name)){ e->gsize=(int32_t)files[k].size; break; }       /* Panacea: exact strcmp sets the size */
                int hit=-1; for(int k=0;k<nf;k++) if(!strcasecmp(e->name,files[k].name)){ hit=k; break; }                 /* GetRemasteredAsset: the file system finds it */
                if(hit>=0){ served[hit]=1; if(mod_asset(&files[hit],e,t,err,cap)) goto fail; }
                else if(i<base){ if(vanilla_asset(dir,rel,&pt->e[i],e,t,err,cap)) goto fail; }
                else { e->src=SRC_NULL; t->n_null++; }                                                                    /* padding without a file */
            }
            for(int k=0;k<nf;k++) if(!served[k]) t->unserved++;
        }
    } else {
        /* ---- the package's own table, verbatim ---- */
        t->rule=gmod?R_PKG_MODGAME:(nmod?R_PKG_MODNAMES:R_PKG);
        t->e=calloc((size_t)pt->n,sizeof *t->e); if(!t->e){ snprintf(err,cap,"out of memory"); goto fail; }
        for(int i=0;i<pt->n;i++){ HdEnt *e=&t->e[i]; t->n++;
            strcpy(e->name,pt->e[i].name); e->orig=pt->e[i].orig; e->gsize=pt->e[i].size;
            int hit=-1; for(int k=0;k<nf;k++) if(!strcasecmp(e->name,files[k].name)){ hit=k; break; }
            if(hit>=0){ served[hit]=1; if(mod_asset(&files[hit],e,t,err,cap)) goto fail; if(files[hit].size!=pt->e[i].size) t->n_modsize++; }
            else if(vanilla_asset(dir,rel,&pt->e[i],e,t,err,cap)) goto fail;
        }
        for(int k=0;k<nf;k++) if(!served[k]) t->unserved++;
    }
    if(styles){
        int count=0,bad=0;
        free(offs);offs=NULL;
        if(t->n!=9 || !kh_rm_offsets(rel,bytes,n,&offs,&count,&bad) || bad || count!=19){
            snprintf(err,cap,"title-style-hd-layout: expected 9 original, 3 controller and 7 alternate slots");goto fail;
        }
        HdEnt *entries=realloc(t->e,count*sizeof *entries);
        if(!entries){snprintf(err,cap,"title-style-hd-memory");goto fail;}
        t->e=entries;t->offsets=count;
        for(int i=0;i<count;i++){
            if(i>=9){
                static const int files[]={5,1,2,3,6,7,8};
                char path[4096];
                if(i<12) snprintf(path,sizeof path,"%s",entries[8].host);
                else kh_title_logo_path(files[i-12],"png",path,sizeof path);
                if(stat(path,&st) || st.st_size<=0 || st.st_size>INT_MAX){snprintf(err,cap,"title-style-hd-file: %s",path);goto fail;}
                HdEnt *e=&entries[t->n++];memset(e,0,sizeof *e);
                snprintf(e->name,sizeof e->name,"-%d.png",i);e->gsize=st.st_size;
                HdFile image={.host=path,.size=st.st_size};
                if(mod_asset(&image,e,t,err,cap)) goto fail;
            }
            entries[i].orig=(uint32_t)offs[i];
        }
    }
    if(t->look>0){
        int slot=t->look-1;
        char name[32],key[2600],path[4300];
        snprintf(name,sizeof name,"-%d.%s",slot,!strcasecmp(rel,TITLE_LAYOUT)?"png":"dds");
        if(slot>=t->n || strcasecmp(t->e[slot].name,name)){
            snprintf(err,cap,"player-look-hd-slot: %s lacks texture entry %s",rel,name); goto fail;
        }
        for(int i=0;i<t->n;i++){
            HdEnt *e=&t->e[i];
            snprintf(key,sizeof key,"remastered/%s/%s",rel,e->name);
            if(i!=slot && !kh_player_voice(key)) continue;
            kh_look_path(NULL,key,path,sizeof path);
            if(stat(path,&st)){ snprintf(err,cap,"player-look-hd-stat: %s",path); goto fail; }
            if(e->src==SRC_MOD) t->n_mod--; else if(e->src==SRC_VANILLA) t->n_van--; else t->n_null--;
            free(e->host); e->host=strdup(path);
            if(!e->host){ snprintf(err,cap,"out of memory"); goto fail; }
            e->src=SRC_MOD; t->n_mod++; e->size=e->gsize=e->fsize=st.st_size;
        }
    }
    /* The local layout packer appends a tag and a distinct length per logo.
     * The existing cache and savestate key already include this byte length. */
    if(!strcasecmp(rel,TITLE_LAYOUT) && n>=5 && bytes && !memcmp(bytes+n-5,"T80L",4)){
        int id=bytes[n-1];
        if(id<1 || id>3 || t->n<=7 || strcasecmp(t->e[7].name,"-7.png")){
            snprintf(err,cap,"title-logo-slot: unexpected layout or HD table"); goto fail;
        }
        char path[4096]; kh_title_logo_path(id,"png",path,sizeof path);
        if(stat(path,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || st.st_size>INT_MAX){
            snprintf(err,cap,"title-logo-atlas: %s",path); goto fail;
        }
        HdEnt *e=&t->e[7];
        free(e->host); e->host=strdup(path);
        if(!e->host){ snprintf(err,cap,"out of memory"); goto fail; }
        e->size=e->gsize=e->fsize=st.st_size;
    }
    if(leaves_n) for(int i=0;i<t->n;i++){
        HdEnt *e=&t->e[i];
        snprintf(full,sizeof full,"%s/%s",sub,e->name);
        KhRmcFile *leaf=kh_rmc_find(full);
        if(leaf && e->host && !strcmp(leaf->host,e->host)){
            e->rmc=leaf;
            e->gsize=e->size;
        }
    }
    rc=0;
fail:
    free_files(files,nf); free(offs); free(served);
    if(rc){ table_free(t); return -1; }
    *out=t; return 0;
}

/* ---- guest side -------------------------------------------------------------------------------------------------- */
static int key_eq(const char *a,const char *b){ return !strcasecmp(a,b); }

HdTable *kh_hd_begin(KCtx *g,const char *root,const char *rel_in,const unsigned char *bytes,int64_t n){
    char err[1500]; HdTable *prev=cur_table; st_begin++;
    char rel[2300]; if(!canon_rel(rel_in,rel,sizeof rel)||!*rel) kh_fatal(g,"hd assets: no usable tree path in \"%s\"",rel_in);
    pthread_mutex_lock(&hd_lock);
    int styles=!strcasecmp(rel,TITLE_LAYOUT) && bytes && n>=4 && !memcmp(bytes+n-4,"T89L",4);
    int look=styles?0:hd_look(rel),hooded=look>0 && kh_look_enabled()==2;
    unsigned b=(ci_hash(rel)+hooded)&1023; HdTable *t;
    for(t=buckets[b];t;t=t->next) if(t->hooded==hooded&&t->look==look&&t->fsize==n&&key_eq(t->key,rel)) break;
    if(t&&t->gent){ st_cache_hit++; pthread_mutex_unlock(&hd_lock); cur_table=t; return prev; }
    if(!t){
        cfg_load(g,root);
        if(hd_build(root,rel,bytes,n,&t,err,sizeof err)){ pthread_mutex_unlock(&hd_lock); kh_fatal(g,"hd assets: %s",err); }
        t->key=strdup(rel); t->fsize=n; if(!t->key) kh_fatal(g,"hd assets: out of memory");
        t->next=buckets[b]; buckets[b]=t;
    }
    t->gent=ga_calloc((u64)(t->n?t->n:1),0x30); if(!t->gent) kh_fatal(g,"hd assets: guest arena exhausted for %d entries of %s",t->n,rel);
    for(int i=0;i<t->n;i++){ u64 e=t->gent+0x30ull*(u64)i;
        memcpy(hp(g,e),t->e[i].name,32); wr32(g,e+0x24,t->e[i].orig); wr32(g,e+0x28,(u32)t->e[i].gsize); wr32(g,e+0x2c,(u32)-2); }
    st_built++; st_rule[t->rule]++; st_entries_listed+=(u64)t->n; st_truncated+=(u64)t->truncated; st_unserved+=(u64)t->unserved;
    st_src[SRC_MOD]+=(u64)t->n_mod; st_src[SRC_VANILLA]+=(u64)t->n_van; st_src[SRC_NULL]+=(u64)t->n_null; st_trim+=(u64)t->n_trim; st_modsize+=(u64)t->n_modsize;
    for(int i=0;i<t->n;i++) if(t->e[i].orig==0xffffffffu) st_orig_minus1++;
    if(t->unknown_ext) st_unknown_ext++;
    pthread_mutex_unlock(&hd_lock);
    if(t->rule==R_SCAN_SHORT) fprintf(stderr,"kh: hd: %s: scan-short: the mod's folder has %d files for %d walker offsets; package table of %d cut or padded to %d: %d from mod files by name, %d package assets, %d padding without a file\n",
                                      rel,t->mod_files,t->offsets,t->pkg_n,t->n,t->n_mod,t->n_van,t->n_null);
    if(t->rule!=R_SCAN_SHORT&&t->unserved) fprintf(stderr,"kh: hd: %s: %s: %d of the mod's %d files in the folder are not served (%s), as on the PC\n",rel,rule_names[t->rule],t->unserved,t->mod_files,
                                      t->rule==R_SCAN?"beyond the walker's offset count":"the game file is not the mod's and no package entry has their names");
    if(t->unknown_ext) fprintf(stderr,"kh: hd: %s: scanned folder of a file type without a container walker: the package's %d origOffsets were used\n",rel,t->offsets);
    const char *lg=getenv("KHNMC_HDLOG");
    if(lg){ FILE *f=fopen(lg,"a"); if(!f) kh_fatal(g,"hd assets: cannot append %s",lg);
        int bad=fprintf(f,"%s rule=%s served=%d from-mod=%d from-package=%d null=%d game-file=%s mod-files=%d package-entries=%d offsets=%d truncated=%d unserved-mod-files=%d\n",rel,rule_names[t->rule],t->n,t->n_mod,t->n_van,t->n_null,
                        t->game_mod?"mod":"package",t->mod_files,t->pkg_n,t->offsets,t->truncated,t->unserved)<0;
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
    wr32(g,A1(g),t->e[i].orig); SET_RAX(g,t->gent+0x30ull*(u64)i); KH_RET(g);
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

/* GetRemasteredAsset thunk 0x106460 (CFileMan*, uint *outSize, int index): 16-aligned zero-padded block (rounded to 256) holding the asset, or NULL */
void f_106460(KCtx *g){
    if(!cur_table){ st_orig_fallback++; orig_f_106460(g); return; }
    HdTable *t=cur_table; s32 i=(s32)A2(g); u64 sizeptr=A1(g);
    if(i<0) kh_fatal(g,"hd assets: GetRemasteredAsset called with negative index %d",i);
    if(!sizeptr) kh_fatal(g,"hd assets: GetRemasteredAsset called without a size pointer (index %d of %s)",i,t->key);
    if(i>=t->n){ SET_RAX(g,0); KH_RET(g); return; }
    HdEnt *e=&t->e[i];
    if(!e->host){                               /* scan-short padding: the PC's package has no asset at this index and returns NULL; ResourceEntry then gives the load up */
        st_null_assets++;
        fprintf(stderr,"kh: hd: %s entry %d (\"%s\") is padding without a file: NULL, as the PC's package returns for an index past its table (the game gives this load up)\n",t->key,i,e->name);
        SET_RAX(g,0); KH_RET(g); return; }
    if(e->size<=0) kh_fatal(g,"hd assets: %s entry %d (%s) is an empty file",t->key,i,e->name);
    FILE *f=fopen(e->host,"rb");
    if(!f) kh_fatal(g,"hd assets: cannot open %s (entry %d \"%s\" of %s): %s",e->host,i,e->name,t->key,strerror(errno));
    if(e->rmc) kh_rmc_report(e->rmc);
    struct stat st;
    if(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||st.st_size!=e->fsize){ fclose(f); kh_fatal(g,"hd assets: %s changed after the table was built (%lld bytes then, now %lld)",e->host,(long long)e->fsize,(long long)st.st_size); }
    u64 alloc=((u64)e->size+0xff)&~0xffull, buf=ga_alloc_src(alloc,16,GA_SRC_FILE);
    if(!buf){ fclose(f); kh_fatal(g,"hd assets: guest arena exhausted for a %llu-byte asset (%s)",(unsigned long long)alloc,e->host); }
    memset(hp(g,buf),0,alloc);
    if(fread(hp(g,buf),1,(size_t)e->size,f)!=(size_t)e->size){ fclose(f); ga_free(buf); kh_fatal(g,"hd assets: short read of %s",e->host); }
    fclose(f);
    wr32(g,sizeptr,(u32)e->size);
    st_assets++; st_asset_bytes+=(u64)e->size; note_asset(hp(g,buf),(u32)e->size);
    { extern void kh_file_live_note(u64,u64,const char *); char what[200]; snprintf(what,sizeof what,"%s#%d:%s",t->key,i,e->name); kh_file_live_note(buf,alloc,what); }   /* task 31: who holds the id zone */
    SET_RAX(g,buf); KH_RET(g);
}

#define ULL(x) ((unsigned long long)(x))
void kh_hd_report(FILE *f){
    pthread_mutex_lock(&hd_lock);
    fprintf(f,"kh: hd: loads-with-HD=%llu built=%llu cache-hits=%llu entries-listed=%llu (origOffset -1: %llu) assets-served=%llu (%llu bytes) null-assets=%llu count-calls=%llu entry-calls=%llu lifted-fallback=%llu\n",
        ULL(st_begin),ULL(st_built),ULL(st_cache_hit),ULL(st_entries_listed),ULL(st_orig_minus1),ULL(st_assets),ULL(st_asset_bytes),ULL(st_null_assets),ULL(st_count_calls),ULL(st_entry_calls),ULL(st_orig_fallback));
    fprintf(f,"kh: hd: tables by rule: package=%llu package+mod-names=%llu package(mod-file)=%llu scan=%llu scan-short=%llu none=%llu\n",
        ULL(st_rule[R_PKG]),ULL(st_rule[R_PKG_MODNAMES]),ULL(st_rule[R_PKG_MODGAME]),ULL(st_rule[R_SCAN]),ULL(st_rule[R_SCAN_SHORT]),ULL(st_rule[R_NONE]));
    fprintf(f,"kh: hd: entries by source: mod files=%llu (of another size than the package entry they replace by name: %llu) package assets=%llu (cut from a padded tree file: %llu) padding without a file=%llu; "
              "mod files not served=%llu (beyond a scan's offset count: %llu); scanned folders without a walker=%llu\n",
        ULL(st_src[SRC_MOD]),ULL(st_modsize),ULL(st_src[SRC_VANILLA]),ULL(st_trim),ULL(st_src[SRC_NULL]),ULL(st_unserved),ULL(st_truncated),ULL(st_unknown_ext));
    if(pkg_state==2) fprintf(f,"kh: hd: package tables %s (%d game files, %ld entries)",pkg_file,pkg_files,pkg_entries);
    else fprintf(f,"kh: hd: package tables %s",pkg_state==1?"OFF (KHNMC_HD_PACKAGES=none)":pkg_state==3?"absent (tree without a remastered folder)":"not looked at (no load with HD assets)");
    if(prov_state==2) fprintf(f,"; provenance list %s (%d mod files, %d obsolete nohd lines ignored)\n",prov_file,prov_mods,prov_nohd_ignored);
    else fprintf(f,"; provenance list %s\n",!cfg_loaded?"not looked at":prov_none_explicit?"NONE (KHNMC_HD_MODLIST=none)":"NONE");
    fprintf(f,"kh: hd: served asset kinds:");
    for(int k=0;k<8;k++) fprintf(f," %s=%llu",kind_names[k],ULL(st_kind[k]));
    fprintf(f,"\nkh: hd: served DDS dimensions:");
    for(int i=0;i<st_ndims;i++) fprintf(f," %ux%u=%llu",st_dims[i].w,st_dims[i].h,ULL(st_dims[i].n));
    fprintf(f,"\n");
    pthread_mutex_unlock(&hd_lock);
}

/* ---- whole-tree check (khnmc --hd-check): every game file's table, no game run -------------------------------------- */
enum { W_IDENT, W_FLAGS, W_ORDER, W_DIFF, W_NOWALKER, W_ERROR, W_COUNT };
static const char *wcmp_names[W_COUNT]={"identical","flags-only","order-only","DIFFERENT","no-walker","walker-error"};
static uint32_t off_mask(uint32_t v){ return v==0xffffffffu?v:(v&0x1fffffffu); }      /* what the game compares (0x12c530): the low 29 bits; -1 stays -1 */
static int u32_cmp(const void *a,const void *b){ uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b; return x<y?-1:x>y; }
/* The walker's offset list against a package table: identical / equal but for the flag bits / the same offsets in another order / different. */
static int walker_compare(const int32_t *offs,int noffs,const PkgTab *pt){
    if(noffs!=pt->n) return W_DIFF;
    int ident=1,flags=1;
    for(int i=0;i<noffs;i++){ uint32_t a=(uint32_t)offs[i],b=pt->e[i].orig; if(a!=b) ident=0; if(off_mask(a)!=off_mask(b)) flags=0; }
    if(ident) return W_IDENT; if(flags) return W_FLAGS;
    uint32_t *x=malloc(sizeof *x*(size_t)(noffs?noffs:1)*2); if(!x) return W_ERROR; uint32_t *y=x+(noffs?noffs:1);
    for(int i=0;i<noffs;i++){ x[i]=off_mask((uint32_t)offs[i]); y[i]=off_mask(pt->e[i].orig); }
    qsort(x,(size_t)noffs,sizeof *x,u32_cmp); qsort(y,(size_t)noffs,sizeof *y,u32_cmp);
    int same=!memcmp(x,y,sizeof *x*(size_t)noffs); free(x); return same?W_ORDER:W_DIFF;
}
static char **chk_files; static size_t chk_n,chk_cap;
/* every regular file below host/ as a tree path; the top-level remastered folder, dot directories and the two list files are not game files.  Returns 0, or -1 (errno says why). */
static int chk_walk(const char *host,const char *rel){
    DIR *d=opendir(host); if(!d) return -1; struct dirent *de; int rc=0;
    while(!rc&&(de=next_ent(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        char h[8192],r[4096]; struct stat st;
        if(snprintf(h,sizeof h,"%s/%s",host,de->d_name)>=(int)sizeof h||snprintf(r,sizeof r,"%s%s%s",rel,*rel?"/":"",de->d_name)>=(int)sizeof r){ errno=ENAMETOOLONG; rc=-1; break; }
        if(stat(h,&st)){ rc=-1; break; }
        if(S_ISDIR(st.st_mode)){ if(!*rel&&(!strcasecmp(de->d_name,"remastered")||de->d_name[0]=='.')) continue; rc=chk_walk(h,r); }
        else if(S_ISREG(st.st_mode)){
            if(!*rel&&(!strcmp(de->d_name,"khnmc-hd-modlist.txt")||!strcmp(de->d_name,"khnmc-hd-packages.tsv"))) continue;
            if(chk_n==chk_cap){ chk_cap=chk_cap?chk_cap*2:4096; char **nw=realloc(chk_files,sizeof *nw*chk_cap); if(!nw){ errno=ENOMEM; rc=-1; break; } chk_files=nw; }
            if(!(chk_files[chk_n]=strdup(r))){ errno=ENOMEM; rc=-1; break; } chk_n++;
        }
    }
    if(!rc&&errno) rc=-1;                           /* the loop ended on a NULL with errno set: a failed read, the list would be incomplete */
    int e=errno; if(closedir(d)&&!rc){ rc=-1; e=errno; } errno=e; return rc;
}
static int str_cmp(const void *a,const void *b){ return strcmp(*(char *const *)a,*(char *const *)b); }
static unsigned char *read_whole(const char *path,int64_t *n){
    FILE *f=fopen(path,"rb"); if(!f) return NULL; struct stat st; unsigned char *b=NULL;
    if(!fstat(fileno(f),&st)&&S_ISREG(st.st_mode)&&st.st_size<=INT_MAX&&(b=malloc((size_t)st.st_size+1))){ if(fread(b,1,(size_t)st.st_size,f)!=(size_t)st.st_size){ free(b); b=NULL; } else *n=st.st_size; }
    fclose(f); return b;
}
/* KHNMC_DATA names the tree, KHNMC_HD_PACKAGES / KHNMC_HD_MODLIST as in a run.  Exit status: 0, 1 when a table could not be built, 2 on set-up errors. */
int kh_hd_check(const char *report){
    const char *env=getenv("KHNMC_DATA"); if(!env||!*env){ fprintf(stderr,"hd-check: KHNMC_DATA must name the tree\n"); return 2; }
    char root[4096]; if(!realpath(env,root)){ fprintf(stderr,"hd-check: cannot resolve KHNMC_DATA=%s: %s\n",env,strerror(errno)); return 2; }
    pthread_mutex_lock(&hd_lock); cfg_load(NULL,root); pthread_mutex_unlock(&hd_lock);
    chk_n=0;
    if(chk_walk(root,"")){ fprintf(stderr,"hd-check: cannot walk %s: %s\n",root,strerror(errno)); return 2; }
    qsort(chk_files,chk_n,sizeof *chk_files,str_cmp);
    FILE *rf=NULL; if(report&&*report&&!(rf=fopen(report,"w"))){ fprintf(stderr,"hd-check: cannot write %s: %s\n",report,strerror(errno)); return 2; }
    if(rf) fprintf(rf,"path\trule\tgame-file\tentries\tfrom-mod\tfrom-package\tnull\tmod-files\tpackage-entries\twalker-offsets\ttruncated\tunserved-mod-files\tscan-kind2-to-1\tscan-kind3-to-1\twalker-vs-package\n");
    u64 rules[R_COUNT]={0},src[3]={0},trim=0,modsize=0,trunc=0,unserved=0,unknown=0,errors=0,entries=0,bytes_mod=0,bytes_van=0,minus1=0,bit28=0,hd_folder_modfiles=0,tabs_hit=0;
    u64 wc[2][W_COUNT]={{0}};                     /* [game file is the mod's][category], tables served from the package */
    u64 wcs[W_COUNT]={0};                         /* scanned folders: the mod's file against the package's table */
    u64 kind_lost[4]={0},kind_moot[4]={0},scan_nibble[4]={0},kind_files=0;
    for(size_t i=0;i<chk_n;i++){
        const char *rel=chk_files[i]; char host[8192],err[1500]; HdTable *t=NULL; unsigned char *b=NULL; int64_t n=0;
        const PkgTab *pt=pkg_state==2?pkg_find(rel):NULL; int gmod=prov_state==2&&prov_find(rel), nmod=prov_state==2?prov_dir_files(rel):0;
        hd_folder_modfiles+=(u64)nmod; if(pt) tabs_hit++;
        snprintf(host,sizeof host,"%s/%s",root,rel);
        KhRmcFile *game=kh_rmc_find(rel);
        if(pt||(gmod&&nmod)||game){ b=read_whole(game?game->host:host,&n); if(!b){ fprintf(stderr,"hd-check: ERROR %s: cannot read the game file\n",rel); errors++; continue; } }
        if(hd_build(root,rel,b,n,&t,err,sizeof err)){ fprintf(stderr,"hd-check: ERROR %s: %s\n",rel,err); errors++; free(b); if(rf) fprintf(rf,"%s\tERROR\t%s\t\t\t\t\t\t\t\t\t\t\t\t%s\n",rel,gmod?"mod":"package",err); continue; }
        rules[t->rule]++; src[SRC_MOD]+=(u64)t->n_mod; src[SRC_VANILLA]+=(u64)t->n_van; src[SRC_NULL]+=(u64)t->n_null; trim+=(u64)t->n_trim; modsize+=(u64)t->n_modsize;
        trunc+=(u64)t->truncated; unserved+=(u64)t->unserved; unknown+=(u64)t->unknown_ext; entries+=(u64)t->n;
        for(int k=0;k<t->n;k++){ if(t->e[k].src==SRC_MOD) bytes_mod+=(u64)t->e[k].size; else if(t->e[k].src==SRC_VANILLA) bytes_van+=(u64)t->e[k].size;
            if(t->e[k].orig==0xffffffffu) minus1++; else if(t->e[k].orig&0x10000000u) bit28++; }
        int w=-1, lost[4]={0};
        if(pt){                                                                         /* what the container walker would have said for this file */
            int32_t *offs=NULL; int noffs=0,werr=0, known=kh_rm_offsets(rel,b,n,&offs,&noffs,&werr);
            w=werr?W_ERROR:!known?W_NOWALKER:walker_compare(offs,noffs,pt);
            if(t->rule==R_SCAN||t->rule==R_SCAN_SHORT){ wcs[w]++;
                /* the kind bits (29..30) the package gives a texture that the scan replaces by kind 1 */
                for(int k=0;k<pt->n;k++){ uint32_t o=pt->e[k].orig; if(o==0xffffffffu) continue; int kind=(int)((o>>29)&3); scan_nibble[kind]++;
                    if(kind==1||werr||!known) continue; int still=0; for(int q=0;q<noffs;q++) if(off_mask((uint32_t)offs[q])==off_mask(o)){ still=1; break; }
                    if(still){ kind_lost[kind]++; lost[kind]++; } else kind_moot[kind]++; }
                if(lost[2]||lost[3]) kind_files++;
            } else wc[gmod?1:0][w]++;
            free(offs);
        }
        if(rf) fprintf(rf,"%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n",rel,rule_names[t->rule],gmod?"mod":"package",t->n,t->n_mod,t->n_van,t->n_null,t->mod_files,t->pkg_n,t->offsets,t->truncated,t->unserved,lost[2],lost[3],w<0?"-":wcmp_names[w]);
        table_free(t); free(b);
    }
    if(rf&&fclose(rf)){ fprintf(stderr,"hd-check: cannot write %s\n",report); return 2; }
    printf("hd-check: tree %s: %zu game files; package tables: %s (%d files, %ld entries; %llu of them have their game file in the tree); provenance: %s (%d mod files, %d remastered)\n",root,chk_n,
           pkg_state==2?pkg_file:"none",pkg_files,pkg_entries,ULL(tabs_hit),prov_state==2?prov_file:"none",prov_mods,prov_hd);
    printf("hd-check: tables by rule: package=%llu package+mod-names=%llu package(mod-file)=%llu scan=%llu scan-short=%llu none=%llu; could not be built=%llu\n",
           ULL(rules[R_PKG]),ULL(rules[R_PKG_MODNAMES]),ULL(rules[R_PKG_MODGAME]),ULL(rules[R_SCAN]),ULL(rules[R_SCAN_SHORT]),ULL(rules[R_NONE]),ULL(errors));
    printf("hd-check: entries: %llu = from mod files %llu (%llu bytes; %llu replace a package entry of another size by name) + package assets %llu (%llu bytes; %llu cut from a padded tree file) + padding without a file %llu; origOffset -1: %llu; bit 28 set: %llu\n",
           ULL(entries),ULL(src[SRC_MOD]),ULL(bytes_mod),ULL(modsize),ULL(src[SRC_VANILLA]),ULL(bytes_van),ULL(trim),ULL(src[SRC_NULL]),ULL(minus1),ULL(bit28));
    printf("hd-check: mod remastered files: %d named by the provenance list, %llu in the folder of a game file of the tree (the other %lld belong to no game file and are never loaded); not served: %llu (beyond a scan's offset count: %llu); scanned folders without a walker: %llu\n",
           prov_hd,ULL(hd_folder_modfiles),(long long)prov_hd-(long long)hd_folder_modfiles,ULL(unserved),ULL(trunc),ULL(unknown));
    for(int m=0;m<2;m++){ u64 tot=0; for(int k=0;k<W_COUNT;k++) tot+=wc[m][k];
        printf("hd-check: walker against the package table, %s (%llu tables served from the package):",m?"MOD game files":"PACKAGE (vanilla) game files",ULL(tot));
        for(int k=0;k<W_COUNT;k++) printf(" %s=%llu",wcmp_names[k],ULL(wc[m][k])); printf("\n"); }
    { u64 tot=0; for(int k=0;k<W_COUNT;k++) tot+=wcs[k]; printf("hd-check: walker against the package table, scanned folders that have one (%llu):",ULL(tot)); for(int k=0;k<W_COUNT;k++) printf(" %s=%llu",wcmp_names[k],ULL(wcs[k])); printf("\n"); }
    printf("hd-check: scanned folders, kind bits (origOffset >> 29 & 3) of the package entries: kind0=%llu kind1=%llu kind2=%llu kind3=%llu; the scan gives every texture kind 1, so of the textures still at the package's offset "
           "kind0->1: %llu (no reader tells them apart), kind2->1: %llu, kind3->1: %llu (the game's draw paths for kind 2 / kind 3 are not taken, as on the PC; %llu files); package entries whose offset the mod's file no longer has: kind0=%llu kind2=%llu kind3=%llu\n",
           ULL(scan_nibble[0]),ULL(scan_nibble[1]),ULL(scan_nibble[2]),ULL(scan_nibble[3]),ULL(kind_lost[0]),ULL(kind_lost[2]),ULL(kind_lost[3]),ULL(kind_files),ULL(kind_moot[0]),ULL(kind_moot[2]),ULL(kind_moot[3]));
    fflush(stdout);
    return errors?1:0;
}

/* ---- selftest ---------------------------------------------------------------------------------------------------- */
static int rm_cb(const char *p,const struct stat *s,int fl,struct FTW *w){ (void)s;(void)fl;(void)w; return remove(p); }
static int put_file(const char *root,const char *rel,const void *data,size_t n){
    char path[KH_TMP_MAX+256],dir[KH_TMP_MAX+256]; snprintf(path,sizeof path,"%s/%s",root,rel); snprintf(dir,sizeof dir,"%s",path);
    for(char *p=dir+strlen(root)+1;*p;p++) if(*p=='/'){ *p=0; mkdir(dir,0700); *p='/'; }
    FILE *f=fopen(path,"wb"); if(!f) return 0; int ok=fwrite(data,1,n,f)==n; return fclose(f)==0&&ok;
}
/* BAR of `nimd` type-24 IMD entries followed by `naudio` type-31 "ORIGIN" entries. IMD i sits at `first`+0x40*i with pixel offset 0x20+i, so origOffset = 0x20000000+first+0x40*i+0x20+i. */
static int make_bar_at(unsigned char *buf,int nimd,int naudio,int first){
    int cnt=nimd+naudio; memset(buf,0,0x1000); memcpy(buf,"BAR\x01",4); int32_t c=cnt; memcpy(buf+4,&c,4);
    for(int i=0;i<cnt;i++){ unsigned char *e=buf+16+16*i; int32_t typ=i<nimd?24:31,off=first+0x40*i,sz=0x40; memcpy(e,&typ,4); memcpy(e+8,&off,4); memcpy(e+12,&sz,4);
        unsigned char *d=buf+off; if(i<nimd){ memcpy(d,"IMGD",4); int32_t po=0x20+i; memcpy(d+8,&po,4); memset(d+12,0,4); } else memcpy(d,"ORIGIN",6); }
    return first+0x40*cnt;
}
static int make_bar(unsigned char *buf,int nimd,int naudio){ return make_bar_at(buf,nimd,naudio,0x100); }
static int st_pkg(const char *line){ char b[1024],e[300]; snprintf(b,sizeof b,"%s",line); int r=pkg_parse_line(b,e,sizeof e); if(r) fprintf(stderr,"hd selftest: table line refused: %s\n",e); else pkg_state=2; return !r; }
static int st_pkg_err(const char *line,const char *want){ char b[1024],e[300]; e[0]=0; snprintf(b,sizeof b,"%s",line); return pkg_parse_line(b,e,sizeof e)==-1&&strstr(e,want)!=NULL; }
/* runs the configuration loader in a child with the given environment; returns its exit status (3 = named fatal), -1 on a set-up error */
static int st_cfg_child(const char *root,const char *modlist,const char *packages,int want_pkg_state,int want_prov_state,int want_nohd){
    if(!kh_fork_ok()) return -2;                    /* KH_IOS_PATHS: no child process (host_env.c); the caller reports these cases NOT RUN */
    pid_t pid=kh_fork(); if(pid<0) return -1;
    if(pid==0){
        if(modlist) setenv("KHNMC_HD_MODLIST",modlist,1); else unsetenv("KHNMC_HD_MODLIST");
        if(packages) setenv("KHNMC_HD_PACKAGES",packages,1); else unsetenv("KHNMC_HD_PACKAGES");
        if(!freopen("/dev/null","w",stderr)){}
        cfg_loaded=0; cfg_load(NULL,root);
        _exit(pkg_state==want_pkg_state&&prov_state==want_prov_state&&prov_nohd_ignored==want_nohd?0:1); }
    int st=-1; if(waitpid(pid,&st,0)!=pid||!WIFEXITED(st)) return -1; return WEXITSTATUS(st);
}
/* the selftest's hd_readdir: the reads of one directory (st_rd_dev/st_rd_ino) fail with EIO once st_rd_left of its entries were read */
static dev_t st_rd_dev; static ino_t st_rd_ino; static int st_rd_left;
static struct dirent *st_readdir(DIR *d){ struct stat s;
    if(!fstat(dirfd(d),&s)&&s.st_dev==st_rd_dev&&s.st_ino==st_rd_ino&&st_rd_left--<=0){ errno=EIO; return NULL; }
    return readdir(d);
}
static int st_rd_fail(const char *path,int left){ struct stat s; if(stat(path,&s)) return 0; st_rd_dev=s.st_dev; st_rd_ino=s.st_ino; st_rd_left=left; hd_readdir=st_readdir; return 1; }
static void st_chk_clear(void){ for(size_t i=0;i<chk_n;i++) free(chk_files[i]); chk_n=0; }
int kh_hd_selftest(KCtx *g){
    char dir[KH_TMP_MAX]; int bad=0,checks=0; kh_tmp_path(dir,sizeof dir,"khnmc-hd-XXXXXX");   /* host_env.c: /tmp, or the app container on iOS */
#define TEST(c) do { checks++; if(!(c)){ fprintf(stderr,"hd selftest FAIL (line %d): %s\n",__LINE__,#c); bad++; } } while(0)
#define ORIG(i) (0x20000000u+0x100u+0x40u*(unsigned)(i)+0x20u+(unsigned)(i))
#define BUILD(rel,buf) (t=NULL,!hd_build(dir,rel,buf,0x1000,&t,err,sizeof err)&&t)
#define FAILS(rel,buf,what) (t=NULL,err[0]=0,hd_build(dir,rel,buf,0x1000,&t,err,sizeof err)==-1&&strstr(err,what)!=NULL)
    if(cfg_loaded){ fprintf(stderr,"hd selftest FAIL: the run's configuration is already loaded (the selftest must run before any load)\n"); return 1; }
    if(!mkdtemp(dir)){ fprintf(stderr,"hd selftest: mkdtemp failed\n"); return 1; }
    unsigned char bar[0x1000]; char err[1500]; HdTable *t;
    cfg_loaded=1; prov_clear(); pkg_clear();                                           /* the tests set both lists themselves */

    /* Exercise the real HD cache across a look change, including packs predating title art. */
    {
        const char *oldroot=getenv("KHNMC_LOOK_DIR");
        int savedlook=kh_look_enabled();
        char *savedroot=oldroot?strdup(oldroot):NULL;
        char pack[KH_TMP_MAX+32]; snprintf(pack,sizeof pack,"%s/pack",dir); mkdir(pack,0700);
        setenv("KHNMC_LOOK_DIR",pack,1); kh_look_set(g,0);
        make_bar(bar,8,0);
        TEST(prov_add(TITLE_LAYOUT));
        for(int i=0;i<8;i++){
            char key[128]; snprintf(key,sizeof key,"remastered/" TITLE_LAYOUT "/-%d.png",i);
            TEST(prov_add(key) && put_file(dir,key,"stock",5));
        }
        HdTable *prev=kh_hd_begin(g,dir,TITLE_LAYOUT,bar,sizeof bar), *stock=cur_table;
        TEST(stock && stock->n==8 && stock->look==0);
        kh_look_set(g,1);
        TEST(!hd_look(TITLE_LAYOUT) && put_file(pack,TITLE_IMAGE,"variant",7));
        kh_hd_begin(g,dir,TITLE_LAYOUT,bar,sizeof bar);
        HdTable *variant=cur_table;
        TEST(variant!=stock && variant->n==8 && variant->e[4].size==7 && stock->e[4].size==5);
        if(variant->n==8 && stock->n==8) for(int i=0;i<8;i++){
            TEST(variant->e[i].orig==stock->e[i].orig &&
                 ((strcmp(variant->e[i].host,stock->e[i].host)!=0)==(i==4)));
        }
        kh_look_set(g,0);
        kh_hd_begin(g,dir,TITLE_LAYOUT,bar,sizeof bar); TEST(cur_table==stock);
        const char *logos=getenv("KHNMC_TITLE_LOGO_DIR");
        char *savedlogos=logos?strdup(logos):NULL;
        setenv("KHNMC_TITLE_LOGO_DIR",pack,1);
        TEST(put_file(pack,"xkb.png","logo-xkb",8));
        memcpy(bar+sizeof bar-6,"T80L",4); bar[sizeof bar-2]=3;
        kh_hd_begin(g,dir,TITLE_LAYOUT,bar,sizeof bar-1);
        TEST(cur_table!=stock && cur_table->n==8 && cur_table->e[7].size==8);
        if(cur_table->n==8) for(int i=0;i<8;i++)
            TEST(cur_table->e[i].orig==stock->e[i].orig &&
                 ((strcmp(cur_table->e[i].host,stock->e[i].host)!=0)==(i==7)));
        if(savedlogos) setenv("KHNMC_TITLE_LOGO_DIR",savedlogos,1); else unsetenv("KHNMC_TITLE_LOGO_DIR");
        free(savedlogos);
        kh_hd_end(prev);
        HdTable **bucket=&buckets[ci_hash(TITLE_LAYOUT)&1023];
        while(*bucket){
            HdTable *entry=*bucket; *bucket=entry->next;
            ga_free(entry->gent); table_free(entry);
        }
        if(savedroot) setenv("KHNMC_LOOK_DIR",savedroot,1); else unsetenv("KHNMC_LOOK_DIR");
        kh_look_set(g,savedlook);
        free(savedroot); prov_clear();
    }
    /* ---- helpers: canonical paths, the walker-against-package classes ---- */
    { char c[64]; TEST(canon_rel("./obj//X.mdlx/",c,sizeof c)&&!strcmp(c,"obj/X.mdlx")); TEST(canon_rel("a/./b",c,sizeof c)&&!strcmp(c,"a/b")); TEST(!canon_rel("0123456789",c,8)); }
    { TEST(st_pkg("w/a.bar\t3\t-0.dds:20000010:1\t-1.dds:20000020:1\t-2.dds:ffffffff:1")); const PkgTab *p=pkg_find("W/A.BAR"); TEST(p&&p->n==3);
      int32_t same[3]={0x20000010,0x20000020,-1},flg[3]={0x60000010,0x00000020,-1},ord[3]={0x20000020,-1,0x40000010},dif[3]={0x20000010,0x20000024,-1},b28[3]={0x30000010,0x20000020,-1};
      if(p){ TEST(walker_compare(same,3,p)==W_IDENT&&walker_compare(flg,3,p)==W_FLAGS&&walker_compare(ord,3,p)==W_ORDER&&walker_compare(dif,3,p)==W_DIFF);
             TEST(walker_compare(same,2,p)==W_DIFF&&walker_compare(b28,3,p)==W_DIFF); }                 /* another count; bit 28 is part of what the game compares */
      pkg_clear(); }

    /* ---- the table file: format, every refusal ---- */
    TEST(st_pkg("obj/A.mdlx\t2\t-0.dds:20000120:16\tUS_a:b_0.png:6fffffff:7")&&pkg_files==1&&pkg_entries==2);
    { const PkgTab *p=pkg_find("OBJ/a.MDLX"); TEST(p&&p->n==2&&!strcmp(p->e[0].name,"-0.dds")&&p->e[0].orig==0x20000120u&&p->e[0].size==16&&!strcmp(p->e[1].name,"US_a:b_0.png")&&p->e[1].orig==0x6fffffffu&&p->e[1].size==7); }
    TEST(!pkg_find("obj/B.mdlx"));
    TEST(st_pkg_err("obj/A.mdlx\t1\t-0.dds:20000120:16","second table"));
    TEST(st_pkg_err("obj/B.mdlx\t2\t-0.dds:20000120:16","the count says 2"));
    TEST(st_pkg_err("obj/B.mdlx\t1\t-0.dds:20000120:16\t-1.dds:1:1","more entries than the count"));
    TEST(st_pkg_err("obj/B.mdlx\t0","at least one entry")&&st_pkg_err("obj/B.mdlx\tx\t-0.dds:1:1","bad entry count")&&st_pkg_err("obj/B.mdlx","no tab"));
    TEST(st_pkg_err("obj/B.mdlx\t1\t-0.dds:2000012g:16","hex digits")&&st_pkg_err("obj/B.mdlx\t1\t-0.dds:200001200:16","hex digits")&&st_pkg_err("obj/B.mdlx\t1\t-0.dds::16","hex digits"));
    TEST(st_pkg_err("obj/B.mdlx\t1\t-0.dds:20000120:-1","byte count")&&st_pkg_err("obj/B.mdlx\t1\t-0.dds:20000120:1x","byte count")&&st_pkg_err("obj/B.mdlx\t1\t-0.dds:20000120:","byte count"));
    TEST(st_pkg_err("obj/B.mdlx\t1\t-0.dds:20000120","name:origOffset:size")&&st_pkg_err("obj/B.mdlx\t1\tnocolon","name:origOffset:size"));
    TEST(st_pkg_err("obj/B.mdlx\t1\t01234567890123456789012345678901.dds:1:1","longer than 31")&&st_pkg_err("obj/B.mdlx\t1\t:1:1","empty or longer"));
    TEST(st_pkg_err("\t1\t-0.dds:1:1","empty or over-long path"));
    TEST(pkg_files==1&&!pkg_find("obj/B.mdlx"));                                        /* a refused line leaves nothing behind */
    { const char *good="# two tables\n\nobj/A.mdlx\t1\t-0.dds:20000120:16\r\nmsg/us/B.bar\t2\tUS_B_bar0.png:00000030:5\tse/obj/se1:ffffffff:9\n";
      char lp[KH_TMP_MAX+256]; TEST(put_file(dir,"good.tsv",good,strlen(good))); snprintf(lp,sizeof lp,"%s/good.tsv",dir);
      pkg_clear(); TEST(!pkg_load_file(lp,err,sizeof err)&&pkg_state==2&&pkg_files==2&&pkg_entries==3&&!strcmp(pkg_file,lp));
      { const PkgTab *p=pkg_find("msg/us/b.bar"); TEST(p&&p->n==2&&p->e[1].orig==0xffffffffu&&!strcmp(p->e[1].name,"se/obj/se1")); }
      TEST(put_file(dir,"bad.tsv","obj/A.mdlx\t1\t-0.dds:20000120:16\nobj/C.mdlx\t1\t-0.dds:zz:16\n",58)); snprintf(lp,sizeof lp,"%s/bad.tsv",dir);
      pkg_clear(); TEST(pkg_load_file(lp,err,sizeof err)==-1&&strstr(err,"bad.tsv line 2")&&strstr(err,"hex digits"));
      TEST(put_file(dir,"empty.tsv","# nothing\n",10)); snprintf(lp,sizeof lp,"%s/empty.tsv",dir);
      pkg_clear(); TEST(pkg_load_file(lp,err,sizeof err)==-1&&strstr(err,"holds no table"));
      snprintf(lp,sizeof lp,"%s/does-not-exist.tsv",dir); pkg_clear(); TEST(pkg_load_file(lp,err,sizeof err)==-1&&strstr(err,"cannot open")); }

    /* ---- the provenance list: mod files, the folder index, obsolete nohd lines ---- */
    prov_clear(); TEST(prov_state==1&&!prov_dir_files("obj/PV.mdlx"));
    TEST(prov_add("obj/PV.mdlx")&&prov_add("remastered/obj/PV.mdlx/-0.png")&&prov_add("./remastered/OBJ/pv.MDLX/-1.png")&&prov_add("/remastered/obj/PV.mdlx/se/obj/se7")&&prov_add("remastered/obj/PV.mdlx/-0.png"));
    TEST(prov_state==2&&prov_mods==4&&prov_hd==3&&prov_find("OBJ/pv.mdlx")&&!prov_find("remastered/obj/PV.mdlx/-0.dds")&&prov_dir_files("obj/pv.MDLX")==3&&prov_dir_files("obj/PV.mdlx/se")==1&&!prov_dir_files("obj/PV")&&prov_dir_files("obj")==3);   /* every directory level counts what lies below it */
    { char lp[KH_TMP_MAX+256]; const char *good="# made by a test\n\nmod obj/PV.mdlx\nmod remastered/obj/PV.mdlx/-0.png  # the mod's\n  nohd field2d/us/zz0command.2dd  # moved\n";
      TEST(put_file(dir,"modlist.txt",good,strlen(good))); snprintf(lp,sizeof lp,"%s/modlist.txt",dir);
      prov_clear(); TEST(!prov_load_file(lp,err,sizeof err)&&prov_state==2&&prov_mods==2&&prov_hd==1&&prov_nohd_ignored==1&&prov_find("remastered/obj/PV.mdlx/-0.png")&&!prov_find("field2d/us/zz0command.2dd")&&!strcmp(prov_file,lp));
      TEST(put_file(dir,"badlist.txt","mod obj/A.mdlx\nvanilla obj/B.mdlx\n",34)); snprintf(lp,sizeof lp,"%s/badlist.txt",dir);
      prov_clear(); TEST(prov_load_file(lp,err,sizeof err)==-1&&strstr(err,"line 2")&&strstr(err,"expected `mod <path>`"));
      TEST(put_file(dir,"onlynohd.txt","nohd obj/A.mdlx\n",16)); snprintf(lp,sizeof lp,"%s/onlynohd.txt",dir);
      prov_clear(); TEST(prov_load_file(lp,err,sizeof err)==-1&&strstr(err,"holds no `mod` line"));
      snprintf(lp,sizeof lp,"%s/does-not-exist.txt",dir); prov_clear(); TEST(prov_load_file(lp,err,sizeof err)==-1&&strstr(err,"cannot open")); }

    /* ---- start-up configuration (in a child): defaults, `none`, and every named fatal ---- */
    { char cfg[KH_TMP_MAX+256],pk[KH_TMP_MAX+256],ml[KH_TMP_MAX+256],bare[KH_TMP_MAX+256]; snprintf(cfg,sizeof cfg,"%s/cfgtree",dir); snprintf(bare,sizeof bare,"%s/baretree",dir); snprintf(pk,sizeof pk,"%s/good.tsv",dir); snprintf(ml,sizeof ml,"%s/modlist.txt",dir);
      TEST(put_file(dir,"cfgtree/remastered/obj/A.mdlx/-0.dds","x",1)&&put_file(dir,"baretree/obj/A.mdlx","x",1));
      if(kh_fork_ok()){
      TEST(st_cfg_child(cfg,NULL,NULL,0,0,0)==3);                                       /* remastered folder, no tables: named fatal */
      TEST(st_cfg_child(cfg,NULL,"none",1,1,0)==0);                                     /* ... unless switched off explicitly (no list needed then) */
      TEST(st_cfg_child(bare,NULL,NULL,3,1,0)==0);                                      /* a tree without a remastered folder needs neither */
      TEST(st_cfg_child(cfg,NULL,pk,0,0,0)==3);                                         /* tables but no provenance list: named fatal */
      TEST(st_cfg_child(cfg,"none",pk,2,1,0)==0);                                       /* ... unless the tree is said to hold no mod */
      TEST(st_cfg_child(cfg,ml,pk,2,2,1)==0);                                           /* both named: loaded, the nohd line counted as ignored */
      TEST(st_cfg_child(cfg,ml,"none",1,2,1)==0);
      { char x[KH_TMP_MAX+256]; snprintf(x,sizeof x,"%s/does-not-exist.tsv",dir); TEST(st_cfg_child(cfg,ml,x,0,0,0)==3); snprintf(x,sizeof x,"%s/bad.tsv",dir); TEST(st_cfg_child(cfg,ml,x,0,0,0)==3);
        snprintf(x,sizeof x,"%s/does-not-exist.txt",dir); TEST(st_cfg_child(cfg,x,pk,0,0,0)==3); snprintf(x,sizeof x,"%s/badlist.txt",dir); TEST(st_cfg_child(cfg,x,pk,0,0,0)==3); }
      /* the defaults live in the tree */
      { char a[1300]; snprintf(a,sizeof a,"cfgtree/khnmc-hd-packages.tsv"); TEST(put_file(dir,a,"obj/A.mdlx\t1\t-0.dds:20000120:1\n",31)); TEST(st_cfg_child(cfg,NULL,NULL,0,0,0)==3);   /* tables found, list missing */
        TEST(put_file(dir,"cfgtree/khnmc-hd-modlist.txt","mod obj/A.mdlx\n",15)); TEST(st_cfg_child(cfg,NULL,NULL,2,2,0)==0); }
      } else kh_selftest_not_run("hd","the start-up configuration read in a child process: defaults, `none` and every named fatal (13), with the two fixture files written for them (2)",15);   /* iOS: no fork (host_env.c) */
    }

    /* ==== rule 1: the package's own table, verbatim ==== */
    /* a vanilla game file: package order (not sorted), names, flag bits 0/2/4/6, an origOffset of -1 with a nested name; the walker is not asked (the bytes are no container at all) */
    prov_clear(); pkg_clear(); memset(bar,0xee,sizeof bar);
    TEST(st_pkg("obj/V1.mdlx\t5\t-0.dds:00000120:4\t-10.dds:60000161:2\t-2.dds:40000200:3\tUS_V1_0.png:20000300:21\tse/obj/se200:ffffffff:4"));
    TEST(put_file(dir,"remastered/obj/V1.mdlx/-0.dds","DDS0",4)&&put_file(dir,"remastered/obj/V1.mdlx/-10.dds","Dx",2)&&put_file(dir,"remastered/obj/V1.mdlx/-2.dds","D2!",3)&&put_file(dir,"remastered/obj/V1.mdlx/se/obj/se200","SCD0",4));
    TEST(put_file(dir,"remastered/obj/V1.mdlx/US_V1_0.png","PNG-0123456789abcdef\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd",32));   /* 21 bytes of asset in a file padded to 32 */
    TEST(put_file(dir,"remastered/obj/V1.mdlx/-1.dds","EXTRA",5)&&put_file(dir,"remastered/obj/V1.mdlx/zz.bin","EXTRA",5));          /* files no entry names: never listed */
    TEST(BUILD("obj/V1.mdlx",bar));
    if(t){ TEST(t->rule==R_PKG&&t->n==5&&t->offsets==-1&&t->n_van==5&&t->n_mod==0&&t->n_null==0&&t->n_trim==1&&t->unserved==0&&!t->game_mod&&t->pkg_n==5);
        TEST(!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[1].name,"-10.dds")&&!strcmp(t->e[2].name,"-2.dds")&&!strcmp(t->e[3].name,"US_V1_0.png")&&!strcmp(t->e[4].name,"se/obj/se200"));
        TEST(t->e[0].orig==0x00000120u&&t->e[1].orig==0x60000161u&&t->e[2].orig==0x40000200u&&t->e[3].orig==0x20000300u&&t->e[4].orig==0xffffffffu);   /* flag bits as the package has them */
        TEST(t->e[0].size==4&&t->e[1].size==2&&t->e[3].size==21&&t->e[3].fsize==32&&t->e[3].gsize==21&&t->e[4].size==4&&t->e[0].src==SRC_VANILLA); }
    table_free(t);
    {
        char pack[KH_TMP_MAX+32]; snprintf(pack,sizeof pack,"%s/rmc",dir);
        TEST(mkdir(pack,0700)==0);
        TEST(put_file(pack,"OBJ/v1.MDLX","PATCH",5));
        TEST(put_file(pack,"REMASTERED/OBJ/v1.MDLX/-0.DDS","override",8));
        kh_rmc_init(pack);
        TEST(BUILD("obj/V1.mdlx",bar));
        if(t){
            t->key=strdup("obj/V1.mdlx"); HdTable *prev=cur_table; cur_table=t;
            u64 sz=ga_alloc(16,16);
            u64 b=kh_call(g,GS_BASE+0x106460,0,sz,0,0);
            TEST(b && rd32(g,sz)==8 && !memcmp(hp(g,b),"override",8)); if(b) ga_free(b);
            b=kh_call(g,GS_BASE+0x106460,0,sz,1,0);
            TEST(b && rd32(g,sz)==2 && !memcmp(hp(g,b),"Dx",2)); if(b) ga_free(b);
            ga_free(sz); cur_table=prev; table_free(t);
        }
        kh_rmc_init(NULL);
    }
    /* case-insensitive game path */
    TEST(BUILD("OBJ/v1.MDLX",bar)); if(t) TEST(t->rule==R_PKG&&t->n==5); table_free(t);
    /* a table entry whose file is missing: named error; so is a vanilla file of the wrong size */
    TEST(st_pkg("obj/V2.mdlx\t2\t-0.dds:20000120:4\t-1.dds:20000161:4")&&put_file(dir,"remastered/obj/V2.mdlx/-0.dds","DDS0",4));
    TEST(FAILS("obj/V2.mdlx",bar,"has no file in the tree")&&strstr(err,"\"-1.dds\""));
    TEST(put_file(dir,"remastered/obj/V2.mdlx/-1.dds","DDS01",5)); TEST(FAILS("obj/V2.mdlx",bar,"is not the package's asset"));
    TEST(put_file(dir,"remastered/obj/V2.mdlx/-1.dds","DDS0123456789ab",15)); TEST(FAILS("obj/V2.mdlx",bar,"is not the package's asset"));     /* one byte short of the padded size */
    TEST(put_file(dir,"remastered/obj/V2.mdlx/-1.dds","DDS0",4)); TEST(BUILD("obj/V2.mdlx",bar)); table_free(t);
    /* a table without the folder: named error */
    TEST(st_pkg("obj/V3.mdlx\t1\t-0.dds:20000120:4")); TEST(FAILS("obj/V3.mdlx",bar,"has no such folder"));
    /* no table, no mod folder: zero entries, whatever lies in the tree */
    TEST(put_file(dir,"remastered/obj/N1.mdlx/-0.dds","DDS0",4)); TEST(BUILD("obj/N1.mdlx",bar)); if(t) TEST(t->rule==R_NONE&&t->n==0); table_free(t);
    TEST(BUILD("nodir/at/all.bar",bar)); if(t) TEST(t->rule==R_NONE&&t->n==0); table_free(t);

    /* a MOD game file without a mod folder whose images moved: still the package's table with the vanilla offsets (here the images sit at 0x400.., the table says 0x120..) */
    make_bar_at(bar,2,0,0x400);
    TEST(prov_add("obj/M1.mdlx")&&st_pkg("obj/M1.mdlx\t2\t-0.dds:20000120:4\t-1.dds:00000161:4")&&put_file(dir,"remastered/obj/M1.mdlx/-0.dds","VAN0",4)&&put_file(dir,"remastered/obj/M1.mdlx/-1.dds","VAN1",4));
    TEST(BUILD("obj/M1.mdlx",bar));
    if(t){ TEST(t->rule==R_PKG_MODGAME&&t->game_mod&&t->n==2&&t->offsets==-1&&t->n_van==2&&t->e[0].orig==0x20000120u&&t->e[1].orig==0x00000161u&&strstr(t->e[1].host,"M1.mdlx/-1.dds")); }
    table_free(t);
    /* a mod game file without a table and without a folder: nothing */
    TEST(prov_add("obj/M2.mdlx")); TEST(BUILD("obj/M2.mdlx",bar)); if(t) TEST(t->rule==R_NONE&&t->n==0&&t->game_mod); table_free(t);

    /* a VANILLA game file for which the mod has a folder: never scanned; a mod file is served where its name is a package entry's name (as NTFS compares), the others never */
    make_bar(bar,3,0);
    TEST(st_pkg("msg/us/C1.bar\t3\tUS_C1_bar0.png:20000030:4\tUS_C1_bar1.png:60000040:4\t-2.dds:00000050:4"));
    TEST(put_file(dir,"remastered/msg/us/C1.bar/US_C1_bar0.png","MODFILE0",8)&&put_file(dir,"remastered/msg/us/C1.bar/US_C1_bar1.png","VAN1",4)&&put_file(dir,"remastered/msg/us/C1.bar/-2.DDS","MOD2",4)
       &&put_file(dir,"remastered/msg/us/C1.bar/-0.png","MODX",4));
    TEST(prov_add("remastered/msg/us/C1.bar/US_C1_bar0.png")&&prov_add("remastered/msg/us/C1.bar/-2.DDS")&&prov_add("remastered/msg/us/C1.bar/-0.png"));
    TEST(BUILD("msg/us/C1.bar",bar));
    if(t){ TEST(t->rule==R_PKG_MODNAMES&&!t->game_mod&&t->n==3&&t->offsets==-1&&t->mod_files==3&&t->n_mod==2&&t->n_van==1&&t->unserved==1&&t->n_modsize==1);
        TEST(t->e[0].orig==0x20000030u&&t->e[1].orig==0x60000040u&&t->e[2].orig==0x00000050u);                                       /* the package's offsets, not the walker's */
        TEST(t->e[0].src==SRC_MOD&&t->e[0].size==8&&t->e[0].gsize==4&&t->e[1].src==SRC_VANILLA&&t->e[2].src==SRC_MOD&&!strcmp(t->e[2].name,"-2.dds")&&strstr(t->e[2].host,"-2.DDS")); }
    table_free(t);
    /* the same without a package table: the mod's files are never used */
    TEST(put_file(dir,"remastered/msg/us/C2.bar/-0.png","MODX",4)&&prov_add("remastered/msg/us/C2.bar/-0.png")); TEST(BUILD("msg/us/C2.bar",bar)); if(t) TEST(t->rule==R_NONE&&t->n==0&&t->unserved==1&&t->mod_files==1); table_free(t);
    /* a tree that kept the vanilla file next to a mod file of the same name in another case: named error (NTFS has one name for both) */
    { /* ... which can only exist where the file system keeps both spellings.  Probe the scratch tree itself (macOS APFS folds case by
       * default; a case-sensitive volume there, or a folding one on Linux, is decided by the probe, not by the platform). */
        char pa[1200],pb[1200]; struct stat sb; snprintf(pa,sizeof pa,"%s/CaseProbe.tmp",dir); snprintf(pb,sizeof pb,"%s/caseprobe.tmp",dir);
        FILE *pf=fopen(pa,"w"); int folds=0; if(pf){ fclose(pf); folds=(stat(pb,&sb)==0); remove(pa); }
        /* and directly where the test writes: "-2.DDS" exists there, so a folding file system already finds "-2.dds" (the root probe
         * alone said "keeps both" inside an iOS Simulator container on 2026-11-17, while this directory folded) */
        snprintf(pb,sizeof pb,"%s/remastered/msg/us/C1.bar/-2.dds",dir); if(stat(pb,&sb)==0) folds=1;
#if KH_IOS_PATHS
        /* Inside the iOS Simulator (2026-11-17) both probes said "keeps both spellings" and yet writing "-2.dds" next to "-2.DDS"
         * failed: what the app's sandboxed container does with two spellings is not established, so the case is not run there */
        if(!folds){ fprintf(stderr,"kh: hd selftest: NOT RUN in an iOS app container: 4 checks (case-only name collisions: the sandbox's behaviour is not established)\n"); folds=2; }
#endif
        if(folds){ if(folds==1) fprintf(stderr,"kh: hd selftest: NOT RUN on this file system: 4 checks (two names that differ only in case cannot both exist in %s)\n",dir); }
        else {
    TEST(put_file(dir,"remastered/msg/us/C1.bar/-2.dds","VAN2",4)); TEST(FAILS("msg/us/C1.bar",bar,"differ only in case"));
    TEST(!remove((snprintf(err,sizeof err,"%s/remastered/msg/us/C1.bar/-2.dds",dir),err))); TEST(BUILD("msg/us/C1.bar",bar)); table_free(t);
        } }
    /* list and tree disagree: named error */
    TEST(prov_add("remastered/msg/us/C1.bar/gone.png")); TEST(FAILS("msg/us/C1.bar",bar,"list and tree do not belong together"));

    /* ==== rule 2: ScanRemasteredFolder over the mod's files (mod game file AND mod folder) ==== */
    /* more mod files than offsets: dds by number (leading '-' ignored), png after dds, others last; cut to the offset count; the vanilla files of the folder and the package table play no part */
    prov_clear(); pkg_clear(); make_bar(bar,3,0);
    { const char *a[]={"-10.dds","-2.dds","b.png","-1.dds","-0.dds","-3.png","zz.bin"};
      TEST(prov_add("obj/S1.mdlx"));
      for(unsigned i=0;i<sizeof a/sizeof *a;i++){ char rel[128],body[16]; snprintf(rel,sizeof rel,"remastered/obj/S1.mdlx/%s",a[i]); snprintf(body,sizeof body,"M%u",i); TEST(put_file(dir,rel,body,strlen(body))&&prov_add(rel)); }
      TEST(put_file(dir,"remastered/obj/S1.mdlx/-00.dds","VANILLA",7)&&put_file(dir,"remastered/obj/S1.mdlx/-4.dds","VANILLA",7)); }
    TEST(st_pkg("obj/S1.mdlx\t3\t-4.dds:60000128:7\t-00.dds:00000161:7\tgone.dds:40000999:7"));                                      /* another order, other flags, even a name without a file */
    TEST(BUILD("obj/S1.mdlx",bar));
    if(t){ TEST(t->rule==R_SCAN&&t->game_mod&&t->offsets==3&&t->mod_files==7&&t->n==3&&t->truncated==4&&t->unserved==4&&t->n_mod==3&&t->n_van==0&&t->n_null==0);
        TEST(!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[1].name,"-1.dds")&&!strcmp(t->e[2].name,"-2.dds"));
        for(int i=0;i<3;i++) TEST(t->e[i].orig==ORIG(i)&&t->e[i].src==SRC_MOD);                                                      /* always 0x20000000 + the container offset, as Panacea's walkers make it */
        TEST(t->e[0].size==2&&t->e[0].gsize==2&&strstr(t->e[0].host,"S1.mdlx/-0.dds")); }
    table_free(t);
    /* as many mod files as offsets: exactly those, nothing truncated; png sorted by number; nested audio file last on the audio slot (-1) */
    make_bar(bar,2,1);
    TEST(prov_add("obj/F_X.a.us")&&put_file(dir,"remastered/obj/F_X.a.us/se/obj/se200","SCD0",4)&&put_file(dir,"remastered/obj/F_X.a.us/-1.png","PNG1",4)&&put_file(dir,"remastered/obj/F_X.a.us/-0.png","PNG0",4)
       &&prov_add("remastered/obj/F_X.a.us/se/obj/se200")&&prov_add("remastered/obj/F_X.a.us/-1.png")&&prov_add("remastered/obj/F_X.a.us/-0.png"));
    TEST(BUILD("obj/f_x.A.US",bar));
    if(t){ TEST(t->rule==R_SCAN&&t->n==3&&t->truncated==0&&t->unserved==0&&!strcmp(t->e[0].name,"-0.png")&&!strcmp(t->e[1].name,"-1.png")&&!strcmp(t->e[2].name,"se/obj/se200")&&t->e[2].orig==0xffffffffu&&t->e[0].orig==ORIG(0)&&t->e[1].orig==ORIG(1)); }
    table_free(t);
    /* breadth first: top-level files before nested ones, each directory in NTFS (upper-case) order; names the comparator calls equal keep that order */
    { unsigned char b4[0x1000]; make_bar(b4,4,0);
      TEST(prov_add("obj/bfs.bar")&&put_file(dir,"remastered/obj/bfs.bar/sub/a","1",1)&&put_file(dir,"remastered/obj/bfs.bar/z","2",1)&&put_file(dir,"remastered/obj/bfs.bar/B","3",1)&&put_file(dir,"remastered/obj/bfs.bar/_u","4",1)
         &&prov_add("remastered/obj/bfs.bar/sub/a")&&prov_add("remastered/obj/bfs.bar/z")&&prov_add("remastered/obj/bfs.bar/B")&&prov_add("remastered/obj/bfs.bar/_u"));
      TEST(BUILD("obj/bfs.bar",b4)); if(t) TEST(t->n==4&&!strcmp(t->e[0].name,"B")&&!strcmp(t->e[1].name,"z")&&!strcmp(t->e[2].name,"_u")&&!strcmp(t->e[3].name,"sub/a")); table_free(t); }
    /* a directory read that fails part-way (EIO from readdir) is an error, never the end of the directory: the scan and --hd-check's walk */
    { char w[KH_TMP_MAX+64],ws[KH_TMP_MAX+64]; HdFile *fl=NULL; int nf=-1; snprintf(w,sizeof w,"%s/rdtree",dir); snprintf(ws,sizeof ws,"%s/rdtree/sub",dir);
      TEST(put_file(dir,"rdtree/a","1",1)&&put_file(dir,"rdtree/b","2",1)&&put_file(dir,"rdtree/c","3",1)&&put_file(dir,"rdtree/sub/d","4",1)&&put_file(dir,"rdtree/sub/e","5",1));
      TEST(!scan_files(w,&fl,&nf,err,sizeof err)&&nf==5); free_files(fl,nf); fl=NULL;                          /* every read succeeds: the whole tree */
      TEST(st_rd_fail(ws,1)); err[0]=0;                                                                         /* the nested folder fails after 1 entry */
      TEST(scan_files(w,&fl,&nf,err,sizeof err)==-1&&!fl&&!nf&&strstr(err,"cannot read directory")&&strstr(err,"rdtree/sub")&&strstr(err,strerror(EIO))); if(fl){ free_files(fl,nf); fl=NULL; }
      TEST(st_rd_fail(w,2)); err[0]=0;                                                                          /* the top folder fails after 2 entries */
      TEST(scan_files(w,&fl,&nf,err,sizeof err)==-1&&!fl&&!nf&&strstr(err,"cannot read directory")&&strstr(err,strerror(EIO))); if(fl){ free_files(fl,nf); fl=NULL; }
      hd_readdir=readdir;
      st_chk_clear(); TEST(!chk_walk(w,"")&&chk_n==5); st_chk_clear();
      TEST(st_rd_fail(w,2)); errno=0; TEST(chk_walk(w,"")==-1&&errno==EIO&&chk_n<5); st_chk_clear();             /* the top directory fails after 2 entries */
      TEST(st_rd_fail(ws,0)); errno=0; TEST(chk_walk(w,"")==-1&&errno==EIO&&chk_n<5); st_chk_clear();            /* a nested one: the failure reaches the top */
      hd_readdir=readdir; free(chk_files); chk_files=NULL; chk_cap=0; }
    /* a mod file name over 31 characters in a scanned folder, a malformed container: named errors */
    make_bar(bar,1,0);
    TEST(prov_add("obj/long.bar")&&put_file(dir,"remastered/obj/long.bar/0123456789012345678901234567890123456789.dds","L",1)&&prov_add("remastered/obj/long.bar/0123456789012345678901234567890123456789.dds"));
    TEST(FAILS("obj/long.bar",bar,"longer than 31"));
    { unsigned char z[32]; memset(z,0,sizeof z); memcpy(z,"IMGZ",4); int32_t cnt=1000; memcpy(z+12,&cnt,4);
      TEST(prov_add("obj/bad.imz")&&put_file(dir,"remastered/obj/bad.imz/-0.dds","B",1)&&prov_add("remastered/obj/bad.imz/-0.dds"));
      t=NULL; TEST(hd_build(dir,"obj/bad.imz",z,sizeof z,&t,err,sizeof err)==-1&&strstr(err,"malformed")); }
    /* a file type without a walker: the package's own origOffsets, flag bits and all (nothing when there is no table) */
    TEST(prov_add("obj/x.dat")&&put_file(dir,"remastered/obj/x.dat/-1.dds","X1",2)&&put_file(dir,"remastered/obj/x.dat/-0.dds","X0",2)&&prov_add("remastered/obj/x.dat/-1.dds")&&prov_add("remastered/obj/x.dat/-0.dds"));
    TEST(BUILD("obj/x.dat",bar)); if(t) TEST(t->rule==R_SCAN&&t->unknown_ext&&t->offsets==0&&t->n==0&&t->truncated==2); table_free(t);
    TEST(st_pkg("obj/x.dat\t2\ta.dds:60000040:9\tb.dds:00000080:9"));
    TEST(BUILD("obj/x.dat",bar)); if(t) TEST(t->rule==R_SCAN&&t->unknown_ext&&t->offsets==2&&t->n==2&&!strcmp(t->e[0].name,"-0.dds")&&t->e[0].orig==0x60000040u&&t->e[1].orig==0x00000080u&&t->e[1].src==SRC_MOD); table_free(t);

    /* fewer mod files than offsets: the package table cut to the offset count, the walker's offsets, mod files by name */
    prov_clear(); pkg_clear(); make_bar(bar,4,0);
    TEST(prov_add("obj/H1.mdlx")&&st_pkg("obj/H1.mdlx\t6\t-0.dds:60000999:4\tUS_H1_1.png:00000998:21\t-2.dds:40000997:4\t-3.dds:20000996:4\t-4.dds:20000995:4\t-5.dds:20000994:4"));
    TEST(put_file(dir,"remastered/obj/H1.mdlx/-0.dds","MOD-ZERO",8)&&prov_add("remastered/obj/H1.mdlx/-0.dds"));                    /* exact name: the mod's file, the size field follows */
    TEST(put_file(dir,"remastered/obj/H1.mdlx/-2.DDS","MOD-TWO!!",9)&&prov_add("remastered/obj/H1.mdlx/-2.DDS"));                   /* other case: strcmp misses (size field stays), the file system finds it */
    TEST(put_file(dir,"remastered/obj/H1.mdlx/other.png","MODX",4)&&prov_add("remastered/obj/H1.mdlx/other.png"));                  /* no entry of that name: never served */
    TEST(put_file(dir,"remastered/obj/H1.mdlx/US_H1_1.png","PNG-0123456789abcdef\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd\xcd",32)&&put_file(dir,"remastered/obj/H1.mdlx/-3.dds","VAN3",4));
    TEST(BUILD("obj/H1.mdlx",bar));
    if(t){ TEST(t->rule==R_SCAN_SHORT&&t->offsets==4&&t->mod_files==3&&t->pkg_n==6&&t->n==4&&t->n_mod==2&&t->n_van==2&&t->n_null==0&&t->unserved==1&&t->n_trim==1&&t->truncated==0);
        TEST(!strcmp(t->e[0].name,"-0.dds")&&!strcmp(t->e[1].name,"US_H1_1.png")&&!strcmp(t->e[2].name,"-2.dds")&&!strcmp(t->e[3].name,"-3.dds"));     /* the package's names in the package's order */
        for(int i=0;i<4;i++) TEST(t->e[i].orig==ORIG(i));                                                                            /* but the walker's offsets and flag */
        TEST(t->e[0].src==SRC_MOD&&t->e[0].size==8&&t->e[0].gsize==8&&t->e[1].src==SRC_VANILLA&&t->e[1].size==21&&t->e[1].gsize==21
           &&t->e[2].src==SRC_MOD&&t->e[2].size==9&&t->e[2].gsize==4&&strstr(t->e[2].host,"-2.DDS")&&t->e[3].src==SRC_VANILLA&&t->e[3].size==4); }
    table_free(t);
    /* the package's i-th asset must be in the tree when no mod file has its name */
    TEST(!remove((snprintf(err,sizeof err,"%s/remastered/obj/H1.mdlx/-3.dds",dir),err))); TEST(FAILS("obj/H1.mdlx",bar,"has no file in the tree")&&strstr(err,"\"-3.dds\""));
    /* padding: the package table is shorter than the offsets (here: 1 entry for 4, and no table at all): empty entries "-<index>.dds"; one has a mod file, the others have no asset */
    prov_clear(); pkg_clear(); make_bar(bar,4,0);
    TEST(prov_add("obj/H2.mdlx")&&st_pkg("obj/H2.mdlx\t1\tUS_H2_0.png:00000998:4")&&put_file(dir,"remastered/obj/H2.mdlx/US_H2_0.png","VAN0",4));
    TEST(put_file(dir,"remastered/obj/H2.mdlx/-2.dds","MOD-TWO",7)&&prov_add("remastered/obj/H2.mdlx/-2.dds"));
    TEST(BUILD("obj/H2.mdlx",bar));
    if(t){ TEST(t->rule==R_SCAN_SHORT&&t->n==4&&t->n_van==1&&t->n_mod==1&&t->n_null==2&&t->unserved==0);
        TEST(!strcmp(t->e[0].name,"US_H2_0.png")&&!strcmp(t->e[1].name,"-1.dds")&&!strcmp(t->e[2].name,"-2.dds")&&!strcmp(t->e[3].name,"-3.dds"));
        TEST(t->e[1].src==SRC_NULL&&!t->e[1].host&&t->e[1].gsize==0&&t->e[1].orig==ORIG(1)&&t->e[2].src==SRC_MOD&&t->e[2].size==7&&t->e[2].gsize==7&&t->e[3].src==SRC_NULL&&t->e[3].orig==ORIG(3)); }
    table_free(t);
    TEST(prov_add("obj/H3.mdlx")&&put_file(dir,"remastered/obj/H3.mdlx/-1.dds","MOD-ONE",7)&&prov_add("remastered/obj/H3.mdlx/-1.dds"));
    TEST(BUILD("obj/H3.mdlx",bar)); if(t) TEST(t->rule==R_SCAN_SHORT&&t->pkg_n==0&&t->n==4&&t->n_null==3&&t->n_mod==1&&!strcmp(t->e[0].name,"-0.dds")&&t->e[0].src==SRC_NULL&&t->e[1].src==SRC_MOD); table_free(t);

    /* ---- guest-visible protocol through the hooks (dispatch table), including cache, thread-local state and the lifted fallback ---- */
    prov_clear(); pkg_clear(); memset(buckets,0,sizeof buckets);
    TEST(st_pkg("obj/V1.mdlx\t5\t-0.dds:00000120:4\t-10.dds:60000161:2\t-2.dds:40000200:3\tUS_V1_0.png:20000300:21\tse/obj/se200:ffffffff:4"));
    make_bar(bar,3,0);
    u64 gb=ga_alloc(0x1000,16); memcpy(hp(g,gb),bar,0x1000);
    u64 oldcount=rd32(g,GS_BASE+0x79cfd4),oldpkg=rd64(g,GS_BASE+0x79cfe0);
    u64 fake=ga_calloc(1,0x260); wr32(g,GS_BASE+0x79cfd4,0); wr64(g,GS_BASE+0x79cfe0,fake);
    TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==0);                                    /* no current table: the lifted code reads the (empty) package */
    u64 scratch=ga_alloc(16,16); TEST(kh_call(g,GS_BASE+0x106480,0,scratch,0,0)==0); ga_free(scratch);
    u64 built0=st_built; HdTable *prev=kh_hd_begin(g,dir,"./obj//V1.mdlx",hp(g,gb),0x1000); TEST(prev==NULL&&st_built==built0+1);
    HdTable *t1=cur_table; u64 outp=ga_alloc(16,16),sizep=outp+8; wr32(g,outp,0xdeadbeef);
    TEST(t1&&!strcmp(t1->key,"obj/V1.mdlx")&&kh_call(g,GS_BASE+0x106440,0,0,0,0)==5);
    u64 e1=kh_call(g,GS_BASE+0x106480,0,outp,1,0);
    TEST(e1&&!strcmp((char*)hp(g,e1),"-10.dds")&&rd32(g,outp)==0x60000161u&&rd32(g,e1+0x24)==0x60000161u&&(s32)rd32(g,e1+0x2c)==-2&&rd32(g,e1+0x28)==2);       /* flag bits reach the game */
    e1=kh_call(g,GS_BASE+0x106480,0,outp,4,0); TEST(e1&&!strcmp((char*)hp(g,e1),"se/obj/se200")&&rd32(g,outp)==0xffffffffu);
    TEST(kh_call(g,GS_BASE+0x106480,0,outp,5,0)==0&&kh_call(g,GS_BASE+0x106460,0,sizep,5,0)==0);   /* index past the end: NULL */
    wr32(g,sizep,0); u64 as=kh_call(g,GS_BASE+0x106460,0,sizep,1,0);
    TEST(as&&!(as&15)&&rd32(g,sizep)==2&&!memcmp(hp(g,as),"Dx",2)&&rd8(g,as+2)==0&&rd8(g,as+255)==0&&ga_usable(as)>=256);
    ga_free(as);
    wr32(g,sizep,0); as=kh_call(g,GS_BASE+0x106460,0,sizep,3,0);                                    /* a padded tree file: exactly the package's 21 bytes, zeroes behind them (not the 0xCD filler) */
    TEST(as&&rd32(g,sizep)==21&&!memcmp(hp(g,as),"PNG-0123456789abcdef",20)&&rd8(g,as+20)==0xcd&&rd8(g,as+21)==0&&rd8(g,as+31)==0);
    ga_free(as);
    u64 hits=st_cache_hit; HdTable *p2=kh_hd_begin(g,dir,"OBJ/v1.mdlx",hp(g,gb),0x1000);            /* same file, other case: served from the cache */
    TEST(p2==t1&&cur_table==t1&&st_cache_hit==hits+1);
    kh_hd_end(p2); TEST(cur_table==t1); kh_hd_end(prev); TEST(cur_table==NULL);
    TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==0);                                    /* state is thread-local and restored */
    /* a scanned folder through the hooks: the mod's bytes; then a padding entry: NULL and counted */
    TEST(prov_add("obj/S1.mdlx")); { const char *a[]={"-10.dds","-2.dds","b.png","-1.dds","-0.dds","-3.png","zz.bin"}; for(unsigned i=0;i<7;i++){ char rel[128]; snprintf(rel,sizeof rel,"remastered/obj/S1.mdlx/%s",a[i]); TEST(prov_add(rel)); } }
    { HdTable *pv=kh_hd_begin(g,dir,"obj/S1.mdlx",hp(g,gb),0x1000); TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==3);
      u64 e0=kh_call(g,GS_BASE+0x106480,0,outp,2,0); TEST(e0&&!strcmp((char*)hp(g,e0),"-2.dds")&&rd32(g,outp)==ORIG(2));
      u64 as2=kh_call(g,GS_BASE+0x106460,0,sizep,2,0); TEST(as2&&rd32(g,sizep)==2&&!memcmp(hp(g,as2),"M1",2)); ga_free(as2); kh_hd_end(pv); }       /* "M1": -2.dds is file 1 of the written list */
    { unsigned char b4[0x1000]; make_bar(b4,4,0); u64 gb4=ga_alloc(0x1000,16); memcpy(hp(g,gb4),b4,0x1000);
      TEST(prov_add("obj/H3.mdlx")&&prov_add("remastered/obj/H3.mdlx/-1.dds"));
      u64 nulls=st_null_assets; HdTable *pv=kh_hd_begin(g,dir,"obj/H3.mdlx",hp(g,gb4),0x1000); TEST(kh_call(g,GS_BASE+0x106440,0,0,0,0)==4);
      u64 e0=kh_call(g,GS_BASE+0x106480,0,outp,0,0); TEST(e0&&!strcmp((char*)hp(g,e0),"-0.dds")&&rd32(g,outp)==ORIG(0)&&rd32(g,e0+0x28)==0);
      wr32(g,sizep,77); TEST(kh_call(g,GS_BASE+0x106460,0,sizep,0,0)==0&&rd32(g,sizep)==77&&st_null_assets==nulls+1);                               /* padding without a file: NULL, size untouched */
      u64 as2=kh_call(g,GS_BASE+0x106460,0,sizep,1,0); TEST(as2&&rd32(g,sizep)==7&&!memcmp(hp(g,as2),"MOD-ONE",7)); ga_free(as2); kh_hd_end(pv); ga_free(gb4); }
    wr32(g,GS_BASE+0x79cfd4,(u32)oldcount); wr64(g,GS_BASE+0x79cfe0,oldpkg); ga_free(fake); ga_free(gb); ga_free(outp);

    /* the run's own lists are read at the first real load; drop the selftest's tables and counters (they point into the temporary tree) */
    prov_clear(); pkg_clear(); prov_state=0; pkg_state=0; cfg_loaded=0; memset(buckets,0,sizeof buckets);
    st_begin=st_cache_hit=st_built=st_count_calls=st_entry_calls=st_assets=st_asset_bytes=st_null_assets=st_orig_fallback=st_entries_listed=st_orig_minus1=st_truncated=st_unserved=st_unknown_ext=st_trim=st_modsize=0;
    for(int i=0;i<R_COUNT;i++) st_rule[i]=0; for(int i=0;i<3;i++) st_src[i]=0; for(int i=0;i<8;i++) st_kind[i]=0; st_ndims=0;
#undef ORIG
#undef BUILD
#undef FAILS
    nftw(dir,rm_cb,16,FTW_DEPTH|FTW_PHYS);
    fprintf(stderr,"kh: hd selftest: %d checks, %d failures\n",checks,bad); return bad;
}

#include "rewind.h"
/* Table metadata lives for the session. A guest entry array lives on just one
 * timeline. A cached table without its array materializes it on the next hit. */
void kh_rewind_hd(KhRecords *r){
    size_t count=0;
    for(unsigned i=0;i<1024;i++) for(HdTable *t=buckets[i];t;t=t->next){
        if(r->loading) t->gent=0; else count+=(t->gent!=0);
    }
    KH_RECORD(r,count);
    if(r->loading){
        while(count--){
            size_t n=0; int look,hooded; int64_t size; char key[2300];
            KH_RECORD(r,n);
            if(!n||n>sizeof key) kh_fatal(NULL,"savestate-hd-key-length");
            kh_record(r,key,n); KH_RECORD(r,look); KH_RECORD(r,hooded); KH_RECORD(r,size);
            if(key[n-1]) kh_fatal(NULL,"savestate-hd-key");
            HdTable *t=buckets[(ci_hash(key)+hooded)&1023];
            while(t && (t->hooded!=hooded||t->look!=look||t->fsize!=size||!key_eq(t->key,key))) t=t->next;
            if(!t) kh_fatal(NULL,"savestate-hd-binding: %s",key);
            KH_RECORD(r,t->gent);
        }
    }else{
        for(unsigned i=0;i<1024;i++) for(HdTable *t=buckets[i];t;t=t->next) if(t->gent){
            size_t n=strlen(t->key)+1;
            KH_RECORD(r,n); kh_record(r,t->key,n); KH_RECORD(r,t->look); KH_RECORD(r,t->hooded); KH_RECORD(r,t->fsize); KH_RECORD(r,t->gent);
        }
    }
}
