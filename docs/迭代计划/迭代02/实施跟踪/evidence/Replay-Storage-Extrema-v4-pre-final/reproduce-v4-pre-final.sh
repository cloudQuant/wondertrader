#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

/opt/homebrew/bin/ninja -C build-cov -j 2 \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/l06_wtbtcore/l06_match_engine_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/MatchEngine.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p04_btcore_replayer/p04_replayer_csv_replay_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/HisDataReplayer.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r23_storage4/r23_rdm_count_slice_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtRdmDtReader.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtDataReader.cpp.o

(
  cd build-cov
  common_args=(
    /opt/homebrew/lib/libgtest_main.a
    /opt/homebrew/lib/libgtest.a
    -L/opt/homebrew/lib -L/usr/local/lib
    -lboost_filesystem -lboost_thread -lzstd -liconv -lpthread
  )

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/l06_wtbtcore/l06_match_engine_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/MatchEngine.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
    build_x64/Debug/libs/libWtBtCore.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-match-batch-v4

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p04_btcore_replayer/p04_replayer_csv_replay_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/HisDataReplayer.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
    build_x64/Debug/libs/libWtBtCore.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-replayer-batch-v4

  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r23_storage4/r23_rdm_count_slice_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtRdmDtReader.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/__/__/WtDataStorage/WtDataReader.cpp.o \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    "${common_args[@]}" \
    -o build_x64/Debug/bin/TestUnits/wt-rdm-batch-v4
)

build-cov/build_x64/Debug/bin/TestUnits/wt-match-batch-v4
build-cov/build_x64/Debug/bin/TestUnits/wt-replayer-batch-v4 \
  --gtest_filter='P04ReplayCsvTest.failedMainBarsLoadDoesNotPoisonReplayAndCanRetry'
build-cov/build_x64/Debug/bin/TestUnits/wt-rdm-batch-v4
