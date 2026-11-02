#!/bin/bash
set -euo pipefail

if [[ $# -ne 1 || "$1" != /* || -e "$1" || -L "$1" ]]; then
    printf 'usage: %s /absolute/nonexistent/output-directory\n' "$0" >&2
    exit 2
fi

output="$1"
root="$(cd "$(dirname "$0")/../../../.." && pwd -P)"
cd "$root"
mkdir -p "$output"

inputs=(
    src/WtDataStorageAD/LMDBBarKeyV2.hpp
    src/WtDataStorageAD/LMDBKeys.h
    src/Includes/WTSStruct.h
    src/Includes/WTSTypes.h
    src/Includes/WTSMarcos.h
    src/TestUnits/fixtures/lmdb_bar_key_v2/lmdb_bar_key_v2_test.cpp
    src/TestUnits/fixtures/lmdb_bar_key_v2/run.sh
)
shasum -a 256 "${inputs[@]}" > "$output/input-before.sha256"
/usr/bin/c++ --version > "$output/compiler-normal.txt"
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ --version > "$output/compiler-sanitized.txt"

set -x
/usr/bin/c++ -std=c++17 -Wall -Wextra -Werror -Isrc \
    src/TestUnits/fixtures/lmdb_bar_key_v2/lmdb_bar_key_v2_test.cpp \
    -o "$output/key-test" > "$output/build-normal.log" 2>&1
perl -e 'alarm 30; exec @ARGV' -- "$output/key-test" > "$output/normal.log" 2>&1
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
    src/TestUnits/fixtures/lmdb_bar_key_v2/lmdb_bar_key_v2_test.cpp \
    -o "$output/key-test-sanitized" > "$output/build-sanitized.log" 2>&1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    perl -e 'alarm 30; exec @ARGV' -- "$output/key-test-sanitized" > "$output/sanitized.log" 2>&1
set +x

shasum -a 256 "${inputs[@]}" > "$output/input-after.sha256"
cmp "$output/input-before.sha256" "$output/input-after.sha256"
shasum -a 256 "$output/key-test" "$output/key-test-sanitized" > "$output/artifacts.sha256"
find "$output" -type l -print > "$output/symlinks.log"
test ! -s "$output/symlinks.log"
