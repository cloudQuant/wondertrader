#!/bin/bash
# run_suites.sh <label-prefix> <binary> [timeout] [suite-regex]
# Runs every gtest suite of <binary> (optionally filtered by an extended regex
# on the suite name) in its own process and cwd via runcase.sh; prints one
# summary line per suite and, for failing suites, the failing case names.
set -u
prefix=${1:?prefix}; bin=${2:?binary}; limit=${3:-180}; regex=${4:-.}
here=$(cd "$(dirname "$0")" && pwd)
listdir=/tmp/wt-i03-s5-runners-run/$prefix-list
mkdir -p "$listdir"
suites=$(cd "$listdir" && LLVM_PROFILE_FILE="$listdir/p.profraw" "$bin" --gtest_list_tests | grep -v '^ ' | sed 's/\.$//' | grep -E "$regex")
for s in $suites; do
  line=$("$here/runcase.sh" "$prefix-$s" "$bin" "$s.*" "$limit")
  echo "$line"
  dir=/tmp/wt-i03-s5-runners-run/$prefix-$s
  grep -E '^\[  FAILED  \] [A-Za-z0-9_]+\.[A-Za-z0-9_]+ \(' "$dir/out.log" 2>/dev/null | sed 's/^/    /'
  if ! grep -q "<testsuites" "$dir/result.xml" 2>/dev/null; then
    grep -E '^\[ RUN' "$dir/out.log" | tail -1 | sed 's/^/    last: /'
  fi
done
