#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
out=${1:?provide a new output directory}
if [[ -e "$out" || -L "$out" ]]; then echo "output exists: $out" >&2; exit 1; fi
mkdir -p "$out"
inputs=(src/TraderCTP/TraderCTP.cpp src/TraderCTP/TraderCTP.h src/WtCore/TraderAdapter.cpp src/WtCore/TraderAdapter.h src/TestUnits/coverage/t_TraderCTP/t25_traderctp_gap_test.cpp src/TestUnits/coverage/t_TraderCTP/t25_ctp_api_stub.hpp src/TestUnits/coverage/p11_wtcore_traderadapter/p11_push_notify_test.cpp src/TestUnits/fixtures/i03_ctp_order_identity_20261103/ctp_identity_test.cpp src/TestUnits/fixtures/i03_ctp_order_identity_20261103/CMakeLists.txt src/WTSTools/WTSDataFactory.cpp)
shasum -a 256 "${inputs[@]}" > "$out/before.sha256"
cmake -S src -B "$out/production" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-Wno-error=unused-parameter > "$out/production-config.log" 2>&1
cmake --build "$out/production" --target TraderCTP WtCore WTSTools WTSUtils -j2 > "$out/production-build.log" 2>&1
cmake -S src/TestUnits/fixtures/i03_ctp_order_identity_20261103 -B "$out/fixture" -DWT_DEP_BUILD="$out/production" -DCMAKE_BUILD_TYPE=Debug -DGTest_DIR=/opt/homebrew/lib/cmake/GTest > "$out/fixture-config.log" 2>&1
cmake --build "$out/fixture" -j2 > "$out/fixture-build.log" 2>&1
"$out/fixture/i03_ctp_identity" --gtest_filter='T25TraderCTPTest.I03Identity*:T25TraderCTPTest.order_insert_and_action_send_failures_return_error_and_are_logged:T25TraderCTPTest.order_insert_without_usertag_allocates_identity_and_cache:T25TraderCTPTest.order_insert_rejects_malformed_entrust_id_when_usertag_set:T25TraderCTPTest.order_insert_wraps_bestprice_lastprice_and_every_offset:T25TraderCTPTest.order_insert_direction_flag_is_swapped_for_short_close:T25TraderCTPTest.usertag_round_trips_from_eid_cache_into_order_then_trade' | tee "$out/ctp-tests.log"
/usr/bin/c++ -std=c++17 -Wno-deprecated-declarations -I src -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/include -I /opt/local/include -include src/Share/fmt_enum_compat.h src/TestUnits/coverage/p11_wtcore_traderadapter/p11_push_notify_test.cpp -o "$out/host_tag_tests" -L "$out/production/build_x64/Debug/libs" -lWtCore -lWTSTools -lWTSUtils /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -lfmt -lspdlog -lboost_filesystem -lboost_thread -lboost_system -lnanomsg -liconv -pthread > "$out/host-build.log" 2>&1
cp /usr/local/lib/libnanomsg.6.0.1.dylib "$out/libnanomsg.6.dylib"
DYLD_LIBRARY_PATH="$out" "$out/host_tag_tests" --gtest_filter='P11PushNotify.entrust_error_updates_undone_once' | tee "$out/host-tests.log"
shasum -a 256 "${inputs[@]}" > "$out/after.sha256"
cmp "$out/before.sha256" "$out/after.sha256"
if find "$out" -type l -print -quit | grep -q .; then echo 'unexpected symlink in build output' >&2; exit 1; fi
