#!/bin/bash
set -euxo pipefail
root=/tmp/wt-i03-t25-005-20261102
src="$root/srcsnap/src"
flags=(-DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DNOTFOUND -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/opt/homebrew/Cellar/googletest/1.17.0/include -isystem "$src" -I"$src/Includes" -I"$src/Share" -I"$src/WTSTools" -I"$src/WTSUtils" -include "$src/Share/fmt_enum_compat.h" -std=gnu++17 -g -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy -Wno-\#pragma-messages -Werror)
/usr/bin/c++ "${flags[@]}" -Dinst_hlp=cov_t25_source -c "$src/TraderCTP/TraderCTP.cpp" -o "$root/obj/TraderCTP.o"
/usr/bin/c++ "${flags[@]}" -Dinst_hlp=cov_t25_test -c "$src/TestUnits/coverage/t_TraderCTP/t25_traderctp_gap_test.cpp" -o "$root/obj/t25.o"
lib=/tmp/ctpm-i03-build/build_x64/Debug/libs
/usr/bin/c++ "$root/obj/t25.o" "$root/obj/TraderCTP.o" /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a "$lib/libWTSTools.a" "$lib/libWTSUtils.a" "$src/API/CTP6.6.9/darwin/libthosttraderapi_se.a" "$src/API/CTP6.6.9/darwin/libs/libssl.a" "$src/API/CTP6.6.9/darwin/libs/libcrypto.a" "$src/API/CTP6.6.9/darwin/libs/comunicationkeylib.a" /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib -lboost_filesystem -lboost_thread /usr/local/lib/libiconv.dylib -ldl -lpthread -o "$root/focused"
