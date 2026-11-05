#!/bin/bash
set -u
cd /tmp/wt-i03-zreview-20261104
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
B=/tmp/wt-i03-zreview-20261104/build
/opt/homebrew/bin/cmake -S snap/src -B $B -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest > logs/configure.log 2>&1
echo "configure exit $?" > logs/configure.exit
for t in CoverageTests_btcore CoverageTests_porter CoverageTests_btporter CoverageTests_t_ParserShm CoverageTests_wtcore; do
  /opt/homebrew/bin/cmake --build $B --target $t -j 4 > logs/build-$t.log 2>&1
  echo "$t exit $?" >> logs/build.exit
done
echo DONE >> logs/build.exit
