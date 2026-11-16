// KHNMC DECOMP EDITION - Win64 runtime v0 scoping sketch (not compiled). Scale is expressed as counts only.
// Evidence: ../05-OTHER-SEAMS.md, ../IMPORT-CENSUS.md, ../import_census.json, ../platform_seams.json
#pragma once
#include <cstdint>

namespace khnmc::rt {

// One entry per import slot (368). The loader fills the guest IAT (.rdata 0x57b000..) with thunks resolved by (dll,name).
struct ImportThunk { const char* dll; const char* name; void* guestEntry; };

// ---- handle objects the guest sees as HANDLE (CloseHandle: 65 sites) ----
enum class HandleKind : uint8_t { Event, Semaphore, Mutex, Thread, Process, FileMapping, FindHandle, Heap };

// ---- threads: 12 creator sites; named CThreads: StatsMan, TrophyMan, JobManager x6 (static slots 0x8bafd8 + n*0x70, proc 0x130030),
// TRANSFER x2, Stream x2, AesReCipher, Launcher Entry, CDevSd MSECHandler/VSYNCHandler, StreamReadThread, GX x2 (0x11d0a0), movie mp4 (0x13d260),
// 3 anonymous _beginthread users (0x44bbd0/0x44ea80/0x4501d0, raw HID: dropped).
// Per-thread TLS block: 160 bytes seeded from the PE TLS template (G1): gs:[0x58] sites 162.
struct GuestThreadBlock { uint8_t tls[160]; uint64_t fiberSentinel; };

// ---- time ----
// QueryPerformanceCounter 73 sites / QueryPerformanceFrequency 48 / timeGetTime 4 / timeSetEvent 3 (sd MSEC/VSYNC timers via 0xfb040, 0x126940, 0x1287d0).
uint64_t qpc();        // mach_absolute_time scaled to a 10 MHz or 1 GHz QueryPerformanceFrequency
uint32_t timeGetTime();
uint32_t timeSetEvent(uint32_t delayMs, uint32_t resolutionMs, void* guestCallback, uint64_t user, uint32_t flags);  // callback on a dedicated timer thread
void     timeKillEvent(uint32_t id);

// ---- memory ----
// No VirtualAlloc/HeapCreate imports. CRT heap only: malloc 116, calloc 43, realloc 18, free 214, _aligned_malloc 50, _aligned_free 74, HeapAlloc 5 (GetProcessHeap 10).
// Constant-size big allocations seen: 2 x 0x1000000 (16 MB) in AxaSoundStream (0x13ab75, 0x13c157). .data = 37.6 MB zero-initialised static (BSS) at 0x713000.
// GlobalMemoryStatus (2 sites, SQEX::CDev::Engine::Memory::SystemHeap 0xf9030/0xf9350) sizes the sd SystemHeap: return a fixed budget.
struct MemoryBudget { uint64_t totalPhys; uint64_t availPhys; };

// ---- window / display (user32 67 sites, all in GX 0x10be40..0x124970 + settings pointer 0x105810) -> native shell, see ../05-OTHER-SEAMS.md section 6
// ---- saves: <Documents>/My Games/<product>/Steam/<SteamID64>/ -> app container; see section 1 of ../05-OTHER-SEAMS.md
}  // namespace khnmc::rt
