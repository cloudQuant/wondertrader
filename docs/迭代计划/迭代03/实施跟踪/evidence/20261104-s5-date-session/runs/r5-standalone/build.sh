#!/bin/sh
ROOT=/Users/yunjinqi/Documents/new_projects/wondertrader
EV="$ROOT/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-date-session/before"
L=/tmp/wt-i03-s5-date-session-cov/build_x64/Debug/libs
for v in new old; do
  src=$ROOT/src/TestUnits/coverage/q05_dtsrv/r5_state_monitor_bdt_test.cpp
  [ $v = old ] && src="$EV/src/TestUnits/coverage/q05_dtsrv/r5_state_monitor_bdt_test.cpp"
  c++ -std=gnu++17 -g -I$ROOT/src -I$ROOT/src/Share -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/include -I/usr/local/include \
    -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include $ROOT/src/Share/fmt_enum_compat.h \
    "$src" $ROOT/src/WtDtCore/StateMonitor.cpp stubs.cpp -o r5_$v -fprofile-instr-generate \
    $L/libWTSTools.a $L/libWTSUtils.a /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
    -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -ldl /usr/local/lib/libiconv.dylib
  echo "$v build=$?"
done
