/* audio_mix_test.c : unit tests of the software mixer core (audio_mix.c) with host callbacks only: MS-ADPCM against hand-built
 * blocks, PCM decoders, exact 1:1 playback, frequency-ratio and fixed-rate resampling, matrix routing, volumes and ramps, the
 * callback order, buffer queue / loop / flush / stop semantics, effect chain plumbing, the meter, determinism, the WAV writer.
 * Part of `khnmc --selftest` (kh_audio_mix_selftest). */
#include "khnmc_mix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>

static int fails, checks;
#define CHECK(c) do{ checks++; if(!(c)){ fails++; fprintf(stderr, "audio mix selftest FAIL: %s (line %d)\n", #c, __LINE__); } }while(0)
#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

/* ---- event recorder ---- */
typedef struct { int ev; uint64_t arg; void *user; } Ev;
static Ev evlog[8192]; static int nev;
static void (*hook)(void *ctx, void *user, int ev, uint64_t arg);
static int freed_count; static void *last_freed;
#define EV_ENGINE 100
static void t_voice_event(void *ctx, void *user, int ev, uint64_t arg){
    if(nev < 8192){ evlog[nev].ev = ev; evlog[nev].arg = arg; evlog[nev].user = user; nev++; }
    if(hook) hook(ctx, user, ev, arg);
}
static void t_engine_event(void *ctx, void *eu, int ev){ (void)ctx; if(nev < 8192){ evlog[nev].ev = EV_ENGINE + ev; evlog[nev].arg = 0; evlog[nev].user = eu; nev++; } }
static void t_voice_free(void *user){ freed_count++; last_freed = user; }
/* hosted test effect: user points at one of these */
typedef struct { float gain; int calls, last_enabled, last_in_silent, set_silent; uint32_t last_in_ch, last_out_ch, last_frames; int inplace_seen; } TFx;
static void t_fx(void *ctx, void *fu, float *in, uint32_t ic, int in_silent, float *out, uint32_t oc, uint32_t n, int enabled, int *out_silent){
    (void)ctx; TFx *f = fu; f->calls++; f->last_enabled = enabled; f->last_in_silent = in_silent; f->last_in_ch = ic; f->last_out_ch = oc; f->last_frames = n; f->inplace_seen = in == out;
    float g = enabled ? f->gain : 1.0f;
    for(uint32_t k = 0; k < n; k++){
        if(ic == oc) for(uint32_t c = 0; c < oc; c++) out[k * oc + c] = in[k * ic + c] * g;
        else { float s = 0.0f; for(uint32_t c = 0; c < ic; c++) s += in[k * ic + c]; for(uint32_t c = 0; c < oc; c++) out[k * oc + c] = s * g; }   /* sum of the inputs on every output */
    }
    *out_silent = f->set_silent;
}
static const KmxHost thost = { NULL, NULL, t_engine_event, t_voice_event, t_fx, t_voice_free };

static KmxFormat pcm_fmt(uint32_t ch, uint32_t rate, uint32_t bits){ KmxFormat f; memset(&f, 0, sizeof f); f.tag = KMX_TAG_PCM; f.channels = ch; f.rate = rate; f.bits = bits; f.block_align = ch * bits / 8; return f; }
static KmxBuffer mkbuf(const void *data, uint32_t bytes, uint32_t flags, uint64_t ctx){ KmxBuffer b; memset(&b, 0, sizeof b); b.data = data; b.bytes = bytes; b.flags = flags; b.context = ctx; return b; }
/* count the recorded events of one kind for one voice (user NULL = any) */
static int count_ev(int ev, void *user){ int n = 0; for(int i = 0; i < nev; i++) if(evlog[i].ev == ev && (!user || evlog[i].user == user)) n++; return n; }
/* the recorded events of a voice without the pass start/end pairs, as a compact string: S=start E=end L=loop X=stream end */
static const char *seq(void *user){
    static char s[512]; int n = 0;
    for(int i = 0; i < nev && n < 500; i++){ if(user && evlog[i].user != user) continue;
        switch(evlog[i].ev){ case KMX_EV_BUFFER_START: s[n++] = 'S'; break; case KMX_EV_BUFFER_END: s[n++] = 'E'; break; case KMX_EV_LOOP_END: s[n++] = 'L'; break; case KMX_EV_STREAM_END: s[n++] = 'X'; break; default: continue; }
        if(evlog[i].ev != KMX_EV_STREAM_END) s[n++] = (char)('0' + (int)(evlog[i].arg % 10)); }
    s[n] = 0; return s;
}
static void pass(KmxEngine *e, float *dst, uint32_t ch_expect){
    const float *o = NULL; uint32_t oc = 0; int rc = kmx_engine_pass(e, NULL, &o, &oc);
    if(rc != KMX_OK){ fails++; fprintf(stderr, "audio mix selftest FAIL: pass rc %d\n", rc); }
    uint32_t q = kmx_engine_quantum(e);
    if(dst){ if(o && oc == ch_expect) memcpy(dst, o, sizeof(float) * q * oc); else memset(dst, 0, sizeof(float) * q * ch_expect); }
}

