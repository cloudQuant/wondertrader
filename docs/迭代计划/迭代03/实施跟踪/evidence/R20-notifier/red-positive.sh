#!/bin/bash
set -e
out=/tmp/wt-i03-event-notifier
cd "$out/empty-cwd"
for mode in full-single no-publish-single; do
  : > "$out/red/$mode.log"
  dir=full
  if [ "$mode" = no-publish-single ]; then dir=missing-publish; fi
  WT_R20_MQ_LOG="$out/red/$mode.log" "$out/red/notifier_check" "$mode" "$out/$dir"
done
