/* khnmc_mix.h : software XAudio2-style mixer core (host code only, no guest dependency).
 *
 * One KmxEngine = one IXAudio2: source voices (buffer queues, PCM8/16/24/32, float32, MS-ADPCM), submix voices, one mastering voice,
 * a deterministic float32 processing pass of 10 ms (rate/100 frames per voice), per-voice volume, channel volumes, output matrices,
 * frequency-ratio / fixed-rate resampling (4-point Catmull-Rom, exact rational phase), effect chains (built-in volume meter,
 * counted pass-through, host-hosted effects such as guest XAPOs) and the XAudio2 callback order.
 *
 * Voice pipeline (the order FAudio documents for XAudio2):
 *   source : queue -> decode -> resample to the destination's input rate -> effect chain -> volume * channel volume * matrix -> sends
 *   submix : input mix (own rate) -> resample to the destination's input rate -> volume -> effect chain -> channel volume * matrix -> sends
 *   master : input mix -> volume -> effect chain -> channel volume -> engine output
 *
 * Threading: every API call takes the engine lock.  kmx_engine_pass() runs one quantum on the calling thread and RELEASES the lock
 * around every host callback (voice/engine events, hosted effect Process), so callbacks may call any API of the same engine and
 * other threads are never blocked behind guest code.  While a voice is being processed it is marked busy: destroy / set_chain /
 * set_outputs from another thread wait for it (XAudio2's DestroyVoice blocks the same way).  A voice destroyed from inside its own
 * callback is dropped at the end of the pass. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KMX_MAXCH 64
#define KMX_MAX_QUEUED 64
#define KMX_LOOP_INFINITE 255u
#define KMX_MAX_LOOP_COUNT 254u
#define KMX_END_OF_STREAM 0x40u
#define KMX_PLAY_TAILS 0x20u
#define KMX_OK 0
#define KMX_E_INVALID (-1)          /* XAUDIO2_E_INVALID_CALL */
#define KMX_E_UNSUPPORTED (-2)      /* valid XAudio2, not implemented here: the caller must fail loudly */
#define KMX_E_TIMEOUT (-3)          /* a busy voice did not become idle (callback deadlock) */

enum { KMX_SOURCE = 0, KMX_SUBMIX = 1, KMX_MASTER = 2 };
enum { KMX_TAG_PCM = 1, KMX_TAG_ADPCM = 2, KMX_TAG_FLOAT = 3 };
enum { KMX_FX_METER = 1, KMX_FX_PASS = 2, KMX_FX_HOST = 3 };
enum { KMX_EV_PASS_START = 0, KMX_EV_PASS_END = 1, KMX_EV_STREAM_END = 2, KMX_EV_BUFFER_START = 3, KMX_EV_BUFFER_END = 4, KMX_EV_LOOP_END = 5 };

typedef struct KmxEngine KmxEngine;
typedef struct KmxVoice KmxVoice;

typedef struct {
    int tag;                        /* KMX_TAG_* */
    uint32_t channels, rate, bits;  /* bits: 8/16/24/32 for PCM, 32 for float, 4 for ADPCM */
    uint32_t block_align;           /* bytes per frame (PCM/float) or bytes per ADPCM block */
    uint32_t samples_per_block;     /* ADPCM */
    uint32_t ncoef; int16_t coef[32][2];
} KmxFormat;

typedef struct {
    uint32_t flags, bytes;          /* flags: KMX_END_OF_STREAM */
    const uint8_t *data;
    uint32_t play_begin, play_length, loop_begin, loop_length, loop_count;
    uint64_t context;
} KmxBuffer;

typedef struct { int kind; void *user; uint32_t out_ch; int enabled; int inplace; } KmxFxDesc;

typedef struct {
    void *(*alloc)(size_t bytes);   /* zeroed, 64-byte aligned; NULL = calloc-based default */
    void (*release)(void *p);
    void (*engine_event)(void *ctx, void *engine_user, int ev);
    void (*voice_event)(void *ctx, void *voice_user, int ev, uint64_t arg);   /* PASS_START: arg = bytes required; BUFFER_x / LOOP_END: arg = buffer context */
    /* one hosted effect, one quantum.  in/out are interleaved float32; in == out when the effect was declared in-place. */
    void (*fx_process)(void *ctx, void *fx_user, float *in, uint32_t in_ch, int in_silent, float *out, uint32_t out_ch, uint32_t frames, int enabled, int *out_silent);
    void (*voice_free)(void *voice_user);                                       /* the voice is gone; no callback will follow */
} KmxHost;

typedef struct {
    uint64_t passes, source_voices_created, submix_voices_created, master_voices_created, voices_destroyed;
    uint64_t buffers_submitted, bytes_submitted, buffers_flushed, buffers_ended, loops_ended, streams_ended, buffers_started;
    uint64_t voice_pass_callbacks, engine_pass_callbacks, starvations, fx_pass_through, fx_host_process, fx_meter, frames_decoded, frames_resampled;
    uint64_t missing_default_matrix;   /* passes x sends that had no output matrix and no built-in default: must be treated as fatal */
    uint32_t live_source, live_submix, live_master, active_source, peak_live_source, peak_active_source, peak_queued;
} KmxStats;