/* ---- MS-ADPCM against hand-computed blocks ---- */
static void test_adpcm(void){
    /* mono, predictor 1 (512,-256 => 2*s1 - s2), delta 16, s1 = 1000, s2 = 900, codes 3 F 7 8 4 0 C 1.  Worked by hand:
     *  +3: 1100 + 48 = 1148, delta max(16, 230*16>>8) = 16 ; -1: 1296 - 16 = 1280, delta 16 ; +7: 1412 + 112 = 1524, delta 614*16>>8 = 38 ;
     *  -8: 1768 - 304 = 1464, delta 768*38>>8 = 114 ; +4: 1404 + 456 = 1860, delta 307*114>>8 = 136 ; 0: 2256, delta 230*136>>8 = 122 ;
     *  -4: 2652 - 488 = 2164, delta 307*122>>8 = 146 ; +1: 2072 + 146 = 2218. */
    static const uint8_t mono[11] = {1, 0x10, 0x00, 0xE8, 0x03, 0x84, 0x03, 0x3F, 0x78, 0x40, 0xC1};
    static const int16_t mono_ref[10] = {900, 1000, 1148, 1280, 1524, 1464, 1860, 2256, 2164, 2218};
    int16_t out[32]; memset(out, 0x55, sizeof out);
    CHECK(kmx_adpcm_decode_block(mono, 11, 1, kmx_adpcm_std_coef, 7, out, 10) == 10);
    CHECK(!memcmp(out, mono_ref, sizeof mono_ref)); CHECK(out[10] == 0x5555);
    CHECK(kmx_adpcm_decode_block(mono, 11, 1, kmx_adpcm_std_coef, 7, out, 9) == 0);      /* output too small */
    CHECK(kmx_adpcm_decode_block(mono, 6, 1, kmx_adpcm_std_coef, 7, out, 10) == 0);       /* shorter than a header */
    uint8_t bad[11]; memcpy(bad, mono, 11); bad[0] = 7;
    CHECK(kmx_adpcm_decode_block(bad, 11, 1, kmx_adpcm_std_coef, 7, out, 10) == 0);       /* predictor index out of range */
    /* stereo: ch0 predictor 3 (192,64), delta 16, s1 = -5, s2 = -3 ; ch1 predictor 0 (256,0), delta 32, s1 = 100, s2 = 50.
     * byte 0x02: ch0 code 0: (-960 - 192) / 256 = -4.5 -> -4 (truncation toward zero; a shift would give -5) ; ch1 code 2: 100 + 64 = 164, delta 230*32>>8 = 28
     * byte 0x9E: ch0 code -7: (-768 - 320) / 256 = -4.25 -> -4, -4 - 7*16 = -116 ; ch1 code -2: 164 - 56 = 108 */
    static const uint8_t st[16] = {3, 0, 0x10, 0x00, 0x20, 0x00, 0xFB, 0xFF, 0x64, 0x00, 0xFD, 0xFF, 0x32, 0x00, 0x02, 0x9E};
    static const int16_t st_ref[8] = {-3, 50, -5, 100, -4, 164, -116, 108};
    CHECK(kmx_adpcm_decode_block(st, 16, 2, kmx_adpcm_std_coef, 7, out, 4) == 4);
    CHECK(!memcmp(out, st_ref, sizeof st_ref));
    /* clamp: predictor 1, s1 = 32767, s2 = 0 predicts 65534 -> 32767 */
    static const uint8_t cl[8] = {1, 0x10, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00};
    CHECK(kmx_adpcm_decode_block(cl, 8, 1, kmx_adpcm_std_coef, 7, out, 4) == 4 && out[0] == 0 && out[1] == 32767 && out[2] == 32767);
    KmxFormat f; memset(&f, 0, sizeof f); f.tag = KMX_TAG_ADPCM; f.channels = 2; f.block_align = 140; f.samples_per_block = 128;
    CHECK(kmx_buffer_frames(&f, 1400) == 1280 && kmx_buffer_frames(&f, 1400 + 14) == 1282 && kmx_buffer_frames(&f, 1400 + 13) == 1280 && kmx_buffer_frames(&f, 1400 + 16) == 1284);
    /* through a voice: the two hand blocks back to back, played 1:1 into a mono master */
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; CHECK(kmx_voice_create_master(e, 1, 48000, 0, NULL, &m) == KMX_OK);
    memset(&f, 0, sizeof f); f.tag = KMX_TAG_ADPCM; f.channels = 1; f.rate = 48000; f.bits = 4; f.block_align = 11; f.samples_per_block = 10; f.ncoef = 7; memcpy(f.coef, kmx_adpcm_std_coef, sizeof kmx_adpcm_std_coef);
    CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_OK);
    CHECK(kmx_voice_set_outputs(s, NULL, NULL, 0, 1) == KMX_OK);
    uint8_t two[22]; memcpy(two, mono, 11); memcpy(two + 11, mono, 11);
    KmxBuffer b = mkbuf(two, 22, KMX_END_OF_STREAM, 1);
    CHECK(kmx_source_submit(s, &b) == KMX_OK); kmx_source_start(s);
    float o[480]; pass(e, o, 1); int ok = 1;
    for(int i = 0; i < 20; i++) if(o[i] != (float)mono_ref[i % 10] / 32768.0f) ok = 0;
    CHECK(ok); CHECK(o[20] == 0.0f && o[479] == 0.0f);
    f.samples_per_block = 9; KmxVoice *x; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &x) == KMX_E_INVALID);   /* block size and samples per block disagree */
    f.samples_per_block = 10; f.channels = 3; f.block_align = 33; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &x) == KMX_E_UNSUPPORTED);
    kmx_engine_destroy(e);
}

/* ---- PCM widths and float ---- */
static void test_formats(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    float o[480];
    { KmxFormat f = pcm_fmt(1, 48000, 8); KmxVoice *s; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_OK); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
      static const uint8_t d[4] = {0, 128, 255, 192}; KmxBuffer b = mkbuf(d, 4, 0, 0); CHECK(kmx_source_submit(s, &b) == KMX_OK); kmx_source_start(s); pass(e, o, 1);
      CHECK(o[0] == -1.0f && o[1] == 0.0f && o[2] == 127.0f / 128.0f && o[3] == 0.5f && o[4] == 0.0f); kmx_voice_destroy(s); }
    { KmxFormat f = pcm_fmt(1, 48000, 16); KmxVoice *s; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
      static const int16_t d[4] = {-32768, 32767, 16384, -1}; KmxBuffer b = mkbuf(d, 8, 0, 0); kmx_source_submit(s, &b); kmx_source_start(s); pass(e, o, 1);
      CHECK(o[0] == -1.0f && o[1] == 32767.0f / 32768.0f && o[2] == 0.5f && o[3] == -1.0f / 32768.0f); kmx_voice_destroy(s); }
    { KmxFormat f = pcm_fmt(1, 48000, 24); KmxVoice *s; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
      static const uint8_t d[9] = {0x00, 0x00, 0x80, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x40}; KmxBuffer b = mkbuf(d, 9, 0, 0); kmx_source_submit(s, &b); kmx_source_start(s); pass(e, o, 1);
      CHECK(o[0] == -1.0f && o[1] == 8388607.0f / 8388608.0f && o[2] == 0.5f); kmx_voice_destroy(s); }
    { KmxFormat f = pcm_fmt(1, 48000, 32); KmxVoice *s; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
      static const int32_t d[2] = {INT32_MIN, 0x40000000}; KmxBuffer b = mkbuf(d, 8, 0, 0); kmx_source_submit(s, &b); kmx_source_start(s); pass(e, o, 1);
      CHECK(o[0] == -1.0f && o[1] == 0.5f); kmx_voice_destroy(s); }
    { KmxFormat f = pcm_fmt(2, 48000, 32); f.tag = KMX_TAG_FLOAT; KmxVoice *s; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_OK); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
      static const float d[4] = {0.25f, 0.5f, -1.5f, 0.125f}; float mtx[2] = {1.0f, 2.0f}; CHECK(kmx_voice_set_matrix(s, NULL, 2, 1, mtx) == KMX_OK);
      KmxBuffer b = mkbuf(d, 16, 0, 0); kmx_source_submit(s, &b); kmx_source_start(s); pass(e, o, 1);
      CHECK(o[0] == 1.25f && o[1] == -1.25f); kmx_voice_destroy(s); }
    { KmxFormat f = pcm_fmt(1, 48000, 12); KmxVoice *s; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_E_INVALID);
      f = pcm_fmt(1, 48000, 16); f.block_align = 3; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_E_INVALID);
      f = pcm_fmt(1, 500, 16); CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_E_INVALID);
      f = pcm_fmt(1, 48000, 16); f.tag = 0x161; CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_E_UNSUPPORTED);
      f = pcm_fmt(1, 48000, 16); CHECK(kmx_voice_create_source(e, &f, 0, 0.0f, NULL, 0, &s) == KMX_E_INVALID); }
    kmx_engine_destroy(e);
}

