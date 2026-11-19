/* audio_out.c : the pull interface of khnmc_audio_out.h (TASK28): the lock-free SPSC ring between the mixer thread and a device
 * thread, the device-side calls, and the simulated device of KHNMC_AUDIO=pull-test.  Host code only: no guest state is touched here
 * (the simulated device is a plain pthread, not a guest thread).  The mixer's clock policy lives in null_audio.c (it owns the passes);
 * the contract is written down in khnmc_audio_out.h. */
#define _GNU_SOURCE
#include "khnmc_audio_out.h"
#include "khnmc_mix.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include "kh_compat.h"

/* ---- the ring -------------------------------------------------------------------------------------------------------------------
 * Frame counters wpos (written by the producer only) and rpos (written by the consumer only), both monotonic; fill = wpos - rpos.
 * The producer publishes frames with a release store of wpos after writing them; the consumer reads wpos with acquire, copies, and
 * releases the space with a release store of rpos, which the producer reads with acquire before reusing it.  Every counter below has
 * exactly one writing side, so relaxed atomics are enough for them. */
static float *ring;                         /* cap * 2 floats; allocated by kh_audio_ring_setup, never in a pull */
static uint32_t cap, mask; static _Atomic uint32_t target;      /* target: set before start (kh_audio_out_set_target), read by the mixer */
static _Atomic uint64_t wpos, rpos;
static _Atomic int enabled, started;
/* One consumer. The low bits protect rpos during a cut, the upper bits count
 * completed cuts so even two callbacks straddling an entire restore crossfade. */
enum { READING=1, PAUSED=2, FADE_FRAMES=64 };
static _Atomic unsigned cut_gate;
static unsigned seen_gate, fade_left;
static int await_mix;
static float last_sample[2], fade_from[2];  /* consumer-owned */
/* producer-side counters */
static _Atomic uint64_t c_produced, c_overruns, c_overrun_frames, c_stopped, c_starts, c_stops;
static _Atomic uint32_t c_max_fill;
/* consumer-side counters */
static _Atomic uint64_t c_pulled, c_delivered, c_underruns, c_underrun_frames, c_pulls;
static _Atomic uint32_t c_min_fill; static _Atomic int c_min_valid;
#define RLX memory_order_relaxed

static _Atomic int device_attached;          /* a real output device pulls from the ring (kh_audio_out_attach_device) */
/* attach and rebuild exclude each other (review r9: a rebuild that had passed the check could free the ring while a device attached
 * meanwhile pulled from it); the device's render thread never takes this lock */
