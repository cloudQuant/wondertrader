#!/bin/bash
# usage: loop.sh <worker> <count>
bin=/tmp/wt-i03-s5-wp1-ledger-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore
w=$1; n=$2; d=/tmp/wt-i03-s5-wp1-ledger-run/urj/w$w; mkdir -p $d; cd $d
fail=0
for i in $(seq 1 $n); do
  LLVM_PROFILE_FILE=$d/p-%p.profraw perl -e 'alarm shift; exec @ARGV' 60 $bin --gtest_filter='UnresolvedReportJournal.InterruptedPrimaryAppendAfterProcessExitExportsForensicPrefix' > out-$i.txt 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then fail=$((fail+1)); cp out-$i.txt fail-$i.txt; fi
  rm -f out-$i.txt $d/p-*.profraw
done
echo "worker=$w runs=$n fail=$fail"
