#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?existing isolated build with WtCore static dependencies}
out=${2:?new output directory}
mkdir "$out"
cd "$root"
watched=(src/WtCore/WtEngine.cpp src/WtCore/WtHftEngine.cpp src/WtCore/HftStraBaseCtx.cpp
  src/WtCore/HftStraContext.cpp src/WtCore/TraderAdapter.cpp src/WtCore/WtEngine.h
  src/WtCore/WtHftEngine.h src/WtCore/HftStraBaseCtx.h src/WtCore/HftStraContext.h
  src/WtCore/TraderAdapter.h src/WtCore/ITrdNotifySink.h src/Includes/WTSTradeDef.hpp
  src/Includes/WTSContractInfo.hpp src/TestUnits/fixtures/i03_hft_fee_pipeline/hft_fee_probe.cpp
  src/TestUnits/coverage/n01_wtcore_tradepaths/r71_hft_ctx_test.cpp
  src/TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h)
shasum -a 256 "${watched[@]}" > "$out/source.sha256"
for unit in WtEngine WtHftEngine HftStraBaseCtx HftStraContext TraderAdapter; do
  cp "src/WtCore/$unit.cpp" "$out/$unit.cpp"
done
compiler=/opt/homebrew/opt/llvm/bin/clang++
flags=(-std=c++17 -g -O0 -fsanitize=undefined -fno-sanitize-recover=all
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DBOOST_BIND_GLOBAL_PLACEHOLDERS
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I"$root/src/Includes"
  -I"$root/src/Share" -I"$root/src/WTSTools" -I"$root/src/WTSUtils"
  -I"$root/src" -I"$root/src/WtCore" -I/opt/homebrew/include -I/usr/local/include
  -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" "${flags[@]}" -c src/TestUnits/fixtures/i03_hft_fee_pipeline/hft_fee_probe.cpp -o "$out/test.o"
for unit in WtEngine WtHftEngine HftStraBaseCtx HftStraContext TraderAdapter; do
  "$compiler" "${flags[@]}" -c "$out/$unit.cpp" -o "$out/$unit.o"
done
"$compiler" -fsanitize=undefined -fprofile-instr-generate "$out/test.o" \
  "$out/WtEngine.o" "$out/WtHftEngine.o" "$out/HftStraBaseCtx.o" \
  "$out/HftStraContext.o" "$out/TraderAdapter.o" \
  "$build/build_x64/Debug/libs/libWtCore.a" "$build/build_x64/Debug/libs/libWtDtCore.a" \
  "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test"
shasum -a 256 -c "$out/source.sha256" > "$out/source-check.log"
DYLD_LIBRARY_PATH="/usr/local/lib:/opt/homebrew/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
  UBSAN_OPTIONS=halt_on_error=1 "$out/test" "--gtest_filter=${I03_HFT_FEE_FILTER:-R71HftTest.FeeProbe*:R71HftTest.TradeAccountingAndDynProfitExtremes}"
