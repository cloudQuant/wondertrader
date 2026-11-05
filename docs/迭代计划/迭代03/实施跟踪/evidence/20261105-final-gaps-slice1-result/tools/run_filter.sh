#!/bin/bash
# run_filter.sh BIN LABEL FILTER [TIMEOUT]
SC=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
BIN=$1; LABEL=$2; FILTER=$3; TO=${4:-180}
D=$SC/slice1-result-run/$LABEL; rm -rf "$D"; mkdir -p "$D"; cd "$D"
LLVM_PROFILE_FILE="$D/profile.profraw" perl -e 'alarm shift; exec @ARGV or exit 127' "$TO" "$BIN" --gtest_filter="$FILTER" --gtest_output=xml:"$D/result.xml" > "$D/run.log" 2>&1
RC=$?
echo "$LABEL rc=$RC" | tee "$D/exit.txt"
grep -E '^\[  (PASSED|FAILED)|^\[  FAILED|tests ran|FAILED TEST' "$D/run.log" | head -20
