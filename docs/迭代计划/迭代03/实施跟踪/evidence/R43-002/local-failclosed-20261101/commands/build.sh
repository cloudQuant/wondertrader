#!/bin/zsh
set -e
CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
INCS=(-DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/include -I/usr/local/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -Isrc/WtExeFact -Isrc/WtBtCore -include stdlib.h -include src/Share/fmt_enum_compat.h)
LIBS=(/tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSTools.a /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/lib -L/usr/local/lib -lgtest_main -lgtest -lboost_filesystem -lboost_thread -liconv -ldl -lpthread)
$CXX -std=c++17 -fprofile-instr-generate -O0 -g $INCS src/TestUnits/coverage/r7_exefact/r7_stock_vwap_unit_test.cpp src/WtExeFact/WtStockVWapExeUnit.cpp src/WtExeFact/WtOrdMon.cpp /tmp/wt-i03-r43-final/stubs.cpp $LIBS -o /tmp/wt-i03-r43-final/vwap
$CXX -std=c++17 -fprofile-instr-generate -O0 -g $INCS src/TestUnits/coverage/n04_btcore_mockers/r43_match_engine_flow_test.cpp src/WtBtCore/MatchEngine.cpp /tmp/wt-i03-r43-final/stubs.cpp $LIBS -o /tmp/wt-i03-r43-final/matcher
