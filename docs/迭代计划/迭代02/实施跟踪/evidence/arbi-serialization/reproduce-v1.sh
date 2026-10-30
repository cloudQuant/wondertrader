#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# This is the recorded direct-link invocation. Compile.log records the four objects built for this run;
# WtExecuterFactory.cpp.o, TraderAdapter.cpp.o, WtHelper.cpp.o, and WtExeFact were existing artifacts.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/n02_diff_concurrency_test.cpp.o build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtDiffExecuter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtArbiExecuter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtExecuterFactory.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o build-cov/build_x64/Debug/bin/TestUnits/wt-diff-arbi-focused
build-cov/build_x64/Debug/bin/TestUnits/wt-diff-arbi-focused --gtest_filter='WtArbiExecuterSerialization.*'
