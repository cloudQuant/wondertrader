#!/usr/bin/env bash
# 迭代 01 覆盖率构建/运行/报告入口
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
BUILD="${1:-$ROOT/build-cov}"
JOBS="${JOBS:-8}"
PROF_DIR="$BUILD/coverage"
RUN_DIR="$PROF_DIR/run"

cmake -S "$ROOT/src" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$BUILD" -j "$JOBS"

rm -rf "$PROF_DIR"
mkdir -p "$PROF_DIR" "$RUN_DIR"

mapfile -t TEST_BINS < <(find "$BUILD" -type f \( -name TestUnits -o -name CoverageTests \) -perm -111 | sort)
if [ "${#TEST_BINS[@]}" -eq 0 ]; then
  echo "no test executables found under $BUILD" >&2
  exit 1
fi

for bin in "${TEST_BINS[@]}"; do
  name="$(basename "$bin")"
  echo "== running $name =="
  ( cd "$RUN_DIR" && LLVM_PROFILE_FILE="$PROF_DIR/$name.profraw" "$bin" < /dev/null )
done

xcrun llvm-profdata merge -sparse "$PROF_DIR"/*.profraw -o "$PROF_DIR/coverage.profdata"
python3 "$ROOT/docs/迭代计划/迭代01/tools/coverage_report.py" "$BUILD" "$PROF_DIR/coverage.profdata"
