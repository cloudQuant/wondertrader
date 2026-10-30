#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

# Compile only the focused production/test objects for R160; this does not
# build or run the aggregate CoverageTests_wtcore suite.
/opt/homebrew/bin/ninja -C build-cov \
  WtCore/CMakeFiles/WtCore.dir/WtHftTicker.cpp.o \
  WtCore/CMakeFiles/WtCore.dir/WtHftEngine.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/r160_hft_ticker_test.cpp.o

# Manually link the three focused objects. dynamic_lookup is intentional for
# this test-only binary and is not evidence of aggregate-link completeness.
/usr/bin/c++ -g -arch arm64 -fprofile-instr-generate \
  -Wl,-undefined,dynamic_lookup \
  build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_wtcore.dir/n02_wtcore_engines/r160_hft_ticker_test.cpp.o \
  build-cov/WtCore/CMakeFiles/WtCore.dir/WtHftTicker.cpp.o \
  build-cov/WtCore/CMakeFiles/WtCore.dir/WtHftEngine.cpp.o \
  -o build-cov/build_x64/Debug/bin/TestUnits/wt-hft-ticker-focused \
  -L/opt/homebrew/lib -L/usr/local/lib \
  /opt/homebrew/lib/libgtest_main.a \
  build-cov/build_x64/Debug/libs/libWtCore.a \
  build-cov/build_x64/Debug/libs/libWTSTools.a \
  build-cov/build_x64/Debug/libs/libWTSUtils.a \
  /opt/homebrew/lib/libgtest.a \
  /usr/local/lib/libiconv.dylib \
  -lboost_filesystem -lboost_thread -ldl -lpthread

build-cov/build_x64/Debug/bin/TestUnits/wt-hft-ticker-focused --gtest_color=no
