#!/bin/sh
# Rebuild name DB v2 from the exe, the corrected universe and the S7 / platform-seams / docs inputs. Scratch only.
PY=${PY:-/home/user/projects/kh2-nmc-analysis/scratch/S7-name-db/venv/bin/python}
cd "$(dirname "$0")" || exit 1
set -e
$PY b2_rtti.py          # independent RTTI recovery        -> rtti.pkl
$PY b1_scan.py          # decode every function once       -> scan.pkl
$PY b3_classes.py       # class model, families, thunks    -> classes.pkl
$PY b4_strings_survey.py   # strings                       -> strings.pkl
$PY b5_cands.py         # name candidates (RTTI, Panacea, docs, strings, S7)  -> cands.pkl, strmap.pkl
$PY b6_ps2.py           # PS2 transfer pairs                -> ps2v2.pkl
$PY b7_assemble.py      # choose names, votes, subsystems   -> db.pkl
$PY b8_emit.py          # csv/json/reports
$PY -I validate_namedb2.py
# optional (needs tools/ghidra + tools/jdk25): ./run_ghidra_import.sh
