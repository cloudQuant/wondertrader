#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/n02_exec_mgr_quiesce_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtExecMgr.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o /tmp/wt-exec-quiesce-focused
/tmp/wt-exec-quiesce-focused
