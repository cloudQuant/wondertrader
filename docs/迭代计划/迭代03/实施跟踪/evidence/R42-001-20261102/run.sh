#!/bin/sh
set -u
root=/tmp/wt-i03-r42-001-20261102
binary="$root/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore"
evidence="$(cd "$(dirname "$0")" && pwd)"
: > "$evidence/results.txt"
for name in FreshAutomaticStorageDestructionIsSafe ReleaseThenAutomaticStorageDestructionIsSafe InvalidFeeInitializationCanRunFailClosedAndReleaseConfig SuccessfulInitializationReleasesConfigAfterDestruction; do
  mkdir -p "$root/runs/$name"
  (cd "$root/runs/$name" && ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 LLVM_PROFILE_FILE="$root/runs/$name/%p.profraw" perl -e 'alarm 40; exec @ARGV' -- "$binary" "--gtest_filter=R42SelEngineLifecycle.$name" "--gtest_output=xml:$evidence/$name.xml") > "$evidence/$name.log" 2>&1
  case_exit=$?
  printf '%s exit=%s\n' "$name" "$case_exit" | tee -a "$evidence/results.txt"
done
