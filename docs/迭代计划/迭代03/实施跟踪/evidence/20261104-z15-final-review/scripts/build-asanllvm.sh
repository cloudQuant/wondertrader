#!/bin/bash
# NEW-030: every TU of CoverageTests_uftcore (product, deps built in-tree, tests) compiled with ASan.
# Local diagnostic build only: -DWT_WARNINGS_AS_ERRORS=OFF is a deliberate local workaround (not committed).
set -u
cd /tmp/wt-i03-zreview-20261104
export PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
B=/tmp/wt-i03-zreview-20261104-asanllvm/build
mkdir -p /tmp/wt-i03-zreview-20261104-asanllvm
ASAN="-fsanitize=address -fno-omit-frame-pointer"; ASANCXX="$ASAN -include stdlib.h"
/opt/homebrew/bin/cmake -S snap/src -B $B -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DWT_WARNINGS_AS_ERRORS=OFF \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  "-DCMAKE_C_FLAGS=$ASAN" "-DCMAKE_CXX_FLAGS=$ASANCXX" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address" "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address" "-DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address" \
  -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest > logs/asanllvm-configure.log 2>&1
echo "asan configure exit $?" > logs/asanllvm-build.exit
/opt/homebrew/bin/cmake --build $B --target CoverageTests_uftcore -j 4 > logs/asanllvm-build-uftcore.log 2>&1
echo "CoverageTests_uftcore exit $?" >> logs/asanllvm-build.exit
echo DONE >> logs/asanllvm-build.exit
