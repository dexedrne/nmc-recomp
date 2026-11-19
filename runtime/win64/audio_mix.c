/* audio_mix.c : software XAudio2-style mixer core.  See khnmc_mix.h for the model.  Host code only: guest memory, guest callbacks and
 * guest XAPOs reach this file through the KmxHost function table (null_audio.c provides it), the tests use plain host callbacks. */
#define _GNU_SOURCE
#include "khnmc_mix.h"
#include "kh_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct { uint32_t flags, bytes; const uint8_t *data; uint64_t ctx; uint32_t frames, play_begin, play_end, loop_begin, loop_end, loop_count, cursor; int begun; } QBuf;
typedef struct { KmxVoice *dest; uint32_t flags; float *matrix; uint32_t msrc, mdst; float *prev; uint32_t nprev; } Send;
typedef struct { int kind; void *user; uint32_t in_ch, out_ch; int enabled, inplace; float peak[KMX_MAXCH], rms[KMX_MAXCH]; } Fx;

/* a deferred call (deferred mode): applied by the pass thread before the first pass whose instant is later than `stamp` */
enum { OP_START = 1, OP_STOP, OP_SUBMIT, OP_FLUSH, OP_DISC, OP_EXITLOOP, OP_VOLUME, OP_CHANVOL, OP_MATRIX, OP_RATIO, OP_RATE, OP_FXEN, OP_FXPARAMS, OP_PUBLISH };
typedef struct { int kind; KmxVoice *v, *dest; uint64_t stamp; uint32_t a, b; float f; float *vals; void *blob; QBuf q; } Op;

struct KmxVoice {
    KmxEngine *e; int kind; uint32_t id; void *user; int has_cb;
    int dead, busy, closing, hidden;
    uint32_t flags, stage;
    uint32_t in_ch, in_rate, in_frames;          /* source: channels / rate of the queued data; submix, master: the input mix */
    uint32_t out_rate, out_frames;               /* processing rate = input rate of the destinations */
    uint32_t out_ch, max_ch;                     /* channels after the effect chain; widest stage */
    float volume, prev_volume, chan_vol[KMX_MAXCH], prev_chan_vol[KMX_MAXCH];
    Send *sends; uint32_t nsends; int default_out;
    Fx *fx; uint32_t nfx;
    float *mix, *a, *b;
    float *win; uint64_t frac; int fill, fresh;  /* source: 4 frames (previous, current, 2 look-ahead); submix: 3 frames of history */
    KmxFormat fmt; QBuf q[KMX_MAX_QUEUED]; uint32_t qn;
    int started, tails, tail_quiet, starving, stream_open; uint32_t tail_left, uncounted;
    float ratio, max_ratio; uint64_t samples_played;
    uint64_t starve_count, starve_first, starve_last;      /* episodes; engine pass numbers of the first and last */
    uint64_t *pend; uint32_t npend, cpend;
    int16_t *ad_cache; const uint8_t *ad_data; int64_t ad_block; uint32_t ad_frames;
    float peak_stage[10]; uint32_t nstage;         /* TASK28 level analysis (kmx_engine_set_peaks): |x| peak at the chain input and after each effect, last pass */
};
struct KmxEngine {
    KmxHost host; void *user; uint32_t rate, quantum;
    pthread_mutex_t m; pthread_cond_t cv;
    KmxVoice **voices; uint32_t nvoices, cvoices, next_id;
    KmxVoice *master;
    int running, callbacks, ecb_busy, in_pass; pthread_t pass_thread;
    KmxVoice **snap; uint32_t csnap;
    KmxVoice **grave; uint32_t ngrave, cgrave;
    float *out; size_t cout;                     /* copy of the last master output (the voice may be destroyed by another thread) */
    int deferred; Op *ops; uint32_t ophead, nops, cops;   /* deferred calls: ops[ophead .. nops) in call (= stamp) order */
    int peaks;                                   /* level analysis on (kmx_engine_set_peaks) */
    KmxStats st;
};

const int16_t kmx_adpcm_std_coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};

/* ---- memory ---------------------------------------------------------------------------------------------------------------------- */
static void *def_alloc(size_t n){ void *p = NULL; if(posix_memalign(&p, 64, n ? (n + 63) & ~(size_t)63 : 64)) return NULL; memset(p, 0, n); return p; }
static void def_release(void *p){ free(p); }
static float *falloc(KmxEngine *e, size_t nfloats){
    float *p = e->host.alloc((nfloats ? nfloats : 1) * sizeof(float));
    if(!p){ fprintf(stderr, "kmx: FATAL: mixer buffer allocation of %zu floats failed\n", nfloats); abort(); }
    return p;
}
static void *xcalloc(size_t n, size_t s){ void *p = calloc(n ? n : 1, s); if(!p){ fprintf(stderr, "kmx: FATAL: out of host memory\n"); abort(); } return p; }
#define LOCK(e) pthread_mutex_lock(&(e)->m)
#define UNLOCK(e) pthread_mutex_unlock(&(e)->m)
static int on_pass_thread(KmxEngine *e){ return e->in_pass && pthread_equal(e->pass_thread, pthread_self()); }
/* deferred mode queues a call, except one made by the pass thread itself from inside a pass (a callback): that one applies at once, as in
 * XAudio2 (a buffer submitted from OnVoiceProcessingPassStart is played in that very pass), and its moment in the pass is deterministic */
#ifdef KH_M28_IMMEDIATE_MUTANT                     /* mutation check of the selftest only: deferred mode applies every call at once */
static int defer(KmxEngine *e){ (void)e; return 0; }
#else
static int defer(KmxEngine *e){ return e->deferred && !on_pass_thread(e); }
#endif

