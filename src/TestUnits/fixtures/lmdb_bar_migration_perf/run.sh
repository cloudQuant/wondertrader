#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd "$root"
compiler=${WT_LLVM_BIN:-/opt/homebrew/Cellar/llvm/22.1.8/bin}
sources=(src/WtDataStorageAD/LMDBBarKeyV2.hpp src/WtDataStorageAD/LMDBBarMigration.hpp
 src/TestUnits/fixtures/lmdb_bar_migration/migration_test.cpp
 src/TestUnits/fixtures/lmdb_bar_migration_perf/cache_guard_test.cpp
 src/TestUnits/fixtures/lmdb_bar_migration_perf/run.sh
 src/WTSUtils/lmdb/lmdb.h src/WTSUtils/lmdb/mdb.c src/WTSUtils/lmdb/midl.c src/WTSUtils/lmdb/midl.h
 src/Includes/WTSStruct.h src/Includes/WTSTypes.h)
"$compiler/clang++" --version > "$out/compiler.txt"
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
for variant in normal asan; do
 flags=(-g -O1 -fno-omit-frame-pointer)
 if [[ "$variant" == asan ]]; then flags+=(-fsanitize=address,undefined); fi
 (
  set -x
  "$compiler/clang" "${flags[@]}" -Isrc/WTSUtils/lmdb -c src/WTSUtils/lmdb/mdb.c -o "$out/$variant-mdb.o"
  "$compiler/clang" "${flags[@]}" -c src/WTSUtils/lmdb/midl.c -o "$out/$variant-midl.o"
  "$compiler/clang++" -std=c++17 "${flags[@]}" src/TestUnits/fixtures/lmdb_bar_migration_perf/cache_guard_test.cpp "$out/$variant-mdb.o" "$out/$variant-midl.o" -o "$out/$variant-test"
 ) > "$out/$variant-build.log" 2>&1
 for mode in source-raw target-raw source-live target-live multiple-bindings; do
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
   "$out/$variant-test" "$mode" "$out/$variant-$mode" > "$out/$variant-$mode.log" 2>&1
  printf '%s %s 0\n' "$variant" "$mode" >> "$out/results.txt"
 done
done
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
