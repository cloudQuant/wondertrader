#!/bin/bash
# usage: run_cases.sh <build_out_dir> <run_root> <timeout_sec> [gtest-filter...]
# Runs every listed case (default: all) in its own process and cwd with a hard alarm.
set -u
out=${1:?build dir}; root=${2:?run root}; to=${3:-30}; shift 3 || true
bin="$out/s5_lifecycle_tests"
mkdir -p "$root"
if [ "$#" -gt 0 ]; then cases=("$@"); else
  cases=($("$bin" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}'))
fi
: > "$root/results.txt"
for c in "${cases[@]}"; do
  d="$root/$(echo "$c" | tr './' '__')"
  rm -rf "$d"; mkdir -p "$d"
  ( cd "$d" && S5_MODULE_DIR="$out/mods/" TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 report_thread_leaks=${S5_TSAN_THREAD_LEAKS:-1}" \
      ASAN_OPTIONS="detect_leaks=0" \
      /usr/bin/perl -e 'alarm shift; exec @ARGV' "$to" "$bin" --gtest_filter="$c" --gtest_output=xml:"$d/result.xml" > "$d/out.log" 2>&1 )
  ec=$?
  if [ -f "$d/result.xml" ]; then x=$(grep -o 'tests="[0-9]*" failures="[0-9]*" disabled="[0-9]*" errors="[0-9]*"' "$d/result.xml" | head -1); else x="NO-XML"; fi
  san=$(grep -c -E "WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:" "$d/out.log")
  echo "$c exit=$ec $x sanitizer_reports=$san" | tee -a "$root/results.txt"
done
