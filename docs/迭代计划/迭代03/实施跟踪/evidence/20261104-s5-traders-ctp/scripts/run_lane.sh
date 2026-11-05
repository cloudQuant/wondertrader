#!/bin/bash
# Run one trader lane binary in an isolated cwd with a hard timeout.
# usage: run_lane.sh <binary> <run-dir> [gtest args...]
set -u
bin=${1:?binary}; dir=${2:?run dir}; shift 2
mkdir -p "$dir"
cd "$dir" || exit 2
LLVM_PROFILE_FILE="$dir/%p.profraw" perl -e 'alarm shift; exec @ARGV' 300 "$bin" "$@" --gtest_output=xml:"$dir/result.xml" > "$dir/out.log" 2>&1
rc=$?
echo "exit=$rc" > "$dir/exit.txt"
exit $rc
