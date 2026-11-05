until grep -q "build-exit=" /tmp/wt-i03-s5-btcore-cov-build6-green.log; do sleep 3; done
cd /Users/yunjinqi/Documents/new_projects/wondertrader
cmake --build /tmp/wt-i03-s5-btcore-cov --target CovExecV2Valid CovExecV2InitFail CovExecV2FactoryPreflightFalse CovExecV2InitFailDrainsWorker CovExecLegacy -j 3 -- -k 0 > /tmp/wt-i03-s5-btcore-cov-build7-plugins.log 2>&1
echo build-exit=$? >> /tmp/wt-i03-s5-btcore-cov-build7-plugins.log
