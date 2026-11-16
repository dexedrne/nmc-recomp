# 4. Steam seam: facts only

Scope: Batch B2, item 4. This document records **what the exe imports, where it calls it, what state each call sets or reads, and which game paths depend on it.** It does not propose, design or sketch any stub, emulation or bypass; the owner has not ruled on that question (G1 section 4 item 2). Static analysis of `kh2fm.exe` (sha256 `9002b2de6a1f91a7...`), RVAs relative to `0x140000000`. Machine-readable: `steam_surface.json` (all 60 sites), script `t4_steam.py`.

The exe has no packer. The Steam dependency is ordinary client code: 9 imports from `steam_api64.dll`, 60 import call sites inside 36 functions (the S7 label `steam` covers 65 functions around them; `axa-platform` code reaches Steam only through the wrappers of section 4.2), written against Steamworks interface versions `SteamUser023`, `SteamFriends017`, `SteamUtils010`, `STEAMREMOTESTORAGE_INTERFACE_VERSION016`, `STEAMUSERSTATS_INTERFACE_VERSION012`, `STEAMAPPS_INTERFACE_VERSION008`, `SteamInput006` (.rdata 0x585778, 0x5b0d18..0x5b0da8). The app id constant is `0x26f26e` = 2552430 (0x14d1d0 `GetGameAppId`, and the argument of `SteamAPI_RestartAppIfNecessary` in 0x14d610). `steam_appid.txt` is present in the Steam install directory (listing on build-host).

## 4.1 Imports (9) and call sites (60)

| import | IAT slot | sites | containing functions |
|---|---|---|---|
| SteamAPI_GetHSteamUser | 0x57bc80 | 6 | 0x14d450, 0x14d480, 0x14d4b0, 0x14d4e0, 0x14d510, 0xfe900 |
| SteamAPI_Init | 0x57bc50 | 1 | 0x14d610 |
| SteamAPI_RegisterCallback | 0x57bc68 | 6 | 0xfde20, 0xfe350 |
| SteamAPI_RestartAppIfNecessary | 0x57bc48 | 1 | 0x14d610 |
| SteamAPI_RunCallbacks | 0x57bc58 | 2 | 0x14d570, 0x14d5c0 |
| SteamAPI_Shutdown | 0x57bc88 | 1 | 0x14d0d0 |
| SteamAPI_UnregisterCallback | 0x57bc60 | 15 | 0xfdf00, 0xfdf20, 0xfdf40, 0xfe040, 0xfe090, 0xfe0e0, 0xfe130, 0xfe180, 0xfe1d0, 0xfe2a0, 0xfec30 |
| SteamInternal_ContextInit | 0x57bc78 | 21 | 0x14d0d0, 0x14d120, 0x14d1e0, 0x14d290, 0x14d380, 0x14d3b0, 0x14d3e0, 0x14d410, 0x14d570, 0x14d5c0, 0x14d610, 0xfe560, 0xfe720, 0xfe8b0, 0xfed70, 0xfede0 |
| SteamInternal_FindOrCreateUserInterface | 0x57bc70 | 7 | 0x14d450, 0x14d480, 0x14d4b0, 0x14d4e0, 0x14d510, 0x14d540, 0xfe900 |

All 60 sites (RVA, kind, containing function, S7 name if any):

