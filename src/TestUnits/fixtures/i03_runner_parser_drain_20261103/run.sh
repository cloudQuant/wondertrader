#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
out="${1:?provide a new absolute output directory}"
case "$out" in /*) ;; *) echo 'output path must be absolute' >&2; exit 1;; esac
if [ -e "$out" ]; then echo 'output path already exists' >&2; exit 1; fi
if [ -n "$(find "$repo/src" "$repo/dist/common" -type l -print -quit)" ]; then echo 'source contains symlinks; refusing snapshot' >&2; exit 1; fi
mkdir -p "$out/snapshot/dist"
cp -R "$repo/src" "$out/snapshot/src"
cp -R "$repo/dist/common" "$out/snapshot/dist/common"
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python "$repo/src/TestUnits/fixtures/i03_runner_parser_drain_20261103/prepare.py" "$out/snapshot"
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S "$out/snapshot/src" -B "$out/build" -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$out/configure.log" 2>&1
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$out/build" --target CoverageTests_m_WtRunner -j 2 > "$out/build.log" 2>&1
mkdir -p "$out/run/parsers"
cp "$out/build/i03_runner_parser_plugins/libPvt20.dylib" "$out/run/parsers/libPvt20.dylib"
cd "$out/run"
filter="$(cat "$repo/src/TestUnits/fixtures/i03_runner_parser_drain_20261103/filter.txt")"
LLVM_PROFILE_FILE="$out/test.profraw" perl -e 'alarm 90; exec @ARGV' -- "$out/build/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtRunner" "--gtest_filter=$filter" --gtest_output="xml:$out/test.xml" | tee "$out/test.log"
