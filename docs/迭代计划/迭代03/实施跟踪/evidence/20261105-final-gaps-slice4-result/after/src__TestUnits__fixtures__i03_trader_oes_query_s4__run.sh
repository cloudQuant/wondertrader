#!/bin/sh
# I03-NEW-101 isolated fixture: compiles the current src/TraderOES/TraderOES.cpp
# against the vendored OES 0.17.5.8 headers on macOS/Linux, links the test with
# unresolved OES symbols allowed (TraderOES is never connected, so no OES API
# function is called) and runs each case in its own process.
#
# usage: run.sh <new output dir>
# env:   CXX (default c++), WT_BOOST_INCLUDE, WT_GTEST_ROOT
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/i03_trader_oes_query_s4"
out=${1:?pass a new output directory}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd -P)
CXX=${CXX:-c++}
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/opt/boost/include}
gtest=${WT_GTEST_ROOT:-/opt/homebrew/opt/googletest}
case $(uname -s) in
 Darwin) undefined="-Wl,-undefined,dynamic_lookup" ;;
 *) undefined="-Wl,--unresolved-symbols=ignore-all" ;;
esac
common="-std=gnu++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I$boost -I/opt/homebrew/include -isystem $src_root -I$src_root/API/oesApi0.17.5.8/include -include $src_root/Share/fmt_enum_compat.h"
shasum -a 256 "$src_root/TraderOES/TraderOES.cpp" "$src_root/TraderOES/TraderOES.h" "$fixture/oes_query_test.cpp" "$fixture/run.sh" > "$out/sources.sha256"
# TraderOES is not part of the CMake build; its pre-existing legacy warnings
# (unused parameters, member order, unused private fields) are recorded, not
# promoted to errors. The test TU is compiled with -Werror.
"$CXX" $common -Wall -Wextra -c "$src_root/TraderOES/TraderOES.cpp" -o "$out/TraderOES.o" 2> "$out/trader-oes-warnings.log"
grep -c 'warning:' "$out/trader-oes-warnings.log" > "$out/trader-oes-warning-count.txt" || true
"$CXX" $common -Wall -Wextra -Werror -I"$gtest/include" -c "$fixture/oes_query_test.cpp" -o "$out/oes_query_test.o"
"$CXX" -pthread "$out/TraderOES.o" "$out/oes_query_test.o" -L"$gtest/lib" -lgtest -lgtest_main $undefined -o "$out/oes_query_test"
status=0
for c in $("$out/oes_query_test" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}'); do
 name=$(printf '%s' "$c" | tr '.' '_')
 mkdir -p "$out/run/$name"
 set +e
 (cd "$out/run/$name" && perl -e 'alarm 30; exec @ARGV' -- "$out/oes_query_test" --gtest_filter="$c" --gtest_output="xml:$out/run/$name/result.xml" > "$out/run/$name/output.log" 2>&1)
 rc=$?
 set -e
 printf '%s exit=%s\n' "$c" "$rc" | tee -a "$out/results.txt"
 [ "$rc" -eq 0 ] || status=1
done
exit $status
