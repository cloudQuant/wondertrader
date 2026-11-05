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
 src/TestUnits/fixtures/lmdb_bar_migration/capability_test.cpp
 src/TestUnits/fixtures/lmdb_bar_migration/run_capability.sh
 src/TestUnits/fixtures/lmdb_bar_migration/annotate_lmdb.py
 src/WTSUtils/lmdb/lmdb.h src/WTSUtils/lmdb/mdb.c src/WTSUtils/lmdb/midl.c src/WTSUtils/lmdb/midl.h
 src/Includes/WTSStruct.h src/Includes/WTSTypes.h)
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
"$compiler/clang++" --version > "$out/compiler.txt"
for variant in normal asan tsan-raw tsan-annotated; do
 flags=(-g -O1 -fno-omit-frame-pointer)
 if [[ "$variant" == asan ]]; then flags+=(-fsanitize=address,undefined); fi
 if [[ "$variant" == tsan-* ]]; then flags+=(-fsanitize=thread); fi
 source=src/WTSUtils/lmdb/mdb.c
 if [[ "$variant" == tsan-annotated ]]; then
  source="$out/mdb-tsan-annotated.c"
  /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/annotate_lmdb.py src/WTSUtils/lmdb/mdb.c "$source"
 fi
 (
  set -x
  "$compiler/clang" "${flags[@]}" -Isrc/WTSUtils/lmdb -c "$source" -o "$out/$variant-mdb.o"
  "$compiler/clang" "${flags[@]}" -c src/WTSUtils/lmdb/midl.c -o "$out/$variant-midl.o"
  "$compiler/clang++" -std=c++17 "${flags[@]}" src/TestUnits/fixtures/lmdb_bar_migration/capability_test.cpp "$out/$variant-mdb.o" "$out/$variant-midl.o" -o "$out/$variant-test"
 ) > "$out/$variant-build.log" 2>&1
 if [[ "$variant" == tsan-* ]]; then modes=(concurrent-revoke); else modes=(legacy revoke tamper evidence-tamper batch-atomic crash concurrent-revoke); fi
 for mode in "${modes[@]}"; do
  set +e
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
   "$out/$variant-test" "$mode" "$out/$variant-$mode" > "$out/$variant-$mode.log" 2>&1
  status=$?
  set -e
  printf '%s %s %s\n' "$variant" "$mode" "$status" >> "$out/results.txt"
  if [[ "$variant" != tsan-* && "$status" != 0 ]]; then exit "$status"; fi
 done
done
for count in 10000 50000 100000 250000; do
 /usr/bin/time -l "$out/normal-test" "benchmark-$count" "$out/benchmark-$count" > "$out/benchmark-$count.log" 2>&1
done
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
