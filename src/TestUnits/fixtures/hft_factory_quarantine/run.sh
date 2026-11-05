#!/bin/sh
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
repo_root=$(CDPATH= cd -- "$src_root/.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/hft_factory_quarantine"
build_dir=${1:?pass an isolated build directory}
mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd -P)
run_dir=$(mktemp -d "$build_dir/run.XXXXXXXX")
cd "$repo_root"
set -x
CXX=${CXX:-c++}
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/Cellar/boost/1.88.0/include}
fmt=${WT_FMT_INCLUDE:-/opt/homebrew/Cellar/fmt/11.2.0/include}
spdlog=${WT_SPDLOG_INCLUDE:-/opt/homebrew/Cellar/spdlog/1.15.3/include}
case $(uname -s) in
 Darwin) ext=dylib; shared_flag=-dynamiclib; dl_lib= ;;
 *) ext=so; shared_flag=-shared; dl_lib=-ldl ;;
esac
mkdir -p "$run_dir/plugins" "$run_dir/plugins2" "$run_dir/no-creator" "$run_dir/no-remover" "$run_dir/logs"
shasum -a 256 "$src_root/WtCore/HftStrategyMgr.cpp" "$src_root/WtCore/HftStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-before.sha256"
"$CXX" -std=c++17 -pthread -fPIC "$shared_flag" "$fixture/factory.cpp" -o "$run_dir/plugins/fixture.$ext"
cp "$run_dir/plugins/fixture.$ext" "$run_dir/plugins2/fixture.$ext"
segment=abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789
deep_folder="deep/$segment/$segment/$segment"
mkdir -p "$run_dir/$deep_folder"
cp "$run_dir/plugins/fixture.$ext" "$run_dir/$deep_folder/fixture.$ext"
"$CXX" -std=c++17 -pthread -fPIC "$shared_flag" "$fixture/no_creator.cpp" -o "$run_dir/no-creator/fixture.$ext"
"$CXX" -std=c++17 -pthread -fPIC "$shared_flag" -DOMIT_REMOVER "$fixture/factory.cpp" -o "$run_dir/no-remover/fixture.$ext"
"$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
 -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
 "$src_root/WtCore/HftStrategyMgr.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/current"
# A baseline is optional: when supplied it must contain both the saved source
# and matching header from one pre-change worktree snapshot.
baseline=${WT_HFT_BASELINE_DIR:-}
if [ -n "$baseline" ]; then
 shasum -a 256 "$baseline/HftStrategyMgr.cpp" "$baseline/HftStrategyMgr.h" > "$run_dir/baseline.sha256"
 cp "$baseline/HftStrategyMgr.cpp" "$run_dir/head-baseline.cpp"
 cp "$baseline/HftStrategyMgr.h" "$run_dir/HftStrategyMgr.h"
 sed 's!../../../WtCore/HftStrategyMgr.h!HftStrategyMgr.h!' "$fixture/check.cpp" > "$run_dir/head-check.cpp"
 "$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I"$run_dir" -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
  "$run_dir/head-baseline.cpp" "$fixture/logger_stub.cpp" "$run_dir/head-check.cpp" $dl_lib -o "$run_dir/head-baseline"
fi
ulimit -c 0
run_case() {
 binary=$1; mode=$2; env_mode=$3; prefix=$4; folder=${5:-plugins}
 set +e
 WT_HFT_FACTORY_MODE="$env_mode" WT_HFT_MARKER="$run_dir/logs/$prefix.marker" WT_HFT_LOG_MARKER="$run_dir/logs/$prefix.formatted" \
  perl -e 'alarm 20; exec @ARGV' -- "$binary" "$mode" "$run_dir/$folder" "$run_dir/$folder/fixture.$ext" "$run_dir/plugins2" "$run_dir/plugins2/fixture.$ext" >"$run_dir/logs/$prefix.log" 2>&1
 rc=$?
 set -e
 printf '%s exit=%s\n' "$prefix" "$rc"
 case "$prefix:$rc" in
  current-*:0|head-positive:0) ;;
  head-red-*:1|head-red-*:134) ;;
  head-red-*:*) echo "unexpected historical control result" >&2; cat "$run_dir/logs/$prefix.log" >&2; return 1 ;;
  *) cat "$run_dir/logs/$prefix.log" >&2; return 1 ;;
 esac
}
for mode in creator-throw name-throw empty null duplicate concurrent reentrant single-name duplicate-different idempotent-cross-manager long-name owner-lifetime; do
 run_case "$run_dir/current" "$mode" "$mode" "current-$mode"
done
run_case "$run_dir/current" same-manager-concurrent concurrent current-same-manager-concurrent
run_case "$run_dir/current" missing-creator missing-creator current-missing-creator no-creator
run_case "$run_dir/current" exception-recovery name-throw current-exception-recovery
run_case "$run_dir/current" long-invalid-path good current-long-invalid-path
run_case "$run_dir/current" long-path-valid good current-long-path-valid "$deep_folder"
for mode in owner-outlives-manager owner-no-preload owner-null-strategy legacy-raw-wrapper allocation-wrapper-failure allocation-control-failure allocation-registry-failure; do
 run_case "$run_dir/current" "$mode" "$mode" "current-$mode"
done
run_case "$run_dir/current" owner-without-remover owner-without-remover current-owner-without-remover no-remover
if [ -n "$baseline" ]; then
 if [ "${WT_HFT_BASELINE_KIND:-initial}" = owner ]; then
  run_case "$run_dir/head-baseline" owner-outlives-manager owner-outlives-manager head-red-owner-outlives-manager
  run_case "$run_dir/head-baseline" owner-no-preload owner-no-preload head-red-owner-no-preload
  run_case "$run_dir/head-baseline" single-name single-name head-positive
 else
  for mode in creator-throw name-throw empty null duplicate single-name; do
   run_case "$run_dir/head-baseline" "$mode" "$mode" "head-red-$mode"
  done
  if [ "${WT_HFT_LONG_PATH_RED:-0}" = 1 ]; then
   run_case "$run_dir/head-baseline" long-invalid-path good head-red-long-invalid-path
  fi
  run_case "$run_dir/head-baseline" old-positive good head-positive
 fi
else
 echo 'baseline controls NOT RUN (WT_HFT_BASELINE_DIR not supplied)'
fi
shasum -a 256 "$src_root/WtCore/HftStrategyMgr.cpp" "$src_root/WtCore/HftStrategyMgr.h" "$fixture/factory.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$run_dir/current" "$run_dir/plugins/fixture.$ext" > "$run_dir/sha256.txt"
shasum -a 256 "$src_root/WtCore/HftStrategyMgr.cpp" "$src_root/WtCore/HftStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-after.sha256"
cmp "$run_dir/source-before.sha256" "$run_dir/source-after.sha256"
find "$run_dir" -type l -print > "$run_dir/symlinks.txt"
test ! -s "$run_dir/symlinks.txt"
printf 'evidence=%s\n' "$run_dir"
