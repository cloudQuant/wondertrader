#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
/opt/homebrew/bin/cmake --build build-cov --target WtBtCore -j 4
/opt/homebrew/bin/ninja -C build-cov -j 4 TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/n04_btcore_mockers/r43_match_engine_flow_test.cpp.o
/usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup 'docs/迭代计划/迭代02/实施跟踪/evidence/match-code-isolation/test_id_stub.cpp' build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/n04_btcore_mockers/r43_match_engine_flow_test.cpp.o build-cov/WtBtCore/CMakeFiles/WtBtCore.dir/MatchEngine.cpp.o /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a build-cov/build_x64/Debug/libs/libWTSTools.a build-cov/build_x64/Debug/libs/libWTSUtils.a /usr/local/lib/libiconv.dylib -L/opt/homebrew/lib -lboost_filesystem -lboost_thread -lpthread -o /tmp/wt-match-focused
mkdir -p /tmp/wt-iter02-match-focused
cd /tmp/wt-iter02-match-focused
/tmp/wt-match-focused
