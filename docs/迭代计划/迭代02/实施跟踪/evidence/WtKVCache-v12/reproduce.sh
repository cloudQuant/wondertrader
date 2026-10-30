#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../../.." && pwd)"
cd "$repo_root"

/opt/homebrew/bin/ninja -C build-cov -j 2 \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_kvcache_test.cpp.o

# Focused link reuses the installed GoogleTest and existing Boost dependencies.
# This is not a clean build of all production libraries or the aggregate suite.
(
  cd build-cov
  /usr/bin/c++ -arch arm64 -fprofile-instr-generate -g \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_kvcache_test.cpp.o \
    /opt/homebrew/lib/libgtest_main.a \
    /opt/homebrew/lib/libgtest.a \
    -L/opt/homebrew/lib -L/usr/local/lib \
    -lboost_filesystem -lboost_thread -liconv -lpthread \
    -o build_x64/Debug/bin/TestUnits/wt-kvcache-focused-v12
)

build-cov/build_x64/Debug/bin/TestUnits/wt-kvcache-focused-v12
