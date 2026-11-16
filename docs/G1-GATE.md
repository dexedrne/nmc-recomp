# Gate G1 assessment: KH2 FM + NMC to iPhone, route B

## 1. G1 verdict

**Commit to route B.** No spike found a blocker. Several of the evidence bases are narrower than the headlines claimed (see "Fidelity limits"), so G1 is a conditional pass.

| Criterion | Result | Evidence and limits |
|---|---|---|
| A lifter can handle this code | **Pass, conditional** | VEX-IR-to-C (pyvex plus a custom emitter) lifted 17,895 of 17,908 roots (1,174,649 of 1,180,298 instructions); the C is 257,836,057 B. 13 failures: 8 on 256-bit AVX types, 1 on a V256 op, 2 on CPUID, 2 on `packuswb`. 1,500 sampled functions compiled cleanly with gcc. The same C compiles for arm64 with clang, on a 2.4 MB subset, not linked. 327 leaf functions (5,339 random-input tests, 64.4% instruction coverage) match Unicorn bit-for-bit. The 5 jump-table leaves all match. **Not shown:** any function with callees, tail or indirect dispatch, EH, the 9 AVX functions, any run on arm64, or the 55 untestable leaves. Remill was never run (the harness blocked executing the downloaded binary). By name, Remill master lacks 9 mnemonics used at about 341 sites: VSHUFPS, VPERM2F128, VEXTRACTF128, VBLENDPS, VPERMILPS, RSQRTPS, RCPPS, PMULLD (xmm form) and MOVMSKPD. |
| EH, fiber and TLS runtime is tractable | **Pass** | C++ EH is cleanup-only: 75 try blocks, all `catch(...)`, 74 of them rethrow-only. The game has no throws of its own (2 real throw sites, both CRT bad_alloc). So terminate-on-throw is sufficient for gameplay. SEH is 7 functions. Fibers are real stackful coroutines. The verification found 2 yield leaves with no .pdata: 0x150060 (278 call sites in 158 functions) and 0x150080 (14 sites in 10 functions). TLS is a 160-byte block (165 gs:[0x58] sites). setjmp/longjmp is libpng-only. |
| IL is off the boot path or stubbable | **Pass** | About 50 of 439 IL methods are live. They are WinForms window-host code, with no game logic. The native game runs on two threads started by `GXDevice.init`. No CLR is needed. The 9 TinyTexEntry texture helpers are probably dead (medium confidence). |
| Graphics API identified and Metal backend sized | **Pass** | D3D12 only: no D3D11 path, no selector, no fallback. D3D11 exists only for Media Foundation movie decode. There are about 60-65 D3D12/DXGI methods on about 15 interfaces, no compute or tessellation, and a fixed 4-CBV, 2-SRV, 1-sampler binding model. There are 47 SM4 shaders (26 VS, 21 PS), 3 BC formats plus BGRA8, and NV12 movies. GX (corrected extent 0x109000-0x12a000) spans 591 pdata entries, about 120 KB, 190 entry functions and 2,275 call sites. Not captured statically: runtime render-target sizes and formats. |
| Mod patches decoded | **Partial (pass for the plan, not complete)** | Byte patches and the main hooks are decoded. 126 differing exe regions (403 bytes): 35 Detours hooks plus 89 static edits, and a 114-entry script-syscall table. 18 of the 35 detours have undecoded behaviour. Whether NMC scripts use the 114 syscalls is unchecked. KH2Moose's write bytes were not recovered (not needed). The deployed DLL and ini are hash-identical to the repo, and no Lua is in use. |
| Gummi skip plan exists | **Pass, untested in play** | World gating is data in 00progress.bin (WLDF and LINK) plus world and course state bytes. The mod already retargets 8 unblock triggers. One structural gate is left (WLDF id 53/54, Traverse Town). Route nodes stay active unless removed. The world-map-to-gummi call site is not located. |
| Name DB coverage | **Fail on the M1 criterion; does not gate route B** | Only 447 of 17,908 roots (2.5%) have a real name. 4,252 (23.7%) have a name or class-role label. 14,225 (79.4%) have a subsystem label, but only 4,290 of those are seeded (the rest come from adjacency or call-graph votes). RTTI is fully recovered: 1,115 types, 1,066 vtables, 20,667 slots, 5,990 unique virtual functions, 866 logical classes with vtables. A lifter does not need names. |

