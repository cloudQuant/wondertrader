#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
cd "$root"
for mode in normal ubsan; do
  flags=(-O0)
  if [[ $mode == ubsan ]]; then flags=(-O0 -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all); fi
  /usr/bin/c++ -std=c++17 -Wall -Wextra -Werror "${flags[@]}" -Isrc -I/opt/homebrew/include -I/usr/local/include \
    src/TestUnits/fixtures/i03_fee_context_20261103/fee_context_test.cpp \
    src/TestUnits/fixtures/i03_fee_schedule_candidate/fee_schedule_test.cpp \
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -pthread -o "$out/$mode"
  UBSAN_OPTIONS=halt_on_error=1 "$out/$mode" | tee "$out/$mode.log"
done
shasum -a 256 src/Share/FeeSchedule.hpp src/TestUnits/fixtures/i03_fee_context_20261103/fee_context_test.cpp > "$out/hashes.txt"
