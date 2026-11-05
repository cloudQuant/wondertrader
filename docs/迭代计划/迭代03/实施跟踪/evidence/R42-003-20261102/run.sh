#!/bin/sh
set -u
EVIDENCE_DIR='/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R42-003-20261102'
RUN_ROOT=/tmp/wt-i03-r42-003-20261102/runs
BIN=/tmp/wt-i03-r42-003-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore
mkdir -p "$RUN_ROOT"
for TEST_NAME in ManagerReleaseStopsRetainedSuccessfulAdapter InitExtHandlesApiStates BusyInitExtRetainsAndDeduplicatesTransferredApis ReleaseExceptionFailsClosedWithoutRetry RemoverExceptionPreservesOwnerAndFailsClosed ParserAdapterMgrRetainsFailedOwner ParserAdapterMgrRetriesDeferredRemoverWithoutLosingOwner; do
  mkdir -p "$RUN_ROOT/$TEST_NAME"
  cd "$RUN_ROOT/$TEST_NAME" || exit 1
  LLVM_PROFILE_FILE="$RUN_ROOT/$TEST_NAME.profraw" perl -e 'alarm 40; exec @ARGV' -- "$BIN" "--gtest_filter=R42ParserAdapterTest.$TEST_NAME" "--gtest_output=xml:$EVIDENCE_DIR/$TEST_NAME.xml" > "$EVIDENCE_DIR/$TEST_NAME.log" 2>&1
  CODE=$?
  printf '%s %s\n' "$TEST_NAME" "$CODE" >> "$EVIDENCE_DIR/results.txt"
done
