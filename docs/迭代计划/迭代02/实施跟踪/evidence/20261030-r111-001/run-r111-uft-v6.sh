#!/bin/bash
set -euo pipefail
cd /tmp/wt-p08-side-run
LLVM_PROFILE_FILE=/tmp/wt-p08-side-run/r111-uft-v6-%p.profraw \
  ./TestUnits/CoverageTests_btcore_r111_uft_v6 \
  --gtest_filter='R111UftMocker.*' \
  --gtest_output=xml:/tmp/wt-p08-side-run/r111-uft-v6.xml > /tmp/wt-p08-side-run/r111-uft-v6.log 2>&1
