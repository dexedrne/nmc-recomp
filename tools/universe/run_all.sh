#!/bin/sh
# Rebuild the whole universe from the exe + the S7 name DB / CLR method list. Read-only on everything outside this directory.
# Needs a python3 with pefile + capstone (e.g. ../S2-lifter/venv/bin/python).
PY=${PY:-/home/user/projects/kh2-nmc-analysis/scratch/S2-lifter/venv/bin/python}
cd "$(dirname "$0")" || exit 1
set -e
$PY u4_cfg_pdata.py       # recursive descent over every merged .pdata function (jump tables, SEH jump targets)  -> cfg_pdata.pkl
$PY u5_data_refs.py       # .reloc code pointers, RTTI COLs/vtables, init tables                                  -> data_refs.pkl
$PY u6a_candidates.py     # all code references, EH funclet references, exports, entry                           -> cand.pkl
$PY u6c_gaps.py           # leaf (no .pdata) discovery to a fixpoint + gap-scan of orphan code                   -> gapfn.pkl
$PY emit.py               # functions.tsv/json, ranges.tsv, gaps.tsv, overlaps.tsv, coverage.json                -> F.pkl
$PY emit2.py              # fragments.tsv, funclets.tsv, reconcile.json, summary.json
$PY u7_funclet_shape.py   # funclet shape cross-check                                                            -> funclet_shape_check.json
$PY u8_report.py          # coverage.md
$PY validate.py           # independent re-check against the exe
