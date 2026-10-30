#!/bin/sh
set -eu
export LLVM_PROFILE_FILE=/tmp/wt-r20-01/r124-asan-%p.profraw
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1
exec /usr/bin/perl -e 'alarm 60; exec @ARGV' /tmp/wt-r20-01/CoverageTests_btporter_r124_asan --gtest_filter='R124BtRunnerRun.EmptySyncReplayCompletesWithoutWorker' --gtest_output=xml:/tmp/wt-r20-01/r124-asan.xml