| import | site | kind | function (size) | S7 name / class |
|---|---|---|---|---|
| SteamAPI_GetHSteamUser | 0xfe909 | call | 0xfe900 (39) |  |
| SteamAPI_GetHSteamUser | 0x14d459 | call | 0x14d450 (39) |  |
| SteamAPI_GetHSteamUser | 0x14d489 | call | 0x14d480 (39) |  |
| SteamAPI_GetHSteamUser | 0x14d4b9 | call | 0x14d4b0 (39) |  |
| SteamAPI_GetHSteamUser | 0x14d4e9 | call | 0x14d4e0 (39) |  |
| SteamAPI_GetHSteamUser | 0x14d519 | call | 0x14d510 (39) |  |
| SteamAPI_Init | 0x14d627 | call | 0x14d610 (197) |  |
| SteamAPI_RegisterCallback | 0xfde65 | call | 0xfde20 (216) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RegisterCallback | 0xfdea1 | call | 0xfde20 (216) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RegisterCallback | 0xfdedd | call | 0xfde20 (216) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RegisterCallback | 0xfe3b2 | call | 0xfe350 (235) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RegisterCallback | 0xfe3e1 | call | 0xfe350 (235) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RegisterCallback | 0xfe410 | call | 0xfe350 (235) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_RestartAppIfNecessary | 0x14d619 | call | 0x14d610 (197) |  |
| SteamAPI_RunCallbacks | 0x14d57d | call | 0x14d570 (65) | void __cdecl SteamPlatformTick(void) |
| SteamAPI_RunCallbacks | 0x14d5cd | call | 0x14d5c0 (65) |  |
| SteamAPI_Shutdown | 0x14d118 | jmp | 0x14d0d0 (79) |  |
| SteamAPI_UnregisterCallback | 0xfdf10 | jmp | 0xfdf00 (24) |  |
| SteamAPI_UnregisterCallback | 0xfdf30 | jmp | 0xfdf20 (24) |  |
| SteamAPI_UnregisterCallback | 0xfdf50 | jmp | 0xfdf40 (24) |  |
| SteamAPI_UnregisterCallback | 0xfe05f | call | 0xfe040 (70) | CCallback<class CSteamStatsAndAchievements, struct UserAchievementStor |
| SteamAPI_UnregisterCallback | 0xfe0af | call | 0xfe090 (70) | CCallback<class CSteamStatsAndAchievements, struct UserStatsReceived_t |
| SteamAPI_UnregisterCallback | 0xfe0ff | call | 0xfe0e0 (70) | CCallback<class CSteamStatsAndAchievements, struct UserStatsStored_t,  |
| SteamAPI_UnregisterCallback | 0xfe14f | call | 0xfe130 (70) | CCallbackImpl<16>::vfn3 |
| SteamAPI_UnregisterCallback | 0xfe19f | call | 0xfe180 (70) | CCallbackImpl<24>::vfn3 |
| SteamAPI_UnregisterCallback | 0xfe1ef | call | 0xfe1d0 (70) | CCallbackImpl<152>::vfn3 |
| SteamAPI_UnregisterCallback | 0xfe2c6 | call | 0xfe2a0 (126) | CCallbackImpl<152>::ctor_or_dtor |
| SteamAPI_UnregisterCallback | 0xfe2e0 | call | 0xfe2a0 (126) | CCallbackImpl<152>::ctor_or_dtor |
| SteamAPI_UnregisterCallback | 0xfe2fa | call | 0xfe2a0 (126) | CCallbackImpl<152>::ctor_or_dtor |
| SteamAPI_UnregisterCallback | 0xfecd5 | call | 0xfec30 (253) | CCallbackImpl<152>::~dtor |
| SteamAPI_UnregisterCallback | 0xfecef | call | 0xfec30 (253) | CCallbackImpl<152>::~dtor |
| SteamAPI_UnregisterCallback | 0xfed09 | call | 0xfec30 (253) | CCallbackImpl<152>::~dtor |
| SteamInternal_ContextInit | 0xfe56b | call | 0xfe560 (47) |  |
| SteamInternal_ContextInit | 0xfe763 | call | 0xfe720 (392) |  |
| SteamInternal_ContextInit | 0xfe8c0 | call | 0xfe8b0 (68) |  |
| SteamInternal_ContextInit | 0xfed8c | call | 0xfed70 (112) |  |
| SteamInternal_ContextInit | 0xfedfc | call | 0xfede0 (112) |  |
| SteamInternal_ContextInit | 0x14d0f9 | call | 0x14d0d0 (79) |  |
| SteamInternal_ContextInit | 0x14d18b | call | 0x14d120 (162) | void __cdecl Example_PresenceService_SetPresenceAsync(wchar_t const *) |
| SteamInternal_ContextInit | 0x14d267 | call | 0x14d1e0 (170) | class CSteamID __cdecl GetMySteamId(void) |
| SteamInternal_ContextInit | 0x14d2a7 | call | 0x14d290 (37) | class ISteamApps * __cdecl GetSteamApps(void) |
| SteamInternal_ContextInit | 0x14d397 | call | 0x14d380 (37) | class ISteamInput * __cdecl GetSteamInput(void) |
| SteamInternal_ContextInit | 0x14d3c7 | call | 0x14d3b0 (37) | class ISteamRemoteStorage * __cdecl GetSteamRemoteStorage(void) |
| SteamInternal_ContextInit | 0x14d3f7 | call | 0x14d3e0 (37) | class ISteamUserStats * __cdecl GetSteamUserStats(void) |
| SteamInternal_ContextInit | 0x14d41b | call | 0x14d410 (60) |  |
| SteamInternal_ContextInit | 0x14d42e | call | 0x14d410 (60) |  |
| SteamInternal_ContextInit | 0x14d596 | call | 0x14d570 (65) | void __cdecl SteamPlatformTick(void) |
| SteamInternal_ContextInit | 0x14d5e6 | call | 0x14d5c0 (65) |  |
| SteamInternal_ContextInit | 0x14d63c | call | 0x14d610 (197) |  |
| SteamInternal_ContextInit | 0x14d661 | call | 0x14d610 (197) |  |
| SteamInternal_ContextInit | 0x14d678 | call | 0x14d610 (197) |  |
| SteamInternal_ContextInit | 0x14d68f | call | 0x14d610 (197) |  |
| SteamInternal_ContextInit | 0x14d6a6 | call | 0x14d610 (197) |  |
| SteamInternal_FindOrCreateUserInterface | 0xfe918 | call | 0xfe900 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d468 | call | 0x14d450 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d498 | call | 0x14d480 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d4c8 | call | 0x14d4b0 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d4f8 | call | 0x14d4e0 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d528 | call | 0x14d510 (39) |  |
| SteamInternal_FindOrCreateUserInterface | 0x14d552 | call | 0x14d540 (33) |  |

