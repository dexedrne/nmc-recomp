// KHNMC DECOMP EDITION - platform seam: AUDIO ('sd' engine on XAudio2 -> CoreAudio). Design sketch, not compiled.
// Details and evidence: ../02-AUDIO.md, ../audio_surface.json, ../sd_classes_contract.json, ../sd_game_api.json
#pragma once
#include <cstdint>

namespace khnmc::audio {

// ---------------------------------------------------------------------------------------------------------------
// Stage 1: an XAudio2 shim that the LIFTED sd/XA2 driver code calls unchanged (same idea as the D3D12 shim).
// The lifted code reaches XAudio2 only through COM vtables (guest pointers). Each interface below is a table of
// guest-callable thunks; the displacement is the vtable byte offset seen in the exe.
// ---------------------------------------------------------------------------------------------------------------
enum IXAudio2Slot : uint16_t {            // IXAudio2 (global at .data 0x2b81230)
  X2_Release = 0x10, X2_RegisterForCallbacks = 0x18, X2_UnregisterForCallbacks = 0x20,
  X2_CreateSourceVoice = 0x28,            // only call site 0xcf9dc (inside 0xcf070): flags 0, MaxFrequencyRatio = float at .data 0x78a964 (=2.0), pCallback = &VoiceXA2+8
  X2_CreateSubmixVoice = 0x30,            // 0xa99e0 reverb, 0xaae05..0xab00a (BusXA2 x5), 0xcf733, 0xc4da0
  X2_CreateMasteringVoice = 0x38,         // 0xa850e, 0xa85a7, 0xcf70e (MasterBusXA2)
  X2_StartEngine = 0x40, X2_StopEngine = 0x48, X2_GetPerformanceData = 0x58 };
enum IXAudio2VoiceSlot : uint16_t {       // IXAudio2Voice base (no IUnknown): used subset (see audio_surface.json for site counts)
  V_SetOutputVoices = 0x08, V_SetEffectChain = 0x10, V_EnableEffect = 0x18, V_DisableEffect = 0x20,
  V_SetEffectParameters = 0x30, V_SetFilterParameters = 0x40, V_SetOutputFilterParameters = 0x50,
  V_SetVolume = 0x60, V_SetChannelVolumes = 0x70, V_SetOutputMatrix = 0x80, V_DestroyVoice = 0x90,
  // IXAudio2SourceVoice extras
  S_Start = 0x98, S_Stop = 0xa0, S_SubmitSourceBuffer = 0xa8, S_FlushSourceBuffers = 0xb0, S_Discontinuity = 0xb8,
  S_ExitLoop = 0xc0, S_GetState = 0xc8, S_SetFrequencyRatio = 0xd0, S_GetFrequencyRatio = 0xd8 };

struct XA2Buffer {                        // XAUDIO2_BUFFER, 0x30 bytes on x64
  uint32_t flags, audioBytes; const uint8_t* audioData;
  uint32_t playBegin, playLength, loopBegin, loopLength, loopCount; void* context; };
struct XA2VoiceState { void* currentBufferContext; uint32_t buffersQueued; uint32_t pad; uint64_t samplesPlayed; };  // 24 bytes

// Callbacks the shim must invoke on its audio/render thread (guest function pointers):
//   IXAudio2EngineCallback  : OnProcessingPassStart / OnProcessingPassEnd / OnCriticalError   (3 slots, RTTI EngineCallback, vtable 0x61beb0)
//   IXAudio2VoiceCallback   : OnVoiceProcessingPassStart(bytesRequired), OnVoiceProcessingPassEnd, OnStreamEnd,
//                             OnBufferStart(ctx), OnBufferEnd(ctx), OnLoopEnd(ctx), OnVoiceError   (7 slots; VoiceXA2 and PlaySoundStreamVoiceContext)
// XAPO host: 11 custom XAPOs run inside the voice/submix graph (IXAPO: GetRegistrationProperties, IsInputFormatSupported,
// IsOutputFormatSupported, Initialize, Reset, LockForProcess, UnlockForProcess, Process, CalcInputFrames, CalcOutputFrames).

struct SourceFormat {                     // what VoiceXA2 passes as WAVEFORMATEX (PCM16 after Vorbis decode; MS-ADPCM for SE bank)
  uint16_t formatTag;                     // 0x0001 PCM, 0x0002 MS-ADPCM, 0xFFFE extensible
  uint16_t channels; uint32_t sampleRate; uint32_t avgBytesPerSec; uint16_t blockAlign, bitsPerSample, cbSize; };

// ---------------------------------------------------------------------------------------------------------------
// Backend (CoreAudio / AVAudioEngine) contract that the shim sits on. One implementation, iOS 16+.
// ---------------------------------------------------------------------------------------------------------------
struct BackendConfig {
  double   sampleRate = 48000.0;          // the engine mixes at the device rate; source voices carry 22.05/24/44.1/48 kHz
  uint32_t outputChannels = 2;            // stereo; the engine can emit 4ch/5.1 (Surround4chSound, ProLogicIIz) - fold down in MasterBus
  uint32_t maxSourceVoices = 128;         // engine voice budget not recovered statically; size from a runtime trace
  uint32_t ioBufferFrames = 256;
};
class Backend {
 public:
  virtual ~Backend() = default;
  virtual bool start(const BackendConfig&) = 0;
  virtual void stop() = 0;
  // render thread entry: pulls every active source voice, runs effect chains (XAPO / reverb / filters), sums submixes,
  // applies SetOutputMatrix/volumes/frequency ratio, writes interleaved float to the device.
  virtual void renderCallback(float* out, uint32_t frames) = 0;
};
// MS-ADPCM block decoder (codec 12, SE bank): blockAlign 2 channels = 2*? ; standard 7 coefficient table; 4-bit nibbles.
int decodeMsAdpcm(const uint8_t* block, int blockBytes, int channels, int16_t* outInterleaved);
// SCD container reader (SEDBSSCF v3 LE): header size u16 @0x0e, counts u16[4] @hdr, offsets u32[4] @hdr+8, per-sound header
// {u32 streamSize, channels, sampleRate, codec(6 Vorbis | 12 MS-ADPCM), loopStart, loopEnd, extraSize, u16 auxCount}.
struct ScdSound { uint32_t streamSize, channels, sampleRate, codec, loopStart, loopEnd; const uint8_t* data; };

// ---------------------------------------------------------------------------------------------------------------
// Stage 2 (replacement of the lifted XA2 layer, optional): implement the sd backend interfaces directly.
//   IVoice (56 vtable slots)  <- XA2::VoiceXA2        IDSPBus (2) <- XA2::BusXA2     MasterBus (8) <- XA2::MasterBusXA2
//   IEffect (9) <- XA2::ReverbXA2 / SimpleMeterXA2    IDataTransfer (24) / StreamDataTransferEx (27)
// Slot tables: ../sd_classes_contract.json.
// ---------------------------------------------------------------------------------------------------------------
}  // namespace khnmc::audio
