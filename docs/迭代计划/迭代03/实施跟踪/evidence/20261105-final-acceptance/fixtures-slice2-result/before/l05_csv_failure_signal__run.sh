#!/bin/sh
set -eu
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
"$compiler" -fsanitize=address,undefined -fno-omit-frame-pointer \
  -isystem /usr/local/include -I/opt/homebrew/Cellar/zstd/1.5.7_1/include \
  -I/opt/homebrew/Cellar/spdlog/1.15.3/include -std=c++17 -g -O1 \
  -fprofile-instr-generate -fcoverage-mapping -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -DWT_DT_HELPER_CLOSE_TEST_HOOKS -I/opt/homebrew/Cellar/boost/1.88.0/include \
  -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include \
  -Isrc/WtDtHelper -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils \
  -include stdlib.h -include src/Share/fmt_enum_compat.h \
  src/WtDtHelper/WtDtHelper.cpp src/TestUnits/fixtures/l05_csv_failure_signal/probe.cpp \
  /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.dylib \
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.dylib \
  /opt/homebrew/Cellar/zstd/1.5.7_1/lib/libzstd.1.5.7.dylib -liconv -ldl -lpthread -o "$out/probe"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
LLVM_PROFILE_FILE="$out/green-%p.profraw" "$out/probe" --gtest_filter='*-*.KnownRedNullLoggerFailureNotObservable/*' > "$out/green.log" 2>&1
set +e
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
LLVM_PROFILE_FILE="$out/red-%p.profraw" "$out/probe" --gtest_filter='*.KnownRedNullLoggerFailureNotObservable/*' > "$out/red.log" 2>&1
status=$?
set -e
printf '%s\n' "$status" > "$out/red.exit"
test "$status" -eq 1
