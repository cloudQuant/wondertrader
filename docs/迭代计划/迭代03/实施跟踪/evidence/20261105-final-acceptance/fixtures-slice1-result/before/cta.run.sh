#!/bin/sh
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
repo_root=$(CDPATH= cd -- "$src_root/.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/cta_factory_quarantine"
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
mkdir -p "$run_dir/plugins" "$run_dir/plugins2" "$run_dir/no-creator" "$run_dir/logs"
deep_rel=deep
for part in 01 02 03 04 05; do
 deep_rel="$deep_rel/${part}_abcdefghijklmnopqrstuvwxyz0123456789"
done
mkdir -p "$run_dir/$deep_rel"
shasum -a 256 "$src_root/WtCore/CtaStrategyMgr.cpp" "$src_root/WtCore/CtaStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-before.sha256"
"$CXX" -std=c++17 -pthread -fPIC "$shared_flag" "$fixture/factory.cpp" -o "$run_dir/plugins/fixture.$ext"
cp "$run_dir/plugins/fixture.$ext" "$run_dir/plugins2/fixture.$ext"
cp "$run_dir/plugins/fixture.$ext" "$run_dir/$deep_rel/fixture.$ext"
"$CXX" -std=c++17 -pthread -fPIC "$shared_flag" "$fixture/no_creator.cpp" -o "$run_dir/no-creator/fixture.$ext"
"$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
 -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
 "$src_root/WtCore/CtaStrategyMgr.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/current"
# HEAD is a historical control, not the previously dirty working-tree snapshot.
git rev-parse HEAD > "$run_dir/head-commit.txt"
git show HEAD:src/WtCore/CtaStrategyMgr.cpp > "$run_dir/head-baseline.cpp"
"$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
 -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
 "$run_dir/head-baseline.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/head-baseline"
ulimit -c 0
run_case() {
 binary=$1; mode=$2; env_mode=$3; prefix=$4; folder=${5:-plugins}
 set +e
 WT_CTA_FACTORY_MODE="$env_mode" WT_CTA_MARKER="$run_dir/logs/$prefix.marker" WT_CTA_LOG_MARKER="$run_dir/logs/$prefix.formatted" \
  perl -e 'alarm 20; exec @ARGV' -- "$binary" "$mode" "$run_dir/$folder" "$run_dir/$folder/fixture.$ext" "$run_dir/plugins2" "$run_dir/plugins2/fixture.$ext" >"$run_dir/logs/$prefix.log" 2>&1
 rc=$?
 set -e
 printf '%s exit=%s\n' "$prefix" "$rc"
 case "$prefix:$rc" in
  current-*:0|head-positive:0|path-previous-positive:0) ;;
  head-red-*:0|head-red-*:142|previous-red-*:0|previous-red-*:142) echo "unexpected historical control result" >&2; return 1 ;;
  head-red-*:*|previous-red-*:*) ;;
  *) cat "$run_dir/logs/$prefix.log" >&2; return 1 ;;
 esac
}
for mode in creator-throw name-throw empty null duplicate concurrent reentrant single-name duplicate-different idempotent-cross-manager long-name; do
 run_case "$run_dir/current" "$mode" "$mode" "current-$mode"
done
run_case "$run_dir/current" same-manager-concurrent concurrent current-same-manager-concurrent
run_case "$run_dir/current" missing-creator missing-creator current-missing-creator no-creator
run_case "$run_dir/current" exception-recovery name-throw current-exception-recovery
run_case "$run_dir/current" short-directory short-directory current-short-directory
run_case "$run_dir/current" deep-directory deep-directory current-deep-directory "$deep_rel"
run_case "$run_dir/current" long-directory-error long-directory-error current-long-directory-error
for mode in creator-throw name-throw empty null duplicate single-name; do
 run_case "$run_dir/head-baseline" "$mode" "$mode" "head-red-$mode"
done
run_case "$run_dir/head-baseline" old-positive good head-positive
if [ -n "${WT_CTA_PREVIOUS_SOURCE:-}" ]; then
 shasum -a 256 "$WT_CTA_PREVIOUS_SOURCE" > "$run_dir/previous-source-before.sha256"
 "$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
  "$WT_CTA_PREVIOUS_SOURCE" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/previous"
 run_case "$run_dir/previous" idempotent-cross-manager idempotent-cross-manager previous-red-idempotent
 run_case "$run_dir/previous" missing-creator missing-creator previous-red-missing-creator no-creator
 run_case "$run_dir/previous" long-name long-name previous-red-long-name
 run_case "$run_dir/previous" duplicate-different duplicate-different previous-red-duplicate-different
 shasum -a 256 "$WT_CTA_PREVIOUS_SOURCE" > "$run_dir/previous-source-after.sha256"
 cmp "$run_dir/previous-source-before.sha256" "$run_dir/previous-source-after.sha256"
fi
if [ -n "${WT_CTA_PATH_PREVIOUS_SOURCE:-}" ]; then
 shasum -a 256 "$WT_CTA_PATH_PREVIOUS_SOURCE" > "$run_dir/path-previous-source-before.sha256"
 "$CXX" -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
  -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include "$src_root/Share/fmt_enum_compat.h" \
  "$WT_CTA_PATH_PREVIOUS_SOURCE" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/path-previous"
 run_case "$run_dir/path-previous" short-directory short-directory path-previous-positive
 run_case "$run_dir/path-previous" deep-directory deep-directory previous-red-path-deep "$deep_rel"
 run_case "$run_dir/path-previous" long-directory-error long-directory-error previous-red-path-error
 shasum -a 256 "$WT_CTA_PATH_PREVIOUS_SOURCE" > "$run_dir/path-previous-source-after.sha256"
 cmp "$run_dir/path-previous-source-before.sha256" "$run_dir/path-previous-source-after.sha256"
fi
shasum -a 256 "$src_root/WtCore/CtaStrategyMgr.cpp" "$src_root/WtCore/CtaStrategyMgr.h" "$fixture/factory.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$run_dir/head-baseline.cpp" "$run_dir/current" "$run_dir/plugins/fixture.$ext" > "$run_dir/sha256.txt"
shasum -a 256 "$src_root/WtCore/CtaStrategyMgr.cpp" "$src_root/WtCore/CtaStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/no_creator.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run.sh" > "$run_dir/source-after.sha256"
cmp "$run_dir/source-before.sha256" "$run_dir/source-after.sha256"
find "$run_dir" -type l -print > "$run_dir/symlinks.txt"
test ! -s "$run_dir/symlinks.txt"
printf 'evidence=%s\n' "$run_dir"
