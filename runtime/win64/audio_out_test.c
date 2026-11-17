/* audio_out_test.c : selftest of the pull ring (runtime/audio_out.c, khnmc_audio_out.h; TASK28).  Host code only.
 * A producer thread pushes 480-frame passes whose samples count the frames (left = n, right = -n), a consumer thread pulls with the
 * callback sizes of a real device (128, 256, 441, 480, 512, 1024) and irregular sleeps; every frame must arrive once, in order, the
 * zeros of an underrun must be exactly the counted underrun frames, and a full ring must refuse a push without writing. */
#include "khnmc_audio_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr, "audio-out selftest FAIL: %s (line %d)\n", #c, __LINE__); } }while(0)
#define TOTAL (480u * 600u)                                  /* 6 s of frames through a 1024-frame ring */
static _Atomic int prod_done;
static void nap(uint32_t us){ struct timespec ts = {0, (long)us * 1000L}; nanosleep(&ts, NULL); }
static void *producer(void *arg){
    (void)arg; float buf[480 * 2]; uint32_t n = 0, x = 7;
    while(n < TOTAL){
        for(uint32_t i = 0; i < 480; i++){ buf[2 * i] = (float)(n + i); buf[2 * i + 1] = -(float)(n + i); }
        while(kh_audio_ring_push(buf, 480)) nap(50);
        n += 480; x = x * 1664525u + 1013904223u; if((x >> 28) == 0) nap(3000);      /* now and then late: underruns */
    }
    atomic_store(&prod_done, 1); return NULL;
}
int kh_audio_out_selftest(void){
    fails = checks = 0;
    kh_audio_ring_setup(0, 0, 0);
    CHECK(kh_audio_out_start() == -1);                        /* not the selected backend */
    kh_audio_ring_setup(1, 1000, 640);                        /* rounded up to 1024 */
    KhAudioOutFormat f; kh_audio_out_format(&f);
    CHECK(f.rate == 48000 && f.channels == 2 && f.float32 == 1 && f.interleaved == 1 && f.ring_frames == 1024 && f.target_frames == 640 && f.quantum_frames == 480);
    /* empty ring: zeros, counted */
    float out[1024 * 2]; memset(out, 0x55, sizeof out);
    CHECK(kh_audio_out_pull(out, 300) == 0 && out[0] == 0.0f && out[599] == 0.0f && out[600] != 0.0f);
    KhAudioOutStats st; kh_audio_out_stats(&st); CHECK(st.underruns == 1 && st.underrun_frames == 300 && st.pulls == 1);
    /* full ring: a push that does not fit writes nothing */
    float pass[480 * 2]; for(int i = 0; i < 960; i++) pass[i] = 0.5f;
    CHECK(kh_audio_ring_push(pass, 480) == 0 && kh_audio_ring_push(pass, 480) == 0 && kh_audio_ring_push(pass, 480) == -1 && kh_audio_ring_fill() == 960);
    CHECK(kh_audio_ring_push(NULL, 64) == 0 && kh_audio_ring_fill() == 1024);           /* NULL = silence */
    CHECK(kh_audio_out_pull(out, 1024) == 1024 && out[0] == 0.5f && out[1919] == 0.5f && out[1920] == 0.0f && out[2047] == 0.0f);
    /* threads: every frame once and in order, the wrap included */
    kh_audio_ring_setup(1, 1024, 640); CHECK(kh_audio_out_start() == 0 && kh_audio_ring_started() == 1);
    atomic_store(&prod_done, 0); pthread_t th; pthread_create(&th, NULL, producer, NULL);
    static const uint32_t sizes[6] = {128, 256, 441, 480, 512, 1024};
    uint32_t next = 0, x = 3, bad = 0, zeros = 0, k = 0;
    while(next < TOTAL){
        uint32_t n = sizes[k++ % 6], got = kh_audio_out_pull(out, n);
        for(uint32_t i = 0; i < got; i++) if(out[2 * i] != (float)next + (float)i || out[2 * i + 1] != -((float)next + (float)i)) bad++;
        for(uint32_t i = got; i < n; i++) if(out[2 * i] != 0.0f || out[2 * i + 1] != 0.0f) bad++; else zeros++;
        next += got;
        x = x * 1664525u + 1013904223u; uint32_t r = x >> 28; if(r < 10) nap(n * 1000000u / 48000u); else if(r < 13) nap(5000);   /* real time, late, or a burst */
        if(atomic_load(&prod_done) && !kh_audio_ring_fill() && next < TOTAL){ bad++; break; }
    }
    pthread_join(th, NULL);
    kh_audio_out_stats(&st);
    CHECK(bad == 0 && next == TOTAL);
    CHECK(st.produced_frames == TOTAL && st.delivered_frames == TOTAL && st.underrun_frames == zeros && st.pulled_frames == TOTAL + zeros);
    CHECK(st.underruns > 0 && st.max_fill <= 1024);
    kh_audio_out_stop(); CHECK(kh_audio_ring_started() == 0);
    unsigned long long thr_underruns = (unsigned long long)st.underruns;
    /* review of task 28: the ring of KHNMC_AUDIO=pull (the sizes null_audio.c uses) takes the target of a device with 4096-frame slices
     * (iOS, screen locked: the CoreAudio glue asks for twice its slice), up to the ring minus one pass; a refused target changes nothing.
     * With that target and the mixer's policy (a pass only while fill + 480 <= target), 4096-frame pulls never come up short.  Before:
     * an 8192-frame ring refused 8192, the glue ignored the -1, the target stayed 2048 and every 4096-frame pull underran by half. */
    kh_audio_ring_setup(1, KH_AUDIO_OUT_RING_FRAMES, KH_AUDIO_OUT_TARGET_FRAMES); kh_audio_out_format(&f);
    CHECK(f.ring_frames >= 2 * KH_AUDIO_OUT_MAX_SLICE + f.quantum_frames && f.target_frames == KH_AUDIO_OUT_TARGET_FRAMES);
    CHECK(kh_audio_out_set_target(f.ring_frames - f.quantum_frames + 1) == -1); kh_audio_out_format(&f); CHECK(f.target_frames == KH_AUDIO_OUT_TARGET_FRAMES);
    CHECK(kh_audio_out_set_target(2 * f.quantum_frames - 1) == -1);
    CHECK(kh_audio_out_set_target(f.ring_frames - f.quantum_frames) == 0);
    kh_audio_ring_setup(1, KH_AUDIO_OUT_RING_FRAMES, KH_AUDIO_OUT_TARGET_FRAMES);           /* back to the default target: the 4096 case on its own */
    CHECK(kh_audio_out_set_target(2 * KH_AUDIO_OUT_MAX_SLICE) == 0); kh_audio_out_format(&f); CHECK(f.target_frames == 2 * KH_AUDIO_OUT_MAX_SLICE);
    CHECK(kh_audio_out_start() == 0);
    { static float big[KH_AUDIO_OUT_MAX_SLICE * 2]; uint32_t short_pulls = 0, refused = 0;
      for(int s = 0; s < 64; s++){
          while(kh_audio_ring_fill() + f.quantum_frames <= f.target_frames) refused += kh_audio_ring_push(NULL, f.quantum_frames) != 0;   /* the mixer's policy */
          short_pulls += kh_audio_out_pull(big, KH_AUDIO_OUT_MAX_SLICE) != KH_AUDIO_OUT_MAX_SLICE;
      }
      CHECK(refused == 0 && short_pulls == 0); kh_audio_out_stats(&st); CHECK(st.underruns == 0 && st.max_fill <= f.target_frames); }
    kh_audio_out_stop();
    kh_audio_ring_setup(0, 0, 0);
    fprintf(stderr, "kh: audio-out selftest: %d checks, %d failures (%u frames through a 1024-frame ring, %llu underruns of %u zero frames)\n", checks, fails, (unsigned)TOTAL, thr_underruns, zeros);
    return fails;
}
