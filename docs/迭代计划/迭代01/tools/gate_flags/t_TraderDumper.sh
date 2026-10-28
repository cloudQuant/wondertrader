#!/bin/sh
# 由 tools/gen_gate_flags.py 从真实构建命令生成，勿手改。
# 用法: sh t_TraderDumper.sh <你的测试 .cpp>
if [ $# -ne 1 ]; then echo 'usage: $0 <file.cpp>' >&2; exit 2; fi
exec /usr/bin/c++ -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/Users/yunjinqi/Documents/new_projects/wondertrader/build-cov/TestUnits/coverage/cov_boost_include -I/Users/yunjinqi/Documents/new_projects/wondertrader/src/Includes -I/Users/yunjinqi/Documents/new_projects/wondertrader/src/Share -I/Users/yunjinqi/Documents/new_projects/wondertrader/src/WTSTools -I/Users/yunjinqi/Documents/new_projects/wondertrader/src/WTSUtils -isystem /opt/homebrew/include -isystem /Users/yunjinqi/Documents/new_projects/wondertrader/src -include /Users/yunjinqi/Documents/new_projects/wondertrader/src/Share/fmt_enum_compat.h -g -std=gnu++17 -arch arm64 -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy '-Wno-#pragma-messages' -Werror -fprofile-instr-generate -fcoverage-mapping -fsyntax-only "$1"
