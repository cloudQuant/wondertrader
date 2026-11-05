#!/bin/bash
# usage: run1.sh <binary> <outroot> <test>   (one isolated process per test)
b=$1; o=$2; t=$3
d="$o/cwd/$t"; mkdir -p "$d"; cd "$d" || exit 1
LLVM_PROFILE_FILE="$d/p-%p.profraw" perl -e 'alarm 90; exec @ARGV' "$b" --gtest_filter="$t" --gtest_output=xml:"$d/result.xml" > "$d/log.txt" 2>&1
rc=$?
echo "$rc $t"
