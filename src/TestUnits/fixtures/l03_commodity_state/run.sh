#!/bin/sh
set -eu
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for variant in normal sanitizer; do
  flags=""
  if [ "$variant" = sanitizer ]; then flags="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
  "$compiler" -std=c++17 -g -O1 $flags -Isrc \
    -I/opt/homebrew/Cellar/boost/1.88.0/include \
    src/TestUnits/fixtures/l03_commodity_state/probe.cpp -o "$out/$variant"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    "$out/$variant" > "$out/$variant.log" 2>&1
  cat "$out/$variant.log"
done
shasum -a 256 src/Includes/WTSContractInfo.hpp src/Includes/WTSTypes.h \
  src/TestUnits/fixtures/l03_commodity_state/probe.cpp > "$out/hashes.txt"
