until grep -q "build-exit=" /tmp/wt-i03-s5-btcore-cov-build5-green.log; do sleep 3; done
cd /Users/yunjinqi/Documents/new_projects/wondertrader
cmake --build /tmp/wt-i03-s5-btcore-cov --target CoverageTests_btcore CoverageTests_btporter CoverageTests_m_WtBtRunner R160FakeWtMsgQue WtDataStorage -j 3 -- -k 0 > /tmp/wt-i03-s5-btcore-cov-build6-green.log 2>&1
echo build-exit=$? >> /tmp/wt-i03-s5-btcore-cov-build6-green.log
