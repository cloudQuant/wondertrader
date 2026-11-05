#!/bin/bash
# Manual Femas builds: normal extra control + LLVM22 ASan/UBSan of product+original test
set -u
R=/tmp/wt-i03-reverify-20261103; F=$R/femas; REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
EV="$REPO/docs/迭代计划/迭代03/实施跟踪/evidence/T4-002-NEW-019"
cd $REPO
COMMON="-std=gnu++17 -g -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I$R/boost-include -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -isystem /opt/homebrew/include -isystem src -include src/Share/fmt_enum_compat.h"
MACROS=(-DWT_T4_FEMAS_MISSING_LIBRARY_MODULE='"wt_t4_femas_module_absent"' -DWT_T4_FEMAS_MISSING_SYMBOL_MODULE='"wt_t4_femas_missing_symbol"' -DWT_T4_FEMAS_NULL_CREATOR_MODULE='"wt_t4_femas_null_creator"' -DWT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE='"wt_t4_femas_successful_creator"')
LINK="-Wl,-undefined,dynamic_lookup -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -lboost_filesystem -lboost_thread -liconv"
run() { echo "+ $*"; "$@"; echo "rc=$?"; }
# normal: product + extra test (evidence femas_extra.cpp)
run /usr/bin/c++ $COMMON -Dinst_hlp=femas_product -c src/TraderFemas/TraderFemas.cpp -o $F/product-normal.o
run /usr/bin/c++ $COMMON "${MACROS[@]}" -Dinst_hlp=femas_extra -c "$EV/femas_extra.cpp" -o $F/extra-normal.o
run /usr/bin/c++ -g $F/product-normal.o $F/extra-normal.o -o $F/extra-normal $LINK
# LLVM22 sanitizer: product + current original test
L=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
SAN="-include stdlib.h -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
run $L $SAN $COMMON -Dinst_hlp=femas_product -c src/TraderFemas/TraderFemas.cpp -o $F/product-asan.o
run $L $SAN $COMMON "${MACROS[@]}" -Dinst_hlp=femas_test -c src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp -o $F/test-asan.o
run $L $SAN $COMMON "${MACROS[@]}" -Dinst_hlp=femas_extra -c "$EV/femas_extra.cpp" -o $F/extra-asan.o
GT="-I/opt/homebrew/Cellar/googletest/1.17.0/include"
run $L -g -fsanitize=address,undefined $F/product-asan.o $F/test-asan.o -o $F/test-asan $LINK
run $L -g -fsanitize=address,undefined $F/product-asan.o $F/extra-asan.o -o $F/extra-asan $LINK
# smoke + leak positive control
printf '#include <cstdlib>\nint main(){ void* p = std::malloc(77); (void)p; p = nullptr; return 0; }\n' > $F/leak-control.cpp
printf 'int main(){ return 0; }\n' > $F/smoke.cpp
run $L -g -fsanitize=address,undefined $F/smoke.cpp -o $F/smoke
run $L -g -O0 -fsanitize=address $F/leak-control.cpp -o $F/leak-control
