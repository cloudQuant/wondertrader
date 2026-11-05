#!/bin/bash
# BUG-I02-NEW-016 / decision card 10 = A: TraderAdapter initExt login responsibility.
# usage: run.sh <libs-build-dir> <new-output-dir> <base|fix> <plain|asan> [gtest-filter]
# Each test runs in its own process (the pre-change variant crashes), with its own XML.
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fixture="$root/src/TestUnits/fixtures/i03_initext_login_20261104"
build=${1:?libs build dir (WtCore WtDtCore WTSTools WTSUtils, Debug)}
out=${2:?new output directory}
variant=${3:?base|fix}
mode=${4:?plain|asan}
filter=${5:-I03InitExtLogin.*}
mkdir "$out" || exit 90
out=$(cd "$out" && pwd)
cxx=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
libs="$build/build_x64/Debug/libs"

case "$variant" in
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
  *) exit 94 ;;
esac

plugin_file=libR59CredentialsTrader.dylib
flags=(-std=gnu++17 -g -O0 -Wall -Wextra -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages"
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DWT_I03_VARIANT=$vnum "-DI03_CRED_PLUGIN_FILE=\"$plugin_file\""
  -I"$vdir" -I"$root/src/WtCore" -I/opt/homebrew/include -I/usr/local/include -isystem "$root/src"
  -include cstdlib -include "$root/src/Share/fmt_enum_compat.h" ${san[@]+"${san[@]}"})

{
  echo "HEAD $(cd "$root" && git rev-parse HEAD)"
  echo "variant=$variant mode=$mode filter=$filter"
  "$cxx" --version | head -1
} > "$out/manifest.txt"
shasum -a 256 "$vdir/WtCore/TraderAdapter.h" "$vdir/WtCore/TraderAdapter.cpp" \
  "$fixture/initext_login_test.cpp" "$fixture/run.sh" "$root/src/TestUnits/fixtures/r59_credentials_trader.cpp" >> "$out/manifest.txt"

"$cxx" "${flags[@]}" -Werror -c "$vdir/WtCore/TraderAdapter.cpp" -o "$out/TraderAdapter.o" > "$out/ta-build.log" 2>&1
echo "ta-build exit $?" >> "$out/manifest.txt"
"$cxx" "${flags[@]}" -c "$root/src/WtCore/UnresolvedReportJournal.cpp" -o "$out/UnresolvedReportJournal.o" > "$out/journal-build.log" 2>&1
echo "journal-build exit $?" >> "$out/manifest.txt"
"$cxx" "${flags[@]}" -Werror -c "$fixture/initext_login_test.cpp" -o "$out/test.o" > "$out/test-build.log" 2>&1
echo "test-build exit $?" >> "$out/manifest.txt"
"$cxx" -std=gnu++17 -g -O0 -Wall -Wextra -Werror -fPIC -shared -I"$root/src" -I/opt/homebrew/include ${san[@]+"${san[@]}"} \
  "$root/src/TestUnits/fixtures/r59_credentials_trader.cpp" -o "$out/$plugin_file" > "$out/plugin-build.log" 2>&1
echo "plugin-build exit $?" >> "$out/manifest.txt"

# Drop archived TraderAdapter/UnresolvedReportJournal objects so only the
# variant objects above satisfy those symbols (no silent lazy extraction).
cp "$libs/libWtCore.a" "$out/libWtCore_stripped.a"
ar d "$out/libWtCore_stripped.a" TraderAdapter.cpp.o UnresolvedReportJournal.cpp.o 2> "$out/ar.log"
"$cxx" ${san[@]+"${san[@]}"} "$out/test.o" "$out/TraderAdapter.o" "$out/UnresolvedReportJournal.o" \
  "$out/libWtCore_stripped.a" "$libs/libWtDtCore.a" "$libs/libWTSTools.a" "$libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test" > "$out/link.log" 2>&1
link=$?
echo "link exit $link" >> "$out/manifest.txt"
[ $link -eq 0 ] || { head -40 "$out/link.log"; exit 95; }
shasum -a 256 "$out/test" "$out/TraderAdapter.o" "$out/$plugin_file" >> "$out/manifest.txt"

mkdir -p "$out/cwd/traders"
cp "$out/$plugin_file" "$out/cwd/traders/"
tests=$(cd "$out/cwd" && "$out/test" --gtest_filter="$filter" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}')
overall=0
for t in $tests; do
  name=${t#*.}
  (cd "$out/cwd" && ASAN_OPTIONS=halt_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    perl -e 'alarm 120; exec @ARGV' "$out/test" --gtest_filter="$t" --gtest_output=xml:"$out/$name.xml") \
    > "$out/$name.log" 2>&1
  rc=$?
  [ $rc -eq 0 ] || overall=1
  echo "test $t exit $rc" | tee -a "$out/manifest.txt"
done
echo "overall exit $overall" >> "$out/manifest.txt"
grep -h -E "ERROR: AddressSanitizer|runtime error|SEGV" "$out"/*.log | sort | uniq -c | head -20
exit $overall
