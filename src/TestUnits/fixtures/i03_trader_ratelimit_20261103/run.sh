#!/bin/bash
# Build and run the iteration-03 TraderAdapter rate-limit / split-ledger tests.
# usage: run.sh <libs-build-dir> <new-output-dir> <head|base|fix> <plain|asan|tsan> [gtest-filter]
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fixture="$root/src/TestUnits/fixtures/i03_trader_ratelimit_20261103"
build=${1:?libs build dir (WtCore WtDtCore WTSTools WTSUtils, Debug)}
out=${2:?new output directory}
variant=${3:?head|base|fix}
mode=${4:?plain|asan|tsan}
filter=${5:-I03RateLimit.*}
mkdir "$out" || exit 90
out=$(cd "$out" && pwd)
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
libs="$build/build_x64/Debug/libs"

case "$variant" in
  head) vdir="$out/variant"; mkdir -p "$vdir/WtCore"
        (cd "$root" && git show HEAD:src/WtCore/TraderAdapter.h > "$vdir/WtCore/TraderAdapter.h" &&
         git show HEAD:src/WtCore/TraderAdapter.cpp > "$vdir/WtCore/TraderAdapter.cpp") || exit 91
        vnum=0 ;;
  base) vdir="$out/variant"; mkdir -p "$vdir/WtCore"
        cp "$fixture/baseline/TraderAdapter.h" "$fixture/baseline/TraderAdapter.cpp" "$vdir/WtCore/" || exit 91
        (cd "$fixture/baseline" && shasum -a 256 -c baseline.sha256) > "$out/baseline-check.log" 2>&1 || exit 92
        vnum=1 ;;
  fix)  vdir="$root/src"; vnum=2 ;;
  *) exit 93 ;;
esac

case "$mode" in
  plain) san=() ;;
  asan)  san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer) ;;
  tsan)  san=(-fsanitize=thread) ;;
  *) exit 94 ;;
esac

flags=(-std=gnu++17 -g -O0 -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages"
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWT_I03_VARIANT=$vnum
  -I"$vdir" -I"$root/src/WtCore" -I/opt/homebrew/include -I/usr/local/include -isystem "$root/src"
  -include cstdlib -include "$root/src/Share/fmt_enum_compat.h" ${san[@]+"${san[@]}"})

{
  echo "HEAD $(cd "$root" && git rev-parse HEAD)"
  echo "variant=$variant mode=$mode filter=$filter"
  "$cxx" --version | head -1
} > "$out/manifest.txt"
shasum -a 256 "$vdir/WtCore/TraderAdapter.h" "$vdir/WtCore/TraderAdapter.cpp" \
  "$root/src/Share/FrequencyWindow.hpp" "$fixture/rate_limit_test.cpp" "$fixture/run.sh" >> "$out/manifest.txt"

"$cxx" "${flags[@]}" -Werror -c "$vdir/WtCore/TraderAdapter.cpp" -o "$out/TraderAdapter.o" > "$out/ta-build.log" 2>&1
echo "ta-build exit $?" >> "$out/manifest.txt"
"$cxx" "${flags[@]}" -c "$root/src/WtCore/UnresolvedReportJournal.cpp" -o "$out/UnresolvedReportJournal.o" > "$out/journal-build.log" 2>&1
echo "journal-build exit $?" >> "$out/manifest.txt"
"$cxx" "${flags[@]}" -c "$fixture/rate_limit_test.cpp" -o "$out/test.o" > "$out/test-build.log" 2>&1
echo "test-build exit $?" >> "$out/manifest.txt"

# Drop the archived TraderAdapter/UnresolvedReportJournal objects so only the
# variant objects above can satisfy those symbols (no silent lazy extraction).
cp "$libs/libWtCore.a" "$out/libWtCore_stripped.a"
ar d "$out/libWtCore_stripped.a" TraderAdapter.cpp.o UnresolvedReportJournal.cpp.o 2> "$out/ar.log"
"$cxx" ${san[@]+"${san[@]}"} "$out/test.o" "$out/TraderAdapter.o" "$out/UnresolvedReportJournal.o" \
  "$out/libWtCore_stripped.a" "$libs/libWtDtCore.a" "$libs/libWTSTools.a" "$libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test" > "$out/link.log" 2>&1
link=$?
echo "link exit $link" >> "$out/manifest.txt"
[ $link -eq 0 ] || { cat "$out/link.log" | head -40; exit 95; }
shasum -a 256 "$out/test" "$out/TraderAdapter.o" >> "$out/manifest.txt"

mkdir -p "$out/cwd"
(cd "$out/cwd" && ASAN_OPTIONS=halt_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  perl -e 'alarm 180; exec @ARGV' "$out/test" --gtest_filter="$filter" --gtest_output=xml:"$out/result.xml") \
  > "$out/test.log" 2>&1
rc=$?
echo "test exit $rc" >> "$out/manifest.txt"
grep -E "^\[  (PASSED|FAILED|SKIPPED) |tests? ran|^\[  FAILED  \]|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error" "$out/test.log" | sort | uniq -c | head -60
exit $rc
