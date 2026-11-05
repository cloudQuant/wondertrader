#!/bin/bash
# usage: run1.sh <binary> <outdir> <test> [timeout]
b=$1; o=$2; t=$3; to=${4:-60}
d="$o/cwd/$t"; mkdir -p "$d"; cd "$d" || exit 1
LLVM_PROFILE_FILE="$d/%p.profraw" perl -e 'alarm shift; exec @ARGV' "$to" "$b" --gtest_filter="$t" --gtest_output=xml:"$d/result.xml" > "$d/log.txt" 2>&1
rc=$?
echo "$rc $t"
