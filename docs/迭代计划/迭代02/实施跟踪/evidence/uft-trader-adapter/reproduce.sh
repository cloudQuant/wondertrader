#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# compile.log records rebuilding the UFT TraderAdapter and its lifecycle test object.
# WtCore/WtHelper.cpp.o is an existing build-cov artifact.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_uftcore.dir/q04_uftcore3/n01_uft_trader_adapter_lifecycle_test.cpp.o build-cov/WtUftCore/CMakeFiles/WtUftCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o /tmp/wt-uft-trader-focused
/tmp/wt-uft-trader-focused
