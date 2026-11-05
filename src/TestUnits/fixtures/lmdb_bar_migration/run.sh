#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
cd "$root"
compiler=${WT_LLVM_BIN:-/opt/homebrew/Cellar/llvm/22.1.8/bin}
sources=(src/WtDataStorageAD/LMDBBarKeyV2.hpp src/WtDataStorageAD/LMDBBarMigration.hpp src/TestUnits/fixtures/lmdb_bar_migration/migration_test.cpp src/TestUnits/fixtures/lmdb_bar_migration/no_test_hooks.cpp src/TestUnits/fixtures/lmdb_bar_migration/run.py src/TestUnits/fixtures/lmdb_bar_migration/run.sh src/TestUnits/fixtures/lmdb_bar_migration/annotate_lmdb.py src/TestUnits/fixtures/lmdb_bar_migration/tsan_sem_control.cpp src/TestUnits/fixtures/lmdb_bar_migration/tsan_controls.py src/TestUnits/fixtures/lmdb_bar_migration/benchmark.py src/WTSUtils/lmdb/lmdb.h src/WTSUtils/lmdb/mdb.c src/WTSUtils/lmdb/midl.c src/WTSUtils/lmdb/midl.h src/Includes/WTSStruct.h src/Includes/WTSTypes.h)
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
  "$compiler/clang++" -std=c++17 "${flags[@]}" src/TestUnits/fixtures/lmdb_bar_migration/migration_test.cpp "$out/$variant-mdb.o" "$out/$variant-midl.o" -o "$out/$variant-test"
 ) > "$out/$variant-build.log" 2>&1
 /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/run.py "$out" "$variant"
done
(
 set -x
 "$compiler/clang++" -std=c++17 -g -O1 src/TestUnits/fixtures/lmdb_bar_migration/no_test_hooks.cpp "$out/normal-mdb.o" "$out/normal-midl.o" -o "$out/no-test-hooks"
 "$out/no-test-hooks"
 nm -u "$out/no-test-hooks" > "$out/no-test-hooks-undefined.txt"
 if grep -E 'adjustSourceStatForTest|afterStagedPutForTest' "$out/no-test-hooks-undefined.txt"; then exit 1; fi
) > "$out/no-test-hooks.log" 2>&1
"$compiler/clang++" -std=c++17 -g -O1 -fsanitize=thread src/TestUnits/fixtures/lmdb_bar_migration/tsan_sem_control.cpp -o "$out/tsan-sem-control" > "$out/tsan-sem-build.log" 2>&1
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/tsan_controls.py "$out"
shasum -a 256 "${sources[@]}" > "$out/source-after.sha256"
cmp "$out/source-before.sha256" "$out/source-after.sha256"
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
