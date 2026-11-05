#!/bin/bash
# usage: runcases.sh <binary> <outdir> <timeout> <gtest-filter>   (each listed case in its own process + cwd)
# env passthrough: extra env via EXTRA_ENV="A=1 B=2"
bin="$1"; out="$2"; to="$3"; filter="$4"
mkdir -p "$out"
: > "$out/results.txt"
"$bin" --gtest_list_tests --gtest_filter="$filter" 2>/dev/null | awk '/^[^ ]/{s=$1} /^  /{print s $1}' | grep -v DISABLED_ > "$out/cases.txt"
while read -r c; do
  safe=$(echo "$c" | tr '/' '_')
  mkdir -p "$out/work/$safe"
  ( cd "$out/work/$safe" && env LLVM_PROFILE_FILE="$out/work/$safe/%p.profraw" $EXTRA_ENV perl -e 'alarm shift; exec @ARGV' "$to" "$bin" --gtest_filter="$c" --gtest_output=xml:"$out/$safe.xml" > "$out/$safe.log" 2>&1 )
  echo "$c $?" >> "$out/results.txt"
done < "$out/cases.txt"
awk '{n++; if($2==0)p++; else f++} END{printf "total=%d pass=%d fail=%d\n", n, p, f}' "$out/results.txt"
