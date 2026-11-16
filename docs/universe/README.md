# Canonical function universe for `kh2fm.exe`

Input: `pc/kh2fm.exe` (PE32+ x64, owner's own copy, sha256 starts `9002b2de6a1f91a7`, image base `0x140000000`).
Everything here was derived by static analysis only (pefile, capstone, struct). Nothing was executed and nothing derived from the
game leaves `scratch/`.

**Result: 38,455 functions** = 17,908 merged `.pdata` functions + 20,547 leaf functions that have no `.pdata`.
94.23% of all executable bytes lie inside a function extent; the rest is classified (int3/nop padding, CLR IL method bodies, IL alignment
pad) with **zero unknown bytes**. No two function ranges overlap. `validate.py` re-checks all of it against the exe independently.

All RVAs are image-relative. TSV: hex strings (`0x1000`). JSON: integers. Sizes are bytes. `VA = RVA + 0x140000000`.

## Files

| file | content |
|---|---|
| `functions.tsv` / `functions.json` | one row per function, the authoritative table (columns below). JSON carries a few extra fields (`imports`, `data_in_code`, `unreached_ranges`, `fragments`, `cls`) |
| `ranges.tsv` | one row per address range of every function (48,073 rows): `func_id start_rva end_rva size role section pdata_entry_rva chain_parent_rva` |
| `gaps.tsv` | every executable byte run that is not in a function (36,928 rows): class `padding-int3`, `padding-nop`, `il-align-pad`, `clr-il-body` (detail = IL method) |
| `coverage.md` / `coverage.json` | coverage report, overlap report, reconciliation with the 30,246 estimate |
| `overlaps.tsv` | overlap report rows (header only: zero overlaps) |
| `fragments.tsv` | the 3,315 functions that have chained fragments, with the old name DB size (first fragment only) next to the merged size |
| `funclets.tsv` | the 2,243 unwind funclets with kind (cleanup / catch / seh-filter / seh-finally) and parent function(s) |
| `reconcile.json`, `summary.json`, `funclet_shape_check.json` | machine-readable counts |
| `validate.py` | independent re-check (`python3 validate.py [exe]`, needs pefile + capstone, exit status 0 = pass) |
| `run_all.sh` | rebuilds everything from the exe (`PY=` selects the interpreter; default is the S2 venv) |
| `pecore.py x64scan.py cfgscan.py u1_pdata.py u2_eh.py u4..u8_*.py build.py emit.py emit2.py` | the pipeline; `*.pkl` are caches that `run_all.sh` regenerates |

## `functions.tsv` columns

| column | meaning |
|---|---|
| `id` | `F00001...`, ascending by `start_rva`; stable only for this build of the table |
| `start_rva`, `start_va` | the function entry (for `.pdata` functions the begin of the root RUNTIME_FUNCTION, which is the lowest address and the only entry point that anything calls) |
| `size` | total bytes of all ranges (declared `.pdata` sizes summed over every chained fragment; for leaf functions the bytes reached by decoding plus embedded jump tables) |
| `nranges`, `ranges` | the individual ranges `start-end` (half-open), primary range first; for `.pdata` functions these are exactly the merged RUNTIME_FUNCTION entries |
| `nextents`, `extents` | the ranges merged where they touch. Every function in this build is exactly **one** contiguous extent (`nextents` = 1 for all 38,455) |
| `kind` | `root`, `leaf-no-pdata`, `funclet`, `thunk`, `import-stub`, `trampoline` (definitions below) |
| `confidence` | `certain` (has `.pdata`), `high` (at least one real reference or table entry), `medium` (found only by orphan scan but its address is taken by a `lea`), `low` (found only by orphan scan, nothing references it) |
| `primary_source`, `sources` | how it was discovered: highest-priority tag and all tags (below) |
| `has_pdata`, `n_pdata_entries` | whether `.pdata` covers it, and with how many RUNTIME_FUNCTION entries (root plus chained fragments) |
| `unwind_flags`, `handler_type`, `prolog_size` | UNWIND_INFO of the root entry: `NONE`, `EHANDLER`, `UHANDLER`, `EHANDLER|UHANDLER`; handler is `none`, `__CxxFrameHandler3`, `__GSHandlerCheck`, `__GSHandlerCheck_EH` or `__C_specific_handler`; prolog size in bytes |
| `funclet_kind`, `funclet_parents` | for funclets: `cleanup`, `catch`, `seh-filter`, `seh-finally`; the parent function(s) whose EH tables name it |
| `is_virtual_target`, `virtual_slots` | in at least one RTTI vtable; `Class+colOffset:slot;...` (class names from the S7 class DB, else the raw RTTI name) |
| `ncalls`, `calls` | distinct direct `call rel32` targets (function start RVAs; includes calls found in unreached-code chunks) |
| `ntail`, `tail_targets` | distinct direct `jmp`/`jcc` targets that leave the function (tail calls, conditional tail calls) |
| `n_import_calls` | indirect calls through the import table / data slots (`call [rip+x]`); JSON `imports` lists `dll!name` |
| `n_ind_calls` | indirect calls through a register or non-rip memory operand |
| `n_ind_jmp` | indirect jumps that are not resolved jump tables (tail calls through a register or vtable) |
| `n_jumptables`, `data_in_code_bytes` | MSVC jump tables resolved inside the function and the bytes of data (tables, byte-index tables, two unclassified data chunks) embedded in its ranges; JSON `data_in_code` lists them. A lifter must not decode these bytes |
| `unreached_bytes` | non-padding bytes inside the ranges that the recursive descent did not reach (156 bytes in 26 functions); JSON `unreached_ranges` classifies them `unreached-code` or `unreached-data` |
| `subsystem`, `subsystem_basis`, `name`, `class`, `name_tier` | carried over from the S7 name DB when the function was in S7 (30,242 of the 38,455; 21,113 have a subsystem, 8,763 a name) |
| `s7_size` | the extent the old name DB had (first `.pdata` fragment only for `.pdata` functions) |
| `flags` | `unaligned16` (leaf start not 16-aligned), `unreferenced`, `indirect-jmp`, `discovered-via-orphan-callees`, plus `entered-via-fragment` / `eh-target-also-referenced` (both occur zero times) |

### `kind` definitions

* `root`: has `.pdata`, not a funclet. 17,528.
* `funclet`: named by an EH table (UnwindMap cleanup action, catch handler address, SEH filter or `__finally` handler) and by nothing else. 2,243, of which **380 have `.pdata`** (298 cleanup, 75 catch, 5 seh-filter, 2 seh-finally) and **1,863 have none** (cleanup stubs of the form `lea rcx,[rdx+off]; jmp dtor`). Funclets are not independent functions: they run on the parent's frame (RDX = establisher frame).
* `import-stub`: no `.pdata`; the only instruction is `jmp qword ptr [rip+d]` (6 bytes, or 7 with REX.W) through a slot in the import address table. 382.
* `trampoline`: no `.pdata`; ends in `jmp`, no `ret`, changes no argument register: plain `jmp rel32`, `jmp [rip+slot]` through a non-import slot, and the 16-byte CLR native-entry units in `.nep`. 690.
* `thunk`: no `.pdata`; at most 6 instructions ending in `jmp`; earlier instructions only adjust `rcx/rax/rdx/r8/r11` (this-pointer adjustors, vcall thunks such as `mov rax,[rcx]; jmp [rax+x]`, `mov rcx,rdx; jmp x`, atexit registration stubs `lea rcx,[rip+d]; jmp x`). 3,296.
* `leaf-no-pdata`: every other function without `.pdata`. 14,316. A function without `.pdata` has no stack frame, so it never contains a `call`; these are leaves by construction (getters, setters, constructors that only store, math helpers, `ret 0` stubs).

Only functions without `.pdata` are ever classed `thunk`, `trampoline` or `import-stub`.

### `sources` tags

`pdata` (RUNTIME_FUNCTION begin); `call` (direct `call rel32` from any function); `tailjmp` (`jmp`/`jcc` leaving a function to a function start); `vtable` (RTTI vtable slot); `reloc-ptr` (a `.reloc` DIR64 base-relocated pointer to code, in `.rdata`/`.data`); `init-xc` (4,653 entries of the `__xc` constructor table at RVA 0x57bcb0-0x584e18) and `init-xi` (3 entries at 0x584e28); `export` (222 exports, all code); `entry` (0x5669ce, an import stub to `_CorExeMain`); `tls-callback` (the callback list is empty, zero); `eh-cleanup`, `eh-catch`, `eh-seh-filter`, `eh-seh-finally` (EH table references); `clr-native` (MethodDef rows with native implementation, taken from S7 `clr_methods.json` and re-validated); `nep-unit` (16-byte stub unit in `.nep`); `gap-scan` (orphan code found by scanning the `.pdata` gaps). `rip-ref` (address taken with `lea`/`mov [rip+d]`) and `jcc-out` are recorded as weak tags only: they can attach to a function but never create one on their own. Priority for `primary_source`: pdata, entry, export, vtable, init-xc, init-xi, reloc-ptr, clr-native, call, nep-unit, eh-*, tailjmp, tls-callback, gap-scan.

## Method

1. **`.pdata` and UNWIND_INFO** (`u1_pdata.py`). All 27,526 RUNTIME_FUNCTION entries parsed with their UNWIND_INFO (all version 1; unwind codes decoded and slot counts verified). 9,618 entries carry `UNW_FLAG_CHAININFO`; each chain record equals its parent entry exactly (begin, end, unwind RVA), and chains nest up to depth 5 (7,450 / 1,845 / 280 / 35 / 8 entries at depth 1..5). Following chains to the root gives 17,908 logical functions, the same 17,908 roots as the spikes. 3,315 of them have fragments (9,618 fragments, 1,508,979 bytes). **Every fragment touches another range of its own function, so every logical function is one contiguous extent; this build has no cold or split fragments.** The old name DB size is the first entry only for all 3,315 (283,890 bytes against 1,792,869 merged): merged extents add 1,508,979 bytes that the DB did not see.
2. **EH tables** (`u2_eh.py`). The four handlers are `__CxxFrameHandler3` (import thunk 0x471a36; 1,408 roots), `__GSHandlerCheck` (static, 0x4399c8; 1,238), `__GSHandlerCheck_EH` (static, 0x439a94; 248) and `__C_specific_handler` (0x471a7e; 7), 15,007 roots have no handler. All 1,656 FH3 FuncInfo records parsed (magic 0x19930522; no FH4). Every UnwindMap action, TryBlockMap catch handler address and SEH scope-table handler is a funclet reference: 2,161 distinct cleanup, 75 catch, 5 filter, 2 finally = **2,243 funclets**. Only 380 of them are `.pdata` entries; the other 1,863 are leaf stubs without `.pdata` (this was the missing piece in the earlier 30,246 estimate). S3's 2,161 cleanup and 75 catch funclets, and its 298 cleanup funclets with `.pdata`, are reproduced exactly. G1's "at least 182 funclets among the roots" is superseded by 380 (298 + 75 + 7). Cross-check by shape (`u7_funclet_shape.py`): 376 of the 380 match the funclet prologue `[mov [rsp+10h],rdx] push rbp; sub rsp,N; mov rbp,rdx | lea rbp,[rdx+X]`, and no other root matches that strict shape, so no unreferenced funclet-shaped root exists.
3. **Recursive descent per function** (`cfgscan.py`, `u4_cfg_pdata.py`): 1,180,151 instructions in `.pdata` functions (and 116,425 in leaf functions) decoded by following control flow from each range start, never linearly through data. MSVC jump tables are recovered (`lea base; cmp idx,N; ja; [movzx byte-table;] mov e,[base+idx*4+T]; add; jmp reg`, with the image-base register assumed when it was set earlier in the function, bounds from `cmp`/`and`, and every entry validated to land inside the function): 522 tables in 416 functions, 45,064 bytes of embedded data. In this build jump tables and byte-index tables sit inside `.text` at the end of the function. SEH `__except` jump targets and `lea`-taken catch-continuation labels are added as extra entry points. Fall-through into a table is blocked. Direct `call`/`jmp` targets, rip-relative code addresses and indirect-call/jmp operands are collected.
4. **Data references** (`u5_data_refs.py`). 39,265 `.reloc` DIR64 entries; 27,139 point into executable sections (25,446 in `.rdata`, 1,693 in `.data`). 1,066 RTTI Complete Object Locators and vtables (the `.rdata` COL scan reproduces S7: 20,667 slots, 5,990 unique virtual targets). The 4,653-entry run at RVA 0x57bcb0 is the `__xc` table, the 3-entry run at 0x584e28 the `__xi` table; the TLS callback array at 0x584e50 is empty. 222 exports.
5. **Leaf function discovery** (`u6a_candidates.py`, `u6c_gaps.py`). Strong references into `.pdata` gaps (call, tail jmp, vtable, reloc pointer, init table, export, entry, EH table, CLR native method, `.nep` unit) are decoded by recursive descent bounded by the gap, by the entry address (jumps below the entry and jumps to a 16-aligned address after `int3` padding are tail calls) and by every other known function start; newly found callees and tail targets are added until nothing changes. Conflicts (a strong start inside another function's reached bytes) re-run the owner with the start as a stop. Then the uncovered gap bytes are scanned: after skipping `int3` and NOP padding, every remaining chunk is decoded from its first byte; a chunk becomes a function (`gap-scan`) only if it decodes cleanly to a `ret`/`jmp`/`ud2`. CLR IL method bodies (439, found with exact extents from their IL headers) and the `.nep` stub units are excluded first. Weak references (`lea` addresses) never create functions by themselves. Result: 20,547 leaf functions = 14,655 with a strong reference (including the `.nep` units) + 5,892 found during the scan (5,875 accepted orphans carrying the `gap-scan` tag, plus 17 tail-jump targets that those orphans pointed to); the final residue is 13 bytes of IL alignment padding. The only multi-range ambiguity is shared code reached by several stubs, which is resolved as one function per stub plus one for the shared body.
6. **Classification, edges, names** (`build.py`): kinds, direct call and tail-call lists (all distinct targets are function starts), virtual slots, S7 name/subsystem carry-over, old extents, flags.
7. **Validation** (`validate.py`, 25 independent checks, all pass). It re-parses `.pdata` and chains, re-derives funclets from the EH tables, re-scans COLs and relocations, linearly re-decodes every function (excluding data-in-code) and proves that all 227,009 direct call/jmp edges land on function starts or inside the function, that every reloc code pointer and vtable slot is a function start, that ranges tile the executable sections without overlap together with `gaps.tsv`, that padding rows contain only padding, and that IL rows start with valid IL headers.

## Reconciliation with the spikes (30,246)

| source | spikes | here |
|---|---:|---:|
| `.pdata` roots (merged logical functions) | 17,908 | 17,908 (same set; extents now merged, 3,315 enlarged) |
| leaf functions without `.pdata` | 12,338 | 20,547 |
| total | 30,246 | **38,455** |

* 12,334 of the 12,338 S7 leaf functions are confirmed; the other 4 were not functions (one address lies in padding, three are interior addresses of the leaf at 0x1bebe0).
* Added: **1,863** cleanup funclets with no `.pdata`; **279** functions reached only by tail jumps from other leaves; **191** `.nep` CLR stub units not referenced by anything else (211 units exist; 20 are also referenced); 14 reached by call, relocation or entry; and **5,866** found by the orphan scan, of which 1,664 have a `lea`-taken address (`medium`) and **4,202 have no reference of any kind** (`low`, probably dead code). Total added 8,213.
* The spikes counted only referenced leaf functions that their reference kinds could see (call, data pointer, vtable, tail jmp, CLR, export); they had no EH-table source and no orphan scan. The difference is explained, not a conflict.

Consumers that want only referenced code can drop `confidence = low`: 4,202 functions, 113,089 bytes (21.6% of the 523,920 leaf bytes, 2.0% of all executable bytes). The other 34,253 functions are `.pdata` functions or have at least one reference.

## Facts worth carrying forward

* The image is mixed-mode: **CLR IL method bodies (439, 40,005 bytes) live inside `.text`** between native functions and are data, not code. Metadata and the COR header are in `.rdata`. `gaps.tsv` marks them (`clr-il-body`, detail = IL method).
* `.nep` (0x579000-0x57a276) holds 211 sixteen-byte units `jmp +8; ud2; jmp [slotA]; jmp [slotB]` through data slots filled by the CLR loader (VTableFixups), plus 6-byte `jmp [slot]` stubs. They are trampolines, meaningless off Windows; the runtime should provide or stub them.
* Import stubs (`jmp [IAT]`, 382) are separate functions; 368 distinct imports appear as call/jmp targets in the table (`imports` in JSON).
* `.pdata` covers 85.17% of `.text`; the full universe covers 94.24% of `.text` and 94.23% of all executable bytes.
* Leaf functions never contain calls (no frame), so the call graph of the leaf set is tail jumps only. 227 low-confidence leaves are not 16-byte aligned (packed stub runs).

## Limits and heuristics (what is not proven)

* Static only: indirect calls and jumps are resolved through vtables, relocated pointers and jump tables, not through run-time data. 1,517 functions contain an indirect jump that is not a jump table (tail calls through a register or vtable); the table lists them (`n_ind_jmp`, flag `indirect-jmp`) but cannot name their targets.
* Orphan (`gap-scan`) functions are inferred from clean decoding plus alignment and padding, with no reference. 5,875 functions carry this tag; 4,202 have `low` confidence. A chunk that is data which happens to decode cleanly to a `ret`/`jmp` would be misclassified, but all 5,875 were checked to be disjoint from IL bodies, jump tables and `.pdata`, and 5,646 of the 5,875 are 16-byte aligned.
* Jump-table sizes come from `cmp`/`and` bounds in 515 of 522 cases; 4 tables are scanned until the first invalid entry and 3 are bounded by the byte table that follows them. Two small data chunks inside `.pdata` ranges are marked `unreached-data`.
* `thunk`, `trampoline` and `leaf-no-pdata` are decided by instruction-shape rules above; they are labels for consumers, not part of the unwind structure.
* The 4 funclets not matching the strict shape are still funclets (EH-table evidence is exact).
* Subsystem, name and class come from S7 only. The 8,213 functions that are new here have none.
* Reused and re-checked: S2 `bounds.py`/`functions_table.tsv` (same 27,526 / 17,908 / 9,618 numbers); S7 `universe.pkl`/`name_db.json` (12,334 of 12,338 leaf starts confirmed, first-fragment extents confirmed as the old size); S7 `clr_methods.json` (native method RVAs and IL method RVAs; the IL extents were recomputed from the headers); S3 funclet counts (reproduced).

## Reproduce

`./run_all.sh` (needs `pefile` and `capstone`; reads only the exe and `../S7-name-db/{name_db.json,class_db.json,clr_methods.json}`; writes only here). `python3 validate.py` re-checks the delivered tables.