static pthread_mutex_t setup_m = PTHREAD_MUTEX_INITIALIZER;
void kh_audio_out_attach_device(int on){ pthread_mutex_lock(&setup_m); atomic_store(&device_attached, on ? 1 : 0); pthread_mutex_unlock(&setup_m); }
static void ring_setup_locked(int enable, uint32_t ring_frames, uint32_t target_frames);
void kh_audio_ring_setup(int enable, uint32_t ring_frames, uint32_t target_frames){
    pthread_mutex_lock(&setup_m); ring_setup_locked(enable, ring_frames, target_frames); pthread_mutex_unlock(&setup_m);
}
static void ring_setup_locked(int enable, uint32_t ring_frames, uint32_t target_frames){
    if(atomic_load(&device_attached)){ fprintf(stderr, "kh: FATAL: audio-ring-rebuilt-with-device: the pull ring would be %s while a real output device reads it (its next pull would read freed or test data)\n", enable ? "rebuilt" : "disabled"); abort(); }
    if(!enable){ atomic_store(&enabled, 0); atomic_store(&started, 0); return; }
    uint32_t c = 1; while(c < ring_frames) c <<= 1;
    if(c != cap || !ring){ free(ring); ring = calloc((size_t)c * KH_AUDIO_OUT_CHANNELS, sizeof(float)); if(!ring){ fprintf(stderr, "kh: FATAL: audio-out: ring allocation of %u frames failed\n", c); abort(); } cap = c; mask = c - 1; }
    atomic_store_explicit(&target, target_frames < cap ? target_frames : cap, RLX);
    atomic_store(&cut_gate, 0); seen_gate=fade_left=0; await_mix=0;
    memset(last_sample,0,sizeof last_sample);
    atomic_store(&wpos, 0); atomic_store(&rpos, 0); atomic_store(&started, 0);
    atomic_store(&c_produced, 0); atomic_store(&c_overruns, 0); atomic_store(&c_overrun_frames, 0); atomic_store(&c_stopped, 0); atomic_store(&c_starts, 0); atomic_store(&c_stops, 0); atomic_store(&c_max_fill, 0);
    atomic_store(&c_pulled, 0); atomic_store(&c_delivered, 0); atomic_store(&c_underruns, 0); atomic_store(&c_underrun_frames, 0); atomic_store(&c_pulls, 0); atomic_store(&c_min_fill, 0); atomic_store(&c_min_valid, 0);
    atomic_store(&enabled, 1);
}
int kh_audio_ring_enabled(void){ return atomic_load(&enabled); }
int kh_audio_ring_started(void){ return atomic_load(&started); }
uint32_t kh_audio_ring_fill(void){ return (uint32_t)(atomic_load_explicit(&wpos, RLX) - atomic_load_explicit(&rpos, memory_order_acquire)); }
int kh_audio_ring_push(const float *x, uint32_t frames){
    if(!ring) return -1;
    uint64_t w = atomic_load_explicit(&wpos, RLX), r = atomic_load_explicit(&rpos, memory_order_acquire);
    if(frames > cap || cap - (uint32_t)(w - r) < frames) return -1;
    uint32_t at = (uint32_t)(w & mask), first = cap - at < frames ? cap - at : frames;
    float *d0 = ring + (size_t)at * KH_AUDIO_OUT_CHANNELS, *d1 = ring;
    if(x){ memcpy(d0, x, (size_t)first * KH_AUDIO_OUT_CHANNELS * sizeof(float)); memcpy(d1, x + (size_t)first * KH_AUDIO_OUT_CHANNELS, (size_t)(frames - first) * KH_AUDIO_OUT_CHANNELS * sizeof(float)); }
    else { memset(d0, 0, (size_t)first * KH_AUDIO_OUT_CHANNELS * sizeof(float)); memset(d1, 0, (size_t)(frames - first) * KH_AUDIO_OUT_CHANNELS * sizeof(float)); }
    atomic_store_explicit(&wpos, w + frames, memory_order_release);
    atomic_fetch_add_explicit(&c_produced, frames, RLX);
    uint32_t fill = (uint32_t)(w + frames - r); if(fill > atomic_load_explicit(&c_max_fill, RLX)) atomic_store_explicit(&c_max_fill, fill, RLX);
    return 0;
}
void kh_audio_ring_note(int overrun_frames, int stopped_pass){
    if(overrun_frames > 0){ atomic_fetch_add_explicit(&c_overruns, 1, RLX); atomic_fetch_add_explicit(&c_overrun_frames, (uint64_t)overrun_frames, RLX); }
    if(stopped_pass) atomic_fetch_add_explicit(&c_stopped, 1, RLX);
}

/* ---- device side --------------------------------------------------------------------------------------------------------------- */
int kh_audio_out_start(void){ if(!atomic_load(&enabled)) return -1; atomic_fetch_add(&c_starts, 1); atomic_store(&started, 1); return 0; }
void kh_audio_out_stop(void){ if(atomic_load(&started)) atomic_fetch_add(&c_stops, 1); atomic_store(&started, 0); }
/* Called only with the producer parked. New callbacks render a short ramp to
 * silence. Only a callback already copying the ring can delay the discard. */
