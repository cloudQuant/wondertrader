#!/bin/bash
# usage: run_all_cases.sh <binary> <outdir> <gtest-filter-for-listing> [timeout] [env assignments...]
# 列出匹配用例后逐例独立进程运行（-P3），汇总到 <outdir>/summary.txt
b=$1; o=$2; f=$3; to=${4:-120}; shift 4 || shift $#
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$o"
"$b" --gtest_list_tests --gtest_filter="$f" | awk '/^[^ ]/{s=$1} /^  /{print s $1}' > "$o/cases.txt"
cat "$o/cases.txt" | xargs -P 3 -I{} "$here/runcase.sh" "$b" "$o/cwd" "{}" "$to" "$@" > "$o/summary.txt"
sort -k2 "$o/summary.txt" -o "$o/summary.txt"
echo "cases=$(wc -l < "$o/cases.txt") exit0=$(awk '$1==0' "$o/summary.txt" | wc -l) nonzero=$(awk '$1!=0' "$o/summary.txt" | wc -l)"
