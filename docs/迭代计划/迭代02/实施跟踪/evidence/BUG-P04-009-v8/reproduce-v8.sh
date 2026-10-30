#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

# Build the real WtDataStorage module that the focused test dlopens, plus the
# final replay/test objects. This is an incremental focused build, not clean or
# aggregate CoverageTests.
/opt/homebrew/bin/ninja -C build-cov -j 2 \
  build_x64/Debug/bin/libWtDataStorage.dylib \
  WtBtCore/CMakeFiles/WtBtCore.dir/HisDataReplayer.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p04_btcore_replayer/p04_replayer_csv_replay_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/HisDataReplayer.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o

(
  cd build-cov
  /usr/bin/c++ -arch arm64 -g -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/p04_btcore_replayer/p04_replayer_csv_replay_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/HisDataReplayer.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/__/__/WtBtCore/WtHelper.cpp.o \
    build_x64/Debug/libs/libWtBtCore.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    /opt/homebrew/lib/libgtest_main.a \
    /opt/homebrew/lib/libgtest.a \
    -L/opt/homebrew/lib -L/usr/local/lib \
    -lboost_filesystem -lboost_thread -lzstd -liconv -lpthread \
    -o build_x64/Debug/bin/TestUnits/wt-replayer-bin-v8
)

build-cov/build_x64/Debug/bin/TestUnits/wt-replayer-bin-v8 --gtest_color=no \
  --gtest_filter='P04ReplayCsvTest.failedMainBarsLoadDoesNotPoisonReplayAndCanRetry:P04ReplayCsvTest.failedBinaryRawBarsLoadCanBeRetriedSafely'
