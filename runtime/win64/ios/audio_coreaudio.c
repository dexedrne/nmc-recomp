/* runtime/ios/audio_coreaudio.c : the Apple half of the audio output (TASK28): an output audio unit whose render callback pulls the
 * game's mix through kh_audio_out_pull() (runtime/khnmc_audio_out.h, runtime/audio_out.c).
 *
 * UNRUN.  Written on 2026-11-17 on Linux, without an Apple SDK; never compiled, never linked, never played.  Nothing here has made a
 * sound anywhere.  It is compiled out everywhere unless the build asks for it: `make COREAUDIO=1` on a Mac (or an IOS_SDK build) adds
 * this file with -DKH_AUDIO_COREAUDIO=1 and links AudioToolbox; without that flag the file is not in any build and, if compiled anyway,
 * is empty.
 *
 * What it does:
 *   - one output unit: RemoteIO on iOS (TARGET_OS_IPHONE), the default output unit on macOS (kAudioUnitSubType_DefaultOutput); the
 *     rest of the code is the same for both;
 *   - the unit's input scope, element 0, takes the ring's format: 48000 Hz, 2 channels, float32, interleaved (packed).  The unit
 *     converts to the hardware's rate and layout itself (an iPhone's hardware rate is 48 kHz; on 44.1 kHz routes it resamples);
 *   - the render callback runs on the unit's real-time thread and calls kh_audio_out_pull(buffer, frames) and nothing else: no lock,
 *     no allocation, no Objective-C, no logging.  A pull that found nothing sets kAudioUnitRenderAction_OutputIsSilence;
 *   - the ring's target fill is raised to twice the largest slice the device may ask for, so one slice never empties the ring by
 *     itself: the unit's kAudioUnitProperty_MaximumFramesPerSlice, and on iOS at least KH_AUDIO_OUT_MAX_SLICE (4096: iOS uses slices
 *     of up to 4096 frames while the screen is locked, whatever the property said before; from memory, unverified).  That is a
 *     target of 8192 frames = 171 ms of mix queued in front of the device on iOS (the latency the picture is ahead of the sound).
 *     A target the ring cannot take is a loud failure of kh_coreaudio_start() (review of task 28: the call's result was ignored and
 *     4096-frame slices underran by half of every slice while the log printed the target it had asked for);
 *   - kh_coreaudio_start() -> kh_audio_out_start() (the mixer follows the device from then on), kh_coreaudio_stop() ->
 *     kh_audio_out_stop() (the mixer falls back to its own clock and discards).
 * NOT WIRED YET (review of task 28): nothing calls kh_coreaudio_start / kh_coreaudio_stop.  The app shell (runtime/ios/ios_main.m,
 * unchanged since task 26) still has to: set KHNMC_AUDIO=pull before kh_main runs (the ring exists only then; kh_coreaudio_start()
 * fails loudly otherwise), set the AVAudioSession category (playback) and activate the session, call kh_coreaudio_start() once the
 * runtime's audio engine exists, and call stop / start around session interruptions, route changes and background / foreground.
 * Error handling: every OSStatus is checked; a failure is reported with its four-character code on stderr and returned (-1), the
 * unit is disposed, and kh_audio_out_start() is not called (the mixer keeps its fallback clock: silence, counted). */
#if defined(KH_AUDIO_COREAUDIO) && KH_AUDIO_COREAUDIO
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include "../khnmc_audio_out.h"

static AudioComponentInstance kh_unit;
static _Atomic unsigned long long kh_muted_blocks;   /* blocks the render callback muted as not a game mix (reported at stop) */
static pthread_mutex_t kh_ca_m = PTHREAD_MUTEX_INITIALIZER;   /* start and stop one at a time (review r9); never on the render thread */
static int kh_unit_running;