void kh_audio_out_cut_pause(void){
    if(!atomic_load(&enabled)) return;
    atomic_fetch_or(&cut_gate,PAUSED);
    while(atomic_load(&cut_gate)&READING) sched_yield();
    uint64_t write=atomic_load(&wpos), read=atomic_exchange(&rpos,write);
    fprintf(stderr,"kh: savestate-audio-cut attached=%d started=%d dropped=%llu frames\n",
            atomic_load(&device_attached),atomic_load(&started),(unsigned long long)(write-read));
}
void kh_audio_out_cut_resume(void){
    if(atomic_load(&enabled)) atomic_fetch_add(&cut_gate,PAUSED);
}
uint32_t kh_audio_out_pull(float *out, uint32_t frames){
    uint32_t got = 0;
    unsigned gate=atomic_load(&cut_gate);
    int reading=!(gate&PAUSED) && atomic_compare_exchange_strong(&cut_gate,&gate,gate|READING);
    if(reading && ring && atomic_load_explicit(&enabled, RLX)){
        uint64_t r = atomic_load_explicit(&rpos, RLX), w = atomic_load_explicit(&wpos, memory_order_acquire);
        uint32_t have = (uint32_t)(w - r); got = have < frames ? have : frames;
        /* Allow the first restored slice to fill before starting its fade-in. */
        if(gate!=seen_gate || await_mix){
            uint32_t quantum=KH_AUDIO_OUT_RATE/100;
            uint32_t mark=atomic_load_explicit(&target,RLX);
            uint32_t attainable=mark>quantum?mark-quantum+1:1;
            if(have<(frames<attainable?frames:attainable)) got=0;
        }
        if(atomic_load_explicit(&c_min_valid, RLX)){ if(have < atomic_load_explicit(&c_min_fill, RLX)) atomic_store_explicit(&c_min_fill, have, RLX); }
        else if(atomic_load_explicit(&started, RLX) && atomic_load_explicit(&c_produced, RLX)){ atomic_store_explicit(&c_min_fill, have, RLX); atomic_store_explicit(&c_min_valid, 1, RLX); }
        uint32_t at = (uint32_t)(r & mask), first = cap - at < got ? cap - at : got;
        memcpy(out, ring + (size_t)at * KH_AUDIO_OUT_CHANNELS, (size_t)first * KH_AUDIO_OUT_CHANNELS * sizeof(float));
        memcpy(out + (size_t)first * KH_AUDIO_OUT_CHANNELS, ring, (size_t)(got - first) * KH_AUDIO_OUT_CHANNELS * sizeof(float));
        atomic_store_explicit(&rpos, r + got, memory_order_release);
    }
    if(got < frames){
        memset(out + (size_t)got * KH_AUDIO_OUT_CHANNELS, 0, (size_t)(frames - got) * KH_AUDIO_OUT_CHANNELS * sizeof(float));
        atomic_fetch_add_explicit(&c_underruns, 1, RLX); atomic_fetch_add_explicit(&c_underrun_frames, frames - got, RLX);
    }
    atomic_fetch_add_explicit(&c_pulls, 1, RLX); atomic_fetch_add_explicit(&c_pulled, frames, RLX); atomic_fetch_add_explicit(&c_delivered, got, RLX);
    if(frames){
        if(gate!=seen_gate || (await_mix && got)){
            seen_gate=gate;
            await_mix=!got;
            fade_left=FADE_FRAMES;
            memcpy(fade_from,last_sample,sizeof fade_from);
        }
        for(uint32_t i=0;i<frames && fade_left;i++,fade_left--){
            float previous=(float)(fade_left-1)/FADE_FRAMES;
            for(unsigned c=0;c<2;c++)
                out[2*i+c]=previous*fade_from[c]+(1.0f-previous)*out[2*i+c];
        }
        memcpy(last_sample,out+2*(size_t)(frames-1),sizeof last_sample);
    }
    if(reading) atomic_fetch_and(&cut_gate,~(unsigned)READING);
    return got;
}
/* review of task 28: up to the ring minus one pass (was: below the ring, which refused 2 x 4096 = 8192 in the 8192-frame ring of then) */
int kh_audio_out_set_target(uint32_t frames){ uint32_t q = KH_AUDIO_OUT_RATE / 100; if(!ring || frames < 2 * q || frames > cap - q) return -1; atomic_store_explicit(&target, frames, RLX); return 0; }
void kh_audio_out_format(KhAudioOutFormat *f){
    f->rate = KH_AUDIO_OUT_RATE; f->channels = KH_AUDIO_OUT_CHANNELS; f->float32 = 1; f->interleaved = 1;
    f->ring_frames = cap; f->target_frames = atomic_load_explicit(&target, RLX); f->quantum_frames = KH_AUDIO_OUT_RATE / 100;
}
void kh_audio_out_stats(KhAudioOutStats *s){
    memset(s, 0, sizeof *s);
    s->produced_frames = atomic_load(&c_produced); s->pulled_frames = atomic_load(&c_pulled); s->delivered_frames = atomic_load(&c_delivered);
    s->underruns = atomic_load(&c_underruns); s->underrun_frames = atomic_load(&c_underrun_frames); s->overruns = atomic_load(&c_overruns); s->overrun_frames = atomic_load(&c_overrun_frames);
    s->stopped_passes = atomic_load(&c_stopped); s->pulls = atomic_load(&c_pulls); s->starts = atomic_load(&c_starts); s->stops = atomic_load(&c_stops);
    s->max_fill = atomic_load(&c_max_fill); s->min_fill_started = atomic_load(&c_min_valid) ? atomic_load(&c_min_fill) : 0;
}

