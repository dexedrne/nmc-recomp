# Batch B2: platform seams (file layer, audio, input, Steam facts, other seams)

KHNMC DECOMP EDITION: lift `kh2fm.exe` (sha256 `9002b2de6a1f91a7...`, PE32+ x64, image base `0x140000000`) to C and run it on a hand-written runtime. This batch specifies the seams between the lifted code and the host platform. Static analysis only (pefile + capstone); nothing was run. Everything here is derived data and stays in scratch. Scale is expressed as counts only.

| file | content |
|---|---|
| `01-FILE-LAYER.md` | Panacea patterns resolved (24/24 match), engine file API semantics, loose-tree layout and `useHdAsset`/remastered protocol (validated on 224 real files), package-bypass decision and every other consumer |
| `02-AUDIO.md` | 'sd' engine on XAudio2: layers, XAudio2 call surface, game-facing API counts, asset formats (SCD, Vorbis, MS-ADPCM), CoreAudio mapping, threading, flows |
| `03-INPUT.md` | XInput/DirectInput8/HID, pad abstraction (GamePad, PADDATA), PS2-order button table, polling model, rumble, GameController + touch replacement |
| `04-STEAM-FACTS.md` | Facts only: 9 imports, 60 call sites, wrapper functions, state, dependency graph. No stub or bypass proposed |
| `05-OTHER-SEAMS.md` | Saves (PNG + `sqEX`), `PCSettings`, window/DPI/fullscreen, timing, JobManager, memory, process items, full import census scoping |
| `IMPORT-CENSUS.md`, `import_census.json`, `include/khnmc_imports.def` | all 368 imports in 35 DLLs with call-site counts, S3 deltas, category and runtime treatment |
| `platform_seams.json` | one-page machine-readable roll-up of the counts below |
| `file_layer_functions.json`, `pat_raw.json`, `walk_check.json`, `modyml_assets.json` | file-layer function table, raw pattern results, walker validation, mod.yml asset list |
| `audio_surface.json`, `sd_classes_contract.json`, `sd_game_api.json`, `sd_entry_points.json`, `xa2_*.json`, `scd_stats.json` | audio surface data |
| `input_surface.json`, `input_entry_points.json` | input surface data |
| `steam_surface.json` | Steam sites, wrappers, contexts, state |
| `name_proposals.csv` | 58 proposed names (rva, name, subsystem, confidence, evidence) for the S7 name DB |
| `include/khnmc_file_layer.h`, `khnmc_audio.h`, `khnmc_input.h`, `khnmc_win64rt.h` | small C++ header sketches (not compiled) |
| scripts | `seamlib.py` (loader, universe, S7, capstone helpers), `xref.py` (rip/call/IAT index over all 38,455 functions), `pat.py`, `dump.py`, `remaster_walk.py`, `t*.py` |

Re-run: `PY=<venv with pefile+capstone> ; $PY xref.py` (builds `xref.pkl`), then `pat.py`, `t1_*.py` ... `t5_*.py`. The venv used was the existing `scratch/graphics-spec/venv`.

## Headline results

