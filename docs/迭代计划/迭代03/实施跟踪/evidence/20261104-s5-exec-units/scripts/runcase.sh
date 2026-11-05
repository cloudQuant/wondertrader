#!/bin/bash
# usage: runcase.sh <binary> <outdir> <gtest-filter> [timeout-seconds] [extra env assignments...]
# 每个用例独立 cwd；LLVM_PROFILE_FILE 指向该 cwd；perl alarm 硬超时；记录退出码与 XML 计数。
b=$1; o=$2; t=$3; to=${4:-120}; shift 4 || shift $#
safe=$(echo "$t" | tr '/:*?' '____')
d="$o/$safe"; rm -rf "$d"; mkdir -p "$d"; cd "$d" || exit 1
env LLVM_PROFILE_FILE="$d/%p.profraw" "$@" perl -e 'alarm shift; exec @ARGV or exit 127' "$to" "$b" \
  --gtest_filter="$t" --gtest_output=xml:"$d/result.xml" > "$d/log.txt" 2>&1
rc=$?
echo "$rc" > "$d/exit.txt"
counts=$(grep -o 'tests="[0-9]*" failures="[0-9]*" disabled="[0-9]*" errors="[0-9]*"' "$d/result.xml" 2>/dev/null | head -1)
rm -f "$d"/*.profraw
echo "$rc $t ${counts:-NO_XML}"
