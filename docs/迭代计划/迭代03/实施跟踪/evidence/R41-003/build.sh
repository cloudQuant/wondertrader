#!/bin/bash
set -euo pipefail
cd /Users/yunjinqi/Documents/new_projects/wondertrader
CC=/usr/bin/clang++
TASK_ROOT=/tmp/wt-i03-r41-stop
ARGS=(-std=c++17 -O1 -g -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include src/Share/fmt_enum_compat.h -I src -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/Cellar/fmt/11.2.0/include -I /opt/homebrew/Cellar/spdlog/1.15.3/include -I /opt/homebrew/Cellar/googletest/1.17.0/include -I /usr/local/include)
set -x
"$CC" "${ARGS[@]}" -fPIC -dynamiclib src/TestUnits/fixtures/r160_fake_wtmsgque.cpp -o "$TASK_ROOT/fake/libWtMsgQue.dylib"
"$CC" "${ARGS[@]}" -c src/WtCore/EventNotifier.cpp -o "$TASK_ROOT/notifier.o"
"$CC" "${ARGS[@]}" -c src/WtCore/WtHelper.cpp -o "$TASK_ROOT/helper.o"
"$CC" "${ARGS[@]}" -DWT_R160_FAKE_MQ_DIR=\"/tmp/wt-i03-r41-stop/fake\" -c src/TestUnits/coverage/n02_wtcore_engines/r160_event_notifier_test.cpp -o "$TASK_ROOT/test.o"
"$CC" -fprofile-instr-generate -pthread "$TASK_ROOT/notifier.o" "$TASK_ROOT/helper.o" "$TASK_ROOT/test.o" "$TASK_ROOT/libs/libWTSTools.a" "$TASK_ROOT/libs/libWTSUtils.a" /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a -liconv -o "$TASK_ROOT/focused"
