
<h1 align="center">KH2 iOS Port (mod friendly)</h1>

<p align="center"><b>Kingdom Hearts II Final Mix, statically recompiled from the PC executable, and mod friendly.</b><br>
Native on iPhone, macOS and Linux. No emulator, no Windows, no game files in this repository.</p>

---

The PC game's executable is statically lifted to C, function by function (38,242 of them), and runs on a hand-written runtime that
stands in for Windows. **Roxas May Cry** (Roxas in Sora's place, Devil-May-Cry-style combat, four difficulties up to "Less Than
Nothing") is ported into it: the mod's hooks, its patches and its script calls, behaving exactly as the PC mod, with the technical
side improved (memory, stability, size). **Xion Knows Best** and **Axel Won't Cry** come with it as playable looks and title screens. The target is a personal iPhone build, and it plays there.

This is a **static recompilation**, in the family of the "Recompiled" ports: the C is machine-generated from the binary, not a
hand-written, byte-matching decompilation. The project was called *KHNMC: Decomp Edition* until 2026-11-18; KHNMC stays as the code
name (the switches are `KHNMC_*`).

## Where it stands

| | |
|---|---|
| **iPhone** | **Playable** with a controller (a PS5 pad), at 60 fps on the display link, with sound. Pictures are bit-identical to the Mac's GPU. Landscape and portrait: the 3D fills any screen shape, the HUD sits at the edges and is enlarged in portrait, the camera zooms in on the tall screen, the title is re-laid for portrait, and holding the phone upside down turns the picture. Menus and the pause screen have no black bars; the movies the data lacks end by themselves; an on-disk shader cache cuts pipeline creation on the title from 10.1 ms to 2.4 ms. Self-check on the device: 174,840 checks, 0 failures. |
| **macOS, Apple Silicon** | Renders on the GPU through MoltenVK and Metal at the game's own 60 fps cap; the story route plays there on the GPU. Guest memory accesses are ordered (acquire / release) on Apple CPUs, where the plain build was shown to reorder. |
| **Linux x86-64** | Boots and plays headless, with a real picture through vkd3d on a CPU Vulkan driver and a software XAudio2 mixer. Self-check: 172,956 checks, 0 failures. |
| **The mod** | All three of its DLLs ported: 40 function hooks, 87 static edits, 114 added script calls, the four difficulties. Mapped along the way: the 83 enemy AI files that act differently on Less Than Nothing ([docs/difficulty/LTN-ENEMY-AI.md](docs/difficulty/LTN-ENEMY-AI.md)). |
| **Looks** | Pick who you play as: Roxas, Xion, Xion (hooded), Axel or Roxas HD, each from an optional look pack you make from your own files (a missing pack is never offered). Axel is drawn at his own size in the field and in the pause menu. |
| **Title** | Three title screens, Roxas May Cry, Xion Knows Best and Axel Won't Cry, each with its own picture, logo and boot-intro sketches. **R3** on the title cycles them and the choice is saved. The title menu has a fourth row, **CHEATS**, that opens the Roxas May Cry menu (character, cheats, controller, display, gameplay). |
| **Automatic play** | State-driven route scripts (wait for a game state, press, walk, fight, talk) and a debug warp into any of the 311 rooms. |

## Options

Everything below is presentation or an opt-in extra; with a switch unset the game is the PC mod, byte for byte.

| Switch | What | State |
|---|---|---|
| `KHNMC_ASPECT=fill` | Use the whole screen of any shape (foldable, portrait) instead of 16:9 with bars | done |
| `KHNMC_FOV_ZOOM`, `KHNMC_HUD_SCALE_CMD` / `_HP`, `KHNMC_TITLE_LAYOUT` | Portrait tuning: camera zoom, command menu and HP gauge size, title layout | done |
| `KHNMC_CUTSCENES=story` | The story scenes the mod removes, restored from your own data where they are provably presentation-only | in review |
| `KHNMC_SCENE_ROXAS=1` | Roxas instead of Sora in story scenes | in progress |
| `KHNMC_PLAYER_LOOK` | Roxas, Xion, Xion (hooded), Axel or Roxas HD (needs the look pack) | done |
| `KHNMC_TITLE_SCREEN` | Roxas May Cry, Xion Knows Best or Axel Won't Cry title screen (R3 on the title cycles it) | done |
| `KHNMC_PS2_FORM_REWARDS` | The PS2 version's growth rewards for Bladeless (Wisdom) and Light & Dark (Limit); on when the Roxas May Cry package is installed | done |
| Xion Knows Best ([docs/xion/](docs/xion/)) | Swap to playable Xion at a save point, on any difficulty; and Xion's look on Roxas's moves | in progress |
| Live rotation | Turn the phone between landscape and portrait while playing | in progress |

## What changed since 2026-11-21

- **Looks.** Roxas, Xion, Xion (hooded), Axel and Roxas HD bound to the HD pictures; Axel's size in the field and in the pause
  (camp) menu; looks kept out of cutscenes where they do not belong; the Light & Dark keyblade fix; looks stay in sync on the phone.
- **Title screens + R3.** Roxas May Cry, Xion Knows Best and Axel Won't Cry title screens with their own logos and boot-intro
  sketches; R3 on the title switches between them live and the choice is saved.
- **CHEATS row.** A fourth title-menu row opens the Roxas May Cry menu before a game is loaded.
- **Fixes.** Three rounds of review fixes (settings, save slots, files, HD assets, the Xion save), the Mac / iPhone build scripts
  (signing, device runs, data push, cleanup) with their own test script, the PS2 form growth rewards, portrait and touch layout
  changes, and upside-down portrait turned by the app itself (an iPhone does not turn its interface upside down).

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
| `docs/` | plans, the mod's mechanics and difficulties, Xion, graphics notes |
| `docs/FOR-COLLABORATORS.md` | where to start if you work on KH2 with other tools (OpenKH, Unity) |

## Building

First run `python3 tools/publish/prepare_sources.py OUTPUT --exe FILE --mod DIR --data DIR` in the checkout. Use the resulting private sources to build. The data directory is the baked English PC tree.

First run `python3 tools/publish/prepare_sources.py OUTPUT --exe FILE --mod DIR --data DIR` in the checkout. Use the resulting private sources to build. The data directory is the baked English PC tree.

You need your own copy of *Kingdom Hearts HD 1.5+2.5 ReMIX* for PC and of the Nobody May Cry mod. A work tree needs, next to these
sources, the lifted C and the game's data tree, both made locally from those copies. In the build layout, generate runtime/xion_fields.inc with tools/runtime/xion_fields.py using your local data. On Linux:

```sh
make -j8                                         # identity address mode
make XLATE=2                                     # bias mode: the Apple address model, on Linux
KH_EXE=$PWD/pc/kh2fm.exe ./khnmc --selftest
tools/route_batch.sh -n 4 -j 2 -d cycle -g 7000 -s 600 -o <dir>
```

The iPhone build runs on a Mac with Xcode (`tools/runtime/ios/`, see `docs/APPLE-PLAN.md`).

## Related

The mod is also available as a **decomposition for OpenKH** ([nmc-decomposed](https://github.com/dexedrne/nmc-decomposed)): every
change it makes as readable patches rebuilt from the vanilla game. Assets are never uploaded; the tools take them from your own copy
of the mod.

## Rules

- Nothing from the game or the mod is committed: no executable, data, lifted code, frames or audio.
- The mod behaves as shipped, bugs included; improvements are technical, and switchable.
- What leaves the working machine is the output of `tools/publish/export.sh`, never the working repository itself.
- Personal use only.

## more kingdom hearts stuff

- [Roxas May Cry](https://github.com/dexedrne/roxas-may-cry): the Roxas campaign played to the end, built on your PC from your own game files
- [NMC Fixed](https://github.com/dexedrne/NMC-Fixed): the original mod with every fix and addition, for the Steam version through OpenKH
- [nmc-decomposed](https://github.com/dexedrne/nmc-decomposed): the mod as readable patches, rebuilt from your own copy of the game
- [kh2fm-save-converter](https://github.com/dexedrne/kh2fm-save-converter): bring a PS2 (PCSX2) Final Mix save into the Steam version
- [358/2 Days on iPhone](https://github.com/dexedrne/khdays-port/tree/ios) (work in progress): the native Days port brought to iPhone and iPad, dual screens, touch and controllers
