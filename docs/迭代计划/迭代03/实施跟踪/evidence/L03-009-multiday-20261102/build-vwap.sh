#!/bin/zsh
set -eu
root="$1"
out="$2"
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
"$cxx" -std=c++17 -g -O1 -fprofile-instr-generate -fsanitize=address,undefined -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include \
  -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/usr/local/include -I"$root" -I"$root/Includes" -I"$root/Share" -I"$root/WTSTools" -I"$root/WTSUtils" -I"$root/WtExeFact" \
  -include stdlib.h -include "$root/Share/fmt_enum_compat.h" \
  "$root/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp" "$root/WtExeFact/WtVWapExeUnit.cpp" "$root/WtExeFact/WtOrdMon.cpp" /tmp/wt-i03-r43-final/stubs.cpp \
  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSTools.a /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSUtils.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.dylib /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.dylib /usr/local/lib/libiconv.dylib -ldl -lpthread \
  -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib -o "$out"