/* ---- exact playback, callback order, GetState ---- */
static int16_t ramp16[4000];
static void test_playback_order(void){
    for(int i = 0; i < 4000; i++) ramp16[i] = (int16_t)(i * 7 - 9000);
    KmxEngine *e = kmx_engine_create(&thost, (void *)0xE, 48000); kmx_engine_start(e); kmx_engine_set_callbacks(e, 1);
    KmxVoice *m, *s; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    KmxFormat f = pcm_fmt(1, 48000, 16); void *U = (void *)0x51;
    CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, U, 1, &s) == KMX_OK); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    KmxBuffer b = mkbuf(ramp16, 1000 * 2, KMX_END_OF_STREAM, 7); CHECK(kmx_source_submit(s, &b) == KMX_OK);
    uint64_t ctx = 0, played = 9; uint32_t q = 9; kmx_source_state(s, &ctx, &q, &played); CHECK(ctx == 7 && q == 1 && played == 0);
    float o[480]; nev = 0; pass(e, o, 1);                                 /* not started: silence, but the pass callbacks still run with 0 bytes required */
    CHECK(o[0] == 0.0f && nev == 4 && evlog[0].ev == EV_ENGINE + KMX_EV_PASS_START && evlog[1].ev == KMX_EV_PASS_START && evlog[1].arg == 0 && evlog[2].ev == KMX_EV_PASS_END && evlog[3].ev == EV_ENGINE + KMX_EV_PASS_END);
    kmx_source_start(s); nev = 0; pass(e, o, 1);
    int ok = 1; for(int i = 0; i < 480; i++) if(o[i] != (float)ramp16[i] / 32768.0f) ok = 0;
    CHECK(ok);
    CHECK(nev == 5 && evlog[0].ev == EV_ENGINE + KMX_EV_PASS_START && evlog[1].ev == KMX_EV_PASS_START && evlog[1].arg == 0 && evlog[2].ev == KMX_EV_BUFFER_START && evlog[2].arg == 7 && evlog[3].ev == KMX_EV_PASS_END && evlog[4].ev == EV_ENGINE + KMX_EV_PASS_END);
    kmx_source_state(s, &ctx, &q, &played); CHECK(ctx == 7 && q == 1 && played == 480);
    nev = 0; pass(e, o, 1); ok = 1; for(int i = 0; i < 480; i++) if(o[i] != (float)ramp16[480 + i] / 32768.0f) ok = 0;
    CHECK(ok && nev == 4);
    nev = 0; pass(e, o, 1); ok = 1; for(int i = 0; i < 40; i++) if(o[i] != (float)ramp16[960 + i] / 32768.0f) ok = 0;
    for(int i = 40; i < 480; i++) if(o[i] != 0.0f) ok = 0;
    CHECK(ok);
    /* third pass: 40 frames are left but 480 + look-ahead are needed: bytes required = (480 + 2 - 40) * 2 */
    CHECK(nev == 6 && evlog[1].ev == KMX_EV_PASS_START && evlog[1].arg == (480 + 2 - 40) * 2 && evlog[2].ev == KMX_EV_BUFFER_END && evlog[2].arg == 7 && evlog[3].ev == KMX_EV_STREAM_END && evlog[4].ev == KMX_EV_PASS_END && evlog[5].ev == EV_ENGINE + KMX_EV_PASS_END);
    kmx_source_state(s, &ctx, &q, &played); CHECK(ctx == 0 && q == 0 && played == 0);     /* SamplesPlayed restarts after END_OF_STREAM */
    KmxStats st; kmx_engine_stats(e, &st); CHECK(st.starvations == 0 && st.buffers_submitted == 1 && st.bytes_submitted == 2000 && st.buffers_ended == 1 && st.streams_ended == 1 && st.passes == 4);
    /* a buffer that ends exactly on a pass boundary reports its end in that pass */
    b = mkbuf(ramp16, 480 * 2, 0, 3); kmx_source_submit(s, &b); nev = 0; pass(e, o, 1);
    CHECK(!strcmp(seq(U), "S3E3")); kmx_source_state(s, &ctx, &q, &played); CHECK(q == 0 && played == 480);
    nev = 0; pass(e, o, 1); kmx_engine_stats(e, &st); CHECK(st.starvations == 1 && o[0] == 0.0f);   /* no END_OF_STREAM and nothing queued: a glitch, counted once */
    pass(e, o, 1); kmx_engine_stats(e, &st); CHECK(st.starvations == 1);
    { uint64_t sc = 0, sf = 0, sl = 0; CHECK(kmx_source_starvations(s, &sc, &sf, &sl) == KMX_OK && sc == 1 && sf == 5 && sl == 5 && kmx_source_starvations(m, &sc, &sf, &sl) == KMX_E_INVALID); }
    /* engine callbacks off: no engine events; stopped engine: no pass at all */
    kmx_engine_set_callbacks(e, 0); nev = 0; pass(e, o, 1); CHECK(count_ev(EV_ENGINE + KMX_EV_PASS_START, NULL) == 0 && count_ev(KMX_EV_PASS_START, U) == 1);
    kmx_engine_stop(e); nev = 0; { const float *oo = (void *)1; uint32_t oc = 9; CHECK(kmx_engine_pass(e, NULL, &oo, &oc) == KMX_OK && oo == NULL && nev == 0); }
    kmx_engine_destroy(e);
}

