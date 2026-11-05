#!/bin/sh
# Usage (from the repository root):
#   sh src/TestUnits/fixtures/i03_date_mapping_20261104s5/run.sh <build-dir> <run-dir> <WT_BUILD> <BEFORE>
# <WT_BUILD>: coverage build providing build_x64/Debug/libs/libWTSTools.a and libWTSUtils.a
# <BEFORE>:   directory whose src/ holds the pre-change copies (red controls)
# Builds with Homebrew LLVM, runs every executable in <run-dir> with a hard
# timeout, and writes <name>.log/.xml plus results.txt. Baseline (red) binaries
# are expected to fail; current binaries must pass.
set -u
build=${1:?build dir}; run=${2:?run dir}; wtbuild=${3:?WT_BUILD}; before=${4:?BEFORE}
root=$(pwd)
cmake -S "$root/src/TestUnits/fixtures/i03_date_mapping_20261104s5" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
  -DWT_BUILD="$wtbuild" -DBEFORE="$before" > "$build.configure.log" 2>&1 || exit 2
(cd "$build" && ninja -j3 -k 0) > "$build.build.log" 2>&1
echo "build exit=$?"
mkdir -p "$run"
cd "$run" || exit 3
: > results.txt
for name in date_client tz_legacy tz_legacy_baseline monitor_chain monitor_chain_baseline \
            reader_chain reader_chain_baseline xele_chain xele_chain_baseline; do
  LLVM_PROFILE_FILE="$run/$name.profraw" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    perl -e 'alarm shift; exec @ARGV' 120 "$build/$name" --gtest_output="xml:$run/$name.xml" > "$run/$name.log" 2>&1
  echo "$name exit=$?" >> results.txt
done
for name in xele_tsan xele_tsan_baseline; do
  LLVM_PROFILE_FILE="$run/$name.profraw" TSAN_OPTIONS=halt_on_error=1 \
    perl -e 'alarm shift; exec @ARGV' 180 "$build/$name" --gtest_filter='XeleClockField.ConcurrentPreparesKeepIndependentValues' \
    --gtest_output="xml:$run/$name.xml" > "$run/$name.log" 2>&1
  echo "$name exit=$?" >> results.txt
done
cat results.txt
