#!/bin/sh
# usage: build.sh <name> <test.cpp>
ROOT=/Users/yunjinqi/Documents/new_projects/wondertrader
L=/tmp/wt-i03-s5-date-session-cov/build_x64/Debug/libs
c++ -std=gnu++17 -g -I$ROOT/src -I$ROOT/src/Share -I$ROOT/src/Includes -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/include -I/usr/local/include \
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include $ROOT/src/Share/fmt_enum_compat.h \
  "$2" $3 -o "$1" -fprofile-instr-generate \
  $L/libWTSTools.a $L/libWTSUtils.a /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -ldl /usr/local/lib/libiconv.dylib
echo "$1 build=$?"
