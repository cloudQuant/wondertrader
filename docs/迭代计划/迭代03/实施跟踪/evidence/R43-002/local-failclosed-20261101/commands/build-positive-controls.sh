#!/bin/bash
set -eu
CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
INCS=(-DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/include -I/usr/local/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -Isrc/WtExeFact -Isrc/WtBtCore -Isrc/TestUnits/coverage/r7_exefact -include stdlib.h -include src/Share/fmt_enum_compat.h)
LIBS=(/tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSTools.a /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/lib -L/usr/local/lib -lgtest_main -lgtest -lboost_filesystem -lboost_thread -liconv -ldl -lpthread)
EVIDENCE='docs/迭代计划/迭代03/实施跟踪/evidence/R43-002/local-failclosed-20261101'
$CXX -std=c++17 -fprofile-instr-generate -O0 -g "${INCS[@]}" "$EVIDENCE/commands/positive-controls.cpp" src/WtExeFact/WtStockVWapExeUnit.cpp src/WtExeFact/WtOrdMon.cpp "$EVIDENCE/commands/stubs.cpp" "${LIBS[@]}" -o /tmp/wt-i03-r43-final/final-positive
$CXX -std=c++17 -fprofile-instr-generate -O0 -g "${INCS[@]}" "$EVIDENCE/commands/positive-controls.cpp" /tmp/wt-i03-r43-final/baseline_vwap.o src/WtExeFact/WtOrdMon.cpp "$EVIDENCE/commands/stubs.cpp" "${LIBS[@]}" -o /tmp/wt-i03-r43-final/baseline-positive
