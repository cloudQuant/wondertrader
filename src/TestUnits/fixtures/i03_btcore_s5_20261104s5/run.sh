#!/bin/bash
# usage: run.sh <red|green> <build dir> <out dir> <WT_LIB_DIR> [WT_SRC_ROOT]
# Configures/builds this fixture and runs each case in its own process + cwd.
set -u
mode="$1"; build="$2"; out="$3"; libdir="$4"; srcroot="${5:-}"
here=$(cd "$(dirname "$0")" && pwd)
args=(-S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_LIB_DIR="$libdir" -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
[ -n "$srcroot" ] && args+=(-DWT_SRC_ROOT="$srcroot")
[ "$mode" = red ] && args+=(-DWT_S5_RED=ON)
cmake "${args[@]}" > "$build.configure.log" 2>&1 || { echo "configure failed"; exit 2; }
ninja -C "$build" -j 3 > "$build.build.log" 2>&1; echo "build-exit=$?" >> "$build.build.log"
mkdir -p "$out"; : > "$out/results.txt"
for bin in ${S5_BINS:-s5_frozen_tdate s5_q02_integration s5_new029}; do
  [ -x "$build/$bin" ] || { echo "$bin missing" >> "$out/results.txt"; continue; }
  "$build/$bin" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}' | grep -v DISABLED_ > "$out/$bin.cases"
  while read -r c; do
    mkdir -p "$out/work/$c"
    ( cd "$out/work/$c" && LLVM_PROFILE_FILE="$out/work/$c/%p.profraw" perl -e 'alarm shift; exec @ARGV' 120 "$build/$bin" --gtest_filter="$c" --gtest_output=xml:"$out/$c.xml" > "$out/$c.log" 2>&1 )
    echo "$bin $c $?" >> "$out/results.txt"
  done < "$out/$bin.cases"
done
awk '{n++; if($3==0)p++; else f++} END{printf "'"$mode"' total=%d pass=%d fail=%d\n", n, p, f}' "$out/results.txt"
