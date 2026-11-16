# Project Nobody May Cry (NMC) - PS2 executable patch analysis
Target: KH2 Final Mix (JP) SLPM_666.75, MIPS R5900, base 0x100000. 50 patch groups (ids 0-49, 250 bytes in place), injected 3rd LOAD (vaddr 0x1e50000, filesz 0x400, memsz 0xc8000), 3rd program header. Each unit was analysed, then re-checked by a verification that diffed orig vs nmc and re-decoded the words; the verification's corrected reading is used. Static analysis only; nothing was built or run.

## 1. TL;DR
The ELF patch is NOT the combo / Roxas-moveset / style-meter logic. None of the 50 patches holds combo-speed constants, a style-meter routine or moveset changes. It installs a mod switch plus a file-overlay layer, with UI/text/camera tweaks around it. Gameplay content presumably lives in overlay files named `_CCzDA/<original path>` on the disc image; those are not in the ELF, so that link is an inference.
Mechanism:
1. Boot hook (patch 22, tail of FUN_001b13d8): looks up marker file `#CCzDA` via the engine file index. Only if present, it runtime-patches wrapper FUN_002b6bc4 so a 3-state config option (bits 1-2 of savedata+0x41a4) becomes the NMC on/off switch, and cuts that option's choice count 3 -> 2 (halfword 0x373f9c).
2. Switch = bit0 of flag word 0x35ecd8. When set, FUN_001ae460 (patch 21) tries `_CCzDA/<name>` first, then the vanilla name.
3. A third menu item (text id 0x8a81) is added to a 2-item menu (patches 28-34, 36-39, 48, 49 + injected routines). Selecting it sets a sticky flag at 0x1e50134, which swaps message id 0x8a8a and redirects one setter call (patch 35).
4. Per-world/gumi idx buffer is pinned at 0x1efbc00 inside the new segment's zero-fill (patches 17-20).
5. Ten patch ids force REGION::Get() to constant 2 (US) at 12 call sites in 6 functions (8, 9, 24-27, 41-44).
6. Cosmetic/UI: font width metrics (10-15), default text scale 1.0 -> 0.75 (45), four 512x35 strips width 0 (4-7), a UI sprite 35 px lower (16), English save titles (46, 47), camera projection tweak (0-3), extra bit in a menu filter mask (40), mode-6 special case suppressed (23).
Systems touched: file index/loader, config/savedata, menus/title, text/font layout, HUD sprites, camera projection, spawn setup, region handling. Not touched: combat, animation speed, damage, stats.