/* ---- queue, loops, flush, stop, exit loop, discontinuity ---- */
static void test_queue_loops(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    KmxFormat f = pcm_fmt(1, 48000, 16); void *U = (void *)0x52;
    kmx_voice_create_source(e, &f, 0, 2.0f, U, 1, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    float o[480 * 8]; uint64_t ctx, played; uint32_t q;
    /* A: 100 frames ; B: frames 10..59 of the ramp, loop region 20..39 twice, END_OF_STREAM */
    KmxBuffer a = mkbuf(ramp16, 200, 0, 1), b = mkbuf(ramp16, 200, KMX_END_OF_STREAM, 2);
    b.play_begin = 10; b.play_length = 50; b.loop_begin = 20; b.loop_length = 20; b.loop_count = 2;
    CHECK(kmx_source_submit(s, &a) == KMX_OK && kmx_source_submit(s, &b) == KMX_OK); kmx_source_start(s);
    nev = 0; pass(e, o, 1);
    CHECK(!strcmp(seq(U), "S1E1S2L2L2E2X"));
    /* expected sample order: A 0..99, B 10..39, 20..39, 20..39, 40..59 */
    int idx[400], n = 0; for(int i = 0; i < 100; i++) idx[n++] = i; for(int i = 10; i < 40; i++) idx[n++] = i; for(int k = 0; k < 2; k++) for(int i = 20; i < 40; i++) idx[n++] = i; for(int i = 40; i < 60; i++) idx[n++] = i;
    int ok = n == 190; for(int i = 0; i < n; i++) if(o[i] != (float)ramp16[idx[i]] / 32768.0f) ok = 0;
    CHECK(ok && o[190] == 0.0f);
    /* loop begin before play begin, loop length 0 = to the end of the play region, infinite loop left with ExitLoop */
    KmxBuffer c = mkbuf(ramp16, 200, 0, 3); c.play_begin = 30; c.play_length = 20; c.loop_begin = 5; c.loop_count = KMX_LOOP_INFINITE;
    CHECK(kmx_source_submit(s, &c) == KMX_OK); nev = 0; pass(e, o, 1);
    ok = 1; for(int i = 0; i < 20; i++) if(o[i] != (float)ramp16[30 + i] / 32768.0f) ok = 0;
    for(int i = 20; i < 480; i++) if(o[i] != (float)ramp16[5 + (i - 20) % 45] / 32768.0f) ok = 0;
    CHECK(ok); CHECK(count_ev(KMX_EV_LOOP_END, U) == 1 + (480 - 20) / 45 && count_ev(KMX_EV_BUFFER_END, U) == 0);
    kmx_source_state(s, &ctx, &q, &played); CHECK(q == 1 && ctx == 3 && played == 480);  /* looped samples count */
    CHECK(kmx_source_exit_loop(s) == KMX_OK); nev = 0; pass(e, o, 1);
    CHECK(!strcmp(seq(U), "E3")); kmx_source_state(s, &ctx, &q, &played); CHECK(q == 0);
    /* validation */
    KmxBuffer bad = mkbuf(ramp16, 200, 0, 0); bad.play_begin = 100; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(ramp16, 200, 0, 0); bad.play_begin = 50; bad.play_length = 51; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(ramp16, 200, 0, 0); bad.loop_begin = 5; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);             /* loop fields without a loop count */
    bad = mkbuf(ramp16, 200, 0, 0); bad.loop_count = 1; bad.loop_begin = 100; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(ramp16, 200, 0, 0); bad.loop_count = 1; bad.loop_begin = 90; bad.loop_length = 20; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(ramp16, 200, 0, 0); bad.loop_count = 1; bad.play_begin = 50; bad.loop_begin = 10; bad.loop_length = 30; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);   /* loop ends before play begin */
    bad = mkbuf(ramp16, 200, 0, 0); bad.loop_count = 255 + 1; CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(ramp16, 201, 0, 0); CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);                                  /* not a whole frame */
    bad = mkbuf(ramp16, 200, 0x80, 0); CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    bad = mkbuf(NULL, 200, 0, 0); CHECK(kmx_source_submit(s, &bad) == KMX_E_INVALID);
    /* 64 buffers at most */
    for(int i = 0; i < 64; i++){ KmxBuffer x = mkbuf(ramp16, 2000, 0, 100 + (uint64_t)i); CHECK(kmx_source_submit(s, &x) == KMX_OK); }
    { KmxBuffer x = mkbuf(ramp16, 2000, 0, 999); CHECK(kmx_source_submit(s, &x) == KMX_E_INVALID); }
    /* flush on a started voice: the playing buffer stays, 63 report OnBufferEnd (only that) from the next pass, before the kept buffer ends */
    nev = 0; pass(e, o, 1); CHECK(!strcmp(seq(U), "S0")); kmx_source_state(s, &ctx, &q, &played); CHECK(q == 64 && ctx == 100);
    CHECK(kmx_source_flush(s) == KMX_OK); kmx_source_state(s, &ctx, &q, &played); CHECK(q == 1 && ctx == 100);
    nev = 0; pass(e, o, 1); CHECK(count_ev(KMX_EV_BUFFER_END, U) == 63 && count_ev(KMX_EV_BUFFER_START, U) == 0 && count_ev(KMX_EV_STREAM_END, U) == 0);
    CHECK(evlog[0].ev == KMX_EV_PASS_START && evlog[1].ev == KMX_EV_BUFFER_END && evlog[1].arg == 101 && evlog[63].arg == 163 && evlog[64].ev == KMX_EV_PASS_END);
    ok = 1; for(int i = 0; i < 480; i++) if(o[i] != (float)ramp16[480 + i] / 32768.0f) ok = 0;
    CHECK(ok);
    /* stop: silence, position and SamplesPlayed kept, pass callbacks continue; start resumes at the same sample */
    kmx_source_state(s, NULL, NULL, &played); uint64_t before = played;
    CHECK(kmx_source_stop(s, 0) == KMX_OK); nev = 0; pass(e, o, 1); CHECK(o[0] == 0.0f && o[479] == 0.0f && nev == 2 && evlog[0].arg == 0);
    kmx_source_state(s, &ctx, &q, &played); CHECK(played == before && q == 1);
    kmx_source_start(s); nev = 0; pass(e, o, 1); CHECK(o[0] == (float)ramp16[960] / 32768.0f && o[39] == (float)ramp16[999] / 32768.0f && o[40] == 0.0f && !strcmp(seq(U), "E0"));
    /* flush on a stopped voice removes the buffer in progress too and resets the position */
    { KmxBuffer x = mkbuf(ramp16, 2000, 0, 5), y = mkbuf(ramp16, 2000, 0, 6); kmx_source_submit(s, &x); kmx_source_submit(s, &y); }
    pass(e, o, 1); kmx_source_stop(s, 0); kmx_source_flush(s); kmx_source_state(s, &ctx, &q, &played); CHECK(q == 0 && ctx == 0);
    nev = 0; pass(e, o, 1); CHECK(!strcmp(seq(U), "E5E6"));
    { KmxBuffer x = mkbuf(ramp16, 200, 0, 8); kmx_source_submit(s, &x); } kmx_source_start(s); nev = 0; pass(e, o, 1);
    CHECK(o[0] == (float)ramp16[0] / 32768.0f && !strcmp(seq(U), "S8E8"));
    /* Discontinuity marks the last queued buffer as the end of the stream */
    { KmxBuffer x = mkbuf(ramp16, 200, 0, 4); kmx_source_submit(s, &x); } CHECK(kmx_source_discontinuity(s) == KMX_OK); nev = 0; pass(e, o, 1); CHECK(!strcmp(seq(U), "S4E4X"));
    CHECK(kmx_source_stop(s, 0x10) == KMX_E_INVALID && kmx_source_start(m) == KMX_E_INVALID && kmx_source_flush(m) == KMX_E_INVALID);
    /* SetSourceSampleRate only with an empty queue */
    CHECK(kmx_source_set_rate(s, 24000) == KMX_OK && kmx_voice_in_rate(s) == 24000);
    { KmxBuffer x = mkbuf(ramp16, 200, 0, 4); kmx_source_submit(s, &x); } CHECK(kmx_source_set_rate(s, 48000) == KMX_E_INVALID);
    kmx_engine_destroy(e);
}