`kind` is `call` for `call [rip+IAT]` and `jmp` for a tail jump through the IAT (4 sites: 0x14d118 the Shutdown tail call, and the `UnregisterCallback` tail jumps 0xfdf10, 0xfdf30, 0xfdf50); no Steam import is reached through the import-stub thunks.

## 4.2 Wrapper functions and the state they read or write

| function | name | what it does (read from disassembly) |
|---|---|---|
| 0x14d610 (197 B) | SteamPlatform::Init | SteamAPI_RestartAppIfNecessary(0x26f26e); if false: SteamAPI_Init; on success caches 5 interface pointers + SteamID64 and sets state 0x8cc420=1; returns bool. Called lazily (singleton AppInterface-steam object at 0x79cfa0 is created on first use) from 0x1044d0, 0x104a60, 0x105420, 0x105540, and from the WinMain shim 0x143720 after the main returns. |
| 0x14d570 (65 B) | SteamPlatformTick | if state==1: SteamAPI_RunCallbacks(); ISteamInput::RunFrame(true). Called from per-frame 0x105e10 (-> 0x105530 variant from sd timer 0x1287d0) |
| 0x14d5c0 (65 B) | SteamPlatformTick (duplicate body) | same body as 0x14d570; no direct callers |
| 0x14d0d0 (79 B) | SteamPlatform::Shutdown | if state==1: ISteamInput::Shutdown(); state:=2; tail-calls SteamAPI_Shutdown |
| 0x14d120 (162 B) | SetRichPresence wrapper (S7: Example_PresenceService_SetPresenceAsync) | WideCharToMultiByte(UTF8) then ISteamFriends017 vtbl+0x158 (SetRichPresence(key=.rdata 0x5b0db8, value)); caller MyApp::vfn1 0x14ed40 |
| 0x14d1d0 (6 B) | GetGameAppId | returns constant 0x26f26e (2552430) |
| 0x14d1e0 (170 B) | GetMySteamId | returns cached SteamID64 (0x8cc448) after universe/type validation; if invalid re-queries ISteamUser::GetSteamID (vtbl+0x10) |
| 0x14d290 (37 B) | GetSteamApps | ISteamApps008 pointer (cached at 0x8cc428 else ContextInit 0x7155b0) |
| 0x14d2c0 (192 B) | ISteamInput controller-type probe | if state==1 and ISteamInput present: GetConnectedControllers-style query (vtbl+0x130 on controller handle index 0x8cc424, then vtbl+0x128 InputType) maps to 1,2,3,5 (default 5); used for button-glyph set |
| 0x14d380 (37 B) | GetSteamInput | ISteamInput006 pointer |
| 0x14d3b0 (37 B) | GetSteamRemoteStorage | ISteamRemoteStorage016 pointer (0 call sites in game code) |
| 0x14d3e0 (37 B) | GetSteamUserStats | ISteamUserStats012 pointer |
| 0x14d410 (60 B) | ISteamUtils::IsSteamRunningOnSteamDeck wrapper | ContextInit 0x715568 (SteamUtils010); vtbl+0x110; used by Axa.AppInterface.steamworks_is_steamdeck 0x105620 |

