#!/bin/bash
set -u
TASK_ROOT=/tmp/wt-i03-r10-audit-20261103
result=0
for test in R10EventNotifier.MissingRegisterExportUnloadsCandidateAndPreservesActiveNotifier R10EventNotifier.MissingCreateServerExportUnloadsWithoutStartingWorker R160EventNotifier.FakeModuleObservesExactServerAndPublishAbi R160EventNotifier.AsyncOrderHandlerDestroysObjectOnceAfterPublisherReturns; do
  mkdir -p "$TASK_ROOT/runs/$test"
  (cd "$TASK_ROOT/runs/$test" && LLVM_PROFILE_FILE="$TASK_ROOT/runs/$test/profile-%p.profraw" perl -e 'alarm 30; exec @ARGV' -- "$TASK_ROOT/focused" --gtest_filter="$test" --gtest_output="xml:$TASK_ROOT/runs/$test/result.xml") > "$TASK_ROOT/runs/$test/output.log" 2>&1
  status=$?
  printf '%s exit=%s\n' "$test" "$status"
  if [[ $status -ne 0 ]]; then result=1; fi
done
exit "$result"