**Unspiked:** the 'sd' audio and XAudio2 seam, input, file IO, the full import inventory, and the Steam seam. These are not in the G1 criteria, but the port needs them.

**Fidelity limits.** Leaf-level fidelity is demonstrated. Whole-program fidelity is not. All lift testing was x86-64 host to x86-64 C, so arm64 NaN and denormal behaviour is untested. The UCRT sinf/cosf/atan2f results (365 call sites) will not be bit-identical on Apple libm. Whether iOS allows a fixed 0x140000000 image mapping is unverified.

## 2. Architecture decisions now settled

- **Graphics.** D3D12 is the only render API. Because GX is a non-virtual singleton, it is not a clean polymorphic interface.
  - **Stage 1 (decision, medium confidence): D3D12-on-Metal shim.**
    - It implements about 65 methods on about 15 COM interfaces plus 3 imports (D3D12CreateDevice, D3D12SerializeRootSignature, CreateDXGIFactory1), under the lifted GX code. This keeps GX lifted unchanged and keeps the oracle intact.
    - Barriers are mostly no-ops. The 4 root CBVs plus the SRV and sampler tables map to one fixed Metal argument layout.
  - **Stage 2:** replace at the GXDevice boundary (190 entries, 24 classes, 120 vtable slots) with clean C++, using shim call logs as the spec.
  - **Common pieces for both stages:**
    - a PSO cache keyed on the 96-bit state key (the bit decode is only partly inferred and must be redone exactly);
    - 47 shaders translated from SM4 DXBC to MSL offline (the converter tool is not yet chosen);
    - MSAA with an explicit resolve;
    - an NV12 movie path via AVFoundation, replacing the D3D11 shared-handle path.
  - **Ruled out:** DXMT (D3D11-only). A D3D12-capable Wine stack was not evaluated and is not needed.
- **Lifter.** The VEX-to-C pipeline is the lifter of record for now, because it is the only one measured. Remill stays a candidate pending owner permission to run it. Unicorn is used only as a dev oracle, never shipped. GPL licences (libvex, Unicorn) need a check only if anything is distributed. The lift input is the full 30,246-function universe, not the 17,908 .pdata roots. There are 12,338 leaf functions without .pdata, including 3,413 virtual targets.
- **EH.** Terminate-on-throw. Throw thunks and the CRT bad_alloc, _Xlength_error and similar helpers log the site and abort. `RaiseException(0x406D1388)` (SetThreadName) becomes a no-op. The crash filter, Rtl* and MiniDump helpers are stubbed. libpng's 2 setjmp and 3 longjmp sites use the host setjmp/longjmp. A cleanup-then-terminate mode over 2,161 funclets is a later option only.
- **Fibers.**
  - Real stackful coroutines with an arm64 asm context switch (ucontext on iOS is unverified).
  - Each fiber gets a host stack plus a guest stack (default reserve 1 MB, largest static frame 0x40078).
  - The runtime implements CreateFiber, SwitchToFiber, Convert* and DeleteFiber, the main-fiber sentinel 0x43214321, and a Convert/unconvert on every task activation.
  - Yielding is `switch(current, sentinel)`.
  - The earlier idea of turning fibers into plain calls is rejected.
- **TLS.** A 160-byte per-thread block seeded from the template (dword 0x80000000 at +0x10). gs:[0x58] becomes a per-thread base. Thread-safe statics must lift correctly.
- **Boot and IL.**
  - A native shell replaces ManageRun, BaseSimpleForm, ExitApplication, EndMessageClose and MouseEnable (about 51 methods).
  - The loader runs the init tables itself: 3 `__xi` and 4,653 `__xc` constructors, then WinMain 0x143720, then the app main 0x14f1e0.
  - The shell calls DXInput.init and `GXDevice.init(view, app, 0)`, starts the two game threads, handles suspend/resume, and calls ExitApplication when the first thread's loop ends.
  - The ~245 CRT/ATL IL methods and the ~118 dev-tool methods are dropped. The TinyTexEntry helpers are stubbed.
