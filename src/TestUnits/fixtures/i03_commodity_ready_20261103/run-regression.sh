#!/bin/bash
# usage: run-regression.sh <workdir>
# Targeted related tests against the green snapshot/libraries produced by
# `run.sh <workdir> green` (plain build). Not a full regression.
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
work=${1:?workdir}
snap="$work/snap/src"; libdir="$work/build/build_x64/Debug/libs"; out="$work/regression"
mkdir -p "$out"
cov="$root/src/TestUnits/coverage"; old="$root/src/TestUnits/fixtures/l03_commodity_state"
fee="$root/src/TestUnits/fixtures/i03_backtest_fee_consumer_20261103"
shasum -a 256 "$cov"/l03_includes/l03_commodity_info_test.cpp "$cov"/l03_includes/l03_contract_info_test.cpp \
    "$old"/config_gate.cpp "$old"/init.cpp "$fee"/fee_consumer_test.cpp \
    "$cov"/p03_btcore_mockers2/p03_cta_trade_path_test.cpp "$cov"/p03_btcore_mockers2/p03_sel_trade_path_test.cpp \
    "$snap"/Includes/WTSContractInfo.hpp "$snap"/WTSTools/WTSBaseDataMgr.cpp "$snap"/WtBtCore/CtaMocker.cpp "$snap"/WtBtCore/SelMocker.cpp \
    > "$out/inputs.sha256"
common=(-std=c++17 -Wno-deprecated-declarations -I "$snap" -I /opt/homebrew/include -I /opt/local/include -include "$snap/Share/fmt_enum_compat.h")
base=(-L /opt/homebrew/lib -lfmt -lspdlog -lboost_filesystem -lboost_thread -lboost_system -liconv -pthread
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a)
run() { # name binary args...
    local name=$1; shift
    DYLD_LIBRARY_PATH="$work" perl -e 'alarm 120; exec @ARGV' -- "$@" > "$out/$name.log" 2>&1
    echo "$name exit $?" | tee -a "$out/results.txt"
}
: > "$out/results.txt"
/usr/bin/c++ "${common[@]}" "$cov"/l03_includes/l03_commodity_info_test.cpp "$cov"/l03_includes/l03_contract_info_test.cpp \
    -o "$out/l03" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" "${base[@]}" > "$out/l03-build.log" 2>&1 \
    && run l03 "$out/l03" --gtest_output=xml:"$out/l03.xml" || echo "l03 build exit 1" | tee -a "$out/results.txt"
/usr/bin/c++ "${common[@]}" "$old"/config_gate.cpp -o "$out/config_gate" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" "${base[@]}" \
    > "$out/config_gate-build.log" 2>&1 \
    && run config_gate "$out/config_gate" --gtest_filter='L03CommodityReady.*' --gtest_output=xml:"$out/config_gate.xml" \
    || echo "config_gate build exit 1" | tee -a "$out/results.txt"
/usr/bin/c++ -std=c++17 -I "$snap" "$old"/init.cpp -o "$out/init_probe" > "$out/init_probe-build.log" 2>&1 \
    && run init_probe "$out/init_probe" || echo "init_probe build exit 1" | tee -a "$out/results.txt"
bt=("$libdir/libWtBtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" -lnanomsg)
/usr/bin/c++ "${common[@]}" "$fee"/fee_consumer_test.cpp -o "$out/fee_consumer" "${bt[@]}" "${base[@]}" > "$out/fee-build.log" 2>&1 \
    && run fee_consumer "$out/fee_consumer" --gtest_output=xml:"$out/fee_consumer.xml" || echo "fee build exit 1" | tee -a "$out/results.txt"
/usr/bin/c++ "${common[@]}" "$cov"/p03_btcore_mockers2/p03_cta_trade_path_test.cpp "$cov"/p03_btcore_mockers2/p03_sel_trade_path_test.cpp \
    -o "$out/p03" "${bt[@]}" "${base[@]}" > "$out/p03-build.log" 2>&1 \
    && run p03 "$out/p03" --gtest_filter='P03CtaTradePath.FuturesOpenCloseTodayCloseYesterdayReversal:P03CtaTradePath.FixedSlippageAppliedToBothSides:P03CtaTradePath.StockT1FrozenAndShortRejected:P03CtaTradePath.ConditionalEntrustTriggeredByTickRange:P03SelTradePath.*' \
       --gtest_output=xml:"$out/p03.xml" || echo "p03 build exit 1" | tee -a "$out/results.txt"
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
exit 0
