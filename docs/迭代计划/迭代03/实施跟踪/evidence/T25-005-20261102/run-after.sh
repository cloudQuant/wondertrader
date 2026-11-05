#!/bin/bash
set -uo pipefail
root=/tmp/wt-i03-t25-005-20261102
evidence="$PWD/docs/迭代计划/迭代03/实施跟踪/evidence/T25-005-20261102/after"
for name in query_confirm_sync_failure_clears_gate_and_worker_releases i03_no_final_query_response_disconnect_bounded i03_error_final_disconnect_then_late_error_callback; do
  mkdir -p "$root/run/$name"
  (cd "$root/run/$name" && perl -e 'alarm 8; exec @ARGV' -- "$root/focused" "--gtest_filter=T25TraderCTPTest.$name" "--gtest_output=xml:$evidence/$name.xml") > "$evidence/$name.log" 2>&1
  status=$?
  printf '%s %s\n' "$name" "$status" | tee -a "$evidence/results.txt"
done
