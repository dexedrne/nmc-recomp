/* float_probe.c : cross-architecture probe of the float policy pieces that the lifted code gets from the RUNTIME side
 * (docs/LIFT-POLICY.md sections 4, 5, 7), for task 20.  Build and run the same source on x86-64 Linux and on arm64 macOS and
 * compare the outputs line by line (tools/float_probe.sh does that).
 *
 *   cc -O1 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -I lifter-rt tools/float_probe.c \
 *      lifter-rt/rt_libm.c lifter-rt/khrt_rcp_tab.c -lm -lpthread -o float_probe
 *   ./float_probe [dump.bin]
 *
 * What it prints (every line starts with a tag; lines tagged HOST describe the machine and are expected to differ):
 *   RCP   hash of kh_rcp32 / kh_rsqrt32 over ALL 2^32 inputs (the bit-exact AMD Zen 3/4 table model of rcpps / rsqrtps).
 *         x86-64 only: also the number of inputs where the CPU's own RCPPS / RSQRTPS differs from the model (HOST line).
 *   LIBM  per CRT math import of rt_libm.c, called through the real wrapper with a guest context: hash of the result bits over
 *         N seeded inputs (specials, the ranges a game uses, random bit patterns).  Single-precision imports are evaluated in
 *         double and rounded once; double imports are the host libm's results.
 *   DEN   gradual underflow is on (no flush-to-zero / denormals-are-zero) on the main thread and on a new thread; the FP control
 *         word of the host as the runtime finds it.
 *   CONV  float -> int conversions of out-of-range values through the policy helpers (x86 "integer indefinite").
 * With a file argument the result bits of every LIBM call are also written to it (8 bytes each, little endian, the order of the
 * LIBM lines), so that two hosts can be compared value by value: which import differs, how often, by how many ulp.
 *
 *   ./float_probe --replay args.bin results.bin
 * args.bin = the arguments a real run passed to the transcendental CRT math imports (runtime: KH_LIBM_ARGS=file; 24-byte records
 * {name[8], xmm0 low 8 bytes, xmm1 low 8 bytes}, deduplicated).  Every record is run through the same wrapper and the result bits
 * are written to results.bin (8 bytes per record, same order); one REPLAY line per import with a hash.  Run on two hosts with the
 * same args.bin: the result files must be identical for the route's own inputs to give the same floats on both. */
#define _GNU_SOURCE
#include "khrt.h"
#include "khrt_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

void kh_trace_import(KCtx *g,const char *name){ (void)g; (void)name; }      /* rt_libm.c of the runtime traces its calls */
void kh_dispatch(KCtx *g,u64 t){ (void)g; (void)t; abort(); }
void kh_fault(KCtx *g,u64 va,const char *k){ (void)g; (void)va; (void)k; abort(); }

static u64 rs;
static u64 prng(void){ rs+=0x9E3779B97F4A7C15ull; u64 x=rs; x=(x^(x>>30))*0xBF58476D1CE4E5B9ull; x=(x^(x>>27))*0x94D049BB133111EBull; return x^(x>>31); }
static u64 fnv(u64 h,u64 v){ for(int i=0;i<8;i++){ h^=(v>>(8*i))&0xff; h*=0x100000001b3ull; } return h; }
#define FNV0 0xcbf29ce484222325ull

