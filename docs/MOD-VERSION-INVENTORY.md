# What in the port depends on the NMC mod version (task 34)

The port is built from ONE version of the PC mod: the public repository ProjectNobodyMayCry/SillyBillyMod1 at commit
`79a23a6d05b79553cac901345a6fe4ace1a60918` (2026-08-16, "add files"), local clone `~/projects/SillyBillyMod1`. That version is
recorded in `mod/nmc-version.json` (the fingerprint; section 13 explains its fields) and checked by `tools/mod_update_check.py`.
The procedure for a new version is `docs/MOD-UPDATES.md`. This file lists every place in the port that depends on the mod's
version and how each was made, so that nobody has to rediscover it.

How to read the tables: **made by** is the tool and the mod input it reads, or "by hand" with the DLL addresses the work was read
from (static listings, `objdump -d -Mintel`, unless the row says otherwise). **step** is the step of `docs/MOD-UPDATES.md` (and
of the checker's output) that redoes the item. Addresses: "exe 0x..." are RVAs in the pinned `kh2fm.exe` (sha256
`9002b2de...849ed`), "DLL 0x..." are RVAs in the DLL named in the row.

The pinned version in numbers: 4,864 files (1,061,706,924 bytes), 4,861 of them `copy` assets of `mod.yml` (not assets: `mod.yml`,
`icon.png`, `preview.png`); three DLLs, 14 ini files, three data files of the DLL (`dll/tweaks/*.bin`), 435 scripts in 948
BAR / BDX files. The clone's working tree has CR LF line endings in its 14 ini files where the commit has LF
(`git diff --ignore-cr-at-eol` is empty): content-identical; the fingerprint records the working tree as it is (the data tree
was made from it), and the checker reports a fresh LF clone of the same commit as "line endings only".

**Inventory count: 62 mod-dependent items (M1..M62; M59..M62 added after the task 34 review).**

## 1. The data tree

