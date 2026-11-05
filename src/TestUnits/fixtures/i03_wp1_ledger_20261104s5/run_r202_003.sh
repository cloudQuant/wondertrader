#!/bin/bash
# BUG-R202-003 (card 8c=A) red/green runner for the exec-units contract test
# src/TestUnits/fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp
# (read only). Compiles that test with TraderAdapter.cpp from <adapter-src-dir>
# (current src/WtCore or a saved pre-change copy), links the non-coverage lib
# build, and runs it next to a copied (not linked) libTraderMocker.dylib.
#   run_r202_003.sh <lib-build-dir> <new-output-dir> <adapter-src-dir>
# Local TraderMocker only; no broker connection.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?lib build dir (WtCore WtDtCore WTSTools WTSUtils TraderMocker)}
out=${2:?new output directory}
adapterDir=${3:?directory holding TraderAdapter.cpp/.h to test}
mkdir "$out"
out=$(cd "$out" && pwd)
cd "$root"
test_src="$root/src/TestUnits/fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp"
mkdir -p "$out/overlay/src/TestUnits/coverage/n01_wtcore_tradepaths"
cp -R "$root/src/WtCore" "$root/src/Share" "$root/src/Includes" "$root/src/WTSTools" "$root/src/WTSUtils" "$root/src/FasterLibs" "$out/overlay/src/"
mkdir -p "$out/overlay/src/TestUnits/coverage"
cp -R "$root/src/TestUnits/coverage/n02_wtcore_engines" "$root/src/TestUnits/coverage/p11_wtcore_traderadapter" "$out/overlay/src/TestUnits/coverage/"
cp "$test_src" "$out/overlay/src/TestUnits/coverage/n01_wtcore_tradepaths/"
cp "$adapterDir/TraderAdapter.cpp" "$adapterDir/TraderAdapter.h" "$out/overlay/src/WtCore/"
if find "$out/overlay" -type l | grep -q .; then echo "symlink in overlay" >&2; exit 3; fi
inc="$out/overlay/src"
shasum -a 256 "$test_src" "$adapterDir/TraderAdapter.cpp" "$adapterDir/TraderAdapter.h" > "$out/source.sha256"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
flags=(-std=c++17 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DBOOST_BIND_GLOBAL_PLACEHOLDERS
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I"$inc" -I"$inc/TestUnits/coverage"
  -I/opt/homebrew/include -I/usr/local/include -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" "${flags[@]}" -c "$inc/TestUnits/coverage/n01_wtcore_tradepaths/r202_003_risk_unknown_code_test.cpp" -o "$out/test.o" > "$out/test-build.log" 2>&1
"$compiler" "${flags[@]}" -c "$inc/WtCore/TraderAdapter.cpp" -o "$out/adapter.o" > "$out/adapter-build.log" 2>&1
for unit in UnresolvedReportJournal DiffOrderOwnershipV1; do
  "$compiler" "${flags[@]}" -c "$inc/WtCore/$unit.cpp" -o "$out/$unit.o" > "$out/$unit-build.log" 2>&1
done
mkdir -p "$out/bin"
"$compiler" -fsanitize=address,undefined "$out/test.o" "$out/adapter.o" "$out/UnresolvedReportJournal.o" "$out/DiffOrderOwnershipV1.o" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  "$build/build_x64/Debug/libs/libWtCore.a" "$build/build_x64/Debug/libs/libWtDtCore.a" \
  "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
  -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/bin/test" > "$out/link.log" 2>&1
cp "$build/build_x64/Debug/bin/libTraderMocker.dylib" "$out/bin/"
shasum -a 256 -c "$out/source.sha256" > "$out/source-check.log"
shasum -a 256 "$out/bin/test" "$out/bin/libTraderMocker.dylib" > "$out/binary.sha256"
set +e
mkdir -p "$out/cwd"
(cd "$out/cwd" && ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /usr/bin/perl -e 'alarm shift; exec @ARGV' 120 "$out/bin/test" --gtest_output=xml:"$out/result.xml" > "$out/test.log" 2>&1)
rc=$?
set -e
echo "$rc" > "$out/exit_code.txt"
tail -20 "$out/test.log"
echo "exit=$rc"
exit $rc
