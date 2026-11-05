#!/bin/bash
# NEW-030 recheck: every TU of CoverageTests_uftcore compiled with Homebrew LLVM 22 ASan
# (same recipe as evidence/20261104-z15-final-review/scripts/build-asanllvm.sh), from an
# entity snapshot of the working tree (rsync --no-links). Local diagnostic build only:
# -DWT_WARNINGS_AS_ERRORS=OFF and "-include stdlib.h" are deliberate local workarounds.
set -u
R=/tmp/wt-i03-s5-uftcore-asan
mkdir -p $R/snap
rsync -a --no-links --delete /Users/yunjinqi/Documents/new_projects/wondertrader/src/ $R/snap/src/
mkdir -p $R/snap/dist && rsync -a --no-links --delete /Users/yunjinqi/Documents/new_projects/wondertrader/dist/common/ $R/snap/dist/common/
find $R/snap -type l | wc -l > $R/symlinks.txt
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
ASAN="-fsanitize=address -fno-omit-frame-pointer"; ASANCXX="$ASAN -include stdlib.h"
cmake -S $R/snap/src -B $R/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DWT_WARNINGS_AS_ERRORS=OFF \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  "-DCMAKE_C_FLAGS=$ASAN" "-DCMAKE_CXX_FLAGS=$ASANCXX" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address" "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address" \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest > $R/configure.log 2>&1
echo "configure exit $?" > $R/build.exit
ninja -C $R/build -k 0 -j 3 build_x64/Debug/bin/TestUnits/CoverageTests_uftcore > $R/build.log 2>&1
echo "build exit $?" >> $R/build.exit
grep -c "fsanitize=address" $R/build/compile_commands.json >> $R/build.exit 2>/dev/null
echo DONE >> $R/build.exit
