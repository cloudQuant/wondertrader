#!/bin/sh
set -eu
export LLVM_PROFILE_FILE=/tmp/wt-r20-01/r20-%p.profraw
exec /tmp/wt-r20-01/CoverageTests_btcore --gtest_filter='R20EventNotifier.*' --gtest_output=xml:/tmp/wt-r20-01/r20.xml
