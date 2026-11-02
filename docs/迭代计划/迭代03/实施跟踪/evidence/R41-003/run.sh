#!/bin/bash
set -u
TASK_ROOT=/tmp/wt-i03-r41-stop
mkdir -p "$TASK_ROOT/runs"
for case_name in FakeModuleObservesExactServerAndPublishAbi AsyncOrderHandlerDestroysObjectOnceAfterPublisherReturns DiscardedOrderHandlerReleasesRetainedReference DiscardedOrderHandlerDestroysLastReferenceOnce ThrowingOrderPublisherDestroysLastReferenceOnce WorkerFinishesPendingOrderBeforeDestructorReturns DiscardedTradeHandlerReleasesRetainedReference ThrowingTradePublisherReleasesRetainedReference; do
  mkdir -p "$TASK_ROOT/runs/$case_name"
  (cd "$TASK_ROOT/runs/$case_name" && LLVM_PROFILE_FILE="$TASK_ROOT/runs/$case_name/profile-%p.profraw" perl -e 'alarm 30; exec @ARGV' -- "$TASK_ROOT/focused" --gtest_filter="R160EventNotifier.$case_name" --gtest_output="xml:$TASK_ROOT/runs/$case_name/result.xml") > "$TASK_ROOT/runs/$case_name/output.log" 2>&1
  test_status=$?
  printf '%s exit=%s\n' "$case_name" "$test_status"
done
