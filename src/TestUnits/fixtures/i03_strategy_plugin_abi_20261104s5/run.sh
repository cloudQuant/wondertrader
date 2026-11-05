#!/bin/bash
# usage: run.sh <workdir> [red-source-dir]
#   Builds the local fake strategy plugins (4 families x 12 variants) and the
#   live-manager checker against the current sources (green). With a second
#   argument naming a directory that holds pre-change CtaStrategyMgr/
#   SelStrategyMgr/HftStrategyMgr .cpp/.h files, also builds the red control.
#   Every gtest case runs in its own process (managers keep modules resident,
#   so fixture counters must not leak between cases) under a 60 s alarm.
set -uo pipefail
fx=$(cd "$(dirname "$0")" && pwd -P)
src=$(cd "$fx/../../.." && pwd -P)
work=${1:?workdir}; red=${2:-}
mkdir -p "$work"; work=$(cd "$work" && pwd -P)
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/opt/boost/include}
fmt=${WT_FMT_INCLUDE:-/opt/homebrew/opt/fmt/include}
spdlog=${WT_SPDLOG_INCLUDE:-/opt/homebrew/opt/spdlog/include}
gtest=${WT_GTEST_PREFIX:-/opt/homebrew/opt/googletest}
cxx=${CXX:-c++}
case $(uname -s) in Darwin) ext=dylib; shared=-dynamiclib; dl= ;; *) ext=so; shared=-shared; dl=-ldl ;; esac
variants="V2_OK V2_NOCAP LEGACY LEGACY_BYREF BAD_REVISION BAD_FAMILY BAD_MAJOR NULL_TABLE THROW_GETTER RESERVED NULL_DESTROY SHORT_TABLE NULL_FACTORY"
plugins="$work/plugins"
for fam in CTA SEL HFT UFT; do
  for v in $variants; do
    d="$plugins/$fam/$v"; mkdir -p "$d"
    "$cxx" -std=c++17 -fPIC $shared -fvisibility=hidden -Wall -Wextra -Werror \
      -DFAKE_FAMILY_$fam -DFAKE_VARIANT_$v "$fx/fake_plugin.cpp" -o "$d/libfake.$ext" \
      || { echo "plugin build failed $fam/$v"; exit 10; }
  done
done
build_check() { # <name> <manager-dir>
  "$cxx" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -Wall -Wextra \
    -I"$2" -I"$src/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -I"$gtest/include" \
    -include "$src/Share/fmt_enum_compat.h" \
    "$2/CtaStrategyMgr.cpp" "$2/SelStrategyMgr.cpp" "$2/HftStrategyMgr.cpp" \
    "$src/TestUnits/fixtures/sel_factory_quarantine/logger_stub.cpp" "$fx/check_live.cpp" \
    "$gtest/lib/libgtest_main.a" "$gtest/lib/libgtest.a" $dl -o "$work/$1" > "$work/$1-build.log" 2>&1
}
build_check live-green "$src/WtCore" || { cat "$work/live-green-build.log"; exit 11; }
if [[ -n "$red" ]]; then build_check live-red "$red" || { cat "$work/live-red-build.log"; exit 12; }; fi
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
run_all() { # <binary-name>
  local bin="$work/$1" out="$work/$1-runs"; mkdir -p "$out"
  : > "$work/$1-results.txt"
  for t in $("$bin" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}'); do
    (cd "$out" && WT_ABI_PLUGIN_ROOT="$plugins" WT_SEL_LOG_MARKER="$out/$t.logger" \
      perl -e 'alarm shift; exec @ARGV or exit 127' 60 "$bin" --gtest_filter="$t" \
      --gtest_output=xml:"$out/$t.xml" > "$out/$t.log" 2>&1)
    echo "$t exit=$?" >> "$work/$1-results.txt"
  done
  cat "$work/$1-results.txt"
}
# Backtest mockers: link against prebuilt static libraries of one build tree.
# WT_ABI_LIBDIR (green) / WT_ABI_RED_LIBDIR (red) hold libWtBtCore.a,
# libWTSTools.a and libWTSUtils.a; coverage-instrumented libraries need the
# profile runtime, so -fprofile-instr-generate is always passed at link.
build_mocker() { # <name> <libdir>
  "$cxx" -std=c++17 -g -O0 -pthread -fprofile-instr-generate -Wno-deprecated-declarations \
    -I"$src" -isystem "$boost" -isystem /usr/local/include -isystem /opt/homebrew/include -isystem /opt/local/include \
    -include cstdlib -include "$src/Share/fmt_enum_compat.h" "$fx/check_mocker.cpp" \
    "$2/libWtBtCore.a" "$2/libWTSTools.a" "$2/libWTSUtils.a" \
    -L/opt/homebrew/lib -L/usr/local/lib -lfmt -lspdlog -lboost_filesystem -lboost_thread -lboost_system -liconv \
    "$gtest/lib/libgtest_main.a" "$gtest/lib/libgtest.a" $dl -o "$work/$1" > "$work/$1-build.log" 2>&1
}
if [[ -n "${WT_ABI_LIBDIR:-}" ]]; then
  build_mocker mocker-green "$WT_ABI_LIBDIR" || { cat "$work/mocker-green-build.log"; exit 13; }
  shasum -a 256 "$WT_ABI_LIBDIR"/libWtBtCore.a "$WT_ABI_LIBDIR"/libWTSTools.a "$WT_ABI_LIBDIR"/libWTSUtils.a > "$work/mocker-green-libs.sha256"
fi
if [[ -n "${WT_ABI_RED_LIBDIR:-}" ]]; then
  build_mocker mocker-red "$WT_ABI_RED_LIBDIR" || { cat "$work/mocker-red-build.log"; exit 14; }
  shasum -a 256 "$WT_ABI_RED_LIBDIR"/libWtBtCore.a "$WT_ABI_RED_LIBDIR"/libWTSTools.a "$WT_ABI_RED_LIBDIR"/libWTSUtils.a > "$work/mocker-red-libs.sha256"
fi
export LLVM_PROFILE_FILE="$work/profraw/%p.profraw"
run_all live-green
[[ -n "$red" ]] && run_all live-red
[[ -n "${WT_ABI_LIBDIR:-}" ]] && run_all mocker-green
[[ -n "${WT_ABI_RED_LIBDIR:-}" ]] && run_all mocker-red
shasum -a 256 "$fx"/*.cpp "$fx"/run.sh "$src"/Includes/IStrategyPluginV2.h "$src"/WtCore/{Cta,Sel,Hft}StrategyMgr.{cpp,h} > "$work/inputs.sha256"
[[ -n "$red" ]] && shasum -a 256 "$red"/* >> "$work/inputs.sha256"
shasum -a 256 "$work"/live-* 2>/dev/null | grep -v -- '-runs\|results\|build.log' > "$work/binaries.sha256"
exit 0
