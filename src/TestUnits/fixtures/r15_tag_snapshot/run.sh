#!/bin/zsh
set -eu
out=${1:?provide a new absolute output directory}
mode=${2:?asan or tsan}
test ! -e "$out"
mkdir -p "$out"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
case "$mode" in
  asan) flags=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;;
  tsan) flags=(-fsanitize=thread) ;;
  *) exit 2 ;;
esac
common=(-std=c++17 -O0 -g $flags -fno-omit-frame-pointer -nostdinc++
  -isystem /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include
  -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include
  -I/usr/local/include -Isrc -include src/Share/fmt_enum_compat.h)
for item in 'src/WtCore/HftStraBaseCtx.cpp:base' 'src/WtCore/HftStraContext.cpp:ctx' 'src/WtCore/WtHftEngine.cpp:engine' 'src/TestUnits/fixtures/r15_tag_snapshot/test.cpp:test'; do
  source=${item%%:*}; name=${item#*:}
  "$compiler" $common -c "$source" -o "$out/$name.o" > "$out/$name-build.log" 2>&1
done
"$compiler" -g $flags -fprofile-instr-generate -Wl,-undefined,dynamic_lookup \
  "$out/test.o" "$out/base.o" "$out/ctx.o" "$out/engine.o" -o "$out/tests" \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  /private/tmp/wt-r14-resume/build/build_x64/Debug/libs/libWtCore.a \
  /private/tmp/wt-r14-resume/build/build_x64/Debug/libs/libWTSTools.a \
  /private/tmp/wt-r14-resume/build/build_x64/Debug/libs/libWTSUtils.a \
  /private/tmp/wt-r14-resume/build/build_x64/Debug/bin/libWtShareHelper.dylib \
  /usr/local/lib/libnanomsg.6.0.1.dylib /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.dylib \
  /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.dylib -liconv -lpthread \
  -Wl,-rpath,/private/tmp/wt-r14-resume/build/build_x64/Debug/bin \
  -Wl,-rpath,/private/tmp/wt-i03-r15-tags/libs > "$out/link.log" 2>&1
shasum -a 256 src/WtCore/HftStraBaseCtx.h src/WtCore/HftStraBaseCtx.cpp \
  src/WtCore/HftStraContext.cpp src/WtCore/WtHftEngine.cpp \
  src/TestUnits/coverage/q01_wtcore3/r15_hft_stra_context.cpp \
  src/TestUnits/fixtures/r15_tag_snapshot/test.cpp > "$out/hashes.txt"
cd "$out"
if [[ "$mode" == asan ]]; then
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./tests --gtest_filter=R15CtxTest.tag_snapshot_survives_reentrant_erase_during_order_callback
else
  TSAN_OPTIONS=halt_on_error=1 ./tests --gtest_filter=R15CtxTest.tag_snapshot_concurrent_read_write
fi
