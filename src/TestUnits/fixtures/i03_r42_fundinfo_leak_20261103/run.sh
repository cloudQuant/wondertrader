#!/bin/bash
# usage: run.sh <workdir> <red|green>
#   red   : snapshot with baseline/WtEngine.{cpp,h} (pre-edit working tree)
#   green : snapshot with the current repository WtEngine.{cpp,h}
# Homebrew LLVM 22 ASan/UBSan with LSan (detect_leaks=1). Builds WtCore,
# WTSTools, WTSUtils with -j2 in one physical snapshot of src/ (no symbolic
# links); only WtEngine.{cpp,h} are swapped between phases. Each test case
# runs in its own process under a 120 s alarm so LSan results are per case.
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fx="$root/src/TestUnits/fixtures/i03_r42_fundinfo_leak_20261103"
work=${1:?workdir}; phase=${2:?red|green}
llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
export PATH=$llvm:/opt/homebrew/bin:/usr/bin:/bin
sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
mkdir -p "$work"
[[ -d "$work/snap/src" ]] || rsync -a --exclude 'build*/' "$root/src" "$work/snap/" || exit 10
snap="$work/snap/src"
for f in WtEngine.cpp WtEngine.h; do
    if [[ $phase == red ]]; then cp "$fx/baseline/$f" "$snap/WtCore/$f"; else cp "$root/src/WtCore/$f" "$snap/WtCore/$f"; fi
done
(cd "$work/snap" && shasum -a 256 src/WtCore/WtEngine.cpp src/WtCore/WtEngine.h src/WtCore/WtSelEngine.cpp \
    src/WtCore/WtSelEngine.h src/Includes/RiskMonDefs.h src/Includes/WTSRiskDef.hpp) > "$work/$phase-inputs.sha256"
shasum -a 256 "$fx/fund_owner_test.cpp" >> "$work/$phase-inputs.sha256"

build="$work/build"
if [[ ! -f "$build/CMakeCache.txt" ]]; then
    # WT_WARNINGS_AS_ERRORS=OFF: local workaround only (another in-progress
    # WTSDataFactory.cpp change fails -Werror); see *-own-warnings.txt.
    cmake -S "$snap" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=$llvm/clang -DCMAKE_CXX_COMPILER=$llvm/clang++ -DWT_WARNINGS_AS_ERRORS=OFF \
        "-DCMAKE_C_FLAGS=$sflags" "-DCMAKE_CXX_FLAGS=$sflags -include cstdlib" \
        "-DCMAKE_EXE_LINKER_FLAGS=$sflags" "-DCMAKE_SHARED_LINKER_FLAGS=$sflags" \
        -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$work/configure.log" 2>&1 || exit 11
fi
cmake --build "$build" --target WtCore WTSTools WTSUtils -j2 > "$work/$phase-build.log" 2>&1
rc=$?; echo "build exit $rc" | tee -a "$work/$phase-build.log"; [[ $rc == 0 ]] || exit 12
grep -E "WtEngine\.(cpp|h).*warning" "$work/$phase-build.log" > "$work/$phase-own-warnings.txt" || true

libdir="$build/build_x64/Debug/libs"
$llvm/clang++ -std=gnu++17 $sflags -include cstdlib -include "$snap/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
    -I "$snap" -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/include \
    "$fx/fund_owner_test.cpp" -o "$work/$phase-fund" \
    "$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" \
    /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib \
    -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread > "$work/$phase-link.log" 2>&1 || { cat "$work/$phase-link.log"; exit 13; }
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
shasum -a 256 "$work/$phase-fund" > "$work/$phase-binary.sha256"

export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
cd "$work"
: > "$work/$phase-results.txt"
for t in DestroyWithoutInitIsSafe InitThenDestroyLeavesNoFundAllocation EngineReleasesItsOwnReferenceOnDestruction \
         RepeatedLoadKeepsTheLentFundObject RiskMonitorIsStoppedBeforeFundRelease; do
    DYLD_LIBRARY_PATH="$work" perl -e 'alarm 120; exec @ARGV' -- "$work/$phase-fund" \
        --gtest_filter="R42FundOwner.$t" --gtest_output=xml:"$work/$phase-$t.xml" > "$work/$phase-$t.log" 2>&1
    echo "$t exit $?" | tee -a "$work/$phase-results.txt"
done
exit 0
