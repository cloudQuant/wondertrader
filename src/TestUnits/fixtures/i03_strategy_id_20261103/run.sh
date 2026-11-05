#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd -P)
fixture="$src/TestUnits/fixtures/i03_strategy_id_20261103"
build=${1:?pass isolated build directory}
mkdir -p "$build"
build=$(CDPATH= cd -- "$build" && pwd -P)
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/Cellar/boost/1.88.0/include}
fmt=${WT_FMT_INCLUDE:-/opt/homebrew/Cellar/fmt/11.2.0/include}
spdlog=${WT_SPDLOG_INCLUDE:-/opt/homebrew/Cellar/spdlog/1.15.3/include}
cxx=${CXX:-c++}
case $(uname -s) in Darwin) ext=dylib; shared=-dynamiclib; dl= ;; *) ext=so; shared=-shared; dl=-ldl ;; esac
for kind in Hft Sel; do
 dir="$build/$kind"; mkdir -p "$dir/plugins"
 define=; [ "$kind" != Hft ] || define=-DTEST_HFT
 "$cxx" -std=c++17 -pthread -fPIC "$shared" $define "$fixture/plugin.cpp" -o "$dir/plugins/fixture.$ext"
 "$cxx" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL $define \
  -I"$boost" -I"$fmt" -I"$spdlog" -include "$src/Share/fmt_enum_compat.h" \
  "$src/WtCore/${kind}StrategyMgr.cpp" "$src/TestUnits/fixtures/sel_factory_quarantine/logger_stub.cpp" "$fixture/check.cpp" $dl -o "$dir/check"
 printf '%s: ' "$kind"
 perl -e 'alarm 20; exec @ARGV' -- "$dir/check" "$dir/plugins" "$dir/plugins/fixture.$ext"
done
