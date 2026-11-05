#!/bin/bash
REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
E="$REPO/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-traders-other"
until [ -x /tmp/wt-i03-s5-traders-other-bin/fix3/CoverageTests_t_TraderXTP ]; do sleep 10; done
cd /tmp/wt-i03-s5-traders-other-san
export WT_I03_LEAKS_ROOT=/tmp/wt-i03-s5-traders-other-san WT_I03_BOOST_INCLUDE=/tmp/wt-i03-s5-traders-other-cov/TestUnits/coverage/cov_boost_include WT_REPO=$REPO
for m in yd xtp; do
  [ -d manual/$m-fix-asan ] && rm -rf manual/$m-fix-asan-r2 && mv manual/$m-fix-asan manual/$m-fix-asan-r2
  bash $REPO/src/TestUnits/fixtures/i03_leaks_main_20261104/manual_build.sh fix asan $m > mb-$m-asan-r3.log 2>&1
  echo "$m-asan $(tail -1 mb-$m-asan-r3.log)"
done
mkdir -p /tmp/wt-i03-s5-traders-other-red3 && cd /tmp/wt-i03-s5-traders-other-red3
python3 $REPO/src/TestUnits/fixtures/i03_traders_other_20261104s5/red_overlay_build.py $REPO /tmp/wt-i03-s5-traders-other-cov "$E/before-round3" /tmp/wt-i03-s5-traders-other-red3 XTP > red-build.log 2>&1
echo "red exit=$?"
echo chain-done
