#!/bin/bash
set -euo pipefail
cd /tmp/wt-p08-side-run
binary=./TestUnits/CoverageTests_btcore_p08_side
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/p08-006-focused-%p.profraw "$binary" \
  --gtest_filter='P08UftOrderPosition.ExitUnknownCommodityRejectedWithoutStateMutation:P08UftOrderPosition.ExitLongAndShortWithKnownCommodity:P08UftOrderPosition.ExitRejectedWithoutAvailablePosition:P08UftOrderPosition.CancelShortOpenReleasesFrozenAvail:P08UftOrderPosition.OrderParamGuards' \
  --gtest_output=xml:/tmp/wt-p08-side-run/p08-006-focused.xml > /tmp/wt-p08-side-run/p08-006-focused.log 2>&1
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/p08-006-wide-%p.profraw "$binary" \
  --gtest_filter='P08UftOrderPosition.*:R191MockerFixture.R191UftMocker_CoverTodayLongPreavailExitCancel:R72HftUftPaths.UftMarketOrderFillsAndCloses:R72HftUftPaths.UftShortOpenBuysBackAtOppositeBookSides' \
  --gtest_output=xml:/tmp/wt-p08-side-run/p08-006-wide.xml > /tmp/wt-p08-side-run/p08-006-wide.log 2>&1
