# 2. Audio: the 'sd' engine on XAudio2, sized for a CoreAudio replacement

Scope: Batch B2, item 2. Static analysis of `kh2fm.exe` (sha256 `9002b2de6a1f91a7...`), RVAs relative to `0x140000000`. Machine-readable: `audio_surface.json`, `sd_classes_contract.json`, `sd_game_api.json`, `sd_entry_points.json`, `xa2_*.json`, `scd_stats.json`; header sketch `include/khnmc_audio.h`; scripts `t2_*.py`.
Confidence tags: **H** read from disassembly or data, **M** structural inference, **L** guess. The XAudio2 voice-call counts are ceilings unless tagged H (see 2.3).

## 2.1 Layer map

| layer | what it is | where | size |
|---|---|---|---|
| OS | `xaudio2_9.dll`, MMDevice/IAudioClient COM | imports + dynamic load | 3 ordinal imports, 2 dynamically resolved names |
| XA2 backend | SQuare Enix "CDev Sd" driver backend for XAudio2: `AudioXA2`, `EngineCallback`, `MasterBusXA2`, `BusXA2`, `VoiceXA2`, `ReverbXA2`, `SimpleMeterXA2`, filters, 11 XAPOs, Adpcm stream transfer | 0x77a30..0xdb2f5 (XA2 classes), XAPO code 0xc4360..0xcd076, 0xeef30..0xef143 | 35 `::XA2::` classes (378 functions by class label), 15 XAPO-family RTTI classes |
| sd core | platform independent: `SoundDriver` (77 virtuals), banks (`Bank` 46, `ActualBank` 47, `StreamBank` 48, `StreamBankEx` 49), sounds (`RootSound` 111, Stereo/Engine/Surround4ch/Random/Cycle/Order/Atomosgear), controllers (`SoundController`, `BankController`, `BusController`, `ReverbController`, `EffectController`, `ToolBankController`), `DynamicValue`, Layout (`LayoutManager` 75, `ListenerManager` 57, point/line/polygon sound objects), `Whiz` | 0x30dc0..0xf8xxx | 126 RTTI classes under `SQEX::CDev::Engine`, 3,505 vtable slots; 3,027 universe functions carry an sd label once the HID cluster is removed |
| Axa sound layer | `AxaSoundManager`, `AxaSoundStream`, `Sound::CStream`, `BankEntry`, `PlaySoundStreamVoiceContext` (an `IXAudio2VoiceCallback` implementer), `RumblingEntry` (input) | 0x130000..0x144000 | 6 classes, ~150 functions |
| game wrappers | the KH2 side calls: clusters near 0x1d0000..0x1e0400 (gummi/yasui/harata callers), 0x2c0000..0x2d0000, 0x5280f0, plus `VAG_STREAM::*` | scattered | see 2.4 |

**S7 name DB caveat (new finding).** S7's `sd-audio` label is wrong for 0x448000..0x450400 (152 functions): that range is the raw HID game-pad driver (`hid.dll`, setupapi, Sony VID 0x054c). It is excluded from every count here. Also `0xcf070` is labelled `GranularSynthesizerXA2::ctor_or_dtor` but is the VoiceXA2 initialiser (it contains the only `CreateSourceVoice` call, 0xcf9dc).

## 2.2 How XAudio2 is reached (H)

- **Engine creation** (0x953a0 / 0x95730 / 0x95f90): `LoadLibraryExW(L"xaudio2_9.dll")`, `GetProcAddress("XAudio2CreateWithVersionInfo")` and `("XAudio2Create")` into globals 0x2b82e50 / 0x2b82e58; the `IXAudio2*` ends up in global **0x2b81230**. `CoInitializeEx` is called first.
- **Static ordinal imports** (3): `xaudio2_9!ord1` at 0x13d3a0 (movie mp4 thread 0x13d260, args `(IXAudio2**, 0, XAUDIO2_DEFAULT_PROCESSOR)`, i.e. a **second, independent IXAudio2** for movie audio); `ord2` at 0xa98f9 (ReverbXA2 ctor, `HRESULT f(IUnknown**)`: a system effect factory, CreateAudioReverb-style); `ord3` at 0xa8482 (MasterBusXA2 ctor, same signature; M: volume-meter/reverb sibling factory).
- **Device query**: `CoCreateInstance(CLSID_MMDeviceEnumerator, IID_IMMDeviceEnumerator)` (0x95505), default render endpoint, `Activate(IID_IAudioClient)`; mix format / channel mask is read into globals 0x78a960/0x78a968 (`CoTaskMemFree`). There is **no X3DAudio** (no import, no `GetProcAddress` string): 3D positioning is done by the sd Layout code in CPU.
- No `VirtualAlloc`, no WASAPI streaming, no DirectSound, no XACT.

