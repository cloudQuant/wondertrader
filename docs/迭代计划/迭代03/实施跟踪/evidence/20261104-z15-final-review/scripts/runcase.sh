#!/bin/bash
# usage: runcase.sh <label> <binary> <filter> [timeout_sec]
set -u
label=$1; bin=$2; filter=$3; to=${4:-120}
R=/tmp/wt-i03-zreview-20261104/run/$label
rm -rf "$R"; mkdir -p "$R"; cd "$R"
export LLVM_PROFILE_FILE="$R/%p.profraw"
/usr/bin/perl -e 'alarm shift; exec @ARGV' "$to" "$bin" --gtest_filter="$filter" --gtest_output=xml:"$R/result.xml" > "$R/out.log" 2>&1
ec=$?
echo "$ec" > "$R/exit"
if [ -f "$R/result.xml" ]; then x=$(grep -o 'tests="[0-9]*" failures="[0-9]*" disabled="[0-9]*" errors="[0-9]*"' "$R/result.xml" | head -1); else x="NO-XML"; fi
echo "$label exit=$ec $x"
