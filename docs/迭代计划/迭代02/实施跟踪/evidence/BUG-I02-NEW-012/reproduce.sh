#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

/opt/homebrew/bin/ninja -C build-cov -j 2 \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_ini_boostfile_test.cpp.o

# Focused link reuses existing WTSTools/WTSUtils libraries in build-cov.
# This does not perform a clean or complete aggregate build.
(
  cd build-cov
  /usr/bin/c++ -arch arm64 -fprofile-instr-generate -g \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_ini_boostfile_test.cpp.o \
    /opt/homebrew/lib/libgtest_main.a \
    /opt/homebrew/lib/libgtest.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    -L/opt/homebrew/lib -L/usr/local/lib \
    -lboost_filesystem -lboost_thread -liconv -lpthread \
    -o build_x64/Debug/bin/TestUnits/wt-ini-boostfile-focused-v3
)

build-cov/build_x64/Debug/bin/TestUnits/wt-ini-boostfile-focused-v3
