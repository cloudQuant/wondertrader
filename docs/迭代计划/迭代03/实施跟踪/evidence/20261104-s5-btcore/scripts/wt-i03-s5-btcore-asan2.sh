export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin
cd /Users/yunjinqi/Documents/new_projects/wondertrader
cmake --build /tmp/wt-i03-s5-btcore-asan --target CoverageTests_btcore CoverageTests_btporter WtDataStorage CovExecV2Valid CovExecV2InitFail CovExecV2FactoryPreflightFalse -j 3 -- -k 0 > /tmp/wt-i03-s5-btcore-asan-build2.log 2>&1
echo build-exit=$? >> /tmp/wt-i03-s5-btcore-asan-build2.log
