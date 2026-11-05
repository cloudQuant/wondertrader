#!/bin/sh
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
repo_root=$(CDPATH= cd -- "$src_root/.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/sel_factory_quarantine"
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
shasum -a 256 "$src_root/WtCore/SelStrategyMgr.cpp" "$src_root/WtCore/SelStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-before.sha256"
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
 "$src_root/WtCore/SelStrategyMgr.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/current"
# Owner lifetime baseline needs its matching header: the manager's private
# layout changed, so compiling old source against the current header is invalid.
if [ -n "${WT_SEL_OWNER_BASELINE_DIR:-}" ]; then
 cp "$WT_SEL_OWNER_BASELINE_DIR/SelStrategyMgr.cpp" "$run_dir/owner-baseline.cpp"
 cp "$WT_SEL_OWNER_BASELINE_DIR/SelStrategyMgr.h" "$run_dir/SelStrategyMgr.h"
 sed 's!../../../WtCore/SelStrategyMgr.h!SelStrategyMgr.h!' "$fixture/check.cpp" > "$run_dir/owner-check.cpp"
 shasum -a 256 "$run_dir/owner-baseline.cpp" "$run_dir/SelStrategyMgr.h" > "$run_dir/owner-baseline.sha256"
 "$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I"$run_dir" -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
  "$run_dir/owner-baseline.cpp" "$fixture/logger_stub.cpp" "$run_dir/owner-check.cpp" $dl_lib -o "$run_dir/owner-baseline"
fi
# The optional initial baseline must have its matching header too.
if [ -n "${WT_SEL_BASELINE_DIR:-}" ]; then
 mkdir -p "$run_dir/initial-baseline"
 cp "$WT_SEL_BASELINE_DIR/SelStrategyMgr.cpp" "$run_dir/initial-baseline/baseline.cpp"
 cp "$WT_SEL_BASELINE_DIR/SelStrategyMgr.h" "$run_dir/initial-baseline/SelStrategyMgr.h"
 sed 's!../../../WtCore/SelStrategyMgr.h!SelStrategyMgr.h!' "$fixture/check.cpp" > "$run_dir/initial-baseline/check.cpp"
 shasum -a 256 "$run_dir/initial-baseline/baseline.cpp" "$run_dir/initial-baseline/SelStrategyMgr.h" > "$run_dir/baseline-source.sha256"
 "$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I"$run_dir/initial-baseline" -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
  "$run_dir/initial-baseline/baseline.cpp" "$fixture/logger_stub.cpp" "$run_dir/initial-baseline/check.cpp" $dl_lib -o "$run_dir/baseline"
fi
ulimit -c 0
run_case() {
 binary=$1; mode=$2; env_mode=$3; prefix=$4; folder=${5:-plugins}
 set +e
 WT_SEL_FACTORY_MODE="$env_mode" WT_SEL_MARKER="$run_dir/logs/$prefix.marker" WT_SEL_LOG_MARKER="$run_dir/logs/$prefix.formatted" \
  perl -e 'alarm 20; exec @ARGV' -- "$binary" "$mode" "$run_dir/$folder" "$run_dir/$folder/fixture.$ext" "$run_dir/plugins2" "$run_dir/plugins2/fixture.$ext" >"$run_dir/logs/$prefix.log" 2>&1
 rc=$?
 set -e
 printf '%s exit=%s\n' "$prefix" "$rc"
 case "$prefix:$rc" in
  current-*:0|baseline-positive:0|baseline-initial-positive:0) ;;
  baseline-red-*:0|baseline-red-*:142|previous-red-*:0|previous-red-*:142) echo "unexpected historical control result" >&2; return 1 ;;
  baseline-red-*:*|previous-red-*:*) ;;
  *) cat "$run_dir/logs/$prefix.log" >&2; return 1 ;;
 esac
}
for mode in creator-throw name-throw empty null duplicate concurrent reentrant single-name duplicate-different idempotent-cross-manager long-name long-directory; do
 run_case "$run_dir/current" "$mode" "$mode" "current-$mode"
done
run_case "$run_dir/current" same-manager-concurrent concurrent current-same-manager-concurrent
run_case "$run_dir/current" missing-creator missing-creator current-missing-creator no-creator
run_case "$run_dir/current" missing-remover missing-remover current-missing-remover no-remover
run_case "$run_dir/current" exception-recovery name-throw current-exception-recovery
run_case "$run_dir/current" long-valid-path good current-long-valid-path "$deep_folder"
run_case "$run_dir/current" owner-no-preload owner-no-preload current-owner-no-preload
run_case "$run_dir/current" owner-final-removal owner-final-removal previous-red-owner-final-removal
if [ -n "${WT_SEL_OWNER_BASELINE_DIR:-}" ]; then
 run_case "$run_dir/owner-baseline" owner-final-removal owner-final-removal previous-red-owner-no-preload
 run_case "$run_dir/owner-baseline" single-name single-name baseline-positive
fi
if [ -n "${WT_SEL_BASELINE_DIR:-}" ]; then
 for mode in creator-throw name-throw empty null duplicate; do
  run_case "$run_dir/baseline" "$mode" "$mode" "baseline-red-$mode"
 done
 run_case "$run_dir/baseline" old-positive good baseline-initial-positive
 run_case "$run_dir/baseline" missing-remover missing-remover baseline-red-missing-remover no-remover
 run_case "$run_dir/baseline" long-directory good baseline-red-long-directory
fi
shasum -a 256 "$src_root/WtCore/SelStrategyMgr.cpp" "$src_root/WtCore/SelStrategyMgr.h" "$fixture/factory.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$run_dir/current" "$run_dir/plugins/fixture.$ext" > "$run_dir/sha256.txt"
shasum -a 256 "$src_root/WtCore/SelStrategyMgr.cpp" "$src_root/WtCore/SelStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-after.sha256"
cmp "$run_dir/source-before.sha256" "$run_dir/source-after.sha256"
find "$run_dir" -type l -print > "$run_dir/symlinks.txt"
test ! -s "$run_dir/symlinks.txt"
printf 'evidence=%s\n' "$run_dir"
