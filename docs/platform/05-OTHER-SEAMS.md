# 5. Other seams: saves, settings, window, timing, threads, memory, and the full import census

Scope: Batch B2, item 5. Static analysis of `kh2fm.exe` (sha256 `9002b2de6a1f91a7...`), RVAs relative to `0x140000000`; two facts about the owner's install were taken from `build-host` by listing/reading headers only (names, sizes, PNG chunk headers; no save content copied, the SteamID64 is redacted as `<SteamID64>`). Machine-readable: `import_census.json`, `IMPORT-CENSUS.md`, `include/khnmc_imports.def`, `alloc_const_sizes.json`; header sketch `include/khnmc_win64rt.h`.

## 5.1 Saves

**Location (H, code + build-host listing).** `0x103e40` builds the directory: `SHGetFolderPathA` (Documents) -> `sprintf("%s\\My Games")` -> product folder -> platform folder -> `\\<SteamID64>` (`"\\%llu"`, .rdata 0x585e64; EPIC builds take `-epicuserid`, 0x585e58) -> `MakeSureDirectoryPathExists` (`imagehlp`). On the owner's PC: `Documents\My Games\KINGDOM HEARTS HD 1.5+2.5 ReMIX\Steam\<SteamID64>\` containing:

| file | size | what |
|---|---|---|
| `KHIIFM_WW.png` | 7,073,032 B | the KH2 FM save container (see below) |
| `KH1.5+2.5_system_WW.png` | 48,971 B | collection-level system data, same container type |
| `config1525.dat` | 508 B (0x1fc) | the `PCSettings` block, magic `SET-`, version 8 (5.2) |
| `steam_autocloud.vdf` | 51 B | written by the Steam client (AutoCloud rules); the game has **no** `ISteamRemoteStorage` call sites (`GetSteamRemoteStorage` 0x14d3b0 has 0 callers) |
| (other games) `KHFM_WW.png`, `KHReCoM_WW.png` | 18.8 MB / 1.6 MB | KH1 / Re:CoM saves |

**Format (H for the container, M for the contents).** `KHIIFM_WW.png` is a valid PNG: `IHDR` 320 x 176, 8-bit RGB; a `tEXt` chunk (43 bytes, "Author" = SQUARE CO.,LTD. / created by Somnium, written with libpng 1.5.0 by 0x145170); a **private ancillary chunk `sqEX` of 6,991,872 bytes (0x6AB000)** that carries the save data; then 10 `IDAT` chunks (9 x 8,192 B + 1 x 7,200 B = the preview image). The chunk name string is at .rdata 0x5ae908 and is used by 0x144d00 (the chunk reader/writer; `ChunkCheck` at 0x586008 is a different string). The shipped game data contains template carriers: `save_image/en<name>` and `save_image/jp<name>` (`%s/save_image/en%s`, .rdata 0x5859e8/0x585a00; `<name>` = `KHFM.png`, `KHReCoM.png`, `_WW.png` combinations, 0x5859c0..0x5859e8) which `0xfef80` loads through `LoadFileWithMalloc` (call 0xff319, relative to BasePath) and rewrites with the user's data.
The save layer emulates **PS2 memory-card directory semantics** on top of this container (strings `icon.sys`, `mc_icon.bin`, `frec_dat.ico`, `frec_sys.ico`, `system.bin`, `SYS`, `BBS`, per-slot `%s%d/%s` directories, `_mkdir`, `FindFirstFileA` listings, `FileTimeToLocalFileTime`; functions 0xff800..0x1014a0 and `/MareLauncher` entries at 0x101260/0x1014a0). The sqEX payload is therefore a memory-card image; its internal layout was not decoded.
**Size/budget:** `0x100810` calls `GetDiskFreeSpaceExA` and `CreateDirectoryA` (free-space check before saving).
**Encryption/helpers (data layer, described only):** no save-specific cipher was found in the save functions; the exe's package cipher (`DecryptFile`, 01-FILE-LAYER.md 1.2) is platform-gated (platform id 0) and applies to package data, and a separate stream cipher layer exists for movies/streams (`CCipherMovie`, `AesReCipher Thread`). No key material was looked for.
**Port:** `GetFolderPath`-style calls map to the app's Documents/Application Support directory; the SteamID64 path component needs a stable user id (an open item tied to 04-STEAM-FACTS.md); `MakeSureDirectoryPathExists` = `mkdir -p`; keep the PNG container byte-compatible so existing PC saves remain loadable (owner decision: do saves need to be portable from the PC game?).

