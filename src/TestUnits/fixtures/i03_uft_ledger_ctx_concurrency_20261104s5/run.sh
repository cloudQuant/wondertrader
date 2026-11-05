#!/bin/bash
# Builds and runs the WtUftCore ledger / context concurrency fixture (BUG-Q04-002/Q04-007).
# usage: run.sh <wt-coverage-build-dir> <fixture-build-dir> <before-dir> <thread|address,undefined>
#   before-dir: directory holding src/WtUftCore/<file> pre-change copies (red control), or ""
set -u
WT=$1; B=$2; BEFORE=${3:-}; SAN=${4:-thread}
here="$(cd "$(dirname "$0")" && pwd)"
cmake -S "$here" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_BUILD="$WT" -DWT_BEFORE="$BEFORE" \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ "-DCMAKE_CXX_FLAGS=-include stdlib.h" \
  "-DWT_SANITIZE=$SAN" || exit 1
cmake --build "$B" -j 3 || exit 1
if find "$B" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
export LLVM_PROFILE_FILE=/dev/null TSAN_OPTIONS=halt_on_error=1:report_thread_leaks=0 \
  ASAN_OPTIONS=detect_container_overflow=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
rc=0
for bin in uft_cc_cur uft_cc_base; do
  [ -x "$B/$bin" ] || continue
  for c in $("$B/$bin" --gtest_list_tests | awk '/^  /{print s $1} /^[^ ]/{s=$1}'); do
    d="$B/run/$bin-$c"; rm -rf "$d"; mkdir -p "$d"
    (cd "$d" && perl -e 'alarm 300; exec @ARGV' "$B/$bin" --gtest_filter="$c" --gtest_output=xml:"$d/result.xml" > "$d/out.log" 2>&1); ec=$?
    if [ "$bin" = uft_cc_cur ]; then echo "cur $c exit=$ec"; [ $ec -ne 0 ] && rc=1;
    else echo "base $c exit=$ec (red control: non-zero expected for the context case)"; fi
  done
done
exit $rc
