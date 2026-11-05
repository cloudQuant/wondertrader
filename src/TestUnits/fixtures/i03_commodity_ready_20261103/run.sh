#!/bin/bash
# usage: run.sh <workdir> <red|green> [asan]
#   red   : snapshot with the pre-edit files in baseline/ (WTSContractInfo.hpp,
#           WTSBaseDataMgr.cpp, CtaMocker.cpp, SelMocker.cpp)
#   green : snapshot with the current repository versions of the same files
#   asan  : Homebrew LLVM 22 with -fsanitize=address,undefined, LSan enabled
# The workdir keeps one physical snapshot of src/ (no symbolic links); only the
# four files above are swapped between phases. Builds WTSUtils, WTSTools and
# WtBtCore with -j2. Each test binary runs under a 120 s alarm.
set -uo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
fx="$root/src/TestUnits/fixtures/i03_commodity_ready_20261103"
work=${1:?workdir}; phase=${2:?red|green}; san=${3:-}
mkdir -p "$work"
if [[ ! -d "$work/snap/src" ]]; then
    rsync -a --exclude 'build*/' "$root/src" "$work/snap/" || exit 10
fi
snap="$work/snap/src"
files=(Includes/WTSContractInfo.hpp WTSTools/WTSBaseDataMgr.cpp WtBtCore/CtaMocker.cpp WtBtCore/SelMocker.cpp)
for f in "${files[@]}"; do
    if [[ $phase == red ]]; then cp "$fx/baseline/$(basename "$f")" "$snap/$f"; else cp "$root/src/$f" "$snap/$f"; fi
done
(cd "$work/snap" && shasum -a 256 "${files[@]/#/src/}") > "$work/$phase-inputs.sha256"
shasum -a 256 "$fx"/commodity_ready_test.cpp "$fx"/mocker_tick_divisor_test.cpp >> "$work/$phase-inputs.sha256"

build="$work/build"
if [[ $san == asan ]]; then
    llvm=/opt/homebrew/Cellar/llvm/22.1.8/bin
    cxx=$llvm/clang++; cc=$llvm/clang
    sflags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    export PATH=$llvm:/opt/homebrew/bin:/usr/bin:/bin
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
else
    cxx=/usr/bin/c++; cc=/usr/bin/cc; sflags=""
fi
if [[ ! -f "$build/CMakeCache.txt" ]]; then
    # WT_WARNINGS_AS_ERRORS=OFF: local workaround only; another in-progress
    # change (WTSDataFactory.cpp unused parameter) fails -Werror. Warnings of the
    # files changed here are checked in the build log.
    cmake -S "$snap" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWT_WARNINGS_AS_ERRORS=OFF \
        -DCMAKE_C_COMPILER=$cc -DCMAKE_CXX_COMPILER=$cxx \
        "-DCMAKE_C_FLAGS=$sflags" "-DCMAKE_CXX_FLAGS=$sflags -include cstdlib" \
        "-DCMAKE_EXE_LINKER_FLAGS=$sflags" "-DCMAKE_SHARED_LINKER_FLAGS=$sflags" \
        > "$work/configure.log" 2>&1 || { echo "configure failed"; exit 11; }
fi
cmake --build "$build" --target WTSUtils WTSTools WtBtCore -j2 > "$work/$phase-build.log" 2>&1
rc=$?; echo "build exit $rc" | tee -a "$work/$phase-build.log"; [[ $rc == 0 ]] || exit 12
grep -E "(WTSContractInfo|WTSBaseDataMgr|CtaMocker|SelMocker)\.(hpp|cpp).*warning" "$work/$phase-build.log" > "$work/$phase-own-warnings.txt" || true

cp /usr/local/lib/libnanomsg.6.0.1.dylib "$work/libnanomsg.6.dylib"
common=(-std=c++17 $sflags -Wno-deprecated-declarations -I "$snap" -I /opt/homebrew/include -I /opt/local/include
    -include cstdlib -include "$snap/Share/fmt_enum_compat.h" "-DWT_I03_DIST_COMMON=\"$root/dist/common\"")
libdir="$build/build_x64/Debug/libs"
base=(-L /opt/homebrew/lib -lfmt -lspdlog -lboost_filesystem -lboost_thread -lboost_system -liconv -pthread
    /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a)
# Homebrew clang does not search /usr/local/lib (GNU libiconv, nanomsg) by default.
[[ $san == asan ]] && base=("${base[@]}" -L /usr/local/lib -Wl,-rpath,/opt/homebrew/lib)
# LLVM 22 rejects the /opt/local rapidjson; use the one the CMake build used.
[[ $san == asan ]] && common=(-I /usr/local/include "${common[@]}")
$cxx "${common[@]}" "$fx/commodity_ready_test.cpp" -o "$work/$phase-ready" \
    "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" "${base[@]}" > "$work/$phase-link-ready.log" 2>&1 || { cat "$work/$phase-link-ready.log"; exit 13; }
$cxx "${common[@]}" "$fx/mocker_tick_divisor_test.cpp" -o "$work/$phase-mocker" \
    "$libdir/libWtBtCore.a" "$libdir/libWTSTools.a" "$libdir/libWTSUtils.a" -L "$work" -lnanomsg "${base[@]}" \
    > "$work/$phase-link-mocker.log" 2>&1 || { cat "$work/$phase-link-mocker.log"; exit 14; }
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi

cd "$work"
DYLD_LIBRARY_PATH="$work" perl -e 'alarm 120; exec @ARGV' -- "$work/$phase-ready" \
    --gtest_output=xml:"$work/$phase-ready.xml" > "$work/$phase-ready.log" 2>&1
r1=$?
DYLD_LIBRARY_PATH="$work" perl -e 'alarm 120; exec @ARGV' -- "$work/$phase-mocker" \
    --gtest_output=xml:"$work/$phase-mocker.xml" > "$work/$phase-mocker.log" 2>&1
r2=$?
echo "ready exit $r1; mocker exit $r2" | tee "$work/$phase-results.txt"
shasum -a 256 "$work/$phase-ready" "$work/$phase-mocker" > "$work/$phase-binaries.sha256"
exit 0
