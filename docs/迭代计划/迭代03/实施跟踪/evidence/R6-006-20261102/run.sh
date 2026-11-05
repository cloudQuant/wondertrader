#!/bin/sh
set -u
base=/tmp/wt-i03-r6-006
bin="$base/build/build_x64/Debug/bin/TestUnits/CoverageTests_porter"
evidence='/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R6-006-20261102'
for item in 'R46PorterConfig.MissingEnvMakesConfigFail' 'R69PorterRunnerConfig.ExplicitMissingDataStoreModuleFailsConfigAndLeavesRunnerNotReady' 'R69PorterRunnerConfig.MissingExecuterFileAndEmptyDataStillReturnTrue'; do
 mkdir -p "$base/run/$item"
 (cd "$base/run/$item" || exit 2; LLVM_PROFILE_FILE="$base/run/$item/profile.profraw" /usr/bin/perl -e 'alarm 60; exec @ARGV' -- "$bin" "--gtest_filter=$item" "--gtest_output=xml:$evidence/$item.xml") > "$evidence/$item.log" 2>&1
 printf '%s\n' "$?" > "$evidence/$item.exit"
done
