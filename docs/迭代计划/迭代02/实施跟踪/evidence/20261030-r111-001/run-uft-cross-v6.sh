#!/bin/bash
set -euo pipefail
cd /tmp/wt-p08-side-run
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/uft-cross-v6-%p.profraw \
  ./TestUnits/CoverageTests_btcore_r111_uft_v6 \
  --gtest_filter='P08UftOrderPosition.*:R191MockerFixture.R191UftMocker_CoverTodayLongPreavailExitCancel:R72HftUftPaths.UftMarketOrderFillsAndCloses:R72HftUftPaths.UftShortOpenBuysBackAtOppositeBookSides' \
  --gtest_output=xml:/tmp/wt-p08-side-run/uft-cross-v6.xml > /tmp/wt-p08-side-run/uft-cross-v6.log 2>&1