/* ---- callbacks that call back into the engine ---- */
static KmxVoice *hk_voice; static int hk_mode, hk_done;
static void hk(void *ctx, void *user, int ev, uint64_t arg){
    (void)ctx; (void)user;
    if(hk_mode == 1 && ev == KMX_EV_BUFFER_END && arg == 1 && !hk_done){ hk_done = 1; KmxBuffer x = mkbuf(ramp16 + 500, 600, KMX_END_OF_STREAM, 2); if(kmx_source_submit(hk_voice, &x) != KMX_OK) fails++; }
    if(hk_mode == 2 && ev == KMX_EV_BUFFER_START && !hk_done){ hk_done = 1; kmx_voice_destroy(hk_voice); }
    if(hk_mode == 3 && ev == KMX_EV_BUFFER_START && !hk_done){ hk_done = 1; kmx_source_stop(hk_voice, 0); }
    if(hk_mode == 4 && ev == KMX_EV_PASS_START && arg && !hk_done){ hk_done = 1; KmxBuffer x = mkbuf(ramp16, (uint32_t)arg, 0, 9); if(kmx_source_submit(hk_voice, &x) != KMX_OK) fails++; }
}
static void test_reentrancy(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    KmxFormat f = pcm_fmt(1, 48000, 16); void *U = (void *)0x53; float o[480];
    kmx_voice_create_source(e, &f, 0, 2.0f, U, 1, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    /* a buffer submitted from OnBufferEnd continues in the same pass without a gap */
    { KmxBuffer x = mkbuf(ramp16, 200, 0, 1); kmx_source_submit(s, &x); } kmx_source_start(s);
    hk_voice = s; hk_mode = 1; hk_done = 0; hook = hk; nev = 0; pass(e, o, 1); hook = NULL;
    int ok = 1; for(int i = 0; i < 100; i++) if(o[i] != (float)ramp16[i] / 32768.0f) ok = 0;
    for(int i = 0; i < 300; i++) if(o[100 + i] != (float)ramp16[500 + i] / 32768.0f) ok = 0;
    CHECK(ok && o[400] == 0.0f && !strcmp(seq(U), "S1E1S2E2X"));
    /* just-in-time streaming: submit exactly the bytes OnVoiceProcessingPassStart asks for */
    hk_mode = 4; hk_done = 0; hook = hk; nev = 0; pass(e, o, 1); hook = NULL;
    CHECK(evlog[0].ev == KMX_EV_PASS_START && evlog[0].arg == 482 * 2 && o[0] == (float)ramp16[0] / 32768.0f && o[479] == (float)ramp16[479] / 32768.0f);
    kmx_source_flush(s); kmx_source_stop(s, 0); kmx_source_flush(s); pass(e, o, 1);
    /* Stop from OnBufferStart: nothing of the buffer is played, the buffer stays queued */
    { KmxBuffer x = mkbuf(ramp16 + 100, 200, 0, 4); kmx_source_submit(s, &x); } kmx_source_start(s);
    hk_mode = 3; hk_done = 0; hook = hk; nev = 0; pass(e, o, 1); hook = NULL;
    uint32_t q; kmx_source_state(s, NULL, &q, NULL); CHECK(o[0] == 0.0f && q == 1 && !strcmp(seq(U), "S4"));
    kmx_source_start(s); pass(e, o, 1); CHECK(o[0] == (float)ramp16[100] / 32768.0f);
    /* DestroyVoice from the voice's own callback: no further callbacks, freed after the pass */
    { KmxBuffer x = mkbuf(ramp16, 200, 0, 5); kmx_source_submit(s, &x); }
    freed_count = 0; hk_mode = 2; hk_done = 0; hook = hk; nev = 0; pass(e, o, 1); hook = NULL;
    CHECK(freed_count == 1 && last_freed == U && nev == 2 && evlog[1].ev == KMX_EV_BUFFER_START && o[0] == 0.0f);
    nev = 0; pass(e, o, 1); CHECK(nev == 0);
    KmxStats st; kmx_engine_stats(e, &st); CHECK(st.live_source == 0 && st.voices_destroyed == 1);
    CHECK(kmx_engine_pass(e, NULL, NULL, NULL) == KMX_OK);
    kmx_engine_destroy(e);
}

/* ---- graph: matrices, volumes, channel volumes, submix chain, stage order ---- */
static void test_routing(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *sub, *bus, *s, *x;
    /* default output before a mastering voice exists attaches when the master appears */
    KmxFormat f = pcm_fmt(2, 48000, 16);
    CHECK(kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s) == KMX_OK && kmx_voice_set_outputs(s, NULL, NULL, 0, 1) == KMX_OK);
    CHECK(kmx_voice_create_master(e, 4, 48000, 0, NULL, &m) == KMX_OK);
    CHECK(kmx_voice_create_master(e, 2, 48000, 0, NULL, &x) == KMX_E_INVALID);              /* one mastering voice */
    CHECK(kmx_voice_create_submix(e, 4, 48000, 0, 10, NULL, &sub) == KMX_OK && kmx_voice_create_submix(e, 4, 48000, 0, 20, NULL, &bus) == KMX_OK);
    CHECK(kmx_voice_create_submix(e, 4, 44150, 0, 0, NULL, &x) == KMX_E_INVALID);            /* not a multiple of 100 */
    CHECK(kmx_voice_set_outputs(bus, &sub, NULL, 1, 0) == KMX_E_INVALID);                    /* later stage -> earlier stage */
    CHECK(kmx_voice_set_outputs(sub, &sub, NULL, 1, 0) == KMX_E_INVALID && kmx_voice_set_outputs(sub, &s, NULL, 1, 0) == KMX_E_INVALID && kmx_voice_set_outputs(m, &sub, NULL, 1, 0) == KMX_E_INVALID);
    CHECK(kmx_voice_set_outputs(sub, &bus, NULL, 1, 0) == KMX_OK && kmx_voice_set_outputs(bus, NULL, NULL, 0, 1) == KMX_OK);
    KmxVoice *two[2] = {sub, sub}; CHECK(kmx_voice_set_outputs(s, two, NULL, 2, 0) == KMX_E_INVALID);
    /* default 2 -> 4 matrix is "first channels" */
    static int16_t st[960]; for(int i = 0; i < 480; i++){ st[2 * i] = 16384; st[2 * i + 1] = -8192; }
    KmxBuffer b = mkbuf(st, sizeof st, 0, 0); b.loop_count = KMX_LOOP_INFINITE; kmx_source_submit(s, &b); kmx_source_start(s);
    float o[480 * 4], g[8]; pass(e, o, 4);
    CHECK(o[0] == 0.5f && o[1] == -0.25f && o[2] == 0.0f && o[3] == 0.0f);
    CHECK(kmx_voice_matrix(s, m, 2, 4, g) == KMX_OK && g[0] == 1.0f && g[1] == 0.0f && g[2] == 0.0f && g[3] == 1.0f && g[4] == 0.0f && g[7] == 0.0f);
    /* route source -> sub -> bus -> master with explicit matrices: level[src x d + s] */
    CHECK(kmx_voice_set_outputs(s, &sub, NULL, 1, 0) == KMX_OK);
    float mx[8] = {0, 0,  0.5f, 0,  0, 2.0f,  1.0f, 1.0f};                                    /* d0 = 0 ; d1 = L/2 ; d2 = 2R ; d3 = L + R */
    CHECK(kmx_voice_set_matrix(s, sub, 2, 4, mx) == KMX_OK);
    CHECK(kmx_voice_set_matrix(s, sub, 2, 3, mx) == KMX_E_INVALID && kmx_voice_set_matrix(s, m, 2, 4, mx) == KMX_E_INVALID && kmx_voice_set_matrix(s, sub, 1, 4, mx) == KMX_E_INVALID);
    memset(g, 0, sizeof g); CHECK(kmx_voice_matrix(s, NULL, 2, 4, g) == KMX_OK && !memcmp(g, mx, sizeof mx));
    pass(e, o, 4); pass(e, o, 4);
    CHECK(o[0] == 0.0f && o[1] == 0.25f && o[2] == -0.5f && o[3] == 0.25f && o[4 * 479 + 3] == 0.25f);
    /* source volume and channel volumes scale the matrix; submix volume, bus channel volumes and master volume multiply */
    float cv[2] = {0.5f, 2.0f}; CHECK(kmx_voice_set_volume(s, 0.5f) == KMX_OK && kmx_voice_set_channel_volumes(s, 2, cv) == KMX_OK && kmx_voice_set_channel_volumes(s, 3, cv) == KMX_E_INVALID);
    float rd[2] = {0, 0}; CHECK(kmx_voice_channel_volumes(s, 2, rd) == KMX_OK && rd[0] == 0.5f && rd[1] == 2.0f && kmx_voice_volume(s) == 0.5f);
    pass(e, o, 4); pass(e, o, 4);
    /* L' = 0.5 * 0.5 * 0.5 = 0.125, R' = 0.5 * 2 * -0.25 = -0.25 : d1 = L'/2, d2 = 2R', d3 = L' + R' */
    CHECK(o[0] == 0.0f && o[1] == 0.0625f && o[2] == -0.5f && o[3] == -0.125f);
    float bcv[4] = {1, 2, 1, 4}; CHECK(kmx_voice_set_volume(sub, 0.5f) == KMX_OK && kmx_voice_set_channel_volumes(bus, 4, bcv) == KMX_OK && kmx_voice_set_volume(m, 2.0f) == KMX_OK);
    pass(e, o, 4); pass(e, o, 4);
    CHECK(o[1] == 0.0625f * 0.5f * 2.0f * 2.0f && o[2] == -0.5f * 0.5f * 2.0f && o[3] == -0.125f * 0.5f * 4.0f * 2.0f);
    float mcv[4] = {1, 1, 0, 1}; CHECK(kmx_voice_set_channel_volumes(m, 4, mcv) == KMX_OK); pass(e, o, 4); pass(e, o, 4); CHECK(o[2] == 0.0f && o[3] == -0.5f);
    CHECK(kmx_voice_set_volume(s, 3.0e7f) == KMX_E_INVALID && kmx_voice_set_volume(s, NAN) == KMX_E_INVALID);
    /* two sends from one voice */
    KmxVoice *both[2] = {sub, bus}; kmx_voice_set_volume(sub, 1.0f); kmx_voice_set_volume(m, 1.0f); float one4[4] = {1, 1, 1, 1}; kmx_voice_set_channel_volumes(bus, 4, one4); kmx_voice_set_channel_volumes(m, 4, one4);
    kmx_voice_set_volume(s, 1.0f); float cv1[2] = {1, 1}; kmx_voice_set_channel_volumes(s, 2, cv1);
    CHECK(kmx_voice_set_outputs(s, both, NULL, 2, 0) == KMX_OK);
    float direct[8] = {1, 0, 0, 0, 0, 0, 0, 0}; CHECK(kmx_voice_set_matrix(s, bus, 2, 4, direct) == KMX_OK && kmx_voice_set_matrix(s, NULL, 2, 4, direct) == KMX_E_INVALID);
    pass(e, o, 4); pass(e, o, 4);
    CHECK(o[0] == 0.5f && o[1] == 0.25f && o[2] == -0.5f && o[3] == 0.25f);                   /* the matrix to `sub` survived the new send list */
    /* a destroyed destination disappears from the send lists */
    kmx_voice_destroy(sub); pass(e, o, 4); pass(e, o, 4); CHECK(o[0] == 0.5f && o[1] == 0.0f && o[3] == 0.0f);
    /* a send with neither a matrix nor a built-in default (4 -> 2 down-mix) is counted for the caller to fail on */
    KmxVoice *s4, *sub2; KmxFormat f4 = pcm_fmt(4, 48000, 16); kmx_voice_create_submix(e, 2, 48000, 0, 5, NULL, &sub2); kmx_voice_set_outputs(sub2, NULL, NULL, 0, 1);
    kmx_voice_create_source(e, &f4, 0, 2.0f, NULL, 0, &s4); CHECK(kmx_voice_set_outputs(s4, &sub2, NULL, 1, 0) == KMX_OK);
    CHECK(kmx_voice_matrix(s4, sub2, 4, 2, g) == KMX_E_UNSUPPORTED);
    static int16_t q4[40]; KmxBuffer b4 = mkbuf(q4, sizeof q4, 0, 0); kmx_source_submit(s4, &b4); kmx_source_start(s4); pass(e, o, 4);
    KmxStats stt; kmx_engine_stats(e, &stt); CHECK(stt.missing_default_matrix > 0 && stt.live_submix == 2 && stt.live_source == 2 && stt.active_source == 2 && stt.peak_live_source == 2);
    kmx_engine_destroy(e);
}