/* ---- MS-ADPCM ------------------------------------------------------------------------------------------------------------------- */
static inline int clamp16(int v){ return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int rds16(const uint8_t *p){ return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }
/* One block: per channel a predictor index byte, then delta, sample1, sample2 as little-endian s16 (channel interleaved per field),
 * then 4-bit codes, high nibble first, channel interleaved.  The predictor divides by 256 with truncation toward zero (Microsoft's
 * reference and ffmpeg do; an arithmetic shift drifts on low-level signals).  Returns the frames written, 0 on a malformed block. */
int kmx_adpcm_decode_block(const uint8_t *b, uint32_t bytes, uint32_t ch, const int16_t coef[][2], uint32_t ncoef, int16_t *out, uint32_t cap){
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    if(ch < 1 || ch > 2 || bytes < 7 * ch) return 0;
    int pred[2], delta[2], s1[2], s2[2];
    for(uint32_t c = 0; c < ch; c++){
        pred[c] = b[c]; if((uint32_t)pred[c] >= ncoef) return 0;
        delta[c] = rds16(b + ch + 2 * c); s1[c] = rds16(b + 3 * ch + 2 * c); s2[c] = rds16(b + 5 * ch + 2 * c);
    }
    uint32_t nibbles = (bytes - 7 * ch) * 2, frames = 2 + nibbles / ch;
    if(frames > cap) return 0;
    for(uint32_t c = 0; c < ch; c++){ out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    const uint8_t *p = b + 7 * ch; uint32_t o = 2;
    for(uint32_t n = 0; n + ch <= nibbles; n += ch, o++)
        for(uint32_t c = 0; c < ch; c++){
            uint32_t i = n + c; int v = p[i >> 1], nib = (i & 1) ? (v & 15) : (v >> 4), snib = nib >= 8 ? nib - 16 : nib;
            int s = clamp16((s1[c] * coef[pred[c]][0] + s2[c] * coef[pred[c]][1]) / 256 + snib * delta[c]);
            out[o * ch + c] = (int16_t)s; s2[c] = s1[c]; s1[c] = s;
            int d = (adapt[nib] * delta[c]) >> 8; delta[c] = d < 16 ? 16 : d;
        }
    return (int)o;
}
uint32_t kmx_buffer_frames(const KmxFormat *f, uint32_t bytes){
    if(!f->block_align) return 0;
    if(f->tag != KMX_TAG_ADPCM) return bytes / f->block_align;
    uint32_t full = bytes / f->block_align, rem = bytes % f->block_align, n = full * f->samples_per_block;
    if(rem >= 7 * f->channels) n += 2 + (rem - 7 * f->channels) * 2 / f->channels;
    return n;
}

/* ---- engine --------------------------------------------------------------------------------------------------------------------- */
KmxEngine *kmx_engine_create(const KmxHost *host, void *user, uint32_t rate){
    if(!rate || rate % 100) return NULL;
    KmxEngine *e = xcalloc(1, sizeof *e);
    if(host) e->host = *host;
    if(!e->host.alloc || !e->host.release){ e->host.alloc = def_alloc; e->host.release = def_release; }
    e->user = user; e->rate = rate; e->quantum = rate / 100; e->next_id = 1;
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&e->m, &at); pthread_mutexattr_destroy(&at); pthread_cond_init(&e->cv, NULL);
    return e;
}
uint32_t kmx_engine_rate(KmxEngine *e){ return e->rate; }
uint32_t kmx_engine_quantum(KmxEngine *e){ return e->quantum; }
void kmx_engine_start(KmxEngine *e){ LOCK(e); e->running = 1; UNLOCK(e); }
void kmx_engine_stop(KmxEngine *e){ LOCK(e); e->running = 0; UNLOCK(e); }
int kmx_engine_running(KmxEngine *e){ LOCK(e); int r = e->running; UNLOCK(e); return r; }
void kmx_engine_set_callbacks(KmxEngine *e, int on){
    LOCK(e); e->callbacks = on;
    if(!on && !on_pass_thread(e)) while(e->ecb_busy) pthread_cond_wait(&e->cv, &e->m);
    UNLOCK(e);
}
void kmx_engine_stats(KmxEngine *e, KmxStats *out){
    LOCK(e); *out = e->st; uint32_t act = 0;
    for(uint32_t i = 0; i < e->nvoices; i++) if(e->voices[i]->kind == KMX_SOURCE && e->voices[i]->started) act++;
    out->active_source = act; UNLOCK(e);
}

/* ---- voices: creation, graph ------------------------------------------------------------------------------------------------------ */
static void free_send(Send *s){ free(s->matrix); free(s->prev); }
static void free_voice(KmxVoice *v){
    KmxEngine *e = v->e;
    if(e->host.voice_free) e->host.voice_free(v->user);
    for(uint32_t i = 0; i < v->nsends; i++) free_send(&v->sends[i]);
    free(v->sends); free(v->fx); free(v->pend); free(v->ad_cache); free(v->win);
    if(v->mix) e->host.release(v->mix); if(v->a) e->host.release(v->a); if(v->b) e->host.release(v->b);
    free(v);
}
static void reset_src(KmxVoice *v){
    if(v->win) memset(v->win, 0, sizeof(float) * 4 * v->in_ch);
    v->frac = 0; v->fill = 0; v->fresh = 1; v->uncounted = 0; v->ad_data = NULL; v->ad_block = -1;
}
static void realloc_proc(KmxVoice *v){            /* (re)allocate the processing buffers for out_frames x max_ch */
    KmxEngine *e = v->e; uint32_t mc = v->in_ch;
    for(uint32_t i = 0; i < v->nfx; i++) if(v->fx[i].out_ch > mc) mc = v->fx[i].out_ch;
    v->max_ch = mc; v->out_ch = v->nfx ? v->fx[v->nfx - 1].out_ch : v->in_ch;
    if(v->a) e->host.release(v->a); if(v->b) e->host.release(v->b);
    v->a = falloc(e, (size_t)v->out_frames * mc); v->b = falloc(e, (size_t)v->out_frames * mc);
}
static KmxVoice *new_voice(KmxEngine *e, int kind, uint32_t ch, uint32_t rate, uint32_t flags, void *user){
    KmxVoice *v = xcalloc(1, sizeof *v);
    v->e = e; v->kind = kind; v->id = e->next_id++; v->user = user; v->flags = flags & ~KMX_VOICE_HIDDEN; v->hidden = (flags & KMX_VOICE_HIDDEN) != 0;
    v->in_ch = v->out_ch = v->max_ch = ch; v->in_rate = rate; v->in_frames = rate / 100;
    v->out_rate = kind == KMX_MASTER ? rate : e->rate; v->out_frames = v->out_rate / 100;
    v->volume = v->prev_volume = 1.0f; for(uint32_t i = 0; i < KMX_MAXCH; i++) v->chan_vol[i] = v->prev_chan_vol[i] = 1.0f;
    v->ratio = 1.0f; v->max_ratio = 2.0f; v->ad_block = -1; v->fresh = 1;
    v->win = xcalloc(4 * (size_t)ch, sizeof(float));
    if(kind != KMX_SOURCE) v->mix = falloc(e, (size_t)v->in_frames * ch);
    realloc_proc(v);
    if(e->nvoices == e->cvoices){ e->cvoices = e->cvoices ? e->cvoices * 2 : 32; e->voices = realloc(e->voices, e->cvoices * sizeof *e->voices); if(!e->voices) abort(); }
    e->voices[e->nvoices++] = v;
    return v;
}
static Send *find_send(KmxVoice *v, KmxVoice *dest){
    if(!dest && v->nsends == 1) return &v->sends[0];               /* NULL = the single destination */
    for(uint32_t i = 0; i < v->nsends; i++) if(v->sends[i].dest == dest) return &v->sends[i];
    return NULL;
}
/* returns KMX_OK or KMX_E_UNSUPPORTED (called from the pass thread while this voice is being processed) / KMX_E_TIMEOUT */
static int wait_idle(KmxVoice *v){
    KmxEngine *e = v->e;
    if(!v->busy) return KMX_OK;
    if(on_pass_thread(e)) return KMX_E_UNSUPPORTED;
    unsigned stuck = 0;                                              /* 10 slices (kh_compat.h): a stopped process loses one slice, not the budget */
    while(v->busy) if(kh_cond_wait_stuck_rt(&e->cv, &e->m, &stuck, 10) == ETIMEDOUT) return KMX_E_TIMEOUT;
    return KMX_OK;
}
static void op_drop_voice(KmxEngine *e, KmxVoice *v);
static int has_host_fx(KmxVoice *v){ for(uint32_t i = 0; i < v->nfx; i++) if(v->fx[i].kind == KMX_FX_HOST) return 1; return 0; }

int kmx_voice_set_outputs(KmxVoice *v, KmxVoice *const *dests, const uint32_t *sflags, uint32_t n, int use_default){
    KmxEngine *e = v->e; LOCK(e);
    int rc = KMX_OK; Send *ns = NULL;
    if(v->kind == KMX_MASTER || v->dead){ rc = KMX_E_INVALID; goto out; }
    KmxVoice *one[1]; uint32_t zero = 0;
    if(use_default){ if(e->master){ one[0] = e->master; dests = one; sflags = &zero; n = 1; } else n = 0; }
    uint32_t rate = 0;
    for(uint32_t i = 0; i < n; i++){
        KmxVoice *d = dests[i];
        if(!d || d->e != e || d == v || d->dead || d->kind == KMX_SOURCE){ rc = KMX_E_INVALID; goto out; }
        if(v->kind == KMX_SUBMIX && d->kind == KMX_SUBMIX && d->stage <= v->stage){ rc = KMX_E_INVALID; goto out; }   /* XAudio2: only to a later stage */
        for(uint32_t k = 0; k < i; k++) if(dests[k] == d){ rc = KMX_E_INVALID; goto out; }
        if(rate && d->in_rate != rate){ rc = KMX_E_INVALID; goto out; }                                               /* all destinations share one rate */
        rate = d->in_rate;
    }
    if(rate && rate != v->out_rate && has_host_fx(v)){ rc = KMX_E_UNSUPPORTED; goto out; }                           /* would need the XAPOs re-locked */
    if((rc = wait_idle(v)) != KMX_OK) goto out;
    ns = xcalloc(n, sizeof *ns);
    for(uint32_t i = 0; i < n; i++){
        ns[i].dest = dests[i]; ns[i].flags = sflags ? sflags[i] : 0;
        Send *o = NULL; for(uint32_t k = 0; k < v->nsends; k++) if(v->sends[k].dest == dests[i]) o = &v->sends[k];
        if(o){ ns[i].matrix = o->matrix; ns[i].msrc = o->msrc; ns[i].mdst = o->mdst; ns[i].prev = o->prev; ns[i].nprev = o->nprev; o->matrix = NULL; o->prev = NULL; }
    }
    for(uint32_t k = 0; k < v->nsends; k++) free_send(&v->sends[k]);
    free(v->sends); v->sends = ns; v->nsends = n; v->default_out = use_default;
    if(rate && rate != v->out_rate){ v->out_rate = rate; v->out_frames = rate / 100; realloc_proc(v); if(v->kind == KMX_SOURCE) reset_src(v); else memset(v->win, 0, sizeof(float) * 4 * v->in_ch); }
out:
    UNLOCK(e); return rc;
}
static int check_rate(uint32_t r){ return r >= 1000 && r <= 200000; }
int kmx_voice_create_source(KmxEngine *e, const KmxFormat *f, uint32_t flags, float max_ratio, void *user, int has_cb, KmxVoice **out){
    if(!f || !out || f->channels < 1 || f->channels > KMX_MAXCH || !check_rate(f->rate)) return KMX_E_INVALID;
    if(!(max_ratio >= 1.0f / 1024.0f && max_ratio <= 1024.0f)) return KMX_E_INVALID;
    if(f->tag == KMX_TAG_PCM){ if((f->bits != 8 && f->bits != 16 && f->bits != 24 && f->bits != 32) || f->block_align != f->channels * f->bits / 8) return KMX_E_INVALID; }
    else if(f->tag == KMX_TAG_FLOAT){ if(f->bits != 32 || f->block_align != f->channels * 4) return KMX_E_INVALID; }
    else if(f->tag == KMX_TAG_ADPCM){
        if(f->channels > 2) return KMX_E_UNSUPPORTED;
        if(f->ncoef < 7 || f->ncoef > 32 || f->block_align < 7 * f->channels + 1) return KMX_E_INVALID;
        if(f->samples_per_block != (f->block_align - 7 * f->channels) * 2 / f->channels + 2) return KMX_E_INVALID;
    } else return KMX_E_UNSUPPORTED;
    LOCK(e);
    KmxVoice *v = new_voice(e, KMX_SOURCE, f->channels, f->rate, flags, user);
    v->fmt = *f; v->has_cb = has_cb; v->max_ratio = max_ratio;
    if(f->tag == KMX_TAG_ADPCM) v->ad_cache = xcalloc((size_t)f->samples_per_block * f->channels, sizeof(int16_t));
    e->st.source_voices_created++; if(++e->st.live_source > e->st.peak_live_source) e->st.peak_live_source = e->st.live_source;
    *out = v; UNLOCK(e); return KMX_OK;
}
int kmx_voice_create_submix(KmxEngine *e, uint32_t ch, uint32_t rate, uint32_t flags, uint32_t stage, void *user, KmxVoice **out){
    if(!out || ch < 1 || ch > KMX_MAXCH || !check_rate(rate) || rate % 100) return KMX_E_INVALID;
    LOCK(e); KmxVoice *v = new_voice(e, KMX_SUBMIX, ch, rate, flags, user); v->stage = stage;
    e->st.submix_voices_created++; e->st.live_submix++; *out = v; UNLOCK(e); return KMX_OK;
}
int kmx_voice_create_master(KmxEngine *e, uint32_t ch, uint32_t rate, uint32_t flags, void *user, KmxVoice **out){
    if(!out || ch < 1 || ch > KMX_MAXCH) return KMX_E_INVALID;
    LOCK(e);
    if(e->master){ UNLOCK(e); return KMX_E_INVALID; }               /* XAudio2 2.8+: one mastering voice per engine */
    if(rate != e->rate){ UNLOCK(e); return KMX_E_UNSUPPORTED; }     /* the device rate is the engine rate: no master-rate conversion */
    KmxVoice *v = new_voice(e, KMX_MASTER, ch, rate, flags, user); e->master = v;
    e->st.master_voices_created++; e->st.live_master++;
    for(uint32_t i = 0; i < e->nvoices; i++){                       /* voices created with the default send list before the master existed */
        KmxVoice *o = e->voices[i]; if(o != v && o->kind != KMX_MASTER && o->default_out && !o->nsends) kmx_voice_set_outputs(o, NULL, NULL, 0, 1);
    }
    *out = v; UNLOCK(e); return KMX_OK;
}
int kmx_voice_quiesce(KmxVoice *v){
    KmxEngine *e = v->e; LOCK(e); v->closing = 1; int rc = KMX_OK;
    if(v->busy && !on_pass_thread(e)) rc = wait_idle(v);
    UNLOCK(e); return rc;
}
void kmx_voice_destroy(KmxVoice *v){
    KmxEngine *e = v->e; LOCK(e);
    if(v->dead){ UNLOCK(e); return; }
    v->closing = 1;
    if(v->busy && !on_pass_thread(e)) wait_idle(v);                 /* XAudio2's DestroyVoice also waits for the audio thread */
    v->dead = 1;
    op_drop_voice(e, v);
    for(uint32_t i = 0; i < e->nvoices; i++){
        KmxVoice *o = e->voices[i];
        for(uint32_t k = 0; k < o->nsends;) if(o->sends[k].dest == v){ free_send(&o->sends[k]); memmove(&o->sends[k], &o->sends[k + 1], (o->nsends - k - 1) * sizeof(Send)); o->nsends--; } else k++;
    }
    for(uint32_t i = 0; i < e->nvoices; i++) if(e->voices[i] == v){ memmove(&e->voices[i], &e->voices[i + 1], (e->nvoices - i - 1) * sizeof *e->voices); e->nvoices--; break; }
    if(e->master == v) e->master = NULL;
    if(v->kind == KMX_SOURCE) e->st.live_source--; else if(v->kind == KMX_SUBMIX) e->st.live_submix--; else e->st.live_master--;
    e->st.voices_destroyed++;
    if(e->in_pass){                                                 /* the pass still holds the pointer in its snapshot */
        if(e->ngrave == e->cgrave){ e->cgrave = e->cgrave ? e->cgrave * 2 : 16; e->grave = realloc(e->grave, e->cgrave * sizeof *e->grave); if(!e->grave) abort(); }
        e->grave[e->ngrave++] = v;
    } else free_voice(v);
    UNLOCK(e);
}
void kmx_engine_destroy(KmxEngine *e){
    LOCK(e);
    while(e->nvoices){ KmxVoice *v = e->voices[e->nvoices - 1]; kmx_voice_destroy(v); }
    for(uint32_t i = 0; i < e->ngrave; i++) free_voice(e->grave[i]);
    UNLOCK(e);
    free(e->voices); free(e->snap); free(e->grave); free(e->out); free(e->ops); pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->cv); free(e);
}

