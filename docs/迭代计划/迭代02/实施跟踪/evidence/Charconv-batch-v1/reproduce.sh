#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

/opt/homebrew/bin/ninja -C build-cov -j 2 \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_charconv_safe_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/l01_share_wtsutils/l01_converter_charconv_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r55_codehelper_charconv_test.cpp.o

(
  cd build-cov
  /usr/bin/c++ -arch arm64 -fprofile-instr-generate -g -Wl,-undefined,dynamic_lookup \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r12_headers/r12_charconv_safe_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/l01_share_wtsutils/l01_converter_charconv_test.cpp.o \
    TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r55_codehelper_charconv_test.cpp.o \
    /opt/homebrew/lib/libgtest_main.a \
    /opt/homebrew/lib/libgtest.a \
    build_x64/Debug/libs/libWTSTools.a \
    build_x64/Debug/libs/libWTSUtils.a \
    -L/opt/homebrew/lib -L/usr/local/lib \
    -lboost_filesystem -lboost_thread -liconv -lpthread \
    -o build_x64/Debug/bin/TestUnits/wt-charconv-focused
)

"$repo_root/build-cov/build_x64/Debug/bin/TestUnits/wt-charconv-focused" \
  --gtest_color=no \
  --gtest_filter='R55_charconv.*:r12_charconv_encoding_helper.*:l01_charconv.*'
