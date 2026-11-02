#!/bin/bash
set -euo pipefail
cd /Users/yunjinqi/Documents/new_projects/wondertrader
A=/tmp/wt-i03-r41-audit
B=/tmp/wt-i03-r41-drain
ARGS=(-std=c++17 -O1 -g -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include src/Share/fmt_enum_compat.h -I src -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/Cellar/fmt/11.2.0/include -I /opt/homebrew/Cellar/spdlog/1.15.3/include -I /usr/local/include)
set -x
/usr/bin/clang++ "${ARGS[@]}" -c "$A/audit.cpp" -o "$A/audit.o"
/usr/bin/clang++ -fprofile-instr-generate -pthread "$B/notifier.o" "$B/helper.o" "$A/audit.o" "$B/libs/libWTSTools.a" "$B/libs/libWTSUtils.a" /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.a /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_system.a -liconv -o /tmp/wt-i03-r41-drain/audit-after
