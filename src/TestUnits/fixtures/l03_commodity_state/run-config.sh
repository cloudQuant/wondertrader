#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?provide a configured build with WTSTools and WTSUtils}
out=$(mktemp -d /tmp/wt-l03-config.XXXXXX)
cd "$root"
printf 'outputs: %s\n' "$out"
compiler=/usr/bin/c++
flags=(-O0)
if [[ ${L03_SANITIZE:-0} == 1 ]]; then
    compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
    flags=(-include stdlib.h -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
    export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1}
fi
"$compiler" -std=gnu++17 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL "${flags[@]}" \
    -include src/Share/fmt_enum_compat.h -I/opt/homebrew/Cellar/boost/1.88.0/include \
    -I/opt/homebrew/include -Isrc -g -fprofile-instr-generate \
    src/TestUnits/fixtures/l03_commodity_state/config_gate.cpp src/WTSTools/WTSBaseDataMgr.cpp \
    "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
    -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread \
    -lpthread -ldl /usr/local/lib/libiconv.dylib -o "$out/tests"
LLVM_PROFILE_FILE="$out/profile-%p.profraw" perl -e 'alarm 60; exec @ARGV' -- \
    "$out/tests" '--gtest_filter=L03CommodityReady.*'
