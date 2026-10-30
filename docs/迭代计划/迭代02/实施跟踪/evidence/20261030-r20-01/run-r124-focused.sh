#!/bin/sh
set -eu
export LLVM_PROFILE_FILE=/tmp/wt-r20-01/r124-%p.profraw
exec /usr/bin/perl -e 'alarm 45; exec @ARGV' /tmp/wt-r20-01/CoverageTests_btporter_r124 --gtest_filter='R124BtRunnerRun.EmptySyncReplayCompletesWithoutWorker' --gtest_output=xml:/tmp/wt-r20-01/r124.xml
