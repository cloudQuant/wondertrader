#!/bin/bash
# usage: r20fix.sh <fixture source dir> <build dir> <libdir>
src="$1"; b="$2"; lib="$3"
cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_R20_TOOLS_LIBRARY="$lib/libWTSTools.a" -DWT_R20_UTILS_LIBRARY="$lib/libWTSUtils.a" \
  "-DWT_R20_EXTRA_INCLUDES=/opt/homebrew/Cellar/boost/1.88.0/include;/opt/homebrew/include;/usr/local/include" \
  "-DWT_R20_EXTRA_LIBRARIES=/opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.dylib;/opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.dylib;/usr/local/lib/libiconv.dylib;-fprofile-instr-generate" \
  > "$b.configure.log" 2>&1 || { echo configure-failed; exit 2; }
ninja -C "$b" -j 3 -k 0 > "$b.build.log" 2>&1; echo "build-exit=$?" >> "$b.build.log"
perl -e 'alarm 300; exec @ARGV' ctest --test-dir "$b" --output-on-failure --timeout 60 > "$b.ctest.log" 2>&1; echo "ctest-exit=$?" >> "$b.ctest.log"
tail -15 "$b.ctest.log"