/* ---- volume ramp over one pass ---- */
static void test_ramp(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    KmxFormat f = pcm_fmt(1, 48000, 16); kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    static int16_t dc[480]; for(int i = 0; i < 480; i++) dc[i] = 16384;
    KmxBuffer b = mkbuf(dc, sizeof dc, 0, 0); b.loop_count = KMX_LOOP_INFINITE; kmx_source_submit(s, &b); kmx_source_start(s);
    float o[480]; pass(e, o, 1); CHECK(o[0] == 0.5f && o[479] == 0.5f);
    kmx_voice_set_volume(s, 0.0f); pass(e, o, 1);                                              /* linear ramp 1 -> 0 across the pass, no step */
    CHECK(NEAR(o[0], 0.5f * 479.0f / 480.0f, 1e-6) && NEAR(o[239], 0.5f * 240.0f / 480.0f, 1e-6) && o[479] == 0.0f);
    int mono = 1; for(int i = 1; i < 480; i++) if(o[i] > o[i - 1]) mono = 0;
    CHECK(mono); pass(e, o, 1); CHECK(o[0] == 0.0f && o[479] == 0.0f);
    kmx_voice_set_volume(s, 1.0f); pass(e, o, 1); kmx_voice_set_volume(m, 0.5f); pass(e, o, 1);  /* master volume ramps the same way */
    CHECK(NEAR(o[0], 0.5f * (1.0f - 0.5f / 480.0f), 1e-6) && o[479] == 0.25f);
    kmx_engine_destroy(e);
}

/* ---- resampling ---- */
static void test_resample(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; kmx_voice_create_master(e, 1, 48000, 0, NULL, &m);
    static float lin[20000]; for(int i = 0; i < 20000; i++) lin[i] = (float)i * (1.0f / 32768.0f);
    KmxFormat f = pcm_fmt(1, 48000, 32); f.tag = KMX_TAG_FLOAT; void *U = (void *)0x54;
    kmx_voice_create_source(e, &f, 0, 2.0f, U, 1, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    float o[480]; uint64_t played, base;
    /* ratio 2: every other sample, exactly (the phase stays on samples); consumes 960 frames per pass */
    KmxBuffer b = mkbuf(lin, 4000 * 4, KMX_END_OF_STREAM, 1); kmx_source_submit(s, &b); CHECK(kmx_source_set_ratio(s, 2.0f) == KMX_OK); kmx_source_start(s);
    pass(e, o, 1); int ok = 1; for(int i = 0; i < 480; i++) if(o[i] != lin[2 * i]) ok = 0;
    CHECK(ok); kmx_source_state(s, NULL, NULL, &played); CHECK(played == 960);
    CHECK(kmx_source_set_ratio(s, 4.0f) == KMX_OK && kmx_source_ratio(s) == 2.0f);            /* clamped to MaxFrequencyRatio */
    CHECK(kmx_source_set_ratio(s, 0.0f) == KMX_E_INVALID && kmx_source_set_ratio(s, 2000.0f) == KMX_E_INVALID);
    kmx_source_stop(s, 0); kmx_source_flush(s); pass(e, o, 1);
    /* ratio 0.5: half-sample positions; Catmull-Rom reproduces a straight line (first output pair aside: flat start) */
    b = mkbuf(lin + 1000, 4000 * 4, KMX_END_OF_STREAM, 2); kmx_source_submit(s, &b); kmx_source_set_ratio(s, 0.5f); kmx_source_start(s);
    pass(e, o, 1); ok = 1; for(int i = 2; i < 480; i++) if(!NEAR(o[i], (1000.0 + i * 0.5) / 32768.0, 2e-7)) ok = 0;
    CHECK(ok && o[0] == lin[1000]); base = played; kmx_source_state(s, NULL, NULL, &played); CHECK(played - base == 240);   /* Stop / flush do not reset SamplesPlayed */
    pass(e, o, 1); ok = 1; for(int i = 0; i < 480; i++) if(!NEAR(o[i], (1000.0 + (480 + i) * 0.5) / 32768.0, 2e-7)) ok = 0;
    CHECK(ok);                                                                                 /* continuous across the pass boundary */
    kmx_source_stop(s, 0); kmx_source_flush(s); pass(e, o, 1); kmx_voice_destroy(s);
    /* 24 kHz source in a 48 kHz graph: a 1 kHz sine stays a 1 kHz sine */
    static float sine[24000]; for(int i = 0; i < 24000; i++) sine[i] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * i / 24000.0));
    f = pcm_fmt(1, 24000, 32); f.tag = KMX_TAG_FLOAT; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    b = mkbuf(sine, sizeof sine, KMX_END_OF_STREAM, 3); kmx_source_submit(s, &b); kmx_source_start(s);
    double maxerr = 0.0; for(int p = 0; p < 20; p++){ pass(e, o, 1); for(int i = 0; i < 480; i++){ if(p == 0 && i < 4) continue; double want = 0.5 * sin(2.0 * M_PI * 1000.0 * (p * 480 + i) / 48000.0), er = fabs(o[i] - want); if(er > maxerr) maxerr = er; } }
    CHECK(maxerr < 2e-3); kmx_source_state(s, NULL, NULL, &played); CHECK(played >= 4799 && played <= 4801);
    kmx_voice_destroy(s);
    /* 44.1 kHz source: 441 frames consumed per pass on average */
    f = pcm_fmt(1, 44100, 32); f.tag = KMX_TAG_FLOAT; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    b = mkbuf(lin, 20000 * 4, 0, 4); kmx_source_submit(s, &b); kmx_source_start(s);
    for(int p = 0; p < 10; p++) pass(e, o, 1);
    kmx_source_state(s, NULL, NULL, &played); CHECK(played == 4410);
    ok = 1; for(int i = 0; i < 480; i++) if(!NEAR(o[i], (9 * 441 + i * 441.0 / 480.0) / 32768.0, 3e-7)) ok = 0;
    CHECK(ok);
    kmx_engine_destroy(e);
    /* fixed-rate submix: 44.1 kHz 2-channel submix into a 48 kHz master; a 44.1 kHz source feeds it 1:1 (441 frames per pass) */
    e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *sub; kmx_voice_create_master(e, 2, 48000, 0, NULL, &m); CHECK(kmx_voice_create_submix(e, 2, 44100, 0, 0, NULL, &sub) == KMX_OK && kmx_voice_set_outputs(sub, NULL, NULL, 0, 1) == KMX_OK);
    CHECK(kmx_voice_proc_rate(sub) == 48000 && kmx_voice_proc_frames(sub) == 480 && kmx_voice_in_rate(sub) == 44100);
    static float lin2[40000]; for(int i = 0; i < 20000; i++){ lin2[2 * i] = (float)i * (1.0f / 32768.0f); lin2[2 * i + 1] = 0.25f; }
    f = pcm_fmt(2, 44100, 32); f.tag = KMX_TAG_FLOAT; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); CHECK(kmx_voice_set_outputs(s, &sub, NULL, 1, 0) == KMX_OK);
    CHECK(kmx_voice_proc_rate(s) == 44100 && kmx_voice_proc_frames(s) == 441);
    b = mkbuf(lin2, sizeof lin2, 0, 5); kmx_source_submit(s, &b); kmx_source_start(s);
    float o2[960]; pass(e, o2, 2); pass(e, o2, 2);
    kmx_source_state(s, NULL, NULL, &played); CHECK(played == 882);
    /* output frame j of pass 1 sits at input position 441 + j * 441/480 - 2 (two frames of interpolator delay) */
    ok = 1; for(int j = 0; j < 480; j++){ if(!NEAR(o2[2 * j], (441.0 + j * 441.0 / 480.0 - 2.0) / 32768.0, 3e-7) || o2[2 * j + 1] != 0.25f) ok = 0; }
    CHECK(ok);
    float last = o2[2 * 479]; pass(e, o2, 2); CHECK(NEAR(o2[0] - last, 441.0 / 480.0 / 32768.0, 3e-7));     /* no seam between passes */
    /* a 48 kHz source into the 44.1 kHz submix is resampled down then up: DC survives exactly */
    kmx_voice_destroy(s); static float dcbuf[960]; for(int i = 0; i < 960; i++) dcbuf[i] = 0.5f;
    f = pcm_fmt(2, 48000, 32); f.tag = KMX_TAG_FLOAT; kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, &sub, NULL, 1, 0);
    b = mkbuf(dcbuf, sizeof dcbuf, 0, 6); b.loop_count = KMX_LOOP_INFINITE; kmx_source_submit(s, &b); kmx_source_start(s);
    for(int p = 0; p < 4; p++) pass(e, o2, 2);
    ok = 1; for(int j = 0; j < 960; j++) if(o2[j] != 0.5f) ok = 0;
    CHECK(ok); kmx_source_state(s, NULL, NULL, &played); CHECK(played >= 4 * 480 - 3 && played <= 4 * 480);
    /* all destinations of a voice share one input rate */
    KmxVoice *sub48; kmx_voice_create_submix(e, 2, 48000, 0, 1, NULL, &sub48); KmxVoice *mixrate[2] = {sub, sub48}; CHECK(kmx_voice_set_outputs(s, mixrate, NULL, 2, 0) == KMX_E_INVALID);
    KmxVoice *bad; CHECK(kmx_voice_create_master(e, 2, 44100, 0, NULL, &bad) == KMX_E_INVALID);
    kmx_engine_destroy(e);
    e = kmx_engine_create(&thost, NULL, 48000); CHECK(kmx_voice_create_master(e, 2, 44100, 0, NULL, &bad) == KMX_E_UNSUPPORTED); kmx_engine_destroy(e);
    CHECK(kmx_engine_create(&thost, NULL, 44150) == NULL);
}

