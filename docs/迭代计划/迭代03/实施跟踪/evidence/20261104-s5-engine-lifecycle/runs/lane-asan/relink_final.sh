#!/bin/bash
set -uo pipefail
cd /tmp/wt-i03-s5-engine-lifecycle-run
repo=/Users/yunjinqi/Documents/new_projects/wondertrader/src; snap=/tmp/wt-i03-s5-engine-lifecycle-asan-libs/green-src; libdir=/tmp/wt-i03-s5-engine-lifecycle-asan-libs/green; llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
common=(-std=gnu++17 -fsanitize=address,undefined -fno-omit-frame-pointer -g -include cstdlib -include "$snap/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I "$snap" -I "$snap/Includes" -I "$snap/Share" -I "$snap/WTSTools" -I "$snap/WTSUtils" -isystem /opt/homebrew/opt/boost/include -isystem /usr/local/include -isystem /opt/homebrew/include)
libs=("$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread)
$llvm/clang++ "${common[@]}" -I "$repo/TestUnits/coverage/n02_wtcore_engines" "$repo/TestUnits/coverage/n02_wtcore_engines/s5_live_fee_schedule_test.cpp" "$repo/TestUnits/coverage/n02_wtcore_engines/r41_engine_base_test.cpp" -o asan-lane/green-s5_live_fee_schedule_test "${libs[@]}" > asan-lane/green-s5_live_fee_schedule_test-link.log 2>&1; echo "fee link rc=$?"
$llvm/clang++ "${common[@]}" -I "$repo/TestUnits/coverage/q01_wtcore3" "$repo/TestUnits/coverage/q01_wtcore3/r130_engine_fee_test.cpp" -o asan-lane/green-r130_engine_fee "${libs[@]}" > asan-lane/green-r130-link.log 2>&1; echo "r130 link rc=$?"
rm -rf asan-lane/run-green; ./run_asan_lane.sh green
mkdir -p asan-lane/r130 && cd asan-lane/r130 && rm -f *.xml
# R130 fixtures never release their base-data objects (test-side leaks), so LSan is off here.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ../green-r130_engine_fee --gtest_output=xml:result.xml > out.log 2>&1; echo "r130 exit=$? $(grep -o 'tests="[0-9]*"' result.xml | head -1) $(grep -o 'failures="[0-9]*"' result.xml | head -1) san=$(grep -c 'ERROR: AddressSanitizer\|runtime error' out.log)"
echo RELINK-DONE
