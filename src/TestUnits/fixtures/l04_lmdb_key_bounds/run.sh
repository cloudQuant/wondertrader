#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
build=${1:?provide configured Ninja build containing WTSUtils and WtDataStorageAD}
out=$(mktemp -d /tmp/wt-l04-key-test.XXXXXX)
cd "$root"
printf 'outputs: %s\n' "$out"
"${WT_CLANGXX:-/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++}" -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer src/TestUnits/fixtures/l04_lmdb_key_bounds/key_bounds.cpp -o "$out/key_bounds"
"$out/key_bounds"
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/l04_lmdb_key_bounds/compile_product.py "$build" "$out"
/usr/bin/c++ -std=gnu++17 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include src/Share/fmt_enum_compat.h -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/include -Isrc -g -fprofile-instr-generate src/TestUnits/fixtures/l04_lmdb_key_bounds/reader_rejection.cpp "$out/WtBtDtReaderAD.o" "$out/WtDataReaderAD.o" "$out/WtRdmDtReaderAD.o" "$out/WtDataWriterAD.o" "$build/build_x64/Debug/libs/libWTSUtils.a" -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lpthread -ldl /usr/local/lib/libiconv.dylib -o "$out/reader_rejection"
"$out/reader_rejection"
printf 'key and product rejection tests passed\n'