/* ---- effect chains: meter, counted pass-through, hosted effects ---- */
static void test_effects(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *s; kmx_voice_create_master(e, 2, 48000, 0, NULL, &m);
    KmxFormat f = pcm_fmt(2, 48000, 16); kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s); kmx_voice_set_outputs(s, NULL, NULL, 0, 1);
    static int16_t sq[960]; for(int i = 0; i < 480; i++){ sq[2 * i] = (i & 1) ? 16384 : -16384; sq[2 * i + 1] = (i % 4 == 0) ? 8192 : 0; }
    KmxBuffer b = mkbuf(sq, sizeof sq, 0, 0); b.loop_count = KMX_LOOP_INFINITE; kmx_source_submit(s, &b); kmx_source_start(s);
    /* master: hosted gain x0.5 in place, counted pass-through, meter */
    TFx g1 = {0.5f}; KmxFxDesc d[3] = {{KMX_FX_HOST, &g1, 2, 1, 1}, {KMX_FX_PASS, NULL, 2, 1, 0}, {KMX_FX_METER, NULL, 2, 1, 0}};
    CHECK(kmx_voice_set_chain(m, d, 3) == KMX_OK && kmx_voice_out_channels(m) == 2 && kmx_voice_fx_user(m, 0) == &g1 && kmx_voice_fx_user(m, 3) == NULL);
    float o[960], pk[2], rms[2]; pass(e, o, 2);
    CHECK(o[0] == -0.25f && o[1] == 0.125f && o[2] == 0.25f && g1.calls == 1 && g1.last_enabled == 1 && g1.last_in_ch == 2 && g1.last_out_ch == 2 && g1.last_frames == 480 && g1.inplace_seen == 1 && g1.last_in_silent == 0);
    CHECK(kmx_voice_meter(m, 2, pk, rms, 2) == KMX_OK && pk[0] == 0.25f && NEAR(rms[0], 0.25, 1e-7) && pk[1] == 0.125f && NEAR(rms[1], 0.125 * 0.5, 1e-7));
    CHECK(kmx_voice_meter(m, 2, pk, rms, 3) == KMX_E_INVALID && kmx_voice_meter(m, 1, pk, rms, 2) == KMX_E_INVALID && kmx_voice_meter(m, 9, pk, rms, 2) == KMX_E_INVALID);
    KmxStats st; kmx_engine_stats(e, &st); CHECK(st.fx_pass_through == 1 && st.fx_host_process == 1 && st.fx_meter == 1);
    /* a disabled hosted effect is still called (IsEnabled = FALSE) */
    int on = 5; CHECK(kmx_voice_fx_enable(m, 0, 0) == KMX_OK && kmx_voice_fx_enabled(m, 0, &on) == KMX_OK && on == 0 && kmx_voice_fx_enable(m, 7, 0) == KMX_E_INVALID);
    pass(e, o, 2); CHECK(o[0] == -0.5f && g1.calls == 2 && g1.last_enabled == 0);
    CHECK(kmx_voice_fx_enable(m, 2, 0) == KMX_OK); pass(e, o, 2); kmx_voice_meter(m, 2, pk, rms, 2); CHECK(pk[0] == 0.0f && rms[0] == 0.0f);   /* disabled meter reports zero */
    /* channel-converting hosted effects: 2 -> 1 (sum) -> 2, not in place; then the meter on the result */
    TFx dn = {1.0f}, up = {0.5f}; KmxFxDesc d2[3] = {{KMX_FX_HOST, &dn, 1, 1, 1}, {KMX_FX_HOST, &up, 2, 1, 0}, {KMX_FX_METER, NULL, 0, 1, 0}};
    CHECK(kmx_voice_set_chain(m, d2, 3) == KMX_OK && kmx_voice_out_channels(m) == 2);
    pass(e, o, 2);
    CHECK(dn.last_in_ch == 2 && dn.last_out_ch == 1 && dn.inplace_seen == 0 && up.last_in_ch == 1 && up.last_out_ch == 2 && up.inplace_seen == 0);
    CHECK(o[0] == (-0.5f + 0.25f) * 0.5f && o[1] == o[0] && o[2] == 0.25f && o[3] == 0.25f);
    /* an effect that reports a silent output buffer */
    up.set_silent = 1; pass(e, o, 2); CHECK(o[0] == 0.0f && o[959] == 0.0f); up.set_silent = 0;
    /* silent input is flagged */
    kmx_source_stop(s, 0); pass(e, o, 2); CHECK(dn.last_in_silent == 1 && o[0] == 0.0f); kmx_source_start(s);
    /* a meter or pass-through cannot change the channel count; too many effects */
    KmxFxDesc bad[1] = {{KMX_FX_METER, NULL, 1, 1, 0}}; CHECK(kmx_voice_set_chain(m, bad, 1) == KMX_E_UNSUPPORTED);
    KmxFxDesc bad2[1] = {{9, NULL, 2, 1, 0}}; CHECK(kmx_voice_set_chain(m, bad2, 1) == KMX_E_INVALID);
    CHECK(kmx_voice_set_chain(m, NULL, 0) == KMX_OK && kmx_voice_out_channels(m) == 2); pass(e, o, 2); CHECK(o[0] == -0.5f);
    /* source voice chain 2 -> 1 changes the matrix shape: matrix and channel volume shape follow the output width */
    TFx sdn = {1.0f}; KmxFxDesc sd[1] = {{KMX_FX_HOST, &sdn, 1, 1, 0}};
    CHECK(kmx_voice_set_chain(s, sd, 1) == KMX_OK && kmx_voice_out_channels(s) == 1 && kmx_voice_in_channels(s) == 2);
    float cv = 2.0f; CHECK(kmx_voice_set_channel_volumes(s, 1, &cv) == KMX_OK); float mm[2] = {1.0f, 0.0f}; CHECK(kmx_voice_set_matrix(s, m, 1, 2, mm) == KMX_OK && kmx_voice_set_matrix(s, m, 2, 2, mm) == KMX_E_INVALID);
    pass(e, o, 2); CHECK(o[0] == (-0.5f + 0.25f) * 2.0f && o[1] == 0.0f);
    /* effect tails: Stop with PLAY_TAILS keeps the chain running on silence, plain Stop does not */
    int c0 = sdn.calls; kmx_source_stop(s, KMX_PLAY_TAILS); pass(e, o, 2); CHECK(sdn.calls == c0 + 1 && sdn.last_in_silent == 1);
    for(int i = 0; i < 12; i++) pass(e, o, 2);
    c0 = sdn.calls; pass(e, o, 2); CHECK(sdn.calls == c0);                                     /* the tail ended after 8 silent passes */
    kmx_source_start(s); pass(e, o, 2); kmx_source_stop(s, 0); c0 = sdn.calls; pass(e, o, 2); CHECK(sdn.calls == c0);
    /* submix: volume is applied ahead of the chain (the meter sees it), channel volumes after */
    KmxVoice *sub; kmx_voice_create_submix(e, 2, 48000, 0, 0, NULL, &sub); kmx_voice_set_outputs(sub, NULL, NULL, 0, 1);
    kmx_voice_set_chain(s, NULL, 0); kmx_voice_set_outputs(s, &sub, NULL, 1, 0); kmx_source_start(s);
    KmxFxDesc md[1] = {{KMX_FX_METER, NULL, 2, 1, 0}}; kmx_voice_set_chain(sub, md, 1); kmx_voice_set_volume(sub, 0.5f); float scv[2] = {4.0f, 1.0f}; kmx_voice_set_channel_volumes(sub, 2, scv);
    pass(e, o, 2); pass(e, o, 2); kmx_voice_meter(sub, 0, pk, rms, 2); CHECK(pk[0] == 0.25f && (o[0] == -1.0f || o[0] == 1.0f));
    kmx_engine_destroy(e);
}