## 5.2 Settings: `PCSettings`

Block at `.data 0x715350` = `{ u32 magic 'SET-' (0x2d544553), u32 version 8, u32 size 0x1fc, u32 0, ...payload }`; the payload pointer is **0x715368** (Panacea's `PCSettingsPtr`). Initialised by `0x14d070` (`memset` 0x1fc, header, then `0x14ce50` defaults); persisted as `config1525.dat` (508 bytes) next to the saves. Defaults written by `0x14ce50` (payload-relative offsets, H for the values, M for the meanings):

| offset | default | meaning |
|---|---|---|
| +0x00 | u8 1 | screen mode (windowed/fullscreen flag) (M) |
| +0x02 / +0x04 | u16 0x0780 / u16 0x0438 | resolution 1920 x 1080 (confirmed by the owner's file: `80 07 38 04`) |
| +0x06, +0x08..+0x0f | 1; 0x10000, 0x10000 | flags/vsync/frame-rate options (M) |
| **+0x10** | u16 10 | **MasterVolume** index into `VolumeLevels[11]` (Panacea) |
| **+0x12** | u16 10 | **MusicVolume** index |
| +0x14 / +0x16 | u16 10 / 10 | further volume indices (voice/SE, M) |
| +0x18 | 1 | |
| +0x1c | u16 0/1/2 by the argument of 0x14d070 | language/region variant (M) |
| +0x1e | 0x140000 | |
| +0x24 .. +0xe0 | key bindings: DirectInput scancodes and 0x1001..0x100a pad-button ids | PC keyboard/pad configuration (CKeyConfig / CGamePadMenu) |
| +0xe8 | 8 x { u32 0xffff } stride 0x20 | pad-config slots; +0x104 onward 8 x 0x20 bytes of {0 x 0x18, 0xff x 8} (identity maps) |

For iOS: keep the block (the lifted `SettingMenu` code reads and writes it), seed the resolution fields from the device (see 5.3), and hide the PC-only menus.

## 5.3 Window, DPI, fullscreen

User32/GDI use (67 + 296 sites) is confined to: GX window creation `0x11d0a0` (called from `Axa.GX.GXDevice.init` 0x11d070: `RegisterClassExA` class `AxaSimpleWindow`, `CreateWindowExA`, `AdjustWindowRect`, `LoadCursorA`, `ShowWindow`, plus the 2 GX threads via `_beginthreadex`); mode/size changes `0x117fd0` (swap-chain create/resize; `MonitorFromWindow`, `GetMonitorInfoA`, `SetWindowPos`, `IsZoomed`, `GetClientRect`), `0x114e20`, `0x121d90` (`SetWindowLongPtrW` borderless style), `0x123020`, `0x124970` (`GXDevice.resize` 0x124940); resolution enumeration `0x11c970` (`EnumDisplaySettingsA`, called by `GXDevice.getResolutions` 0x11c930 and the settings menu); the window procedure `0x10eb40` (`DefWindowProcA`, `PostQuitMessage`, close handling with `GetExitCodeThread` of the game thread). GDI (296 sites: `CreatePen`, `Ellipse`, `LineTo`, `TextOutA`, ...) occurs only in the sd Layout debug-draw classes (0x446f0 `DirectionSound`, `PointSound`, `LineSound`, `PolyLineSound`, ...): developer visualisation, not reachable in play.
**DPI:** the embedded manifest declares Common-Controls 6, `asInvoker`, Windows 7-10 compatibility and **no `dpiAware`** element and the exe imports no `SetProcessDPIAware*`; it is DPI-unaware (bitmap-scaled by the OS). The swap chain is created at the client size (`DXGI_SWAP_CHAIN_DESC1` B8G8R8A8, 3 buffers, FLIP_DISCARD, flags 0x42; see graphics-spec/D3D12-SHIM.md): the port presents a fixed 16:9 logical surface (the engine's output aspect is 16:9; settings offer 1920x1080 and other 16:9 sizes) into a `CAMetalLayer` and handles notch/safe area in the shell, not in the engine.
**Fullscreen:** `IDXGISwapChain::SetFullscreenState([dev+0x416], NULL)` only (no exclusive-mode path of its own); window mode drives `ResizeBuffers(3,w,h,0x57,0x42)`.

## 5.4 Timing and vsync

`QueryPerformanceCounter` 73 sites / `QueryPerformanceFrequency` 48 sites (GX frame pacing 0x10e7c0, 0x114f10, 0x11a510, 0x1231e0, 0x1233d0; movie 0x13d790/0x13d8e0/0x13d970/0x13e350; sd). `timeGetTime` 4 sites (0x1040c0, 0x12ba80 (called by the frame tick 0x105e10), 0x449c80 and 0x44a3a0 (HID)), `timeBeginPeriod` 7 / `timeEndPeriod` 9 / `timeGetDevCaps` 5 / `timeSetEvent` 3 / `timeKillEvent` 5 (all in the sd timer helpers 0xfb040, 0x126940, 0x1287d0, 0x10c450). `Sleep` 48 sites (GX exit 0x11ad80, device suspend 0x1042e0, spin loops), `SleepEx` 7, `SwitchToThread` 2. Vsync: the settings classes `CVSyncMode` / `CFrameRate` write the config; the sync interval reaches `Present1(sync, flags)` (graphics-spec). The frame tick is `0x105e10` (`0x128450`, sd tick 0x12ba80, `SteamPlatformTick`); the task manager tick is `0x14fd60` (S3). The port needs a 1 ms-resolution timer thread for `timeSetEvent` (the sd MSEC handler) and display-link pacing for the main loop.

## 5.5 Thread pool / JobManager

`0x12fd00(jobKind)` (callers `calc_motion`, `exec`, `exec_zsort` in yasui-YS: 0x3beec0, 0x411b90, 0x411f30) sets `.data[0x8bafa4 + kind*4] = 1` and, on first use, creates **6 job threads** (`Axa::CThread::Create`, name "JobManager", thread proc `0x130030`, over static slots `0x8bafd8 + n*0x70` up to 0x8bb278; `Axa::JobManagerImpl::JobThread` class). The two GX threads (0x11d0a0), the TRANSFER/Stream/AesReCipher threads (02-AUDIO.md), `StatsMan` / `TrophyMan` (Steam), `Launcher Entry` (SettingMenu::LauncherEntry 0x4f0130) and a movie thread (0x13d260) complete the 12 creation sites (S3: `CreateThread` 2, `_beginthreadex` 4, `_beginthread` 6). Main-thread scheduling is the cooperative fiber task manager (`TASK_MANAGER`, G1). A pthread pool of 6 workers plus per-thread TLS blocks reproduces this.

## 5.6 Memory

- **No `VirtualAlloc`/`HeapCreate` imports.** Heap = UCRT `malloc` 116 sites, `calloc` 43, `realloc` 18, `free` 214, `_aligned_malloc` 50, `_aligned_free` 74, `_aligned_realloc` 2, `_recalloc` 1, plus `HeapAlloc`/`HeapFree` 5/5 (with `GetProcessHeap` 10; used by CRT-internal code 0x43bde8/0x43c324), `CoTaskMemAlloc/Free` 5/12, `operator new/delete` (0x439064/0x4390a0). 234 allocation sites; only 47 have a constant size, and only **three constants >= 64 KB: 2 x 0x1000000 (16 MB, `AxaSoundStream` 0x13ab75 and `_threadProc` 0x13c157) and one 0xfffff800 immediate at gummi 0x2123e7, which is almost certainly an unrelated register value picked up by the detector (it takes the last `mov ecx, imm` before the call), not a size**. Everything else is computed (file sizes via `LoadFileWithMalloc`, resource buffers, GX buffers).
- **Static arena:** the `.data` section has a 38,207,160-byte virtual size with only 564,224 bytes initialised: **37,642,936 bytes (35.9 MiB) of zero-initialised static data** at 0x713000; 8,307 distinct accessed locations are in it; the hottest (by access count) are 0x8cc418 (1,605), 0x2b81278 (786, the sd driver lock), 0xb0d4e0 (604), 0xbeecd8 (589). The loader must reserve this at the fixed image base (G1 open question: fixed 0x140000000 vs base-relative).
- `SQEX::CDev::Engine::Memory::SystemHeap` (0xf9030, 0xf9350) calls `GlobalMemoryStatus` to size the sd heap: return a fixed budget (not the device RAM).
- Large-object behaviour on iOS: resource buffers come in at file size (largest non-remastered files in the mod: `se/se999.win32.scd` 17.7 MB, `vagstream/Title.win32.scd` 10.1 MB, `obj/P_EX020*.a.us` 5.7 MB, `map/lm02.map` 5.7 MB; remastered DDS up to 1024x1024; vanilla files not measured); budget needs a runtime trace.

## 5.7 Process, crash and misc

`MiniDumpWriteDump` (1 site, 0x1435d0: writes `KINGDOM HEARTS II FINAL MIX.dmp` from the WinMain SEH filter), `SetUnhandledExceptionFilter` 3, `IsDebuggerPresent` 2, `OutputDebugStringA` 39 (via stubs; release logging), `ShellExecuteA` 2 (0x104620 launcher helper, 0x122370 URL/open), `CopyFileA` 4 / `DeleteFileA` 6 / `CreateDirectoryA` 1 / `RemoveDirectoryA` 1 / `FindFirstFileA` 9 / `FindNextFileA` 5 (save layer 0xff8b0..0x100810, dev loaders), registry `HKCU\\Software\\SQUARE\\KHProj` (0x148510 get, 0x1485d0 set; 5 sites, no direct callers), process listing for `WaitTitleProject.exe` (04-STEAM-FACTS.md 4.5), named file mapping for the exit code (0x143690), `CreateMutexA` 1 (0xfd9b0, `CMutex` helper; `ReleaseMutex` 37), `CoInitialize`/`CoUninitialize`.

## 5.8 Full import census (scoping the Win64 runtime v0)

Method: a capstone linear sweep of all 38,455 universe functions (1,300,082 instructions, data-in-code excluded) collecting `call [rip+d]` on IAT slots, `jmp [rip+d]`, address-taken IAT references, and rel32 calls/jumps to the 382 import-stub thunks. **368 imports in 35 DLLs, 7,785 call sites; 14 imports have no reference in code** (`_CorExeMain` is the PE entry thunk; `__C_specific_handler`, `__FrameUnwindFilter`, `_purecall` are reached only through unwind data/vtables; `CallNextHookEx`, `LoadIconA`, `RegisterDeviceNotificationA`, `SetDlgItemTextW`, `SetWindowsHookExA`, `UnhookWindowsHookEx`, `UnregisterClassA`, `SysAllocStringByteLen`, `SysStringByteLen`, `VariantCopy`). The earlier S3 census (`s3-eh-fibers/import_callsites.json`: 343 entries, 7,518 sites) undercounted 58 imports by 267 sites in total because it covered `.pdata` roots only (largest gaps: `Sleep` 16 -> 48, `OutputDebugStringA` 0 -> 39, `ReleaseMutex` 0 -> 37, `SetThreadPriority` 7 -> 13, `DeleteCriticalSection` 55 -> 79, `sqrtf` 61 -> 68); both agree on every import not listed in `IMPORT-CENSUS.md` with an S3 column that differs.

| DLL | imports | sites | treatment for runtime v0 |
|---|---|---|---|
| kernel32.dll | 94 | 3,175 | implement: sync (19 imports / 2,697 sites: CRITICAL_SECTION 2,500+, events, semaphores, mutex, CloseHandle), thread (16), time (5), fs (16), fibers (5), mem (6), eh (11), debug (4), loader (5), str (3), launcher (4, drop) |
| vcruntime140.dll | 21 | 1,337 | libc mem/str functions; EH imports -> terminate (76 `_CxxThrowException` sites, G1) |
| api-ms-win-crt-math | 33 | 951 | libm (UCRT sinf/cosf/atan2f 365 sites: bit-exactness caveat) |
| api-ms-win-crt-heap | 10 | 520 | libc allocator, `_aligned_*` -> `posix_memalign` |
| api-ms-win-crt-string | 17 | 358 | libc |
| gdi32.dll | 10 | 296 | drop (sd debug-draw only) |
| api-ms-win-crt-stdio | 26 | 271 | POSIX stdio with game-path mapping (`_sopen_dispatch` 19, `_read` 34, `_lseeki64` 53, `_close` 32, `_write` 7, `fopen` 13, `fwrite` 28) |
| api-ms-win-crt-convert | 5 | 202 | libc (`atof` 125, `atoi` 71) |
| api-ms-win-crt-runtime | 29 | 135 | loader owns startup/onexit tables; `_beginthread(ex)` 10 -> pthread; `abort`/`exit`/`terminate` |
| api-ms-win-crt-utility | 4 | 90 | libc (`bsearch` 53, `qsort` 16, `rand` 18, `srand` 3: rand sequence is implementation-defined) |
| msvcp140.dll | 11 | 69 | `_Mtx_*` -> recursive mutex, `_Thrd_sleep`, `_Xtime_get_ticks`, throw helpers -> terminate |
| user32.dll | 29 | 67 | native window shell (22 referenced imports) |
| hid.dll + setupapi.dll | 15 | 67 | drop (raw DualShock driver) |
| steam_api64.dll | 9 | 60 | facts only (04-STEAM-FACTS.md) |
| d3dcompiler_47.dll | 1 | 51 | `D3DStripShader` 51 sites: shader blob strip, identity (graphics-spec) |
| winmm.dll | 6 | 33 | timers (5.4) |
| ole32.dll + oleaut32.dll | 13 | 38 | stubs; CoCreateInstance sites 0x131ef0 (WMI pad check, dropped), 0x13d260 (movie), 0x953a0 (MMDevice) replaced |
| api-ms-win-crt-time | 6 | 19 | libc time |
| d3d12 + dxgi + d3d11 | 4 | 7 | D3D12-on-Metal shim; D3D11 only for movie decode |
| mfplat + mfreadwrite | 7 | 11 | AVFoundation movie path |
| xaudio2_9 + xinput1_4 + dinput8 | 6 | 7 | XAudio2 shim / GameController (02, 03) |
| advapi32 / shell32 / imagehlp / dbghelp | 8 | 11 | tiny: registry store, folder paths, `mkdir -p`, minidump no-op |
| other CRT (filesystem 2, locale 1) | 3 | 10 | POSIX |

Scale as counts: **368 import slots (354 referenced): 165 CRT/C++ runtime (api-ms-win-crt 133, vcruntime140 21, msvcp140 11), 94 kernel32, 109 other (device and OS APIs)**; the 20 imports with the most call sites cover 5,164 of the 7,785 sites (66%). 382 import-stub thunks exist in the universe (S2/universe): the loader fills the guest IAT once; no thunk needs per-site work. Full per-function lists with S3 deltas: `IMPORT-CENSUS.md` (586 lines); the X-macro `include/khnmc_imports.def` lists all 368 with category and site count.

## 5.9 Open items
- Internal layout of the `sqEX` payload (the PS2 memory-card image) and the 0xff400..0x101600 save functions.
- Whether the registry functions are live (no direct callers).
- The meaning of several `PCSettings` fields beyond volumes, resolution and key tables.
- Total static+heap working set: needs a runtime trace of the lifted build on the owner's machine (owner-gated).