## 2.3 XAudio2 call surface

**IXAudio2** (receiver = global 0x2b81230, 19 vtable call sites; plus calls through a local copy in the ctors) (H):

| method | vtbl disp | sites |
|---|---|---|
| Release | 0x10 | 3 |
| RegisterForCallbacks | 0x18 | 1 |
| UnregisterForCallbacks | 0x20 | 3 |
| CreateSourceVoice | 0x28 | 1 (0xcf9dc, flags 0, float MaxFrequencyRatio = .data 0x78a964 = 2.0f, callback = `this+8`, out voice = `this+0x18`) |
| CreateSubmixVoice | 0x30 | >= 10 call sites: BusXA2 ctor x5 (0xaae05, 0xaae6b, 0xaaf11, 0xaaf73, 0xab00a), ReverbXA2 ctor 0xa99e0, default submix 0x958c0 (0x9594d), 0xcf733, 0xc4da0; 4 of them use the global receiver |
| CreateMasteringVoice | 0x38 | 3 (0xa850e, 0xa85a7, 0xcf70e) |
| StartEngine / StopEngine | 0x40 / 0x48 | 2 / 3 |
| GetPerformanceData | 0x58 | 3 (0x95b17, 0x95cd9 and 0x95af0 family: debug stats) |

Global **0x2b81200** is the `IXAudio2MasteringVoice*` (`DestroyVoice` vtbl+0x90, 5 sites); 0x2b81208 / 0x2b81210 / 0x2b811e8 / 0x2b811f0 hold the default submix and further voice/effect pointers created in `AudioXA2::CreateDefaultSubmixVoice` (0x958c0, S7 name correct).

**IXAudio2SourceVoice** methods used by `VoiceXA2` (receiver = `[VoiceXA2+0x18]`, **tight set**, H): Stop 10, SetFrequencyRatio 9, SetVolume 7, Start 5, FlushSourceBuffers 5, SetOutputMatrix 4, SetEffectChain 2, SubmitSourceBuffer 2, GetState 1, SetEffectParameters 1. Further voice methods appear in the XA2 region as vtable calls whose receivers are other voice/submix objects; the **ceiling** over `0x77000..0xdc000` + `0x13a000..0x13f000` (includes sd-internal virtuals, so treat as an upper bound): DestroyVoice 25, SetOutputVoices 57, SetEffectChain 77, EnableEffect 29, DisableEffect 52, SetEffectParameters 31, GetEffectParameters 39, SetFilterParameters 53, SetOutputFilterParameters 17, SetVolume 28, SetChannelVolumes 25, SetOutputMatrix 13, Start 32, Stop 21, SubmitSourceBuffer 11, FlushSourceBuffers 13, ExitLoop 11, GetState 23, SetFrequencyRatio 20, Discontinuity 5. Never present as plausible XAudio2 receivers: GetVolume (1), GetFrequencyRatio (2), SetSourceSampleRate (1), GetChannelVolumes (2), GetOutputMatrix (2).

**Callbacks** (H): `IXAudio2EngineCallback` (RTTI `AudioXA2::EngineCallback`, 3 slots), `IXAudio2VoiceCallback` implemented by `VoiceXA2` (secondary vtable 0x622310, 7 slots, object offset +8) and by `PlaySoundStreamVoiceContext` (vtable 0x5ae710, 8 slots). The shim must call them from its audio thread in XAudio2 order.

**Effects** (H): the XAPOs are *in-process C++* (`CXAPOBase`-derived): IIRFilter (biquad low/high/band/notch/peaking/shelving, 8 `FilterXA2` classes), PrologicIIz, StereoMixdown, AudioBounce, Stringendo, Compressor, Fourier, LoFi, GranularSynthesizer, GranularPitchShift, SimpleMeter. They run unchanged if the shim hosts the `IXAPO` calling sequence (`GetRegistrationProperties`, `IsInputFormatSupported`, `IsOutputFormatSupported`, `Initialize`, `Reset`, `LockForProcess`, `UnlockForProcess`, `Process`, `CalcInputFrames`, `CalcOutputFrames`). The one **system** effect is the reverb created through `ord2` (ReverbXA2, 21 vtable slots, drives `XAUDIO2FX_REVERB_PARAMETERS`-style values): Microsoft's FXReverb is closed, so a CoreAudio build needs its own implementation (see 2.6, the main audio-quality risk).

