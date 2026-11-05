#!/bin/bash
set -euxo pipefail
repo="$(git rev-parse --show-toplevel)"
out="${1:?new absolute output directory required}"
case "$out" in /*) ;; *) exit 2;; esac
if [ -e "$out" ]; then echo 'output already exists' >&2; exit 2; fi
mkdir -p "$out"
find "$repo/src" -type l -print > "$out/source-symlinks.txt"
test ! -s "$out/source-symlinks.txt"
cp -R "$repo/src" "$out/green"
cp -R "$out/green" "$out/red"
git -C "$repo" show HEAD:src/WtDataStorage/WtDataWriter.cpp > "$out/red/WtDataStorage/WtDataWriter.cpp"
git -C "$repo" show HEAD:src/WtDataStorage/WtDataWriter.h > "$out/red/WtDataStorage/WtDataWriter.h"
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
for version in red green; do
 src="$out/$version"
 common=(-std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -I/usr/local/include -I"$src" -include stdlib.h -include "$src/Share/fmt_enum_compat.h")
 "$cxx" "${common[@]}" -Dinst_hlp=l03_writer_test -c "$src/TestUnits/coverage/r23_storage4/l03_writer_session_date_test.cpp" -o "$out/$version-test.o"
 "$cxx" "${common[@]}" -Dinst_hlp=l03_writer_product -c "$src/WtDataStorage/WtDataWriter.cpp" -o "$out/$version-writer.o"
 "$cxx" -g -fsanitize=address,undefined -fprofile-instr-generate -fno-omit-frame-pointer "$out/$version-writer.o" "$out/$version-test.o" /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -L/opt/homebrew/Cellar/boost/1.88.0/lib -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -pthread -Wl,-undefined,dynamic_lookup -o "$out/$version-test"
done
find "$out" -type l -print > "$out/symlinks.txt"
test ! -s "$out/symlinks.txt"