- **Mod patches (decision; the lifting part is an inference to be proven).**
  - Static edits that touch .text must be applied to the image before lifting, because lifted C does not see runtime byte edits. Data-section edits are applied at load.
  - The 35 detours become function-table replacements. The trampoline becomes a call to the lifted original. Replacement handlers come from lifting the DLL's own handler functions (this covers the 18 undecoded ones exactly), then are rewritten as clean C++ and checked against the lifted original.
  - The Detours, ini and logging scaffolding is replaced by a hand-written config layer.
  - Skip KH2Moose, retry, savepoint and the debug options.
- **Gummi skip.**
  - Data: WLDF entry id 53 cmd 5 to 14 (file offset 36636), so Traverse Town is not re-blocked. Optionally set cmd 9 to 10 at 36652 (cosmetic only).
  - Course state 2 does NOT disable a route node (exe 0x397490 treats states 1 and 2 alike). To stop nodes being flown into, remove the `m_51` Spawn from the 15 wm00.ard "map" programs (program 25 already omits it), or make WM_GUMI nodes inert host-side. A host-side override of getter 0x3f1e20 (byte 3 = 0) also works, as in the Lua and the mod's `patch_gummiskip`.
  - Leave 00worldpoint, 07localset and the 50* files unchanged. Keep gumimenu/gumibattle stubs so references resolve.

## 3. Ordered next work

**Corrections every later workflow must carry:**
- Use merged function extents. The name DB's size and end cover only the first .pdata fragment, and 3,315 roots have fragments.
- The roots include at least 182 unwind funclets.
- The mod address base (calc_damage 0x3c12f0) is correct.
- Use GX extent 0x109000-0x12a000.
- Use the 15-program wm00.ard count.
- Course=1 writers in the mod are ids 2, 7, 8, 14, 22, 30, 53, 92 and 202.

**Sequential spine**
1. **Canonical function universe (first).** Merge .pdata fragments via UNWIND_INFO, add the 12,338 leaf functions and vtable targets, tag EH funclets, and output precise extents. Inputs: S2 `bounds.py` and `functions_table.tsv`, S7 `s7_universe.py`. Everything below depends on it.
2. **Headless boot of the lifted vanilla program to the first GX call** on x86-64 Linux (needs batch A: lifter and runtime). Then a differential check with call-sequence comparison.
3. **arm64 cross-build and run** (owner-gated Mac). Settle the NaN, min/max, libm and fixed-mapping policy. Decide between fixed 0x140000000 and base-relative addressing.
4. **D3D12-on-Metal shim and Metal backend**, then the movie path.
5. **iOS shell, sd audio (CoreAudio), input, Steam seam, package file IO.**
6. **Mod layer integration:** lift the patched image and the DLL handlers, then run lifted vanilla against lifted patched as a differential.
7. **Gummi data edits plus node removal**, then a playthrough test (owner-gated).
8. **Gradual replacement of lifted code** with clean C++ (GX boundary first, then sd, filemgr and Axa), with continued name DB growth.


Batch A, after step 1:
- **A1 Lifter hardening.**
  - Add V256, `packuswb`, CPUID/xgetbv and rcpps/rsqrtps handling to the VEX emitter.
  - Lift all 30,246 functions.
  - Extend the differential harness to functions with callees.
  - If the owner permits, run Remill on the same 382 leaves and compare.
  - Files: `scratch/S2-lifter/vex2c.py` and `diff.py`.
- **A2 Win64 runtime v0 plus full import census.**
  - Loader and init-table runner, TLS block, fiber switch, thread and sync mapping (pthreads, recursive mutexes, 6 JobManager slots), EH-terminate thunks, libpng setjmp, and import stubs for every kh2fm import.
  - The import census, beyond graphics and EH, has not been done.
  - Files: `scratch/s3-eh-fibers/*`.
- **A3 Graphics spec.**
  - Exact PSO-key decode (`fn 0x117710`, producers of the key, callers of 0x113590).
  - Per-shader PSFLAGS, alphaRef and RegionU/V semantics for the 47 DXBC shaders, and a DXBC-to-MSL converter evaluation.
  - A D3D12 shim interface spec (the ~65 methods from the corrected table).
  - Confirmation from OpenKH data of which DDS formats actually ship (BC1-3, A8R8G8B8, A4R4G4B4, A1R5G5B5, R8G8B8, R5G6B5).
  - The movie path spec (0x13da10, 0x13dd40, 0x13e7f0, 0x122890).
  - Files: `scratch/S1-graphics-api/*`, `scratch/S1-verification/*`.

