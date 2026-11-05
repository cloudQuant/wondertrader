#!/bin/bash
# usage: runcases.sh <binary> <label> <suite-regex> [timeout]
# Runs each matching gtest case in its own cwd under /tmp/wt-i03-s5-uftcore-run/<label>/<case>/
set -u
BIN=$1; LABEL=$2; RE=$3; TO=${4:-60}
ROOT=/tmp/wt-i03-s5-uftcore-run/$LABEL
mkdir -p $ROOT
LIST=$ROOT/cases.txt
( mkdir -p $ROOT/_list && cd $ROOT/_list && LLVM_PROFILE_FILE=$ROOT/_list/%p.profraw $BIN --gtest_list_tests ) | awk '/^[^ ]/{s=$1} /^  /{print s $1}' | grep -E "$RE" > $LIST
: > $ROOT/summary.txt
pass=0; fail=0
while read -r c; do
  d=$ROOT/$(echo "$c" | tr '/.' '__')
  mkdir -p $d
  ( cd $d && LLVM_PROFILE_FILE=$d/%p.profraw perl -e 'alarm shift; exec @ARGV' $TO $BIN --gtest_filter="$c" --gtest_output=xml:$d/result.xml > $d/out.log 2>&1 ); rc=$?
  if [ $rc -eq 0 ] && grep -q 'failures="0"' $d/result.xml 2>/dev/null; then st=PASS; pass=$((pass+1)); else st=FAIL; fail=$((fail+1)); fi
  echo "$st rc=$rc $c" >> $ROOT/summary.txt
done < $LIST
echo "TOTAL $(wc -l < $LIST) PASS $pass FAIL $fail" | tee -a $ROOT/summary.txt
