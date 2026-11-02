#!/bin/bash
set -o pipefail
repo=/Users/yunjinqi/Documents/new_projects/wondertrader
out=/tmp/wt-i03-event-notifier
fixture="$repo/src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture"
includes=(-I"$repo/src" -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -I/usr/local/include)
for mode in full missing-create missing-destroy missing-register missing-publish zero; do
  mkdir -p "$out/$mode"
  defines=()
  case $mode in
    missing-create) defines=(-DOMIT_CREATE);;
    missing-destroy) defines=(-DOMIT_DESTROY);;
    missing-register) defines=(-DOMIT_REGISTER);;
    missing-publish) defines=(-DOMIT_PUBLISH);;
    zero) defines=(-DFAIL_CREATE);;
  esac
  /usr/bin/clang++ -std=c++17 -dynamiclib "${includes[@]}" "${defines[@]}" "$fixture/fake_mq.cpp" -o "$out/$mode/libWtMsgQue.dylib" || exit 1
done
/usr/bin/clang++ -std=c++17 -fprofile-instr-generate -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -include "$repo/src/Share/fmt_enum_compat.h" "${includes[@]}" "$fixture/notifier_check.cpp" "$repo/src/WtBtCore/EventNotifier.cpp" "$repo/src/WtBtCore/WtHelper.cpp" /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSTools.a /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/Cellar/boost/1.88.0/lib -lboost_filesystem -lboost_thread -liconv -o "$out/notifier_check" || exit 1
mkdir -p "$out/empty-cwd"
cd "$out/empty-cwd" || exit 1
for variant in missing-create missing-destroy missing-register missing-publish zero full retry retry-zero full-single no-publish-single; do
  : > "$out/$variant.log"
  mode=$variant
  moddir=$variant
  case $variant in missing-create|missing-destroy|missing-register) mode=missing;; missing-publish) mode=no-publish;; retry) moddir=missing-register;; retry-zero) moddir=zero;; full-single) moddir=full;; no-publish-single) moddir=missing-publish;; esac
  WT_R20_MQ_LOG="$out/$variant.log" "$out/notifier_check" "$mode" "$out/$moddir" "$out/full" || exit 1
done
