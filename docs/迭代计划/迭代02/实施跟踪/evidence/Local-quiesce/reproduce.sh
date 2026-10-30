#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
# Reuses the existing directly linked local focused binary; the v3 build log records rebuilt objects.
build-cov/build_x64/Debug/bin/TestUnits/wt-local-focused
