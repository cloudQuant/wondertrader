#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?fresh build with WtCore and dependencies}
out=${2:?new output directory}
mkdir "$out"
out=$(cd "$out" && pwd)
cd "$root"
watched=(src/WtCore/TraderAdapter.cpp src/WtCore/TraderAdapter.h src/WtCore/UnresolvedReportJournal.cpp
  src/WtCore/UnresolvedReportJournal.h src/Includes/WTSTradeDef.hpp src/Share/FrequencyWindow.hpp
  src/TestUnits/fixtures/i03_report_quarantine_20261103/check.cpp
  src/TestUnits/fixtures/i03_report_quarantine_20261103/fake_provider.cpp)
shasum -a 256 "${watched[@]}" > "$out/source.sha256"
compiler=/opt/homebrew/opt/llvm/bin/clang++
flags=(-std=c++17 -g -O0 -fsanitize=undefined -fno-sanitize-recover=all
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DBOOST_BIND_GLOBAL_PLACEHOLDERS
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I"$root/src/Includes" -I"$root/src/Share"
  -I"$root/src/WTSTools" -I"$root/src/WTSUtils" -I"$root/src" -I"$root/src/WtCore"
  -I/opt/homebrew/include -I/usr/local/include -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" "${flags[@]}" -dynamiclib src/TestUnits/fixtures/i03_report_quarantine_20261103/fake_provider.cpp -o "$out/libI03ReportFake.dylib" > "$out/plugin-build.log" 2>&1
"$compiler" "${flags[@]}" -c src/TestUnits/fixtures/i03_report_quarantine_20261103/check.cpp -o "$out/test.o" > "$out/test-build.log" 2>&1
for unit in TraderAdapter UnresolvedReportJournal; do
  "$compiler" "${flags[@]}" -c "src/WtCore/$unit.cpp" -o "$out/$unit.o" > "$out/$unit-build.log" 2>&1
done
"$compiler" -fsanitize=undefined "$out/test.o" "$out/TraderAdapter.o" "$out/UnresolvedReportJournal.o" \
  "$build/build_x64/Debug/libs/libWtCore.a" "$build/build_x64/Debug/libs/libWtDtCore.a" \
  "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test" > "$out/link.log" 2>&1
shasum -a 256 -c "$out/source.sha256" > "$out/source-check.log"
I03_REPORT_FAKE_PLUGIN="$out/libI03ReportFake.dylib" UBSAN_OPTIONS=halt_on_error=1 \
  /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python -c \
  'import os, subprocess, sys; subprocess.run([sys.argv[1], "--gtest_filter=I03Quarantine.*"], cwd=sys.argv[2], timeout=30, check=True, env=os.environ.copy())' \
  "$out/test" "$out" > "$out/test.log" 2>&1
cat "$out/test.log"
