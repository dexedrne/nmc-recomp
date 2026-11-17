# KHNMC: DECOMP EDITION

Kingdom Hearts II Final Mix (the PC build) with the Nobody May Cry mod (Roxas replaces Sora, Devil-May-Cry-style
combat), rebuilt as a source-level port. The executable is statically lifted to C and runs on a hand-written runtime that
stands in for Windows. The goal is a personal iPhone build. Personal use only.

**This repository holds tools, runtime code and documents only.** No game executable, data, asset, lifted or generated
code, frame or audio is ever committed (see `.gitignore`). You supply your own legally owned copy of the game and the mod;
everything derived from them is built locally on your own machine.

## Status (2026-11-17)

| Area | State |
|---|---|
| Lift | 38,242 functions of the x64 executable as C (117 files, 173 MB, generated locally). |
| Linux x86-64 | Boots and plays headless. Real picture through vkd3d on a CPU Vulkan driver, real sound from a software XAudio2 mixer. Self-check: 169,324 checks, 0 failures. |
| macOS on Apple Silicon | The same build in bias address mode. Renders on the GPU through MoltenVK and Metal at the game's own 60 fps cap. Self-check with the GPU renderer: 169,517 checks, 0 failures. |
| The mod | All three of the mod's DLLs are ported into the runtime: function hooks, static edits (re-lifted from the patched image), script system calls, the four difficulties including "Less Than Nothing". |
| Automatic play | A scripted controller follows game state, not frame counts. The longest route starts a new game, plays the two opening maps with their missions and continues into Twilight Town. |
| Linux and macOS agree | In the frame-locked test mode the two hosts write the same sound file, sample for sample. |
| iOS | Not compiled yet. Sandbox paths, an app shell, the audio output glue and the build scripts are written and have never run. |

Out of scope: pre-rendered cutscene movies. The gummi ship is removed (the world map warps directly).

## How it works

1. **Lift.** `tools/khlift` turns every function of the executable into C with the guest ABI `void f_<rva>(KCtx *)`. The
   output is generated locally and never committed.
2. **Runtime.** `runtime/win64` is everything the lifted code calls: loader, memory layout, threads, fibers and
   synchronisation, files over a loose data tree, COM, Direct3D 12, XAudio2, input, timing.
   - Graphics: a recorder backend for headless tests, and real backends that forward to vkd3d (Vulkan; MoltenVK on Apple).
   - Sound: a software mixer that writes a file or feeds a pull interface for a device callback.
   - Two address modes: identity (guest addresses are host addresses, Linux) and bias (host = guest + offset, needed on
     Apple systems, where the shared cache occupies the game's address range).
3. **The mod's code.** The mod ships DLLs that patch the running game. A static lift cannot be patched at run time, so
   each patch is ported: hooked functions are replaced at link time, edited functions are re-lifted from the patched
   image, and the mod's script system calls are implemented in the runtime.
4. **Checks.** A self-check suite inside the binary (`khnmc --selftest`), state-driven play routes
   (`runtime/win64/input/`, language reference in its `README.md`), and independent reviews of merged work

Platform services: the target has no Steam client, so the runtime answers the game's Steamworks calls itself, in one file
(`runtime/win64/platform_seam_store.c`). The executable is mapped as shipped and is never modified or redistributed.

## Layout

- `runtime/win64/` the runtime sources, the per-job reports (`STATUS-*.md`, `TASK*.md`) and the play routes (`input/`)
- `runtime/win64/ios/` app shell and audio glue for iOS (unrun)
- `tools/khlift/` the lifter and its runtime headers
- `tools/runtime/` build, route and measurement tools; `tools/runtime/ios/` Xcode projects and scripts (unrun)
- `tools/mod/`, `tools/gummi/`, `tools/platform/`, `tools/namedb/`, `tools/universe/` analysis and data tools
- `tools/apple/` building vkd3d against MoltenVK
- `tools/publish/` the scrubbed export of this repository
- `docs/` plans, format and graphics notes, the lift policy, review records
- `docs/FOR-COLLABORATORS.md` where to start if you work on KH2 with other tools (OpenKH, Unity), and NMC's combat in short

## Building

First run `python3 tools/publish/prepare_sources.py OUTPUT --exe FILE --mod DIR --data DIR` in the checkout. Use the resulting private sources to build. The data directory is the baked English PC tree.

The repository alone does not build a game. A work tree needs, next to these sources, the lifted C and the game's data
tree, both generated locally from your own copy. With those in place, on Linux:

```sh
make -j8                                        # identity mode
make XLATE=2                                    # bias mode, the Apple address model on Linux
KH_EXE=$PWD/pc/kh2fm.exe ./khnmc --selftest
tools/route_batch.sh -n 4 -j 2 -d cycle -g 7000 -s 600 -o <dir>
```

(In a work tree the scripts of `tools/runtime/` sit directly in `tools/`.)


## Rules

- Nothing derived from the game or the mod is committed: no executable, data, lifted or generated code, frames or audio.
- The mod's behaviour is kept as shipped, bugs included.
- No time estimates in plans or reports.
- What leaves the machine is the output of `tools/publish/export.sh`, never the working repository itself.
