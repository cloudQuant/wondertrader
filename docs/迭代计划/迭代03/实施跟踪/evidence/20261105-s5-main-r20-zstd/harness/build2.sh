#!/bin/bash
# usage: build2.sh <zstdlib dir> <out> <sanitize:yes|no> <harness.c>
set -e
SRC="$1"; OUT="$2"; SAN="$3"; H="$4"; HERE="$(cd "$(dirname "$0")" && pwd)"
FLAGS="-O1 -g"; [ "$SAN" = yes ] && FLAGS="$FLAGS -fsanitize=undefined -fno-sanitize-recover=all"
OBJ="$(mktemp -d "$HERE/obj.XXXX")"
for c in "$SRC"/*.c; do clang -c $FLAGS -I"$SRC" "$c" -o "$OBJ/$(basename "$c").o" 2>>"$HERE/build-warnings.log"; done
clang $FLAGS -I"$SRC" "$H" "$OBJ"/*.o -o "$OUT"
rm -rf "$OBJ"