/* ---- deferred calls ------------------------------------------------------------------------------------------------------------- */
static float default_level(uint32_t S, uint32_t D, uint32_t d, uint32_t s, int *ok);
void kmx_engine_set_peaks(KmxEngine *e, int on){ LOCK(e); e->peaks = !!on; UNLOCK(e); }
/* the last pass, for the level analysis: every published voice that carried signal, with what scales it on its way to the master */
void kmx_engine_peak_dump(KmxEngine *e, FILE *f, const char *(*name)(void *voice_user)){
    LOCK(e);
    for(uint32_t i = 0; i < e->nvoices; i++){ KmxVoice *v = e->voices[i]; if(v->hidden || v->dead || !v->nstage || v->peak_stage[0] == 0.0f) continue;
        if(v->kind == KMX_SOURCE && !v->started) continue;
        fprintf(f, "  %s %u%s%s in %uch@%u: peak in=%.3f", v->kind == KMX_SOURCE ? "source" : v->kind == KMX_SUBMIX ? "submix" : "MASTER", v->id, name ? " " : "", name ? name(v->user) : "", v->in_ch, v->in_rate, (double)v->peak_stage[0]);
        for(uint32_t k = 1; k < v->nstage; k++) fprintf(f, " fx%u=%.3f", k - 1, (double)v->peak_stage[k]);
        fprintf(f, " vol=%.3f", (double)v->volume);
        if(v->kind == KMX_SOURCE) fprintf(f, " ratio=%.3f", (double)v->ratio);
        for(uint32_t si = 0; si < v->nsends; si++){ Send *sd = &v->sends[si]; uint32_t S = v->out_ch, D = sd->dest->in_ch; float mx = 0.0f;
            for(uint32_t d = 0; d < D; d++){ float row = 0.0f; for(uint32_t c = 0; c < S; c++){ int ok; float m = (sd->matrix && sd->msrc == S && sd->mdst == D) ? sd->matrix[d * S + c] : default_level(S, D, d, c, &ok); row += fabsf(v->chan_vol[c] * m); } if(row > mx) mx = row; }
            fprintf(f, " -> %s %u (largest row sum of |chanvol*matrix| %.3f)", sd->dest->kind == KMX_MASTER ? "master" : "submix", sd->dest->id, (double)mx); }
        if(v->kind == KMX_MASTER){ float cm = 0.0f; for(uint32_t c = 0; c < v->out_ch; c++) if(v->chan_vol[c] > cm) cm = v->chan_vol[c]; fprintf(f, " largest channel volume %.3f", (double)cm); }
        fputc('\n', f);
    }
    UNLOCK(e);
}
void kmx_engine_set_deferred(KmxEngine *e, int on){ LOCK(e); e->deferred = !!on; UNLOCK(e); }   /* off: new calls apply at once; queued ones still apply at their pass */
int kmx_engine_deferred(KmxEngine *e){ LOCK(e); int r = e->deferred; UNLOCK(e); return r; }
uint32_t kmx_engine_pending(KmxEngine *e){ LOCK(e); uint32_t r = e->nops - e->ophead; UNLOCK(e); return r; }
static void op_free(Op *o){ free(o->vals); free(o->blob); o->vals = NULL; o->blob = NULL; }
/* caller holds the lock; the stamp is read under it, so the queue is in stamp order */
static Op *op_push(KmxEngine *e, int kind, KmxVoice *v){
    if(e->ophead && e->ophead * 2 >= e->nops){ memmove(e->ops, e->ops + e->ophead, (e->nops - e->ophead) * sizeof *e->ops); e->nops -= e->ophead; e->ophead = 0; }
    if(e->nops == e->cops){ e->cops = e->cops ? e->cops * 2 : 64; e->ops = realloc(e->ops, e->cops * sizeof *e->ops); if(!e->ops) abort(); }
    Op *o = &e->ops[e->nops++]; memset(o, 0, sizeof *o);
    o->kind = kind; o->v = v; o->stamp = e->host.now ? e->host.now(e->user) : 0;
    e->st.deferred_ops++; return o;
}
/* the latest queued call of `kind` on v (dest / index a must match where they matter), or NULL: what a getter returns in deferred mode */
static const Op *op_latest(KmxEngine *e, KmxVoice *v, int kind, KmxVoice *dest, int match_a, uint32_t a){
    for(uint32_t i = e->nops; i-- > e->ophead;){ const Op *o = &e->ops[i];
        if(o->v == v && o->kind == kind && (kind != OP_MATRIX || o->dest == dest) && (!match_a || o->a == a)) return o; }
    return NULL;
}
/* destroy: the voice's queued calls (and matrices that name it as the destination) go with it, without callbacks */
static void op_drop_voice(KmxEngine *e, KmxVoice *v){
    uint32_t w = e->ophead;
    for(uint32_t i = e->ophead; i < e->nops; i++){
        if(e->ops[i].v == v || e->ops[i].dest == v){ op_free(&e->ops[i]); e->st.deferred_dropped++; continue; }
        if(w != i) e->ops[w] = e->ops[i];
        w++;
    }
    e->nops = w;
}
/* review of task 28: SetEffectChain applies at once (not deferred).  Calls of this voice still queued were made BEFORE it: EnableEffect /
 * SetEffectParameters address an effect of the OLD chain by index, and (when the output width changed) SetChannelVolumes /
 * SetOutputMatrix the old width, which the new chain resets.  In XAudio2's call order they went to the old chain or were reset with it;
 * applied later they would reach whatever the new chain holds at that index (another XAPO, a parameter blob of another layout).  They
 * are dropped here and counted (deferred_superseded).  Caller holds the lock. */
static void op_drop_superseded(KmxEngine *e, KmxVoice *v, int levels){
    uint32_t w = e->ophead;
    for(uint32_t i = e->ophead; i < e->nops; i++){ int k = e->ops[i].kind;
        if(e->ops[i].v == v && (k == OP_FXEN || k == OP_FXPARAMS || (levels && (k == OP_CHANVOL || k == OP_MATRIX)))){ op_free(&e->ops[i]); e->st.deferred_superseded++; continue; }
        if(w != i) e->ops[w] = e->ops[i];
        w++;
    }
    e->nops = w;
}
/* buffers queued on a source voice as its caller sees them: the applied queue changed by the queued submits and flushes */
static uint32_t front_queued(KmxEngine *e, KmxVoice *v, uint64_t *ctx){
    uint32_t n = v->qn; uint64_t c = v->qn ? v->q[0].ctx : 0;
    for(uint32_t i = e->ophead; i < e->nops; i++){ const Op *o = &e->ops[i]; if(o->v != v) continue;
        if(o->kind == OP_SUBMIT){ if(!n) c = o->q.ctx; n++; }
        else if(o->kind == OP_FLUSH){ uint32_t keep = (v->started && v->qn && v->q[0].begun) ? 1 : 0; if(n > keep) n = keep; if(!n) c = 0; } }
    if(ctx) *ctx = c; return n;
}

