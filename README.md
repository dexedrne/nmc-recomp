
<h1 align="center">KHNMC: DECOMP EDITION</h1>

<p align="center"><b>Kingdom Hearts II Final Mix + Nobody May Cry, rebuilt from the executable up.</b><br>
Native on Linux, macOS and iPhone. No emulator, no Windows, no game files in this repository.</p>

---

The PC game's executable is statically lifted to C, function by function (38,242 of them), and runs on a hand-written runtime that
stands in for Windows. The **Nobody May Cry** mod (Roxas in Sora's place, Devil-May-Cry-style combat, four difficulties up to "Less
Than Nothing") is ported into it: its hooks, its patches and its script calls, behaving exactly as the PC mod, with the technical
side improved (memory, stability, size). The target is a personal iPhone build.

## Where it stands

| | |
|---|---|
| **iPhone** | The game runs on the device: the whole self-check (169,479 checks, 0 failures), the opening of the story played by a scripted controller (both opening maps, their missions, into Twilight Town), with sound. 42 GB of game data on the phone, verified file by file. **Next: the picture** (vkd3d is built for iOS; the presenter is in progress) and the controller. |
| **Linux x86-64** | Boots and plays headless; a real picture through vkd3d on a CPU Vulkan driver; a software XAudio2 mixer. Self-check: 169,980 checks, 0 failures. |
| **macOS, Apple Silicon** | Renders on the GPU through MoltenVK and Metal at the game's own 60 fps cap. The story route plays there on the GPU (4.3 million draw calls in 9,300 frames); self-check with the GPU: 170,166 checks, 0 failures. |
| **The mod** | All three of its DLLs ported: 40 function hooks, 87 static edits, 114 added script calls, the four difficulties. |
| **Automatic play** | State-driven route scripts (wait for a game state, press, walk, fight, talk) and a debug warp into any of the 311 rooms. |

## How it works

1. **Lift.** `tools/khlift` turns every function of the executable into C with one calling convention, `void f_<rva>(KCtx *)`.
   The output is generated locally from your own copy and never committed.
2. **Runtime.** `runtime/win64/` is everything the game calls: loader and memory layout, threads and fibers, files over a loose data
   tree, COM, Direct3D 12 (to vkd3d, then Vulkan or MoltenVK/Metal), XAudio2 (to a software mixer), input, timing, Steamworks.
3. **The mod.** Run-time patches cannot touch a static lift, so each one is ported: hooked functions are replaced at link time,
   edited functions are re-lifted from the patched image, and the mod's script calls are implemented in the runtime.
4. **Checks.** A self-check suite inside the binary, play routes that follow the game's own state, and checks for each merged change.

## Layout

| Path | What |
|---|---|
| `runtime/win64/` | the runtime, input examples and the iOS app shell (`ios/`) |
| `tools/khlift/` | the lifter |
| `tools/runtime/` | build, route, data and measurement tools; `ios/`: the Xcode projects and the Mac / iPhone scripts |
| `tools/mod/`, `tools/apple/`, `tools/publish/` | mod and data analysis; vkd3d for Apple; the scrubbed export |
| `docs/` | plans (`APPLE-PLAN.md`, `IOS-GRAPHICS-PLAN.md`), the mod's mechanics, graphics notes |
| `docs/FOR-COLLABORATORS.md` | where to start if you work on KH2 with other tools (OpenKH, Unity) |

## Building

First run `python3 tools/publish/prepare_sources.py OUTPUT --exe FILE --mod DIR --data DIR` in the checkout. Use the resulting private sources to build. The data directory is the baked English PC tree.

A work tree needs, next to these sources, the lifted C and the game's data tree, both made locally from your own copy of the game
and the mod. On Linux:

```sh
make -j8                                         # identity address mode
make XLATE=2                                     # bias mode: the Apple address model, on Linux
KH_EXE=$PWD/pc/kh2fm.exe ./khnmc --selftest
tools/route_batch.sh -n 4 -j 2 -d cycle -g 7000 -s 600 -o <dir>
```

The iPhone build runs on a Mac with Xcode (`tools/runtime/ios/`, see `docs/APPLE-PLAN.md`).

## Rules

- Nothing from the game or the mod is committed: no executable, data, lifted code, frames or audio.
- The mod behaves as shipped, bugs included; improvements are technical, and switchable.
- What leaves the working machine is the output of `tools/publish/export.sh`, never the working repository itself.
- Personal use only.

