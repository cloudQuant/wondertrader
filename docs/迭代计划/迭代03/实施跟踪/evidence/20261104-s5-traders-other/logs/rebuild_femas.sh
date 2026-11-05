#!/bin/bash
set -u
REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
RUN=/tmp/wt-i03-s5-traders-other-run
RC=$REPO/src/TestUnits/fixtures/i03_leaks_main_20261104/run_cases.py
cd /tmp/wt-i03-s5-traders-other-cov || exit 1
ninja -j 3 $(cat $RUN/Femas-objs.txt) > $RUN/rebuild-femas-objs.log 2>&1 || { echo objs_failed; exit 1; }
grep -cE "warning:|error:" $RUN/rebuild-femas-objs.log
python3 $REPO/src/TestUnits/fixtures/i03_traders_other_20261104s5/link_only.py . $RUN/link-fix4.json Femas || exit 1
cp -p build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderFemas /tmp/wt-i03-s5-traders-other-bin/fix/
cd /tmp/wt-i03-s5-traders-other-san
for m in tsan asan; do
  [ -d manual/femas-fix-$m ] && rm -rf manual/femas-fix-$m-prev && mv manual/femas-fix-$m manual/femas-fix-$m-prev
  WT_I03_LEAKS_ROOT=/tmp/wt-i03-s5-traders-other-san WT_I03_BOOST_INCLUDE=/tmp/wt-i03-s5-traders-other-cov/TestUnits/coverage/cov_boost_include WT_I03_FEMAS_EXTRA_TESTS="t4_003_femas_lifecycle_test.cpp" WT_REPO=$REPO bash $REPO/src/TestUnits/fixtures/i03_leaks_main_20261104/manual_build.sh fix $m femas > mb-femas-$m.log 2>&1
  echo "femas-$m $(tail -1 mb-femas-$m.log)"
done
cd $RUN
export WT_I03_LEAKS_ROOT=$RUN
for g in t4003-fix t4002-fix lane2-fix tsan-femas asan-femas; do
  [ -d run/$g ] && rm -rf run/$g-prev2 && mv run/$g run/$g-prev2
  [ -f results-$g.json ] && mv results-$g.json results-$g-prev2.json
  python3 $RC cases/cases-$g.json results-$g.json 2>&1 | tail -1
done
grep -l "ThreadSanitizer\|AddressSanitizer\|LeakSanitizer\|runtime error" run/tsan-femas/*/output.log run/asan-femas/*/output.log
echo done
