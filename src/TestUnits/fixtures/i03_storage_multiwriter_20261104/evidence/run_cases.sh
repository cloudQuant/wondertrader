#!/bin/bash
# usage: run_cases.sh <exe> <outdir> [per-case-timeout]
exe="$1"; out="$2"; to="${3:-120}"
mkdir -p "$out/xml" "$out/log"; cd "$out" || exit 2
: > results.tsv
"$exe" --gtest_list_tests | awk '/^[A-Za-z]/{s=$1} /^  /{print s $1}' > cases.txt
while read -r t; do
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    perl -e "alarm $to; exec @ARGV" "$exe" --gtest_filter="$t" --gtest_output=xml:"$out/xml/$t.xml" > "$out/log/$t.log" 2>&1 < /dev/null
  printf '%s\t%s\n' "$t" "$?" >> results.tsv
done < cases.txt 2>/dev/null
