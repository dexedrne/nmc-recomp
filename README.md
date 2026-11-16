# KHNMC: DECOMP EDITION

Kingdom Hearts II Final Mix (PC / Steam build) with the Nobody May Cry mod (Roxas replaces Sora,
Devil-May-Cry-style combat) integrated, rebuilt as a source-level port for iPhone. Personal use only.

**This repository holds tools, runtime code and documents only.** No game executable, disc image, archive,
asset, lifted/generated code or mod data is ever committed (see `.gitignore`). You supply your own
legally owned copy of the game; derived artifacts are built locally on your own machine.

## Approach (route B, see docs/G1-GATE.md and docs/PC-PORT-PLAN.md)
1. Canonical function catalogue of the x64 executable (merged unwind fragments, leaf functions, vtable targets).
2. Lift x64 to C/LLVM IR, compile for x86-64 Linux first (differential oracle), then arm64.
3. Hand-written runtime: Win64 loader/TLS/fibers/EH, D3D12-on-Metal shim, sd audio on CoreAudio,
   Media Foundation movies on AVFoundation, input, package file IO.
4. NMC integration: the mod's 89 static edits applied before lifting, its 35 detours as function replacements,
   its data (vanilla KH2 formats) loaded as-is.
5. Gummi ship removed entirely (world map warps directly; see docs/G1-GATE.md section 2).
6. Replace lifted code with clean C++ progressively (GX boundary first, then sd, file manager, platform layer),
   growing a name/type database from RTTI, strings and the PS2 decomp (GovanifY/kh2).

## Layout
- `docs/`     plans, gate assessment, data/format reports
- `tools/`    analysis and data tools (code only)
- `runtime/`  host runtime (to be written)
- `config/`   build/config templates

## Rules
- No DRM or copy-protection circumvention: the Steam startup check is not bypassed by this project.
- No time estimates in plans or reports.
- Mac builds only through the owner's rules (windowless jobs, shared lock, never the Duo).
