#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../../.." && pwd -P)
fixture="$root/src/TestUnits/fixtures/i03_parser_shm_lifecycle_v2"
work=$(mktemp -d /tmp/wt-shm-baseline-XXXXXX)
# Only rewrite relative include paths in copied source; logic is the saved baseline.
sed -e "s!\"../Includes/!\"$root/src/Includes/!g" -e "s!\"../Share/!\"$root/src/Share/!g" "$fixture/baseline/ParserShm.cpp" > "$work/ParserShm.cpp"
sed -e "s!\"../Includes/!\"$root/src/Includes/!g" -e "s!\"../Share/!\"$root/src/Share/!g" "$fixture/baseline/ParserShm.h" > "$work/ParserShm.h"
sed 's!"ParserShm/ParserShm.h"!"ParserShm.h"!' "$root/src/TestUnits/coverage/t_ParserShm/t5_shm_parser_test.cpp" > "$work/t5.cpp"
/usr/bin/c++ -std=c++17 -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include "$root/src/Share/fmt_enum_compat.h" \
 -I "$root/src" -I "$work" -I /opt/homebrew/include -I /opt/homebrew/Cellar/boost/1.88.0/include \
 -fprofile-instr-generate "$work/ParserShm.cpp" "$work/t5.cpp" \
 /tmp/wt-i03-r58-20261102-build/build_x64/Debug/libs/libWtCore.a \
 /tmp/wt-i03-r58-20261102-build/build_x64/Debug/libs/libWTSTools.a \
 /tmp/wt-i03-r58-20261102-build/build_x64/Debug/libs/libWTSUtils.a \
 /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib \
 -lboost_filesystem -lboost_thread -lpthread -ldl /usr/local/lib/libiconv.dylib -o "$work/test"
set +e
LLVM_PROFILE_FILE="$work/profile.profraw" perl -e 'alarm 20; exec @ARGV' -- "$work/test" \
 --gtest_filter=ParserShmTest.subscribe_and_unsubscribe_preserve_multisegment_standard_code \
 --gtest_output="xml:$work/result.xml" > "$work/result.log" 2>&1
rc=$?
set -e
cat "$work/result.log"
printf 'baseline output directory: %s; expected failing exit: %s\n' "$work" "$rc"
test "$rc" -eq 1
