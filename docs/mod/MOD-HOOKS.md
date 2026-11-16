# SillyBillyMod1 code layer: what it changes in kh2fm.exe (batch B1, 2026-11-16)

Method: the mod DLL's own init was run in Unicorn against the static exe bytes with the shipped ini (the game and any lifted
program were never run). Tools: `tools/mod/` (`derive_edits.py`, `apply_edits.py`, `verify_edits.py`, `emu_rerun.py`).

- 126 differing regions / 403 bytes = 35 hook sites (37 regions, 217 bytes) + 89 static edits (186 bytes).
- Static edits apply to the file image BEFORE lifting; 87 are file-backed, 2 (script-table pointers at 0x7534a8/0x7534ac) are host-applied
  at load. The 8 rip-relative edits point at a 4 KiB RW buffer the DLL allocates; the patched image appends a `.nmc` section for it.
- As shipped: the 9 AntiForm patches are NOT applied (inline '#' comment reads false; `--with-antiform` exists), `worldpoint_amount=on`
  is a no-op (stoi wrapper falls back to 54), the `begginer_*` HP/MP keys are ignored; Proud HP/MP 0.5x and Proud AP 1.5x are real (detours).
- All 266 byte signatures reproduce their hit counts/RVAs on the pristine exe. Signature scans must run BEFORE patching
  (edit 0x3b1789 rewrites the scanner's own anchor bytes).
- Detours decoded (35 total): dual-wield predicates 0x2fc860/0x2fc4e0 (forms {0,2,3,5}), script-event wrappers 0x3d60c0/0x3d61d0/0x3a89a0
  (run BDX triggers via script VM entry 0x3e1840), consumable-effect extension hooks (dormant for NMC), 0x4060e0 custom event ops (dormant),
  0x405440 map-event types 0x7f/0x3e8/0x3e9, 0x3f27d0 (neutral), 0x3f6510 (idle). Undecoded/partial: per-kind item formulae, the else
  branch of 0x3f29e0, a sign quirk in 0x3c4b80, UI hooks 0x31dd70/0x15aba0/0x15aab0/0x153850. Semantics sketch: `detour_semantics.cpp`.
- Script syscalls (114 added to VM table index 3 at 0x41b400): PS2 NMC data uses 0 of them; the PC repo uses 12 (160 uses in 22 files:
  17 player-object AIs, 3 NPC mdlx, 2 ARD maps tt01/tt28). No msn file uses any. Four duplicated export names make index->name ambiguous.
- Skipped as out of scope: KH2Moose, retry, savepoint, debug, multiple_shortcuts, gummi-skip option (replaced by our data edit).
- 00progress.bin: 12 subfiles differ from vanilla (9 world scripts, wlds, wldf, link): story-flag rewiring that creates the NMC arena
  structure (inference, medium confidence).
- Port plan: apply static edits to the image before lifting; 35 detours become function-table replacements that call the lifted
  original; replacement handlers are rewritten as clean C++ and diffed against the lifted original.
