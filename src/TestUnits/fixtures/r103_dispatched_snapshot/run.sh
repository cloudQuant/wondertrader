#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
old=${WT_R103_BUILD:-/tmp/ctpm-i03-build}
out=${1:?provide an output directory that does not already exist}
mode=${2:-asan}
if [[ -e "$out" ]]; then
  printf 'Output directory already exists: %s\n' "$out" >&2
  exit 1
fi
mkdir -p "$out"
compiler=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
case "$mode" in
  asan) sanitizer=address,undefined ;;
  tsan) sanitizer=thread ;;
  *) printf 'Unknown mode: %s\n' "$mode" >&2; exit 1 ;;
esac
flags=(-std=c++17 -g -O0 "-fsanitize=$sanitizer" -fno-omit-frame-pointer
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I"$old/TestUnits/coverage/cov_boost_include"
  -I"$root/src/Includes" -I"$root/src/Share" -I"$root/src/WTSTools" -I"$root/src/WTSUtils"
  -I"$root/src" -I/opt/homebrew/include -I/usr/local/include -I"$root/src/WtBtCore"
  -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
cd "$root"
"$compiler" "${flags[@]}" -c src/TestUnits/coverage/q02_btcore3/r103_replayer_last_tick_boundary_test.cpp -o "$out/test.o"
"$compiler" "${flags[@]}" -c src/WtBtCore/HisDataReplayer.cpp -o "$out/replayer.o"
"$compiler" "-fsanitize=$sanitizer" -fprofile-instr-generate "$out/test.o" "$out/replayer.o" \
  "$old/build_x64/Debug/libs/libWtBtCore.a" "$old/build_x64/Debug/libs/libWtDtCore.a" \
  "$old/build_x64/Debug/libs/libWtCore.a" "$old/build_x64/Debug/libs/libWTSTools.a" \
  "$old/build_x64/Debug/libs/libWTSUtils.a" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/prog"
if [[ "$mode" == asan ]]; then
  DYLD_LIBRARY_PATH="$old/build_x64/Debug/bin:/usr/local/lib:/opt/homebrew/lib" \
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
    "$out/prog" '--gtest_filter=R103LastTickBoundaryTest.*'
else
  DYLD_LIBRARY_PATH=/usr/local/lib:/opt/homebrew/lib TSAN_OPTIONS=halt_on_error=1 \
    perl -e 'alarm 30; exec @ARGV' -- "$out/prog" \
    '--gtest_filter=R103LastTickBoundaryTest.ConcurrentSnapshotReadDuringNextDispatch'
fi
