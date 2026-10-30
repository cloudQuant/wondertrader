#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# R120 test object replaces n02_diff_concurrency_test.cpp.o; WtDtMgr.cpp.o is added.
# Latest stage 2/2 evidence rebuilt the R120 test object and WtDiffExecuter.cpp.o
# in ../diff-serialization/build-v13-quiesce-stage.log.
# This 2/2 stage result does not cover the reviewed unknown-code broadcast/ledger regression.
# The v2 SIGSEGV and crash report are retained; the ABI-layout explanation is only a hypothesis.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/r120_diff_executer_unit_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtDiffExecuter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtExecuterFactory.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/ActionPolicyMgr.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtDtMgr.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o build-cov/build_x64/Debug/bin/TestUnits/wt-r120-diff-focused
build-cov/build_x64/Debug/bin/TestUnits/wt-r120-diff-focused