1. **File layer.** All 24 Panacea byte patterns resolve in this exe (23 unique; `Axa::DebugPrint` has 2 hits, Panacea binds the lower). The package API is 12 small functions at 0x106000..0x107900 plus the resource registration at 0x12f070; the **loaders actually used are `LoadFileWithMalloc` (19 sites) and `LoadFile` (2 sites); `LoadFileWithSize` is never called**. A loose tree can replace the data path entirely; what must be kept is the "current file" state that the remastered-asset protocol reads, and the stream-fd API (`OpenStream/ReadStream/CloseStream`) used by sound streams. Loose layout: `<tree>/<rel>` + `<tree>/remastered/<rel>/<entry name>` (recursive, names <= 31 chars, order = dds by number, png by number, rest by name, truncated to the container-walker count). Validated: of 224 real mod files with remastered folders, **215 match the walker count exactly, 9 have extra files, 0 are short.**
2. **Audio.** The sd engine is 126 RTTI classes / 3,505 vtable slots on a 35-class XA2 backend. XAudio2 is created dynamically (`XAudio2CreateWithVersionInfo`/`XAudio2Create` via `LoadLibraryExW`), plus 3 ordinal imports; no X3DAudio, no VirtualAlloc. The XAudio2 call surface is 8 `IXAudio2` methods, about 10 source-voice methods (tight set; ceiling table in 02-AUDIO.md), 2 callback interfaces and 11 in-process XAPOs. Game-facing API: 249 entry targets / 839 sites (235 / 520 from game code; 319 sites are EH cleanup funclets). Formats: SCD v3 with Ogg Vorbis (codec 6, decoded in-engine) and MS-ADPCM (codec 12, decoded by XAudio2 today, so the replacement must decode it). Recommended: an XAudio2 shim with one render callback, own reverb.
3. **Input.** The pad word is PS2 order; XInput, DirectInput8 (mouse, keyboard, generic pads) and a raw HID DualShock 4 driver (152 functions that S7 had mislabelled `sd-audio`). 8 `GamePad` slots of 0x140 bytes at 0x8bb2a8 with three `std::function` objects each; per-frame poll from `0x105810` -> `0x134ff0`, **gated on the window being foreground**. Triggers are digital (>= 0x80), the engine shapes the sticks itself.
4. **Steam (facts).** 9 imports, 60 call sites in 36 functions; app id 0x26f26e; state 0x8cc420; the run gate call is `axaAppMain` 0x14f4a7 -> 0x105420 -> `ManageRun`, with the state == 1 test only on 0x105420's object-creating path (corrected by the verification pass: 0x104a60/0x1044d0 earlier in axaAppMain already create the object, see 04-STEAM-FACTS.md 4.4); language comes from `ISteamApps::GetCurrentGameLanguage`; the SteamID64 is part of the save path; achievements/stats/rich presence/Input/Deck queries are independent of game state.
5. **Other.** Save = PNG with a 6,991,872-byte private `sqEX` chunk (PS2 memory-card emulation); settings = 508-byte `SET-` block; DPI-unaware window; 6 job threads; 37.6 MB of static BSS; import census 368 / 354 referenced / 7,785 sites.

## Checks of earlier artifacts (corrections to carry forward)

- **S7 name DB.** 21 of 24 file/sound/settings functions had no name; `0x1064d0` (`FreeAllPackages`) was named `Axa::PackageFile::ctor_or_dtor` (wrong role). The `sd-audio` subsystem label over-extends: **0x448000..0x450400 (152 functions) is the HID game-pad driver**, and `0xcf070` (named `GranularSynthesizerXA2::ctor_or_dtor`) is the VoiceXA2 initialiser holding the only `CreateSourceVoice` call. The `steam` names are consistent with the code. 58 proposals in `name_proposals.csv`.
- **S3 import census.** `import_callsites.json` (343 imports, 7,518 sites) covers `.pdata` roots only; the full sweep finds 368 imports and 7,785 sites (58 imports differ, +267 sites; e.g. `Sleep` 16 -> 48, `OutputDebugStringA` 0 -> 39, `ReleaseMutex` 0 -> 37).
- **Universe table.** All 24 matched functions are function starts in the table with the sizes used here (the three leaf functions without `.pdata`: 0x106440, 0x106480, 0x102840 and the `ret` stubs are present); no extents needed correcting. The EH-cleanup funclets at 0x560000.. are universe `funclet`s, not IL thunks.
- **G1 / S4.** The Steam gate (state 0x8cc420, fn 0x14d610) and the `0x105420 -> 0x579bf0 ManageRun` structure agree with this batch, except that G1's "only if state is 1" holds only on the object-creating path (see 04-STEAM-FACTS.md 4.4); the new refinement is that the gate call site is axaAppMain 0x14f4a7 and that language detection also depends on the same state. The engine does **not** use X3DAudio.
- **Panacea table.** `LaunchGameSteam` matches 0x143690 here, which is a different function (exit-code file mapping); `axaAppMain`'s launcher-variant pattern does hit the real 0x14f1e0.
- **OPENKH-MOD-APPLICATION.md section 3.** The documented overlay order `raw/` then loose is confirmed; `LoadFileWithSize` is dead code in the Steam build; the HD-asset hooks key off the last opened package (global state), which the document did not call out.

## Open items for the owner or the next batches
1. SettingMenu package content is not in `data/kh2`; the baker must extract it (Apache-2.0 OpenKH Egs).
2. Steam: owner decision (G1 section 4 item 2) is still open; this batch records facts only.
3. Save portability: keep the PNG/`sqEX` container byte-compatible? The sqEX payload layout is not decoded.
4. Audio: reverb implementation choice (own FDN vs AVAudioUnitReverb) is the main fidelity risk; voice budget needs a runtime trace.
5. Runtime traces (owner-gated): pad slot assignment for pads 1..7, the max simultaneous voices, working-set size.
6. Remaining unverified: 69 vanilla-only remastered directories, `ord3` identity, the `.shared` dword 0x2bd4000 writers, ownership of `GetRemasteredAsset` buffers.
