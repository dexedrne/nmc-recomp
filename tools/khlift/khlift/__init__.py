"""khlift: VEX-IR (pyvex) -> C lifter for kh2fm.exe (KHNMC decomp edition, route B).

Modules
  universe  canonical function universe loader (scratch/universe)
  ops       table-driven VEX operation -> C expression rules
  emit      per-function lifter (basic blocks, calls, jump tables, tail jumps)
  imports   PE import table -> runtime hook names / header generation
  shard     grouping of lifted functions into bounded C files, ninja/cmake/make manifests
"""
BASE = 0x140000000
