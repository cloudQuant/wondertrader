#!/bin/bash
set -euo pipefail
repo=/Users/yunjinqi/Documents/new_projects/wondertrader
out=/tmp/wt-i03-event-notifier/cmake-cold-build
/opt/homebrew/bin/cmake -S "$repo/src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture" -B "$out" -G Ninja -DCMAKE_MAKE_PROGRAM=/Users/yunjinqi/opt/anaconda3/bin/ninja \
  -DWT_R20_TOOLS_LIBRARY=/tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSTools.a \
  -DWT_R20_UTILS_LIBRARY=/tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSUtils.a \
  '-DWT_R20_EXTRA_INCLUDES=/opt/homebrew/Cellar/boost/1.88.0/include;/opt/homebrew/Cellar/fmt/11.2.0/include;/opt/homebrew/Cellar/spdlog/1.15.3/include;/usr/local/include' \
  '-DWT_R20_EXTRA_LIBRARIES=-lboost_filesystem;-lboost_thread;-liconv' \
  -DCMAKE_CXX_FLAGS=-fprofile-instr-generate \
  '-DCMAKE_EXE_LINKER_FLAGS=-L/opt/homebrew/Cellar/boost/1.88.0/lib -fprofile-instr-generate'
/opt/homebrew/bin/cmake --build "$out" --verbose
/opt/homebrew/bin/ctest --test-dir "$out" --output-on-failure
find "$out" -type l -print
