#!/bin/bash
# Card 4b=A / 9a for WtUftCore UftStrategyMgr.
# usage: run.sh <workdir> [red-manager-dir]
#   Builds the UFT and HFT fake plugins of ../i03_strategy_plugin_abi_20261104s5,
#   then the checker against src/WtUftCore (green) and, if given, a directory
#   holding pre-change UftStrategyMgr.{h,cpp} (red). One process per case.
set -uo pipefail
fx=$(cd "$(dirname "$0")" && pwd -P)
abi="$fx/../i03_strategy_plugin_abi_20261104s5"
src=$(cd "$fx/../../.." && pwd -P)
work=${1:?workdir}; red=${2:-}
mkdir -p "$work"; work=$(cd "$work" && pwd -P)
boost=/opt/homebrew/opt/boost/include; gtest=/opt/homebrew/opt/googletest
variants="V2_OK V2_NOCAP LEGACY LEGACY_BYREF BAD_REVISION BAD_FAMILY BAD_MAJOR NULL_TABLE THROW_GETTER RESERVED NULL_DESTROY SHORT_TABLE NULL_FACTORY"
for fam in UFT HFT; do
  for v in $variants; do
    d="$work/plugins/$fam/$v"; mkdir -p "$d"
    c++ -std=c++17 -fPIC -dynamiclib -fvisibility=hidden -Wall -Wextra -Werror \
      -DFAKE_FAMILY_$fam -DFAKE_VARIANT_$v "$abi/fake_plugin.cpp" -o "$d/libfake.dylib" || exit 10
  done
done
build() { # <name> <manager-dir>
  c++ -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -Wall -Wextra \
    -I"$2" -I"$src/WtUftCore" -I"$src" -I"$boost" -I/opt/homebrew/include -I"$gtest/include" \
    -include "$src/Share/fmt_enum_compat.h" \
    "$2/UftStrategyMgr.cpp" "$src/TestUnits/fixtures/sel_factory_quarantine/logger_stub.cpp" "$fx/check_uft_mgr.cpp" \
    "$gtest/lib/libgtest_main.a" "$gtest/lib/libgtest.a" -o "$work/$1" > "$work/$1-build.log" 2>&1
}
build green "$src/WtUftCore" || { cat "$work/green-build.log"; exit 11; }
if [[ -n "$red" ]]; then build red "$red" || { cat "$work/red-build.log"; exit 12; }; fi
if find "$work" -type l -print -quit | grep -q .; then echo "symbolic link found" >&2; exit 15; fi
rc=0
for bin in green red; do
  [ -x "$work/$bin" ] || continue
  for t in $("$work/$bin" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}'); do
    out="$work/$bin-runs"; mkdir -p "$out"
    (cd "$out" && WT_ABI_PLUGIN_ROOT="$work/plugins" perl -e 'alarm 60; exec @ARGV' "$work/$bin" \
      --gtest_filter="$t" --gtest_output=xml:"$out/$t.xml" > "$out/$t.log" 2>&1); ec=$?
    echo "$bin $t exit=$ec"; [ "$bin" = green ] && [ $ec -ne 0 ] && rc=1
  done
done
exit $rc
