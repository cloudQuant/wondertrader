#!/bin/bash
set -euo pipefail
root=/Users/yunjinqi/Documents/new_projects/wondertrader
b=/tmp/wt-i03-r103-between
old=/tmp/ctpm-i03-build
compiler=/opt/homebrew/opt/llvm/bin/clang++
flags=(-std=c++17 -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I"$old/TestUnits/coverage/cov_boost_include" -I"$root/src/Includes" -I"$root/src/Share" -I"$root/src/WTSTools" -I"$root/src/WTSUtils" -I"$root/src" -I/opt/homebrew/include -I/usr/local/include -I"$root/src/WtBtCore" -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" "${flags[@]}" -c "$root/src/TestUnits/coverage/q02_btcore3/r103_replayer_last_tick_boundary_test.cpp" -o "$b/test.o"
"$compiler" "${flags[@]}" -c "$b/baseline/HisDataReplayer.cpp" -o "$b/baseline.o"
"$compiler" "${flags[@]}" -c "$root/src/WtBtCore/HisDataReplayer.cpp" -o "$b/final.o"
for which in baseline final; do
  "$compiler" -fsanitize=address,undefined -fprofile-instr-generate "$b/test.o" "$b/$which.o" "$old/build_x64/Debug/libs/libWtBtCore.a" "$old/build_x64/Debug/libs/libWtDtCore.a" "$old/build_x64/Debug/libs/libWtCore.a" "$old/build_x64/Debug/libs/libWTSTools.a" "$old/build_x64/Debug/libs/libWTSUtils.a" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$b/prog-$which"
done
