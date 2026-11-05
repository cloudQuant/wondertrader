#!/bin/bash
# usage: link_lane_tests.sh <phase> <libdir> <snap-src> <outdir>
# Compiles the two s5 lane tests (NEW-130, I03-NEW-008) plus the R41 lane
# environment TU with LLVM 22 ASan/UBSan against one ASan build of WtCore.
set -uo pipefail
phase=$1; libdir=$2; snap=$3; out=$4
repo=/Users/yunjinqi/Documents/new_projects/wondertrader/src
llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
mkdir -p "$out"
for t in s5_engine_uninit_metadata_test s5_live_fee_schedule_test; do
  extra=
  # The fee test relies on the lane generate-dir environment registered in r41_engine_base_test.cpp.
  [[ $t == s5_live_fee_schedule_test ]] && extra="$repo/TestUnits/coverage/n02_wtcore_engines/r41_engine_base_test.cpp"
  $llvm/clang++ -std=gnu++17 $sflags -include cstdlib -include "$snap/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
    -I "$snap" -I "$snap/Includes" -I "$snap/Share" -I "$snap/WTSTools" -I "$snap/WTSUtils" \
    -I "$repo/TestUnits/coverage/n02_wtcore_engines" \
    -isystem /opt/homebrew/opt/boost/include -isystem /usr/local/include -isystem /opt/homebrew/include \
    "$repo/TestUnits/coverage/n02_wtcore_engines/$t.cpp" $extra -o "$out/$phase-$t" \
    "$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" \
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib \
    -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread > "$out/$phase-$t-link.log" 2>&1
  echo "$phase $t link rc=$?"
done
