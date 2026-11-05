#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out/current/CTPLoader" "$out/current/API/CTP6.6.9"
out=$(cd "$out" && pwd)
cp -RL src/Share src/Includes src/WTSUtils src/FasterLibs "$out/current/"
cp src/API/CTP6.6.9/*.h "$out/current/API/CTP6.6.9/"
cp src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h "$out/current/CTPLoader/"
cp -RL "$out/current/" "$out/baseline/"
# 2026-11-05: the red control no longer reverse-applies baseline-*.patch to the moving
# working tree (it drifted after T26-011, card 14 and K02=B). baseline/ holds the fixed
# pre-T26-011 TraderSpi.{cpp,h}: commit ccaad010 + t26_fees_atomic/evidence-20261102
# T26-007 patches (065f27b0... / 903cea67...); forward-applying baseline-*.patch to it
# yields the 2026-11-02 T26-011 candidate (dd4640b5... / aff40149...). Hashes are checked.
check() { echo "$2  $1" | shasum -a 256 -c - >/dev/null || { echo "baseline hash mismatch: $1" >&2; exit 3; }; }
cp src/TestUnits/fixtures/t26_request_completion/baseline/CTPLoader/TraderSpi.cpp \
   src/TestUnits/fixtures/t26_request_completion/baseline/CTPLoader/TraderSpi.h "$out/baseline/CTPLoader/"
check "$out/baseline/CTPLoader/TraderSpi.cpp" 065f27b0775033387dbc64ae0a8ac1a1ec6a6a3bfe96e20c80f5e1fc880acd07
check "$out/baseline/CTPLoader/TraderSpi.h" 903cea6794c9f579c1811f3474874f2c67ff0544429469f6886d16f375c8f69c
sources=(src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h
 src/TestUnits/fixtures/t26_request_completion/query_probe.cpp src/TestUnits/fixtures/t26_request_completion/adapt_fee_probe.py
 src/TestUnits/fixtures/t26_request_completion/run_queries.py src/TestUnits/fixtures/t26_request_completion/run.sh
 src/TestUnits/fixtures/t26_request_completion/baseline-cpp.patch src/TestUnits/fixtures/t26_request_completion/baseline-h.patch
 src/TestUnits/fixtures/t26_request_completion/baseline/CTPLoader/TraderSpi.cpp src/TestUnits/fixtures/t26_request_completion/baseline/CTPLoader/TraderSpi.h
 src/TestUnits/fixtures/t26_fees_atomic/fee_probe.cpp src/TestUnits/fixtures/t26_fees_atomic/run_tests.py
 src/TestUnits/fixtures/t26_fees_atomic/consumer_key_probe.cpp src/TestUnits/coverage/t_CTPLoader/t26_ctp_api_stub.hpp)
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/t26_request_completion/adapt_fee_probe.py "$out/fee_probe_adapted.cpp" "$out/fee_tests_adapted.py"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in baseline normal asan tsan; do
 snap="$out/current"; flags=(-DWT_T26_FEES_TEST_HOOKS -DWT_T26_QUERY_TIMEOUT_MS=3500)
 if [[ $variant == baseline ]]; then snap="$out/baseline"; flags=(-DWT_T26_TEST_BASELINE); fi
 if [[ $variant == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
 if [[ $variant == tsan ]]; then flags+=(-fsanitize=thread -fno-omit-frame-pointer); fi
 args=(-std=c++17 -g -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h
  -include "$snap/Share/fmt_enum_compat.h" -I"$snap" -Isrc/TestUnits/coverage/t_CTPLoader
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
  -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include)
 libs=(/tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a
  /opt/homebrew/Cellar/libiconv/1.18/lib/libiconv.a -fprofile-instr-generate -lpthread)
 (
  set -x
  "$compiler" "${args[@]}" "${flags[@]}" -c "$snap/CTPLoader/TraderSpi.cpp" -o "$out/$variant.o"
  "$compiler" "${args[@]}" "${flags[@]}" src/TestUnits/fixtures/t26_request_completion/query_probe.cpp "$out/$variant.o" "${libs[@]}" -o "$out/$variant-query-probe"
  if [[ $variant == normal || $variant == asan ]]; then
   "$compiler" "${args[@]}" "${flags[@]}" "$out/fee_probe_adapted.cpp" "$out/$variant.o" "${libs[@]}" -o "$out/$variant-probe"
  fi
 ) > "$out/$variant-build.log" 2>&1
done
"$compiler" -std=c++17 -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h -include "$out/current/Share/fmt_enum_compat.h" \
 -I"$out/current" -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include \
 -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include \
 src/TestUnits/fixtures/t26_fees_atomic/consumer_key_probe.cpp -o "$out/consumer-key-probe" > "$out/consumer-key-build.log" 2>&1
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python "$out/fee_tests_adapted.py" "$out" | tee "$out/fee-results.txt"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/t26_request_completion/run_queries.py "$out" | tee "$out/query-results.txt"
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
