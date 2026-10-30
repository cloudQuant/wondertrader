#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
/opt/homebrew/bin/ninja -C build-cov -j 4 WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n01_wtcore_tradepaths/n01_trader_adapter_subscribe_concurrency_test.cpp.o
# WtHelper.cpp.o is an existing build-cov artifact; it was reused, not rebuilt in this run.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n01_wtcore_tradepaths/n01_trader_adapter_subscribe_concurrency_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o /tmp/wt-trader-subscribe-focused
/tmp/wt-trader-subscribe-focused
