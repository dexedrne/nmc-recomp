# For collaborators: what is here and what might help an OpenKH / Unity project

This repository is a source-level port of Kingdom Hearts II Final Mix (the PC / Steam build) with the Nobody May Cry (NMC) mod
integrated. The game's executable is statically lifted to C and runs on a hand-written runtime; the target is a personal iPhone
build. Status, layout and build steps are in `README.md`.

**No game material is in this repository**: no executable, data, lifted code, textures, models, animations, frames or audio. Every
tool here reads YOUR OWN copy of the game and the mod and writes its output locally. Please keep it that way in anything you add.

## If you are building with OpenKH and Unity

The lifted engine itself will not drop into Unity. What should carry over is the knowledge about formats, scripts and mechanics:

| You want to know | Look at |
|---|---|
| How NMC's combat works, mechanic by mechanic, in its PS2 form and its updated PC form | `docs/mod/MECHANICS.md` |
| Where the PC mod's code changes sit (the DLL's 40 function hooks, 87 static edits, 114 added script calls) | `docs/mod/MOD-HOOKS.md` |
| The four difficulties, including how "Less Than Nothing" turns restore items into damage | `docs/difficulty/` |
| How the game renders: the Direct3D 12 calls, the pipeline-state key, texture formats, shaders | `docs/graphics/` |
| How the game talks to the platform: files, audio, input | `docs/platform/` |
| How the gummi ship was removed (world map warps directly) | `docs/gummi/GUMMI-REMOVAL.md` |

Tools that work on game files and need nothing but Python 3 (each says how to use it at the top of the file):

- `tools/mod/msgdump.py`: every message of a message file (`msg/<lang>/*.bar`) as text: ability names and descriptions,
  tutorial boxes, objectives. The quickest way to read what a mod says about its own mechanics.
- `tools/mod/bdxdis.py`, `tools/mod/bdxscan.py`: disassemble the game's script bytecode (BDX: AI and area scripts, found inside
  `.mdlx`, `.ard`, `.bdx` files) with the names of its system calls, including the 114 the NMC DLL adds. They read the instruction
  and trap tables from OpenKH's own sources at run time (pass `--descs` and `--traps`), so nothing from OpenKH is copied here.
- `tools/mod/ard_events.py`, `tools/mod/ard_barstat.py`: area files: which events (cutscenes), jumps and programs each holds.
- `tools/graphics/pso_key.py`: decode the game's pipeline-state key.

## Other projects worth knowing (not ours, not included here)

- **libkh2** (https://github.com/Kozarusa/libkh2): Sora's controller from KH2 Final Mix as an embeddable library with a C ABI, in
  the spirit of libsm64. The host engine (Unity, Godot) supplies collision triangles and input each tick; libkh2 runs the movement
  and attack logic from the user's own PS2 disc and answers position, facing, state, motion, attack windows and the posed skeleton.
  For a Unity PvP project this is the closest thing to the game's own movement and combat rules.
- **kh2fm-decomp** (https://github.com/Kozarusa/kh2fm-decomp, branch `fm-source`): a matching decompilation of the PS2 KH2 Final
  Mix executable (about 8,000 functions as C++ that compiles to the original instructions), built on GovanifY/kh2. Readable game
  logic and names.
- **kh2-ai-decomp** (https://github.com/thundrio-kh/kh2-ai-decomp): every vanilla AI and area script decoded with OpenKH's tools,
  with notes on the script system calls. Useful next to `tools/mod/bdxdis.py` to see what NMC changed in a script.

## NMC's combat in two minutes (details and sources in docs/mod/MECHANICS.md)

On PC almost all of it is ONE script: the party slot of Donald is turned into an invisible helper object, and its AI script
(`dona` inside `obj/P_EX020.mdlx`) runs every combat rule each frame; the player objects' own scripts (`obj/P_EX100*.mdlx`) do the
rest, and data tables (`00battle.bin`, `03system.bin`) carry the movesets, costs and starting abilities.

- **Reversal**: an invincible sidestep. Timed at the last moment it is a **Perfect Reversal** (restores Drive, raises the style
  rank; with the right ability, brief slow motion). Used mid-combo it keeps your place in the combo (**Dodge Offset**). An attack
  right after a Perfect Reversal gives special slashes.
- **Style rank and Luminous Drive**: hits and taunts raise a style gauge with seven ranks (Crazy! up to SSStylish!!). At rank S or
  higher Roxas glows (**Luminous Drive**): stat buffs, MP over time, faster Drive; at the top rank Drive Forms do not run out.
- **Taunt** (L2 + Attack on the ground): style and Drive; **EX Taunt** costs MP and attacks and heals.
- **Enemy Step**: jump off an enemy to reset the air combo and the aerial dodge.
- **Hold-R2 moves** (Upper Slash, Finishing Leap, Horizontal Slash), **keyblade-specific specials**, and aerial dash / glide rules.
- **Forms**: Bladeless, Light & Dark and Awakened, each with its own command menu, MP rules and Limit Break.
- Changed from the original: Anti Form can still trigger; summons work; restore items hurt on the hardest difficulty.

An "NMC mode" for a PvP project would mostly be these rules, which are readable in the scripts with the tools above.

## Working together

- Write access is shared through this repository. Please work on a branch of your own and open a pull request; `main` is
  rewritten from the author's working copy.
- What would help this project back: format knowledge OpenKH has and these notes lack, especially on motion (`.mset` / `.anb`)
  files and the AI scripts of enemies and bosses.