Interface context objects (arguments of `SteamInternal_ContextInit`; first qword = init function that calls `SteamAPI_GetHSteamUser` + `SteamInternal_FindOrCreateUserInterface(version)`):

- `0x713360`: SteamUser023 (init fn 0xfe900)
- `0x7155b0`: STEAMAPPS_INTERFACE_VERSION008 (0x14d450)
- `0x715598`: STEAMUSERSTATS_INTERFACE_VERSION012 (0x14d510)
- `0x715580`: STEAMREMOTESTORAGE_INTERFACE_VERSION016 (0x14d4e0)
- `0x7155c8`: SteamInput006 (0x14d4b0)
- `0x715568`: SteamUtils010 (0x14d540)
- `0x715550`: SteamFriends017 (0x14d480)

Globals:

- `0x8cc420`: steam state dword: 0 = not initialised, 1 = initialised OK, 2 = shut down. Written by 0x14d610 (=1) and 0x14d0d0 (=2); tested by 0x14d570, 0x14d5c0, 0x14d0d0, 0x14d2c0, 0x1044d0, 0x104a60, 0x105420, 0x105540, 0x105620
- `0x8cc424`: dword used as controller-handle index by 0x14d2c0
- `0x8cc428`: ISteamApps*
- `0x8cc430`: ISteamRemoteStorage*
- `0x8cc438`: ISteamUserStats*
- `0x8cc440`: ISteamInput*
- `0x8cc448`: CSteamID (u64 SteamID64)
- `0x79cfa0`: pointer to the 1-byte lazily-created steam platform object (created by new in 0x1044d0/0x104a60/0x105420/0x105540; deleted in 0x105380)

The three callback classes are `CCallback<CSteamStatsAndAchievements, UserStatsReceived_t / UserStatsStored_t / UserAchievementStored_t>` (payload sizes 24, 16, 152 bytes: `CCallbackImpl<24/16/152>`), registered through `SteamAPI_RegisterCallback` (6 sites) and removed through `SteamAPI_UnregisterCallback` (15 sites); the handlers (0xfe5b0, 0xfe5e0, 0xfe620) compare the callback's game id against `GetGameAppId()` before acting.

## 4.3 Which game paths depend on the Steam state

