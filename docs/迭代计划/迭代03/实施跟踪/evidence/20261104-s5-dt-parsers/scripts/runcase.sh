#!/bin/bash
# usage: runcase.sh <label> <binary> <filter> [timeout_sec] [extra env assignments...]
# Runs one gtest binary in an isolated cwd under /tmp/wt-i03-s5-dt-parsers-run/<label>,
# with a hard alarm, and copies out.log/result.xml/exit into the evidence runs/ dir.
set -u
label=$1; bin=$2; filter=$3; to=${4:-120}; shift 4 || true
R=/tmp/wt-i03-s5-dt-parsers-run/$label
E=/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-dt-parsers/runs/$label
rm -rf "$R"; mkdir -p "$R" "$E"; cd "$R"
export LLVM_PROFILE_FILE="$R/%p.profraw"
env "$@" /usr/bin/perl -e 'alarm shift; exec @ARGV' "$to" "$bin" --gtest_filter="$filter" --gtest_output=xml:"$R/result.xml" > "$R/out.log" 2>&1
ec=$?
echo "$ec" > "$R/exit"
if [ -f "$R/result.xml" ]; then x=$(grep -o 'tests="[0-9]*" failures="[0-9]*" disabled="[0-9]*" errors="[0-9]*"' "$R/result.xml" | head -1); else x="NO-XML"; fi
cp "$R/out.log" "$R/exit" "$E/" 2>/dev/null; [ -f "$R/result.xml" ] && cp "$R/result.xml" "$E/"
echo "$bin" > "$E/binary.txt"; shasum -a 256 "$bin" >> "$E/binary.txt"; echo "filter=$filter env=$*" >> "$E/binary.txt"
echo "$label exit=$ec $x"