/* ---- the simulated device (KHNMC_AUDIO=pull-test:<file>) ----------------------------------------------------------------------- */
static const uint32_t sim_sizes[6] = {128, 256, 441, 480, 512, 1024};
static pthread_t sim_th; static _Atomic int sim_quit, sim_running; static KmxWav *sim_wav; static FILE *sim_gaps; static char sim_path[1100];
static uint64_t sim_pos, sim_size_calls[6], sim_late, sim_burst, sim_max_late_ns; static uint32_t sim_seed;
static float sim_buf[1024 * KH_AUDIO_OUT_CHANNELS];
static uint64_t sim_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void sim_sleep_until(uint64_t t){ struct timespec d = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)}; kh_sleep_until(&d); }   /* kh_compat.h: Darwin has no clock_nanosleep */
static uint32_t sim_rand(void){ uint32_t x = sim_seed; x ^= x << 13; x ^= x >> 17; x ^= x << 5; sim_seed = x; return x; }
static void sim_take(uint32_t n){
    uint32_t got = kh_audio_out_pull(sim_buf, n);
    if(got < n && sim_gaps) fprintf(sim_gaps, "%llu %u\n", (unsigned long long)(sim_pos + got), n - got);
    if(kmx_wav_write(sim_wav, sim_buf, n)){ fprintf(stderr, "kh: FATAL: audio-out: simulated device: writing %s failed: %s\n", sim_path, strerror(errno)); abort(); }
    sim_pos += n;
}
/* irregular schedule, real time on average: the device clock advances n frames per call; a call is made on time (60 %), late by up
 * to 8 ms (15 %), late by up to 30 ms (5 %: provokes underruns at the 2048-frame target), or at once after the previous one (20 %: a
 * burst that runs ahead of the device clock until the next on-time call catches it up) */