int kmx_voice_set_chain(KmxVoice *v, const KmxFxDesc *d, uint32_t n){
    KmxEngine *e = v->e; LOCK(e); int rc = KMX_OK; Fx *nf = NULL;
    if(v->dead || n > 64){ rc = KMX_E_INVALID; goto out; }
    uint32_t ch = v->in_ch;
    nf = xcalloc(n, sizeof *nf);
    for(uint32_t i = 0; i < n; i++){
        uint32_t oc = d[i].out_ch ? d[i].out_ch : ch;
        if(oc < 1 || oc > KMX_MAXCH || (d[i].kind != KMX_FX_METER && d[i].kind != KMX_FX_PASS && d[i].kind != KMX_FX_HOST)){ rc = KMX_E_INVALID; goto out; }
        if(d[i].kind != KMX_FX_HOST && oc != ch){ rc = KMX_E_UNSUPPORTED; goto out; }      /* a meter / pass-through cannot convert channels */
        nf[i].kind = d[i].kind; nf[i].user = d[i].user; nf[i].in_ch = ch; nf[i].out_ch = oc; nf[i].enabled = d[i].enabled; nf[i].inplace = d[i].inplace && oc == ch;
        ch = oc;
    }
    if((rc = wait_idle(v)) != KMX_OK) goto out;
    uint32_t old_out = v->out_ch;
    free(v->fx); v->fx = nf; v->nfx = n; nf = NULL; realloc_proc(v);
    op_drop_superseded(e, v, v->out_ch != old_out);
    if(v->out_ch != old_out){                                        /* output width changed: levels and matrices return to their defaults */
        for(uint32_t i = 0; i < KMX_MAXCH; i++) v->chan_vol[i] = v->prev_chan_vol[i] = 1.0f;
        for(uint32_t k = 0; k < v->nsends; k++){ free(v->sends[k].matrix); free(v->sends[k].prev); v->sends[k].matrix = v->sends[k].prev = NULL; v->sends[k].nprev = 0; }
    }
out:
    free(nf); UNLOCK(e); return rc;
}
void *kmx_voice_fx_user(KmxVoice *v, uint32_t i){ KmxEngine *e = v->e; LOCK(e); void *u = i < v->nfx ? v->fx[i].user : NULL; UNLOCK(e); return u; }
int kmx_voice_fx_enable(KmxVoice *v, uint32_t i, int on){
    KmxEngine *e = v->e; LOCK(e); int rc = i < v->nfx ? KMX_OK : KMX_E_INVALID;
    if(rc == KMX_OK){ if(defer(e)){ Op *o = op_push(e, OP_FXEN, v); o->a = i; o->b = !!on; } else v->fx[i].enabled = !!on; }
    UNLOCK(e); return rc;
}
int kmx_voice_fx_enabled(KmxVoice *v, uint32_t i, int *on){
    KmxEngine *e = v->e; LOCK(e); int rc = i < v->nfx ? KMX_OK : KMX_E_INVALID;
    if(rc == KMX_OK){ const Op *o = op_latest(e, v, OP_FXEN, NULL, 1, i); *on = o ? (int)o->b : v->fx[i].enabled; }
    UNLOCK(e); return rc;
}
/* effect parameters (host fx only): deferred mode queues them for host->fx_params on the pass thread; from the pass thread itself (a callback)
 * they are delivered at once on it.  Without deferred mode the caller delivers them itself: KMX_E_UNSUPPORTED. */
int kmx_voice_fx_params(KmxVoice *v, void *ctx, uint32_t i, const void *blob, uint32_t size){
    KmxEngine *e = v->e; LOCK(e); int rc = (i < v->nfx && v->fx[i].kind == KMX_FX_HOST && e->host.fx_params && blob && size) ? KMX_OK : KMX_E_INVALID;
    if(rc == KMX_OK && !e->deferred) rc = KMX_E_UNSUPPORTED;
    if(rc == KMX_OK){
        if(defer(e)){ Op *o = op_push(e, OP_FXPARAMS, v); o->a = i; o->b = size; o->blob = xcalloc(size, 1); memcpy(o->blob, blob, size); }
        else { void *fu = v->fx[i].user; UNLOCK(e); e->host.fx_params(ctx, fu, blob, size); LOCK(e); }
    }
    UNLOCK(e); return rc;
}
int kmx_voice_publish(KmxVoice *v){
    KmxEngine *e = v->e; LOCK(e); int rc = v->dead ? KMX_E_INVALID : KMX_OK;
    if(rc == KMX_OK && v->hidden){ if(defer(e)) op_push(e, OP_PUBLISH, v); else v->hidden = 0; }
    UNLOCK(e); return rc;
}
int kmx_voice_hidden(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); int r = v->hidden; UNLOCK(e); return r; }
int kmx_voice_meter(KmxVoice *v, uint32_t i, float *peak, float *rms, uint32_t ch){
    KmxEngine *e = v->e; LOCK(e); int rc = KMX_E_INVALID;
    if(i < v->nfx && v->fx[i].kind == KMX_FX_METER && ch == v->fx[i].out_ch){ memcpy(peak, v->fx[i].peak, ch * sizeof(float)); memcpy(rms, v->fx[i].rms, ch * sizeof(float)); rc = KMX_OK; }
    UNLOCK(e); return rc;
}
static int vol_ok(float x){ return x >= -16777216.0f && x <= 16777216.0f; }      /* XAUDIO2_MAX_VOLUME_LEVEL = 2^24 */
int kmx_voice_set_volume(KmxVoice *v, float vol){
    if(!vol_ok(vol)) return KMX_E_INVALID; KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_VOLUME, v)->f = vol; else v->volume = vol;
    UNLOCK(e); return KMX_OK;
}
float kmx_voice_volume(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); const Op *o = op_latest(e, v, OP_VOLUME, NULL, 0, 0); float r = o ? o->f : v->volume; UNLOCK(e); return r; }
int kmx_voice_set_channel_volumes(KmxVoice *v, uint32_t n, const float *vols){
    KmxEngine *e = v->e; LOCK(e); int rc = (vols && n == v->out_ch) ? KMX_OK : KMX_E_INVALID;
    for(uint32_t i = 0; rc == KMX_OK && i < n; i++) if(!vol_ok(vols[i])) rc = KMX_E_INVALID;
    if(rc == KMX_OK){
        if(defer(e)){ Op *o = op_push(e, OP_CHANVOL, v); o->a = n; o->vals = xcalloc(n, sizeof(float)); memcpy(o->vals, vols, n * sizeof(float)); }
        else memcpy(v->chan_vol, vols, n * sizeof(float));
    }
    UNLOCK(e); return rc;
}
int kmx_voice_channel_volumes(KmxVoice *v, uint32_t n, float *vols){
    KmxEngine *e = v->e; LOCK(e); int rc = (vols && n == v->out_ch) ? KMX_OK : KMX_E_INVALID;
    if(rc == KMX_OK){ const Op *o = op_latest(e, v, OP_CHANVOL, NULL, 1, n); memcpy(vols, o ? o->vals : v->chan_vol, n * sizeof(float)); }
    UNLOCK(e); return rc;
}
/* XAudio2 default levels, only the shapes that need no down-mix table: identity, and "first channels" for an up-mix (mono feeds
 * front left and right).  Anything else has no default here: *ok = 0 and the caller fails loudly. */
static float default_level(uint32_t S, uint32_t D, uint32_t d, uint32_t s, int *ok){
    *ok = 1;
    if(S == D) return s == d ? 1.0f : 0.0f;
    if(S == 1 && D >= 2) return d < 2 ? 1.0f : 0.0f;
    if(S == 2 && D == 1) return 0.5f;
    if(S < D) return s == d ? 1.0f : 0.0f;
    *ok = 0; return 0.0f;
}
static void matrix_store(Send *s, uint32_t S, uint32_t D, const float *m){ free(s->matrix); s->matrix = xcalloc((size_t)S * D, sizeof(float)); memcpy(s->matrix, m, (size_t)S * D * sizeof(float)); s->msrc = S; s->mdst = D; }
int kmx_voice_set_matrix(KmxVoice *v, KmxVoice *dest, uint32_t S, uint32_t D, const float *m){
    KmxEngine *e = v->e; LOCK(e); int rc = KMX_E_INVALID; Send *s = find_send(v, dest);
    if(s && m && S == v->out_ch && D == s->dest->in_ch){
        rc = KMX_OK; for(uint32_t i = 0; i < S * D; i++) if(!vol_ok(m[i])) rc = KMX_E_INVALID;
        if(rc == KMX_OK){
            if(defer(e)){ Op *o = op_push(e, OP_MATRIX, v); o->dest = s->dest; o->a = S; o->b = D; o->vals = xcalloc((size_t)S * D, sizeof(float)); memcpy(o->vals, m, (size_t)S * D * sizeof(float)); }
            else matrix_store(s, S, D, m);
        }
    }
    UNLOCK(e); return rc;
}
int kmx_voice_matrix(KmxVoice *v, KmxVoice *dest, uint32_t S, uint32_t D, float *m){
    KmxEngine *e = v->e; LOCK(e); int rc = KMX_E_INVALID; Send *s = find_send(v, dest);
    if(s && m && S == v->out_ch && D == s->dest->in_ch){
        rc = KMX_OK; const Op *o = op_latest(e, v, OP_MATRIX, s->dest, 0, 0);
        if(o && o->a == S && o->b == D) memcpy(m, o->vals, (size_t)S * D * sizeof(float));
        else if(s->matrix && s->msrc == S && s->mdst == D) memcpy(m, s->matrix, (size_t)S * D * sizeof(float));
        else for(uint32_t d = 0; d < D && rc == KMX_OK; d++) for(uint32_t c = 0; c < S; c++){ int ok; m[d * S + c] = default_level(S, D, d, c, &ok); if(!ok){ rc = KMX_E_UNSUPPORTED; break; } }
    }
    UNLOCK(e); return rc;
}
uint32_t kmx_voice_in_channels(KmxVoice *v){ return v->in_ch; }
uint32_t kmx_voice_out_channels(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); uint32_t r = v->out_ch; UNLOCK(e); return r; }
uint32_t kmx_voice_in_rate(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); uint32_t r = v->in_rate; UNLOCK(e); return r; }
uint32_t kmx_voice_proc_rate(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); uint32_t r = v->out_rate; UNLOCK(e); return r; }
uint32_t kmx_voice_proc_frames(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); uint32_t r = v->out_frames; UNLOCK(e); return r; }
int kmx_voice_kind(KmxVoice *v){ return v->kind; }
void *kmx_voice_user(KmxVoice *v){ return v->user; }

