#!/bin/bash
set -euxo pipefail
out=/tmp/wt-i03-l03-writer-20261102
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
common=(-std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
 -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
 -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include
 -I/usr/local/include -I"$out/src" -include stdlib.h -include "$out/src/Share/fmt_enum_compat.h")
"$cxx" "${common[@]}" -Dinst_hlp=l03_writer_test -c "$out/src/TestUnits/coverage/r23_storage4/l03_writer_session_date_test.cpp" -o "$out/test.o"
for version in red green; do
 source="$out/src/WtDataStorage/WtDataWriter.cpp"
 if [ "$version" = red ]; then source="$out/src/WtDataStorage/WtDataWriter_red.cpp"; fi
 if [ "$version" = green ]; then "$cxx" "${common[@]}" -Dinst_hlp=l03_writer_product -c "$source" -o "$out/$version.o"; fi
 "$cxx" -g -fsanitize=address,undefined -fprofile-instr-generate -fno-omit-frame-pointer \
  "$out/$version.o" "$out/test.o" /tmp/wt-i03-r41-drain/libs/libWTSTools.a /tmp/wt-i03-r41-drain/libs/libWTSUtils.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  -L/opt/homebrew/Cellar/boost/1.88.0/lib -Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib \
  -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -pthread -Wl,-undefined,dynamic_lookup -o "$out/$version"
done
