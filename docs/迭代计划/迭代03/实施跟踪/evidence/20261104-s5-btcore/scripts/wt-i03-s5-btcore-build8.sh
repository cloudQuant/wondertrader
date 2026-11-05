until grep -q "build-exit=" /tmp/wt-i03-s5-btcore-cov-build7-plugins.log 2>/dev/null; do sleep 3; done
cd /Users/yunjinqi/Documents/new_projects/wondertrader
cmake --build /tmp/wt-i03-s5-btcore-cov --target CoverageTests_btcore CoverageTests_btporter CoverageTests_m_WtBtRunner R160FakeWtMsgQue WtDataStorage CovExecV2Valid CovExecV2InitFail CovExecV2FactoryPreflightFalse -j 3 -- -k 0 > /tmp/wt-i03-s5-btcore-cov-build8-final.log 2>&1
echo build-exit=$? >> /tmp/wt-i03-s5-btcore-cov-build8-final.log
