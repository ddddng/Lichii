#!/bin/bash
# Writes and replays each churn pattern on lavapipe, then counts texture cache images per frame.
set -u
CLI=${CLI:-$HOME/.capy/work/build/bin/eden-cli}
SRC=${SRC:-/workspace/lichii}
FRAMES=${FRAMES:-300}
OUT=${OUT:-$HOME/.capy/work/churn}
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
mkdir -p "$OUT"
run() {
  local name=$1 pattern=$2; shift 2
  "$CLI" --gpu-trace-churn "$OUT/$pattern.lxtrace" "$pattern" "$FRAMES" || return 1
  rm -rf "$OUT/$name-out"
  env EDEN_NX2_TC_TRACE=1 EDEN_NX2_MEM_TRACE=1 "$@" "$CLI" --replay "$OUT/$pattern.lxtrace" --out "$OUT/$name-out" > "$OUT/$name.log" 2>&1
  echo "== $name (rc=$?)"
  python3 -I "$SRC/tools/nx2/tc_churn.py" "$OUT/$name.log" --every 30
  tr -d '\000' < "$OUT/$name.log" | sed 's/\x1b\[[0-9;]*m//g' | grep -o 'Memory trace: texture cache [0-9]* images [0-9]* MiB ([0-9]* MiB GPU-written' | tail -1
}
for p in ${PATTERNS:-steady resize resize-unique color-resize-unique formats drift heap heap-drift table}; do run "$p" "$p"; done
[[ " ${PATTERNS:-table} " == *" table "* ]] && run table-filter table LICHII_TABLE_DEPTH_FILTER=1
