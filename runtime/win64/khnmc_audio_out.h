/* khnmc_audio_out.h : the device-neutral half of a real audio output (TASK28).  KHNMC_AUDIO=pull selects it.
 *
 * A phone's audio unit PULLS: its render callback asks for N frames at a time on its own real-time thread and must never block,
 * lock, allocate or make a system call.  This interface is what such a callback calls.  Between the runtime's mixer thread (the
 * "xa2-mix" thread of null_audio.c, the only producer) and the device thread (the only consumer) there is a lock-free
 * single-producer / single-consumer ring of interleaved float32 frames; the two sides share nothing else but atomic counters.
 *
 *   format   48000 Hz, 2 channels, float32, interleaved (L R L R ...); kh_audio_out_format() reports it with the ring sizes.  A device
 *            that runs at another rate converts on its side (an Apple output unit does: its input scope takes this format).
 *   start    kh_audio_out_start(): the device is about to pull.  From then on the MIXER FOLLOWS THE DEVICE (policy below).
 *   pull     kh_audio_out_pull(buf, frames): copies `frames` frames into buf; whatever the ring does not hold is written as zeros (an
 *            UNDERRUN: counted, in events and in frames).  Returns the frames that came from the mix.  Any `frames` value is accepted
 *            (one call is split at the ring's end internally).  Real-time safe: two atomic loads, memcpy, two atomic stores.
 *   stop     kh_audio_out_stop(): the device stopped pulling (app in background, route change, end of run).
 *   counters kh_audio_out_stats(): frames produced / pulled / delivered, underruns, overruns, fill level extremes.
 *
 * The mixer's clock in this mode (null_audio.c, mix_thread_pull / mix_thread_virtual):
 *   - The device's consumption is the clock.  The mixer runs its next 10 ms pass (480 frames) only when the ring holds less than the
 *     TARGET fill (kh_audio_out_format().target_frames) minus one pass, i.e. it keeps the ring between target - 480 and target.  It
 *     waits for that by sleeping for the device time the excess represents (frames above the mark / 48 kHz, 0.5 .. 5 ms per sleep):
 *     the device thread never signals anything, so it never makes a system call for the mixer's sake.
 *   - With KHNMC_CLOCK=host the passes are therefore spaced by what the device consumed, not by CLOCK_MONOTONIC.  With
 *     KHNMC_CLOCK=frames the passes stay on the virtual clock (pass n at virtual instant t0 + n * 10 ms, as in the WAV path) and the
 *     wait happens before a pass, while the mixer is the clock's client: the virtual clock, and with it the game, cannot run ahead of
 *     the device by more than the target fill.  The clock's host-time floor (KHNMC_CLOCK_PACE=realtime) is then dropped unless it was
 *     set explicitly: the device is the pace, and after a lag (a load) the clock may run faster than real time until the ring is full.
 *   - UNDERRUN (the device asks for more than the ring holds): the missing frames are zeros, counted.  The mixer does NOT skip ahead:
 *     no pass is dropped and none is shortened, the mix continues sample for sample where it was, later by the length of the gap
 *     (what XAudio2 does after a glitch: its sample position is the samples it rendered, not the wall clock).  The ring is then
 *     below its mark, so the mixer produces passes back to back until the target fill is restored.
 *   - OVERRUN (a pass does not fit because the device stopped consuming without kh_audio_out_stop): the mixer waits up to 2 s of host
 *     time for room; after that the pass is dropped and counted (frames and events), and the mixer goes on.  It never blocks forever.
 *   - Before kh_audio_out_start and after kh_audio_out_stop there is no device: the mixer runs on the fallback clock of the WAV path
 *     (CLOCK_MONOTONIC, or the virtual clock) and its passes are counted as "produced while stopped" and discarded, not queued.
 *
 * KHNMC_AUDIO=pull-test:<file.wav> adds a SIMULATED device: a host thread (no guest state) that starts the device, pulls with
 * callback sizes cycling through 128, 256, 441, 480, 512, 1024 frames with irregular scheduling (on average real time: bursts of
 * back-to-back calls, late calls, short sleeps), writes everything it received to <file.wav> (16-bit, the WAV path's converter) and
 * the underrun gaps to <file.wav>.gaps, and drains the ring at the end of the run.  The mixer's own output of the same run is written
 * to <file.wav>.mix.wav by the WAV path, so the two can be compared sample for sample (tools/pull_compare.py).
 * Apple glue that calls this interface from an output unit's render callback: runtime/ios/audio_coreaudio.c (UNRUN). */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define KH_AUDIO_OUT_RATE 48000u
