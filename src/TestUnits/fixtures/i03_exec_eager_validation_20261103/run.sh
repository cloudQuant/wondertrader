#!/bin/bash
# run.sh <new-absolute-output-dir> <green|red> [asan]
# asan: whole build (product TUs, tests, fixture plugin) with Homebrew LLVM 22
# -fsanitize=address,undefined; Homebrew gtest/boost and system libs are not instrumented.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
fx="$repo/src/TestUnits/fixtures/i03_exec_eager_validation_20261103"
out="${1:?provide a new absolute output directory}"
mode="${2:?green or red}"
san="${3:-}"
cc=/usr/bin/cc; cxx=/usr/bin/c++; sanflags=()
if [ "$san" = asan ]; then
  cc=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang; cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
  sanflags=("-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -include cstdlib"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined ${LDFLAGS:-}" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined ${LDFLAGS:-}"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address,undefined ${LDFLAGS:-}")
  # Explicit -DCMAKE_*_LINKER_FLAGS replace CMake's LDFLAGS initialisation; keep the
  # caller's LDFLAGS (e.g. -Wl,-rpath,/usr/local/lib for nanomsg) in the asan build.
  export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
elif [ -n "$san" ]; then echo 'third argument must be asan' >&2; exit 1; fi
# WT_I03_NO_WERROR=1 only works around unrelated in-progress files of other
# work packages; warnings in this item's files are checked from build.log.
werror=()
if [ "${WT_I03_NO_WERROR:-0}" = 1 ]; then werror=(-DWT_WARNINGS_AS_ERRORS=OFF); fi
case "$out" in /*) ;; *) echo 'output path must be absolute' >&2; exit 1;; esac
if [ -e "$out" ]; then echo 'output path already exists' >&2; exit 1; fi
if [ -n "$(find "$repo/src" "$repo/dist/common" -type l -print -quit)" ]; then echo 'source contains symlinks; refusing snapshot' >&2; exit 1; fi
mkdir -p "$out/snapshot/dist"
cp -R "$repo/src" "$out/snapshot/src"
cp -R "$repo/dist/common" "$out/snapshot/dist/common"
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python "$fx/prepare.py" "$out/snapshot" "$mode" > "$out/prepare.log" 2>&1
( cd "$out/snapshot" && shasum -a 256 src/WtCore/WtLocalExecuter.cpp src/WtCore/WtDiffExecuter.cpp src/WtCore/WtArbiExecuter.cpp \
  src/WtCore/WtLocalExecuter.h src/WtCore/WtDiffExecuter.h src/WtCore/WtArbiExecuter.h src/WtCore/ExecPolicyConfig.h \
  src/WtCore/WtSelEngine.cpp src/WtRunner/WtRunner.cpp src/WtRunner/WtRunner.h \
  src/TestUnits/fixtures/i03_exec_eager_validation_20261103/exec_eager_test.cpp \
  src/TestUnits/fixtures/i03_exec_eager_validation_20261103/runner.inc \
  src/TestUnits/fixtures/i03_exec_eager_validation_20261103/sel_factory.cpp ) > "$out/source.sha256"
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S "$out/snapshot/src" -B "$out/build" -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_PREFIX_PATH=/opt/homebrew ${sanflags[@]+"${sanflags[@]}"} ${werror[@]+"${werror[@]}"} \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "$out/configure.log" 2>&1
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$out/build" --target CoverageTests_wtcore CoverageTests_m_WtRunner -j 2 > "$out/build.log" 2>&1
grep -nE "(ExecPolicyConfig|WtLocalExecuter|WtDiffExecuter|WtArbiExecuter|WtSelEngine|WtRunner|exec_eager_test|runner.inc|r104_runner_executers_test|sel_factory)[^ ]*:[0-9]+:[0-9]+: warning" "$out/build.log" > "$out/own_warnings.txt" || true
bin="$out/build/build_x64/Debug/bin/TestUnits"
mkdir -p "$out/run_exec" "$out/run_runner/sel"
cp "$out/build/i03_sel_plugins/libI03SelFact.dylib" "$out/run_runner/sel/"
storage="$(find "$out/build" -name libWtDataStorage.dylib -type f | head -1)"
cp "$storage" "$out/run_runner/"
set +e
( cd "$out/run_exec" && LLVM_PROFILE_FILE="$out/exec.profraw" perl -e 'alarm 120; exec @ARGV' -- "$bin/CoverageTests_wtcore" \
  --gtest_filter='I03ExecEagerValidation.*' --gtest_output="xml:$out/exec.xml" > "$out/exec.log" 2>&1 ); echo "exec exit=$?" | tee "$out/exit_codes.txt"
( cd "$out/run_runner" && LLVM_PROFILE_FILE="$out/runner.profraw" perl -e 'alarm 120; exec @ARGV' -- "$bin/CoverageTests_m_WtRunner" \
  --gtest_filter='I03RunnerExecEager.*:I03RunnerSelStrategies.*' --gtest_output="xml:$out/runner.xml" > "$out/runner.log" 2>&1 ); echo "runner exit=$?" | tee -a "$out/exit_codes.txt"
( cd "$out/run_runner" && LLVM_PROFILE_FILE="$out/r104.profraw" perl -e 'alarm 120; exec @ARGV' -- "$bin/CoverageTests_m_WtRunner" \
  --gtest_filter='r104_wtrunner_config.*' --gtest_output="xml:$out/r104.xml" > "$out/r104.log" 2>&1 ); echo "r104 exit=$?" | tee -a "$out/exit_codes.txt"
