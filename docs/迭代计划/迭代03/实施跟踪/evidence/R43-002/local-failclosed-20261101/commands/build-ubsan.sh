#!/bin/bash
set -eu
CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
INCS=(-DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/include -I/usr/local/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h)
LIBS=(/tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSTools.a /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -liconv -ldl -lpthread)
EVIDENCE='docs/迭代计划/迭代03/实施跟踪/evidence/R43-002/local-failclosed-20261101'
FLAGS=(-std=c++17 -O1 -g -fsanitize=undefined,float-divide-by-zero,float-cast-overflow -fno-sanitize-recover=all -fprofile-instr-generate)
$CXX "${FLAGS[@]}" "${INCS[@]}" src/TestUnits/coverage/n04_btcore_mockers/r43_match_engine_flow_test.cpp src/WtBtCore/MatchEngine.cpp "$EVIDENCE/commands/stubs.cpp" "${LIBS[@]}" -lgtest_main -lgtest -o /tmp/wt-i03-r43-final/matcher_ubsan
$CXX "${FLAGS[@]}" "${INCS[@]}" "$EVIDENCE/commands/probe.cpp" src/WtBtCore/MatchEngine.cpp "${LIBS[@]}" -o /tmp/wt-i03-r43-final/probe
