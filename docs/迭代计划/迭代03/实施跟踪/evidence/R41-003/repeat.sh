#!/bin/bash
set -u
TASK_ROOT=/tmp/wt-i03-r41-stop
for i in $(seq 1 10); do
  mkdir -p "$TASK_ROOT/repeat/$i"
  (cd "$TASK_ROOT/repeat/$i" && LLVM_PROFILE_FILE="$TASK_ROOT/repeat/$i/profile-%p.profraw" perl -e 'alarm 30; exec @ARGV' -- "$TASK_ROOT/focused" --gtest_filter=R160EventNotifier.WorkerFinishesPendingOrderBeforeDestructorReturns --gtest_output="xml:$TASK_ROOT/repeat/$i/result.xml") > "$TASK_ROOT/repeat/$i/output.log" 2>&1
  printf '%s exit=%s\n' "$i" "$?"
done
