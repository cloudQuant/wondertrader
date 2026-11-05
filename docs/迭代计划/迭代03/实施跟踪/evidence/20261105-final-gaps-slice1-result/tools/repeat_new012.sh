#!/bin/bash
# repeat_new012.sh BIN LABEL N : run the I03-NEW-012 case N times, each in its own process
SC=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
BIN=$1; LABEL=$2; N=${3:-300}
D=$SC/slice1-result-run/$LABEL; rm -rf "$D"; mkdir -p "$D"; cd "$D"
fail=0
for i in $(seq 1 $N); do
  LLVM_PROFILE_FILE="$D/p.profraw" perl -e 'alarm shift; exec @ARGV or exit 127' 60 "$BIN" --gtest_filter=ParserUDPShutdownTest.OldTwoChannelReadCompletionsBelongToOneReconnectEpoch > "$D/last.log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then fail=$((fail+1)); cp "$D/last.log" "$D/fail-$i.log"; fi
done
echo "total=$N failures=$fail" | tee "$D/summary.txt"
