# khlift tools (route B lifter: kh2fm.exe -> C)

Source only; nothing here is game data.  Generated C (`../out/*`) is game-derived and must never be committed.

| file | what |
|---|---|
| `khlift/universe.py` | loads the canonical function universe (`scratch/universe/F.pkl`) and the PE image; extends the 2 under-sized jump tables |
| `khlift/ops.py` | table-driven VEX op -> C rules (scalar ints inline, SIMD/float -> `vx_*` helpers) |
| `khlift/emit.py` | per-function lifter: two-pass basic-block discovery (no duplicated instructions), calls, imports, tail jumps, jump-table switches, xgetbv/cpuid, V256 |
| `khlift/imports.py` | PE import table -> `rt_imp_<dll>__<name>` symbols and synthetic import addresses |
| `lift_all.py` | lift the universe (or a subset) into bounded shards + `shards.json` + `lift_stats.jsonl` |
| `genrt.py` | `kh_table.c` (dispatch + import tables), weak import defaults, `.nep` stubs, `khrt_imports.h` |
| `buildgen.py` | `build.x86.ninja`, `build.arm64.ninja`, `Makefile`, `CMakeLists.txt` for a run directory |
| `ccwrap.py` | compile wrapper that logs wall time and max RSS per translation unit |
| `rt/` | runtime headers/sources: `khrt.h` (ctx, memory model, flags), `khrt_simd.h` (NaN/min/max/rounding policy, vx_ ops), `khrt_cpu.c`, `khrt_dispatch.c`, `rt_libm.c`, `khrt_rcp_tab.c`, test stubs |
| `difftest.py` (`diff.py`) | differential test: original bytes (Unicorn or native CPU) vs compiled lifted C, callee stubs identical on both sides |
| `native_oracle.c` | native-execution oracle (INT3 callee stubs through a SIGTRAP handler) for AVX and rcpps/rsqrtps |
| `select_sets.py` | pick test candidates by structure (direct calls, dispatch, jump tables, tails, imports, leaves) |
| `mkdrv.py`, `tests/drv.c` | cross-architecture determinism driver over a shard subset (x86-64 vs arm64 output diff) |
| `tests/nanmicro.c` | NaN / denormal / min-max / conversion micro-test (policy vs host raw vs x86 SSE reference) |
| `tests/gen_rcp_tab.c`, `tests/rcp_check_header.c`, `tests/rcp_model_check.c` | rcpps/rsqrtps table generator and exhaustive 2^32 checks |
| `census.py` | VEX op / expr / ccall / dirty census over the universe |
| `POLICY.md` | the one policy (ABI, memory, NaN/denormal/min-max/rounding, rcp, CPUID, libm, flags) |

Typical run (venv with pyvex/archinfo/pefile/unicorn/capstone: `scratch/S2-lifter/venv`):

    python lift_all.py --out ../out/run2 --all --jobs 6
    python genrt.py --out ../out/run2/gen ; python buildgen.py --root ../out/run2
    # copy rt/ and ccwrap.py next to the manifests, then: ninja -f build.x86.ninja -j8 all   (or make ARCH=arm64 all)
    python select_sets.py ../out/run2/lift_stats.jsonl ../work/sets
    python difftest.py --oracle native --rvas-file SET --tests 24 --out result.json   # --profile dsp for the AVX kernels

Linking the whole universe: with GNU ld use `-Wl,--start-group libkh2lifted.a libkh2rt.a -Wl,--end-group` (kh_table.o references every
lifted function).  Both architectures link: x86-64 gcc 37 MB, arm64 Apple clang 26 MB (probe: `kh_lookup(0x140001000)`).
Run directory layout expected by the manifests: `shards/ gen/ rt/ tools/ccwrap.py logs/`.
