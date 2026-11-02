#!/bin/bash
set -u
A=/tmp/wt-i03-r41-drain
for i in $(seq 1 10); do
  for test_name in DeliversAfterIdleWithoutExternalWorkGuard ThrowingPublisherIsReportedAndLaterItemRuns ConcurrentStopDrainsAcceptedItemsAndRejectsLaterItems; do
    mkdir -p "$A/repeat/$i-$test_name"
    (cd "$A/repeat/$i-$test_name" && LLVM_PROFILE_FILE="$A/repeat/$i-$test_name/profile-%p.profraw" perl -e 'alarm 30; exec @ARGV' -- "$A/focused" --gtest_filter="R160EventNotifier.$test_name" --gtest_output="xml:$A/repeat/$i-$test_name/result.xml") > "$A/repeat/$i-$test_name/output.log" 2>&1
    printf '%s %s exit=%s\n' "$i" "$test_name" "$?"
  done
done