/* ---- inputs: a mix that covers what a game feeds libm (angles, small magnitudes, unit range) and the corners ---- */
static double uni(double lo,double hi){ return lo+(hi-lo)*((double)(prng()>>11)/9007199254740992.0); }
static float in_f32(unsigned k){
    static const u32 sp[]={0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00000,0xffc00000,0x7f800001,0x00000001,0x007fffff,0x00800000,0x7f7fffff,0xff7fffff,0x3f000000,0x40490fdb,0x3fc90fdb,0x33800000};
    unsigned c=(unsigned)(prng()%100);
    if(k<sizeof sp/sizeof*sp) return kh_bf32(sp[k]);
    if(c<35) return (float)uni(-6.5,6.5);                       /* angles */
    if(c<55) return (float)uni(-1.0,1.0);                       /* asin / acos domain, unit vectors */
    if(c<70) return (float)uni(0.0,2000.0);                     /* distances, positive magnitudes */
    if(c<80) return (float)uni(-100000.0,100000.0);
    if(c<88) return (float)((int)(prng()%4001)-2000)/16.0f;     /* exact binary fractions */
    if(c<94) return kh_bf32((u32)prng()&0x807fffffu);           /* denormals */
    return kh_bf32((u32)prng());                                /* any bit pattern (huge, tiny, NaN payloads) */
}
static double in_f64(unsigned k){
    static const u64 sp[]={0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000000ull,0xfff8000000000000ull,
                           0x7ff0000000000001ull,1,0x000fffffffffffffull,0x0010000000000000ull,0x7fefffffffffffffull,0x3fe0000000000000ull,0x400921fb54442d18ull,0x3ff921fb54442d18ull};
    unsigned c=(unsigned)(prng()%100);
    if(k<sizeof sp/sizeof*sp) return kh_bf64(sp[k]);
    if(c<35) return uni(-6.5,6.5);
    if(c<55) return uni(-1.0,1.0);
    if(c<70) return uni(0.0,2000.0);
    if(c<80) return uni(-100000.0,100000.0);
    if(c<88) return (double)(float)uni(-700.0,700.0);           /* values that came from a float */
    if(c<94) return kh_bf64(prng()&0x800fffffffffffffull);
    return kh_bf64(prng());
}

#define M(n) rt_imp_api_ms_win_crt_math_l1_1_0__##n
#define DECL(n) void M(n)(KCtx *g);
DECL(sinf) DECL(cosf) DECL(tanf) DECL(asinf) DECL(acosf) DECL(atanf) DECL(expf) DECL(logf) DECL(log10f) DECL(floorf) DECL(ceilf) DECL(roundf) DECL(sqrtf)
DECL(atan2f) DECL(powf) DECL(fmodf) DECL(sin) DECL(cos) DECL(atan) DECL(acos) DECL(exp) DECL(log) DECL(floor) DECL(ceil) DECL(sqrt) DECL(pow)
DECL(modf) DECL(modff) DECL(frexp) DECL(ldexp) DECL(_isnan) DECL(_finite)
typedef void (*Imp)(KCtx*);
enum { K_F1, K_F2, K_D1, K_D2, K_MODF, K_MODFF, K_FREXP, K_LDEXP, K_PRED };
static const struct { const char *name; Imp fn; int kind; } L[]={
    {"sinf",M(sinf),K_F1},{"cosf",M(cosf),K_F1},{"tanf",M(tanf),K_F1},{"asinf",M(asinf),K_F1},{"acosf",M(acosf),K_F1},{"atanf",M(atanf),K_F1},
    {"expf",M(expf),K_F1},{"logf",M(logf),K_F1},{"log10f",M(log10f),K_F1},{"floorf",M(floorf),K_F1},{"ceilf",M(ceilf),K_F1},{"roundf",M(roundf),K_F1},{"sqrtf",M(sqrtf),K_F1},
    {"atan2f",M(atan2f),K_F2},{"powf",M(powf),K_F2},{"fmodf",M(fmodf),K_F2},
    {"sin",M(sin),K_D1},{"cos",M(cos),K_D1},{"atan",M(atan),K_D1},{"acos",M(acos),K_D1},{"exp",M(exp),K_D1},{"log",M(log),K_D1},{"floor",M(floor),K_D1},{"ceil",M(ceil),K_D1},{"sqrt",M(sqrt),K_D1},
    {"pow",M(pow),K_D2},{"modf",M(modf),K_MODF},{"modff",M(modff),K_MODFF},{"frexp",M(frexp),K_FREXP},{"ldexp",M(ldexp),K_LDEXP},{"_isnan",M(_isnan),K_PRED},{"_finite",M(_finite),K_PRED} };
#define NL (sizeof L/sizeof*L)
#define NIN 200000u

static void *den_thread(void *p){ volatile float a=kh_bf32(0x00800000u), h=0.5f; volatile double b=kh_bf64(0x0010000000000000ull); u64 *o=p;
    o[0]=kh_fb32(a*h); o[1]=kh_fb64(b*0.5); volatile float d=kh_bf32(0x00000003u); o[2]=kh_fb32(d+d); return 0; }

