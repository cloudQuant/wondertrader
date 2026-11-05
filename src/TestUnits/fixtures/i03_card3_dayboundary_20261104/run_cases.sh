#!/bin/bash
# usage: run_cases.sh <gtest binary> <out dir> [timeout seconds]
# Runs each test case in its own process and working directory; writes results.txt (case rc).
bin="$1"; out="$2"; to="${3:-120}"
mkdir -p "$out"
: > "$out/results.txt"
"$bin" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}' > "$out/cases.txt"
while read -r c; do
  safe=$(echo "$c" | tr '/' '_')
  mkdir -p "$out/work/$safe"
  ( cd "$out/work/$safe" && perl -e 'alarm shift; exec @ARGV' "$to" "$bin" --gtest_filter="$c" --gtest_output=xml:"$out/$safe.xml" > "$out/$safe.log" 2>&1 )
  echo "$c $?" >> "$out/results.txt"
done < "$out/cases.txt"
awk '{n++; if($2==0)p++; else f++} END{printf "total=%d pass=%d fail=%d\n", n, p, f}' "$out/results.txt"