#define KH_AUDIO_OUT_CHANNELS 2u
/* ring sizes of KHNMC_AUDIO=pull (null_audio.c sets the ring up with these; review of task 28): the default target fill is 2048 frames
 * (43 ms: above one 1024-frame callback and two passes); the ring holds 16384 frames so that a device with slices of up to
 * KH_AUDIO_OUT_MAX_SLICE frames (iOS while the screen is locked) can raise the target to twice its slice and still leave a pass free */
#define KH_AUDIO_OUT_TARGET_FRAMES 2048u
#define KH_AUDIO_OUT_MAX_SLICE 4096u
#define KH_AUDIO_OUT_RING_FRAMES 16384u

typedef struct {
    uint32_t rate, channels;          /* 48000, 2 */
    uint32_t float32, interleaved;    /* 1, 1 */
    uint32_t ring_frames;             /* capacity of the ring */
    uint32_t target_frames;           /* the mixer keeps the ring between target - quantum and target */
    uint32_t quantum_frames;          /* frames per mixer pass (480) */
} KhAudioOutFormat;

typedef struct {
    uint64_t produced_frames;         /* pushed into the ring by the mixer */
    uint64_t pulled_frames;           /* asked for by the device */
    uint64_t delivered_frames;        /* of those, frames that came from the mix */
    uint64_t underruns, underrun_frames;      /* pulls that found too little; zero frames written */
    uint64_t overruns, overrun_frames;        /* passes that found no room for 2 s and were dropped */
    uint64_t stopped_passes;          /* passes made while no device was started (discarded) */
    uint64_t pulls, starts, stops;
    uint32_t max_fill, min_fill_started;      /* ring fill extremes (min: at a pull, after the first pass while started) */
} KhAudioOutStats;

#ifdef __cplusplus
extern "C" {
#endif
/* device side */
int      kh_audio_out_start(void);                                   /* 0 ok; -1: KHNMC_AUDIO=pull is not the selected backend */
void     kh_audio_out_stop(void);
uint32_t kh_audio_out_pull(float *interleaved, uint32_t frames);    /* real-time safe: no lock, no allocation, no system call */
void     kh_audio_out_format(KhAudioOutFormat *f);
void     kh_audio_out_stats(KhAudioOutStats *s);
int      kh_audio_out_set_target(uint32_t frames);           /* before start: a larger target fill (a device with large callbacks); 0 ok, -1 (nothing changed) unless 2 passes <= frames <= ring - 1 pass */
/* the Apple glue (runtime/ios/audio_coreaudio.c, UNRUN, only with -DKH_AUDIO_COREAUDIO=1): an output unit that pulls through this interface */
int      kh_coreaudio_start(void);
void     kh_coreaudio_stop(void);

/* producer side (runtime only: null_audio.c) */
int      kh_audio_ring_enabled(void);                                /* 1 = KHNMC_AUDIO=pull / pull-test */
int      kh_audio_ring_started(void);
uint32_t kh_audio_ring_fill(void);
int      kh_audio_ring_push(const float *interleaved, uint32_t frames);    /* NULL = silence; 0 ok, -1 no room (nothing written) */
void     kh_audio_ring_note(int overrun_frames, int stopped_pass);  /* the producer's counters */
void     kh_audio_ring_setup(int enable, uint32_t ring_frames, uint32_t target_frames);   /* before the first pass; tests reset with it */
/* the simulated device of KHNMC_AUDIO=pull-test (runtime/audio_out.c) */
int      kh_audio_simdev_start(const char *wav_path, uint32_t seed);
int      kh_audio_simdev_finish(FILE *report);                       /* stop pulling, drain the ring into the file, close it; 0 ok */
#ifdef __cplusplus
}
#endif
