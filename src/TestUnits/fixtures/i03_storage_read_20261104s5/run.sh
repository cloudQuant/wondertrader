#!/bin/bash
# Red/green runner for iteration 03 package s5 storage-read. Usage:
#   run.sh <lib_dir_with_plain_WTSUtils_WTSTools> <out_dir> [red_src_root]
# Builds s5_ad_read against the working tree (green) and, when red_src_root is given,
# against that snapshot with WT_S5_BASELINE=ON (red). Each run uses its own cwd and a
# hard timeout. Uses Homebrew LLVM 22 with ASan+UBSan (this host's paths).
set -u
here=$(cd "$(dirname "$0")" && pwd)
lib=${1:?lib dir}; out=${2:?out dir}; red=${3:-}
cxx=${WT_CLANGXX:-/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++}
mkdir -p "$out"
build_and_run() {
  local label=$1; shift
  cmake -S "$here" -B "$out/build-$label" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$cxx" \
    -DWT_LIB_DIR="$lib" -DWT_SAN=address "$@" > "$out/$label.configure.log" 2>&1 || return 2
  ninja -C "$out/build-$label" -j 3 s5_ad_read > "$out/$label.build.log" 2>&1 || return 3
  mkdir -p "$out/run-$label"
  (cd "$out/run-$label" && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    perl -e 'alarm shift; exec @ARGV' 300 "$out/build-$label/s5_ad_read" \
    --gtest_output=xml:"$out/run-$label/result.xml" > "$out/run-$label/out.log" 2>&1; echo $? > "$out/run-$label/exit")
  echo "$label exit=$(cat "$out/run-$label/exit") $(grep -o 'tests="[0-9]*" failures="[0-9]*"' "$out/run-$label/result.xml" 2>/dev/null | head -1)"
}
build_and_run green
[ -n "$red" ] && build_and_run red -DWT_SRC_ROOT="$red" -DWT_S5_BASELINE=ON
exit 0
