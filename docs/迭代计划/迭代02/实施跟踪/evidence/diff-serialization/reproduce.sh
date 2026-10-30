#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# Latest quiesce-stage source/test/R120 object build output is in build-v13-quiesce-stage.log.
# The 24/24 assertions are not acceptance: review found unknown-code broadcast/ledger semantics
# under repair. Earlier 21/21, 18/18 and previous runs remain archived by version.
# The v5 Ninja invocation failed overall on an unrelated unused-variable -Werror after
# the R120 test object and WtDiffExecuter.cpp.o compile steps had succeeded.
# WtExecuterFactory.cpp.o, TraderAdapter.cpp.o, WtHelper.cpp.o, and ActionPolicyMgr.cpp.o were existing artifacts.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/n02_diff_concurrency_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtDiffExecuter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtExecuterFactory.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/ActionPolicyMgr.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o build-cov/build_x64/Debug/bin/TestUnits/wt-diff-focused
build-cov/build_x64/Debug/bin/TestUnits/wt-diff-focused
