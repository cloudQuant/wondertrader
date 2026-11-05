#!/bin/bash
# usage: manual_build.sh <variant: base|fix> <mode: normal|asan|tsan> <module: femas|mocker>
set -u
R=${WT_I03_LEAKS_ROOT:-/tmp/wt-i03-leaks-20261104}; REPO=${WT_REPO:-$(cd "$(dirname "$0")/../../../.." && pwd)}
variant=$1; mode=$2; mod=$3
O=$R/manual/$mod-$variant-$mode; rm -rf $O; mkdir -p $O
cd $REPO
if [ $variant = base ]; then PSRC=$R/baseline/src; else PSRC=$REPO/src; fi
# product-header resolution: base build puts the baseline tree first so "TraderMocker/TraderMocker.h" etc. resolve to baseline
INC="-I$R/boost-include"
[ $variant = base ] && INC="$INC -iquote $R/baseline/src -I$R/baseline/src"
COMMON="-std=gnu++17 -g -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL $INC -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -isystem /opt/homebrew/include -isystem /usr/local/include -isystem src -include src/Share/fmt_enum_compat.h"
WARN="-Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy -Wno-#pragma-messages"
case $mode in
  normal) CXX=/usr/bin/c++; SAN="-O0"; LSAN="";;
  asan) CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++; SAN="-include stdlib.h -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"; LSAN="-fsanitize=address,undefined";;
  tsan) CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++; SAN="-include stdlib.h -O1 -fsanitize=thread -fno-omit-frame-pointer"; LSAN="-fsanitize=thread";;
esac
MACROS=(-DWT_T4_FEMAS_MISSING_LIBRARY_MODULE='"wt_t4_femas_module_absent"' -DWT_T4_FEMAS_MISSING_SYMBOL_MODULE='"wt_t4_femas_missing_symbol"' -DWT_T4_FEMAS_NULL_CREATOR_MODULE='"wt_t4_femas_null_creator"' -DWT_T4_FEMAS_SUCCESSFUL_CREATOR_MODULE='"wt_t4_femas_successful_creator"')
if [ $mod = femas ]; then
  PROD=$PSRC/TraderFemas/TraderFemas.cpp
  TESTS="src/TestUnits/coverage/t_TraderFemas/femas_trader_test.cpp"
else
  PROD=$PSRC/TraderMocker/TraderMocker.cpp
  TESTS=$(ls src/TestUnits/coverage/t_TraderMocker/*.cpp)
fi
run() { echo "+ $*"; "$@"; local rc=$?; echo "rc=$rc"; return $rc; }
st=0
run $CXX $SAN $COMMON $WARN -Werror -Dinst_hlp=cov_prod -c $PROD -o $O/product.o || st=1
objs="$O/product.o"; i=0
for t in $TESTS; do i=$((i+1)); run $CXX $SAN $COMMON $WARN -Wno-error "${MACROS[@]}" -Dinst_hlp=cov_t$i -c $t -o $O/t$i.o || st=1; objs="$objs $O/t$i.o"; done
LINK="-Wl,-undefined,dynamic_lookup -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -lboost_filesystem -lboost_thread -liconv"
run $CXX -g $LSAN $objs -o $O/tests $LINK || st=1
shasum -a 256 $PROD $TESTS $O/tests > $O/hashes.txt
echo "build_status=$st"
