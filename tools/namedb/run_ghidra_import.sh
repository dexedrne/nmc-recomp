#!/bin/sh
# Headless Ghidra import of name_db_v2 into a fresh project (needs tools/ghidra and tools/jdk under this directory; scratch only).
# Usage: ./run_ghidra_import.sh [maxRows]
D="$(cd "$(dirname "$0")" && pwd)"
export JAVA_HOME="$D/tools/jdk25/jdk-25.0.5+8"
export PATH="$JAVA_HOME/bin:$PATH"
export MAXMEM=${MAXMEM:-3g}
G="$D/tools/ghidra/ghidra_12.2_PUBLIC/support/analyzeHeadless"
EXE=/home/user/projects/kh2-nmc-analysis/pc/kh2fm.exe
PROJ="$D/ghidra_proj"
rm -rf "$PROJ" && mkdir -p "$PROJ"
"$G" "$PROJ" khnmc -import "$EXE" -noanalysis -scriptPath "$D/ghidra_scripts" \
  -postScript GhidraImportNameDbV2.java "$D/name_db_v2.csv" "$D/data_in_code_v2.csv" "$D/vtables_v2.csv" "$D/data_labels.csv" "$D/ghidra_import_report.json" ${1:-100000} \
  -postScript GhidraDecompCheckV2.java "$D/ghidra_decomp_check.json" 0x3c12f0:calc_damage 0x124060:createDevices 0x117710:createGraphicsPipelineState 0x106700:LoadFile 0x2b88c0:SetGameLevel 0x1121d0:bindPipelineState \
  > "$D/ghidra_run.log" 2>&1
echo "exit $?"
