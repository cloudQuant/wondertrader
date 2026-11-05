#!/bin/sh
set -u
evidence_dir="${1:?absolute evidence directory required}"
test_bin=/tmp/wt-i03-r6-004-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_porter
case_filter=R6PorterExtParserQuote.null_inputs_are_diagnosed_and_registered_quote_is_dispatched_once
mkdir -p /tmp/wt-i03-r6-004-20261102/run-parent /tmp/wt-i03-r6-004-20261102/run-child
cd /tmp/wt-i03-r6-004-20261102/run-parent || exit 2
LLVM_PROFILE_FILE=/tmp/wt-i03-r6-004-20261102/run-parent/profile.profraw \
  /usr/bin/perl -e 'alarm 75; exec @ARGV' -- "$test_bin" "--gtest_filter=$case_filter" \
  "--gtest_output=xml:$evidence_dir/parent.xml" > "$evidence_dir/parent.log" 2>&1
r6_parent_status=$?
printf '%s\n' "$r6_parent_status" > "$evidence_dir/parent.exit"
cd /tmp/wt-i03-r6-004-20261102/run-child || exit 2
WT_R6_QUOTE_DIAGNOSTIC_CHILD=1 LLVM_PROFILE_FILE=/tmp/wt-i03-r6-004-20261102/run-child/profile.profraw \
  /usr/bin/perl -e 'alarm 60; exec @ARGV' -- "$test_bin" "--gtest_filter=$case_filter" \
  "--gtest_output=xml:$evidence_dir/child.xml" > "$evidence_dir/child.log" 2>&1
r6_child_status=$?
printf '%s\n' "$r6_child_status" > "$evidence_dir/child.exit"
[ "$r6_parent_status" -eq 0 ] && [ "$r6_child_status" -eq 0 ]
