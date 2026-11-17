# Following a new version of Nobody May Cry (task 34)

The port is pinned to ProjectNobodyMayCry/SillyBillyMod1 commit `79a23a6` (2026-08-16);
`mod/nmc-version.json` records that version, `docs/MOD-VERSION-INVENTORY.md` lists everything in the port that depends on it,
`tools/mod_update_check.py` compares a checkout with the pin and prints the steps below that a given change needs, in this order.

## Rules

1. **Gameplay follows the new mod version exactly; the port's technical improvements stay.** Content, mechanics, difficulty,
   music, which scenes play: whatever the new PC version does, bugs included ("as shipped": a mod bug the new version still has
   stays; one it fixes is fixed here too). Kept on top, re-derived for the new version where they depend on the data: data
   pruning profile A (no change of behaviour), the runtime's fixes (background-load lock, id-table guard, the low guard in bias
   mode, the null backend's resource lifetime, sliced "stuck peer" waits; each behind its switch, so "as shipped" stays
   reproducible), the HD pairing as the PC does it, the iOS paths, the test clocks, and **the gummi ship removal** (an owner
   decision, "not even in the game": `00progress.bin` WLDF entry 53 no longer re-blocks Twilight Town, `wm00.ard` without the
   `m_51` gummi spawn in all five language folders, the gummi task a named fatal; docs/gummi/GUMMI-REMOVAL.md, inventory M3, M4,
   M27, M59..M61). A difference between the port and the new PC version that is not one of these is a defect.
2. **Nothing pinned is changed in place.** The new version goes into a NEW directory; the clone `~/projects/SillyBillyMod1` stays
   at `79a23a6` until the port has moved. The live data tree `data-full` is never written: a new tree is built next to it and the
   symlink is switched only after the verification (step 18). Work happens in a private worktree copy.
3. **Static analysis of the DLLs is the default.** Running DLL code, natively or emulated, needs the owner's explicit OK each time.
   Two steps of this procedure did so for 79a23a6: the static-edit list (Unicorn emulation of KH2Tweaks_Refactor.dll's DllMain,
   step 4) and the native handler oracles of tasks 7, 11 and 16 (`tools/native_*_oracle.c`). Ask before re-running them.
4. **Every difference is accounted for.** The checker reports unknown kinds of change loudly; none may be dropped. A detour, script
   call or option that is not ported gets a named fatal, as today, never a silent path.
5. Live desktop: every build and run under `nice -n 10`, at most 4 processes, at most 2 game runs at once, null graphics backend,
   no GUI. No time estimates to the owner.

## Step 0: is there a new version?

    python3 tools/mod_update_check.py --remote

prints (the tool itself never touches the network):

    git ls-remote https://github.com/ProjectNobodyMayCry/SillyBillyMod1.git refs/heads/main

The owner (or a job with network permission) runs that. A hash other than `79a23a6d05b79553cac901345a6fe4ace1a60918` means new
commits. Check also the repository's releases page and the mod's announcement channel: a release may be published outside `main`.

## Step 1: get it

    git clone https://github.com/ProjectNobodyMayCry/SillyBillyMod1.git ~/projects/SillyBillyMod1-<commit7>

(A fresh clone has LF ini files; the pinned working tree has CR LF in its 14 ini files. The checker calls that "line endings
only" and needs no step for it.) Never `git pull` in `~/projects/SillyBillyMod1`.

## Step 2: run the check

    mkdir -p run-mod-<commit7>
    python3 tools/mod_update_check.py --all-files --json run-mod-<commit7>/check.json ~/projects/SillyBillyMod1-<commit7> | tee run-mod-<commit7>/check.txt

Exit status 0 = identical (stop here), 1 = changes found, 2 = usage error / unreadable input. Read, in this order: `== LOUD` (things
the tool cannot settle: decide them first, with the owner where the item says so), `== FILES` (per kind), `== MOD.YML`, `== INI`,
`== DLL` (detour by detour, the syscall table, Moose / Neverland functions), `== SCRIPTS`, then `== STEPS TO REDO, IN ORDER`, each
with its reasons and commands. It needs `objdump` (binutils) for the DLL comparison; without it the DLL part is reported as not
compared, loudly. The examples in implementation notes (four older versions of the mod) show what real output looks like;
`python3 tools/mod_update_check_variants.py` re-proves the checker's rules on targeted changes of the pinned mod (a patched DLL
helper, a renamed ini key, a case-only rename, an item kind, a shipped `wm00.ard`, ...) and must report 0 missed.