/* ---- source voice API ------------------------------------------------------------------------------------------------------------- */
static void start_locked(KmxVoice *v){ v->started = 1; v->tails = 0; }
static void stop_locked(KmxVoice *v, uint32_t flags){
    v->started = 0;
    v->tails = (flags & KMX_PLAY_TAILS) && v->nfx; v->tail_quiet = 0; v->tail_left = 1000;      /* effect tails: until 8 silent passes, 10 s at most */
}
int kmx_source_start(KmxVoice *v){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID; KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_START, v); else start_locked(v);
    UNLOCK(e); return KMX_OK;
}
int kmx_source_stop(KmxVoice *v, uint32_t flags){
    if(v->kind != KMX_SOURCE || (flags & ~KMX_PLAY_TAILS)) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_STOP, v)->a = flags; else stop_locked(v, flags);
    UNLOCK(e); return KMX_OK;
}
int kmx_source_submit(KmxVoice *v, const KmxBuffer *b){
    if(v->kind != KMX_SOURCE || !b) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e); int rc = KMX_E_INVALID; QBuf q; memset(&q, 0, sizeof q);
    if(v->dead || (b->flags & ~KMX_END_OF_STREAM) || front_queued(e, v, NULL) >= KMX_MAX_QUEUED || !b->data || !b->bytes) goto out;
    if(v->fmt.tag == KMX_TAG_ADPCM ? b->bytes < 7 * v->fmt.channels : (b->bytes % v->fmt.block_align) != 0) goto out;
    q.flags = b->flags; q.bytes = b->bytes; q.data = b->data; q.ctx = b->context; q.frames = kmx_buffer_frames(&v->fmt, b->bytes);
    if(!q.frames || b->play_begin >= q.frames || (uint64_t)b->play_begin + b->play_length > q.frames) goto out;
    q.play_begin = b->play_begin; q.play_end = b->play_length ? b->play_begin + b->play_length : q.frames;
    if(b->loop_count){
        if(b->loop_count > KMX_MAX_LOOP_COUNT && b->loop_count != KMX_LOOP_INFINITE) goto out;
        if(b->loop_begin >= q.play_end) goto out;                                                  /* the loop may begin before play_begin (XAudio2 allows it) */
        q.loop_begin = b->loop_begin; q.loop_end = b->loop_length ? b->loop_begin + b->loop_length : q.play_end;
        if(q.loop_end <= q.play_begin || q.loop_end > q.play_end || q.loop_end <= q.loop_begin) goto out;
        q.loop_count = b->loop_count;
    } else if(b->loop_begin || b->loop_length) goto out;
    q.cursor = q.play_begin; rc = KMX_OK;
    e->st.buffers_submitted++; e->st.bytes_submitted += b->bytes;
    if(defer(e)) op_push(e, OP_SUBMIT, v)->q = q;
    else { v->q[v->qn++] = q; if(v->qn > e->st.peak_queued) e->st.peak_queued = v->qn; }
out:
    UNLOCK(e); return rc;
}
static void pend_push(KmxVoice *v, uint64_t ctx){
    if(v->npend == v->cpend){ v->cpend = v->cpend ? v->cpend * 2 : 8; v->pend = realloc(v->pend, v->cpend * sizeof *v->pend); if(!v->pend) abort(); }
    v->pend[v->npend++] = ctx;
}
/* XAudio2: every removed buffer reports OnBufferEnd (and nothing else) from the next processing pass; a started voice keeps the
 * buffer it is playing; a stopped voice loses that one too and its position is reset. */
static void flush_locked(KmxEngine *e, KmxVoice *v){
    uint32_t keep = (v->started && v->qn && v->q[0].begun) ? 1 : 0;
    for(uint32_t i = keep; i < v->qn; i++){ pend_push(v, v->q[i].ctx); e->st.buffers_flushed++; }
    v->qn = keep; if(!keep) reset_src(v);
}
int kmx_source_flush(KmxVoice *v){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_FLUSH, v); else flush_locked(e, v);
    UNLOCK(e); return KMX_OK;
}
int kmx_source_discontinuity(KmxVoice *v){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_DISC, v); else if(v->qn) v->q[v->qn - 1].flags |= KMX_END_OF_STREAM;
    UNLOCK(e); return KMX_OK;
}
int kmx_source_exit_loop(KmxVoice *v){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e);
    if(defer(e)) op_push(e, OP_EXITLOOP, v); else if(v->qn) v->q[0].loop_count = 0;
    UNLOCK(e); return KMX_OK;
}
int kmx_source_state(KmxVoice *v, uint64_t *ctx, uint32_t *queued, uint64_t *played){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e);
    uint64_t c; uint32_t n = front_queued(e, v, &c);
    if(ctx) *ctx = c; if(queued) *queued = n; if(played) *played = v->samples_played;
    UNLOCK(e); return KMX_OK;
}
int kmx_source_starvations(KmxVoice *v, uint64_t *count, uint64_t *first_pass, uint64_t *last_pass){
    if(v->kind != KMX_SOURCE) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e); *count = v->starve_count; *first_pass = v->starve_first; *last_pass = v->starve_last; UNLOCK(e); return KMX_OK;
}
int kmx_source_set_ratio(KmxVoice *v, float r){
    if(v->kind != KMX_SOURCE || !(r >= 1.0f / 1024.0f && r <= 1024.0f)) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e); float x = r > v->max_ratio ? v->max_ratio : r;                                  /* XAudio2 clamps to MaxFrequencyRatio */
    if(defer(e)) op_push(e, OP_RATIO, v)->f = x; else v->ratio = x;
    UNLOCK(e); return KMX_OK;
}
float kmx_source_ratio(KmxVoice *v){ KmxEngine *e = v->e; LOCK(e); const Op *o = op_latest(e, v, OP_RATIO, NULL, 0, 0); float r = o ? o->f : v->ratio; UNLOCK(e); return r; }
int kmx_source_set_rate(KmxVoice *v, uint32_t rate){
    if(v->kind != KMX_SOURCE || !check_rate(rate)) return KMX_E_INVALID;
    KmxEngine *e = v->e; LOCK(e); int rc = front_queued(e, v, NULL) ? KMX_E_INVALID : KMX_OK;   /* XAudio2: only with an empty queue */
    if(rc == KMX_OK){ if(defer(e)) op_push(e, OP_RATE, v)->a = rate; else { v->in_rate = v->fmt.rate = rate; reset_src(v); } }
    UNLOCK(e); return rc;
}

