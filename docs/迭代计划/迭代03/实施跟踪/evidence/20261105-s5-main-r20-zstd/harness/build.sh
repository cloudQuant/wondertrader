#!/bin/bash
# usage: build.sh <zstdlib dir> <out binary>
set -e
SRC="$1"; OUT="$2"; HERE="$(cd "$(dirname "$0")" && pwd)"
OBJ="$(mktemp -d "$HERE/obj.XXXX")"
for c in "$SRC"/*.c; do
  clang -c -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -I"$SRC" -DZSTD_MULTITHREAD=0 "$c" -o "$OBJ/$(basename "$c").o" 2>>"$HERE/build-warnings.log"
done
clang -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -I"$SRC" "$HERE/harness.c" "$OBJ"/*.o -o "$OUT"
rm -rf "$OBJ"