How each detour is matched: by the exe function its slot's signature scanner resolves to (inline sites: by their option; their
exe address is the slot's function plus the offset the DLL adds, both read from the DLL), then by the handler's code. Status:
`same bytes` = the handler's bytes AND everything it reaches are unchanged; `same code (moved)` = the instructions are identical
once addresses are replaced by what they name (an import, a string, a constant, an exe function, a place in the same function): a
relinked DLL; `own code the same, a DLL function it reaches CHANGED`; `CODE CHANGED`; `REMOVED`; `NEW`. Moved rows need no
re-audit, the others do. "Reaches" covers every call, jump (a tail jump too) and `lea` of code, including routines without an
unwind entry (`.pdata`): those are hashed as leaf functions (row 4's handler DLL 0x2de50 tail-jumps to such a helper, 0x2dd60,
which `runtime/mod_hooks_a.c` transcribes).

The DLL comparison also covers the **ini keys the port depends on** (the difficulty and drive constants, the detour gates, the
guarded and out-of-scope options): whether the new DLL still names each key, the function that reads it (raw / normalised / deep
hash: the option loaders DLL 0x20590, 0x21090, 0x22440 and the option initialisers, which no handler reaches) and the default it
passes for an absent key (decoded statically: `beginner_hp_mult` / `beginner_mp_mult` are absent from the shipped ini and get the
DLL's 1.0). Any difference is an `[ini]` step (and `[hooks]` for a gate), even when every handler is byte-identical. A key of the
ini files that the pinned DLL did not read and the new one names is loud.

Names are compared in any letter case (Windows and Panacea do not see it): a DLL renamed by case is still compared. Where the port
opens a file by its exact name, a case-only rename is a step or loud (`dll/KH2Moose.ini`: `runtime/moose.c` would silently run
with vanilla pools; the DLL paths in the Makefile).

## Steps 3..18 (do only the ones the check lists, in this order)

**decide.** Owner decisions and unknowns: a new DLL in `dll/` (Panacea loads every `dll/*.dll` of the compiled mod: a new code
layer), a LuaBackend script, a file kind the port has no rule for, a non-copy `mod.yml` method, an option the port does not have
switched on (`retry`, `savepoint_qol`, `patch_gummiskip`: their detours are not ported; `debug_on`, `soft_reset`,
`multiple_shortcuts`: out of scope with no guard), a script call the port does not implement. Decide what the PC does with it

**bake.** A new tree (never into `data-full`):

    python3 ~/projects/khnmc-decomp-edition/tools/bake_nmc_tree.py --vanilla <VANILLA_EXTRACTION> --mod ~/projects/SillyBillyMod1-<commit7> --out <NEW_TREE> --link
    cp data-full/khnmc-hd-packages.tsv <NEW_TREE>/

`<VANILLA_EXTRACTION>` is the owner's own extraction of the PC game that the first bake used (ROADMAP: on build-host, 55,094 vanilla
files; this machine has none apart from `data-full`, which already holds the mod's files). The bake refuses non-copy methods (run
OpenKH's Patcher for those first: docs/OPENKH-MOD-APPLICATION.md) and a non-empty `--out`. Check its printed counts: copy assets
= the new `mod.yml` count, `missing sources` = 0, `verification: mod files differing from source = 0`.
If no vanilla extraction is at hand, a tree can be made from `data-full` by hard links plus the changed and added mod files; files
the new version REMOVES need the vanilla original back (the checker lists them as `removed`): that needs the extraction.

**gummi.** After EVERY bake (the checker lists it whenever it lists the bake): a bake from vanilla + mod puts back the mod's
unedited `00progress.bin` and the unedited `wm00.ard`, so without this step the new tree silently re-blocks Twilight Town and
spawns the gummi route again (which the port then stops on: `gummi-gate-contract`). Also when the mod ships an `ard/*/wm00.ard`
(the edit must then be made from the mod's file). Copy the repository's `tools/gummi/` (`make_gummi_removal.py`,
`wldf_decode.py`, `spawnparse.py`, `progress_diff.py`) into the work folder; in the copy set `PROG_IN` to the new
`00progress.bin` and `WM_IN` to the `wm00.ard` the tree gets (the owner's vanilla extraction, `scratch/S6-gummi-data/src/us/wm00.ard`,
or the mod's own file); run it. It asserts that WLDF entry 53 still has the block command 5 at file offset 36,636; if that fails,
find the entry that re-blocks Twilight Town in the new file (`wldf_decode.py`) and adapt the edit. Then put both edits into the
new tree, `00progress.bin` and `ard/{us,fr,gr,it,sp}/wm00.ard` (data-full has the edited `wm00.ard`, sha256 `63a84de4...`, in all
five):

    python3 tools/apply_gummi_overlay.py --in-prog $(sha256sum <new>/00progress.bin | cut -c1-64) [--in-wm00 <sha256 of WM_IN>] <NEW_TREE> <work>/gummi/wm00.gummi-removed.ard <work>/gummi/00progress.gummi-removed.bin

It replaces a target only when its hash is the input the edit was made from (or it is already edited), and exits 1 when one is
REFUSED or absent: read every line. At re-pin, set `IN_PROG` (and `IN_WM00` when the mod ships `wm00.ard`) in
`tools/apply_gummi_overlay.py` to those input hashes; a pin check refuses a fingerprint whose `00progress.bin` is not `IN_PROG`.

**hd.** The provenance list from the bake manifest, then the HD table check:

    python3 tools/hd_modlist.py --tree <NEW_TREE> --manifest <NEW_TREE>.manifest.json --out <NEW_TREE>/khnmc-hd-modlist.txt --expect-mod <mod files>
    KHNMC_DATA=<NEW_TREE> KHNMC_HD_PACKAGES=<NEW_TREE>/khnmc-hd-packages.tsv KHNMC_HD_MODLIST=<NEW_TREE>/khnmc-hd-modlist.txt nice -n 10 ./khnmc --hd-check

**edits.** KH2Tweaks_Refactor.dll changed, or an ini key the DLL patches the exe from (class static-edit or read-no-effect). The
list for 79a23a6 came from emulating the DLL's DllMain (owner's OK needed, rule 3):
`~/projects/khnmc-decomp-edition/tools/mod/emu_rerun.py` (needs `scratch/S5-mod-hooks/emu.py` and `scan_emu.json`; copy it and set
`MOD` to the new `dll/`), then `derive_edits.py`, `apply_edits.py`, `verify_edits.py` (copies with the paths changed, output to a
new folder). Check: the same pristine exe (`9002b2de...`), every edit's old bytes, the edits that follow the shipped ini
semantics (inline `#comment`, `worldpoint_amount=on` -> 54). Then in the worktree's Makefile: `MOD_EDITS` = the new list,
`MOD_STATIC_FUNCS` = the functions whose bytes the edits change (`tools/mod_static_lift.py` refuses a mismatch and names the
difference), `MOD_STATIC_INLINE` / `_CHAINED` / `_REPLACED` from the detour audit. Without the owner's OK: read the new option
initialisers statically and write the list by hand in the same format; say so in the status file.

**gen.** Regenerate and re-test the re-lifted functions:

    nice -n 10 make -j4
    nice -n 10 make mod-static-difftest MOD_STATIC_DIFFTEST_JOBS=4 && nice -n 10 make mod-neverland-difftest mod-moose-difftest

0 mismatches is the bar; every edit must be executed by a valid matching test (implementation notes 4.2), sensitivity run for new edits.

**slots.** The new DLL's slot addresses move with every build. For each slot the ports use (the old ones are in
`tools/mod_dll_slots_h23.txt`; find the new addresses from the new attach sites the check printed and from the handlers'
listings), resolve and write a new expectation file `tools/mod_dll_slots_<version>.txt` (every active detour's slot, the two
inline sites' slots, every slot the ports call through):

    python3 tools/mod_dll_slots.py --dll <new>/dll/KH2Tweaks_Refactor.dll --exe pc/kh2fm.exe --show <slot> ...

then point the Makefile's `mod-dll-slots` rule (`--expect`) at the new file and run `make mod-dll-slots
MOD_TWEAKS_DLL=<new>/dll/KH2Tweaks_Refactor.dll`. The `DERIVED` / `OFFSET` / `AVOID` tables inside `tools/mod_dll_slots.py` carry
DLL addresses of the old build: update them from the new listing. Every slot must resolve to one pattern match (or a known second
one). The re-pin takes the file with `--slots-file` and records it in the fingerprint; its pin check fails unless EVERY active
detour slot is in that file, resolves, and equals it, and unless the Makefile rule names the same file (an expectation file of the
old build would otherwise match nothing and pass).

**hooks.** For every detour the check marks CHANGED, callee changed, NEW or REMOVED: read the new handler in
`objdump -d -Mintel <new DLL>` against the port the line names (`runtime/mod_hooks_a.c`, `mod_hooks_b.c`, `mod_difficulty.c`,
`mod_params.c`, `mod_syscalls.c`, `mod_static_sites.c`); classify again chains / replaces (does the handler reach its trampoline
slot between its start and its int3 padding: implementation notes section 8); update the Makefile lists (`MOD_A_HOOKS`, `MOD_B_HOOKS`,
`DIFFICULTY_HOOKS`, `MOD_PARAMS_HOOKS`, `MOD_DLL_REPLACES`) and `DETOUR_PORT` in `tools/mod_update_check.py`; selftest cases with
expectations written from the listing; mutation-check them. A NEW detour behind an option that is off: port it only if the option
reads as on (the DLL's reader: on / true / 1); otherwise extend the option guard. Every port goes through an independent review
before the merge.

Two `[hooks]` reasons come from DATA, not from the DLL, because the port implemented only what the shipped data uses:
* **item effect kinds** (`03system.bin`, `item` records of type 0 / 1, the items the player uses: the kind byte +5). The port
  (`runtime/mod_difficulty.c` `f_3c4b80` / `f_3c3da0`) has kinds 0, 11, 12 and the DLL's no-op kinds; 1..10, 13..24, 30, 31
  are named fatals because no shipped item uses them. A version whose items use one of them: port it from DLL 0x31260 /
  0x30f60 (statically) with selftest cases. The fingerprint's `item_kinds` holds the counts.
* **the extra-file probes** (`01..05common.bdx`, `01..20effect.bar`): the DLL loads such files when the data tree has them; the
  mod as shipped has none, so that code is a named fatal (`runtime/mod_hooks_b.c`). A version that ships one: port DLL
  0x33610 / 0x300a0 / 0x2fd20 / 0x30180 for it.

**syscall-table.** `runtime/mod_syscalls.c` `entries[]` = the new table (index, name, flags, DLL handler) as the checker re-read it
from the builder; indices are what the scripts use, so a shifted table changes every call.

**moose.** `python3 tools/gen_mod_moose.py --dll <new>/dll/KH2Moose.dll ...` (templates at fixed DLL addresses: on another build it
stops; then re-decode the request templates, the patterns and the NOP table from the new listing, and update the tool). Re-read
the pool code (`runtime/moose.c` header: the sites, the 32 MiB alignment, the block increases) against the new DLL;
`MOOSE_HOOKS`, `MOD_MOOSE_FUNCS`; `make mod-moose-difftest`. Then update `MOOSE_CITED` / `MOOSE_DATA` and the build hash in
`CONST_BUILD` (`tools/mod_update_check.py`) to the addresses you decoded from: the re-pin finds every previously cited function
and data block again by its code / bytes and fails on one it cannot find (a3b4bca's rebuilt Moose: 2 of 7 found, 5 fail).

**neverland.** `python3 tools/gen_mod_neverland.py --dll <new>/dll/NeverlandFly.dll ...`; `runtime/mod_neverland.c`,
`MOD_NEVERLAND_HOOKS`; `make mod-neverland-difftest`; then `NEVERLAND_OWN_END` and `CONST_BUILD` as for Moose (899691a's rebuilt
DLL: 4 of 7 functions found again by code, 0x1070 / 0x12d0 / 0x15a0 changed: their signature tables moved).

**scripts.** `python3 tools/bdxscan.py --mod-syscalls runtime/mod_syscalls.c --detail <new>`: every table-3 call a script uses must
be implemented (decode the DLL handler statically, add the case and its selftest); a ported call whose handler changed is
re-audited.

**ini.** The keys of class port-constant: `runtime/mod_config.h` (difficulty table), `runtime/mod_syscalls.c` (drive
multiplier), the dual-wield slots; update the selftests and oracle expectations that carry the numbers. KH2Moose.ini is read at run
time (nothing to rebuild; check the not-ported bits). Listed also when only the DLL changed: the code that reads a key, its name
or its default for an absent key differs (read the loader in the new listing: which key, which default, which global, and that
the handler reads that global). When re-pinning, the pin checks compare the difficulty table and the drive multiplier with the
value the DLL ends up with: the ini's when the DLL names the key and the ini has it, the DLL's own default when the ini lacks it
(decoded from the loader; a key without a decoded default fails), and fail for a key the DLL does not read.

**tweaks-data.** `dll/tweaks/command_menu_offsets.bin`: the icon hooks read it at run time; update the shipped-shape pin in
`runtime/mod_hooks_b_test.c` (29 entries, first "bb0"). A `special_command_menu.bin` or `KH2_Tweaks_SpecialCommandList.ini` means
porting the special-command vector (implementation notes rows 21, 25).

**events.** Area files changed: `python3 tools/event_refs.py --data <NEW_TREE> selfcheck` and `summary`; the events-stub tree
check (`KH_EXE=$PWD/pc/kh2fm.exe KHNMC_DATA=<NEW_TREE> nice -n 10 ./khnmc --selftest`); music after a stub (`event_refs.py music`,
`tools/music_compare.py` on the routes' music logs).

**prune.** Profile A must again be "no change of behaviour":

    python3 tools/prune_lists.py --data <NEW_TREE> --exe pc/kh2fm.exe --out-a prune-A.list --out-b prune-B.list --out-actors prune-B-actors-unproven.list

then the routes with `KHNMC_DATA_DENY=prune-A.list KHNMC_DATA_DENY_FATAL=1` (0 denied), and the room sweep with the deny list.

**subset.** `tools/make_route_subset.sh -o route-subset-<commit7>` (logging runs of the default and story routes on the new tree,
the subset, verification runs), then `tools/darwin_route.sh sync`.

**mechanics.** Re-read the sections of `docs/mod/MECHANICS.md` that name changed files; the mod's own words for a mechanic:
`python3 ~/projects/khnmc-decomp-edition/tools/mod/msgdump.py <msg file>`.

**repin.** After the port is updated and verified:

    python3 tools/mod_update_check.py --write-fingerprint ~/projects/SillyBillyMod1-<commit7> --edit-list <the new edit_list.json> --slots-file tools/mod_dll_slots_<version>.txt

(The fingerprint being replaced is read first: Moose's and Neverland's cited functions are found again in the new build from it.)
It must end with "pin checks: the fingerprint agrees with the port's records" (exit 0): every active detour maps to
`DETOUR_PORT` and the inline sites land on `INLINE_SITES`; every active detour slot is in the expectation file, resolves and
equals it, and the Makefile's `mod-dll-slots` rule names that file; the syscall table equals `entries[]`; the difficulty table and
the drive multiplier equal what the DLL reads (ini or decoded default); gen/ was made from these DLLs; Moose's and Neverland's cited
functions and data were found in this build; every used script call is implemented; `command_menu_offsets.bin` has the count
`runtime/mod_hooks_b_test.c` pins; `tools/apply_gummi_overlay.py` takes this version's `00progress.bin`; no item uses an unported
effect kind.

Build-specific constants (a new build makes them stale; the pin checks name the stale ones, this is the full list):
`tools/mod_update_check.py` `DETOUR_PORT`, `INLINE_SITES`, `GUARDED_OPTIONS` / `GUARDED_SITES`, `OUT_OF_SCOPE_OPTIONS`,
`CONST_KEYS`, `MOOSE_KEYS`, `MOOSE_CITED` / `MOOSE_DATA` / `NEVERLAND_OWN_END` with `CONST_BUILD`, `ITEM_USE_TYPES` /
`ITEM_KINDS_UNPORTED`, `EXTRA_PROBE_RE`, `EXACT_NAME_USERS`, `DEFAULT_EDITS`; `tools/mod_dll_slots.py` `DERIVED` / `OFFSET` /
`AVOID`; `tools/apply_gummi_overlay.py` `IN_PROG` / `IN_WM00`; the slot expectation file and the Makefile rule that names it;
Makefile `MOD_TWEAKS_DLL` / `MOD_MOOSE_DLL` / `MOD_NEVERLAND_DLL` (exact letter case) and `MOD_EDITS`; the clone paths in
`tools/mod/*.py` and `tools/gummi/make_gummi_removal.py`. Then: the counts in `docs/MOD-VERSION-INVENTORY.md`, the pinned-version
entry in the version record, and `python3 tools/mod_update_check.py <new>` must say IDENTICAL.

**verify.**

    nice -n 10 make -j4 && KH_EXE=$PWD/pc/kh2fm.exe KHNMC_DATA=<NEW_TREE> nice -n 10 ./khnmc --selftest
    tools/route_batch.sh -n 4 -j 2 -d cycle -g 7000 -s 600 -w "nice -n 10" -o run-<commit7>
    tools/route_batch.sh -n 1 -j 1 -i $PWD/input/route-story.txt -r none -g 9300 -s 600 -w "nice -n 10" -o run-<commit7>-story
    tools/room_sweep.sh   (task 33: every area file, with the new tree)
    tools/darwin_route.sh sync / selftest / run   (the Mac mini, its own rules)

The routes are scripted against the story of 79a23a6 (`input/route-*.txt`: missions EH03_INTRO1/2, TT07_MS101/103, the Less Than
Nothing item drain); if the new version changes those parts, the route scripts change with it (that is following the mod, not a
defect). Only then switch `data-full` to the new tree, merge, and copy tools, docs and the status file into the repository.

## What to tell the owner

Short, one message: the version (commit, date, its title), what changed for the player in the mod's own terms (from its messages
and `mod.yml`, not our file names), what the port did (steps, number of hooks re-audited / added, script calls added), what is NOT
ported and stops with a named fatal, the verification (selftest count, routes, sweep, Mac), the size of profile A, and the cost of
the work. Ask only what needs his decision (the `decide` items, an emulation or native DLL run).