/* ---- the processing pass ---------------------------------------------------------------------------------------------------------- */
static inline int gone(KmxVoice *v){ return v->dead || v->closing; }
static void emit(KmxEngine *e, KmxVoice *v, void *ctx, int ev, uint64_t arg){
    switch(ev){ case KMX_EV_BUFFER_START: e->st.buffers_started++; break; case KMX_EV_BUFFER_END: e->st.buffers_ended++; break;
                case KMX_EV_LOOP_END: e->st.loops_ended++; break; case KMX_EV_STREAM_END: e->st.streams_ended++; break; default: e->st.voice_pass_callbacks++; }
    if(!v->has_cb || !e->host.voice_event) return;
    UNLOCK(e); e->host.voice_event(ctx, v->user, ev, arg); LOCK(e);
}
static void decode_frame(KmxVoice *v, const QBuf *b, uint32_t idx, float *dst){
    const KmxFormat *f = &v->fmt; uint32_t ch = f->channels;
    v->e->st.frames_decoded++;
    if(f->tag == KMX_TAG_ADPCM){
        int64_t blk = idx / f->samples_per_block;
        if(v->ad_data != b->data || v->ad_block != blk){
            uint64_t off = (uint64_t)blk * f->block_align; uint32_t n = b->bytes - off < f->block_align ? (uint32_t)(b->bytes - off) : f->block_align;
            int got = kmx_adpcm_decode_block(b->data + off, n, ch, f->coef, f->ncoef, v->ad_cache, f->samples_per_block);
            if(got <= 0){ memset(v->ad_cache, 0, sizeof(int16_t) * f->samples_per_block * ch); got = 0; }   /* malformed block (bad predictor index): silence */
            v->ad_frames = (uint32_t)got; v->ad_data = b->data; v->ad_block = blk;
        }
        const int16_t *s = v->ad_cache + (size_t)(idx % f->samples_per_block) * ch;
        for(uint32_t c = 0; c < ch; c++) dst[c] = (float)s[c] * (1.0f / 32768.0f);
        return;
    }
    const uint8_t *p = b->data + (size_t)idx * f->block_align;
    if(f->tag == KMX_TAG_FLOAT){ memcpy(dst, p, ch * sizeof(float)); return; }
    switch(f->bits){
    case 8:  for(uint32_t c = 0; c < ch; c++) dst[c] = ((float)p[c] - 128.0f) * (1.0f / 128.0f); break;
    case 16: for(uint32_t c = 0; c < ch; c++) dst[c] = (float)(int16_t)(uint16_t)(p[2 * c] | (p[2 * c + 1] << 8)) * (1.0f / 32768.0f); break;
    case 24: for(uint32_t c = 0; c < ch; c++){ int32_t x = (int32_t)(((uint32_t)p[3 * c] << 8) | ((uint32_t)p[3 * c + 1] << 16) | ((uint32_t)p[3 * c + 2] << 24)) >> 8; dst[c] = (float)x * (1.0f / 8388608.0f); } break;
    default: for(uint32_t c = 0; c < ch; c++){ int32_t x; memcpy(&x, p + 4 * c, 4); dst[c] = (float)((double)x * (1.0 / 2147483648.0)); }
    }
}
/* the head buffer is exhausted: OnBufferEnd, then OnStreamEnd when it carried END_OF_STREAM */
static void finish_head(KmxEngine *e, KmxVoice *v, void *ctx){
    uint64_t bctx = v->q[0].ctx; int eos = (v->q[0].flags & KMX_END_OF_STREAM) != 0;
    memmove(&v->q[0], &v->q[1], (v->qn - 1) * sizeof(QBuf)); v->qn--;
    if(eos){ v->samples_played = 0; v->uncounted = (uint32_t)v->fill; v->stream_open = 0; v->fresh = 1; }
    emit(e, v, ctx, KMX_EV_BUFFER_END, bctx);
    if(eos && !v->dead) emit(e, v, ctx, KMX_EV_STREAM_END, 0);
}
/* 1 = one frame decoded into dst; 0 = nothing queued; -1 = the voice was stopped or destroyed by a callback */
static int pull_frame(KmxEngine *e, KmxVoice *v, void *ctx, float *dst){
    for(;;){
        if(gone(v) || !v->started) return -1;
        if(!v->qn) return 0;
        QBuf *b = &v->q[0];
        if(!b->begun){ b->begun = 1; emit(e, v, ctx, KMX_EV_BUFFER_START, b->ctx); continue; }   /* re-read the head: the callback may have changed the queue */
        uint32_t end = b->loop_count ? b->loop_end : b->play_end;
        if(b->cursor >= end){
            if(b->loop_count){
                if(b->loop_count != KMX_LOOP_INFINITE) b->loop_count--;
                b->cursor = b->loop_begin; emit(e, v, ctx, KMX_EV_LOOP_END, b->ctx); continue;
            }
            finish_head(e, v, ctx); continue;
        }
        decode_frame(v, b, b->cursor, dst); b->cursor++; v->stream_open = 1; v->starving = 0;
        return 1;
    }
}
static inline float cubic(float p0, float p1, float p2, float p3, float t){        /* Catmull-Rom through p1 (t = 0) and p2 (t = 1) */
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}
static uint64_t src_step(KmxVoice *v){ uint64_t n = (uint64_t)llround((double)v->in_rate * (double)v->ratio * 65536.0); return n ? n : 1; }
/* bytes that must be queued now to avoid starvation in this pass (0 when enough is queued) */
static uint32_t bytes_required(KmxVoice *v){
    uint64_t den = (uint64_t)v->out_rate << 16, num = src_step(v);
    uint64_t need = (v->frac + num * v->out_frames + den - 1) / den + 2, have = (uint64_t)v->fill;
    for(uint32_t i = 0; i < v->qn && have < need; i++){ if(v->q[i].loop_count) return 0; have += v->q[i].play_end - v->q[i].cursor; }
    if(have >= need) return 0;
    uint64_t miss = need - have;
    if(v->fmt.tag == KMX_TAG_ADPCM) return (uint32_t)((miss + v->fmt.samples_per_block - 1) / v->fmt.samples_per_block * v->fmt.block_align);
    return (uint32_t)(miss * v->fmt.block_align);
}
/* decode + resample one quantum of a started source voice into v->a (zeroed by the caller) */
static void render_source(KmxEngine *e, KmxVoice *v, void *ctx){
    const uint32_t ch = v->in_ch, N = v->out_frames; float *out = v->a, fr[KMX_MAXCH];
    const uint64_t den = (uint64_t)v->out_rate << 16;
    for(uint32_t o = 0; o < N; o++){
        uint64_t num = src_step(v);
        int need = v->frac ? 3 : 1, r = 1;
        while(v->fill < need){
            r = pull_frame(e, v, ctx, fr);
            if(r <= 0) break;
            float *slot = v->win + (size_t)(1 + v->fill) * ch; memcpy(slot, fr, ch * sizeof(float));
            if(v->fresh){ memcpy(v->win, fr, ch * sizeof(float)); v->fresh = 0; }       /* a new stream starts with a flat previous sample */
            v->fill++;
        }
        if(r < 0 || gone(v) || !v->started) return;
        if(v->fill < 1){                                                               /* nothing to play */
            if(v->stream_open && !v->starving){                                       /* queue ran dry before END_OF_STREAM: a glitch */
                v->starving = 1; e->st.starvations++;
                if(!v->starve_count++) v->starve_first = e->st.passes; v->starve_last = e->st.passes;
            }
            v->frac = 0; break;
        }
        const float *w = v->win; float *d = out + (size_t)o * ch;
        if(!v->frac) memcpy(d, w + ch, ch * sizeof(float));                            /* on a sample: exact copy, no look-ahead */
        else {
            float t = (float)((double)v->frac / (double)den);
            for(uint32_t c = 0; c < ch; c++) d[c] = cubic(w[c], w[ch + c], w[2 * ch + c], w[3 * ch + c], t);   /* missing look-ahead reads as zero */
            e->st.frames_resampled++;
        }
        v->frac += num;
        while(v->frac >= den){
            if(!v->fill){                                                              /* a step of 2 or more skips frames that were never looked at */
                r = pull_frame(e, v, ctx, fr);
                if(r < 0 || gone(v)) return;
                if(!r){ v->frac = 0; break; }                                          /* ran past the queued data */
                memcpy(v->win + ch, fr, ch * sizeof(float)); v->fresh = 0; v->fill = 1;
            }
            v->frac -= den;
            memmove(v->win, v->win + ch, 3 * (size_t)ch * sizeof(float)); memset(v->win + 3 * (size_t)ch, 0, ch * sizeof(float));
            v->fill--; if(v->uncounted) v->uncounted--; else v->samples_played++;
        }
    }
    /* settle: a buffer whose last frame was just taken ends in this pass, not the next */
    while(v->qn && v->started && !gone(v)){
        QBuf *b = &v->q[0];
        if(!b->begun || b->loop_count || b->cursor < b->play_end) break;
        finish_head(e, v, ctx);
    }
}
static int all_zero(const float *p, size_t n){ for(size_t i = 0; i < n; i++) if(p[i] != 0.0f) return 0; return 1; }
static float peak_of(const float *p, size_t n){ float m = 0.0f; for(size_t i = 0; i < n; i++){ float a = fabsf(p[i]); if(a > m) m = a; } return m; }
/* run the effect chain over cur (ch channels, n frames); returns the buffer holding the result and updates *pch */
static float *run_chain(KmxEngine *e, KmxVoice *v, void *ctx, float *cur, uint32_t *pch, uint32_t n){
    uint32_t ch = *pch;
    if(e->peaks){ v->peak_stage[0] = peak_of(cur, (size_t)n * ch); v->nstage = 1; }
    for(uint32_t i = 0; i < v->nfx && !gone(v); i++){
        Fx *f = &v->fx[i];
        if(f->kind == KMX_FX_METER){
            e->st.fx_meter++;
            for(uint32_t c = 0; c < ch; c++){
                float pk = 0.0f; double sq = 0.0;
                if(f->enabled) for(uint32_t k = 0; k < n; k++){ float x = cur[(size_t)k * ch + c], ax = fabsf(x); if(ax > pk) pk = ax; sq += (double)x * x; }
                f->peak[c] = pk; f->rms[c] = (float)sqrt(sq / (double)n);
            }
        } else if(f->kind == KMX_FX_PASS){
            e->st.fx_pass_through++;                                                   /* system effect with no implementation: audio passes unchanged */
        } else {
            float *dst = f->inplace ? cur : (cur == v->a ? v->b : v->a);
            int silent_in = all_zero(cur, (size_t)n * ch), silent_out = 0, en = f->enabled; uint32_t oc = f->out_ch;
            e->st.fx_host_process++;
            UNLOCK(e); e->host.fx_process(ctx, f->user, cur, ch, silent_in, dst, oc, n, en, &silent_out); LOCK(e);
            if(gone(v)) break;
            if(silent_out) memset(dst, 0, (size_t)n * oc * sizeof(float));
            cur = dst; ch = oc;
            if(e->peaks && v->nstage < 10) v->peak_stage[v->nstage++] = peak_of(cur, (size_t)n * ch);
        }
    }
    *pch = ch; return cur;
}
/* buf (S channels, n frames) -> every destination's input mix, through gain * channel volume * matrix, ramped over the pass */
static int mix_to_sends(KmxVoice *v, const float *buf, uint32_t S, uint32_t n, float gain){
    int rc = KMX_OK;
    for(uint32_t si = 0; si < v->nsends; si++){
        Send *s = &v->sends[si]; KmxVoice *d = s->dest; uint32_t D = d->in_ch;
        if(d->dead || d->hidden || d->in_frames != n) continue;
        size_t nc = (size_t)S * D; float coef[nc];
        int nz = 0;
        for(uint32_t dd = 0; dd < D; dd++) for(uint32_t ss = 0; ss < S; ss++){
            float m; int ok = 1;
            if(s->matrix && s->msrc == S && s->mdst == D) m = s->matrix[dd * S + ss]; else m = default_level(S, D, dd, ss, &ok);
            if(!ok){ rc = KMX_E_UNSUPPORTED; v->e->st.missing_default_matrix++; }
            float c = gain * v->chan_vol[ss] * m; coef[dd * S + ss] = c; nz |= c != 0.0f;
        }
        if(s->nprev != nc){ free(s->prev); s->prev = xcalloc(nc, sizeof(float)); memcpy(s->prev, coef, nc * sizeof(float)); s->nprev = (uint32_t)nc; }   /* first pass: no ramp */
        int ramp = memcmp(s->prev, coef, nc * sizeof(float)) != 0;
        for(size_t i = 0; i < nc; i++) nz |= s->prev[i] != 0.0f;
        if(nz){
            float *o = d->mix; const float inv = 1.0f / (float)n;
            for(uint32_t dd = 0; dd < D; dd++) for(uint32_t ss = 0; ss < S; ss++){
                float c1 = coef[dd * S + ss], c0 = s->prev[dd * S + ss];
                if(c0 == 0.0f && c1 == 0.0f) continue;
                const float *x = buf + ss; float *y = o + dd;
                if(c0 == c1) for(uint32_t k = 0; k < n; k++) y[(size_t)k * D] += c1 * x[(size_t)k * S];
                else { float dc = (c1 - c0) * inv; for(uint32_t k = 0; k < n; k++) y[(size_t)k * D] += (c0 + dc * (float)(k + 1)) * x[(size_t)k * S]; }
            }
        }
        if(ramp) memcpy(s->prev, coef, nc * sizeof(float));
    }
    return rc;
}
static void process_source(KmxEngine *e, KmxVoice *v, void *ctx){
    if(v->has_cb){ emit(e, v, ctx, KMX_EV_PASS_START, v->started ? bytes_required(v) : 0); if(gone(v)) return; }
    while(v->npend && !gone(v)){                                                         /* buffers removed by FlushSourceBuffers */
        uint64_t c = v->pend[0]; memmove(&v->pend[0], &v->pend[1], (v->npend - 1) * sizeof *v->pend); v->npend--;
        emit(e, v, ctx, KMX_EV_BUFFER_END, c);
    }
    if(gone(v)) return;
    if(v->started || v->tails){
        uint32_t ch = v->in_ch, n = v->out_frames;
        memset(v->a, 0, (size_t)n * ch * sizeof(float));
        if(v->started) render_source(e, v, ctx);
        if(gone(v)) return;
        float *cur = run_chain(e, v, ctx, v->a, &ch, n);
        if(gone(v)) return;
        mix_to_sends(v, cur, ch, n, v->volume);
        if(v->tails){
            if(all_zero(cur, (size_t)n * ch)) v->tail_quiet++; else v->tail_quiet = 0;
            if(v->tail_quiet >= 8 || !--v->tail_left) v->tails = 0;
        }
    }
    if(v->has_cb) emit(e, v, ctx, KMX_EV_PASS_END, 0);
}
/* fixed-rate conversion of one input block (in_frames) to out_frames, 2 input frames of delay, exact rational phase */
static void resample_block(KmxVoice *v, const float *in, float *out){
    const uint32_t ch = v->in_ch, N = v->in_frames, M = v->out_frames; const uint64_t num = v->in_rate, den = v->out_rate;
    uint64_t acc = 0; int64_t base = -2; const float *hist = v->win;                     /* hist[0..2] = input frames -3, -2, -1 */
#define FR(k) ((k) < 0 ? hist + (size_t)(3 + (k)) * ch : in + (size_t)(k) * ch)
    for(uint32_t j = 0; j < M; j++){
        float *d = out + (size_t)j * ch; const float *p1 = FR(base);
        if(!acc) memcpy(d, p1, ch * sizeof(float));
        else { const float *p0 = FR(base - 1), *p2 = FR(base + 1), *p3 = FR(base + 2); float t = (float)((double)acc / (double)den);
               for(uint32_t c = 0; c < ch; c++) d[c] = cubic(p0[c], p1[c], p2[c], p3[c], t); }
        acc += num; while(acc >= den){ acc -= den; base++; }
    }
#undef FR
    v->e->st.frames_resampled += M;
    if(N >= 3) memcpy(v->win, in + (size_t)(N - 3) * ch, 3 * (size_t)ch * sizeof(float));
}
static const float *process_mix_voice(KmxEngine *e, KmxVoice *v, void *ctx, uint32_t *out_ch){
    uint32_t ch = v->in_ch, n = v->out_frames; float *cur = v->mix;
    if(v->in_rate != v->out_rate){ resample_block(v, v->mix, v->a); cur = v->a; }
    float v0 = v->prev_volume, v1 = v->volume;                                           /* volume ahead of the effect chain */
    if(v0 != 1.0f || v1 != 1.0f){
        if(v0 == v1) for(size_t i = 0; i < (size_t)n * ch; i++) cur[i] *= v1;
        else { float dv = (v1 - v0) / (float)n; for(uint32_t k = 0; k < n; k++){ float gk = v0 + dv * (float)(k + 1); for(uint32_t c = 0; c < ch; c++) cur[(size_t)k * ch + c] *= gk; } }
    }
    v->prev_volume = v1;
    cur = run_chain(e, v, ctx, cur, &ch, n);
    if(gone(v)) return NULL;
    if(v->kind == KMX_SUBMIX){ mix_to_sends(v, cur, ch, n, 1.0f); return NULL; }
    for(uint32_t c = 0; c < ch; c++){                                                    /* master: channel volumes, ramped */
        float c0 = v->prev_chan_vol[c], c1 = v->chan_vol[c];
        if(c0 == 1.0f && c1 == 1.0f) continue;
        float dc = (c1 - c0) / (float)n; for(uint32_t k = 0; k < n; k++) cur[(size_t)k * ch + c] *= c0 + dc * (float)(k + 1);
        v->prev_chan_vol[c] = c1;
    }
    *out_ch = ch; return cur;
}
static int snap_order(const void *pa, const void *pb){
    const KmxVoice *a = *(KmxVoice *const *)pa, *b = *(KmxVoice *const *)pb;
    if(a->kind != b->kind) return a->kind < b->kind ? -1 : 1;                             /* sources, submixes, master */
    if(a->kind == KMX_SUBMIX && a->stage != b->stage) return a->stage < b->stage ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}