Batch B, as slots free:
- **B1 Mod and gummi data.**
  - Script the 89 static edits into a patched PE image.
  - Decode the 18 undecoded detours by diffing each handler against the exe original.
  - Parse NMC ard/msn bytecode for use of the 114 syscalls.
  - Examine the 9 changed world-script subfiles in 00progress.bin.
  - Locate the world-map-to-gummi call site (candidates 0x151a50 and 0x151980, no direct callers).
  - Produce the edited 00progress.bin and wm00.ard in scratch.
  - Files: `scratch/S5-mod-hooks/patches.json`, `scratch/S6-gummi-data/*`.
- **B2 Platform seam spikes** (none exist yet): sd/XAudio2 call surface sizing, XInput/DInput/GamePad, file IO (`Axa.fileIoOpen`, package format) and the Steam call surface. The Steam findings go factually into the shared notes. Hold the Steam shim design until the owner has answered item 2 below.
- **B3 Name DB growth.** Import `name_db.csv` into Ghidra (the script is untested). Name virtual families (1,725 or 1,742), starting with the GX, sd, filemgr, input and Axa seam classes and the 291 classes matched to PS2. Decode the TASK_MANAGER task layout. Rebuild the DB with merged extents.

## 4. Owner decisions and supplies

1. **Remill.** Permission to run the downloaded Remill binary, or approve a source build against LLVM 19/21. Otherwise the VEX path stays the lifter of record.
2. **Steam gate (needs an explicit decision).** `SteamAPI_RestartAppIfNecessary` then `SteamAPI_Init` (fn 0x14d610 sets state at RVA 0x8cc420). `ManageRun`, and so the whole game, runs only if that state is 1. The plan's "Steam stubs" would have to satisfy this check. The exe has no packer, so this is the only protection-like item found. Reported factually. The Steam seam work is held until the owner rules on whether stubbing it is acceptable under the no-circumvention rule.
3. **Running the game or the lifted program.** The rules forbid running the game. Headless runs of the lifted program, runtime D3D12 captures (render-target sizes and formats, draw counts) and golden-frame oracles from the original on build-host all need owner OK.
4. **Mac and device.** arm64 validation needs the Mac (windowless jobs only, no Terminal windows), the iPhone model and iOS version (memory limits, entitlements), and the team and bundle details for personal signing. Also say which machine compiles the roughly 257 MB of lifted C (build-host has 13 GB RAM and about 15 GB free).
5. **Mod behaviour.** Replicate as shipped or as intended? As shipped, the three AntiForm options are inactive (inline `#` comments read false, so 9 patches are not applied), `worldpoint_amount=on` is a no-op, and the `begginer_*` HP/MP keys are ignored. Proud HP/MP at 0.5x and Proud AP at 1.5x are real changes. Confirm that skipping KH2Moose is acceptable.
6. **Gummi.** Should route nodes be removed or inert? Which gummi rewards (blocks, blueprints, Final Mix extras) should be granted outright, since they are lost on skip? Confirm whether any story beat or tutorial inside gummi must be preserved (unverified; the PS2 decomp has lastbattle_event.cpp and goal_bgm.cpp).
7. **Input.** Controller (MFi) or touch controls.
8. **Build pin.** Confirm the exe stays at SHA-256 9002b2de... (identical to Steam's `KINGDOM HEARTS II FINAL MIX.exe` on build-host). The mod signatures are fragile against any other build.

**Low-confidence items to re-verify:** the VTABLE/leaf extent heuristics, the "mostly Gummi" attribution of GIF-tag builders (at least 11 functions, some unidentified and possibly outside gummi), and the 5.5-7.1K fiber-reachable root estimate. The Remill gaps are name-level only. The family count (1,725 vs 1,742) and the 7-function difference to the plan's 5,983 virtual targets are unresolved.

Key artifact directories under `/home/user/projects/kh2-nmc-analysis/scratch/`: `S1-graphics-api`, `S1-verification`, `S2-lifter`, `S2-lifter-verification`, `s3-eh-fibers`, `s3-verification`, `S4-il-boot-path`, `S4-verification`, `S5-mod-hooks`, `S6-gummi-data`, `S7-name-db`, `S7-verification`.