KmxEngine *kmx_engine_create(const KmxHost *host, void *user, uint32_t device_rate);
void kmx_engine_destroy(KmxEngine *e);                 /* destroys every remaining voice (voice_free is called for each) */
void kmx_engine_set_callbacks(KmxEngine *e, int on);   /* engine pass callbacks; turning off waits for a running one */
void kmx_engine_start(KmxEngine *e);
void kmx_engine_stop(KmxEngine *e);
int  kmx_engine_running(KmxEngine *e);
uint32_t kmx_engine_rate(KmxEngine *e);
uint32_t kmx_engine_quantum(KmxEngine *e);             /* device frames per pass */
void kmx_engine_stats(KmxEngine *e, KmxStats *out);
/* One 10 ms pass.  *out = interleaved float32 master output (engine-owned copy, valid until the next pass), *out_ch its channel count, quantum frames.
 * Without a mastering voice or while stopped: *out = NULL (silence).  Returns KMX_OK. */
int  kmx_engine_pass(KmxEngine *e, void *ctx, const float **out, uint32_t *out_ch);

int  kmx_voice_create_source(KmxEngine *e, const KmxFormat *f, uint32_t flags, float max_ratio, void *user, int has_callback, KmxVoice **out);
int  kmx_voice_create_submix(KmxEngine *e, uint32_t channels, uint32_t rate, uint32_t flags, uint32_t stage, void *user, KmxVoice **out);
int  kmx_voice_create_master(KmxEngine *e, uint32_t channels, uint32_t rate, uint32_t flags, void *user, KmxVoice **out);
/* Wait until the pass is not inside this voice (unless called from the pass thread itself) and stop processing it. */
int  kmx_voice_quiesce(KmxVoice *v);
void kmx_voice_destroy(KmxVoice *v);
int  kmx_voice_set_outputs(KmxVoice *v, KmxVoice *const *dests, const uint32_t *send_flags, uint32_t n, int use_default);
int  kmx_voice_set_chain(KmxVoice *v, const KmxFxDesc *fx, uint32_t n);
void *kmx_voice_fx_user(KmxVoice *v, uint32_t i);
int  kmx_voice_fx_enable(KmxVoice *v, uint32_t i, int on);
int  kmx_voice_fx_enabled(KmxVoice *v, uint32_t i, int *on);
int  kmx_voice_meter(KmxVoice *v, uint32_t i, float *peak, float *rms, uint32_t channels);
int  kmx_voice_set_volume(KmxVoice *v, float vol);
float kmx_voice_volume(KmxVoice *v);
int  kmx_voice_set_channel_volumes(KmxVoice *v, uint32_t n, const float *vols);
int  kmx_voice_channel_volumes(KmxVoice *v, uint32_t n, float *vols);
int  kmx_voice_set_matrix(KmxVoice *v, KmxVoice *dest, uint32_t src, uint32_t dst, const float *m);
int  kmx_voice_matrix(KmxVoice *v, KmxVoice *dest, uint32_t src, uint32_t dst, float *m);
uint32_t kmx_voice_in_channels(KmxVoice *v);
uint32_t kmx_voice_out_channels(KmxVoice *v);          /* after the effect chain */
uint32_t kmx_voice_in_rate(KmxVoice *v);
uint32_t kmx_voice_proc_rate(KmxVoice *v);             /* rate of the effect chain / output */
uint32_t kmx_voice_proc_frames(KmxVoice *v);
int  kmx_voice_kind(KmxVoice *v);
void *kmx_voice_user(KmxVoice *v);

int  kmx_source_start(KmxVoice *v);
int  kmx_source_stop(KmxVoice *v, uint32_t flags);
int  kmx_source_submit(KmxVoice *v, const KmxBuffer *b);
int  kmx_source_flush(KmxVoice *v);
int  kmx_source_discontinuity(KmxVoice *v);
int  kmx_source_exit_loop(KmxVoice *v);
int  kmx_source_state(KmxVoice *v, uint64_t *cur_context, uint32_t *queued, uint64_t *samples_played);
int  kmx_source_starvations(KmxVoice *v, uint64_t *count, uint64_t *first_pass, uint64_t *last_pass);   /* queue ran dry without END_OF_STREAM */
int  kmx_source_set_ratio(KmxVoice *v, float ratio);
float kmx_source_ratio(KmxVoice *v);
int  kmx_source_set_rate(KmxVoice *v, uint32_t rate);

/* codec + helpers, exposed for the tests */
int  kmx_adpcm_decode_block(const uint8_t *block, uint32_t bytes, uint32_t channels, const int16_t coef[][2], uint32_t ncoef, int16_t *out, uint32_t cap_frames);
uint32_t kmx_buffer_frames(const KmxFormat *f, uint32_t bytes);
extern const int16_t kmx_adpcm_std_coef[7][2];

/* 16-bit PCM WAV writer whose header is valid after every kmx_wav_sync() (a crash still leaves a playable file). */
typedef struct KmxWav KmxWav;
typedef struct { uint64_t frames, clipped_samples, nonsilent_frames; double sum_sq; float peak; } KmxWavStats;
KmxWav *kmx_wav_open(const char *path, uint32_t rate, uint32_t channels);
int  kmx_wav_write(KmxWav *w, const float *interleaved, uint32_t frames);   /* NULL = silence; returns 0 or -1 on an I/O error */
int  kmx_wav_sync(KmxWav *w);
int  kmx_wav_close(KmxWav *w, KmxWavStats *stats);
void kmx_wav_stats(KmxWav *w, KmxWavStats *stats);
