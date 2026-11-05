#!/bin/bash
# usage: run.sh <workdir> <red|green> [before-dir]
#   red   : snapshot with <before-dir>/WtEngine.{cpp,h} (pre-change copies)
#   green : snapshot with the current repository WtEngine.{cpp,h}
# Homebrew LLVM 22 ASan/UBSan (+LSan where stated). Builds WtCore, WTSTools,
# WTSUtils (-j3) in one physical rsync snapshot of src/ (no symbolic links).
# Each case runs in its own process and cwd under a 120 s alarm.
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fx="$root/src/TestUnits/fixtures/i03_riskmon_quiesce_20261104s5"
work=${1:?workdir}; phase=${2:?red|green}; before=${3:-}
llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
export PATH=$llvm:/opt/homebrew/bin:/usr/bin:/bin
sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
mkdir -p "$work"; work=$(cd "$work" && pwd -P)
[[ -d "$work/snap/src" ]] || rsync -a --exclude 'build*/' "$root/src" "$work/snap/" || exit 10
snap="$work/snap/src"
for f in WtEngine.cpp WtEngine.h; do
    if [[ $phase == red ]]; then srcf="${before:?before-dir required for red}/$f"; else srcf="$root/src/WtCore/$f"; fi
    # Copy only on change so an unchanged phase does not force a rebuild.
    cmp -s "$srcf" "$snap/WtCore/$f" || cp "$srcf" "$snap/WtCore/$f"
done
(cd "$work/snap" && shasum -a 256 src/WtCore/WtEngine.cpp src/WtCore/WtEngine.h src/Includes/RiskMonDefs.h \
    src/Includes/IRiskMonPluginV2.h) > "$work/$phase-inputs.sha256"
shasum -a 256 "$fx"/*.cpp "$fx"/run.sh >> "$work/$phase-inputs.sha256"
build="$work/build"
if [[ ! -f "$build/CMakeCache.txt" ]]; then
    # WT_WARNINGS_AS_ERRORS=OFF: sanitizer-only local build; warnings of the
    # changed files are extracted into *-own-warnings.txt.
    cmake -S "$snap" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=$llvm/clang -DCMAKE_CXX_COMPILER=$llvm/clang++ -DWT_WARNINGS_AS_ERRORS=OFF \
        "-DCMAKE_C_FLAGS=$sflags" "-DCMAKE_CXX_FLAGS=$sflags -include cstdlib" \
        "-DCMAKE_EXE_LINKER_FLAGS=$sflags" "-DCMAKE_SHARED_LINKER_FLAGS=$sflags" \
        -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$work/configure.log" 2>&1 || exit 11
fi
cmake --build "$build" --target WtCore WTSTools WTSUtils -j3 > "$work/$phase-build.log" 2>&1
rc=$?; echo "build exit $rc" | tee -a "$work/$phase-build.log"; [[ $rc == 0 ]] || exit 12
grep -E "WtEngine\.(cpp|h).*warning" "$work/$phase-build.log" > "$work/$phase-own-warnings.txt" || true
libdir="$build/build_x64/Debug/libs"
$llvm/clang++ -std=gnu++17 $sflags -include cstdlib -include "$snap/Share/fmt_enum_compat.h" -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
    -I "$snap" -isystem /opt/homebrew/opt/boost/include -isystem /usr/local/include -isystem /opt/homebrew/include \
    "$fx/riskmon_test.cpp" -o "$work/$phase-riskmon" \
    "$libdir/libWtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" \
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a -L /opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib \
    -lboost_filesystem -lboost_thread -lboost_system /usr/local/lib/libiconv.2.dylib -pthread > "$work/$phase-link.log" 2>&1 || { cat "$work/$phase-link.log"; exit 13; }
for v in sync nocap; do
    d="$work/$phase-$v"; mkdir -p "$d"
    def=; [[ $v == sync ]] && def=-DFAKE_SYNC_STOP
    $llvm/clang++ -std=c++17 $sflags -fPIC -dynamiclib $def -isystem /opt/homebrew/opt/boost/include "$fx/fake_riskmon.cpp" -o "$d/libFakeRiskMon.dylib" || exit 14
done
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
shasum -a 256 "$work/$phase-riskmon" "$work/$phase-sync/libFakeRiskMon.dylib" "$work/$phase-nocap/libFakeRiskMon.dylib" > "$work/$phase-binary.sha256"
# I03-NEW-062: no LSan suppression. The proven (sync) variant's factory must be
# freed through the verified by-value deleter of IRiskMonPluginV2.
: > "$work/lsan.supp"
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
: > "$work/$phase-results.txt"
run_case() { # <test> <variant dir> <detect_leaks>
    (cd "$work/$phase-$2" && ASAN_OPTIONS=detect_leaks=$3:halt_on_error=1 LSAN_OPTIONS=suppressions="$work/lsan.supp" \
        perl -e 'alarm shift; exec @ARGV or exit 127' 120 "$work/$phase-riskmon" --gtest_filter="S5RiskMonQuiesce.$1" \
        --gtest_output=xml:"$work/$phase-$1.xml" > "$work/$phase-$1.log" 2>&1)
    echo "$1 variant=$2 detect_leaks=$3 exit $?" | tee -a "$work/$phase-results.txt"
}
# The quarantine intentionally keeps the monitor, its context and the fund
# alive until exit (card 9a), so LSan is off only for that case.
run_case UnprovenMonitorIsQuarantinedAndOutlivesEngineSafely nocap 0
run_case ProvenMonitorIsStoppedThenFundAndMonitorReleased sync 1
run_case RepeatedRiskMonitorInitIsRejected sync 1
exit 0