## 2.4 The sd engine's game-facing API (counts)

Definition used: a function that belongs to the sd/Axa-sound layers (universe label sd, or RTTI class under `SQEX::CDev::Engine` / `Axa::AxaSound*` / `Sound::CStream` / `PlaySoundStreamVoiceContext` / `BankEntry`; HID range removed) and has a direct `call`/`jmp` caller outside that set. Trivial `ret` stubs are dropped.

- **249 entry targets, 839 call sites.** By caller region: 373 sites from unlabeled game code, 319 sites from **EH cleanup funclets** in the 0x560000.. tail of `.text` (unwind code that destroys stack-resident controller/handle objects: they pair with the constructor sites in game functions and are not independent API use; most target `BusController`/`SoundController` dtors such as 0x327a0 and 0x31e80), 31 settingmenu, 30 gummi, 22 axa-platform, 18 harata-dk (2D UI), 16 input (rumble), 13 yasui, 7 naka-kn, 5 sasa-musical, 5 tozawa-menu.
- **Excluding the cleanup funclets: 235 entry targets, 520 call sites**; the top groups: the "kh2 sound wrappers" at 0x1dc000..0x1e0400 (44 targets, 150 sites, callers: gummi, yasui, naka, harata, tozawa), the 0x2c0000 bucket (23 targets / 69 sites; includes tiny accessors such as 0x2ce6b0 which converts a packed u16 pair to float, and 0x2ce670/0x2ce650), the 0x2d0000 bucket (23 / 25), the 0x1e0000 bucket (14 / 25), the Axa layer 0x130000..0x144000 (24 targets / 44 sites), `SoundObject` 0x3bfc0 (13 sites), `SoundController` ctor 0x80c50 (the "controller handle" pattern: construct, call, destruct), `BankController` 0x7d3a0, `BusController` 0x7f7e0 (setters like 0x7fad0 = SetVolume, used by 0x137d50), `ReverbController` 0x7cee0, `RootSound` 0xbb250, `DynamicValue` 0x8cc80/0x86260, `LayoutManager` 0xb6e70, `SoundDriver` (7 sites, 6 targets). Full list with callers: `sd_game_api.json`.
- `VAG_STREAM::play/fadeOut/setVolume/exit` (0x537420 / 0x5373e0 / 0x5374a0 / 0x5373a0): **6 / 12 / 2 / 10 call sites**. `play(name, vol, fadeVol, ms)` builds `"%s/%s"` from BasePath + name and creates an `AxaSoundStream` (singleton pointers 0x2b591c0/0x2b591c8); `setVolume(v)` converts `v * (1/16383)` to a float 0..1; `exit` fades then destroys. Call sites are mostly gummi (0x211fec.., 0x1e20ff), the sd title wrapper 0x2b35a0, harata-dk (0x2b3c10, 0x2b9a20, 0x2e8930, 0x31b220), tozawa `Layout` 0x2b4590.
- 54 `Axa::DebugPrint` calls (0x143760, body is `ret`) in the sd/Axa code and 4 in `RumblingEntry`: dead in release; keep as stubs.

## 2.5 Asset formats and where they are decoded

Container: **SCD, `SEDBSSCF` version 3, little endian, header size 0x30** (`*.win32.scd`). Table counts (u16 x4) at the header size offset, table offsets (u32 x4) at +8; the second table lists sound headers `{u32 streamSize, channels, sampleRate, codec, loopStart, loopEnd, extraSize, u16 auxCount}`.
Scanning the 37 `.scd` files in the mod (bgm 14, vagstream 6, se 1, voice/us 16) gives **712 sound entries: 532 codec 6 and 180 codec 12.**

| codec | engine class | content | decoded where |
|---|---|---|---|
| 6 Ogg Vorbis | `OggVorbisDataTransfer` (24 slots), `OggVorbisStreamDataTransferEx` (27) | BGM 44.1/48 kHz stereo, voice 22.05/24 kHz mono (496 entries), vagstream (Title 9.6 MB) | **in the engine, on the CPU** (statically linked vorbis); the voice receives PCM |
| 12 Microsoft ADPCM | `AdpcmStreamDataTransferEx` (XA2 subclass, 27 slots) | SE bank `se/se999.win32.scd`: 180 stereo entries at ~48 kHz | **by XAudio2**: the engine submits ADPCM blocks with a `WAVEFORMATEX` ADPCM tag, so the replacement must decode MS-ADPCM itself (small decoder; blockAlign taken from the format) |

