#!/bin/sh
set -eu
product=$1
output=$2
/usr/bin/clang++ -std=c++17 -g -O1 -fprofile-instr-generate -fcoverage-mapping -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWT_DT_HELPER_CLOSE_TEST_HOOKS -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -Isrc/WtDtHelper -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h "$product" src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_helper_bar_test.cpp src/TestUnits/coverage/l05_dt_servo_porter/l05_dt_helper_tick_test.cpp /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lzstd -liconv -ldl -lpthread -o "$output"