- **Language selection** (0x1044d0 and 0x104a60): ISteamApps::GetCurrentGameLanguage() (vtbl+0x20 on ISteamApps008) -> strstr against "english","french","italian","german","spanish" (.rdata 0x585e70..0x585e90); result selects language-specific data folders (us/uk/fr/gr/it/sp, .a.xx suffixes) and AppInterface language field. If Steam is not initialised the function falls through to its non-Steam branch (language from AppInterface/default; not analysed further).
- **Main game gate (ManageRun)** (axaAppMain 0x14f1e0 -> 0x105420 (call site 0x14f4a7)): 0x105420 lazily runs 0x14d610 and calls the IL entry 0x579bf0 (ManageRun stub); the `state 0x8cc420==1` test sits on the object-creating path only (see 4.4, corrected). 0x105540 has the same prologue and returns true directly when the object already exists.
- **Per-user save directory and save images** (0x103e40, 0x101000, 0xfef80): GetMySteamId() (0x14d1e0) -> sprintf "\\%llu" appended to <Documents>\My Games\<product>\Steam; also "%llu%d" in 0x101000/0xfef80 (save image key). A SteamID64 is therefore part of every save path.
- **Achievements / stats** (CTrophyMan (0xfd710..0xfee50), CStatsMan (0xfed70, 0x14dc80), CSteamStatsAndAchievements callbacks (0xfde20.. 0xfe350)): ISteamUserStats012: RequestCurrentStats (vtbl+0x0), GetStat/SetStat int (0x8/0x18) and float (0x10/0x20) for stat names "Mare2" and "MareTimePlayed", SetAchievement (0x38) for names "ACH_%03d", StoreStats (0x50). Two worker threads named "StatsMan" and "TrophyMan" (CThread, creator 0xfdd00) poll with Sleep. Trophy unlock requests are enqueued by Axa::TrophyUnlock ctor 0xfee50 from game code (call sites 0x158f2d, 0x21058c, 0x2c8687, 0x31bc29). Callbacks: UserStatsReceived_t (cb 24 bytes), UserStatsStored_t (16), UserAchievementStored_t (152); each handler filters on GetGameAppId()==m_nGameID.
- **Rich presence** (0x14d120 (from MyApp::vfn1 0x14ed40)): ISteamFriends017 SetRichPresence
- **Controller glyph type / Steam Deck** (0x14d2c0 (callers 0x1055a0,0x1055d0,0x105640,0x105810), 0x14d410 (0x105620 steamworks_is_steamdeck)): ISteamInput006 controller type probe and ISteamUtils010 IsSteamRunningOnSteamDeck feed Axa.AppInterface; consumers are in settingmenu (0x39bf00, 0x4edd60) and the AppInterface.
- **Per-frame callbacks** (0x105e10 (frame tick) -> 0x14d570): SteamAPI_RunCallbacks + ISteamInput::RunFrame once per frame; also from 0x1287d0 timer helper via 0x105530.
- **Shutdown** (0x105380 (AppInterface dtor) -> 0x14d0d0): ISteamInput::Shutdown then SteamAPI_Shutdown
- **Exit-code handoff after main** (0x143720 WinMain shim): after the game main returns, 0x14d610 is invoked once more (bool) and if true 0x143690 writes the exit code (4 bytes) into a named file mapping (.rdata 0x5ae810, CreateFileMappingA/MapViewOfFile). Not Steam API, but gated on the Steam result.

## 4.4 The gate the owner asked about (fn 0x14d610, state 0x8cc420)