| # | item | where | made by | step |
|---|---|---|---|---|
| M1 | The baked data tree: the vanilla PC extraction with the mod's 4,861 copy assets laid over it (55,094 vanilla + 4,861 mod files on build-host; `data-full/` here) | `~/work/khnmc-build/data-full` (symlinked into every worktree) | `tools/bake_nmc_tree.py` (repo) `--vanilla <extraction> --mod <clone> --out <tree> --link`, OpenKH Panacea semantics (docs/OPENKH-MOD-APPLICATION.md); case-insensitive name mapping | bake |
| M2 | The bake manifest (`<tree>.manifest.json`: which files came from the mod) | was `<tree>.manifest.json` on build-host; a copy used for M4 was in a scratchpad (`mod-manifest.json`) | written by the bake | bake |
| M3 | Gummi removal, a PORT data edit on the mod's file (owner decision, kept: MOD-UPDATES rule 1): `00progress.bin` WLDF entry 53 block cmd 5 -> 14 at file offset 36,636 (the TWTNW unlock no longer re-blocks Twilight Town) | `data-full/00progress.bin` (sha256 `73f984ec...`; = `~/projects/kh2-nmc-analysis/scratch/mod-gummi/00progress.gummi-removed.bin`) | repository `tools/gummi/make_gummi_removal.py` (`PROG_IN` = the pinned clone's `00progress.bin`, sha256 `cdba15a9...`; asserts the entry and offset), put into the tree by `tools/apply_gummi_overlay.py` (M59). Every bake puts the unedited file back: the step follows every bake | gummi |
| M4 | Gummi removal on a vanilla file: `wm00.ard` without the `m_51` spawn (gummi route nodes), in ALL FIVE `ard/{us,fr,gr,it,sp}` | `data-full/ard/*/wm00.ard` (sha256 `63a84de4...` in all five; = `scratch/mod-gummi/wm00.gummi-removed.ard`) | same script (`WM_IN` = the owner's vanilla `scratch/S6-gummi-data/src/us/wm00.ard`, sha256 `32a8a76a...`), applied by M59 to the five folders; depends on the mod when the mod ships an `ard/*/wm00.ard` (the checker lists [gummi] for it) | gummi |
| M5 | HD provenance list (`mod <path>` lines: which game files are the mod's, so Panacea's pairing with the MOD's remastered folder is reproduced; 4,861 mod files, 8 `nohd` rules, obsolete since task 22) | `data-full/khnmc-hd-modlist.txt`, read by `runtime/hd_assets.c` (`KHNMC_HD_MODLIST`) | `tools/hd_modlist.py --tree --manifest --out --expect-mod` | hd |
| M6 | The pruning lists, profile A (as shipped, 32,326 files, 3.75 GB) and B (no cutscenes, 4.18 GB) and the unproven high-detail actors | `prune-A.list`, `prune-B.list`, `prune-B-actors-unproven.list` (worktree; never committed) | `tools/prune_lists.py --data <tree> --exe pc/kh2fm.exe ...` from the tree's area files, objentry, spawn files and scripts (task 32) | prune |
| M7 | The route data subset the Mac mini gets (642 MB, 1,762 files) and its name list `FULLTREE.txt` (92,765 names of the full tree) | `~/work/khnmc-build/route-subset/` | `tools/make_route_subset.sh` (logging runs under `openlog_shim`, then `tools/route_subset.py`, then verification runs) | subset |

Not mod-dependent, for contrast: `data-full/khnmc-hd-packages.tsv` (the game packages' own HD tables, `tools/hd_packages.py`),
`pkgdir/` (the game's packages), the lifted exe (`lifted/`).

## 2. KH2Tweaks_Refactor.dll: the static edits (data and code)

| # | item | where | made by | step |
|---|---|---|---|---|
| M8 | The static-edit list: 89 edits (87 file-backed: 60 code bytes in 28 functions, 10 `.rdata`, 17 `.data`; 2 run-time-only script-table pointer bytes), 9 inactive AntiForm edits, the 4 KiB buffer the DLL allocates (blow_gravity float, world-symbol table, relocated symbol-instance block) | `~/projects/kh2-nmc-analysis/scratch/mod-gummi/edit_list.json` (Makefile `MOD_EDITS`) | **emulation**: `tools/mod/emu_rerun.py` runs the DLL's DllMain in Unicorn on the static exe bytes with the shipped ini (S5 scratch `emu.py`, `scan_emu.json`), `tools/mod/derive_edits.py` (ini semantics, labels from S5 `patches.json`), `apply_edits.py`, `verify_edits.py` (task B1, docs/mod/MOD-HOOKS.md). Running DLL code, even emulated, now needs the owner's OK | edits |
| M9 | Edit table for the runtime: old / new bytes, class, owning function, option, buffer image | `gen/mod_static_table.h`, `gen/mod_static_classify.json` | `tools/gen_mod_static.py --edits MOD_EDITS --exe --universe --funcs MOD_STATIC_FUNCS ...` (Makefile rule) | gen |
| M10 | Applying the data edits and the buffer at load, old bytes verified (named fatals `mod-static-*`) | `runtime/mod_static.c` (`kh_mod_static_init`, called first in `guest_main`, `runtime/loader.c`) | by hand (task 12); data from M9 | gen (code only if the mechanism changes) |
| M11 | The 30 functions whose bytes the edits change, re-lifted from the patched image | Makefile `MOD_STATIC_FUNCS` (30), `MOD_STATIC_INLINE` (3c3fe0 360920), `MOD_STATIC_CHAINED` (3a89a0), `MOD_STATIC_REPLACED` (3f29e0); `gen/mod_static_lifted.c` (+ Neverland and Moose functions, 35 in all), `gen/mod_static_lift.json` | `tools/mod_static_lift.py` with the lifter venv (`KHLIFT`, `KHLIFT_PY`, `KH_UNIVERSE`); it requires the changed-function set to equal `MOD_STATIC_FUNCS` and re-lifts each vanilla function first to prove the lifter run is reproduced | edits, gen |
| M12 | The two inline detour sites spliced into the re-lifted bodies: exe 0x3c40a9 (DLL handler 0x34490, `dualwield_forms`, continuation 0x3c40bf / 0x3c41a5) and exe 0x360a5a (DLL 0x34460, `patch_ability_categories`, 0x360a6d / 0x360ba5); the six dual-wield slot values 0,2,3,0,5,0 | `runtime/mod_static_sites.c`, the splice in `tools/mod_static_lift.py` | by hand from the DLL installers 0x1bdd7..0x1be42 and 0x1ee9d..0x1eeff and the handlers (task 12) | hooks, edits |
| M13 | Differential tests of the re-lifted functions against the patched bytes (native oracle of the lifter; no DLL code) | `gen/mod_static_difftest.json`, `..._mutation.json`; `tools/mod_static_difftest.py`, `mod_static_oracle.c`, `mod_static_teststub.c` | `make mod-static-difftest` | gen |
| M14 | Link check: every re-lifted function linked as the PC has it (chained / replaced / ini-switched) | `tools/check_mod_static_link.py` on every link (`obj/mod_static_link.ok`) | Makefile lists M11, M17, M27, M33 | gen |

## 3. KH2Tweaks_Refactor.dll: the detours, slots, script calls, options

| # | item | where | made by | step |
|---|---|---|---|---|
| M15 | The detour table: 42 attach sites (40 `DetourAttach` + 2 `DetourAttachEx`), 29 in the unconditional installer DLL 0x33930, 13 in option initialisers; 35 active as shipped, 7 inactive (`retry` x5, `savepoint_qol`, `patch_gummiskip`); per row slot, handler, exe target, option, chains / replaces | implementation notes section 2 (the audit), `tools/mod_update_check.py` `DETOUR_PORT` / `INLINE_SITES` / `GUARDED_SITES`, fingerprint `dll[...].detours` | by hand (tasks 8, 9, 11, 23 + review); re-derived statically by the checker (Detours' `DetourAttach` found by its byte signature, every call with its `lea rcx/rdx`, the gating option from the strings of the initialiser; the two inline sites' slot and offset from `mov REG,[slot]; add REG,N` in front of `DetourAttachEx`: 0x82300 + 0xc9, 0x81e20 + 0x13a). Each handler is hashed with everything it reaches, leaf routines without `.pdata` included (row 4: DLL 0x2de50 tail-jumps to 0x2dd60, the ability-level helper `ability_levels()` transcribes) | hooks |
| M16 | Signature-scanned pointer slots: 106 slots -> exe RVAs (the 35 detour targets and every engine function / data pointer the ports call) | `tools/mod_dll_slots_h23.txt` (expectation; named by the Makefile rule `mod-dll-slots --expect` and by the fingerprint `port.slots.file`), `gen/mod_dll_slots.json` | `tools/mod_dll_slots.py --dll --exe --expect` (`make mod-dll-slots`): reads each scanner's pattern from the DLL file and scans the exe; `DERIVED` / `OFFSET` / `AVOID` tables in the tool hold DLL addresses of this build. A new build gets its own file (`--slots-file` at re-pin); the pin check compares EVERY active slot with it | slots |
| M17 | Function hooks of the port: the hook lists that make each detoured exe function a runtime-owned `f_<rva>` | Makefile `MOD_A_HOOKS`, `MOD_B_HOOKS`, `DIFFICULTY_HOOKS`, `MOD_PARAMS_HOOKS`, `PHASE_HOOKS` (405440, 3f29e0), `MOD_DLL_REPLACES` (16 full replacements) | by hand from M15 | hooks |
| M18 | Hooks batch A: exe 0x2fc860 / 0x2fc4e0 (dual-wield, DLL 0x2da30 / 0x2d8f0), 0x4014a0 (DLL 0x2de50), 0x3a7cb0 (DLL 0x32780, solo_summon_fix), 0x29efa0 (DLL 0x2d190, shopface_fix), 0x405440 (DLL 0x333b0), 0x3a89a0 (DLL 0x32710), 0x3d61d0 (DLL 0x32580), 0x3d60c0 (DLL 0x32680) | `runtime/mod_hooks_a.c`, `mod_hooks_a_test.c` | by hand (task 9) | hooks |
| M19 | Hooks batch B: 0x3f29e0 (DLL 0x306c0 + helpers 0x30290 / 0x30510, the command-menu builder), 0x3f27d0 (0x30a80), 0x3c4780 (0x31c60), 0x351390 (0x2d5c0), 0x368820 (0x2da70), 0x4060e0 (0x33550), 0x3d1730 (0x2f970), the extra-file chain 0x3f6620 / 0x3f65a0 / 0x3f6510 / 0x3a17f0 (0x300a0 / 0x2fd20 / 0x30180 / 0x33610), icon hooks 0x153850 / 0x15aab0 / 0x15aba0 (0x10340 / 0x10590 / 0x10100), 0x31dd70 (0x2d670); the DLL's two command word lists (DLL .bss 0x819d8 / 0x819e8). **Data subset:** the extra-file chain's loading code is NOT ported because the shipped mod has none of the probed files (`01..05common.bdx`, `01..20effect.bar`): a hit is the fatal `mod-extra-file-not-ported`; the checker makes an added file of those names loud + [hooks] (`EXTRA_PROBE_RE`) | `runtime/mod_hooks_b.c`, `mod_hooks_b_test.c` | by hand (task 23) | hooks |
| M20 | Difficulty hooks: damage 0x3c12f0 (DLL 0x32d30), level-up popup 0x3ed7d0 (0x31ee0), bonus 0x3edf90 (0x2f9b0), item effect 0x3c4b80 (0x31260, kinds 0, 11, 12, the Less Than Nothing reversal), item usable 0x3c3da0 (0x30f60). **Data subset:** the item-effect port has kinds 0, 11, 12 and the DLL's no-op kinds; 1..10, 13..24, 30, 31 are named fatals because the shipped item records the item functions take (`03system.bin` `item`, types 0 and 1: ids 1..7, 0x83, 0x112..0x117, implementation notes) are all kind 0 (byte +5). Fingerprint `item_kinds` (counts by type and kind); a kind outside the ported set on a type-0 / 1 item is a [hooks] step and fails the pin check | `runtime/mod_difficulty.c`, `mod_difficulty_*_test.c`, `mod_item_cases.h` | by hand (tasks 7, 16, 23 review) | hooks, ini |
| M21 | Player-motion data hooks: 0x40d810 (DLL 0x2df50, range kinds 54..83, table DLL 0x2ebb8) and 0x40dd70 (DLL 0x2ec30, frame ops 50..80, tables 0x2f02c / 0x2f0a8) | `runtime/mod_params.c`, `mod_params_cases.h`, `mod_params_test.c` | by hand (task 11) | hooks |
| M22 | Native DLL oracles: the shipped DLL's handler bytes run natively on x86-64 in isolation (task 7, 11, 16 cross-checks; their transcript hashes are in the selftest) | `tools/native_params_oracle.c`, `native_item_oracle.c`, `native_difficulty_oracle.c` | they EXECUTE DLL code: re-running them needs the owner's OK | hooks |
| M23 | The script system call table (VM directory slot 3, 114 entries: name, flags, DLL handler), built at load | `runtime/mod_syscalls.c` `entries[]` | transcribed by hand from the builder DLL 0x15120..0x15cd1 (table global 0x817f8; names from the DLL's exports, four duplicated); re-read statically by the checker | syscall-table |
| M24 | The 18 implemented script calls (3 ConvertPointer, 0 Set_Action, 1 AddExp, 8 MapJump, 13 AddMP, 15 GetAttackParam, 33 SheetReload, 34 PadIsTrigger, 35 PadIsButton, 46 AbilityCount, 47 GetGrowthTable, 48 GetCurrentRC_MEM, 55 SetCameraMode, 69 WriteByte, 78 ItemGetNumBackyard, 80 PlayerParamApply, 87 SetDriveMultiplier, 93 PartyJump); the other 96 are named fatals | `runtime/mod_syscalls.c` (`case N:`), `mod_syscalls_test.c` | by hand from each DLL handler (tasks 6, 7, 23) | scripts |
| M25 | The 0x1f9a80 detour (DLL 0x2d110: installs table 3, tail-jumps to exe 0x3e1620) and the drive-gain hook 0x3a9cf0 (DLL 0x327e0, `drive_add_mult` 1.0) | `runtime/mod_syscalls.c` `f_1f9a80`, `f_3a9cf0` | by hand (tasks 6, 23) | hooks, ini |
| M26 | Option guard: `retry`, `savepoint_qol`, `patch_gummiskip` read with the DLL's boolean reader (0x2a840: on / true / 1); on = named fatal `mod-option-not-ported`. `dll/tweaks/special_command_menu.bin` or `dll/KH2_Tweaks_SpecialCommandList.ini` present = start-up fatal | `runtime/mod_hooks_b.c` `kh_mod_b_check_options` | by hand (task 23) | ini, hooks |
| M27 | Gummi gate: a request for the gummi task (exe 0x1505a0 with 0x716c10) is the named fatal `gummi-gate-contract` (the mod's `patch_gummiskip` is off; the port removes the ship by data, M3 / M4) | `runtime/mod_hooks.c`; Makefile `PHASE_HOOKS` 1505a0 | by hand (task 8) | gummi |
| M28 | The DLL's own data file `dll/tweaks/command_menu_offsets.bin` (16-byte header, count at +4, 29 entries of 16 bytes), read at run time from the tree for the icon hooks | `runtime/mod_hooks_b.c` `kh_mod_b_load_command_menu`; selftest pin in `mod_hooks_b_test.c` (29 entries, first "bb0", bytes eb / ec) | the file itself (baked into the tree) | tweaks-data |
| M29 | `dll/tweaks/shopface.bin` (the shopface_fix hook's path builder names it; the game loads it) and `specialcommand.bin` (never opened) | baked into the tree | the files themselves | tweaks-data |

## 4. The ini files (keys and values as the DLLs read them)

Semantics (GetPrivateProfileStringA, as the DLL calls it; implementation notes section 8): first occurrence of a key wins, value trimmed,
an inline `#comment` is part of the value (so `antiform_tweaks=on #Makes ...` is NOT "on" for the boolean reader: the 9 AntiForm
edits stay inactive), `;` lines are comments. The checker classifies every key of the pinned version (fingerprint
`ini[file] = [section, key, value, class, first]`); the classes say where a changed value lands:

| # | class (keys) | what the port does with the value | step |
|---|---|---|---|
| M30 | **static-edit** (28 keys: blow_gravity, remove_modifier_attack_flag, aerial_dodge_patch, disable_genie_tink_special, enable_negative_stats, the five *_growth, dualwield_forms, disable_drive_restrictions, uncap_form_level, apply_pridelands_patches, camera_patches_outdoors / _indoors, patch_limits, shop_restrict_fix, disable_subjective, apply_worldsymbol_patch, symbol_zz/es/di/po/wi/tr, and the three AntiForm keys of the inactive edits) | the DLL's DllMain patches the exe from it: the value is baked into M8 / M9 / M11 | edits, gen |
| M31 | **port-constant** (18 keys: the eight *_party_dmg / *_enemy_dmg, four *_ap_mult, six standard/proud/crit_hp_mult / _mp_mult; plus dualwield_forms and drive_add_mult; and beginner_hp_mult / beginner_mp_mult, ABSENT from the ini: the port's beginner HP / MP 1.0 is the DLL's own default, loader DLL 0x21090 `movss xmm7,[0x75aa4]` = 1.0 passed to the float reader 0x2ad10) | compiled into `runtime/mod_config.h` `kh_mod_difficulty_config` (enemy 50/100/200/200, party 150/100/125/125, AP 1/1/1.5/1.5, HP and MP 1/1/0.5/0.5), `runtime/mod_syscalls.c` (drive multiplier 1.0f), `runtime/mod_static_sites.c` / `mod_hooks_a.c` (forms 0,2,3,0,5,0); the damage / rewards / dual-wield selftests and the native oracles carry the same numbers. The values come from the option LOADERS (DLL 0x20590 damage, 0x21090 multipliers, 0x22440 drive, the option initialisers), which no handler reaches: fingerprint `ini_keys` (named or not, sites, reading functions, decoded default) and `ini_key_functions` (raw / norm / deep hashes) | ini |
| M32 | **detour-gate** (patch_ability_categories, solo_summon_fix, shopface_fix, drive_add_mult, dualwield_forms) and **detour-gate:guarded** (retry, savepoint_qol, patch_gummiskip) | the option initialiser attaches detours (M15); guarded = not ported, stopped by M26 | hooks |
| M33 | **moose-runtime** (KH2Moose.ini: [kh2.cache] size 524288, player 32768, friend 32768; [kh2.data] size 131072; [kh2.fixes] scd_priority 1; [kh2.hacks] fast_title 1, attempt_continue_after_exception_... 1) | read at RUN time from `<tree>/dll/KH2Moose.ini` by `runtime/moose.c` (no rebuild; by its EXACT name, without `kh_resolve_ci`: under another letter case the port prints "no ini" and runs with vanilla pools, the PC does not; the checker makes a case-only rename loud, `EXACT_NAME_USERS`); scd_priority bit 2, no_death, test_crash are named fatals | ini |
| M34 | **read-no-effect** (10 keys: global_gravity, CursorRemain, debug_on, turbo_speed, soft_reset, set_level_cap, multiple_shortcuts, worldpoint_amount (`on` is not a number: the DLL's stoi wrapper falls back to 54), halve_gummiship_plans, halve_tinyship_plans) | the DLL reads the key, but at the pinned value nothing reached the edit list or the port; a new value may patch the exe or switch on code. debug_on / soft_reset / multiple_shortcuts were left out as out of scope with NO guard: the checker shouts when one turns on | edits |
| M35 | **not-read** (7: begginer_hp_mult, begginer_mp_mult (the DLL reads `beginner_*`: absent, its default 1.0 applies), submenu_memory, menu_memory, repoint_and_extend_btlobj_sheet, change_custom_magic_mpdrive_status, `#dualwield_forms`) | no effect in this DLL build; the checker re-tests the key against a new DLL's strings (the 2026-08-15 DLL DID read repoint_and_extend_btlobj_sheet and attached three detours for it) | edits, hooks |

## 5. KH2Moose.dll

| # | item | where | made by | step |
|---|---|---|---|---|
| M36 | Pool sizes and the four re-encoded pool instructions: cache 512 MiB / data heap 128 MiB / reserved blocks (+32 MiB player and friend), hand-written equivalents of exe 0x39d590, 0x39dab0, 0x3e5f30, 0x3e5f90 and wrappers of 0x39e260 / 0x39e270; the byte patterns the DLL scans for | `runtime/moose.c`; Makefile `MOOSE_HOOKS`; tools/mod_dll_slots.py `AVOID` (bytes Moose rewrites) | by hand (task 17: DLL 0x1d60 `_aligned_malloc(size, 32 MiB)`, 0x3470 block increases) | moose |
| M37 | The two ini-switched code patches: scd_priority bit 1 (exe 0x927e2 call -> `xor eax,eax` + NOP; DLL 0x3050, pattern 0x44038) and fast_title (exe 0x2b46eb -> `mov edi,0x11`; DLL 0x1fba, pattern 0x440a0); encoder entry DLL 0xb640, NOP table 0x495a0 | `gen/mod_moose_edits.json`, `gen/mod_moose_table.h`; re-lifted `f_92680`, `f_2b4590` (Makefile `MOD_MOOSE_FUNCS`); `runtime/moose.c` picks the body | `tools/gen_mod_moose.py --dll ...` (templates at fixed DLL addresses: stops on another build) | moose, gen |
| M38 | The exception handler (`attempt_continue_...`; DLL 0xc670 registers 0xbbf0) decoded, NOT emulated: the fault report says what the PC would have done | `runtime/moose.c` header, `kh_moose_fault_note` (`runtime/core.c`) | by hand (task 23) | moose |
| M39 | Differential test of the two re-lifted Moose functions | `gen/mod_moose_difftest*.json` | `make mod-moose-difftest` | gen |

## 6. NeverlandFly.dll

| # | item | where | made by | step |
|---|---|---|---|---|
| M40 | Five signatures (patterns at DLL 0x91e0 / 0x9200 ...), six edits / 54 bytes in exe 0x424570 (swim / flight motion ids, a 13-byte cave), 0x306400 (menu model y offset -25.0), the row-step thread (exe 0x15b962 immediate 22 / 28 every 250 ms, DLL 0x1290); "flytest" banner | `gen/mod_neverland_edits.json`, `gen/mod_neverland_table.h`; `runtime/mod_neverland.c`; site handler `kh_mod_site_15b960` in `runtime/mod_static_sites.c`; Makefile `MOD_NEVERLAND_HOOKS` (424570 306400 15b6d0) | `tools/gen_mod_neverland.py --dll ...` (the DLL's own code is DLL 0x1000..0x17cb) | neverland, gen |
| M41 | Differential test of the three re-lifted functions | `gen/mod_neverland_difftest*.json` | `make mod-neverland-difftest` | gen |

## 7. Scripts and other data

| # | item | where | made by | step |
|---|---|---|---|---|
| M42 | The table-3 calls the mod's scripts use: 16 traps (3 8 13 15 33 34 35 46 47 48 55 69 78 80 87 93), 167 uses in 22 files (17 player-object AIs, NPC objects, area files tt01 / tt28); `dona` in `obj/P_EX020.mdlx` is Roxas' controller | fingerprint `scripts.uses`; implementation notes section 3 | `tools/bdxscan.py` (entry / callback / sweep / raw passes) | scripts |
| M43 | The cutscene stub mode: the mod's identical 56-byte empty scene (670 of 1,110 area event entries) is the template `KHNMC_EVENTS=stub` copies from the tree; seven scenes with their own music commands | `runtime/events_stub.c` (reads the template from `KHNMC_DATA`; its tree check runs with `KHNMC_DATA=<tree>`) | by hand (task 32) from the area files | events |
| M44 | Event / music analysis of the tree (which events can start, what each names, music after a stub) | `tools/event_refs.py`, `tools/music_compare.py`, `runtime/music_log.c` | task 32 | events |
| M45 | `00progress.bin`: the mod rewires 12 subfiles (world scripts, wlds, wldf, link) | docs/mod/MOD-HOOKS.md; M3 depends on its layout | analysis (task B1) | gummi, mechanics |

## 8. Routes, inputs, reports

| # | item | where | made by | step |
|---|---|---|---|---|
| M46 | The state-driven routes follow the NMC story: maps eh02 / eh03 (missions EH03_INTRO1/2), Twilight Town tt01 / tt02 / tt07 (TT07_MS101, MS103), Less Than Nothing item drain | `input/route-story.txt`, `route-story-nocutscenes.txt`, `route-leave-eh02.txt`, `route-field-battle.txt` | by hand (tasks 27, 29, 30) | verify |
| M47 | Route conditions that read mod state (item observer of f_3c4b80, field-command counter of the 3f29e0 hook, the title timing that `fast_title` shortens by ~750 presents) | `runtime/route_state.c`, `runtime/null_input.c`, `runtime/phase_log.c` | by hand | verify |
| M48 | Report parsers keyed to mod lines (`kh: mod neverland: 5 signatures verified ...`, `kh: moose: ... cache C MiB`, damage slot lines) | `tools/route_layers.py`, `tools/route_one.py`, `tools/route_table.py` | by hand | verify |
| M49 | The Mac build copies `gen/` (GEN_PREBUILT=1: the mod tables and the re-lifted file are inputs there) | `tools/darwin_build.sh`, Makefile `GEN_PREBUILT` | - | subset, verify |

## 9. Selftest pins (counts that move with the mod)

| # | pin | where |
|---|---|---|
| M50 | Total 169,449 checks, 0 failures (merged build of task 32) | `KH_EXE=$PWD/pc/kh2fm.exe ./khnmc --selftest` |
| M51 | mod static: edit counts by class, 30 re-lifted functions (`kh_mod_static_fn_count==30`), buffer at the vanilla SizeOfImage, 4 KiB | `runtime/mod_static_test.c` |
| M52 | mod neverland: six edits (`n==6`), five signatures and their hit counts | `runtime/mod_neverland_test.c` |
| M53 | mod B: shipped command-menu table (29 entries), 0x3f29e0 / 0x3f27d0 table cases, option-reader cases | `runtime/mod_hooks_b_test.c` |
| M54 | difficulty: damage / rewards / items / caps expectations computed from the ini numbers (M31) | `runtime/mod_difficulty*_test.c`, `mod_item_cases.h`, `mod_params_cases.h` (+ transcript hashes `5d45fbf33da7e1c1`, `73471e28cc7fa6ca`) |
| M55 | moose: ini rules, pool sizes, both code patches; syscalls: 114 entries, the 18 implemented | `runtime/moose.c` selftest, `runtime/mod_syscalls_test.c` |

## 10. Documents written from this version

| # | document | what of the mod it describes |
|---|---|---|
| M56 | `docs/mod/MECHANICS.md` (repo) | the PC mod's mechanics (scripts, DLL options, data) against the PS2 patch file; 133 PS2 features mapped |
| M57 | `docs/mod/MOD-HOOKS.md`, `docs/mod/detour_semantics.cpp`, `docs/difficulty/DIFFICULTY-REPORT.md`, `docs/difficulty/MESSAGES.md`, `docs/DATA-COMPARE.md`, `docs/DATA-REPORT.md`, `docs/G1-GATE.md` (repo) | code layer summary, difficulty system and its texts, PS2 vs PC data |
| M58 | The job reports `STATUS-*.md` (A12 static edits, C17 heaps, C Task 11, E16 items, E19 Neverland, H23 detour audit, N29 item route, P30, R32 cutscenes) and measurements (counts, addresses) | the decode of record for each item above |

## 10a. The gummi tooling and the checker's own constants (added after the task 34 review)

| # | item | where | made by / depends on | step |
|---|---|---|---|---|
| M59 | The gummi overlay: puts M3 / M4 into a baked tree (`00progress.bin` and `ard/{us,fr,gr,it,sp}/wm00.ard`), only over the input it was made from; `IN_PROG` = sha256 of the pinned mod's `00progress.bin` (`cdba15a9...`), `IN_WM00` = the vanilla `wm00.ard` (`32a8a76a...`); exits 1 on REFUSED / absent | `tools/apply_gummi_overlay.py` (worktree: a fixed copy of the repository's, which printed REFUSED and exited 0; `--in-prog` / `--in-wm00` / `--langs`) | by hand (task 32 era); the re-pin's pin check requires `IN_PROG` = the pinned mod's `00progress.bin` (and `IN_WM00` = a shipped `wm00.ard`) | gummi, repin |
| M60 | The gummi edit generators | repository `tools/gummi/make_gummi_removal.py` (`PROG_IN` = the pinned clone's path, `WM_IN` = `scratch/S6-gummi-data/src/us/wm00.ard`), `wldf_decode.py`, `spawnparse.py`, `progress_diff.py` | by hand (batch B1); copied and re-pointed for a new version (MOD-UPDATES step gummi) | gummi |
| M61 | The gummi removal's decode of record | repository `docs/gummi/GUMMI-REMOVAL.md` (task gate 0x1505a0, WLDF entry 53, `m_51`) | analysis (batch B1) | gummi |
| M62 | The checker's build-specific constants | `tools/mod_update_check.py` `DETOUR_PORT`, `INLINE_SITES`, `GUARDED_OPTIONS` / `GUARDED_SITES`, `OUT_OF_SCOPE_OPTIONS`, `CONST_KEYS`, `MOOSE_KEYS`, `MOOSE_CITED` / `MOOSE_DATA` / `NEVERLAND_OWN_END` with `CONST_BUILD` (the DLL sha256 they were read from), `ITEM_USE_TYPES` / `ITEM_KINDS_UNPORTED`, `EXTRA_PROBE_RE`, `EXACT_NAME_USERS`, `DEFAULT_EDITS`, `SLOTS_FILE_DEFAULT` | by hand from the audits above; on another build the re-pin finds the cited Moose / Neverland functions again by code and FAILS on one it cannot find; MOD-UPDATES "repin" lists them all | repin |

## 11. Inputs outside the worktree

`~/projects/SillyBillyMod1` (Makefile `MOD_TWEAKS_DLL`, `MOD_MOOSE_DLL`, `MOD_NEVERLAND_DLL`; `tools/mod/*.py`, `derive_edits.py`
and `make_gummi_removal.py` have the clone path written in); `~/projects/kh2-nmc-analysis/scratch/mod-gummi/` (edit list, gummi
edits), `scratch/S6-gummi-data/src/us/wm00.ard` (the vanilla input of M4), `scratch/S5-mod-hooks/` (the emulator, scan
shortcuts, ini variants), `scratch/universe` (the function universe); `~/projects/khnmc-decomp-edition/tools/{bake_nmc_tree.py,
mod/, gummi/, apply_gummi_overlay.py, khlift}` and `docs/gummi/`; the lifter venv `scratch/S2-lifter/venv`.

## 12. What is NOT mod-dependent

The lifted exe and everything about it (`lifted/`, `lifter-rt/`, the runtime's Win64 layer, graphics, audio, threads, clocks, the
id-table guard, the background-load lock, the HD package tables, the profile-A pruning METHOD). These are the port's technical
improvements; they stay when the mod changes (docs/MOD-UPDATES.md, rule 1).

## 13. The fingerprint (`mod/nmc-version.json`)

Written by `tools/mod_update_check.py --write-fingerprint <clone> [--slots-file F]`; schema 2 (after the task 34 review; the tool
refuses a schema-1 file); 724 KB of JSON; no game or mod content (hashes, sizes, counts, addresses, ini keys and values, file
names). Integers are decimal (RVAs too). Added in schema 2: `case` (canonical name -> the checkout's letter case, for the DLLs,
`command_menu_offsets.bin`, `03system.bin`), `item_kinds` (`03system.bin` `item`: version, record count, `by_type` = {type:
{effect kind: count}}), in the Tweaks record `ini_keys` and `ini_key_functions`, in each inline detour row `slot` and
`site_add` (derived), in the Moose / Neverland records `located` and `problems`, in `port` `makefile_slots_expect`,
`drive_multiplier`, `command_menu_pin`, `gummi_overlay`. Function hashes (`norm`, `deep`) now follow leaf routines without
`.pdata` (a call, jump or `lea` to one names it `F` and reaches its code).

* `mod`: repository, commit, date, subject, branch, local modifications of the working tree (14), the checkout path.
* `files`: `count`, `bytes`, `assets`, `by_top`, `list` = one row per file `[path, size, sha256, sha256 of the LF-normalised text
  (text files only) or null, "A" if it is a plain copy asset of mod.yml]`.
* `mod_yml`: asset count, methods, `irregular` (assets that are not "copy the file of the same name": none), top-level keys,
  `asset_list_sha256` (canonical list).
* `dll_data`: `command_menu_offsets_entries` (29).
* `dll[path]`: sha256, size, link time, number of `.pdata` functions. For KH2Tweaks_Refactor.dll: `detour_attach` / `_ex` (0x35d00 /
  0x35d40), `installer` (0x33930), `detours` = 42 rows {kind, site, slot, handler, func (the initialiser), gate (installer or the
  option key), ini, exe (slot resolved by tools/mod_dll_slots.py's scanner reader, or the inline site), handler_fn}, and
  `syscalls` = 114 rows {index, handler, flags, names, sha256, norm, deep}; `syscall_builder` 0x15120, `syscall_table_global` 0x817f8.
  For KH2Moose.dll and NeverlandFly.dll: `cited` (the functions the ports were decoded from) and Moose's `data` blocks.
  Every function record: `begin`, `bounds` (`pdata` = from the DLL's unwind table, chained fragments included; `leaf` for the two
  inline handlers, which have no unwind entry: to the first int3), `fragments`, `size`, `sha256` (raw bytes), `norm` (normalised
  instruction text: addresses replaced by what they name), `deep` (over every DLL function it reaches), `reach`.
* `ini[path]`: `[section, key, value, class, first]` per key line (section 4).
* `scripts`: files scanned, scripts, `traps_used`, `files_using`, `uses` = per file {trap: [entry, callback, sweep, raw]}.
* `static_edits`: source file, exe sha256, summary, buffer size and hash, `edits` / `inactive` = {rva, len, cls, section, sha256 of
  old and new bytes, ini keys}.
* `port`: the Makefile lists, the implemented syscalls, the difficulty table, the slot expectation file's hash, the gen/ sources of
  Moose and Neverland, `pinned_counts`.
* `pin_checks`: disagreements between the fingerprint and the port's records found when it was written (empty for 79a23a6:
  every active detour maps to the port's table and the inline sites land on the spliced addresses, every active slot is in the
  expectation file and equals it and the Makefile rule names that file, the syscall table equals `entries[]`, the difficulty
  constants and the drive multiplier equal what the DLL reads (the ini, or the DLL's decoded default for the absent
  `beginner_hp_mult` / `beginner_mp_mult`), gen/ was made from these DLLs, Moose's / Neverland's cited functions were found, every
  used script call is implemented, the command-menu count equals the B selftest's pin, the gummi overlay takes this
  `00progress.bin`, no item-use record has an unported effect kind).
