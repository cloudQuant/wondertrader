#!/bin/bash
set -uo pipefail
root=/tmp/wt-i03-t25-005-20261102
evidence="$PWD/docs/迭代计划/迭代03/实施跟踪/evidence/T25-005-20261102/after"
for n in 1 2 3 4 5; do
  mkdir -p "$root/run/repeat-$n"
  (cd "$root/run/repeat-$n" && perl -e 'alarm 8; exec @ARGV' -- "$root/focused" --gtest_filter=T25TraderCTPTest.query_confirm_sync_failure_clears_gate_and_worker_releases "--gtest_output=xml:$evidence/repeat-$n.xml") > "$evidence/repeat-$n.log" 2>&1
  status=$?
  printf '%s %s\n' "$n" "$status" | tee -a "$evidence/repeat-results.txt"
done
