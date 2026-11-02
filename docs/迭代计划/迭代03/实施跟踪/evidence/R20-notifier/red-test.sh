#!/bin/bash
set -o pipefail
repo=/Users/yunjinqi/Documents/new_projects/wondertrader
out=/tmp/wt-i03-event-notifier
fixture="$repo/src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture"
/usr/bin/clang++ -std=c++17 -fprofile-instr-generate -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include "$repo/src/Share/fmt_enum_compat.h" -I"$out/red/src" -I"$repo/src/WtBtCore" -I"$repo/src" -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/usr/local/include "$fixture/notifier_check.cpp" "$out/red/src/WtBtCore/EventNotifier.cpp" "$repo/src/WtBtCore/WtHelper.cpp" /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSTools.a /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread -liconv -o "$out/red/notifier_check" || exit 1
cd "$out/empty-cwd" || exit 1
for variant in missing-create missing-destroy missing-register missing-publish zero full retry; do
  : > "$out/red/$variant.log"
  mode=$variant
  moddir=$variant
  case $variant in missing-create|missing-destroy|missing-register) mode=missing;; missing-publish) mode=no-publish;; retry) moddir=missing-create;; esac
  WT_R20_MQ_LOG="$out/red/$variant.log" "$out/red/notifier_check" "$mode" "$out/$moddir" "$out/full" > "$out/red/$variant.stdout" 2>&1
  rc=$?
  echo "$variant exit=$rc"
done