Other inputs: `12soundinfo.bar` and `KHHD2.8.config` (read through `LoadFileWithMalloc` at 0x1366da, 0x136736, 0x143862 with `useHdAsset = 0`), bank loaders 0x3f9850/0x3f99e0 (`%PROJ%`/`%s/%s`/`.scd`, `LoadFile` with `useHdAsset = 1`), `PlaySoundStreamVoiceContext` ("AesReCipher Thread": a cipher layer on stream data; same data-layer category as the package helpers, not examined). The mod's `vagstream/GM1..GM5` files are 4.7 KB stubs (gummi music replaced).

Remastered audio: 98 SCD files sit in the **remastered** tree (225 MB: `remastered/obj/*.a.us/se/obj/seNNN`, `.../voice/battle/us/<name>`); they are the "ORIGIN" audio slots (`origOffset == -1`) of the object archives and must be fed through the same remastered protocol as textures (01-FILE-LAYER.md 1.3.3).

User settings: `VolumeLevels[11]` = 0.0..1.0; `PCSettings` payload `+0x10 u16 MasterVolume`, `+0x12 u16 MusicVolume`, more u16 at +0x14 (default 10 = 1.0 for all four); `0x137d50` applies `BusController(bus).SetVolume(VolumeLevels[value])` for bus ids 1..10.

## 2.6 Mapping to CoreAudio / AVAudioEngine and what a replacement must provide

Recommended stage 1 (consistent with the G1 D3D12 shim decision): **an XAudio2 shim under the lifted sd/XA2 code**, because the XA2 backend and all 11 XAPOs are plain CPU code that lifts as-is and keeps the engine's own mixing semantics. The shim exposes: `IXAudio2` (Release, Register/UnregisterForCallbacks, CreateSource/Submix/MasteringVoice, Start/StopEngine, GetPerformanceData), `IXAudio2Voice` (SetOutputVoices, SetEffectChain, Enable/DisableEffect, Set/GetEffectParameters, SetFilterParameters, SetOutputFilterParameters, SetVolume, SetChannelVolumes, SetOutputMatrix, DestroyVoice), `IXAudio2SourceVoice` (Start, Stop, SubmitSourceBuffer, FlushSourceBuffers, ExitLoop, GetState, SetFrequencyRatio, Discontinuity), the two callback interfaces, the XAPO host, and the three factory entry points (`XAudio2Create`, `XAudio2CreateWithVersionInfo`, the two `IUnknown**` factories). Implementation: **one render callback** (AVAudioSourceNode or a RemoteIO unit) that owns the voice graph, i.e. do not map each voice to an `AVAudioPlayerNode`; XAudio2 semantics (sample-accurate queues, OnBufferEnd/OnStreamEnd, SetOutputMatrix, frequency ratio with sinc-quality SRC) are easier to reproduce in one mixer than through AVAudioEngine node graphs.

| XAudio2 / OS | CoreAudio |
|---|---|
| IXAudio2 + MasteringVoice | `AVAudioEngine` output / RemoteIO at the device rate (48 kHz, float32 stereo), `AVAudioSession` category playback, interruption + route-change handling (replaces the MMDevice query: report 2 channels, mask 0x3) |
| SourceVoice | internal voice: input format PCM16 or MS-ADPCM->PCM16 (decode at submit), queue of `XA2Buffer` (flags, loop begin/length/count), `SetFrequencyRatio` resampler (max ratio 2.0 as passed), `GetState` = buffers queued + samples played |
| SubmixVoice | internal mix bus with own channel count/sample rate and effect chain |
| XAPO chain | run the lifted `Process` code inside the callback; `LockForProcess/UnlockForProcess` around graph changes |
| ReverbXA2 (system effect) | own FDN/Schroeder reverb driven by the XAUDIO2FX_REVERB_PARAMETERS the sd code sets (WetDryMix, ReflectionsDelay, ReverbDelay, RearDelay, PositionLeft/Right, RoomFilterFreq/Main/HF, ReflectionsGain, ReverbGain, DecayTime, Density, RoomSize); `AVAudioUnitReverb` presets are too coarse for "audio quality matters" |
| Engine/voice callbacks | invoked from the render thread (or a high-priority serial queue fed by it); never from the IO block with locks that the lifted code can hold |
| Movie audio (second IXAudio2, `ord1`) | route through the movie player (AVFoundation) instead; the `AesReCipher` thread and MF source reader are replaced with the movie path (graphics-spec/MOVIES.md) |

