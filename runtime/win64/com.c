/* com.c : generic COM-object machinery for the null backends (D3D12/DXGI, XAudio2, Steam interfaces ...).
 * A class owns one vtable in guest memory; each slot holds a distinct synthetic host-fn address so the lifted code's
 * `call [vtable+slot*8]` reaches com_thunk through kh_dispatch, which knows the slot from the address. */
#include "khnmc_rt.h"

#define MAXCLS 128
#define MAXSLOTS 160
typedef struct { char name[48]; int nslots, strict, noiunk; KhComHandler h; u64 vtbl; u64 slot_addr[MAXSLOTS]; _Atomic u32 cnt[MAXSLOTS]; int group; } Cls;
static Cls cls[MAXCLS]; static int ncls;
static pthread_mutex_t cm=PTHREAD_MUTEX_INITIALIZER;
static int log_com=-1;
/* reverse map from host-fn index to (class,slot): host fn address = KH_HOSTFN_BASE + 16*idx */
static struct { short c, s; } rmap[65536];
static void com_thunk(KCtx *g){
    u64 d=(g->aux[0]-KH_HOSTFN_BASE)>>4; int c=rmap[d].c,s=rmap[d].s; Cls *k=&cls[c]; u64 obj=KH_RCX(g);
    if(log_com<0){ const char *e=getenv("KH_LOG_COM"); log_com=e&&*e=="1"[0]; }
    if(log_com) fprintf(stderr,"com %s#%d this=%llx a1=%llx a2=%llx a3=%llx\n",k->name,s,(unsigned long long)obj,(unsigned long long)KH_RDX(g),(unsigned long long)KH_R8(g),(unsigned long long)KH_R9(g));
    k->cnt[s]++; switch(k->group){ case 1: kh_cnt_steam_calls++; break; case 2: kh_cnt_audio_calls++; break; case 3: kh_cnt_input_calls++; break; case 4: kh_cnt_mf_calls++; break; default: kh_cnt_gfx_calls++; }
    if(k->h && k->h(g,obj,s,k->name)){ KH_RET(g); return; }
    if(!k->noiunk && (s==0||s==1||s==2)){                                    /* IUnknown defaults: QueryInterface (returns the same object), AddRef, Release */
        if(s==0){ if(KH_R8(g)){ wr64(g,KH_R8(g),obj); wr64(g,obj+16,rd64(g,obj+16)+1); } SET_RAX(g,0); }
        else if(s==1){ u64 r=rd64(g,obj+16)+1; wr64(g,obj+16,r); SET_RAX(g,r); }
        else { u64 r=rd64(g,obj+16); if(r) r--; wr64(g,obj+16,r); SET_RAX(g,r); }
        KH_RET(g); return;
    }
    if(k->strict) kh_fatal(g,"unimplemented COM method %s slot %d (vtbl+0x%x) called (this=%llx)",k->name,s,s*8,(unsigned long long)obj);
    kh_log("COM %s slot %d (default 0)",k->name,s); SET_RAX(g,0); KH_RET(g);
}
int kh_com_class(const char *name,int nslots,int flags,KhComHandler h){
    pthread_mutex_lock(&cm);
    for(int i=0;i<ncls;i++) if(!strcmp(cls[i].name,name)){ pthread_mutex_unlock(&cm); return i; }
    if(ncls>=MAXCLS||nslots>MAXSLOTS){ fprintf(stderr,"kh: com class table full\n"); abort(); }
    int id=ncls++; Cls *k=&cls[id]; snprintf(k->name,sizeof k->name,"%s",name); k->nslots=nslots; k->strict=flags&1; k->noiunk=(flags&2)!=0; k->h=h; k->group=!strncmp(name,"steam:",6)?1:!strncmp(name,"xa2:",4)?2:(!strncmp(name,"di:",3)||!strncmp(name,"xi:",3))?3:(!strncmp(name,"mf:",3)||!strncmp(name,"d3d11:",6))?4:0;
    k->vtbl=ga_alloc(8ull*(u64)nslots,16);
    for(int s=0;s<nslots;s++){ char nm[96]; snprintf(nm,sizeof nm,"com:%s#%d",name,s); u64 a=kh_host_fn(nm,com_thunk); k->slot_addr[s]=a; u64 idx=(a-KH_HOSTFN_BASE)>>4; rmap[idx].c=(short)id; rmap[idx].s=(short)s; *(u64*)(k->vtbl+gs_bias+8ull*(u64)s)=a; }
    pthread_mutex_unlock(&cm); return id;
}
u64 kh_com_obj(int c,u64 extra){ u64 o=ga_alloc(32+extra,16); memset((void*)(o+gs_bias),0,32+extra); *(u64*)(o+gs_bias)=cls[c].vtbl; *(u64*)(o+gs_bias+8)=(u64)c; *(u64*)(o+gs_bias+16)=1; return o; }
int kh_com_cls_of(KCtx *g,u64 obj){ return (int)rd64(g,obj+8); }
u64 kh_com_vtbl(int c){ return cls[c].vtbl; }
const char *kh_com_iface(KCtx *g,u64 obj){ return cls[rd64(g,obj+8)].name; }

void kh_com_dump(FILE *f){
    for(int i=0;i<ncls;i++){ u64 tot=0; for(int s=0;s<cls[i].nslots;s++) tot+=cls[i].cnt[s]; if(!tot) continue;
        fprintf(f,"kh: com %-28s calls=%llu  slots:",cls[i].name,(unsigned long long)tot); int shown=0; for(int s=0;s<cls[i].nslots&&shown<14;s++) if(cls[i].cnt[s]){ fprintf(f," #%d=%u",s,cls[i].cnt[s]); shown++; } fputc('\n',f); }
}
