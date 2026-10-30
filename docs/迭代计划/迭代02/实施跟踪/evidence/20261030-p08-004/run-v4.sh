#!/bin/bash
set -euo pipefail
cd /tmp/wt-p08-side-run
binary=./TestUnits/CoverageTests_btcore_p08_side
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/cancelall-v4-%p.profraw "$binary" \
  --gtest_filter='P08UftOrderPosition.CancelAllScansMatchingCodeOnly:P08UftOrderPosition.CancelAllReturnsEveryAcceptedMatchingId' \
  --gtest_output=xml:/tmp/wt-p08-side-run/cancelall-v4.xml > /tmp/wt-p08-side-run/cancelall-v4.log 2>&1
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/wide-v4-%p.profraw "$binary" \
  --gtest_filter='P08UftOrderPosition.*:R191MockerFixture.R191UftMocker_CoverTodayLongPreavailExitCancel:R72HftUftPaths.UftMarketOrderFillsAndCloses:R72HftUftPaths.UftShortOpenBuysBackAtOppositeBookSides' \
  --gtest_output=xml:/tmp/wt-p08-side-run/wide-v4.xml > /tmp/wt-p08-side-run/wide-v4.log 2>&1