Stage 2 (optional replacement of the lifted XA2 layer by clean code) targets the sd backend interfaces instead of XAudio2: `IVoice` (56 vtable slots, `VoiceXA2` implements 56 + the callback table), `IDSPBus` (2), `Bus`/`MasterBus` (8), `IEffect` (9), `IDataTransfer` (24) / `StreamDataTransferEx` (27). Slot tables with function RVAs: `sd_classes_contract.json`.

Must-provide list: (1) engine create/start/stop and a device-rate/channel report; (2) all voice kinds with sample-accurate buffer queues and the callback order; (3) resampling by frequency ratio; (4) output matrices and per-channel volumes (Surround4chSound, ProLogicIIz: 4-channel sources down-mixed to stereo by the engine's own XAPOs, so the master bus can stay stereo); (5) effect chains with parameter blobs; (6) MS-ADPCM decode; (7) a reverb; (8) thread priority hints (QoS user-interactive for the render thread); (9) background/interrupt handling (`StopEngine`/`StartEngine`, `SystemPause` = `Axa.Sound.Sys.SystemPause` 0x13b2d0, called around device suspend 0x1042e0/0x104451).

## 2.7 Threading (H unless noted)

| thread | creator | role |
|---|---|---|
| `CDevSd MSECHandler` | `CThread`-like creator 0xfa9b0 (`_beginthreadex`, semaphore, `SetThreadPriority`, `GetProcessAffinityMask`, `SetThreadIdealProcessor`) via TimerCallback ctor 0x7b220; timer by `timeSetEvent` (0xfb040, 0x126940, 0x1287d0) | sd 1 ms driver tick |
| `CDevSd VSYNCHandler` | 0xfa9b0 via 0x7b450 | per-frame sd tick |
| StreamReadThread | 0xfa9b0 via 0x97250 (from `SoundDriver` 0x91250) | stream bank read-ahead, `CreateEventW` + critical sections |
| `TRANSFER Thread` x2 | `Axa::CThread::Create` 0xfdd00 from `BankEntry` ctors 0x135600, 0x13b2e0 | bank data transfer |
| `Stream Thread` x2 | 0xfdd00 from `CStream` ctor 0x135870 and `AxaSoundStream` ctor 0x13a8d0 | VAG/BGM streams; `_threadProc` 0x13c090 allocates a **16 MB** aligned buffer (0x13c157; 0x13ab75 in the ctor) and reads through `OpenStream` |
| `AesReCipher Thread` | 0xfdd00 from 0x13cee0 | movie/stream cipher |
| XAudio2 engine thread | inside the DLL | all XAudio2 callbacks |

Synchronisation inside sd: 824 of the 1,102 `EnterCriticalSection` sites are in the sd range 0x40000..0xdffff (S3). The replacement needs recursive mutexes and the 1 ms / vsync timer threads; 3 `timeSetEvent` sites and `timeBeginPeriod` (7) / `timeEndPeriod` (9) must be honoured (the VSYNC handler is paced by the game frame, the MSEC handler by a 1 ms timer).

## 2.8 Flows (M, from names and call structure)

- **BGM**: game wrapper -> `BankController`/`SoundController` (sd core) -> `StreamBank(Ex)` + `StreamReadThread` -> `OggVorbisStreamDataTransferEx` decodes -> `VoiceXA2` source voice -> submix buses -> master. Files: `bgm/musicNNN.win32.scd` via `LoadFile*`.
- **Title/gummi/streams**: `VAG_STREAM::play` -> `AxaSoundStream` (own thread, 16 MB buffer) -> `PlaySoundStreamVoiceContext` (voice callback context) -> an IXAudio2 source voice; volume/fade through `VAG_STREAM::setVolume/fadeOut`.
- **SE**: bank `se/se999.win32.scd` (ADPCM) loaded by 0x3f9850/0x3f99e0 into a `BankEntry`; sounds triggered through `RootSound`/`SoundObject` (Layout objects give 3D position, obstruction, listener).
- **Voice**: `voice/<lang>/<bank>/*.win32.scd` Vorbis banks; per-object remastered replacements through the remastered protocol.
- **Rumble** shares the Axa sound manager: `RumblingEntry` (see 03-INPUT.md).

## 2.9 Open items
- Exact `CreateSourceVoice` parameter set per sound type (format tag extensible, flags) needs one more pass over 0xcf070.
- The voice budget (max simultaneous voices) is not a constant in the code paths read; take it from a runtime trace of the lifted build.
- `ord3` function identity (volume meter vs reverb sibling) is unconfirmed; only the signature `HRESULT(IUnknown**)` is.
- The `AesReCipher` stream layer is a data-layer cipher; treat like the package helpers (describe only).
