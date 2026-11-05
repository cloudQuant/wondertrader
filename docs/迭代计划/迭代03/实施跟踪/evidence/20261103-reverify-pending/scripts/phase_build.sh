#!/bin/bash
# usage: phase_build.sh <tag> <builddir> <targets...>
R=/tmp/wt-i03-reverify-20261103; REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
tag=$1; bd=$2; shift 2
cd $REPO && /usr/bin/find src dist/common -type f -print0 | sort -z | xargs -0 shasum -a 256 > $R/hashes/$tag-prebuild.sha256
date '+%F %T' > $R/hashes/$tag-prebuild.time
cd $R && PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/ninja -C $bd -j 7 -k 0 "$@" > $R/$tag-build.log 2>&1
rc=$?
cd $REPO && /usr/bin/find src dist/common -type f -print0 | sort -z | xargs -0 shasum -a 256 > $R/hashes/$tag-postbuild.sha256
date '+%F %T' > $R/hashes/$tag-postbuild.time
echo "exit=$rc" >> $R/$tag-build.log