/* ---- determinism: the same scenario twice gives the same bits ---- */
static uint64_t scenario(void){
    KmxEngine *e = kmx_engine_create(&thost, NULL, 48000); kmx_engine_start(e);
    KmxVoice *m, *sub, *s1, *s2; kmx_voice_create_master(e, 2, 48000, 0, NULL, &m); kmx_voice_create_submix(e, 2, 44100, 0, 0, NULL, &sub); kmx_voice_set_outputs(sub, NULL, NULL, 0, 1);
    KmxFormat f = pcm_fmt(1, 22050, 16); kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s1); kmx_voice_set_outputs(s1, &sub, NULL, 1, 0);
    f = pcm_fmt(2, 48000, 16); kmx_voice_create_source(e, &f, 0, 2.0f, NULL, 0, &s2); kmx_voice_set_outputs(s2, NULL, NULL, 0, 1);
    static int16_t n[4000]; uint32_t x = 12345; for(int i = 0; i < 4000; i++){ x = x * 1664525u + 1013904223u; n[i] = (int16_t)(x >> 17); }
    KmxBuffer b = mkbuf(n, 8000, 0, 0); b.loop_count = KMX_LOOP_INFINITE; kmx_source_submit(s1, &b); kmx_source_submit(s2, &b); kmx_source_start(s1); kmx_source_start(s2);
    uint64_t h = 1469598103934665603ull; float o[960];
    for(int p = 0; p < 30; p++){
        if(p == 5) kmx_source_set_ratio(s1, 1.37f); if(p == 9) kmx_voice_set_volume(s2, 0.3f); if(p == 15) kmx_voice_set_volume(sub, 1.7f); if(p == 20) kmx_source_set_ratio(s2, 0.71f);
        pass(e, o, 2); const uint8_t *q = (const uint8_t *)o; for(size_t i = 0; i < sizeof o; i++){ h ^= q[i]; h *= 1099511628211ull; }
    }
    kmx_engine_destroy(e); return h;
}

/* ---- WAV writer ---- */
static void test_wav(void){
    char path[] = "/tmp/khnmc-wav-XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0); if(fd < 0) return; close(fd);
    KmxWav *w = kmx_wav_open(path, 48000, 2); CHECK(w != NULL); if(!w) return;
    float x[8] = {0.0f, 0.5f, -1.0f, 1.0f, 1.5f, -2.0f, 0.25f, 1.0f / 65536.0f};
    CHECK(kmx_wav_write(w, x, 4) == 0 && kmx_wav_sync(w) == 0);
    uint8_t h[64]; fd = open(path, O_RDONLY); CHECK(read(fd, h, 60) == 60); close(fd);          /* valid before close */
    CHECK(!memcmp(h, "RIFF", 4) && h[4] == 36 + 16 && !memcmp(h + 8, "WAVEfmt ", 8) && h[20] == 1 && h[22] == 2 && h[24] == 0x80 && h[25] == 0xbb && h[32] == 4 && h[34] == 16 && !memcmp(h + 36, "data", 4) && h[40] == 16);
    int16_t smp[8]; memcpy(smp, h + 44, 16);
    CHECK(smp[0] == 0 && smp[1] == 16384 && smp[2] == -32768 && smp[3] == 32767 && smp[4] == 32767 && smp[5] == -32768 && smp[6] == 8192 && smp[7] == 0);   /* 0.5 rounds to even: 0 */
    CHECK(kmx_wav_write(w, NULL, 3) == 0);
    KmxWavStats st; CHECK(kmx_wav_close(w, &st) == 0);
    CHECK(st.frames == 7 && st.clipped_samples == 2 && st.nonsilent_frames == 4 && st.peak == 2.0f);
    uint8_t all[128]; fd = open(path, O_RDONLY); CHECK(read(fd, all, 128) == 44 + 28); close(fd); CHECK(all[40] == 28 && all[4] == 36 + 28 && all[60] == 0 && all[71] == 0);
    unlink(path);
    CHECK(kmx_wav_open("/nonexistent-dir/x.wav", 48000, 2) == NULL);
}

int kh_audio_mix_selftest(void){
    fails = checks = 0; hook = NULL;
    test_adpcm(); test_formats(); test_playback_order(); test_queue_loops(); test_reentrancy(); test_routing(); test_ramp(); test_resample(); test_effects();
    uint64_t h1 = scenario(), h2 = scenario(); CHECK(h1 == h2);
    test_wav();
    fprintf(stderr, "kh: audio mix selftest: %d checks, %d failures (determinism hash %016llx)\n", checks, fails, (unsigned long long)h1);
    return fails;
}
