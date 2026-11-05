#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
build=${1:-/tmp/wt-i03-ctasel-fee-20261103}
mkdir -p "$build"
cmake -S src -B "$build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$build/configure.log" 2>&1
cmake --build "$build" --target WtBtCore WTSTools WTSUtils -j2 > "$build/build.log" 2>&1
cp /usr/local/lib/libnanomsg.6.0.1.dylib "$build/libnanomsg.6.dylib"
common=(-std=c++17 -Wno-deprecated-declarations -I src -I /opt/homebrew/include -I /opt/local/include -include src/Share/fmt_enum_compat.h)
libs=(-L "$build/build_x64/Debug/libs" -lWtBtCore -lWTSTools -lWTSUtils /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -lfmt -lspdlog -lboost_filesystem -lboost_thread -lboost_system -lnanomsg -liconv -pthread)
/usr/bin/c++ "${common[@]}" src/TestUnits/fixtures/i03_backtest_fee_consumer_20261103/fee_consumer_test.cpp -o "$build/fee_consumer_test" "${libs[@]}"
/usr/bin/c++ "${common[@]}" src/TestUnits/coverage/p03_btcore_mockers2/p03_cta_trade_path_test.cpp src/TestUnits/coverage/p03_btcore_mockers2/p03_sel_trade_path_test.cpp -o "$build/p03_fee_tests" "${libs[@]}"
DYLD_LIBRARY_PATH="$build" "$build/fee_consumer_test" | tee "$build/fee-consumer-final.log"
DYLD_LIBRARY_PATH="$build" "$build/p03_fee_tests" --gtest_filter='P03CtaTradePath.FuturesOpenCloseTodayCloseYesterdayReversal:P03CtaTradePath.FixedSlippageAppliedToBothSides:P03CtaTradePath.StockT1FrozenAndShortRejected:P03CtaTradePath.ConditionalEntrustTriggeredByTickRange:P03SelTradePath.*' | tee "$build/p03-final.log"
shasum -a 256 src/WtBtCore/{CtaMocker.cpp,CtaMocker.h,SelMocker.cpp,SelMocker.h,HisDataReplayer.cpp,HisDataReplayer.h} src/Share/FeeSchedule.hpp src/TestUnits/coverage/p03_btcore_mockers2/p03_{cta,sel}_trade_path_test.cpp src/TestUnits/fixtures/i03_backtest_fee_consumer_20261103/fee_consumer_test.cpp > "$build/source.sha256"
if find "$build" -type l -print -quit | grep -q .; then echo 'unexpected symbolic link in build directory' >&2; exit 1; fi
