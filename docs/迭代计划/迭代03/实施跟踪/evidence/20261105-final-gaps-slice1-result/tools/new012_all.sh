#!/bin/bash
SC=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad
F=ParserUDPShutdownTest.OldTwoChannelReadCompletionsBelongToOneReconnectEpoch
for v in beforetest current; do
  D=$SC/slice1-result-run/new012-repeat300-$v; rm -rf $D; mkdir -p $D; cd $D
  LLVM_PROFILE_FILE=$D/p.profraw perl -e 'alarm shift; exec @ARGV or exit 127' 1800 $SC/slice1-result-bins/t_ParserUDP-$v --gtest_filter=$F --gtest_repeat=300 --gtest_output=xml:$D/result.xml > $D/run.log 2>&1
  echo "rc=$?" > $D/exit.txt
  echo "$v repeat300 rc=$(cat $D/exit.txt) failed_iterations=$(grep -c '^\[  FAILED  \] '$F' (' $D/run.log) passed_iterations=$(grep -c '^\[       OK \] '$F $D/run.log)" | tee $D/summary.txt
done
bash $SC/slice1-tools/repeat_new012.sh $SC/slice1-result-bins/t_ParserUDP-current new012-process300-current 300