Facts only:
- `0x14d610` calls `SteamAPI_RestartAppIfNecessary(0x26f26e)`; a true result makes it return false at once (the process is expected to exit). If false it calls `SteamAPI_Init`; false -> return false. On success it initialises the five interface contexts, calls `ISteamUser::GetSteamID` (vtbl +0x10) into 0x8cc448, calls `ISteamInput::Init(true)` (vtbl +0x0 on SteamInput006), sets **0x8cc420 = 1**, returns true.
- The function is entered lazily from four places (0x1044d0, 0x104a60, 0x105420, 0x105540) when the 1-byte platform object at 0x79cfa0 does not exist yet, and once more from the WinMain shim 0x143720 after the game main returns.
- **The run gate**: in `axaAppMain` (0x14f1e0) the call at 0x14f4a7 goes to 0x105420, which (after the lazy init) calls the IL entry `0x579bf0` (the `ManageRun` stub; G1 and S4). On the object-creating path (platform object 0x79cfa0 still 0) it calls `0x579bf0` **only if 0x8cc420 == 1**, otherwise it returns and `axaAppMain` proceeds to its teardown (0x1064d0 FreeAllPackages, 0x101860 AppInterface dtor). **verification correction (B2 verification):** the state test in 0x105420 is reached only on the call that creates the platform object. 0x105420 first compares the pointer at 0x79cfa0 with 0 (0x105437); if the object already exists it jumps to 0x105471 and calls `0x579bf0` without reading 0x8cc420. The creating path ignores the return value of 0x14d610 and stores the new object pointer whether or not init succeeded (0x105461). In `axaAppMain` the calls to 0x104a60 (call site 0x14f25d) and 0x1044d0 (0x14f308), which have the same lazy-create prologue, sit at lower addresses than the gate call (0x14f4a7); nothing between them calls the deleter 0x105380 (its only callers are 0x101860 and 0x101930, teardown). So in the boot order as laid out in the code the object already exists when the gate runs and the `state == 1` comparison inside 0x105420 is not evaluated (reachability of 0x14f25d/0x14f308 not path-checked). The earlier text below (and G1 section 4) said `ManageRun` runs only if state == 1; that is the behaviour of the creating path only. Original wording kept in verification/originals/.
- A second consumer is the language detection: 0x1044d0 / 0x104a60 call `GetSteamApps()->GetCurrentGameLanguage()` (ISteamApps vtbl +0x20) when state == 1 and compare the returned string against "english", "french", "italian", "german", "spanish" (.rdata 0x585e70..0x585e90). When the state is not 1 these functions skip that block (their other branch was not decoded). verification note: like 0x105420 they test the state only when they create the platform object (0x79cfa0 == 0); if it already exists they enter the Steam branch directly, where a null `GetSteamApps()` result (cached pointer 0x8cc428 still 0) selects an empty-string constant (.rdata 0x6182e0 in 0x1044d0, via the `je` at 0x10452c) instead of querying the language, so no language string matches.
- Save-path construction needs a SteamID64: `GetMySteamId()` (0x14d1e0) returns the value cached at 0x8cc448, or re-queries `ISteamUser::GetSteamID` when the cached value fails the universe/type validation.

## 4.5 Other launcher/platform items that sit next to the Steam seam (facts)

- `WaitTitleProject.exe`: `0x1042e0` (callers 0x100fbe, 0x101098, 0x1010d8, 0x1011a5, 0x104e16, 0x104e56, 0x1999da, 0x2e9e00) and `0x1046e0` (callers in `SettingMenu::CSettingMenu` 0x4f94b0) enumerate processes with `CreateToolhelp32Snapshot`/`Process32First`/`Process32Next`, `strcmp` the image name to "WaitTitleProject.exe" (.rdata 0x585ec0) and `TerminateProcess` matches; `0x104620` builds `<exe dir>\WaitTitleProject.exe` and `ShellExecuteA(open)`s it. Around the process kill, `0x1042e0` pauses sound (`Axa.Sound.Sys.SystemPause` 0x13b2d0) and suspends/resumes the GX device (`suspendDevice` 0x127eb0, `resumeDevice` 0x125a10) while polling a window message (`SendMessageA`).
- Exit-code mailbox: 0x143690 (above).
- Platform id 0x79cfcc: values 0 STEAM, 1 PS4, 2 XB1, 3 GGP, 4 EPIC select directory names and gate `DecryptFile`; the exe contains the EPIC option (`-epicuserid`, .rdata 0x585e58, parsed by 0x103c90) as well.
- Registry: `HKCU\Software\SQUARE\KHProj` read (0x148510) and written (0x1485d0) through `RegQueryValueExA`/`RegSetValueExA`; no direct callers were found (virtual or dead).

## 4.6 What this means for the dependency graph (no recommendation)

Steam-dependent behaviour, in order of reach: (1) the run gate (everything; but see the 4.4 correction: the state test is only on the object-creating path of 0x105420), (2) language selection (data folders), (3) per-user save directory (SteamID64 in the path), (4) achievements and stats (two worker threads, 15 `ISteamUserStats` call sites), (5) glyph/controller-type queries and Steam Deck detection, (6) rich presence, (7) per-frame callbacks and `ISteamInput::RunFrame`. Items 4-7 do not influence game state; items 1-3 do. The owner decision in G1 section 4 item 2 is still open.

