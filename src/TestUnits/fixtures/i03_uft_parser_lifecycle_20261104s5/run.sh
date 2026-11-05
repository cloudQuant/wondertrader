#!/bin/bash
# Builds and runs the WtUftCore ParserAdapter lifecycle fixture (BUG-R78-001, card 9).
# usage: run.sh <wt-coverage-build-dir> <fixture-build-dir> <before-dir> [thread]
#   wt-coverage-build-dir : coverage build providing build_x64/Debug/libs/{libWTSTools,libWTSUtils}.a
#   before-dir            : directory holding src/WtUftCore/ParserAdapter.{h,cpp} pre-change
#                           copies (red control); pass "" to skip the baseline
#   thread                : optional; builds with Homebrew LLVM -fsanitize=thread
set -u
WT=$1; B=$2; BEFORE=${3:-}; SAN=${4:-}
here="$(cd "$(dirname "$0")" && pwd)"
extra=()
if [ "$SAN" = thread ]; then
  extra=(-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ "-DCMAKE_CXX_FLAGS=-include stdlib.h" -DWT_SANITIZE=thread)
fi
cmake -S "$here" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_BUILD="$WT" -DWT_BEFORE="$BEFORE" ${extra[@]+"${extra[@]}"} || exit 1
cmake --build "$B" -j 3 || exit 1
export I03_UFT_PARSER_ROOT="$B" LLVM_PROFILE_FILE=/dev/null
[ "$SAN" = thread ] && export TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0
mkdir -p "$B/run" && cd "$B/run" || exit 1
rc=0
for c in $("$B/uft_parser_cur" --gtest_list_tests | awk '/^  /{print s $1} /^[^ ]/{s=$1}'); do
  perl -e 'alarm 120; exec @ARGV' "$B/uft_parser_cur" --gtest_filter="$c" --gtest_output=xml:"$B/run/cur-$c.xml" > "$B/run/cur-$c.log" 2>&1; ec=$?
  echo "cur $c exit=$ec"; [ $ec -ne 0 ] && rc=1
done
# Baseline cases are red controls: non-zero exit (failure or crash) is expected.
if [ -x "$B/uft_parser_base" ]; then
  for c in $("$B/uft_parser_base" --gtest_list_tests | awk '/^  /{print s $1} /^[^ ]/{s=$1}'); do
    perl -e 'alarm 60; exec @ARGV' "$B/uft_parser_base" --gtest_filter="$c" --gtest_output=xml:"$B/run/base-$c.xml" > "$B/run/base-$c.log" 2>&1
    echo "base $c exit=$? (red control: non-zero expected)"
  done
fi
exit $rc
