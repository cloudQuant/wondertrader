#!/bin/bash
# 回归：旧 T26-011 探针 query_probe.cpp 的 20 个场景，针对当前候选 TraderSpi（normal/asan/tsan）。
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd); cd "$root"
out=${1:?new output dir}; snap=${2:?run.sh output dir}/current; test ! -e "$out"; mkdir -p "$out"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in normal asan tsan; do
 flags=(-DWT_T26_FEES_TEST_HOOKS -DWT_T26_QUERY_TIMEOUT_MS=3500)
 if [[ $variant == asan ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
 if [[ $variant == tsan ]]; then flags+=(-fsanitize=thread -fno-omit-frame-pointer); fi
 args=(-std=c++17 -g -O1 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include stdlib.h -include "$snap/Share/fmt_enum_compat.h" -I"$snap" -Isrc/TestUnits/coverage/t_CTPLoader
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -isystem /usr/local/include)
 libs=(/tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a /opt/homebrew/Cellar/libiconv/1.18/lib/libiconv.a -fprofile-instr-generate -lpthread)
 ( set -x
   "$compiler" "${args[@]}" "${flags[@]}" -c "$snap/CTPLoader/TraderSpi.cpp" -o "$out/$variant.o"
   "$compiler" "${args[@]}" "${flags[@]}" src/TestUnits/fixtures/t26_request_completion/query_probe.cpp "$out/$variant.o" "${libs[@]}" -o "$out/$variant-query-probe" ) > "$out/$variant-build.log" 2>&1
done
# run_queries.py 的场景表与判定，去掉末尾 baseline 段
sed '/^for mode in (.delayed.,.sync.):/,$d' src/TestUnits/fixtures/t26_request_completion/run_queries.py > "$out/run_queries_nobaseline.py"
python3 -I "$out/run_queries_nobaseline.py" "$out"