## 2. Patch table (id | vaddr | function | subsystem | effect | verdict/confidence)
Confidence = verification. "mech" = byte-level mechanics verified; intent mostly inferred.
- 0 | 0x106e10 | FUN_00106de8 | camera | drops lwc1 of cam[+76]; starts building 0x3f4ccccc | corrected; medium (mech high, intent low)
- 1 | 0x106e28 | FUN_00106de8 | camera | $f31=0.8 (0x3f4ccccc); mode-0 numerator = (512/0.8)*0.5 = 320 regardless of cam[+76] | same
- 2 | 0x106e54 | FUN_00106de8 | camera | $f30=0.75 scratch | same
- 3 | 0x106e70 | FUN_00106de8 | camera | stores 0.8*0.75 (~0.6) to cam[+76] after each mode-0 call; later readers (mode!=0 branch, other camera code) see 0.6; clobbers $at/$k1/$f30/$f31 unsaved | same
- 4 | 0x14ad80 | FUN_0014ad58 | hud-ui | width of strip obj 0x3495c0 (0,0) 512->0, height 35 kept | corrected; medium (that width 0 hides it is unproven; "letterbox" is geometry-only)
- 5 | 0x14ada8 | FUN_0014ad58 | hud-ui | strip 0x3496d0 (0,381) 512->0 | same
- 6 | 0x14add0 | FUN_0014ad58 | hud-ui | strip 0x3497e0 (0,0) 512->0 | same
- 7 | 0x14ae00 | FUN_0014ad58 | hud-ui | strip 0x3498f0 (0,381) 512->0 | same
- 8 | 0x1aab54 | FUN_001aaa08 | hud-ui | REGION::Get -> li $v0,2 (region==1 shortcut dead) | corrected; medium
- 9 | 0x1aab68 | FUN_001aaa08 | hud-ui | 2nd REGION::Get -> 2; flag stays 0, so 0x1c7cc60 table override (bytes 32/24, word 0x350a30=32) always runs | corrected; medium
- 10-12, 15 | 0x1ab8fc, 0x1ab934, 0x1ab968, 0x1aba04 | FUN_001ab890 | hud-ui | font table entries 0,1,2,5 byte +0x10: 18->13 | corrected; medium (data-only; purpose unverified)
- 13, 14 | 0x1ab99c, 0x1ab9d4 | FUN_001ab890 | hud-ui | entries 3,4 byte +0x10: 24->18 | same
- 16 | 0x1ac8d8 | FUN_001ac8b0 | hud-ui (2D sprite) | 7th ctor arg (obj+8 = screen Y) 365->400: sprite 35 px lower | corrected; medium (which element unknown)
- 17 | 0x1ade74 | FUN_001adcf8 | loading | lw count -> lui $13,0x1f0 | corrected; medium
- 18 | 0x1ade90 | FUN_001adcf8 | loading | and -> addiu $5,$13,-0x4400: per-world idx buffer and ptr3 global fixed at 0x1efbc00 | corrected; medium
- 19 | 0x1adec8 | FUN_001adea8 | loading | same as 17 for 000gumi.idx | corrected; medium
- 20 | 0x1adee8 | FUN_001adea8 | loading | same as 18; gumi idx buffer at 0x1efbc00 | corrected; medium
- 21 | 0x1ae460 | FUN_001ae460 | loading | entry hook to 0x1e50050: if bit0 of 0x35ecd8, look up `_CCzDA/`+name, fall back to vanilla | confirmed; high
- 22 | 0x1b13e4 | FUN_001b13d8 | injected-code | jr $ra -> j 0x1e50208: boot installer | corrected; medium
- 23 | 0x1d09f8 | FUN_001d0980 | spawn-ai | bne -> j 0x1d0a2c: mode==6 special init (slot 29, FUN_00192048(0xc44f)) dead; mode 6 uses generic FUN_001c11a8(29) | corrected; medium (purpose unknown)
- 24, 25 | 0x24738c, 0x2473a0 | FUN_00247198 | hud-ui | both REGION::Get -> 2; always message-then-number layout | corrected; medium
- 26, 27 | 0x2476ac, 0x2476c0 | FUN_00247580 | menu | both REGION::Get -> 2; label order "<msg> <n>" | corrected; high
- 28 | 0x259e94(+0x259ea4) | FUN_00259e90 | menu | item table 0x372ed8 -> injected 0x1e5016c (3 records) | confirmed; high
- 29 | 0x259ed4-0x259eec | FUN_00259e90 | menu | loop var -> byte offset; item-1 hide test now offset==36; hide path skips without bumping slot counter | confirmed; high
- 30 | 0x259f14 | FUN_00259e90 | menu | addiu $17,36 -> nop (moved to delay slot) | confirmed; high
- 31 | 0x259f58-0x259f60 | FUN_00259e90 | menu | loop bound 2 -> 3 items (offset<72) | confirmed; high
- 32 | 0x25a12c-0x25a138 | FUN_00259fb8 | menu | jump-table bound 6->8; sll -> jal 0x1e501d8 remap | corrected; medium
- 33 | 0x25a1a8 | FUN_00259fb8 | menu | case-0 branch goes to 0x25a1c4 (flag clear) | corrected; medium
- 34 | 0x25a1c4 | FUN_00259fb8 | menu | stores 0 to flag 0x1e50134 | corrected; medium
- 35 | 0x25cd10 | FUN_0025ccf0 | injected-code | jal FUN_00103c80 -> jal 0x1e5014c: if NMC flag, FUN_0014a138(1) instead | confirmed; medium (mech high, effect unknown)
- 36 | 0x25d580 | FUN_0025d560 | menu | FUN_0028b040 wrapped (0x1e500ec): index 1->2 when [0x35eeb0]==2 | corrected; medium
- 37 | 0x25d60c | FUN_0025d560 | menu | tail -> j 0x1e50138: msg id -> 0x8a8a if NMC flag; affects BOTH mode 19 (0x8a82) and mode 12 (0x8aa1) | corrected; medium
- 38 | 0x25d650 | FUN_0025d620 | menu | nop -> li $15,2 | corrected; medium
- 39 | 0x25d670 | FUN_0025d620 | menu | beqz $17 -> bne $17,$15: in mode 0 the [0x35eeb0] gate now applies only when a0==2 (was a0!=0); a0!=2 returns 1 | corrected; medium
- 40 | 0x282188 | FUN_00281fe0 | menu | lui 0x10 -> 0x30: category 14 (and 0) mask gains bit 21 (0x200000) | corrected; medium
- 41 | 0x28de40, 0x28de54 | FUN_0028de08 | hud-ui | both REGION::Get -> 2; '?' mask always 5 chars | corrected; medium
- 42 | 0x28df38, 0x28df3c, 0x28df4c | FUN_0028def0 | hud-ui | same as 41 for sibling | corrected; medium
- 43 | 0x2a8648 | FUN_002a8330 | hud-ui | REGION::Get -> 2 | corrected; medium
- 44 | 0x2a865c | FUN_002a8330 | hud-ui | REGION::Get -> 2; only reorders msg 0x4b80 vs number in type-2 rows | corrected; medium
- 45 | 0x36ce94/98/9c | data (text scale defaults) | data | 3 x f32 1.0->0.75; only 0x36ce98/9c live (0x36ce94 has no callers); [+4]*[+8] product = 0.5625 | corrected; medium
- 46 | 0x3715b8 | data (save title) | data | SJIS 'Kingdom Hearts II FM' -> 'KH(II) Final Mix' (bytes 87 73 = U+338F in CP932, presumably a glyph for II) | corrected; medium
- 47 | 0x371608 | data (save title) | data | 'System file' -> 'System Data' | corrected; medium
- 48 | 0x372ce0 | data (jump table) | data | entries 2-4 -> [0x25a184, 0x25a1b0, 0x1e50114]; table now {0x25a150,0x25a160,0x25a184,0x25a1b0,0x1e50114,0x25a1b0} | corrected; medium
- 49 | 0x372f44 | data (u16 msg table 0x372f40) | data | entry [2] 0 -> 0x0a88 (description msg for third item; value odd vs 0x8axx/0xb8xx ids) | corrected; medium, low on value
Notes: patches 8/9, 24-27, 41-44 = `jal 0x105c30` (REGION::Get = lw 0x33cafc) -> `addiu $2,$0,2`; REGION::Get itself unpatched. func_00105870: 2->'U', 1 and 8->'J', else 'E'. Menu patches 28-39, 48, 49 are one feature with the injected code.

