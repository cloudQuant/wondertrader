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
 src/TestUnits/fixtures/lmdb_bar_migration/capability_legacy_regression.cpp
 src/TestUnits/fixtures/lmdb_bar_migration/run_controlled_regression.py
 src/TestUnits/fixtures/lmdb_bar_migration/run_controlled_regression.sh
 src/WTSUtils/lmdb/lmdb.h src/WTSUtils/lmdb/mdb.c src/WTSUtils/lmdb/midl.c src/WTSUtils/lmdb/midl.h
 src/Includes/WTSStruct.h src/Includes/WTSTypes.h)
shasum -a 256 "${sources[@]}" > "$out/source-before.sha256"
"$compiler/clang++" --version > "$out/compiler.txt"
failure=0
for variant in normal asan; do
 flags=(-g -O1 -fno-omit-frame-pointer)
 if [[ "$variant" == asan ]]; then flags+=(-fsanitize=address,undefined); fi
 (
  set -x
  "$compiler/clang" "${flags[@]}" -Isrc/WTSUtils/lmdb -c src/WTSUtils/lmdb/mdb.c -o "$out/$variant-mdb.o"
  "$compiler/clang" "${flags[@]}" -c src/WTSUtils/lmdb/midl.c -o "$out/$variant-midl.o"
  "$compiler/clang++" -std=c++17 "${flags[@]}" src/TestUnits/fixtures/lmdb_bar_migration/capability_legacy_regression.cpp "$out/$variant-mdb.o" "$out/$variant-midl.o" -o "$out/$variant-test"
 ) > "$out/$variant-build.log" 2>&1
 /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/run_controlled_regression.py "$out" "$variant" || failure=1
done
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
exit "$failure"
