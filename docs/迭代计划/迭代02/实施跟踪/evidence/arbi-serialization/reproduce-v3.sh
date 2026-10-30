#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# compile.log records this run's WtArbiExecuter.cpp.o and r220_arbi_serialization_test.cpp.o rebuilds.
# Factory/helper/ActionPolicy objects and the WtExeFact plugin were already built and are reused.
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtArbiExecuter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtExecuterFactory.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/TraderAdapter.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/WtHelper.cpp.o build-cov/WtCore/CMakeFiles/WtCore.dir/ActionPolicyMgr.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o build-cov/build_x64/Debug/bin/TestUnits/wt-arbi-focused
build-cov/build_x64/Debug/bin/TestUnits/wt-arbi-focused
