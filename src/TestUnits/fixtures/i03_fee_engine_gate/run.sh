#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?existing isolated build containing the project static libraries}
out=${2:?new output directory}
engine_source=${I03_FEE_ENGINE_SOURCE:-src/WtCore/WtEngine.cpp}
mkdir "$out"
cd "$root"
compiler=/opt/homebrew/opt/llvm/bin/clang++
flags=(-std=c++17 -g -O0 -fsanitize=undefined -fno-sanitize-recover=all
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
  -I"$build/TestUnits/coverage/cov_boost_include" -I"$root/src/Includes"
  -I"$root/src/Share" -I"$root/src/WTSTools" -I"$root/src/WTSUtils"
  -I"$root/src" -I"$root/src/WtCore" -I/opt/homebrew/include -I/usr/local/include
  -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" "${flags[@]}" -c src/TestUnits/fixtures/i03_fee_engine_gate/fee_engine_gate.cpp -o "$out/test.o"
"$compiler" "${flags[@]}" -c "$engine_source" -o "$out/WtEngine.o"
"$compiler" -fsanitize=undefined -fprofile-instr-generate "$out/test.o" "$out/WtEngine.o" \
  "$build/build_x64/Debug/libs/libWtCore.a" "$build/build_x64/Debug/libs/libWtDtCore.a" \
  "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test"
DYLD_LIBRARY_PATH="/usr/local/lib:/opt/homebrew/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
  UBSAN_OPTIONS=halt_on_error=1 "$out/test" "--gtest_filter=${I03_FEE_TEST_FILTER:-I03FeeEngineGate.*:R130EngineFee.*}"