static OSStatus kh_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io){
    (void)ref; (void)ts; (void)bus;
    if(!io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
    float *dst = (float *)io->mBuffers[0].mData;
    UInt32 cap = io->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * KH_AUDIO_OUT_CHANNELS);
    if(frames > cap) frames = cap;
    if(kh_audio_out_pull(dst, frames) == 0){ if(flags) *flags |= kAudioUnitRenderAction_OutputIsSilence; return noErr; }
    /* before the speaker (2026-11-18: a test signal of sample counters reached the iPhone speaker as loud noise): a block with a NaN
     * or a sample beyond 4.0 (12 dB over full scale: no game mix comes near that) is not a game mix: it is muted whole and counted
     * (review r9: clamping such a block only turned it into full-scale noise); otherwise every sample is clamped to [-1, 1] */
    UInt32 n = frames * KH_AUDIO_OUT_CHANNELS; int bad = 0;
    for(UInt32 i = 0; i < n; i++){ float v = dst[i]; if(!(v >= -4.0f && v <= 4.0f)) { bad = 1; break; } }
    if(bad){ memset(dst, 0, (size_t)n * sizeof(float)); atomic_fetch_add_explicit(&kh_muted_blocks, 1, memory_order_relaxed); return noErr; }
    for(UInt32 i = 0; i < n; i++){ float v = dst[i]; dst[i] = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }
    return noErr;
}
static int kh_ca_check(OSStatus st, const char *what){
    if(st == noErr) return 0;
    char cc[5]; UInt32 be = (UInt32)st; cc[0] = (char)(be >> 24); cc[1] = (char)(be >> 16); cc[2] = (char)(be >> 8); cc[3] = (char)be; cc[4] = 0;
    for(int i = 0; i < 4; i++) if(cc[i] < 32 || cc[i] > 126) cc[i] = '?';
    fprintf(stderr, "kh: audio-coreaudio: %s failed: OSStatus %d ('%s')\n", what, (int)st, cc);
    return -1;
}
static int coreaudio_start_locked(void);
int kh_coreaudio_start(void){ pthread_mutex_lock(&kh_ca_m); int rc = coreaudio_start_locked(); pthread_mutex_unlock(&kh_ca_m); return rc; }
static int coreaudio_start_locked(void){
    if(kh_unit_running || kh_unit) return kh_unit_running ? 0 : -1;
    if(!kh_audio_ring_enabled()){ fprintf(stderr, "kh: audio-coreaudio: the pull ring does not exist: run with KHNMC_AUDIO=pull\n"); return -1; }
    AudioComponentDescription d; memset(&d, 0, sizeof d);
    d.componentType = kAudioUnitType_Output; d.componentManufacturer = kAudioUnitManufacturer_Apple;
#if TARGET_OS_IPHONE
    d.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    d.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if(!comp){ fprintf(stderr, "kh: audio-coreaudio: no output audio unit found\n"); return -1; }
    if(kh_ca_check(AudioComponentInstanceNew(comp, &kh_unit), "AudioComponentInstanceNew")) return -1;
    KhAudioOutFormat f; kh_audio_out_format(&f);
    AudioStreamBasicDescription asbd; memset(&asbd, 0, sizeof asbd);
    asbd.mSampleRate = (Float64)f.rate; asbd.mFormatID = kAudioFormatLinearPCM; asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mChannelsPerFrame = f.channels; asbd.mBitsPerChannel = 32; asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = (UInt32)(sizeof(float) * f.channels); asbd.mBytesPerPacket = asbd.mBytesPerFrame;
    AURenderCallbackStruct cb; cb.inputProc = kh_render; cb.inputProcRefCon = NULL;
    UInt32 maxslice = 0, sz = sizeof maxslice, slice;
    if(kh_ca_check(AudioUnitSetProperty(kh_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof asbd), "set the stream format (48 kHz float32 stereo interleaved)")
       || kh_ca_check(AudioUnitSetProperty(kh_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb), "set the render callback")
       || kh_ca_check(AudioUnitInitialize(kh_unit), "AudioUnitInitialize")){
        AudioComponentInstanceDispose(kh_unit); kh_unit = NULL; return -1;
    }
    if(kh_ca_check(AudioUnitGetProperty(kh_unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxslice, &sz), "read the largest slice (MaximumFramesPerSlice)")){
        fprintf(stderr, "kh: audio-coreaudio: sizing the ring target for %u-frame slices instead\n", KH_AUDIO_OUT_MAX_SLICE); maxslice = 0; }
    slice = maxslice ? maxslice : KH_AUDIO_OUT_MAX_SLICE;
#if TARGET_OS_IPHONE
    if(slice < KH_AUDIO_OUT_MAX_SLICE) slice = KH_AUDIO_OUT_MAX_SLICE;                   /* the screen-locked slice, whatever the property said */
#endif
    if(slice * 2 > f.target_frames && kh_audio_out_set_target(slice * 2)){
        fprintf(stderr, "kh: audio-coreaudio: FAILED: a target fill of %u frames (twice the %u-frame slice) does not fit the %u-frame ring; not started\n", slice * 2, slice, f.ring_frames);
        AudioUnitUninitialize(kh_unit); AudioComponentInstanceDispose(kh_unit); kh_unit = NULL; return -1;
    }
    kh_audio_out_format(&f);                                  /* what took effect */
    kh_audio_out_attach_device(1);                            /* from here the ring may not be rebuilt (a named fatal if anything tries) */
    kh_audio_out_start();                                     /* before the unit asks for its first slice: the mixer begins to fill */
    if(kh_ca_check(AudioOutputUnitStart(kh_unit), "AudioOutputUnitStart")){
        kh_audio_out_stop(); kh_audio_out_attach_device(0); AudioUnitUninitialize(kh_unit); AudioComponentInstanceDispose(kh_unit); kh_unit = NULL; return -1;
    }
    kh_unit_running = 1;
    fprintf(stderr, "kh: audio-coreaudio: %s started, %u Hz float32 stereo, largest slice %u frames (property %u), ring target %u frames of %u\n",
#if TARGET_OS_IPHONE
            "RemoteIO",
#else
            "default output unit",
#endif
            f.rate, (unsigned)slice, (unsigned)maxslice, f.target_frames, f.ring_frames);
    return 0;
}
void kh_coreaudio_stop(void){
    pthread_mutex_lock(&kh_ca_m);
    if(kh_unit){
        if(kh_ca_check(AudioOutputUnitStop(kh_unit), "AudioOutputUnitStop")){
            /* review r9: the unit may still be pulling: the ring stays guarded and the unit is kept (a later stop can try again) */
            fprintf(stderr, "kh: audio-coreaudio: the unit did not stop: it keeps the ring attached\n");
        } else {
            kh_audio_out_stop();
            kh_audio_out_attach_device(0);                    /* the unit is stopped: it reads the ring no more */
            AudioUnitUninitialize(kh_unit); AudioComponentInstanceDispose(kh_unit); kh_unit = NULL; kh_unit_running = 0;
            unsigned long long m = atomic_load(&kh_muted_blocks);
            if(m) fprintf(stderr, "kh: audio-coreaudio: %llu block(s) were muted as not a game mix\n", m);
        }
    }
    pthread_mutex_unlock(&kh_ca_m);
}
#endif /* KH_AUDIO_COREAUDIO */
