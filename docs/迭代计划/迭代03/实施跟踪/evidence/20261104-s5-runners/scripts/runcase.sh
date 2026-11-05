#!/bin/bash
# runcase.sh <label> <binary> <gtest_filter> [timeout_seconds]
# Runs one filter in its own new cwd under /tmp/wt-i03-s5-runners-run/<label>,
# with a hard perl alarm, a private LLVM_PROFILE_FILE and an XML report.
set -u
label=${1:?label}; bin=${2:?binary}; filter=${3:?filter}; limit=${4:-180}
dir=/tmp/wt-i03-s5-runners-run/$label
rm -rf "$dir"; mkdir -p "$dir"
cd "$dir" || exit 2
# Optional: RUNCASE_STAGE="file1 file2" copies modules (e.g. libWtDataStorage.dylib) into the cwd.
for f in ${RUNCASE_STAGE:-}; do cp "$f" "$dir/"; done
LLVM_PROFILE_FILE="$dir/%p.profraw" perl -e 'alarm shift; exec @ARGV or exit 127' "$limit" \
  "$bin" --gtest_filter="$filter" --gtest_output="xml:$dir/result.xml" > "$dir/out.log" 2>&1
status=$?
echo "$status" > "$dir/exit"
summary=$(grep -m1 "<testsuites" "$dir/result.xml" 2>/dev/null | sed -E 's/.*tests="([0-9]+)" failures="([0-9]+)" disabled="([0-9]+)".*/tests=\1 failures=\2 disabled=\3/')
echo "$label exit=$status ${summary:-no-xml}"