## 3. Injected segment (0x1e50000)
LOAD #3: p_offset 0x281ef4, filesz 0x400, memsz 0xc8000 (ends 0x1f18000 = original 2nd LOAD), flags R+X. Code only to 0x27c; nothing referenced above 0x1e50400; rest is zero-fill (0x1efbc00 buffer lives there).
Map: A 0x1e50000 config sync; byte 0x1e5004c cached (config-1); B 0x1e50050 overlay lookup (patch 21); 0x1e500b0 relocated FUN_001ae460 prologue (`addiu sp,-16; j 0x1ae468; lui $15,455`); C 0x1e500ec FUN_0028b040 wrapper (36); D 0x1e50114 NMC menu action; data word 0x1e50134 NMC flag; E 0x1e50138 msg swap (37); F 0x1e5014c setter redirect (35); data 0x1e5016c 3x36-byte item records {0x301,0x8a7e},{0x300,0x8a80},{0x2ff,0x8a81} (first two copies of vanilla 0x372ed8); G 0x1e501d8 jump index remap (32); H 0x1e50208 boot installer (22).
H (boot installer; FUN_001b13d8's own store *(u32*)0x350e88=0x1c94100 still runs in delay slot):
  write "#CCzDA" to 0x3a7b18; if (orig_lookup(0x3a7b18)) { [0x2b6bd0]=jal FUN_00242a90 (config setter: 0->0, 2->2, else 1, bits1-2 @ savedata+0x41a4); [0x2b6bd4]=nop; [0x2b6be4]=addiu $6,0,1 (engine always gets 1); [0x2b6bf0]=nop; [0x2b6bfc]=j 0x1e50000; *(u16*)0x373f9c=2; }
  Code is rewritten in .text with no icache flush; FUN_002b6bc4 loses its frame setup and its return value becomes the config value.
A (config sync; reached only via the runtime patch, dead if marker missing):
  nc = FUN_00242a68()-1 /* (savedata[0x41a4]>>1)&3 */; if (nc != cache) { cache = nc; *(u32*)0x1c94094 = *(u32*)0x1c940a0 = *(u32*)0x1c940ac = -1; } FUN_00242e58(nc); /* D_0035ecd8 = (D_0035ecd8&~1) | ((nc&0xff)!=0) */
  With 2 choices: choice 0 = NMC on, choice 1 = vanilla; a stored 2 from an old save also turns NMC on. The option's identity ("Summon Effect", label 0xb71b) is a guess.
B (overlay lookup): if (FUN_00242e48() /*bit0 D_0035ecd8*/) { copy "_CCzDA/" to 0x3a7b18; FUN_002ff6f0(0x3a7b18+7, name, 0x40); e = orig_lookup(0x3a7b18); if (e) return e; } return orig_lookup(name);  Prefixed name is hashed whole, so overlay entries must exist in the idx tables. 64-byte copy is unbounded for long names (no NUL).
C/G (when [0x35eeb0]==2 the middle item is hidden): C: idx = FUN_0028b040(menu); if (idx==1 && g_35eeb0==2) idx=2. G: i=r+4; if (i>=2) { if (i==5) i = 3+(g_35eeb0==2); else i -= 2; } return i*4; table @0x372cd8 = {0x25a150,0x25a160,0x25a184,0x25a1b0,0x1e50114,0x25a1b0,0x25a3d8,0x25a3e8}. Side effects: status -4/-3 now reach 0x25a184/0x25a1b0 (instead of 0x25a150/0x25a160); status 2 reaches the NMC handler (probably unreachable).
D: FUN_00287bd0(19,0); FUN_0025ca78(); *(u32*)0x1e50134 = 0x1e50000; goto 0x25a160 (same as vanilla 0x25a1b0 plus flag). Patches 33/34 clear the flag on both vanilla paths; no injected code clears it otherwise.
E/F: E (shared tail of modes 19 and 12): if (flag) msgId = 0x8a8a. F (state==4 exit handler): if (flag) FUN_0014a138(1) /*sets byte +4 of obj at *0x32b96c*/ else FUN_00103c80(1) /* *(u32*)0x32ba2c=1 */. Mode 19 is the state the NMC action sets, so 0x8a8a is plausibly the NMC item's prompt; mode 12 likely collateral.

## 4. Decomp coverage (kh2-decomp)
21 patched functions; injected code is not in the decomp at all.
- Present in src (4): FUN_001adea8 (auto80_199_queue.cpp:5083, matches ORIGINAL logic), FUN_001ae460 (~5091), FUN_001b13d8 (tiny_auto80_solver.cpp:8135), FUN_0025ccf0 (tiny_auto80_queue.cpp:15465).
- Nonmatching only (5): FUN_00106de8 (auto.cpp ~74920, 320/324), FUN_0014ad58 (~19046, 176 vs 184), FUN_001ac8b0 (auto.cpp:34823), FUN_0025d560 (auto.cpp:52884), FUN_0025d620 (auto.cpp:52922).
- No decompiled body (12): FUN_001aaa08 (call stub only), FUN_001ab890 (declared only), FUN_001adcf8 (extern+call only), FUN_001d0980, FUN_00247198, FUN_00247580, FUN_00259e90, FUN_00259fb8, FUN_00281fe0, FUN_0028de08 (decl only, wrong (void) signature), FUN_0028def0, FUN_002a8330.
So 12/21 are not decompiled; 5 more are only near-matches. Data: D_0036ce98/9c declared in src/anon/auto200_999.cpp; title strings, jump table 0x372cd8 and u16 table 0x372f40 are not in src. Recovered callee names used: REGION::Get (0x105c30, tiny.cpp:371), YS::MINIGAME::GetScore (0x1d56c8), YS::MESSAGE::copy/dup/get_size, Tz::Select::*, Tz::MenuCursor::AttachSprite, dk::Sprite::*.

## 5. Porting notes
Constants/source edits:
- Camera func_00106de8 mode 0: replace read of a0[76] with 0.8f, then a0[76]=0.8f*0.75f (~0.6f). Caution: existing decomp ignores the tan result of func_002f8a18 and divides by reloaded cam[76], where the binary divides by the tan result; fix that first. Original: f=((512/cam[76])*0.5)/tan(fov*0.5); NMC makes numerator constant 320. Check other readers of cam+76. (D_0037a0b4/b8/bc really are 0x36a0b4/b8/bc = 0.5, 512.0, 416.0.)
- func_0014ad58: four func_0014b208(obj,512,35) -> width 0 (other code may resize/draw these; may not suffice).
- Region: use 2 in func_001aaa08, 00247198, 00247580, 0028de08, 0028def0, 002a8330 only; do not patch REGION::Get globally.
- Font table (func_001ab890): +0x10 = 13,13,13,18,18,13; reader func_001ab820 returns [+0x10]*2/3 (regions 1/8) else /3.
- func_001ac8b0: 365->400 (7th arg of func_001ac378); leave twin func_001ac858 alone.
- File index: ptr3/dest = 0x1efbc00 in func_001adcf8 and func_001adea8; reserve >= largest idx table (<= 0x1c400 bytes), keep memsz >= 0xc8000 or use an equivalent buffer.
- func_001d0980: drop the mode==6 block. func_00281fe0: 0x100000 -> 0x300000 in the shared tail (affects categories 0 and 14). func_0025d620 mode 0: `if (a0 != 0)` -> `if (a0 == 2)` around the [0x35eeb0] check. Data: text scale 0.75f; two 28-byte SJIS titles.
Hook rewrites (C++, no segment): globals g_nmcMarker, g_nmcStartFlag (was 0x1e50134), g_cachedCfg (was 0x1e5004c); lookup(name) with `_CCzDA/` retry (or VFS override; overlay files must exist in idx tables); config option with 2 choices and constant 1 to the engine; 3-item menu table + 3-iteration loop + remap + NMC case setting flag + flag clears + 0x8a8a selection + FUN_0014a138(1) vs FUN_00103c80(1). Do not copy the shifted jump table.
PS2 vs PC: this is a PS2-only binary patch of the ELF (hooks, injected segment, runtime code patching, overlay prefix). For this comparison the PC release mod uses DLL tweaks instead (not examined here). A decomp-based source port resembles the PC case: constants become source edits, hooks become plain C++, no runtime code patching or $f30/$f31 clobbering. The `_CCzDA/` overlay content (presumably the actual gameplay data) must be supplied separately.

## 6. Caveats / open questions
- Central gap: where Roxas moveset, combo speed and style meter live. Nothing in the ELF implements them; the "overlay files" explanation is inference from the lookup hook only.
- Intent is inferred for most patches; low confidence: camera purpose (0-3), strips (4-7), font metric (10-15), which UI element patch 16 moves, mode-6 suppression (23), mask bit 21 (40), msg ids 0x8a8a and 0x0a88.
- First-pass claims refuted/corrected: patch 39 direction (analyst had it inverted); patch 16 is screen Y, not a size/budget; FUN_001ab890 is a static-init routine, not just a table filler; patch 37 also affects mode 19; "letterbox" identity is geometry-only; FUN_00242a68 is a 2-bit getter, not a flag setter.
- Unresolved contradiction: the FUN_00259fb8 verification said the menu loop still iterates 2 records; patches 28-31 (confirmed, high, in the dedicated pass) give 3 iterations, which this report uses. Unverified whether slot-count/limit fields are raised to 3 elsewhere. Screen identity (title menu vs Continue-style prompt) not established.
- Unknown: meaning of [0x35eeb0]==2 (set to 2 by unnamed setter at 0x253568, called only from 0x2588d8 when halfword 0x36f06c==1; "no save data" is a guess), 0x373f9c, 0x1c94094/a0/ac, 0x32b96c/FUN_0014a138, state 19, and the texts of msg ids 0x8a7e/0x8a80/0x8a81/0x8a8a/0x0a88. 0xa88 looks like a possible patcher quirk.
- Region values: 2='U', 1 and 8='J' per decomp; what SLPM_666.75 actually returns at 0x33cafc was not read (if already not 1/8, region patches are runtime no-ops).
- dis.py drops R5900 3-operand `mult` lines (llvm-mc), shifting the asm column after the first one; several analysts were misled. Raw words and patches.json are correct; use per-word decoding.
- FUN_002ff6f0 assumed a bounded copy returning dest (routine B arithmetic relies on it); its MMI disassembly is garbled.

## Summary
1. The NMC ELF patch is a mod switch plus file-overlay layer, not combat logic: a boot hook checks for marker file `#CCzDA`, repurposes a 3-state config option as the on/off switch, and makes FUN_001ae460 try `_CCzDA/<path>` before the vanilla file; combo/Roxas/style-meter content must live in those overlay files (unverified).
2. The 50 patches split into: camera tweak (0-3), strips set to width 0 (4-7), font/text-scale/save-title UI (10-15, 45-47), a sprite moved 35 px down (16), file-index buffer pinned at 0x1efbc00 (17-20), the lookup and boot hooks (21-22), a mode-6 spawn special case disabled (23), a third menu item with flag-gated message/setter changes (28-39, 48-49, 35), a menu mask bit (40), and 10 patch ids forcing REGION::Get to 2 (8-9, 24-27, 41-44).
3. The injected segment (0x1e50000, 0x27c bytes of code, rest zero-fill to 0x1f18000) has 8 routines A-H plus three 36-byte menu records; it is only reached via hooks, with routine A live only after the runtime patch of FUN_002b6bc4.
4. Decomp coverage: of 21 patched functions, 4 are in src, 5 are nonmatching only, 12 have no decompiled body; none of the injected code exists in the decomp, and the existing func_00106de8 source is semantically wrong for the tan step.
5. Confidence is mostly medium (mechanics verified by diff, intent inferred); the report file could not be written because the harness refused it, so the full report is returned in this message.
