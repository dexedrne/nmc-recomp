/* gxwrap.c : link-time entry probes (ld --wrap) on the GX device boot functions, to report the first GX entry calls with the number of
 * import calls made before them.  Only calls that cross a translation unit are seen; the import-call trace (khnmc_rt.h kh_trace_import) is the
 * authoritative record. */
#include "savestate.h"
u64 kh_trace_count(void);
#define ENTRY(RVA,NAME,RUN) void __real_f_##RVA(KCtx *g); void __wrap_f_##RVA(KCtx *g){ \
    static _Atomic int n; if(atomic_fetch_add(&n,1)==0) fprintf(stderr,"kh: GX ENTRY 0x%s (%s) first call after %llu import calls (this=%llx rdx=%llx r8=%llx)\n",#RVA,NAME,(unsigned long long)kh_trace_count(),(unsigned long long)KH_RCX(g),(unsigned long long)KH_RDX(g),(unsigned long long)KH_R8(g)); RUN; }
#define PROBE(RVA,NAME) ENTRY(RVA,NAME,__real_f_##RVA(g))
PROBE(11d070,"GXDevice.init")
PROBE(11d0a0,"GXDevice.init body: class+window, two game threads")
PROBE(124060,"createDevices")
PROBE(10e860,"adapter enumeration + D3D12CreateDevice")
PROBE(11c150,"GX singleton accessor")
ENTRY(127ed0,"game thread 1 proc (main loop)",if(kh_cut_enabled) kh_cut_game_loop(g,__real_f_127ed0); else __real_f_127ed0(g))
PROBE(129110,"game thread 2 proc (render/present)")
