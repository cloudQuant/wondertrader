#!/bin/bash
set -euo pipefail
work=${1:?provide the existing integrated snapshot/build directory}
src="$work/snapshot/src"
libs="$work/build/build_x64/Debug/libs"
compiler=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
"$compiler" -std=c++17 -g -O0 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I"$src" -I"$src/Includes" -I"$src/Share" \
  -I"$src/WTSTools" -I"$src/WTSUtils" -I"$src/WtBtCore" -I/opt/homebrew/include -I/usr/local/include \
  -include cstdlib -include "$src/Share/fmt_enum_compat.h" \
  "$src/TestUnits/coverage/q02_btcore3/r103_replayer_last_tick_boundary_test.cpp" \
  "$libs/libWtBtCore.a" "$libs/libWtDtCore.a" "$libs/libWtCore.a" "$libs/libWTSTools.a" "$libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L/opt/homebrew/lib -L/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$work/prog-review"
DYLD_LIBRARY_PATH="$work/build/build_x64/Debug/bin:/usr/local/lib:/opt/homebrew/lib" \
  "$work/prog-review" '--gtest_filter=R103LastTickBoundaryTest.*'
