#!/bin/bash
# s5 traders-ctp sanitizer builds (Homebrew LLVM 22, as in src/TestUnits/fixtures/i03_*).
# usage: build_san.sh <asan|tsan> <ctp|ctpid|opt|mini|dumper> <outdir>
# Product + new lifecycle test TUs are instrumented; WTSTools/WTSUtils come from the
# non-coverage Debug build /tmp/wt-i03-s5-traders-ctp-dbg (not instrumented).
set -u
mode=$1; lane=$2; O=$3
REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
S=$REPO/src
CXX=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
LIBS=/tmp/wt-i03-s5-traders-ctp-dbg/build_x64/Debug/libs
BOOSTI=/tmp/wt-i03-s5-traders-ctp-cov/TestUnits/coverage/cov_boost_include
mkdir -p "$O"
case $mode in
  asan) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined";;
  tsan) SAN="-fsanitize=thread -fno-omit-frame-pointer";;
  *) echo "bad mode"; exit 2;;
esac
COMMON=(-std=gnu++17 -g -O1 -include stdlib.h -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I$BOOSTI -I$S/Includes -I$S/Share -I$S/WTSTools -I$S/WTSUtils -isystem /opt/homebrew/include -isystem $S -include $S/Share/fmt_enum_compat.h -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages")
CTPLIBS=($S/API/CTP6.6.9/darwin/libthosttraderapi_se.a $S/API/CTP6.6.9/darwin/libs/libssl.a $S/API/CTP6.6.9/darwin/libs/libcrypto.a $S/API/CTP6.6.9/darwin/libs/comunicationkeylib.a)
EXTRA=()
case $lane in
  ctp)    PROD=($S/TraderCTP/TraderCTP.cpp); TESTS=($S/TestUnits/coverage/t_TraderCTP/i03s5_traderctp_lifecycle_test.cpp); TINC=(-I$S/TestUnits/coverage/t_TraderCTP); EXTRA=("${CTPLIBS[@]}");;
  ctpid)  PROD=($S/TraderCTP/TraderCTP.cpp); TESTS=($S/TestUnits/fixtures/i03_ctp_order_identity_20261103/ctp_identity_test.cpp); TINC=(-I$S/TestUnits/coverage/t_TraderCTP); EXTRA=("${CTPLIBS[@]}");;
  opt)    PROD=($S/TraderCTPOpt/TraderCTPOpt.cpp); TESTS=($S/TestUnits/coverage/t_TraderCTPOpt/i03s5_traderctpopt_lifecycle_test.cpp); TINC=(-I$S/TestUnits/coverage/t_TraderCTPOpt);;
  mini)   PROD=($S/TraderCTPMini/TraderCTPMini.cpp); TESTS=($S/TestUnits/coverage/t_TraderCTPMini/i03s5_traderctpmini_lifecycle_test.cpp); TINC=(-I$S/TestUnits/coverage/t_TraderCTPMini);;
  dumper) PROD=($S/TraderDumper/Dumper.cpp $S/TraderDumper/TraderAdapter.cpp $S/TraderDumper/WtHelper.cpp $S/TraderDumper/TraderDumper.cpp); TESTS=($S/TestUnits/coverage/t_TraderDumper/i03s5_dumper_lifecycle_test.cpp); TINC=(-DWT_DUMPER_TEST_HOOKS);;
  *) echo "bad lane"; exit 2;;
esac
st=0; objs=(); i=0
for f in "${PROD[@]}"; do i=$((i+1)); echo "+ $CXX ... -c $f"; $CXX $SAN "${COMMON[@]}" -Werror ${TINC[@]+"${TINC[@]}"} -Dinst_hlp=san_p$i -c "$f" -o "$O/p$i.o" || st=1; objs+=("$O/p$i.o"); done
for f in "${TESTS[@]}"; do i=$((i+1)); echo "+ $CXX ... -c $f"; $CXX $SAN "${COMMON[@]}" -Wno-error ${TINC[@]+"${TINC[@]}"} -Dinst_hlp=san_t$i -c "$f" -o "$O/t$i.o" || st=1; objs+=("$O/t$i.o"); done
$CXX $SAN "${objs[@]}" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a $LIBS/libWTSTools.a $LIBS/libWTSUtils.a ${EXTRA[@]+"${EXTRA[@]}"} -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lboost_filesystem -lboost_thread -lspdlog -lfmt /usr/local/lib/libiconv.dylib -lpthread -o "$O/tests" || st=1
shasum -a 256 "${PROD[@]}" "${TESTS[@]}" "$O/tests" > "$O/hashes.txt" 2>/dev/null
echo "build_status=$st"
exit $st
