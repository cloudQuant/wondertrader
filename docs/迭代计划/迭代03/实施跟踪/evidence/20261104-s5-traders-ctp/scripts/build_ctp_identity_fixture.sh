#!/bin/bash
# Compile the T2-CTP-08 CTP identity fixture (ctp_identity_test.cpp includes the t25 lane TU)
# against <srcroot>, linking WTSTools/WTSUtils from the s5 coverage build dir.
set -euo pipefail
src=${1:?srcroot}; out=${2:?outdir}
mkdir -p "$out"
cov=/tmp/wt-i03-s5-traders-ctp-cov/build_x64/Debug/libs
flags=(-std=gnu++17 -g -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/opt/boost/include -I/opt/homebrew/include -isystem "$src" -include "$src/Share/fmt_enum_compat.h" -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" -Werror)
/usr/bin/c++ "${flags[@]}" -Dinst_hlp=i03_ctp_source -c "$src/TraderCTP/TraderCTP.cpp" -o "$out/TraderCTP.o"
/usr/bin/c++ "${flags[@]}" -Dinst_hlp=i03_ctp_test -c "$src/TestUnits/fixtures/i03_ctp_order_identity_20261103/ctp_identity_test.cpp" -o "$out/ctp_identity_test.o"
/usr/bin/c++ "$out/ctp_identity_test.o" "$out/TraderCTP.o" /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a "$cov/libWTSTools.a" "$cov/libWTSUtils.a" "$src/API/CTP6.6.9/darwin/libthosttraderapi_se.a" "$src/API/CTP6.6.9/darwin/libs/libssl.a" "$src/API/CTP6.6.9/darwin/libs/libcrypto.a" "$src/API/CTP6.6.9/darwin/libs/comunicationkeylib.a" -L/opt/homebrew/lib -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -lpthread -ldl -o "$out/i03_ctp_identity"
echo "built $out/i03_ctp_identity"