static int replay(const char *in,const char *out){
    FILE *fi=fopen(in,"rb"), *fo=fopen(out,"wb"); if(!fi||!fo){ perror(fi?out:in); return 2; }
    KCtx *g; if(posix_memalign((void**)&g,64,sizeof *g)) return 2; memset(g,0,sizeof *g);
    static u64 hash[sizeof L/sizeof*L], cnt[sizeof L/sizeof*L], nanres[sizeof L/sizeof*L]; unsigned long long unknown=0; char rec[24];
    for(unsigned i=0;i<NL;i++) hash[i]=FNV0;
    while(fread(rec,1,24,fi)==24){
        char nm[9]; memcpy(nm,rec,8); nm[8]=0; u64 a,b; memcpy(&a,rec+8,8); memcpy(&b,rec+16,8); int id=-1;
        for(unsigned i=0;i<NL;i++) if(!strcmp(L[i].name,nm)){ id=(int)i; break; }
        u64 r=0; if(id<0||L[id].kind>K_D2){ unknown++; fwrite(&r,8,1,fo); continue; }
        u64 stack[4]={0,0,0,0}; KH_RSP(g)=(u64)(uintptr_t)&stack[2]; for(int x=0;x<4;x++) KH_XMM(g,x)=VZ;
        int single=L[id].kind==K_F1||L[id].kind==K_F2;
        memcpy(&KH_XMM(g,0),&a,single?4:8); memcpy(&KH_XMM(g,1),&b,single?4:8); L[id].fn(g);
        if(single){ u32 o; memcpy(&o,&KH_XMM(g,0),4); r=o; if((o&0x7f800000u)==0x7f800000u&&(o&0x7fffffu)){ r=0x7fc00000u; nanres[id]++; } }
        else { memcpy(&r,&KH_XMM(g,0),8); if((r&0x7ff0000000000000ull)==0x7ff0000000000000ull&&(r&0xfffffffffffffull)){ r=0x7ff8000000000000ull; nanres[id]++; } }
        hash[id]=fnv(hash[id],r); cnt[id]++; fwrite(&r,8,1,fo);
    }
    for(unsigned i=0;i<NL;i++) if(cnt[i]) printf("REPLAY %-7s n=%llu hash=%016llx nan-results=%llu\n",L[i].name,(unsigned long long)cnt[i],(unsigned long long)hash[i],(unsigned long long)nanres[i]);
    if(unknown) printf("REPLAY records of an import this probe does not replay: %llu\n",unknown);
    fclose(fi); fclose(fo); free(g); return 0;
}
int main(int argc,char **argv){
    if(argc==4&&!strcmp(argv[1],"--replay")) return replay(argv[2],argv[3]);
    FILE *dump=argc>1?fopen(argv[1],"wb"):NULL; if(argc>1&&!dump){ perror(argv[1]); return 2; }
#if defined(__x86_64__)
    printf("HOST arch=x86-64");
#elif defined(__aarch64__)
    printf("HOST arch=arm64");
#else
    printf("HOST arch=other");
#endif
#if defined(__APPLE__)
    printf(" os=darwin");
#elif defined(__linux__)
    printf(" os=linux");
#endif
    printf(" compiler=%s\n",__VERSION__);
    /* ---- RCP: the whole input space ---- */
    { u64 h1=FNV0,h2=FNV0; unsigned long long hw1=0,hw2=0; int have_hw=0;
      for(u64 x=0;x<(1ull<<32);x+=4){
        u32 a[4],b[4]; for(int k=0;k<4;k++){ float f=kh_bf32((u32)(x+k)); a[k]=kh_fb32(kh_rcp32(f)); b[k]=kh_fb32(kh_rsqrt32(f)); }
        h1=(h1^(((u64)a[0]<<32)|a[1]))*0x100000001b3ull; h1=(h1^(((u64)a[2]<<32)|a[3]))*0x100000001b3ull;
        h2=(h2^(((u64)b[0]<<32)|b[1]))*0x100000001b3ull; h2=(h2^(((u64)b[2]<<32)|b[3]))*0x100000001b3ull;
#if defined(__x86_64__)
        { __m128 f=_mm_castsi128_ps(_mm_set_epi32((int)(x+3),(int)(x+2),(int)(x+1),(int)x)); u32 c[4],d[4];
          _mm_storeu_si128((__m128i*)c,_mm_castps_si128(_mm_rcp_ps(f))); _mm_storeu_si128((__m128i*)d,_mm_castps_si128(_mm_rsqrt_ps(f)));
          for(int k=0;k<4;k++){ hw1+=c[k]!=a[k]; hw2+=d[k]!=b[k]; } have_hw=1; }
#endif
      }
      printf("RCP kh_rcp32 all-2^32 hash=%016llx\nRCP kh_rsqrt32 all-2^32 hash=%016llx\n",(unsigned long long)h1,(unsigned long long)h2);
      if(have_hw) printf("HOST this CPU's RCPPS differs from the model on %llu inputs, RSQRTPS on %llu (0 on AMD Zen 3 / Zen 4, the table's source)\n",hw1,hw2);
      else printf("HOST no RCPPS / RSQRTPS on this CPU: the model is the only implementation\n"); }
    /* ---- LIBM through the real import wrappers ---- */
    { KCtx *g; if(posix_memalign((void**)&g,64,sizeof *g)) return 2; memset(g,0,sizeof *g);
      static u64 scratch[4]; g->bias=0;                               /* out-parameters: guest address = host address - bias */
      for(unsigned i=0;i<NL;i++){
        rs=0x243F6A8885A308D3ull+i*0x1000193ull; u64 h=FNV0;
        for(unsigned k=0;k<NIN;k++){
            u64 r=0; u64 stack[4]={0,0,0,0}; KH_RSP(g)=(u64)(uintptr_t)&stack[2]-g->bias; KH_RDX(g)=(u64)(uintptr_t)scratch-g->bias; scratch[0]=0;
            for(int x=0;x<4;x++) KH_XMM(g,x)=VZ;
            switch(L[i].kind){
              case K_F1: { float a=in_f32(k); memcpy(&KH_XMM(g,0),&a,4); L[i].fn(g); u32 o; memcpy(&o,&KH_XMM(g,0),4); r=o; break; }
              case K_F2: { float a=in_f32(k), b=in_f32(k/3); if(k%7==0) b=(float)(int)(prng()%9)-4.0f; memcpy(&KH_XMM(g,0),&a,4); memcpy(&KH_XMM(g,1),&b,4); L[i].fn(g); u32 o; memcpy(&o,&KH_XMM(g,0),4); r=o; break; }
              case K_D1: { double a=in_f64(k); memcpy(&KH_XMM(g,0),&a,8); L[i].fn(g); memcpy(&r,&KH_XMM(g,0),8); break; }
              case K_D2: { double a=in_f64(k), b=in_f64(k/3); if(k%7==0) b=(double)((int)(prng()%9)-4); memcpy(&KH_XMM(g,0),&a,8); memcpy(&KH_XMM(g,1),&b,8); L[i].fn(g); memcpy(&r,&KH_XMM(g,0),8); break; }
              case K_MODF: { double a=in_f64(k); memcpy(&KH_XMM(g,0),&a,8); L[i].fn(g); u64 fr,ip=scratch[0]; memcpy(&fr,&KH_XMM(g,0),8);
                             if((fr&0x7ff0000000000000ull)==0x7ff0000000000000ull&&(fr&0xfffffffffffffull)) fr=0x7ff8000000000000ull;
                             if((ip&0x7ff0000000000000ull)==0x7ff0000000000000ull&&(ip&0xfffffffffffffull)) ip=0x7ff8000000000000ull;
                             r=fr^(ip*0x9E3779B97F4A7C15ull); break; }
              case K_MODFF:{ float a=in_f32(k); memcpy(&KH_XMM(g,0),&a,4); L[i].fn(g); u32 o,ip=(u32)scratch[0]; memcpy(&o,&KH_XMM(g,0),4);
                             if((o&0x7f800000u)==0x7f800000u&&(o&0x7fffffu)) o=0x7fc00000u; if((ip&0x7f800000u)==0x7f800000u&&(ip&0x7fffffu)) ip=0x7fc00000u;
                             r=o|((u64)ip<<32); break; }
              case K_FREXP:{ double a=in_f64(k); if(a!=a||a-a!=0) a=1.5; memcpy(&KH_XMM(g,0),&a,8); L[i].fn(g); memcpy(&r,&KH_XMM(g,0),8); r^=(scratch[0]&0xffffffffu)*0x9E3779B97F4A7C15ull; break; }   /* frexp of NaN / inf: exponent unspecified in C */
              case K_LDEXP:{ double a=in_f64(k); KH_RDX(g)=(u64)(s64)((int)(prng()%121)-60); memcpy(&KH_XMM(g,0),&a,8); L[i].fn(g); memcpy(&r,&KH_XMM(g,0),8); break; }
              default:     { double a=in_f64(k); memcpy(&KH_XMM(g,0),&a,8); KH_RAX(g)=~0ull; L[i].fn(g); r=KH_RAX(g); break; }
            }
            /* a NaN result: which NaN comes back for a NaN argument or an invalid operation is the host libm's choice; the policy for
             * the lifted arithmetic does not reach inside libm.  Hash "is a NaN" so that the comparison is about numbers. */
            if(L[i].kind==K_F1||L[i].kind==K_F2){ if((r&0x7f800000u)==0x7f800000u&&(r&0x7fffffu)) r=0x7fc00000u; }
            else if(L[i].kind==K_D1||L[i].kind==K_D2||L[i].kind==K_LDEXP){ if((r&0x7ff0000000000000ull)==0x7ff0000000000000ull&&(r&0xfffffffffffffull)) r=0x7ff8000000000000ull; }
            h=fnv(h,r); if(dump) fwrite(&r,8,1,dump);
        }
        printf("LIBM %-7s n=%u hash=%016llx\n",L[i].name,NIN,(unsigned long long)h);
      }
      free(g); }
    /* ---- DEN: gradual underflow ---- */
    { u64 m[3],t[3]; den_thread(m); pthread_t th; pthread_create(&th,0,den_thread,t); pthread_join(th,0);
      printf("DEN main-thread  min-normal*0.5=%08llx (want 00400000) dbl=%016llx (want 0008000000000000) 3ulp+3ulp=%08llx (want 00000006)\n",(unsigned long long)m[0],(unsigned long long)m[1],(unsigned long long)m[2]);
      printf("DEN new-thread   min-normal*0.5=%08llx (want 00400000) dbl=%016llx (want 0008000000000000) 3ulp+3ulp=%08llx (want 00000006)\n",(unsigned long long)t[0],(unsigned long long)t[1],(unsigned long long)t[2]);
#if defined(__aarch64__)
      { u64 fpcr; __asm__ __volatile__("mrs %0, fpcr":"=r"(fpcr)); printf("HOST FPCR=%08llx (FZ bit 24 = %llu, RMode bits 23:22 = %llu, DN bit 25 = %llu)\n",(unsigned long long)fpcr,(unsigned long long)((fpcr>>24)&1),(unsigned long long)((fpcr>>22)&3),(unsigned long long)((fpcr>>25)&1)); }
#elif defined(__x86_64__)
      printf("HOST MXCSR=%08x (FTZ bit 15, DAZ bit 6, RC bits 14:13)\n",_mm_getcsr());
#endif
    }
    /* ---- CONV: out-of-range float -> int through the policy helpers ---- */
    { static const double v[]={2147483647.5,2147483648.0,-2147483648.0,-2147483649.0,1e30,-1e30,0.5,-0.5,1.5,2.5,-1.5,-2.5};
      for(unsigned i=0;i<sizeof v/sizeof*v;i++) printf("CONV %g: f2i32 rn=%08x trunc=%08x f2i64 trunc=%016llx\n",v[i],(u32)f2i32(v[i],0),(u32)f2i32(v[i],3),(unsigned long long)f2i64(v[i]*4294967296.0,3));
      double nan=kh_bf64(0x7ff8000000000000ull), inf=kh_bf64(0x7ff0000000000000ull);
      printf("CONV nan: f2i32=%08x f2i64=%016llx ; inf: f2i32=%08x f2i64=%016llx ; -inf: f2i32=%08x\n",(u32)f2i32(nan,3),(unsigned long long)f2i64(nan,3),(u32)f2i32(inf,3),(unsigned long long)f2i64(inf,3),(u32)f2i32(-inf,0)); }
    if(dump) fclose(dump);
    return 0;
}
