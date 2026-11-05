#!/bin/bash
# usage: run_asan_lane.sh <phase>
set -uo pipefail
phase=$1; base=/tmp/wt-i03-s5-engine-lifecycle-run/asan-lane
: > $base/$phase-results.txt
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
for spec in "s5_engine_uninit_metadata_test:S5EngineUninitMetadata.*" "s5_live_fee_schedule_test:S5LiveFeeSchedule.*"; do
  bin=${spec%%:*}; filter=${spec#*:}
  [[ -x "$base/$phase-$bin" ]] || { echo "$bin not built for $phase" | tee -a $base/$phase-results.txt; continue; }
  for t in $("$base/$phase-$bin" --gtest_filter="$filter" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}'); do
    d="$base/run-$phase/$t"; mkdir -p "$d"
    # Death tests fork; LSan in the forked child reports libsystem's fork-time
    # allocation (_notify_fork_child), so leaks are checked only for the fee tests.
    leaks=1; [[ $bin == s5_engine_uninit_metadata_test ]] && leaks=0
    (cd "$d" && ASAN_OPTIONS=detect_leaks=$leaks:halt_on_error=1 perl -e 'alarm shift; exec @ARGV or exit 127' 120 "$base/$phase-$bin" --gtest_filter="$t" --gtest_output=xml:"$d/result.xml" > "$d/out.log" 2>&1)
    rc=$?
    san=$(grep -c "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error" "$d/out.log")
    echo "$t exit=$rc sanitizer_reports=$san" | tee -a $base/$phase-results.txt
  done
done
