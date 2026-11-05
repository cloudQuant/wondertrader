#!/bin/bash
# Local diagnostic workaround: pre-existing r131:116 / r141:107 EXPECT_EQ(unsigned,int32_t) fail -Werror=sign-compare.
# Only that single diagnostic is downgraded; -Werror stays on for everything else.
set -u
cd /tmp/wt-i03-zreview-20261104
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
B=/tmp/wt-i03-zreview-20261104-btcore/build
mkdir -p /tmp/wt-i03-zreview-20261104-btcore
/opt/homebrew/bin/cmake -S snap/src -B $B -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  "-DCMAKE_CXX_FLAGS=-Wno-error=sign-compare" \
  -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest > logs/btcore2-configure.log 2>&1
echo "configure exit $?" > logs/btcore2-build.exit
/opt/homebrew/bin/cmake --build $B --target CoverageTests_btcore -j 4 > logs/btcore2-build.log 2>&1
echo "CoverageTests_btcore exit $?" >> logs/btcore2-build.exit
echo DONE >> logs/btcore2-build.exit