/* deferred mode: the queued calls stamped before `instant`, in call order, on the pass thread (lock held; released only around the
 * delivery of effect parameters, with the voice marked busy so that it cannot be destroyed meanwhile) */
static void apply_ops(KmxEngine *e, void *ctx, uint64_t instant){
    while(e->ophead < e->nops && e->ops[e->ophead].stamp < instant){
        Op o = e->ops[e->ophead++]; KmxVoice *v = o.v;
        if(gone(v)){ e->st.deferred_dropped++; op_free(&o); continue; }
        int ok = 1;
        switch(o.kind){
        case OP_START: start_locked(v); break;
        case OP_STOP: stop_locked(v, o.a); break;
        case OP_SUBMIT: if(v->qn >= KMX_MAX_QUEUED) ok = 0; else { v->q[v->qn++] = o.q; if(v->qn > e->st.peak_queued) e->st.peak_queued = v->qn; } break;
        case OP_FLUSH: flush_locked(e, v); break;
        case OP_DISC: if(v->qn) v->q[v->qn - 1].flags |= KMX_END_OF_STREAM; break;
        case OP_EXITLOOP: if(v->qn) v->q[0].loop_count = 0; break;
        case OP_VOLUME: v->volume = o.f; break;
        case OP_CHANVOL: if(o.a == v->out_ch) memcpy(v->chan_vol, o.vals, o.a * sizeof(float)); else ok = 0; break;
        case OP_MATRIX: { Send *s = find_send(v, o.dest); if(s && o.a == v->out_ch && o.b == o.dest->in_ch) matrix_store(s, o.a, o.b, o.vals); else ok = 0; break; }
        case OP_RATIO: v->ratio = o.f; break;
        case OP_RATE: if(v->qn) ok = 0; else { v->in_rate = v->fmt.rate = o.a; reset_src(v); } break;
        case OP_FXEN: if(o.a < v->nfx) v->fx[o.a].enabled = (int)o.b; else ok = 0; break;
        case OP_FXPARAMS:
            if(o.a < v->nfx && v->fx[o.a].kind == KMX_FX_HOST && e->host.fx_params){ void *fu = v->fx[o.a].user; v->busy = 1;
                UNLOCK(e); e->host.fx_params(ctx, fu, o.blob, o.b); LOCK(e); v->busy = 0; pthread_cond_broadcast(&e->cv); }
            else ok = 0;
            break;
        case OP_PUBLISH: v->hidden = 0; break;
        default: ok = 0;
        }
        if(ok) e->st.deferred_applied++; else e->st.deferred_failed++;
        op_free(&o);
    }
}
int kmx_engine_pass(KmxEngine *e, void *ctx, const float **out, uint32_t *out_ch){ return kmx_engine_pass_at(e, ctx, UINT64_MAX, out, out_ch); }
int kmx_engine_pass_at(KmxEngine *e, void *ctx, uint64_t instant, const float **out, uint32_t *out_ch){
    if(out) *out = NULL; if(out_ch) *out_ch = 0;
    LOCK(e);
    if(e->in_pass){ UNLOCK(e); return KMX_E_INVALID; }
    if(!e->running){ UNLOCK(e); return KMX_OK; }
    e->in_pass = 1; e->pass_thread = pthread_self();
    apply_ops(e, ctx, instant);
    uint32_t n = e->nvoices;
    if(n > e->csnap){ e->csnap = n + 32; e->snap = realloc(e->snap, e->csnap * sizeof *e->snap); if(!e->snap) abort(); }
    memcpy(e->snap, e->voices, n * sizeof *e->snap);
    qsort(e->snap, n, sizeof *e->snap, snap_order);
    uint32_t act = 0;
    for(uint32_t i = 0; i < n; i++){ KmxVoice *v = e->snap[i]; if(v->kind != KMX_SOURCE) memset(v->mix, 0, (size_t)v->in_frames * v->in_ch * sizeof(float)); else if(v->started && !v->hidden) act++; }
    if(act > e->st.peak_active_source) e->st.peak_active_source = act;
    if(e->callbacks && e->host.engine_event){ e->ecb_busy = 1; e->st.engine_pass_callbacks++; UNLOCK(e); e->host.engine_event(ctx, e->user, KMX_EV_PASS_START); LOCK(e); e->ecb_busy = 0; pthread_cond_broadcast(&e->cv); }
    const float *res = NULL; uint32_t res_ch = 0;
    for(uint32_t i = 0; i < n; i++){
        KmxVoice *v = e->snap[i];
        if(gone(v)) continue;
        if(v->hidden){ e->st.hidden_skips++; continue; }                                   /* not published yet: its creation is not finished */
        v->busy = 1;
        if(v->kind == KMX_SOURCE) process_source(e, v, ctx);
        else { uint32_t oc = 0; const float *r = process_mix_voice(e, v, ctx, &oc); if(v->kind == KMX_MASTER && r && !gone(v)){ res = r; res_ch = oc; } }
        v->busy = 0; pthread_cond_broadcast(&e->cv);
    }
    if(e->callbacks && e->host.engine_event){ e->ecb_busy = 1; e->st.engine_pass_callbacks++; UNLOCK(e); e->host.engine_event(ctx, e->user, KMX_EV_PASS_END); LOCK(e); e->ecb_busy = 0; pthread_cond_broadcast(&e->cv); }
    e->st.passes++;
    e->in_pass = 0;
    if(e->master && res && !e->master->dead){
        size_t nf = (size_t)e->quantum * res_ch;
        if(nf > e->cout){ free(e->out); e->out = xcalloc(nf, sizeof(float)); e->cout = nf; }
        memcpy(e->out, res, nf * sizeof(float));
        if(out) *out = e->out; if(out_ch) *out_ch = res_ch;
    }
    for(uint32_t i = 0; i < e->ngrave; i++) free_voice(e->grave[i]);
    e->ngrave = 0;
    UNLOCK(e);
    return KMX_OK;
}

