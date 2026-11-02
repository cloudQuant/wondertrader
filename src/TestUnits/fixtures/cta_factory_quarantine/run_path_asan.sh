#!/bin/sh
set -eu
src_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd -P)
repo_root=$(CDPATH= cd -- "$src_root/.." && pwd -P)
fixture="$src_root/TestUnits/fixtures/cta_factory_quarantine"
build_dir=${1:?pass an isolated output directory}
previous=${2:?pass the pre-path-fix CtaStrategyMgr.cpp}
mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd -P)
run_dir=$(mktemp -d "$build_dir/run.XXXXXXXX")
cd "$repo_root"
set -x
CXX=${CXX:-/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++}
boost=${WT_BOOST_INCLUDE:-/opt/homebrew/Cellar/boost/1.88.0/include}
fmt=${WT_FMT_INCLUDE:-/opt/homebrew/Cellar/fmt/11.2.0/include}
spdlog=${WT_SPDLOG_INCLUDE:-/opt/homebrew/Cellar/spdlog/1.15.3/include}
case $(uname -s) in
 Darwin) ext=dylib; shared_flag=-dynamiclib; dl_lib= ;;
 *) ext=so; shared_flag=-shared; dl_lib=-ldl ;;
esac
mkdir -p "$run_dir/plugins" "$run_dir/logs"
deep_rel=deep
for part in 01 02 03 04 05; do deep_rel="$deep_rel/${part}_abcdefghijklmnopqrstuvwxyz0123456789"; done
mkdir -p "$run_dir/$deep_rel"
shasum -a 256 "$src_root/WtCore/CtaStrategyMgr.cpp" "$src_root/WtCore/CtaStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run_path_asan.sh" "$previous" > "$run_dir/source-before.sha256"
"$CXX" --version > "$run_dir/compiler.txt"
"$CXX" -std=c++17 -pthread -fPIC -fsanitize=address -fno-omit-frame-pointer "$shared_flag" "$fixture/factory.cpp" -o "$run_dir/plugins/fixture.$ext"
cp "$run_dir/plugins/fixture.$ext" "$run_dir/$deep_rel/fixture.$ext"
"$CXX" -std=c++17 -g -O1 -pthread -fsanitize=address -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
 -I"$boost" -I"$fmt" -I"$spdlog" -include stdlib.h -include "$src_root/Share/fmt_enum_compat.h" \
 "$src_root/WtCore/CtaStrategyMgr.cpp" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/current"
"$CXX" -std=c++17 -g -O1 -pthread -fsanitize=address -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL \
 -I"$src_root/WtCore" -I"$boost" -I"$fmt" -I"$spdlog" -include stdlib.h -include "$src_root/Share/fmt_enum_compat.h" \
 "$previous" "$fixture/logger_stub.cpp" "$fixture/check.cpp" $dl_lib -o "$run_dir/previous"
ulimit -c 0
run_case() {
 binary=$1; mode=$2; label=$3; folder=${4:-plugins}
 set +e
 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 WT_CTA_FACTORY_MODE="$mode" WT_CTA_MARKER="$run_dir/logs/$label.marker" WT_CTA_LOG_MARKER="$run_dir/logs/$label.formatted" \
  perl -e 'alarm 20; exec @ARGV' -- "$binary" "$mode" "$run_dir/$folder" "$run_dir/$folder/fixture.$ext" "$run_dir/plugins" "$run_dir/plugins/fixture.$ext" > "$run_dir/logs/$label.log" 2>&1
 rc=$?
 set -e
 printf '%s exit=%s\n' "$label" "$rc"
 case "$label:$rc" in
  current-*:0|previous-short:0) ;;
  previous-red-*:0|previous-red-*:142) echo "unexpected previous control result" >&2; return 1 ;;
  previous-red-*:*) ;;
  *) cat "$run_dir/logs/$label.log" >&2; return 1 ;;
 esac
}
run_case "$run_dir/current" short-directory current-short
run_case "$run_dir/current" deep-directory current-deep "$deep_rel"
run_case "$run_dir/current" long-directory-error current-long-error
run_case "$run_dir/previous" short-directory previous-short
run_case "$run_dir/previous" deep-directory previous-red-deep "$deep_rel"
run_case "$run_dir/previous" long-directory-error previous-red-long-error
if grep -q 'ERROR: AddressSanitizer' "$run_dir/logs/previous-red-long-error.log"; then
 echo 'previous-long-error-address-sanitizer-report=yes'
else
 echo 'previous-long-error-address-sanitizer-report=no'
fi
shasum -a 256 "$src_root/WtCore/CtaStrategyMgr.cpp" "$src_root/WtCore/CtaStrategyMgr.h" "$src_root/WTSTools/WTSLogger.h" "$src_root/Share/fmtlib.h" "$fixture/factory.cpp" "$fixture/check.cpp" "$fixture/logger_stub.cpp" "$fixture/run_path_asan.sh" "$previous" > "$run_dir/source-after.sha256"
cmp "$run_dir/source-before.sha256" "$run_dir/source-after.sha256"
shasum -a 256 "$run_dir/current" "$run_dir/previous" "$run_dir/plugins/fixture.$ext" > "$run_dir/artifact-sha256.txt"
find "$run_dir" -type l -print > "$run_dir/symlinks.txt"
test ! -s "$run_dir/symlinks.txt"
printf 'evidence=%s\n' "$run_dir"
