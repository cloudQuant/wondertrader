#!/bin/bash
# usage: loop.sh <binary> <n> <outfile>
bin=$1; n=$2; out=$3; fail=0; : > $out
for i in $(seq 1 $n); do
  LLVM_PROFILE_FILE=/dev/null perl -e 'alarm 20; exec @ARGV' $bin --gtest_filter='ParserUDPShutdownTest.OldTwoChannelReadCompletionsBelongToOneReconnectEpoch' > run.log 2>&1
  ec=$?
  if [ $ec -ne 0 ]; then fail=$((fail+1)); echo "run $i exit=$ec" >> $out; grep -A4 "Failure" run.log | head -8 >> $out; fi
done
echo "total=$n failures=$fail" | tee -a $out
