#!/bin/bash
set -uo pipefail
ROOT=/Users/yunjinqi/Documents/new_projects/wondertrader
DIR="$ROOT/docs/迭代计划/迭代03/实施跟踪/evidence/L03-session-config-atomic-20261102"
BUILD=/tmp/wt-i03-session-atomic-20261102/build
mkdir -p "$BUILD"
cd "$ROOT" || exit 1
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include cstdlib -include src/Share/fmt_enum_compat.h -Isrc -Isrc/WTSUtils/yamlcpp -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/usr/local/include "$DIR/probe.cpp" src/WTSTools/WTSBaseDataMgr.cpp src/WTSUtils/WTSCfgLoader.cpp -Wl,-dead_strip -L/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -L/opt/homebrew/Cellar/yaml-cpp/0.9.0/lib -lyaml-cpp -L/usr/local/lib -liconv -o "$BUILD/probe" > "$DIR/build.log" 2>&1 || exit 2
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TZ=UTC "$BUILD/probe" > "$DIR/red.log" 2>&1
red=$?
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TZ=UTC "$BUILD/probe" overflow > "$DIR/overflow.log" 2>&1
ub=$?
printf 'red exit=%s\noverflow exit=%s\n' "$red" "$ub" | tee "$DIR/status.log"
[[ "$red" == 1 && "$ub" == 134 ]]
