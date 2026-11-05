#!/bin/bash
# usage: ticker075.sh <label> <libdir> <hdr> <asan|tsan> <extra test cpp...>
set -uo pipefail
label=$1; libdir=$2; hdr=$3; san=$4; shift 4
repo=/Users/yunjinqi/Documents/new_projects/wondertrader/src
llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
out=/tmp/wt-i03-s5-engine-lifecycle-run/ticker075; mkdir -p $out
if [[ $san == tsan ]]; then sflags="-fsanitize=thread -fno-omit-frame-pointer -g"; else sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"; fi
$llvm/clang++ -std=gnu++17 $sflags -include cstdlib -include "$hdr/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I "$hdr" -I "$hdr/Includes" -I "$hdr/Share" -I "$hdr/WTSTools" -I "$hdr/WTSUtils" -I "$repo/TestUnits/coverage/n02_wtcore_engines" \
  -isystem /opt/homebrew/opt/boost/include -isystem /usr/local/include -isystem /opt/homebrew/include \
  "$repo/TestUnits/coverage/n02_wtcore_engines/s5_ticker_null_session_test.cpp" "$repo/TestUnits/coverage/n02_wtcore_engines/r41_engine_base_test.cpp" "$@" \
  -o $out/$label "$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread > $out/$label-link.log 2>&1 || { echo "link failed"; tail -5 $out/$label-link.log; exit 11; }
shasum -a 256 $out/$label "$libdir"/*.a > $out/$label-binaries.sha256
: > $out/$label-results.txt
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1
for t in $($out/$label --gtest_list_tests --gtest_filter='*S5*Ticker*' | awk '/^[^ ]/{s=$1} /^  /{print s $1}'); do
  d=$out/$label-run/$t; mkdir -p $d
  (cd $d && ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 perl -e 'alarm shift; exec @ARGV or exit 127' 120 $out/$label --gtest_filter="$t" --gtest_output=xml:$d/result.xml > $d/out.log 2>&1)
  rc=$?; echo "$t exit=$rc sanitizer_reports=$(grep -c 'ERROR: AddressSanitizer\|runtime error\|WARNING: ThreadSanitizer' $d/out.log)" | tee -a $out/$label-results.txt
done
echo TICKER-DONE
