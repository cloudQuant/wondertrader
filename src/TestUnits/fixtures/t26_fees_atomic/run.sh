#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out/current/CTPLoader" "$out/current/API/CTP6.6.9" "$out/baseline" "$out/unit-baseline"
out=$(cd "$out" && pwd)
cp -RL src/Share src/Includes src/WTSUtils src/FasterLibs "$out/current/"
cp src/API/CTP6.6.9/*.h "$out/current/API/CTP6.6.9/"
cp src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h "$out/current/CTPLoader/"
cp -RL "$out/current/" "$out/baseline/"
cp -RL "$out/current/" "$out/unit-baseline/"
# 2026-11-04: the working-tree CTPLoader moved on after 2026-11-02 (T26-011 request tracking,
# card 14), so reverse-applying the stored patches to it no longer works. Rebuild both red
# controls from immutable inputs instead and verify them against the 2026-11-02 hashes:
#   baseline      = TraderSpi.{cpp,h} at checkpoint ccaad010 (identical to the 2026-11-02
#                   reverse-patched baseline, dc2a4e9c... / edc91a5c...);
#   unit-baseline = baseline + forward T26-007 patches (= 2026-11-02 candidate, 065f27b0... /
#                   903cea67...), then the units patch reversed (20f3627d... / 903cea67...).
ev=src/TestUnits/fixtures/t26_fees_atomic/evidence-20261102
base_commit=ccaad010e9169f610aded4d8dc0f5647f70a5cff
check() { [ "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$2" ] || { echo "hash mismatch: $1" >&2; exit 1; }; }
git show "$base_commit:src/CTPLoader/TraderSpi.cpp" > "$out/baseline/CTPLoader/TraderSpi.cpp"
git show "$base_commit:src/CTPLoader/TraderSpi.h" > "$out/baseline/CTPLoader/TraderSpi.h"
check "$out/baseline/CTPLoader/TraderSpi.cpp" dc2a4e9c522612bd0d6aa9fefd83a6c36b2e8820c9db3389d7ad41a526cd7d31
check "$out/baseline/CTPLoader/TraderSpi.h" edc91a5cf58ce3341734896491168554039800bb4385d81d9f9274cf92422553
cp "$out/baseline/CTPLoader/TraderSpi.cpp" "$out/baseline/CTPLoader/TraderSpi.h" "$out/unit-baseline/CTPLoader/"
patch -s -p1 -d "$out/unit-baseline" < "$ev/TraderSpi.cpp.patch"
patch -s -p1 -d "$out/unit-baseline" < "$ev/TraderSpi.h.patch"
check "$out/unit-baseline/CTPLoader/TraderSpi.cpp" 065f27b0775033387dbc64ae0a8ac1a1ec6a6a3bfe96e20c80f5e1fc880acd07
check "$out/unit-baseline/CTPLoader/TraderSpi.h" 903cea6794c9f579c1811f3474874f2c67ff0544429469f6886d16f375c8f69c
patch -s -R -p1 -d "$out/unit-baseline" < "$ev/TraderSpi.units.patch"
check "$out/unit-baseline/CTPLoader/TraderSpi.cpp" 20f3627dca70853f8d3d612f6210c795e2f35cb19a1a08c6c429b6e466593ba0
check "$out/unit-baseline/CTPLoader/TraderSpi.h" 903cea6794c9f579c1811f3474874f2c67ff0544429469f6886d16f375c8f69c
find "$out/baseline/CTPLoader" "$out/unit-baseline/CTPLoader" -name '*.orig' -delete
shasum -a 256 "$out"/{current,baseline,unit-baseline}/CTPLoader/TraderSpi.{cpp,h} > "$out/variant-sources.sha256"
sources=(src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h
 src/TestUnits/fixtures/t26_fees_atomic/fee_probe.cpp
 src/TestUnits/fixtures/t26_fees_atomic/consumer_key_probe.cpp
 src/TestUnits/fixtures/t26_fees_atomic/run_tests.py
 src/TestUnits/fixtures/t26_fees_atomic/run.sh
 "$ev/TraderSpi.cpp.patch" "$ev/TraderSpi.h.patch" "$ev/TraderSpi.units.patch")
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in baseline unit-baseline normal asan; do
 snap="$out/current"; flags=()
 if [[ $variant = baseline ]]; then snap="$out/baseline"; fi
 if [[ $variant = unit-baseline ]]; then snap="$out/unit-baseline"; fi
 if [[ $variant != baseline ]]; then flags+=(-DWT_T26_FEES_TEST_HOOKS); fi
 # current-code builds only: the probe registers its injected request IDs (see fee_probe.cpp)
 if [[ $variant = normal || $variant = asan ]]; then flags+=(-DWT_T26_PROBE_ARM_REQUESTS); fi
 if [[ $variant = asan || $variant = baseline || $variant = unit-baseline ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
 args=(-std=c++17 -g -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h
  -include "$snap/Share/fmt_enum_compat.h" -I"$snap"
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
  -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include)
 (
  set -x
  "$compiler" "${args[@]}" "${flags[@]}" -c "$snap/CTPLoader/TraderSpi.cpp" -o "$out/$variant.o"
  "$compiler" "${args[@]}" "${flags[@]}" src/TestUnits/fixtures/t26_fees_atomic/fee_probe.cpp "$out/$variant.o" \
   /tmp/wt-i03-r41-drain/libs/libWTSUtils.a \
   /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a \
   /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a \
   /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a \
   /opt/homebrew/Cellar/libiconv/1.18/lib/libiconv.a -fprofile-instr-generate -lpthread -o "$out/$variant-probe"
 ) > "$out/$variant-build.log" 2>&1
done
"$compiler" -std=c++17 -g -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h \
 -include "$out/current/Share/fmt_enum_compat.h" -I"$out/current" \
 -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include \
 -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include \
 src/TestUnits/fixtures/t26_fees_atomic/consumer_key_probe.cpp -o "$out/consumer-key-probe" > "$out/consumer-key-build.log" 2>&1
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python \
 src/TestUnits/fixtures/t26_fees_atomic/run_tests.py "$out" | tee "$out/results.txt"
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
