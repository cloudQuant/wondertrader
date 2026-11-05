#!/bin/bash
# WP1 identity/account-gate fixture.
#   run.sh <lib-build-dir> <new-output-dir> <variant>
# variant: current-asan | current-tsan | baseline-asan | baseline2-asan
#   baseline-* compiles TraderAdapter.cpp/.h and CodeHelper.hpp from ./baseline
#   (pre-change copies) through a copied overlay tree (no symlinks) and only
#   the tests that use pre-change public APIs (-DWP1_BASELINE).
#   baseline2-* uses ./baseline2 (accepted WP1 state before the 2026-11-04
#   follow-up: verified cancel while paused, owner routing) with -DWP1_BASELINE2.
# Local fakes only; no broker connection, no real orders.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fixture="$root/src/TestUnits/fixtures/i03_wp1_identity_gate_20261104"
build=${1:?lib build dir with WtCore WtDtCore WTSTools WTSUtils}
out=${2:?new output directory}
variant=${3:?current-asan|current-tsan|baseline-asan|baseline2-asan}
mkdir "$out"
out=$(cd "$out" && pwd)
cd "$root"
compiler=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++
case "$variant" in
  *-asan) san=(-fsanitize=address,undefined -fno-sanitize-recover=all) ;;
  *-tsan) san=(-fsanitize=thread) ;;
  *) echo "bad variant" >&2; exit 2 ;;
esac
inc="$root/src"
defs=()
if [[ "$variant" == baseline2-* ]]; then
  mkdir -p "$out/overlay/src"
  cp -R "$root/src/WtCore" "$root/src/Share" "$root/src/Includes" "$root/src/WTSTools" "$root/src/WTSUtils" "$root/src/FasterLibs" "$out/overlay/src/"
  cp "$fixture/baseline2/WtCore/"* "$out/overlay/src/WtCore/"
  cp "$fixture/baseline2/Includes/"* "$out/overlay/src/Includes/"
  rm -f "$out/overlay/src/WtCore/ExecOwnerModes.h"
  if find "$out/overlay" -type l | grep -q .; then echo "symlink in overlay" >&2; exit 3; fi
  inc="$out/overlay/src"
  defs=(-DWP1_BASELINE2)
elif [[ "$variant" == baseline-* ]]; then
  mkdir -p "$out/overlay/src"
  cp -R "$root/src/WtCore" "$root/src/Share" "$root/src/Includes" "$root/src/WTSTools" "$root/src/WTSUtils" "$root/src/FasterLibs" "$out/overlay/src/"
  cp "$fixture/baseline/TraderAdapter.cpp" "$fixture/baseline/TraderAdapter.h" "$out/overlay/src/WtCore/"
  cp "$fixture/baseline/CodeHelper.hpp" "$out/overlay/src/Share/"
  if find "$out/overlay" -type l | grep -q .; then echo "symlink in overlay" >&2; exit 3; fi
  inc="$out/overlay/src"
  defs=(-DWP1_BASELINE)
fi
watched=(src/WtCore/TraderAdapter.cpp src/WtCore/TraderAdapter.h src/WtCore/AccountTradingGate.h
  src/Share/CodeHelper.hpp src/WtCore/UnresolvedReportJournal.cpp src/WtCore/DiffOrderOwnershipV1.cpp
  src/WtCore/WtExecuterFactory.cpp src/WtCore/WtExecuterFactory.h src/Includes/IExecPluginV2.h
  "$fixture/wp1_test.cpp" "$fixture/wp1_fake_provider.cpp" "$fixture/wp1_fake_exec_plugin.cpp" "$fixture/baseline/TraderAdapter.cpp"
  "$fixture/baseline/TraderAdapter.h" "$fixture/baseline/CodeHelper.hpp")
shasum -a 256 "${watched[@]}" > "$out/source.sha256"
git -C "$root" rev-parse HEAD > "$out/head.txt"
flags=(-std=c++17 -g -O1 -fno-omit-frame-pointer "${san[@]}" ${defs[@]+"${defs[@]}"}
  -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -DBOOST_BIND_GLOBAL_PLACEHOLDERS
  -I/opt/homebrew/Cellar/boost/1.88.0/include -I"$inc/Includes" -I"$inc/Share"
  -I"$inc/WTSTools" -I"$inc/WTSUtils" -I"$inc" -I"$inc/WtCore"
  -I/opt/homebrew/include -I/usr/local/include -include cstdlib -include "$root/src/Share/fmt_enum_compat.h")
"$compiler" -std=c++17 -g -I"$root/src" -I/opt/homebrew/include -dynamiclib \
  "$fixture/wp1_fake_provider.cpp" -o "$out/libWp1Fake.dylib" > "$out/plugin-build.log" 2>&1
for cap in 1 0; do
  name=$([ $cap = 1 ] && echo Cap || echo Old)
  "$compiler" -std=c++17 -g -DWP1_FAKE_HOST_IDENTITY=$cap -DFMT_HEADER_ONLY -I"$root/src" -I/opt/homebrew/include -I/usr/local/include \
    -include cstdlib -dynamiclib "$fixture/wp1_fake_exec_plugin.cpp" -o "$out/libWp1FakeExec$name.dylib" > "$out/exec-plugin-$name-build.log" 2>&1
done
"$compiler" "${flags[@]}" -c "$fixture/wp1_test.cpp" -o "$out/test.o" > "$out/test-build.log" 2>&1
"$compiler" "${flags[@]}" -Wall -Wextra -c "$inc/WtCore/TraderAdapter.cpp" -o "$out/TraderAdapter.o" > "$out/TraderAdapter-build.log" 2>&1
for unit in UnresolvedReportJournal DiffOrderOwnershipV1; do
  "$compiler" "${flags[@]}" -c "$root/src/WtCore/$unit.cpp" -o "$out/$unit.o" > "$out/$unit-build.log" 2>&1
done
"$compiler" "${san[@]}" "$out/test.o" "$out/TraderAdapter.o" "$out/UnresolvedReportJournal.o" "$out/DiffOrderOwnershipV1.o" \
  "$build/build_x64/Debug/libs/libWtCore.a" "$build/build_x64/Debug/libs/libWtDtCore.a" \
  "$build/build_x64/Debug/libs/libWTSTools.a" "$build/build_x64/Debug/libs/libWTSUtils.a" \
  /opt/homebrew/lib/libgtest.a \
  -L/opt/homebrew/lib -L/usr/local/lib -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib \
  -lboost_filesystem -lboost_thread -lnanomsg -liconv -ldl -lpthread -o "$out/test" > "$out/link.log" 2>&1
shasum -a 256 -c "$out/source.sha256" > "$out/source-check.log"
shasum -a 256 "$out/test" "$out/TraderAdapter.o" "$out/libWp1Fake.dylib" > "$out/binary.sha256"
set +e
(cd "$out" && WP1_FAKE_PLUGIN="$out/libWp1Fake.dylib" WP1_FAKE_EXEC_CAP="$out/libWp1FakeExecCap.dylib" \
  WP1_FAKE_EXEC_OLD="$out/libWp1FakeExecOld.dylib" ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  /usr/bin/perl -e 'alarm shift; exec @ARGV' 300 "$out/test" --gtest_output=xml:"$out/result.xml" > "$out/test.log" 2>&1)
rc=$?
set -e
echo "$rc" > "$out/exit_code.txt"
tail -40 "$out/test.log"
echo "exit=$rc"
exit $rc
