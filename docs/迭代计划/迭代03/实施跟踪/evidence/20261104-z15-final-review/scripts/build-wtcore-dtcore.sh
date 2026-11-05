#!/bin/bash
set -u
cd /tmp/wt-i03-zreview-20261104
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
B=/tmp/wt-i03-zreview-20261104-btcore/build
for t in CoverageTests_wtcore CoverageTests_dtcore; do
  /opt/homebrew/bin/cmake --build $B --target $t -j 4 > logs/btcore2-build-$t.log 2>&1
  echo "$t exit $?" >> logs/btcore2-build.exit
done
echo DONE2 >> logs/btcore2-build.exit
