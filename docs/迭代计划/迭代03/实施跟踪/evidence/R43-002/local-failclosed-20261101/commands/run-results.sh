#!/bin/bash
set -u
results=${1:?pass output directory}
mkdir -p "$results"
filter_vwap='r7_stkvw.empty_retry_ids_quarantine_across_ticks_and_reconnect:r7_stkvw.empty_initial_ids_quarantine_without_counting_or_auto_cancellation:r7_stkvw.timeout_without_verified_identity_keeps_monitor_and_never_cancels:r7_stkvw.canceled_sell_refires_with_safe_sell_quantity:r7_stkvw.canceled_retry_keeps_sublot_remainder_pending_without_invalid_order'
filter_match='R43MatchEngineFlowTest.emptyBookRejectsWithoutSideEffectsAndLaterQuoteAllowsOrders:R43MatchEngineFlowTest.passiveBuyBelowLimitFillsRemainingQty'
filter_depth='R43MatchEngineFlowTest.oneSidedBidIsSafeAndDoesNotCreateAFillWithoutAnAsk:R43MatchEngineFlowTest.oneSidedAskAndEmptyQuoteSidesAreIteratorSafe:R43MatchEngineFlowTest.bidAndAskDepthUseTheirOwnQuantities'
filter_old_vwap='r7_stkvw.empty_retry_ids_quarantine_across_ticks_and_reconnect:r7_stkvw.empty_initial_ids_quarantine_without_counting_or_auto_cancellation'
filter_old_match='R43MatchEngineFlowTest.emptyBookRejectsWithoutSideEffectsAndLaterQuoteAllowsOrders'
run_test() {
  name=$1; bin=$2; filter=$3
  LLVM_PROFILE_FILE="/tmp/wt-i03-r43-final/xml-${name}.profraw" UBSAN_OPTIONS=print_stacktrace=1 "$bin" --gtest_filter="$filter" --gtest_output="xml:$results/$name.xml" > "$results/$name.stdout.log" 2>&1
  code=$?
  printf '%s exit=%s\n' "$name" "$code" >> "$results/exit-codes.txt"
}
: > "$results/exit-codes.txt"
run_test final-vwap /tmp/wt-i03-r43-final/vwap "$filter_vwap"
run_test final-matcher /tmp/wt-i03-r43-final/matcher "$filter_match"
run_test final-matcher-ubsan /tmp/wt-i03-r43-final/matcher_ubsan "$filter_match"
run_test final-depth-ubsan /tmp/wt-i03-r43-final/matcher_ubsan "$filter_depth"
run_test baseline-vwap /tmp/wt-i03-r43-final/baseline_vwap "$filter_old_vwap"
run_test baseline-matcher /tmp/wt-i03-r43-final/baseline_matcher "$filter_old_match"
: > "$results/probe-matrix.stdout.log"
for side in buy sell; do
  for book in empty bid ask both; do
    LLVM_PROFILE_FILE="/tmp/wt-i03-r43-final/xml-${side}-${book}.profraw" UBSAN_OPTIONS=print_stacktrace=1 /tmp/wt-i03-r43-final/probe "$side" "$book" >> "$results/probe-matrix.stdout.log" 2>&1
    code=$?
    printf 'probe-%s-%s exit=%s\n' "$side" "$book" "$code" >> "$results/exit-codes.txt"
  done
done
for version in baseline final; do
  LLVM_PROFILE_FILE="/tmp/wt-i03-r43-final/positive-$version.profraw" "/tmp/wt-i03-r43-final/$version-positive" --gtest_filter='r43_positive_controls.known_canceled_order_retries_once_for_zero_and_nonzero_span' --gtest_output="xml:$results/$version-positive.xml" > "$results/$version-positive.stdout.log" 2>&1
  printf '%s-positive exit=%s\n' "$version" "$?" >> "$results/exit-codes.txt"
done