static void *sim_main(void *arg){
    (void)arg; kh_audio_out_start();
    uint64_t due = sim_now();
    for(uint64_t i = 0; !atomic_load(&sim_quit); i++){
        uint32_t k = (uint32_t)(i % 6); if(sim_rand() % 4 == 0) k = sim_rand() % 6;           /* every size in turn, a quarter of them shuffled */
        uint32_t n = sim_sizes[k]; sim_size_calls[k]++;
        uint32_t r = sim_rand() % 100; uint64_t at = due;
        if(r < 15){ at += (uint64_t)(sim_rand() % 8000) * 1000ull; sim_late++; }
        else if(r < 20){ at += (uint64_t)(sim_rand() % 30000) * 1000ull; sim_late++; }
        else if(r < 40){ at = 0; sim_burst++; }
        if(at){ uint64_t now = sim_now(); if(at > now) sim_sleep_until(at); else if(now - at > sim_max_late_ns) sim_max_late_ns = now - at; }
        sim_take(n);
        due += (uint64_t)n * 1000000000ull / KH_AUDIO_OUT_RATE;
        uint64_t now = sim_now(); if(now > due + 200000000ull) due = now;                     /* more than 200 ms behind (host stall): resynchronise */
    }
    atomic_store(&sim_running, 0);
    return NULL;
}
int kh_audio_simdev_start(const char *path, uint32_t seed){
    snprintf(sim_path, sizeof sim_path, "%s", path);
    sim_wav = kmx_wav_open(path, KH_AUDIO_OUT_RATE, KH_AUDIO_OUT_CHANNELS); if(!sim_wav) return -1;
    char gp[1200]; snprintf(gp, sizeof gp, "%s.gaps", path); sim_gaps = fopen(gp, "w"); if(!sim_gaps) goto failed;
    fprintf(sim_gaps, "# underrun gaps of the simulated device: <device frame where zeros begin> <zero frames>\n");
    sim_seed = seed ? seed : 0x2545f491u; sim_pos = 0; atomic_store(&sim_quit, 0); atomic_store(&sim_running, 1);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1u << 20);
    kh_audio_out_attach_device(1);
    int rc = pthread_create(&sim_th, &at, sim_main, NULL); pthread_attr_destroy(&at);
    if(rc){ kh_audio_out_attach_device(0); atomic_store(&sim_running, 0); errno=rc; goto failed; }
    return 0;
failed:
    {
        int err=errno;
        if(sim_gaps){ fclose(sim_gaps); sim_gaps=NULL; }
        kmx_wav_close(sim_wav,NULL); sim_wav=NULL;
        errno=err;
        return -1;
    }
}
int kh_audio_simdev_finish(FILE *rep){
    if(!sim_wav) return 0;
    atomic_store(&sim_quit, 1); pthread_join(sim_th, NULL);
    uint32_t left = kh_audio_ring_fill(), drained = 0;                                         /* the end of the run: what is still queued is delivered too */
    while(left){ uint32_t n = left > 1024 ? 1024 : left; sim_take(n); left -= n; drained += n; }
    kh_audio_out_stop();
    kh_audio_out_attach_device(0);
    KmxWavStats ws; int rc = kmx_wav_close(sim_wav, &ws); sim_wav = NULL; if(sim_gaps){ if(fclose(sim_gaps)) rc = -1; sim_gaps = NULL; }
    if(rep) fprintf(rep, "kh: audio-out: simulated device: %llu frames received into %s (%.2f s), callbacks by size 128:%llu 256:%llu 441:%llu 480:%llu 512:%llu 1024:%llu, late calls %llu (worst %.1f ms behind its due time), burst calls %llu, %u frames drained from the ring at the end\n",
                    (unsigned long long)sim_pos, sim_path, (double)sim_pos / KH_AUDIO_OUT_RATE, (unsigned long long)sim_size_calls[0], (unsigned long long)sim_size_calls[1], (unsigned long long)sim_size_calls[2],
                    (unsigned long long)sim_size_calls[3], (unsigned long long)sim_size_calls[4], (unsigned long long)sim_size_calls[5], (unsigned long long)sim_late, sim_max_late_ns / 1e6, (unsigned long long)sim_burst, drained);
    return rc;
}
