#!/bin/bash
set -euo pipefail
cd /Users/yunjinqi/Documents/new_projects/wondertrader
TASK_ROOT=/tmp/wt-i03-r10-audit-20261103
CC=/usr/bin/clang++
ARGS=(-std=c++17 -O1 -g -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include src/Share/fmt_enum_compat.h -I src -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/Cellar/fmt/11.2.0/include -I /opt/homebrew/Cellar/spdlog/1.15.3/include -I /opt/homebrew/Cellar/googletest/1.17.0/include -I /usr/local/include)
set -x
cp /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a "$TASK_ROOT/libs/"
"$CC" "${ARGS[@]}" -fPIC -dynamiclib src/TestUnits/fixtures/r160_fake_wtmsgque.cpp -o "$TASK_ROOT/good/libWtMsgQue.dylib"
"$CC" "${ARGS[@]}" -fPIC -dynamiclib src/TestUnits/fixtures/r10_missing_register_wtmsgque.cpp -o "$TASK_ROOT/missing-register/libWtMsgQue.dylib"
"$CC" "${ARGS[@]}" -fPIC -dynamiclib src/TestUnits/fixtures/r10_missing_create_wtmsgque.cpp -o "$TASK_ROOT/missing-create/libWtMsgQue.dylib"
"$CC" "${ARGS[@]}" -c src/WtCore/EventNotifier.cpp -o "$TASK_ROOT/notifier.o"
"$CC" "${ARGS[@]}" -c src/WtCore/WtHelper.cpp -o "$TASK_ROOT/helper.o"
"$CC" "${ARGS[@]}" -DWT_R160_FAKE_MQ_DIR=\"$TASK_ROOT/good\" -DWT_R10_MISSING_REGISTER_MQ_DIR=\"$TASK_ROOT/missing-register\" -DWT_R10_MISSING_CREATE_MQ_DIR=\"$TASK_ROOT/missing-create\" -c src/TestUnits/coverage/n02_wtcore_engines/r10_event_notifier_missing_register_test.cpp -o "$TASK_ROOT/r10-test.o"
"$CC" "${ARGS[@]}" -DWT_R160_FAKE_MQ_DIR=\"$TASK_ROOT/good\" -c src/TestUnits/coverage/n02_wtcore_engines/r160_event_notifier_test.cpp -o "$TASK_ROOT/r160-test.o"
"$CC" -fprofile-instr-generate -pthread "$TASK_ROOT/notifier.o" "$TASK_ROOT/helper.o" "$TASK_ROOT/r10-test.o" "$TASK_ROOT/r160-test.o" "$TASK_ROOT/libs/libWTSTools.a" "$TASK_ROOT/libs/libWTSUtils.a" /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a -liconv -o "$TASK_ROOT/focused"
