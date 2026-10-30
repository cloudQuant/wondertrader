#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
cd "$ROOT"
BIN="$ROOT/build-cov/build_x64/Debug/bin/TestUnits/wt-r16-headers-focused"
/opt/homebrew/bin/ninja -C build-cov \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_timeutils_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_strutil_stdutils_test.cpp.o \
  TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_ini_boostfile_test.cpp.o
/usr/bin/c++ -g -arch arm64 -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
  "$ROOT/build-cov/TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_timeutils_test.cpp.o" \
  "$ROOT/build-cov/TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_strutil_stdutils_test.cpp.o" \
  "$ROOT/build-cov/TestUnits/coverage/CMakeFiles/CoverageTests.dir/r16_share_tools/r16_ini_boostfile_test.cpp.o" \
  -o "$BIN" \
  -L/opt/homebrew/lib -L/usr/local/lib \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  "$ROOT/build-cov/build_x64/Debug/libs/libWTSTools.a" \
  "$ROOT/build-cov/build_x64/Debug/libs/libWTSUtils.a" \
  /usr/local/lib/libiconv.dylib -lboost_filesystem -lboost_thread -ldl -lpthread
"$BIN" --gtest_filter='r16_inihelper.long_distinct_section_keys_keep_independent_values:r16_strutil.standardise_path_empty_string:r16_timeutils.time32_default_state_is_deterministic:r16_timeutils.time32_from_local_time_overwrites_previous_value'
# Full current batch: "$BIN" (captured separately; 78/78 PASS).
