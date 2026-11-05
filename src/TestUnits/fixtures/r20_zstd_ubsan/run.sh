#!/bin/bash
# R20_wtbtcore#1 / I03-NEW-082: vendored zstd 1.4.4 null-pointer arithmetic regression check.
# usage: run.sh <new-output-dir>
# Builds the probe twice against this source tree's vendored zstd (src/WTSUtils/zstdlib/*.c):
# once with Homebrew LLVM 22 ASan+UBSan (halt on the first report) and once normally. Both
# modes (direct ZSTD_compress and the WTSCmpHelper wrapper) must round-trip and exit 0, and
# the sanitized logs must contain no runtime error / AddressSanitizer report. Self-contained:
# no prebuilt library, no network, no symbolic links.
set -eu
out=${1:?provide a new output directory}
test ! -e "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../.." && pwd)
llvm=${WT_LLVM_BIN:-/opt/homebrew/Cellar/llvm/22.1.8/bin}
probe="$here/probe.cpp"
shasum -a 256 "$probe" "$here/run.sh" "$src/WTSUtils/WTSCmpHelper.hpp" "$src"/WTSUtils/zstdlib/*.c "$src"/WTSUtils/zstdlib/*.h > "$out/hashes.txt"
"$llvm/clang++" --version > "$out/compiler.txt"
build() { # <variant> <flags...>
  local variant=$1; shift
  mkdir -p "$out/obj-$variant"
  local objs=()
  for f in "$src"/WTSUtils/zstdlib/*.c; do
    local o="$out/obj-$variant/$(basename "$f" .c).o"
    "$llvm/clang" -g -O1 -w "$@" -c "$f" -o "$o"
    objs+=("$o")
  done
  "$llvm/clang++" -std=c++17 -g -O1 "$@" "$probe" "${objs[@]}" -o "$out/$variant"
}
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
build sanitized "${san[@]}" > "$out/sanitized-build.log" 2>&1
build normal -fno-omit-frame-pointer > "$out/normal-build.log" 2>&1
: > "$out/results.txt"
rc=0
for mode in direct wrapper sweep; do
  set +e
  "$out/normal" "$mode" > "$out/normal-$mode.log" 2>&1
  ncode=$?
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$out/sanitized" "$mode" > "$out/sanitized-$mode.log" 2>&1
  scode=$?
  set -e
  reports=$(grep -c -E 'runtime error:|ERROR: AddressSanitizer' "$out/sanitized-$mode.log" || true)
  pass=$(grep -c "$mode roundtrip PASS" "$out/sanitized-$mode.log" || true)
  printf '%s normal=%s sanitized=%s sanitizer_reports=%s roundtrip_pass=%s\n' "$mode" "$ncode" "$scode" "$reports" "$pass" | tee -a "$out/results.txt"
  if [ "$ncode" -ne 0 ] || [ "$scode" -ne 0 ] || [ "$reports" -ne 0 ] || [ "$pass" -ne 1 ]; then rc=1; fi
done
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
exit $rc