/* At a parked cut the guest arena owns all sample bytes and processing buffers.
 * Rebind host identities, preserving the addresses XAPOs see in that arena. */
static void *copy_array(const void *src,size_t count,size_t size){
    if(!src || !count) return NULL;
    void *dst=xcalloc(count,size);
    memcpy(dst,src,count*size);
    return dst;
}
static KmxVoice *clone_ref(KmxEngine *from,KmxEngine *to,KmxVoice *v){
    if(!v) return NULL;
    for(uint32_t i=0;i<from->nvoices;i++) if(from->voices[i]==v) return to->voices[i];
    fprintf(stderr,"kmx: savestate-mixer-dangling-voice\n"); abort();
}
KmxEngine *kmx_clone_graph(KmxEngine *src,void *user,KmxCloneBinding bind){
    LOCK(src);
    if(src->in_pass||src->ngrave){ UNLOCK(src); return NULL; }
    KmxEngine *e=kmx_engine_create(&src->host,user,src->rate);
    e->running=src->running; e->callbacks=src->callbacks; e->deferred=src->deferred;
    e->next_id=src->next_id; e->peaks=src->peaks; e->st=src->st;
    e->cvoices=e->nvoices=src->nvoices; e->voices=xcalloc(e->nvoices,sizeof *e->voices);
    for(uint32_t i=0;i<e->nvoices;i++){
        KmxVoice *s=src->voices[i], *v=copy_array(s,1,sizeof *v);
        e->voices[i]=v; v->e=e;
        v->win=copy_array(s->win,4*s->in_ch,sizeof(float));
        v->ad_cache=copy_array(s->ad_cache,(size_t)s->fmt.samples_per_block*s->in_ch,sizeof(int16_t));
        v->pend=copy_array(s->pend,s->npend,sizeof *s->pend); v->cpend=v->npend;
        v->fx=copy_array(s->fx,s->nfx,sizeof *s->fx);
        if(bind){
            v->user=bind(user,s->user,v,-1);
            for(uint32_t j=0;j<v->nfx;j++) v->fx[j].user=bind(user,s->user,v,(int)j);
        }
        v->sends=copy_array(s->sends,s->nsends,sizeof *s->sends);
        for(uint32_t j=0;j<v->nsends;j++){
            Send *d=&v->sends[j];
            d->matrix=copy_array(d->matrix,(size_t)d->msrc*d->mdst,sizeof(float));
            d->prev=copy_array(d->prev,d->nprev,sizeof(float));
        }
    }
    for(uint32_t i=0;i<e->nvoices;i++) for(uint32_t j=0;j<e->voices[i]->nsends;j++){
        Send *s=&e->voices[i]->sends[j]; s->dest=clone_ref(src,e,s->dest);
    }
    e->master=clone_ref(src,e,src->master);
    e->nops=e->cops=src->nops-src->ophead;
    if(e->nops) e->ops=copy_array(src->ops+src->ophead,e->nops,sizeof *e->ops);
    for(uint32_t i=0;i<e->nops;i++){
        Op *o=&e->ops[i];
        o->v=clone_ref(src,e,o->v); o->dest=clone_ref(src,e,o->dest);
        o->vals=copy_array(o->vals,(size_t)o->a*(o->kind==OP_MATRIX?o->b:1),sizeof(float));
        o->blob=copy_array(o->blob,o->b,1);
    }
    UNLOCK(src);
    return e;
}
/* Dispose only native graph storage. Guest buffers and COM lifetimes belong to
 * the independently restored arena. No callbacks or guest frees at a cut. */
void kmx_discard_graph(KmxEngine *e){
    e->host.voice_free=NULL;
    for(uint32_t i=0;i<e->nvoices;i++){
        KmxVoice *v=e->voices[i]; v->a=v->b=v->mix=NULL;
    }
    kmx_engine_destroy(e);
}

/* ---- WAV writer ------------------------------------------------------------------------------------------------------------------- */
struct KmxWav { int fd; uint32_t rate, ch; uint64_t data_bytes; KmxWavStats st; int16_t *tmp; uint32_t ctmp; };
static void put32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int wav_header(KmxWav *w){
    uint8_t h[44]; uint32_t db = w->data_bytes > 0xffffffffull - 36 ? 0xffffffffu - 36 : (uint32_t)w->data_bytes;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + db); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    h[20] = 1; h[21] = 0; h[22] = (uint8_t)w->ch; h[23] = 0; put32(h + 24, w->rate); put32(h + 28, w->rate * w->ch * 2);
    h[32] = (uint8_t)(w->ch * 2); h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4); put32(h + 40, db);
    return pwrite(w->fd, h, 44, 0) == 44 ? 0 : -1;
}
KmxWav *kmx_wav_open(const char *path, uint32_t rate, uint32_t ch){
    if(!path || !*path || ch < 1 || ch > 8) return NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if(fd < 0) return NULL;
    KmxWav *w = xcalloc(1, sizeof *w); w->fd = fd; w->rate = rate; w->ch = ch;
    if(wav_header(w) || lseek(fd, 44, SEEK_SET) != 44){ close(fd); free(w); return NULL; }
    return w;
}
int kmx_wav_write(KmxWav *w, const float *x, uint32_t frames){
    uint32_t n = frames * w->ch;
    if(n > w->ctmp){ free(w->tmp); w->tmp = xcalloc(n, sizeof(int16_t)); w->ctmp = n; }
    if(!x) memset(w->tmp, 0, n * sizeof(int16_t));
    else for(uint32_t f = 0; f < frames; f++){
        int ns = 0;
        for(uint32_t c = 0; c < w->ch; c++){
            float s = x[(size_t)f * w->ch + c], a = fabsf(s);
            if(!(a == a)){ s = 0.0f; a = 0.0f; w->st.clipped_samples++; }                 /* NaN: counted with the clipped samples, written as zero */
            if(a > w->st.peak) w->st.peak = a;
            if(a > 1.0f) w->st.clipped_samples++;
            float y = s * 32768.0f; long q = lrintf(y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y);
            w->tmp[(size_t)f * w->ch + c] = (int16_t)q; ns |= q != 0;
            double d = (double)q * (1.0 / 32768.0); w->st.sum_sq += d * d;
        }
        w->st.nonsilent_frames += ns;
    }
    size_t bytes = (size_t)n * 2; const uint8_t *p = (const uint8_t *)w->tmp;          /* little-endian hosts only (x86-64, arm64) */
    while(bytes){ ssize_t k = write(w->fd, p, bytes); if(k < 0){ if(errno == EINTR) continue; return -1; } p += k; bytes -= (size_t)k; }
    w->data_bytes += (uint64_t)n * 2; w->st.frames += frames;
    return 0;
}
int kmx_wav_sync(KmxWav *w){ return wav_header(w); }
void kmx_wav_stats(KmxWav *w, KmxWavStats *st){ *st = w->st; }
int kmx_wav_close(KmxWav *w, KmxWavStats *st){
    int rc = wav_header(w); if(st) *st = w->st;
    if(close(w->fd)) rc = -1; free(w->tmp); free(w); return rc;
}
