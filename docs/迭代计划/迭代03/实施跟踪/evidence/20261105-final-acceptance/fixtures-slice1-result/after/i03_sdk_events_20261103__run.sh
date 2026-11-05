#!/bin/bash
# 迭代03 卡14 BUG-T26-011 CTPLoader 探针：baseline（反向应用本目录补丁=修改前）、normal、ASan/UBSan、TSan。
# 用法：bash run.sh <不存在的输出目录>；需 LLVM 22、Homebrew boost/fmt/spdlog/libiconv 与 /tmp/wt-i03-r41-drain/libs/libWTSUtils.a。
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$root"
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out/current/CTPLoader" "$out/current/API/CTP6.6.9"
out=$(cd "$out" && pwd)
fx=src/TestUnits/fixtures/i03_sdk_events_20261103
cp -R src/Share src/Includes src/WTSUtils src/FasterLibs "$out/current/"
cp src/API/CTP6.6.9/*.h "$out/current/API/CTP6.6.9/"
cp src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h "$out/current/CTPLoader/"
cp -R "$out/current/" "$out/baseline/"
# 2026-11-05: baseline = fixed pre-card-14 TraderSpi.{cpp,h} in ./baseline (the 2026-11-02
# T26-011 candidate dd4640b5... / aff40149..., rebuilt from commit ccaad010 + the T26-007 and
# T26-011 patches); forward-applying ctploader-baseline-*.patch to it gives the card-14 state of
# 2026-11-03 (85bc0dbb... / 7b434cc9...). Reverse-applying the patches to the moving working
# tree no longer works after K02=B and later edits.
check() { echo "$2  $1" | shasum -a 256 -c - >/dev/null || { echo "baseline hash mismatch: $1" >&2; exit 3; }; }
cp $fx/baseline/CTPLoader/TraderSpi.cpp $fx/baseline/CTPLoader/TraderSpi.h "$out/baseline/CTPLoader/"
check "$out/baseline/CTPLoader/TraderSpi.cpp" dd4640b59831aed904989e7c9235dbfb4e8f107d354b7e3bfd29fd3182f2186d
check "$out/baseline/CTPLoader/TraderSpi.h" aff40149be7cde0c121d3719d78312510a9b9a9f4d16d3cbaab20258ef35c7b5
sources=(src/CTPLoader/TraderSpi.cpp src/CTPLoader/TraderSpi.h $fx/ctploader_events_probe.cpp $fx/run_probe.py $fx/run.sh
 $fx/ctploader-baseline-cpp.patch $fx/ctploader-baseline-h.patch src/TestUnits/coverage/t_CTPLoader/t26_ctp_api_stub.hpp
 $fx/baseline/CTPLoader/TraderSpi.cpp $fx/baseline/CTPLoader/TraderSpi.h)
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
shasum -a 256 "$out"/baseline/CTPLoader/TraderSpi.* "$out"/current/CTPLoader/TraderSpi.* > "$out/snapshot.sha256"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in baseline normal asan tsan; do
 snap="$out/current"; flags=(-DWT_T26_FEES_TEST_HOOKS -DWT_T26_QUERY_TIMEOUT_MS=3500)
 if [[ $variant == baseline ]]; then snap="$out/baseline"; flags+=(-DWT_T26_TEST_BASELINE); fi
 if [[ $variant == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
 if [[ $variant == tsan ]]; then flags+=(-fsanitize=thread -fno-omit-frame-pointer); fi
 args=(-std=c++17 -g -O1 -Wall -Wextra -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h
  -include "$snap/Share/fmt_enum_compat.h" -I"$snap" -Isrc/TestUnits/coverage/t_CTPLoader
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
  -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include)
 libs=(/tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a
  /opt/homebrew/Cellar/libiconv/1.18/lib/libiconv.a -fprofile-instr-generate -lpthread)
 (
  set -x
  perl -e 'alarm 300; exec @ARGV' "$compiler" "${args[@]}" "${flags[@]}" -c "$snap/CTPLoader/TraderSpi.cpp" -o "$out/$variant.o"
  perl -e 'alarm 300; exec @ARGV' "$compiler" "${args[@]}" "${flags[@]}" $fx/ctploader_events_probe.cpp "$out/$variant.o" "${libs[@]}" -o "$out/$variant-probe"
 ) > "$out/$variant-build.log" 2>&1
done
grep -n "CTPLoader/TraderSpi.*warning:" "$out/normal-build.log" > "$out/product-warnings.txt" || true
set +e
perl -e 'alarm 400; exec @ARGV' python3 -I $fx/run_probe.py "$out" baseline,normal,asan,tsan | tee "$out/results.txt"
rc=${PIPESTATUS[0]}
set -e
echo "runner-exit=$rc" >> "$out/results.txt"
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
exit $rc
